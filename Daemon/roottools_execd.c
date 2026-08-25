#include <arpa/inet.h>
#include <CommonCrypto/CommonDigest.h>
#include <CoreFoundation/CoreFoundation.h>
#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <mach/mach.h>
#include <netinet/in.h>
#include <signal.h>
#include <spawn.h>
#include <sqlite3.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "control_plane.h"
#include "package_controller.h"
#include "principal_store.h"
#include "provider_registry.h"
#include "runtime_observer.h"
#include "update_controller.h"

#define PORT 45821
#define VERSION "0.17.0"
#define SERVICE_SCHEMA_VERSION 1
#define ADMIN_TOKEN "__ROOTTOOLS_TOKEN__"
#define AGENT_TOKEN "__ROOTTOOLS_AGENT_TOKEN__"
#define MAX_REQUEST 524288
#define MAX_ACTION_BODY 458752
#define RT_DEFAULT_AUDIT_PATH "/var/mobile/Library/RootTools/audit.log"
#define RT_DEFAULT_LEDGER_PATH "/var/mobile/Library/RootTools/idempotency.sqlite3"

extern char **environ;

static void json_escape(const char *src, char *dst, size_t cap);
static int json_get_int(const char *body, const char *key, long *value);
static int json_has_key(const char *body, const char *key);
static void send_response(int fd, int code, const char *type, const char *body);
static void send_error(int fd, int code, const char *message);

static unsigned long audit_counter = 0;
static unsigned long request_counter = 0;

static int listen_port(void) {
    const char *override=getenv("ROOTTOOLS_PORT");
    if(!override||!override[0]) return PORT;
    char *end=NULL; long value=strtol(override,&end,10);
    return end&&*end==0&&value>1024&&value<=65535?(int)value:PORT;
}

static const char *audit_path(void) {
    const char *override=getenv("ROOTTOOLS_AUDIT_PATH");
    return (override&&override[0])?override:RT_DEFAULT_AUDIT_PATH;
}

static const char *ledger_path(void) {
    const char *override=getenv("ROOTTOOLS_LEDGER_PATH");
    return (override&&override[0])?override:RT_DEFAULT_LEDGER_PATH;
}

static const char *mobile_scope_root(void) {
    const char *override=getenv("ROOTTOOLS_MOBILE_SCOPE_ROOT");
    return (override&&override[0])?override:"/var/mobile/Library/RootTools/files";
}

static const char *bootstrap_scope_root(void) {
    const char *override=getenv("ROOTTOOLS_BOOTSTRAP_SCOPE_ROOT");
    return (override&&override[0])?override:"/var/jb/etc/roottools";
}

static const char *agent_token_path(void) {
    const char *override=getenv("ROOTTOOLS_AGENT_TOKEN_PATH");
    return (override&&override[0])?override:"/var/mobile/Library/RootTools/agent-token";
}

typedef enum {
    RT_LEDGER_NEW = 0,
    RT_LEDGER_REPLAY = 1,
    RT_LEDGER_CONFLICT = 2,
    RT_LEDGER_INDETERMINATE = 3,
    RT_LEDGER_ERROR = 4,
} RTLedgerReservationKind;

typedef struct {
    RTLedgerReservationKind kind;
    char *receipt;
    char detail[256];
} RTLedgerReservation;

static int sqlite_table_has_column(sqlite3 *db, const char *table, const char *column) {
    char sql[256]={0};
    int n=snprintf(sql,sizeof(sql),"PRAGMA table_info(%s)",table);
    if(n<=0||(size_t)n>=sizeof(sql))return 0;
    sqlite3_stmt *statement=NULL;
    int rc=sqlite3_prepare_v2(db,sql,-1,&statement,NULL);
    if(rc!=SQLITE_OK)return 0;
    int found=0;
    while((rc=sqlite3_step(statement))==SQLITE_ROW){
        const char *name=(const char*)sqlite3_column_text(statement,1);
        if(name&&!strcmp(name,column)){found=1;break;}
    }
    sqlite3_finalize(statement);
    return found;
}

static int ledger_open(sqlite3 **db_out) {
    sqlite3 *db=NULL;
    int rc=sqlite3_open_v2(
        ledger_path(),
        &db,
        SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE|SQLITE_OPEN_FULLMUTEX,
        NULL
    );
    if(rc!=SQLITE_OK){if(db)sqlite3_close(db);return 0;}
    sqlite3_busy_timeout(db,1500);
    const char *schema=
        "PRAGMA journal_mode=DELETE;"
        "PRAGMA synchronous=FULL;"
        "CREATE TABLE IF NOT EXISTS action_requests("
        "request_id TEXT PRIMARY KEY,"
        "caller TEXT NOT NULL,"
        "capability_id TEXT NOT NULL,"
        "request_hash TEXT NOT NULL,"
        "state INTEGER NOT NULL,"
        "receipt TEXT,"
        "created_at INTEGER NOT NULL,"
        "completed_at INTEGER"
        ");"
        "CREATE TABLE IF NOT EXISTS service_meta("
        "key TEXT PRIMARY KEY,"
        "value INTEGER NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS execution_events("
        "sequence INTEGER PRIMARY KEY AUTOINCREMENT,"
        "request_id TEXT NOT NULL,"
        "capability_id TEXT NOT NULL,"
        "caller TEXT NOT NULL,"
        "kind TEXT NOT NULL,"
        "revision INTEGER NOT NULL,"
        "occurred_at INTEGER NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS automation_jobs("
        "job_id TEXT PRIMARY KEY,"
        "kind TEXT NOT NULL,"
        "target TEXT NOT NULL,"
        "state TEXT NOT NULL,"
        "attempt_count INTEGER NOT NULL DEFAULT 0,"
        "created_at INTEGER NOT NULL,"
        "updated_at INTEGER NOT NULL,"
        "result TEXT,"
        "error TEXT"
        ");"
        "CREATE INDEX IF NOT EXISTS automation_jobs_state_idx ON automation_jobs(state,created_at);"
        "CREATE TABLE IF NOT EXISTS device_tasks("
        "task_id TEXT PRIMARY KEY,"
        "capability_id TEXT NOT NULL,"
        "kind TEXT NOT NULL,"
        "target TEXT NOT NULL,"
        "caller TEXT NOT NULL,"
        "state TEXT NOT NULL,"
        "requires_ui INTEGER NOT NULL DEFAULT 0,"
        "payload_json TEXT NOT NULL DEFAULT '{}',"
        "attempt_count INTEGER NOT NULL DEFAULT 0,"
        "created_at INTEGER NOT NULL,"
        "updated_at INTEGER NOT NULL,"
        "result TEXT,"
        "error TEXT"
        ");"
        "CREATE INDEX IF NOT EXISTS device_tasks_state_idx ON device_tasks(state,created_at);"
        "CREATE INDEX IF NOT EXISTS device_tasks_caller_idx ON device_tasks(caller,created_at);"
        "INSERT OR IGNORE INTO device_tasks(task_id,capability_id,kind,target,caller,state,requires_ui,payload_json,attempt_count,created_at,updated_at,result,error) "
        "SELECT job_id,'device.app.launch',kind,target,'legacy:automation',"
        "CASE state WHEN 'pending' THEN 'queued' ELSE state END,1,'{}',attempt_count,created_at,updated_at,result,error FROM automation_jobs;"
        "INSERT OR IGNORE INTO service_meta(key,value) VALUES('revision',0);";
    char *error=NULL;
    rc=sqlite3_exec(db,schema,NULL,NULL,&error);
    sqlite3_free(error);
    if(rc!=SQLITE_OK){sqlite3_close(db);return 0;}
    if(!sqlite_table_has_column(db,"device_tasks","payload_json")){
        rc=sqlite3_exec(db,"ALTER TABLE device_tasks ADD COLUMN payload_json TEXT NOT NULL DEFAULT '{}'",NULL,NULL,&error);
        sqlite3_free(error);error=NULL;
        if(rc!=SQLITE_OK){sqlite3_close(db);return 0;}
    }
    *db_out=db;
    return 1;
}

static int ledger_append_event_db(
    sqlite3 *db,
    const char *request_id,
    const char *capability_id,
    const char *caller,
    const char *kind,
    unsigned long long revision
) {
    sqlite3_stmt *statement=NULL;
    int rc=sqlite3_prepare_v2(db,
        "INSERT INTO execution_events(request_id,capability_id,caller,kind,revision,occurred_at) VALUES(?1,?2,?3,?4,?5,?6)",
        -1,&statement,NULL);
    if(rc==SQLITE_OK){
        sqlite3_bind_text(statement,1,request_id,-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(statement,2,capability_id,-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(statement,3,caller,-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(statement,4,kind,-1,SQLITE_TRANSIENT);
        sqlite3_bind_int64(statement,5,(sqlite3_int64)revision);
        sqlite3_bind_int64(statement,6,(sqlite3_int64)time(NULL));
        rc=sqlite3_step(statement);
    }
    sqlite3_finalize(statement); return rc==SQLITE_DONE;
}

static int ledger_append_event(
    const char *request_id,
    const char *capability_id,
    const char *caller,
    const char *kind,
    unsigned long long revision
) {
    sqlite3 *db=NULL; if(!ledger_open(&db))return 0;
    if(sqlite3_exec(db,"BEGIN IMMEDIATE",NULL,NULL,NULL)!=SQLITE_OK){sqlite3_close(db);return 0;}
    int ok=ledger_append_event_db(db,request_id,capability_id,caller,kind,revision)&&
        sqlite3_exec(db,"COMMIT",NULL,NULL,NULL)==SQLITE_OK;
    if(!ok)sqlite3_exec(db,"ROLLBACK",NULL,NULL,NULL);
    sqlite3_close(db); return ok;
}

static void request_fingerprint(
    const char *caller,
    const char *capability_id,
    const char *body,
    char out[CC_SHA256_DIGEST_LENGTH*2+1]
) {
    size_t caller_len=strlen(caller), capability_len=strlen(capability_id), body_len=strlen(body);
    size_t length=caller_len+1+capability_len+1+body_len;
    unsigned char *bytes=malloc(length?length:1);
    if(!bytes){out[0]=0;return;}
    size_t offset=0;
    memcpy(bytes+offset,caller,caller_len); offset+=caller_len; bytes[offset++]=0;
    memcpy(bytes+offset,capability_id,capability_len); offset+=capability_len; bytes[offset++]=0;
    memcpy(bytes+offset,body,body_len);
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(bytes,(CC_LONG)length,digest); free(bytes);
    for(size_t i=0;i<CC_SHA256_DIGEST_LENGTH;i++) snprintf(out+i*2,3,"%02x",digest[i]);
    out[CC_SHA256_DIGEST_LENGTH*2]=0;
}

static RTLedgerReservation ledger_reserve(
    const char *request_id,
    const char *caller,
    const char *capability_id,
    const char *body
) {
    RTLedgerReservation reservation={.kind=RT_LEDGER_ERROR,.receipt=NULL,.detail={0}};
    char fingerprint[CC_SHA256_DIGEST_LENGTH*2+1]={0};
    request_fingerprint(caller,capability_id,body,fingerprint);
    if(!fingerprint[0]){snprintf(reservation.detail,sizeof(reservation.detail),"request fingerprint allocation failed");return reservation;}

    sqlite3 *db=NULL;
    if(!ledger_open(&db)){snprintf(reservation.detail,sizeof(reservation.detail),"idempotency ledger unavailable");return reservation;}
    if(sqlite3_exec(db,"BEGIN IMMEDIATE",NULL,NULL,NULL)!=SQLITE_OK){
        snprintf(reservation.detail,sizeof(reservation.detail),"idempotency ledger lock failed");sqlite3_close(db);return reservation;
    }

    sqlite3_stmt *select=NULL;
    int rc=sqlite3_prepare_v2(db,
        "SELECT caller,capability_id,request_hash,state,receipt FROM action_requests WHERE request_id=?1",
        -1,&select,NULL);
    if(rc!=SQLITE_OK){sqlite3_exec(db,"ROLLBACK",NULL,NULL,NULL);sqlite3_close(db);snprintf(reservation.detail,sizeof(reservation.detail),"idempotency lookup failed");return reservation;}
    sqlite3_bind_text(select,1,request_id,-1,SQLITE_TRANSIENT);
    rc=sqlite3_step(select);
    if(rc==SQLITE_ROW){
        const char *stored_caller=(const char*)sqlite3_column_text(select,0);
        const char *stored_capability=(const char*)sqlite3_column_text(select,1);
        const char *stored_hash=(const char*)sqlite3_column_text(select,2);
        int state=sqlite3_column_int(select,3);
        const char *receipt=(const char*)sqlite3_column_text(select,4);
        int matches=stored_caller&&stored_capability&&stored_hash&&
            !strcmp(stored_caller,caller)&&!strcmp(stored_capability,capability_id)&&!strcmp(stored_hash,fingerprint);
        if(!matches){
            reservation.kind=RT_LEDGER_CONFLICT;
            snprintf(reservation.detail,sizeof(reservation.detail),"requestId already belongs to a different request");
        } else if(state==1&&receipt){
            reservation.kind=RT_LEDGER_REPLAY;
            reservation.receipt=strdup(receipt);
            if(!reservation.receipt){reservation.kind=RT_LEDGER_ERROR;snprintf(reservation.detail,sizeof(reservation.detail),"cached receipt allocation failed");}
        } else {
            reservation.kind=RT_LEDGER_INDETERMINATE;
            snprintf(reservation.detail,sizeof(reservation.detail),"prior request is pending; outcome is indeterminate and will not be replayed");
        }
        sqlite3_finalize(select);
        sqlite3_exec(db,"COMMIT",NULL,NULL,NULL);
        sqlite3_close(db);
        return reservation;
    }
    sqlite3_finalize(select);
    if(rc!=SQLITE_DONE){
        sqlite3_exec(db,"ROLLBACK",NULL,NULL,NULL);sqlite3_close(db);snprintf(reservation.detail,sizeof(reservation.detail),"idempotency lookup failed");return reservation;
    }

    sqlite3_stmt *insert=NULL;
    rc=sqlite3_prepare_v2(db,
        "INSERT INTO action_requests(request_id,caller,capability_id,request_hash,state,created_at) VALUES(?1,?2,?3,?4,0,?5)",
        -1,&insert,NULL);
    if(rc==SQLITE_OK){
        sqlite3_bind_text(insert,1,request_id,-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(insert,2,caller,-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(insert,3,capability_id,-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(insert,4,fingerprint,-1,SQLITE_TRANSIENT);
        sqlite3_bind_int64(insert,5,(sqlite3_int64)time(NULL));
        rc=sqlite3_step(insert);
    }
    sqlite3_finalize(insert);
    sqlite3_int64 revision=0;
    if(rc==SQLITE_DONE){
        sqlite3_stmt *revision_statement=NULL;
        rc=sqlite3_prepare_v2(db,"SELECT value FROM service_meta WHERE key='revision'",-1,&revision_statement,NULL);
        if(rc==SQLITE_OK){
            int revision_step=sqlite3_step(revision_statement);
            if(revision_step==SQLITE_ROW){revision=sqlite3_column_int64(revision_statement,0);rc=SQLITE_DONE;}
            else rc=SQLITE_ERROR;
        }
        sqlite3_finalize(revision_statement);
    }
    if(rc==SQLITE_DONE)rc=ledger_append_event_db(db,request_id,capability_id,caller,"accepted",revision)?SQLITE_DONE:SQLITE_ERROR;
    if(rc!=SQLITE_DONE||sqlite3_exec(db,"COMMIT",NULL,NULL,NULL)!=SQLITE_OK){
        sqlite3_exec(db,"ROLLBACK",NULL,NULL,NULL);sqlite3_close(db);snprintf(reservation.detail,sizeof(reservation.detail),"idempotency reservation failed");return reservation;
    }
    sqlite3_close(db);
    reservation.kind=RT_LEDGER_NEW;
    return reservation;
}

static int ledger_complete(
    const char *request_id,
    const char *receipt,
    const char *capability_id,
    const char *caller,
    const char *event_kind,
    unsigned long long revision
) {
    sqlite3 *db=NULL; if(!ledger_open(&db))return 0;
    if(sqlite3_exec(db,"BEGIN IMMEDIATE",NULL,NULL,NULL)!=SQLITE_OK){sqlite3_close(db);return 0;}
    sqlite3_stmt *statement=NULL;
    int rc=sqlite3_prepare_v2(db,
        "UPDATE action_requests SET state=1,receipt=?1,completed_at=?2 WHERE request_id=?3 AND state=0",
        -1,&statement,NULL);
    if(rc==SQLITE_OK){
        sqlite3_bind_text(statement,1,receipt,-1,SQLITE_TRANSIENT);
        sqlite3_bind_int64(statement,2,(sqlite3_int64)time(NULL));
        sqlite3_bind_text(statement,3,request_id,-1,SQLITE_TRANSIENT);
        rc=sqlite3_step(statement);
    }
    int changed=sqlite3_changes(db);
    sqlite3_finalize(statement);
    int event_ok=rc==SQLITE_DONE&&changed==1&&ledger_append_event_db(db,request_id,capability_id,caller,event_kind,revision);
    int committed=event_ok&&sqlite3_exec(db,"COMMIT",NULL,NULL,NULL)==SQLITE_OK;
    if(!committed)sqlite3_exec(db,"ROLLBACK",NULL,NULL,NULL);
    sqlite3_close(db); return committed;
}

static int ledger_current_revision(unsigned long long *revision_out) {
    sqlite3 *db=NULL; if(!ledger_open(&db))return 0;
    sqlite3_stmt *statement=NULL;
    int rc=sqlite3_prepare_v2(db,"SELECT value FROM service_meta WHERE key='revision'",-1,&statement,NULL);
    if(rc!=SQLITE_OK){sqlite3_close(db);return 0;}
    rc=sqlite3_step(statement);
    if(rc==SQLITE_ROW){
        sqlite3_int64 value=sqlite3_column_int64(statement,0);
        if(value<0){sqlite3_finalize(statement);sqlite3_close(db);return 0;}
        *revision_out=(unsigned long long)value;
        sqlite3_finalize(statement); sqlite3_close(db); return 1;
    }
    sqlite3_finalize(statement); sqlite3_close(db); return 0;
}

static int ledger_increment_revision(unsigned long long *revision_out) {
    sqlite3 *db=NULL; if(!ledger_open(&db))return 0;
    if(sqlite3_exec(db,"BEGIN IMMEDIATE",NULL,NULL,NULL)!=SQLITE_OK){sqlite3_close(db);return 0;}
    int rc=sqlite3_exec(db,"UPDATE service_meta SET value=value+1 WHERE key='revision'",NULL,NULL,NULL);
    if(rc!=SQLITE_OK||sqlite3_changes(db)!=1){sqlite3_exec(db,"ROLLBACK",NULL,NULL,NULL);sqlite3_close(db);return 0;}
    sqlite3_stmt *statement=NULL;
    rc=sqlite3_prepare_v2(db,"SELECT value FROM service_meta WHERE key='revision'",-1,&statement,NULL);
    if(rc!=SQLITE_OK){sqlite3_exec(db,"ROLLBACK",NULL,NULL,NULL);sqlite3_close(db);return 0;}
    rc=sqlite3_step(statement);
    if(rc!=SQLITE_ROW){sqlite3_finalize(statement);sqlite3_exec(db,"ROLLBACK",NULL,NULL,NULL);sqlite3_close(db);return 0;}
    sqlite3_int64 value=sqlite3_column_int64(statement,0);
    sqlite3_finalize(statement);
    if(value<0||sqlite3_exec(db,"COMMIT",NULL,NULL,NULL)!=SQLITE_OK){sqlite3_exec(db,"ROLLBACK",NULL,NULL,NULL);sqlite3_close(db);return 0;}
    sqlite3_close(db); *revision_out=(unsigned long long)value; return 1;
}

static int automation_enqueue_app_launch(const char *job_id, const char *bundle_id) {
    sqlite3 *db=NULL; if(!ledger_open(&db))return 0;
    sqlite3_stmt *statement=NULL;
    int rc=sqlite3_prepare_v2(db,
        "INSERT INTO automation_jobs(job_id,kind,target,state,attempt_count,created_at,updated_at) "
        "VALUES(?1,'app.launch',?2,'pending',0,?3,?3)",
        -1,&statement,NULL);
    if(rc==SQLITE_OK){
        sqlite3_bind_text(statement,1,job_id,-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(statement,2,bundle_id,-1,SQLITE_TRANSIENT);
        sqlite3_bind_int64(statement,3,(sqlite3_int64)time(NULL));
        rc=sqlite3_step(statement);
    }
    sqlite3_finalize(statement); sqlite3_close(db); return rc==SQLITE_DONE;
}

static int automation_cancel_pending(const char *job_id) {
    sqlite3 *db=NULL; if(!ledger_open(&db))return 0;
    sqlite3_stmt *statement=NULL;
    int rc=sqlite3_prepare_v2(db,
        "UPDATE automation_jobs SET state='cancelled',updated_at=?1,result='cancelled by caller',error=NULL "
        "WHERE job_id=?2 AND state='pending'",-1,&statement,NULL);
    if(rc==SQLITE_OK){
        sqlite3_bind_int64(statement,1,(sqlite3_int64)time(NULL));
        sqlite3_bind_text(statement,2,job_id,-1,SQLITE_TRANSIENT);
        rc=sqlite3_step(statement);
    }
    int changed=sqlite3_changes(db); sqlite3_finalize(statement); sqlite3_close(db);
    return rc==SQLITE_DONE&&changed==1;
}

static int task_count_state(const char *state) {
    sqlite3 *db=NULL; if(!ledger_open(&db))return -1;
    sqlite3_stmt *statement=NULL;
    int rc=sqlite3_prepare_v2(db,
        state?"SELECT COUNT(*) FROM device_tasks WHERE state=?1":"SELECT COUNT(*) FROM device_tasks",
        -1,&statement,NULL);
    if(rc!=SQLITE_OK){sqlite3_close(db);return -1;}
    if(state)sqlite3_bind_text(statement,1,state,-1,SQLITE_TRANSIENT);
    rc=sqlite3_step(statement);int count=-1;
    if(rc==SQLITE_ROW)count=sqlite3_column_int(statement,0);
    sqlite3_finalize(statement);sqlite3_close(db);return count;
}

static int task_active_count(void) {
    sqlite3 *db=NULL; if(!ledger_open(&db))return -1;
    sqlite3_stmt *statement=NULL;
    int rc=sqlite3_prepare_v2(db,
        "SELECT COUNT(*) FROM device_tasks WHERE state IN ('queued','waiting_for_unlock','running','retrying')",
        -1,&statement,NULL);
    if(rc!=SQLITE_OK){sqlite3_close(db);return -1;}
    rc=sqlite3_step(statement);int count=-1;
    if(rc==SQLITE_ROW)count=sqlite3_column_int(statement,0);
    sqlite3_finalize(statement);sqlite3_close(db);return count;
}

static int task_enqueue_app_launch(const char *task_id, const char *bundle_id, const char *caller) {
    sqlite3 *db=NULL;if(!ledger_open(&db))return 0;
    sqlite3_stmt *statement=NULL;
    int rc=sqlite3_prepare_v2(db,
        "INSERT INTO device_tasks(task_id,capability_id,kind,target,caller,state,requires_ui,payload_json,attempt_count,created_at,updated_at) "
        "VALUES(?1,'device.app.launch','app.launch',?2,?3,'queued',1,?4,0,?5,?5)",
        -1,&statement,NULL);
    if(rc==SQLITE_OK){
        char escaped[512]={0},payload[640]={0};json_escape(bundle_id,escaped,sizeof(escaped));
        snprintf(payload,sizeof(payload),"{\"bundleID\":\"%s\"}",escaped);
        sqlite3_bind_text(statement,1,task_id,-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(statement,2,bundle_id,-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(statement,3,caller,-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(statement,4,payload,-1,SQLITE_TRANSIENT);
        sqlite3_bind_int64(statement,5,(sqlite3_int64)time(NULL));
        rc=sqlite3_step(statement);
    }
    sqlite3_finalize(statement);sqlite3_close(db);return rc==SQLITE_DONE;
}

static int task_enqueue_ui_action(
    const char *task_id,
    const char *capability_id,
    const char *kind,
    const char *target,
    const char *caller,
    const char *payload_json
) {
    sqlite3 *db=NULL;if(!ledger_open(&db))return 0;
    sqlite3_stmt *statement=NULL;
    int rc=sqlite3_prepare_v2(db,
        "INSERT INTO device_tasks(task_id,capability_id,kind,target,caller,state,requires_ui,payload_json,attempt_count,created_at,updated_at) "
        "VALUES(?1,?2,?3,?4,?5,'queued',1,?6,0,?7,?7)",
        -1,&statement,NULL);
    if(rc==SQLITE_OK){
        sqlite3_bind_text(statement,1,task_id,-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(statement,2,capability_id,-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(statement,3,kind,-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(statement,4,target,-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(statement,5,caller,-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(statement,6,payload_json,-1,SQLITE_TRANSIENT);
        sqlite3_bind_int64(statement,7,(sqlite3_int64)time(NULL));
        rc=sqlite3_step(statement);
    }
    sqlite3_finalize(statement);sqlite3_close(db);return rc==SQLITE_DONE;
}

static int task_state(const char *task_id, char *state, size_t state_cap) {
    sqlite3 *db=NULL;if(!ledger_open(&db))return 0;
    sqlite3_stmt *statement=NULL;
    int rc=sqlite3_prepare_v2(db,"SELECT state FROM device_tasks WHERE task_id=?1",-1,&statement,NULL);
    if(rc==SQLITE_OK){sqlite3_bind_text(statement,1,task_id,-1,SQLITE_TRANSIENT);rc=sqlite3_step(statement);}
    int ok=0;
    if(rc==SQLITE_ROW){const char *value=(const char*)sqlite3_column_text(statement,0);if(value){snprintf(state,state_cap,"%s",value);ok=1;}}
    sqlite3_finalize(statement);sqlite3_close(db);return ok;
}

static int task_cancel(const char *task_id, const char *caller, int owner) {
    sqlite3 *db=NULL;if(!ledger_open(&db))return 0;
    sqlite3_stmt *statement=NULL;
    const char *sql=owner?
        "UPDATE device_tasks SET state='cancelled',updated_at=?1,result='cancelled by owner',error=NULL "
        "WHERE task_id=?2 AND state IN ('queued','waiting_for_unlock','retrying')":
        "UPDATE device_tasks SET state='cancelled',updated_at=?1,result='cancelled by caller',error=NULL "
        "WHERE task_id=?2 AND caller=?3 AND state IN ('queued','waiting_for_unlock','retrying')";
    int rc=sqlite3_prepare_v2(db,sql,-1,&statement,NULL);
    if(rc==SQLITE_OK){
        sqlite3_bind_int64(statement,1,(sqlite3_int64)time(NULL));
        sqlite3_bind_text(statement,2,task_id,-1,SQLITE_TRANSIENT);
        if(!owner)sqlite3_bind_text(statement,3,caller,-1,SQLITE_TRANSIENT);
        rc=sqlite3_step(statement);
    }
    int changed=sqlite3_changes(db);sqlite3_finalize(statement);sqlite3_close(db);
    return rc==SQLITE_DONE&&changed==1;
}

static int task_next(
    char *task_id,size_t task_cap,char *kind,size_t kind_cap,char *target,size_t target_cap,
    char *caller,size_t caller_cap,char *capability,size_t capability_cap,char *payload,size_t payload_cap,
    int *attempts,int *requires_ui,char *state,size_t state_cap
) {
    sqlite3 *db=NULL;if(!ledger_open(&db))return 0;
    sqlite3_stmt *statement=NULL;
    int rc=sqlite3_prepare_v2(db,
        "SELECT task_id,kind,target,caller,capability_id,payload_json,attempt_count,requires_ui,state FROM device_tasks "
        "WHERE state IN ('queued','waiting_for_unlock','retrying') ORDER BY created_at LIMIT 1",
        -1,&statement,NULL);
    if(rc==SQLITE_OK)rc=sqlite3_step(statement);
    int ok=0;
    if(rc==SQLITE_ROW){
        const char *id=(const char*)sqlite3_column_text(statement,0),*k=(const char*)sqlite3_column_text(statement,1);
        const char *t=(const char*)sqlite3_column_text(statement,2),*c=(const char*)sqlite3_column_text(statement,3);
        const char *capability_id=(const char*)sqlite3_column_text(statement,4),*payload_json=(const char*)sqlite3_column_text(statement,5);
        const char *s=(const char*)sqlite3_column_text(statement,8);
        if(id&&k&&t&&c&&capability_id&&payload_json&&s&&strlen(id)<task_cap&&strlen(k)<kind_cap&&strlen(t)<target_cap&&strlen(c)<caller_cap&&strlen(capability_id)<capability_cap&&strlen(payload_json)<payload_cap&&strlen(s)<state_cap){
            snprintf(task_id,task_cap,"%s",id);snprintf(kind,kind_cap,"%s",k);snprintf(target,target_cap,"%s",t);
            snprintf(caller,caller_cap,"%s",c);snprintf(capability,capability_cap,"%s",capability_id);
            snprintf(payload,payload_cap,"%s",payload_json);snprintf(state,state_cap,"%s",s);
            *attempts=sqlite3_column_int(statement,6);*requires_ui=sqlite3_column_int(statement,7);ok=1;
        }
    }
    sqlite3_finalize(statement);sqlite3_close(db);return ok;
}

static int task_update(const char *task_id,const char *state,const char *result,const char *error,int increment_attempt) {
    sqlite3 *db=NULL;if(!ledger_open(&db))return 0;
    sqlite3_stmt *statement=NULL;
    int rc=sqlite3_prepare_v2(db,
        "UPDATE device_tasks SET state=?1,attempt_count=attempt_count+?2,updated_at=?3,result=?4,error=?5 WHERE task_id=?6",
        -1,&statement,NULL);
    if(rc==SQLITE_OK){
        sqlite3_bind_text(statement,1,state,-1,SQLITE_TRANSIENT);
        sqlite3_bind_int(statement,2,increment_attempt?1:0);
        sqlite3_bind_int64(statement,3,(sqlite3_int64)time(NULL));
        if(result)sqlite3_bind_text(statement,4,result,-1,SQLITE_TRANSIENT);else sqlite3_bind_null(statement,4);
        if(error)sqlite3_bind_text(statement,5,error,-1,SQLITE_TRANSIENT);else sqlite3_bind_null(statement,5);
        sqlite3_bind_text(statement,6,task_id,-1,SQLITE_TRANSIENT);
        rc=sqlite3_step(statement);
    }
    int changed=sqlite3_changes(db);sqlite3_finalize(statement);sqlite3_close(db);return rc==SQLITE_DONE&&changed==1;
}

static void task_recover_incomplete(void) {
    sqlite3 *db=NULL;if(!ledger_open(&db))return;
    sqlite3_exec(db,
        "UPDATE device_tasks SET state='queued',updated_at=strftime('%s','now'),error='recovered after daemon restart' "
        "WHERE state='running'",NULL,NULL,NULL);
    sqlite3_close(db);
}

static char *receipt_mark_replayed(const char *stored) {
    const char *needle="\"replayed\":false";
    const char *position=strstr(stored,needle);
    if(!position)return NULL;
    const char *replacement="\"replayed\":true";
    size_t prefix=(size_t)(position-stored);
    size_t length=strlen(stored)-strlen(needle)+strlen(replacement)+1;
    char *out=malloc(length); if(!out)return NULL;
    memcpy(out,stored,prefix); out[prefix]=0;
    strcat(out,replacement); strcat(out,position+strlen(needle));
    return out;
}

static void send_event_replay(int fd, const char *body) {
    long after_raw=0,limit_raw=100;
    if(json_has_key(body,"afterSequence")&&(!json_get_int(body,"afterSequence",&after_raw)||after_raw<0)){
        send_error(fd,400,"afterSequence must be a non-negative integer");return;
    }
    if(json_has_key(body,"limit")&&(!json_get_int(body,"limit",&limit_raw)||limit_raw<1||limit_raw>200)){
        send_error(fd,400,"limit must be between 1 and 200");return;
    }
    sqlite3 *db=NULL; if(!ledger_open(&db)){send_error(fd,503,"execution event ledger unavailable");return;}
    sqlite3_stmt *statement=NULL;
    int rc=sqlite3_prepare_v2(db,
        "SELECT sequence,request_id,capability_id,caller,kind,revision,occurred_at FROM execution_events WHERE sequence>?1 ORDER BY sequence LIMIT ?2",
        -1,&statement,NULL);
    if(rc!=SQLITE_OK){sqlite3_close(db);send_error(fd,500,"execution event query failed");return;}
    sqlite3_bind_int64(statement,1,(sqlite3_int64)after_raw);
    sqlite3_bind_int(statement,2,(int)limit_raw+1);
    char *response=calloc(1,65536); if(!response){sqlite3_finalize(statement);sqlite3_close(db);send_error(fd,500,"execution event allocation failed");return;}
    size_t used=0; int row=0; int has_more=0; unsigned long long last_sequence=(unsigned long long)after_raw;
    int n=snprintf(response,65536,"{\"schemaVersion\":1,\"events\":[");
    if(n<0){free(response);sqlite3_finalize(statement);sqlite3_close(db);send_error(fd,500,"execution event encoding failed");return;} used=(size_t)n;
    while((rc=sqlite3_step(statement))==SQLITE_ROW){
        if(row>=(int)limit_raw){has_more=1;break;}
        sqlite3_int64 sequence=sqlite3_column_int64(statement,0);
        const char *request_id=(const char*)sqlite3_column_text(statement,1);
        const char *capability_id=(const char*)sqlite3_column_text(statement,2);
        const char *caller=(const char*)sqlite3_column_text(statement,3);
        const char *kind=(const char*)sqlite3_column_text(statement,4);
        sqlite3_int64 revision=sqlite3_column_int64(statement,5),occurred_at=sqlite3_column_int64(statement,6);
        char erequest[256]={0},ecapability[512]={0},ecaller[256]={0},ekind[128]={0};
        json_escape(request_id?request_id:"",erequest,sizeof(erequest)); json_escape(capability_id?capability_id:"",ecapability,sizeof(ecapability));
        json_escape(caller?caller:"",ecaller,sizeof(ecaller)); json_escape(kind?kind:"",ekind,sizeof(ekind));
        n=snprintf(response+used,65536-used,
            "%s{\"sequence\":%lld,\"requestId\":\"%s\",\"capabilityId\":\"%s\",\"caller\":\"%s\",\"kind\":\"%s\",\"revision\":%lld,\"occurredAt\":%lld}",
            row?",":"",(long long)sequence,erequest,ecapability,ecaller,ekind,(long long)revision,(long long)occurred_at);
        if(n<0||(size_t)n>=65536-used){free(response);sqlite3_finalize(statement);sqlite3_close(db);send_error(fd,500,"execution event response too large");return;}
        used+=(size_t)n; row++; last_sequence=(unsigned long long)sequence;
    }
    sqlite3_finalize(statement);
    unsigned long long latest_sequence=0; sqlite3_stmt *latest=NULL;
    if(sqlite3_prepare_v2(db,"SELECT COALESCE(MAX(sequence),0) FROM execution_events",-1,&latest,NULL)==SQLITE_OK&&sqlite3_step(latest)==SQLITE_ROW){
        sqlite3_int64 value=sqlite3_column_int64(latest,0); if(value>0)latest_sequence=(unsigned long long)value;
    }
    sqlite3_finalize(latest); sqlite3_close(db);
    n=snprintf(response+used,65536-used,
        "],\"count\":%d,\"afterSequence\":%ld,\"lastSequence\":%llu,\"latestSequence\":%llu,\"hasMore\":%s}",
        row,after_raw,last_sequence,latest_sequence,has_more?"true":"false");
    if(n<0||(size_t)n>=65536-used){free(response);send_error(fd,500,"execution event response too large");return;}
    send_response(fd,200,"application/json",response); free(response);
}

static void json_escape(const char *src, char *dst, size_t cap) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 2 < cap; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\') { dst[j++]='\\'; dst[j++]=c; }
        else if (c == '\n') { dst[j++]='\\'; dst[j++]='n'; }
        else if (c == '\r') { dst[j++]='\\'; dst[j++]='r'; }
        else if (c == '\t') { dst[j++]='\\'; dst[j++]='t'; }
        else if (c >= 0x20) dst[j++] = c;
    }
    dst[j] = 0;
}

static void appendf(char *out, size_t cap, const char *fmt, ...) {
    size_t used = strlen(out); if (used + 2 >= cap) return;
    va_list ap; va_start(ap, fmt); vsnprintf(out + used, cap - used, fmt, ap); va_end(ap);
}

static void ensure_action_dirs(void) {
    mkdir("/var/mobile/Library/RootTools", 0750);
    mkdir(mobile_scope_root(), 0750);
    mkdir(bootstrap_scope_root(), 0750);
}

static int constant_time_token_equal(const char *presented, size_t presented_len, const char *expected) {
    size_t expected_len=strlen(expected);
    size_t max_len=presented_len>expected_len?presented_len:expected_len;
    unsigned int diff=(unsigned int)(presented_len^expected_len);
    for(size_t i=0;i<max_len;i++){
        unsigned char a=i<presented_len?(unsigned char)presented[i]:0;
        unsigned char b=i<expected_len?(unsigned char)expected[i]:0;
        diff|=(unsigned int)(a^b);
    }
    return diff==0;
}

static int load_runtime_agent_token(char *out, size_t cap, int *runtime_file_present) {
    if(runtime_file_present)*runtime_file_present=0;
    int fd=open(agent_token_path(),O_RDONLY|O_NOFOLLOW);
    if(fd<0){
        if(errno==ENOENT){snprintf(out,cap,"%s",AGENT_TOKEN);return 1;}
        return 0;
    }
    if(runtime_file_present)*runtime_file_present=1;
    struct stat st;
    if(fstat(fd,&st)!=0||st.st_size<0||st.st_size>120){close(fd);return 0;}
    ssize_t n=read(fd,out,cap-1); close(fd); if(n<0)return 0; out[n]=0;
    while(n>0&&(out[n-1]=='\n'||out[n-1]=='\r'||out[n-1]==' '||out[n-1]=='\t'))out[--n]=0;
    return 1;
}

static int persist_runtime_agent_token(const char *token) {
    ensure_action_dirs();
    int fd=open(agent_token_path(),O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0600); if(fd<0)return 0;
    size_t length=strlen(token); ssize_t written=write(fd,token,length); int sync_ok=fsync(fd)==0; close(fd);
    return written==(ssize_t)length&&sync_ok;
}

static void make_audit_id(char *out, size_t cap) {
    audit_counter++;
    snprintf(out, cap, "%lld-%d-%lu", (long long)time(NULL), getpid(), audit_counter);
}

static void audit_action(
    const char *audit_id,
    const char *request_id,
    const RTCapability *capability,
    const char *caller,
    const char *target,
    int ok,
    int executed,
    const char *result,
    const char *policy,
    const char *message,
    int post_checked,
    int post_passed,
    const char *post_detail,
    unsigned long long revision
) {
    ensure_action_dirs();
    FILE *f = fopen(audit_path(), "a");
    if (!f) return;
    char rid[128]={0}, cid[256]={0}, action[128]={0}, provider[256]={0}, c[128]={0}, t[512]={0}, r[128]={0}, p[128]={0}, m[1024]={0}, pd[1024]={0};
    json_escape(request_id, rid, sizeof(rid));
    json_escape(capability ? capability->id : "unknown", cid, sizeof(cid));
    json_escape(capability && capability->legacy_action ? capability->legacy_action : "unknown", action, sizeof(action));
    const char *provider_id=capability?rt_provider_for_capability(capability->id):NULL;
    json_escape(provider_id?provider_id:"unbound", provider, sizeof(provider));
    json_escape(caller, c, sizeof(c)); json_escape(target, t, sizeof(t)); json_escape(result, r, sizeof(r));
    json_escape(policy, p, sizeof(p)); json_escape(message, m, sizeof(m)); json_escape(post_detail, pd, sizeof(pd));
    fprintf(f,
            "{\"time\":%lld,\"requestId\":\"%s\",\"auditId\":\"%s\",\"capabilityId\":\"%s\",\"providerId\":\"%s\",\"action\":\"%s\",\"risk\":\"%s\",\"caller\":\"%s\",\"target\":\"%s\",\"ok\":%s,\"executed\":%s,\"revision\":%llu,\"result\":\"%s\",\"policy\":\"%s\",\"message\":\"%s\",\"postCondition\":{\"checked\":%s,\"passed\":%s,\"detail\":\"%s\"}}\n",
            (long long)time(NULL), rid, audit_id, cid, provider, action, capability ? rt_risk_name(capability->risk) : "R3", c, t,
            ok ? "true" : "false", executed ? "true" : "false", revision, r, p, m, post_checked ? "true" : "false", post_passed ? "true" : "false", pd);
    fclose(f);
}

static int port_open(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET; addr.sin_port = htons(port); addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int flags = fcntl(fd, F_GETFL, 0); fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int rc = connect(fd, (struct sockaddr*)&addr, sizeof(addr));
    if (rc == 0) { close(fd); return 1; }
    if (errno != EINPROGRESS) { close(fd); return 0; }
    fd_set wfds; FD_ZERO(&wfds); FD_SET(fd, &wfds);
    struct timeval tv = {.tv_sec=0, .tv_usec=100000};
    rc = select(fd+1, NULL, &wfds, NULL, &tv);
    close(fd); return rc > 0;
}

typedef struct {
    int lock_known;
    int locked;
    int screen_blank_known;
    int screen_blanked;
    uint64_t lock_raw;
    uint64_t screen_blank_raw;
    const char *lock_source;
    const char *screen_source;
} RTLockSnapshot;

typedef int (*RTNotifyRegisterCheck)(const char *, int *);
typedef int (*RTNotifyGetState)(int, uint64_t *);
typedef int (*RTNotifyCancel)(int);

static int darwin_notify_state(const char *name, uint64_t *state_out) {
    RTNotifyRegisterCheck register_check=(RTNotifyRegisterCheck)dlsym(RTLD_DEFAULT,"notify_register_check");
    RTNotifyGetState get_state=(RTNotifyGetState)dlsym(RTLD_DEFAULT,"notify_get_state");
    RTNotifyCancel cancel=(RTNotifyCancel)dlsym(RTLD_DEFAULT,"notify_cancel");
    void *handle=NULL;
    if(!register_check||!get_state||!cancel){
        handle=dlopen("/usr/lib/system/libsystem_notify.dylib",RTLD_LAZY|RTLD_LOCAL);
        if(handle){
            register_check=(RTNotifyRegisterCheck)dlsym(handle,"notify_register_check");
            get_state=(RTNotifyGetState)dlsym(handle,"notify_get_state");
            cancel=(RTNotifyCancel)dlsym(handle,"notify_cancel");
        }
    }
    if(!register_check||!get_state||!cancel){if(handle)dlclose(handle);return 0;}
    int token=0; uint64_t state=0;
    int ok=register_check(name,&token)==0 && get_state(token,&state)==0;
    if(token>0)cancel(token);
    if(handle)dlclose(handle);
    if(ok&&state_out)*state_out=state;
    return ok;
}

static RTLockSnapshot device_lock_snapshot(void) {
    RTLockSnapshot snapshot={0};
    const char *test_lock=getenv("ROOTTOOLS_TEST_LOCK_STATE");
    if(test_lock&&test_lock[0]){
        snapshot.lock_known=strcmp(test_lock,"unknown")!=0;
        snapshot.locked=!strcmp(test_lock,"locked");
        snapshot.lock_raw=snapshot.locked?1:0;
        snapshot.lock_source="test-override";
    } else if(darwin_notify_state("com.apple.springboard.lockstate",&snapshot.lock_raw)) {
        snapshot.lock_known=1;
        snapshot.locked=snapshot.lock_raw!=0;
        snapshot.lock_source="darwin-notify:com.apple.springboard.lockstate";
    } else {
        snapshot.lock_source="unavailable";
    }

    const char *test_blank=getenv("ROOTTOOLS_TEST_SCREEN_BLANKED");
    if(test_blank&&test_blank[0]){
        snapshot.screen_blank_known=strcmp(test_blank,"unknown")!=0;
        snapshot.screen_blanked=!strcmp(test_blank,"1")||!strcmp(test_blank,"true")||!strcmp(test_blank,"blanked");
        snapshot.screen_blank_raw=snapshot.screen_blanked?1:0;
        snapshot.screen_source="test-override";
    } else if(darwin_notify_state("com.apple.springboard.hasBlankedScreen",&snapshot.screen_blank_raw)) {
        snapshot.screen_blank_known=1;
        snapshot.screen_blanked=snapshot.screen_blank_raw!=0;
        snapshot.screen_source="darwin-notify:com.apple.springboard.hasBlankedScreen";
    } else {
        snapshot.screen_source="unavailable";
    }
    return snapshot;
}

static int ui_execution_ready(RTLockSnapshot snapshot) {
    if(!snapshot.lock_known||snapshot.locked)return 0;
    if(snapshot.screen_blank_known&&snapshot.screen_blanked)return 0;
    return 1;
}

static const char *lock_state_name(RTLockSnapshot snapshot) {
    if(!snapshot.lock_known)return "unknown";
    return snapshot.locked?"locked":"unlocked";
}

static const char *screen_state_name(RTLockSnapshot snapshot) {
    if(!snapshot.screen_blank_known)return "unknown";
    return snapshot.screen_blanked?"blanked":"visible";
}

static unsigned long long sysctl_u64(const char *name) {
    unsigned long long value = 0; size_t size = sizeof(value);
    sysctlbyname(name, &value, &size, NULL, 0); return value;
}

static int sysctl_int(const char *name) {
    int value = 0; size_t size = sizeof(value);
    sysctlbyname(name, &value, &size, NULL, 0); return value;
}

static void sysctl_string(const char *name, char *out, size_t cap) {
    size_t size = cap; if (sysctlbyname(name, out, &size, NULL, 0) != 0) snprintf(out, cap, "unknown");
    out[cap-1] = 0;
}

static unsigned long long free_bytes(const char *path) {
    struct statfs s; if (statfs(path, &s) != 0) return 0;
    return (unsigned long long)s.f_bavail * (unsigned long long)s.f_bsize;
}

static unsigned long long uptime_seconds(void) {
    struct timeval boot={0}; size_t size=sizeof(boot);
    if(sysctlbyname("kern.boottime",&boot,&size,NULL,0)!=0||boot.tv_sec<=0)return 0;
    time_t now=time(NULL); return now>boot.tv_sec?(unsigned long long)(now-boot.tv_sec):0;
}

static int process_count_snapshot(void) {
    size_t length=0; int mib[4]={CTL_KERN,KERN_PROC,KERN_PROC_ALL,0};
    if(sysctl(mib,4,NULL,&length,NULL,0)!=0||length==0)return -1;
    return (int)(length/sizeof(struct kinfo_proc));
}

typedef struct {
    int available;
    unsigned long long free_bytes;
    unsigned long long active_bytes;
    unsigned long long inactive_bytes;
    unsigned long long wired_bytes;
} RTMemorySnapshot;

static RTMemorySnapshot memory_snapshot(void) {
    RTMemorySnapshot snapshot={0};
    mach_port_t host=mach_host_self();
    vm_size_t page_size=0;
    vm_statistics64_data_t stats={0};
    mach_msg_type_number_t count=HOST_VM_INFO64_COUNT;
    if(host_page_size(host,&page_size)==KERN_SUCCESS&&
       host_statistics64(host,HOST_VM_INFO64,(host_info64_t)&stats,&count)==KERN_SUCCESS){
        snapshot.available=1;
        snapshot.free_bytes=(unsigned long long)stats.free_count*(unsigned long long)page_size;
        snapshot.active_bytes=(unsigned long long)stats.active_count*(unsigned long long)page_size;
        snapshot.inactive_bytes=(unsigned long long)stats.inactive_count*(unsigned long long)page_size;
        snapshot.wired_bytes=(unsigned long long)stats.wire_count*(unsigned long long)page_size;
    }
    mach_port_deallocate(mach_task_self(),host);
    return snapshot;
}

static unsigned long long daemon_resident_bytes(void) {
    mach_task_basic_info_data_t info={0};
    mach_msg_type_number_t count=MACH_TASK_BASIC_INFO_COUNT;
    if(task_info(mach_task_self(),MACH_TASK_BASIC_INFO,(task_info_t)&info,&count)!=KERN_SUCCESS)return 0;
    return (unsigned long long)info.resident_size;
}

static char *processes_text(int *dopamineRunning) {
    size_t length = 0;
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0};
    if (sysctl(mib, 4, NULL, &length, NULL, 0) != 0 || length == 0) return strdup("process query unavailable\n");
    struct kinfo_proc *items = malloc(length);
    if (!items || sysctl(mib, 4, items, &length, NULL, 0) != 0) { free(items); return strdup("process query failed\n"); }
    size_t count = length / sizeof(struct kinfo_proc);
    char *out = calloc(1, 65536); appendf(out, 65536, "PID     UID  COMMAND\n");
    if (dopamineRunning) *dopamineRunning = 0;
    for (size_t i = 0; i < count; i++) {
        pid_t pid = items[i].kp_proc.p_pid;
        uid_t uid = items[i].kp_eproc.e_ucred.cr_uid;
        const char *name = items[i].kp_proc.p_comm;
        if (dopamineRunning && strstr(name, "Dopamine")) *dopamineRunning = 1;
        appendf(out, 65536, "%-7d %-4u %s\n", pid, uid, name);
        if (strlen(out) > 62000) break;
    }
    free(items); return out;
}

static const char *system_apps_root(void) {
    const char *override=getenv("ROOTTOOLS_SYSTEM_APPS_ROOT");
    return override&&override[0]?override:"/Applications";
}

static const char *jailbreak_apps_root(void) {
    const char *override=getenv("ROOTTOOLS_JAILBREAK_APPS_ROOT");
    return override&&override[0]?override:"/var/jb/Applications";
}

static const char *user_apps_root(void) {
    const char *override=getenv("ROOTTOOLS_USER_APPS_ROOT");
    return override&&override[0]?override:"/var/containers/Bundle/Application";
}

static int process_info(pid_t wanted, uid_t *uid_out, char *name, size_t name_cap) {
    size_t length = 0; int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0};
    if (sysctl(mib, 4, NULL, &length, NULL, 0) != 0 || length == 0) return 0;
    struct kinfo_proc *items = malloc(length); if (!items) return 0;
    if (sysctl(mib, 4, items, &length, NULL, 0) != 0) { free(items); return 0; }
    size_t count = length / sizeof(struct kinfo_proc); int found = 0;
    for (size_t i = 0; i < count; i++) {
        if (items[i].kp_proc.p_pid != wanted) continue;
        if (uid_out) *uid_out = items[i].kp_eproc.e_ucred.cr_uid;
        if (name && name_cap) snprintf(name, name_cap, "%s", items[i].kp_proc.p_comm);
        found = 1; break;
    }
    free(items); return found;
}

typedef struct {
    int available;
    unsigned long long user_time_ns;
    unsigned long long system_time_ns;
    unsigned long long resident_bytes;
    unsigned long long footprint_bytes;
    unsigned long long disk_read_bytes;
    unsigned long long disk_write_bytes;
    unsigned long long pageins;
    unsigned long long idle_wakeups;
    unsigned long long interrupt_wakeups;
} RTProcessMetrics;

typedef int (*RTProcPidRusageFn)(int, int, rusage_info_t *);

static RTProcessMetrics process_metrics(pid_t pid) {
    RTProcessMetrics metrics={0};
    RTProcPidRusageFn function=(RTProcPidRusageFn)dlsym(RTLD_DEFAULT,"proc_pid_rusage");
    if(!function)return metrics;
    struct rusage_info_v2 usage={0};
    if(function((int)pid,RUSAGE_INFO_V2,(rusage_info_t *)&usage)!=0)return metrics;
    metrics.available=1;
    metrics.user_time_ns=usage.ri_user_time;
    metrics.system_time_ns=usage.ri_system_time;
    metrics.resident_bytes=usage.ri_resident_size;
    metrics.footprint_bytes=usage.ri_phys_footprint;
    metrics.disk_read_bytes=usage.ri_diskio_bytesread;
    metrics.disk_write_bytes=usage.ri_diskio_byteswritten;
    metrics.pageins=usage.ri_pageins;
    metrics.idle_wakeups=usage.ri_pkg_idle_wkups;
    metrics.interrupt_wakeups=usage.ri_interrupt_wkups;
    return metrics;
}

static int has_suffix(const char *text, const char *suffix) {
    size_t a=strlen(text), b=strlen(suffix); return a>=b && !strcmp(text+a-b, suffix);
}

static void scan_app_dir(char *out, size_t cap, const char *root, int nested) {
    DIR *dir = opendir(root); if (!dir) { appendf(out, cap, "%s [unavailable]\n", root); return; }
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (entry->d_name[0] == '.') continue;
        char path[1024]; snprintf(path, sizeof(path), "%s/%s", root, entry->d_name);
        if (has_suffix(entry->d_name, ".app")) appendf(out, cap, "%s\n", path);
        else if (nested) {
            DIR *sub = opendir(path); if (!sub) continue;
            struct dirent *child;
            while ((child = readdir(sub))) {
                if (child->d_name[0] != '.' && has_suffix(child->d_name, ".app")) appendf(out, cap, "%s/%s\n", path, child->d_name);
            }
            closedir(sub);
        }
        if (strlen(out) > cap - 2048) break;
    }
    closedir(dir);
}

static char *apps_text(void) {
    char *out = calloc(1, 65536);
    appendf(out, 65536, "SYSTEM APPS\n"); scan_app_dir(out, 65536, system_apps_root(), 0);
    appendf(out, 65536, "\nJAILBREAK APPS\n"); scan_app_dir(out, 65536, jailbreak_apps_root(), 0);
    appendf(out, 65536, "\nUSER APPS\n"); scan_app_dir(out, 65536, user_apps_root(), 1);
    return out;
}

static void list_dir(char *out, size_t cap, const char *path) {
    appendf(out, cap, "%s\n", path);
    DIR *dir=opendir(path); if (!dir) { appendf(out, cap, "  [unavailable: %s]\n", strerror(errno)); return; }
    struct dirent *entry;
    while ((entry=readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char full[1024]; snprintf(full,sizeof(full),"%s/%s",path,entry->d_name);
        struct stat st; if (lstat(full,&st)==0) appendf(out,cap,"  %c %10llu  %s\n",S_ISDIR(st.st_mode)?'d':'-',(unsigned long long)st.st_size,entry->d_name);
        if (strlen(out)>cap-2048) break;
    }
    closedir(dir);
}

static char *files_text(void) {
    char *out=calloc(1,65536); list_dir(out,65536,"/var/jb"); appendf(out,65536,"\n"); list_dir(out,65536,"/var/mobile/Library"); return out;
}

static char *network_text(void) {
    char *out=calloc(1,65536); appendf(out,65536,"INTERFACES\n");
    struct ifaddrs *ifs=NULL; if (getifaddrs(&ifs)==0) {
        for (struct ifaddrs *it=ifs; it; it=it->ifa_next) {
            if (!it->ifa_addr) continue;
            int family=it->ifa_addr->sa_family; char addr[INET6_ADDRSTRLEN]={0};
            if (family==AF_INET) inet_ntop(AF_INET,&((struct sockaddr_in*)it->ifa_addr)->sin_addr,addr,sizeof(addr));
            else if (family==AF_INET6) inet_ntop(AF_INET6,&((struct sockaddr_in6*)it->ifa_addr)->sin6_addr,addr,sizeof(addr));
            else continue;
            appendf(out,65536,"%-10s %s\n",it->ifa_name,addr);
        }
        freeifaddrs(ifs);
    }
    appendf(out,65536,"\nKNOWN LOCAL SERVICES\nSSH :22      %s\nFrida :27042 %s\nZXTouch :6000 %s\n",port_open(22)?"ready":"off",port_open(27042)?"ready":"off",port_open(6000)?"ready":"off");
    return out;
}

static char *runtime_text(void) {
    char *out=calloc(1,16384);
    appendf(out,16384,"identity\nuid=%d gid=%d\n\nbootstrap\n/var/jb: %s\nlaunchd helper: %s\n\nadapters\nSSH: %s\nFrida: %s\nZXTouch: %s\n\nwrite policy\napp.launch: R1\napp.terminate: R1\nprocess.terminate: R2 (non-root only)\nfile.read: R0 (RootTools scopes only)\nfile.write: R1 (RootTools scopes only)\nraw shell: unavailable\n",
        getuid(),getgid(),access("/var/jb",F_OK)==0?"ready":"missing",access("/var/jb/Library/LaunchDaemons/com.arthur.roottools.execd.plist",F_OK)==0?"installed":"missing",
        port_open(22)?"ready":"off",port_open(27042)?"ready":"off",port_open(6000)?"ready":"off");
    return out;
}

static char *diagnostics_text(void) {
    char machine[64]={0}, osbuild[64]={0}; struct utsname u; uname(&u); sysctl_string("hw.machine",machine,sizeof(machine)); sysctl_string("kern.osversion",osbuild,sizeof(osbuild));
    char *out=calloc(1,16384);
    appendf(out,16384,"ROOT TOOLS SNAPSHOT\nuid=%d gid=%d\nmachine=%s\nosBuild=%s\nkernel=%s\ncpu=%d\nmemory=%llu\nrootFree=%llu\nvarFree=%llu\nrootless=%s\nssh=%s frida=%s zxtouch=%s\ndaemonVersion=%s\n",
        getuid(),getgid(),machine,osbuild,u.release,sysctl_int("hw.ncpu"),sysctl_u64("hw.memsize"),free_bytes("/"),free_bytes("/var"),
        access("/var/jb",F_OK)==0?"true":"false",port_open(22)?"ready":"off",port_open(27042)?"ready":"off",port_open(6000)?"ready":"off",VERSION);
    return out;
}

static char *capabilities_text(void) {
    return rt_capabilities_text();
}

static char *audit_text(void) {
    ensure_action_dirs();
    int fd=open(audit_path(),O_RDONLY|O_NOFOLLOW); if(fd<0) return strdup("No privileged actions recorded yet.\n");
    struct stat st; if(fstat(fd,&st)!=0){close(fd);return strdup("Audit log unavailable.\n");}
    off_t start=st.st_size>49152?st.st_size-49152:0; lseek(fd,start,SEEK_SET);
    char *out=calloc(1,50000); ssize_t n=read(fd,out,49152); close(fd); if(n<0){free(out);return strdup("Audit log read failed.\n");}
    out[n]=0; return out;
}

static void send_response(int fd, int code, const char *type, const char *body) {
    const char *msg = code == 200 ? "OK" : code == 400 ? "Bad Request" : code == 401 ? "Unauthorized" : code == 403 ? "Forbidden" : code == 404 ? "Not Found" : code == 409 ? "Conflict" : code == 503 ? "Service Unavailable" : "Internal Server Error";
    char head[512];
    int n = snprintf(head, sizeof(head), "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", code, msg, type, strlen(body));
    send(fd, head, n, 0); send(fd, body, strlen(body), 0);
}

static void send_text_payload(int fd, char *raw) {
    size_t escapedCap = strlen(raw) * 2 + 64;
    char *escaped = calloc(1, escapedCap);
    json_escape(raw, escaped, escapedCap);
    size_t bodyCap = strlen(escaped) + 64;
    char *body = calloc(1, bodyCap);
    snprintf(body, bodyCap, "{\"ok\":true,\"output\":\"%s\"}", escaped);
    send_response(fd, 200, "application/json", body);
    free(raw); free(escaped); free(body);
}

static void send_capability_catalog(int fd) {
    char *body = rt_capabilities_json();
    if (!body) { send_response(fd, 500, "application/json", "{\"error\":\"capability_catalog_unavailable\"}"); return; }
    send_response(fd, 200, "application/json", body);
    free(body);
}

static void send_policy_status(int fd) {
    char *body=rt_policy_json();
    if(!body){send_error(fd,503,"execution policy unavailable");return;}
    send_response(fd,200,"application/json",body);
    free(body);
}

static void send_error(int fd, int code, const char *message) {
    char escaped[1024]={0}, body[1200]={0}; json_escape(message, escaped, sizeof(escaped));
    snprintf(body,sizeof(body),"{\"ok\":false,\"error\":\"%s\"}",escaped); send_response(fd,code,"application/json",body);
}

static int caller_principal_id(const char *caller, char *out, size_t cap) {
    const char *prefix="principal:";
    size_t prefix_len=strlen(prefix);
    if(!caller||strncmp(caller,prefix,prefix_len))return 0;
    const char *id=caller+prefix_len;
    if(!id[0]||strlen(id)>=cap)return 0;
    snprintf(out,cap,"%s",id);
    return 1;
}

static int principal_grant_allows(const char *caller, const char *capability_id) {
    char principal_id[RT_PRINCIPAL_ID_CAP]={0};
    if(!caller_principal_id(caller,principal_id,sizeof(principal_id)))return 1;
    return rt_principal_capability_allowed(principal_id,capability_id);
}

static int authorize_read_capability(int fd, const char *capability_id, const char *caller) {
    const RTCapability *capability=rt_capability_find(capability_id);
    if(!capability){send_error(fd,500,"read capability missing from registry");return 0;}
    RTPolicyDecision decision=rt_policy_evaluate(capability,0);
    if(!decision.allowed){send_error(fd,403,decision.reason?decision.reason:"read capability denied");return 0;}
    if(!principal_grant_allows(caller,capability_id)){
        send_error(fd,403,"capability is not granted to this command principal");return 0;
    }
    return 1;
}

typedef struct {
    char request_id[128];
    char caller[128];
    int confirmed;
} RTActionContext;

typedef struct {
    int ok;
    int executed;
    int post_checked;
    int post_passed;
    char result[64];
    char target[1024];
    char message[1024];
    char post_detail[1024];
    char *output;
} RTActionExecution;

static void execution_init(RTActionExecution *execution) {
    memset(execution, 0, sizeof(*execution));
    snprintf(execution->result, sizeof(execution->result), "failed");
}

static void execution_free(RTActionExecution *execution) {
    free(execution->output);
    execution->output = NULL;
}

static void send_action_receipt(
    int fd,
    const RTCapability *capability,
    const RTActionContext *context,
    RTPolicyDecision decision,
    const RTActionExecution *execution
) {
    unsigned long long revision=0;
    int revision_ok=(execution->executed&&capability->risk>RT_RISK_R0)
        ? ledger_increment_revision(&revision)
        : ledger_current_revision(&revision);
    if(!revision_ok){
        send_error(fd,500,"execution revision persistence failed; request remains indeterminate");
        return;
    }
    char audit_id[96]={0}; make_audit_id(audit_id,sizeof(audit_id));
    const char *policy = decision.policy ? decision.policy : "deny";
    audit_action(
        audit_id, context->request_id, capability, context->caller, execution->target,
        execution->ok, execution->executed, execution->result, policy, execution->message,
        execution->post_checked, execution->post_passed, execution->post_detail, revision
    );

    char rid[256]={0}, cid[512]={0}, provider[512]={0}, action[256]={0}, caller[256]={0}, target[2048]={0}, result[128]={0}, message[2048]={0}, post_detail[2048]={0};
    json_escape(context->request_id,rid,sizeof(rid)); json_escape(capability->id,cid,sizeof(cid));
    const char *provider_id=rt_provider_for_capability(capability->id);
    json_escape(provider_id?provider_id:"unbound",provider,sizeof(provider));
    json_escape(capability->legacy_action ? capability->legacy_action : capability->id,action,sizeof(action));
    json_escape(context->caller,caller,sizeof(caller)); json_escape(execution->target,target,sizeof(target));
    json_escape(execution->result,result,sizeof(result)); json_escape(execution->message,message,sizeof(message));
    json_escape(execution->post_detail,post_detail,sizeof(post_detail));

    size_t output_cap = execution->output ? strlen(execution->output) * 2 + 64 : 64;
    char *escaped_output = calloc(1, output_cap);
    if(!escaped_output){send_error(fd,500,"receipt allocation failed; request remains indeterminate");return;}
    if (execution->output) json_escape(execution->output, escaped_output, output_cap);
    size_t body_cap = strlen(escaped_output) + 8192;
    char *body = calloc(1, body_cap);
    if(!body){free(escaped_output);send_error(fd,500,"receipt allocation failed; request remains indeterminate");return;}
    snprintf(body,body_cap,
        "{\"ok\":%s,\"executed\":%s,\"replayed\":false,\"revision\":%llu,\"requestId\":\"%s\",\"auditId\":\"%s\",\"capabilityId\":\"%s\",\"providerId\":\"%s\",\"action\":\"%s\",\"risk\":\"%s\",\"caller\":\"%s\",\"target\":\"%s\",\"result\":\"%s\",\"policy\":\"%s\",\"message\":\"%s\",\"postCondition\":{\"checked\":%s,\"passed\":%s,\"detail\":\"%s\"}%s%s%s}",
        execution->ok?"true":"false",execution->executed?"true":"false",revision,rid,audit_id,cid,provider,action,rt_risk_name(capability->risk),caller,target,result,policy,message,
        execution->post_checked?"true":"false",execution->post_passed?"true":"false",post_detail,
        execution->output ? ",\"output\":\"" : "", execution->output ? escaped_output : "", execution->output ? "\"" : "");
    const char *event_kind=!decision.allowed?"rejected":execution->ok?"completed":"failed";
    if(!ledger_complete(context->request_id,body,capability->id,context->caller,event_kind,revision)){
        free(escaped_output); free(body);
        send_error(fd,500,"receipt persistence failed; outcome is indeterminate and automatic replay is blocked");
        return;
    }
    send_response(fd,200,"application/json",body);
    free(escaped_output); free(body);
}

static ssize_t read_request(int fd, char *buf, size_t cap) {
    size_t used=0, needed=0; int have_headers=0;
    while(used+1<cap){
        ssize_t n=recv(fd,buf+used,cap-used-1,0); if(n<=0) break; used+=(size_t)n; buf[used]=0;
        char *end=strstr(buf,"\r\n\r\n");
        if(end && !have_headers){
            have_headers=1; size_t header_len=(size_t)(end+4-buf); needed=header_len;
            char *cl=strstr(buf,"Content-Length:"); if(cl){ long len=strtol(cl+15,NULL,10); if(len<0 || len>MAX_ACTION_BODY) return -2; needed=header_len+(size_t)len; }
        }
        if(have_headers && used>=needed) break;
    }
    return (ssize_t)used;
}

static const char *request_body(char *req) { char *p=strstr(req,"\r\n\r\n"); return p?p+4:""; }

typedef enum {
    RT_AUTH_NONE = 0,
    RT_AUTH_ADMIN = 1,
    RT_AUTH_AGENT = 2,
} RTAuthRole;

static RTAuthRole request_auth_role(const char *req, char *caller, size_t caller_cap) {
    if(caller&&caller_cap)snprintf(caller,caller_cap,"unauthenticated");
    const char *name="X-RootTools-Token:"; size_t name_len=strlen(name);
    const char *line=req;
    while(line && *line){
        const char *end=strstr(line,"\r\n"); if(!end) break; if(end==line) break;
        size_t len=(size_t)(end-line);
        if(len>=name_len && strncasecmp(line,name,name_len)==0){
            const char *value=line+name_len; const char *limit=end;
            while(value<limit && (*value==' '||*value=='\t')) value++;
            while(limit>value && (limit[-1]==' '||limit[-1]=='\t')) limit--;
            size_t value_len=(size_t)(limit-value);
            if(constant_time_token_equal(value,value_len,ADMIN_TOKEN)) {
                if(caller&&caller_cap)snprintf(caller,caller_cap,"roottools-ui");
                return RT_AUTH_ADMIN;
            }
            char agent_token[160]={0};
            if(load_runtime_agent_token(agent_token,sizeof(agent_token),NULL)&&agent_token[0]&&
               constant_time_token_equal(value,value_len,agent_token)) {
                if(caller&&caller_cap)snprintf(caller,caller_cap,"trusted-host-agent");
                return RT_AUTH_AGENT;
            }
            if(value_len>0&&value_len<RT_PRINCIPAL_TOKEN_CAP){
                char presented[RT_PRINCIPAL_TOKEN_CAP]={0};
                memcpy(presented,value,value_len);presented[value_len]=0;
                char principal_id[RT_PRINCIPAL_ID_CAP]={0},kind[RT_PRINCIPAL_KIND_CAP]={0};
                if(rt_principal_authenticate(presented,principal_id,sizeof(principal_id),kind,sizeof(kind))){
                    if(caller&&caller_cap)snprintf(caller,caller_cap,"principal:%s",principal_id);
                    return RT_AUTH_AGENT;
                }
            }
            return RT_AUTH_NONE;
        }
        line=end+2;
    }
    return RT_AUTH_NONE;
}

static const char *auth_role_name(RTAuthRole role) {
    switch(role){
        case RT_AUTH_ADMIN: return "owner";
        case RT_AUTH_AGENT: return "agent";
        case RT_AUTH_NONE: break;
    }
    return "none";
}

static int json_get_string(const char *body, const char *key, char *out, size_t cap) {
    char needle[128]; snprintf(needle,sizeof(needle),"\"%s\"",key); const char *p=strstr(body,needle); if(!p)return 0;
    p+=strlen(needle); while(*p && isspace((unsigned char)*p))p++; if(*p!=':')return 0; p++; while(*p&&isspace((unsigned char)*p))p++; if(*p!='"')return 0; p++;
    size_t j=0; while(*p && *p!='"' && j+1<cap){
        if(*p=='\\'){
            p++; if(!*p)break;
            if(*p=='n')out[j++]='\n'; else if(*p=='r')out[j++]='\r'; else if(*p=='t')out[j++]='\t'; else if(*p=='"'||*p=='\\'||*p=='/')out[j++]=*p; else return 0;
            p++; continue;
        }
        out[j++]=*p++;
    }
    if(*p!='"') return 0; out[j]=0; return 1;
}

static char *json_get_alloc_string(const char *body, const char *key, size_t max_length) {
    char needle[128];
    snprintf(needle,sizeof(needle),"\"%s\"",key);
    const char *p=strstr(body,needle);
    if(!p)return NULL;
    p+=strlen(needle);
    while(*p&&isspace((unsigned char)*p))p++;
    if(*p!=':')return NULL;
    p++;
    while(*p&&isspace((unsigned char)*p))p++;
    if(*p!='"')return NULL;
    p++;
    char *out=calloc(1,max_length+1);
    if(!out)return NULL;
    size_t j=0;
    while(*p&&*p!='"'){
        if(j>=max_length){free(out);return NULL;}
        if(*p=='\\'){
            p++;
            if(!*p){free(out);return NULL;}
            if(*p=='n')out[j++]='\n';
            else if(*p=='r')out[j++]='\r';
            else if(*p=='t')out[j++]='\t';
            else if(*p=='"'||*p=='\\'||*p=='/')out[j++]=*p;
            else {free(out);return NULL;}
            p++;
            continue;
        }
        out[j++]=*p++;
    }
    if(*p!='"'){free(out);return NULL;}
    out[j]=0;
    return out;
}

static int base64_value(unsigned char c) {
    if(c>='A'&&c<='Z')return c-'A';
    if(c>='a'&&c<='z')return c-'a'+26;
    if(c>='0'&&c<='9')return c-'0'+52;
    if(c=='+')return 62;
    if(c=='/')return 63;
    return -1;
}

static unsigned char *base64_decode(const char *input, size_t *out_length) {
    size_t length=input?strlen(input):0;
    if(!length||length%4!=0||length>360000)return NULL;
    size_t cap=(length/4)*3;
    unsigned char *out=malloc(cap?cap:1);
    if(!out)return NULL;
    size_t j=0;
    for(size_t i=0;i<length;i+=4){
        int a=base64_value((unsigned char)input[i]);
        int b=base64_value((unsigned char)input[i+1]);
        int c=input[i+2]=='='?-2:base64_value((unsigned char)input[i+2]);
        int d=input[i+3]=='='?-2:base64_value((unsigned char)input[i+3]);
        if(a<0||b<0||c==-1||d==-1||(c==-2&&d!=-2)||(i+4<length&&(c==-2||d==-2))){free(out);return NULL;}
        unsigned int value=((unsigned int)a<<18)|((unsigned int)b<<12)|((unsigned int)(c<0?0:c)<<6)|(unsigned int)(d<0?0:d);
        out[j++]=(unsigned char)((value>>16)&0xff);
        if(c!=-2)out[j++]=(unsigned char)((value>>8)&0xff);
        if(d!=-2)out[j++]=(unsigned char)(value&0xff);
    }
    *out_length=j;
    return out;
}

static int json_get_int(const char *body, const char *key, long *value) {
    char needle[128]; snprintf(needle,sizeof(needle),"\"%s\"",key); const char *p=strstr(body,needle); if(!p)return 0;
    p+=strlen(needle); while(*p&&isspace((unsigned char)*p))p++; if(*p!=':')return 0; p++; while(*p&&isspace((unsigned char)*p))p++;
    char *end=NULL; long v=strtol(p,&end,10); if(end==p)return 0; *value=v; return 1;
}

static int json_get_bool(const char *body, const char *key, int *value) {
    char needle[128]; snprintf(needle,sizeof(needle),"\"%s\"",key); const char *p=strstr(body,needle); if(!p)return 0;
    p+=strlen(needle); while(*p&&isspace((unsigned char)*p))p++; if(*p!=':')return 0; p++; while(*p&&isspace((unsigned char)*p))p++;
    if(!strncmp(p,"true",4)){*value=1;return 1;}
    if(!strncmp(p,"false",5)){*value=0;return 1;}
    return 0;
}

static int json_has_key(const char *body, const char *key) {
    char needle[128]; snprintf(needle,sizeof(needle),"\"%s\"",key);
    return strstr(body,needle)!=NULL;
}

static void load_action_context(
    const char *body,
    const char *authenticated_caller,
    int trusted_confirmation_source,
    RTActionContext *context
) {
    memset(context,0,sizeof(*context));
    if(!json_get_string(body,"requestId",context->request_id,sizeof(context->request_id)) || !context->request_id[0]) {
        request_counter++;
        snprintf(context->request_id,sizeof(context->request_id),"legacy-%lld-%d-%lu",(long long)time(NULL),getpid(),request_counter);
    }
    snprintf(context->caller,sizeof(context->caller),"%s",authenticated_caller?authenticated_caller:"authenticated-client");
    int requested_confirmation=0;
    if(!json_get_bool(body,"confirmed",&requested_confirmation)) requested_confirmation=0;
    // A remote Agent cannot self-assert the owner's approval. In v0.3 only
    // the on-device Admin/UI credential may turn an explicit UI confirmation
    // into a daemon-side R2 confirmation. Future unattended approval must use
    // a separate signed/one-shot grant rather than this boolean.
    context->confirmed=trusted_confirmation_source&&(requested_confirmation||rt_policy_developer_mode_enabled());
}

static int safe_bundle_id(const char *value) {
    size_t n=strlen(value); if(n<3||n>255)return 0;
    for(size_t i=0;i<n;i++) if(!(isalnum((unsigned char)value[i])||value[i]=='.'||value[i]=='-'||value[i]=='_')) return 0;
    return 1;
}

static int safe_file_name(const char *value) {
    size_t n=strlen(value); if(n<1||n>96||value[0]=='.')return 0;
    for(size_t i=0;i<n;i++) if(!(isalnum((unsigned char)value[i])||value[i]=='.'||value[i]=='-'||value[i]=='_')) return 0;
    return strstr(value,"..") == NULL;
}

static int safe_request_id(const char *value) {
    size_t n=strlen(value); if(n<1||n>120)return 0;
    for(size_t i=0;i<n;i++) if(!(isalnum((unsigned char)value[i])||value[i]=='.'||value[i]=='-'||value[i]=='_')) return 0;
    return 1;
}

static int fixed_spawn_wait(const char *program, char *const argv[], char *output, size_t output_cap) {
    int pipefd[2]={-1,-1}; posix_spawn_file_actions_t actions; posix_spawn_file_actions_init(&actions);
    if(output){ if(pipe(pipefd)!=0){posix_spawn_file_actions_destroy(&actions);return errno;} posix_spawn_file_actions_adddup2(&actions,pipefd[1],STDOUT_FILENO); posix_spawn_file_actions_adddup2(&actions,pipefd[1],STDERR_FILENO); posix_spawn_file_actions_addclose(&actions,pipefd[0]); }
    pid_t pid=0; int rc=posix_spawn(&pid,program,&actions,NULL,argv,environ); posix_spawn_file_actions_destroy(&actions);
    if(output){ close(pipefd[1]); if(rc==0){size_t used=0; while(used+1<output_cap){ssize_t n=read(pipefd[0],output+used,output_cap-used-1);if(n<=0)break;used+=(size_t)n;} output[used]=0;} close(pipefd[0]); }
    if(rc!=0)return rc; int status=0; if(waitpid(pid,&status,0)<0)return errno; if(WIFEXITED(status))return WEXITSTATUS(status); return 128;
}

static int executable_for_bundle(const char *bundle_id, char *out, size_t cap) {
    char info[8192]={0}; char *argv[]={(char*)"uicache",(char*)"-i",(char*)bundle_id,NULL};
    int rc=fixed_spawn_wait("/var/jb/usr/bin/uicache",argv,info,sizeof(info)); if(rc!=0)return 0;
    const char *p=strstr(info,"Executable Name: "); if(!p)return 0; p+=17; const char *e=strchr(p,'\n'); size_t n=e?(size_t)(e-p):strlen(p); if(n==0||n>=cap)return 0;
    memcpy(out,p,n); out[n]=0; return strchr(out,'/')==NULL && strchr(out,'\r')==NULL;
}

static int build_file_path(const char *scope, const char *name, char *out, size_t cap) {
    if(!safe_file_name(name))return 0; ensure_action_dirs();
    if(!strcmp(scope,"mobile")){snprintf(out,cap,"%s/%s",mobile_scope_root(),name);return 1;}
    if(!strcmp(scope,"bootstrap")){snprintf(out,cap,"%s/%s",bootstrap_scope_root(),name);return 1;}
    return 0;
}

static int critical_process_name(const char *name) {
    const char *deny[]={"launchd","kernel_task","SpringBoard","backboardd","roottools-execd",NULL};
    for(int i=0;deny[i];i++) if(!strcmp(name,deny[i]))return 1; return 0;
}

static int process_name_exists(const char *wanted);

static int cfstring_copy_utf8(CFStringRef value, char *out, size_t cap) {
    if(!value||!out||cap<2)return 0;
    return CFStringGetCString(value,out,(CFIndex)cap,kCFStringEncodingUTF8);
}

static int app_metadata(
    const char *path,
    char *bundle_id,
    size_t bundle_cap,
    char *executable,
    size_t executable_cap,
    char *display_name,
    size_t display_cap,
    char *version,
    size_t version_cap,
    char *build,
    size_t build_cap
) {
    CFStringRef path_string=CFStringCreateWithCString(kCFAllocatorDefault,path,kCFStringEncodingUTF8);
    if(!path_string)return 0;
    CFURLRef url=CFURLCreateWithFileSystemPath(kCFAllocatorDefault,path_string,kCFURLPOSIXPathStyle,true);
    CFRelease(path_string); if(!url)return 0;
    CFBundleRef bundle=CFBundleCreate(kCFAllocatorDefault,url); CFRelease(url); if(!bundle)return 0;
    CFStringRef identifier=CFBundleGetIdentifier(bundle);
    CFTypeRef executable_value=CFBundleGetValueForInfoDictionaryKey(bundle,kCFBundleExecutableKey);
    CFTypeRef display_value=CFBundleGetValueForInfoDictionaryKey(bundle,CFSTR("CFBundleDisplayName"));
    CFTypeRef version_value=CFBundleGetValueForInfoDictionaryKey(bundle,CFSTR("CFBundleShortVersionString"));
    CFTypeRef build_value=CFBundleGetValueForInfoDictionaryKey(bundle,kCFBundleVersionKey);
    if(!display_value)display_value=CFBundleGetValueForInfoDictionaryKey(bundle,kCFBundleNameKey);
    int ok=identifier&&CFGetTypeID(identifier)==CFStringGetTypeID()&&
        executable_value&&CFGetTypeID(executable_value)==CFStringGetTypeID()&&
        cfstring_copy_utf8(identifier,bundle_id,bundle_cap)&&
        cfstring_copy_utf8((CFStringRef)executable_value,executable,executable_cap);
    if(ok){
        if(!display_value||CFGetTypeID(display_value)!=CFStringGetTypeID()||
           !cfstring_copy_utf8((CFStringRef)display_value,display_name,display_cap)){
            snprintf(display_name,display_cap,"%s",bundle_id);
        }
        if(version&&version_cap){
            if(!version_value||CFGetTypeID(version_value)!=CFStringGetTypeID()||
               !cfstring_copy_utf8((CFStringRef)version_value,version,version_cap))snprintf(version,version_cap,"unknown");
        }
        if(build&&build_cap){
            if(!build_value||CFGetTypeID(build_value)!=CFStringGetTypeID()||
               !cfstring_copy_utf8((CFStringRef)build_value,build,build_cap))snprintf(build,build_cap,"unknown");
        }
    }
    CFRelease(bundle); return ok;
}

static int append_app_catalog_entry(
    char *response,
    size_t cap,
    size_t *used,
    int row,
    const char *path,
    const char *source
) {
    char bundle_id[256]={0},executable[256]={0},display[512]={0},version[128]={0},build[128]={0};
    if(!app_metadata(path,bundle_id,sizeof(bundle_id),executable,sizeof(executable),display,sizeof(display),version,sizeof(version),build,sizeof(build)))return 0;
    char ebundle[512]={0},eexec[512]={0},edisplay[1024]={0},epath[2048]={0},eversion[256]={0},ebuild[256]={0};
    json_escape(bundle_id,ebundle,sizeof(ebundle)); json_escape(executable,eexec,sizeof(eexec));
    json_escape(display,edisplay,sizeof(edisplay)); json_escape(path,epath,sizeof(epath)); json_escape(version,eversion,sizeof(eversion));json_escape(build,ebuild,sizeof(ebuild));
    int n=snprintf(response+*used,cap-*used,
        "%s{\"bundleID\":\"%s\",\"executable\":\"%s\",\"displayName\":\"%s\",\"version\":\"%s\",\"build\":\"%s\",\"source\":\"%s\",\"path\":\"%s\",\"running\":%s,\"critical\":%s}",
        row?",":"",ebundle,eexec,edisplay,eversion,ebuild,source,epath,process_name_exists(executable)?"true":"false",critical_process_name(executable)?"true":"false");
    if(n<0||(size_t)n>=cap-*used)return -1; *used+=(size_t)n; return 1;
}

static void scan_app_catalog_root(
    char *response,
    size_t cap,
    size_t *used,
    int *row,
    const char *root,
    const char *source,
    int nested
) {
    DIR *dir=opendir(root); if(!dir)return; struct dirent *entry;
    while((entry=readdir(dir))&&*row<256){
        if(entry->d_name[0]=='.')continue;
        char path[1024]={0}; snprintf(path,sizeof(path),"%s/%s",root,entry->d_name);
        if(has_suffix(entry->d_name,".app")){
            int appended=append_app_catalog_entry(response,cap,used,*row,path,source);
            if(appended<0)break; if(appended>0)(*row)++;
        } else if(nested) {
            DIR *sub=opendir(path); if(!sub)continue; struct dirent *child;
            while((child=readdir(sub))&&*row<256){
                if(child->d_name[0]=='.'||!has_suffix(child->d_name,".app"))continue;
                char child_path[1200]={0}; snprintf(child_path,sizeof(child_path),"%s/%s",path,child->d_name);
                int appended=append_app_catalog_entry(response,cap,used,*row,child_path,source);
                if(appended<0){closedir(sub);closedir(dir);return;} if(appended>0)(*row)++;
            }
            closedir(sub);
        }
        if(*used>61000)break;
    }
    closedir(dir);
}

static int resolve_app_bundle_in_root(
    const char *wanted_bundle,
    const char *root,
    const char *source,
    int nested,
    char *path_out,
    size_t path_cap,
    char *source_out,
    size_t source_cap,
    char *executable_out,
    size_t executable_cap,
    char *display_out,
    size_t display_cap,
    char *version_out,
    size_t version_cap,
    char *build_out,
    size_t build_cap
) {
    DIR *dir=opendir(root); if(!dir)return 0;
    struct dirent *entry;
    while((entry=readdir(dir))){
        if(entry->d_name[0]=='.')continue;
        char parent[1024]={0};snprintf(parent,sizeof(parent),"%s/%s",root,entry->d_name);
        if(has_suffix(entry->d_name,".app")){
            char bundle[256]={0};
            if(app_metadata(parent,bundle,sizeof(bundle),executable_out,executable_cap,display_out,display_cap,version_out,version_cap,build_out,build_cap)&&!strcmp(bundle,wanted_bundle)){
                snprintf(path_out,path_cap,"%s",parent);snprintf(source_out,source_cap,"%s",source);closedir(dir);return 1;
            }
        } else if(nested) {
            DIR *sub=opendir(parent);if(!sub)continue;struct dirent *child;
            while((child=readdir(sub))){
                if(child->d_name[0]=='.'||!has_suffix(child->d_name,".app"))continue;
                char child_path[1200]={0};snprintf(child_path,sizeof(child_path),"%s/%s",parent,child->d_name);
                char bundle[256]={0};
                if(app_metadata(child_path,bundle,sizeof(bundle),executable_out,executable_cap,display_out,display_cap,version_out,version_cap,build_out,build_cap)&&!strcmp(bundle,wanted_bundle)){
                    snprintf(path_out,path_cap,"%s",child_path);snprintf(source_out,source_cap,"%s",source);closedir(sub);closedir(dir);return 1;
                }
            }
            closedir(sub);
        }
    }
    closedir(dir);return 0;
}

static int resolve_app_bundle(
    const char *bundle,
    char *path,size_t path_cap,
    char *source,size_t source_cap,
    char *executable,size_t executable_cap,
    char *display,size_t display_cap,
    char *version,size_t version_cap,
    char *build,size_t build_cap
) {
    return resolve_app_bundle_in_root(bundle,system_apps_root(),"system",0,path,path_cap,source,source_cap,executable,executable_cap,display,display_cap,version,version_cap,build,build_cap)||
        resolve_app_bundle_in_root(bundle,jailbreak_apps_root(),"jailbreak",0,path,path_cap,source,source_cap,executable,executable_cap,display,display_cap,version,version_cap,build,build_cap)||
        resolve_app_bundle_in_root(bundle,user_apps_root(),"user",1,path,path_cap,source,source_cap,executable,executable_cap,display,display_cap,version,version_cap,build,build_cap);
}

static void send_app_catalog(int fd) {
    char *response=calloc(1,65536); if(!response){send_error(fd,500,"app catalog allocation failed");return;}
    size_t used=0; int row=0; int n=snprintf(response,65536,"{\"schemaVersion\":1,\"generation\":%d,\"applications\":[",getpid());
    if(n<0){free(response);send_error(fd,500,"app catalog encoding failed");return;} used=(size_t)n;
    scan_app_catalog_root(response,65536,&used,&row,system_apps_root(),"system",0);
    scan_app_catalog_root(response,65536,&used,&row,jailbreak_apps_root(),"jailbreak",0);
    scan_app_catalog_root(response,65536,&used,&row,user_apps_root(),"user",1);
    n=snprintf(response+used,65536-used,"],\"count\":%d}",row);
    if(n<0||(size_t)n>=65536-used){free(response);send_error(fd,500,"app catalog too large");return;}
    send_response(fd,200,"application/json",response); free(response);
}

static void send_process_inspect(int fd, const char *body) {
    long raw=0;
    if(!json_get_int(body,"pid",&raw)||raw<=0||raw>999999){send_error(fd,400,"valid pid is required");return;}
    uid_t uid=0; char name[128]={0};
    if(!process_info((pid_t)raw,&uid,name,sizeof(name))){send_error(fd,404,"process not found");return;}
    RTProcessMetrics metrics=process_metrics((pid_t)raw);
    char escaped[256]={0}, response[3072]={0}; json_escape(name,escaped,sizeof(escaped));
    snprintf(response,sizeof(response),
        "{\"ok\":true,\"process\":{\"pid\":%ld,\"uid\":%u,\"command\":\"%s\",\"critical\":%s,\"privileged\":%s,"
        "\"metricsAvailable\":%s,\"metrics\":{\"userTimeNs\":%llu,\"systemTimeNs\":%llu,\"residentBytes\":%llu,\"footprintBytes\":%llu,"
        "\"diskReadBytes\":%llu,\"diskWriteBytes\":%llu,\"pageins\":%llu,\"idleWakeups\":%llu,\"interruptWakeups\":%llu}}}",
        raw,uid,escaped,critical_process_name(name)?"true":"false",uid==0?"true":"false",metrics.available?"true":"false",
        metrics.user_time_ns,metrics.system_time_ns,metrics.resident_bytes,metrics.footprint_bytes,
        metrics.disk_read_bytes,metrics.disk_write_bytes,metrics.pageins,metrics.idle_wakeups,metrics.interrupt_wakeups);
    send_response(fd,200,"application/json",response);
}

static void send_process_catalog(int fd) {
    size_t length=0; int mib[4]={CTL_KERN,KERN_PROC,KERN_PROC_ALL,0};
    if(sysctl(mib,4,NULL,&length,NULL,0)!=0||!length){send_error(fd,503,"process query unavailable");return;}
    struct kinfo_proc *items=malloc(length); if(!items){send_error(fd,500,"process allocation failed");return;}
    if(sysctl(mib,4,items,&length,NULL,0)!=0){free(items);send_error(fd,503,"process query failed");return;}
    size_t count=length/sizeof(struct kinfo_proc);
    char *response=calloc(1,65536); if(!response){free(items);send_error(fd,500,"process response allocation failed");return;}
    size_t used=0; int row=0; int n=snprintf(response,65536,"{\"schemaVersion\":1,\"generation\":%d,\"processes\":[",getpid());
    if(n<0){free(response);free(items);send_error(fd,500,"process encoding failed");return;} used=(size_t)n;
    for(size_t i=0;i<count&&row<512;i++){
        pid_t pid=items[i].kp_proc.p_pid; uid_t uid=items[i].kp_eproc.e_ucred.cr_uid;
        const char *name=items[i].kp_proc.p_comm; char escaped[256]={0}; json_escape(name,escaped,sizeof(escaped));
        n=snprintf(response+used,65536-used,
            "%s{\"pid\":%d,\"uid\":%u,\"command\":\"%s\",\"critical\":%s,\"privileged\":%s}",
            row?",":"",pid,uid,escaped,critical_process_name(name)?"true":"false",uid==0?"true":"false");
        if(n<0||(size_t)n>=65536-used)break; used+=(size_t)n; row++;
        if(used>61000)break;
    }
    free(items);
    n=snprintf(response+used,65536-used,"],\"count\":%d}",row);
    if(n<0||(size_t)n>=65536-used){free(response);send_error(fd,500,"process response too large");return;}
    send_response(fd,200,"application/json",response); free(response);
}

static void send_app_inspect(int fd, const char *body) {
    char bundle[256]={0}, executable[256]={0},path[1200]={0},source[64]={0},display[512]={0},version[128]={0},build[128]={0};
    if(!json_get_string(body,"bundleID",bundle,sizeof(bundle))||!safe_bundle_id(bundle)){send_error(fd,400,"valid bundleID is required");return;}
    if(!resolve_app_bundle(bundle,path,sizeof(path),source,sizeof(source),executable,sizeof(executable),display,sizeof(display),version,sizeof(version),build,sizeof(build))){
        if(!executable_for_bundle(bundle,executable,sizeof(executable))){send_error(fd,404,"application could not be resolved");return;}
        snprintf(display,sizeof(display),"%s",bundle);snprintf(version,sizeof(version),"unknown");snprintf(build,sizeof(build),"unknown");snprintf(source,sizeof(source),"registry");
    }
    char escaped_executable[512]={0},escaped_display[1024]={0},escaped_version[256]={0},escaped_build[256]={0},escaped_path[2400]={0};
    json_escape(executable,escaped_executable,sizeof(escaped_executable));json_escape(display,escaped_display,sizeof(escaped_display));
    json_escape(version,escaped_version,sizeof(escaped_version));json_escape(build,escaped_build,sizeof(escaped_build));json_escape(path,escaped_path,sizeof(escaped_path));
    char response[5200]={0};
    snprintf(response,sizeof(response),
        "{\"ok\":true,\"application\":{\"bundleID\":\"%s\",\"executable\":\"%s\",\"displayName\":\"%s\",\"version\":\"%s\",\"build\":\"%s\","
        "\"source\":\"%s\",\"bundlePath\":%s%s%s,\"running\":%s,\"critical\":%s}}",
        bundle,escaped_executable,escaped_display,escaped_version,escaped_build,source,
        path[0]?"\"":"null",path[0]?escaped_path:"",path[0]?"\"":"",
        process_name_exists(executable)?"true":"false",critical_process_name(executable)?"true":"false");
    send_response(fd,200,"application/json",response);
}

static void send_fs_scopes(int fd) {
    char mobile[1024]={0},bootstrap[1024]={0},response[4096]={0};
    json_escape(mobile_scope_root(),mobile,sizeof(mobile)); json_escape(bootstrap_scope_root(),bootstrap,sizeof(bootstrap));
    snprintf(response,sizeof(response),
        "{\"schemaVersion\":1,\"scopes\":["
        "{\"id\":\"mobile\",\"root\":\"%s\",\"read\":true,\"write\":true,\"maxReadBytes\":32768,\"maxWriteBytes\":16384},"
        "{\"id\":\"bootstrap\",\"root\":\"%s\",\"read\":true,\"write\":true,\"maxReadBytes\":32768,\"maxWriteBytes\":16384}"
        "]}",mobile,bootstrap);
    send_response(fd,200,"application/json",response);
}

static const char *file_scope_root(const char *scope) {
    if(!strcmp(scope,"mobile"))return mobile_scope_root();
    if(!strcmp(scope,"bootstrap"))return bootstrap_scope_root();
    return NULL;
}

static void send_fs_list(int fd, const char *body) {
    char scope[32]={0};
    if(!json_get_string(body,"scope",scope,sizeof(scope))){send_error(fd,400,"scope is required");return;}
    const char *root=file_scope_root(scope); if(!root){send_error(fd,400,"unknown file scope");return;}
    ensure_action_dirs(); DIR *dir=opendir(root); if(!dir){send_error(fd,503,"file scope unavailable");return;}
    char *response=calloc(1,49152); if(!response){closedir(dir);send_error(fd,500,"file list allocation failed");return;}
    size_t used=0; int row=0; int n=snprintf(response,49152,"{\"schemaVersion\":1,\"scope\":\"%s\",\"entries\":[",scope);
    if(n<0){free(response);closedir(dir);send_error(fd,500,"file list encoding failed");return;} used=(size_t)n;
    struct dirent *entry;
    while((entry=readdir(dir))&&row<256){
        if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,".."))continue;
        if(!safe_file_name(entry->d_name))continue;
        char path[1024]={0}; snprintf(path,sizeof(path),"%s/%s",root,entry->d_name);
        struct stat st; if(lstat(path,&st)!=0)continue;
        char escaped[256]={0}; json_escape(entry->d_name,escaped,sizeof(escaped));
        const char *kind=S_ISDIR(st.st_mode)?"directory":S_ISLNK(st.st_mode)?"symlink":"file";
        n=snprintf(response+used,49152-used,
            "%s{\"name\":\"%s\",\"kind\":\"%s\",\"size\":%llu,\"mode\":%u}",
            row?",":"",escaped,kind,(unsigned long long)st.st_size,(unsigned int)(st.st_mode&07777));
        if(n<0||(size_t)n>=49152-used)break; used+=(size_t)n; row++;
        if(used>46000)break;
    }
    closedir(dir);
    n=snprintf(response+used,49152-used,"],\"count\":%d}",row);
    if(n<0||(size_t)n>=49152-used){free(response);send_error(fd,500,"file list too large");return;}
    send_response(fd,200,"application/json",response); free(response);
}

static void send_network_catalog(int fd) {
    char *response=calloc(1,49152); if(!response){send_error(fd,500,"network response allocation failed");return;}
    size_t used=0; int row=0; int n=snprintf(response,49152,"{\"schemaVersion\":1,\"interfaces\":[");
    if(n<0){free(response);send_error(fd,500,"network encoding failed");return;} used=(size_t)n;
    struct ifaddrs *ifs=NULL;
    if(getifaddrs(&ifs)==0){
        for(struct ifaddrs *it=ifs;it&&row<256;it=it->ifa_next){
            if(!it->ifa_addr)continue; int family=it->ifa_addr->sa_family; char address[INET6_ADDRSTRLEN]={0};
            const char *family_name=NULL;
            if(family==AF_INET){inet_ntop(AF_INET,&((struct sockaddr_in*)it->ifa_addr)->sin_addr,address,sizeof(address));family_name="ipv4";}
            else if(family==AF_INET6){inet_ntop(AF_INET6,&((struct sockaddr_in6*)it->ifa_addr)->sin6_addr,address,sizeof(address));family_name="ipv6";}
            else continue;
            char ename[128]={0},eaddress[256]={0}; json_escape(it->ifa_name,ename,sizeof(ename)); json_escape(address,eaddress,sizeof(eaddress));
            n=snprintf(response+used,49152-used,"%s{\"name\":\"%s\",\"family\":\"%s\",\"address\":\"%s\"}",row?",":"",ename,family_name,eaddress);
            if(n<0||(size_t)n>=49152-used)break; used+=(size_t)n; row++;
            if(used>45000)break;
        }
        freeifaddrs(ifs);
    }
    n=snprintf(response+used,49152-used,
        "],\"count\":%d,\"localAdapters\":{\"ssh\":%s,\"frida\":%s,\"zxtouch\":%s}}",
        row,port_open(22)?"true":"false",port_open(27042)?"true":"false",port_open(6000)?"true":"false");
    if(n<0||(size_t)n>=49152-used){free(response);send_error(fd,500,"network response too large");return;}
    send_response(fd,200,"application/json",response); free(response);
}

static void send_performance(int fd) {
    double load[3]={0,0,0};
    int load_count=getloadavg(load,3);
    if(load_count<0){load[0]=load[1]=load[2]=0;load_count=0;}
    RTMemorySnapshot memory=memory_snapshot();
    int providers_ready=0;
    size_t provider_total=rt_provider_count();
    for(size_t i=0;i<provider_total;i++){
        const RTProvider *provider=rt_provider_at(i);
        if(provider&&rt_provider_available(provider))providers_ready++;
    }
    int process_count=process_count_snapshot();
    int active_tasks=task_active_count();
    char response[4096]={0};
    snprintf(response,sizeof(response),
        "{\"schemaVersion\":1,\"uptimeSeconds\":%llu,\"cpuCount\":%d,"
        "\"loadAverage\":{\"available\":%s,\"oneMinute\":%.3f,\"fiveMinute\":%.3f,\"fifteenMinute\":%.3f},"
        "\"memory\":{\"available\":%s,\"totalBytes\":%llu,\"freeBytes\":%llu,\"activeBytes\":%llu,\"inactiveBytes\":%llu,\"wiredBytes\":%llu},"
        "\"storage\":{\"rootFreeBytes\":%llu,\"varFreeBytes\":%llu},"
        "\"daemon\":{\"pid\":%d,\"residentBytes\":%llu},"
        "\"processCount\":%d,\"activeTaskCount\":%d,"
        "\"providers\":{\"ready\":%d,\"total\":%zu}}",
        uptime_seconds(),sysctl_int("hw.ncpu"),
        load_count>0?"true":"false",load[0],load[1],load[2],
        memory.available?"true":"false",sysctl_u64("hw.memsize"),memory.free_bytes,memory.active_bytes,memory.inactive_bytes,memory.wired_bytes,
        free_bytes("/"),free_bytes("/var"),
        getpid(),daemon_resident_bytes(),
        process_count<0?0:process_count,active_tasks<0?0:active_tasks,
        providers_ready,provider_total);
    send_response(fd,200,"application/json",response);
}

static void send_runtime_catalog(int fd) {
    int dopamine=0; char *process_snapshot=processes_text(&dopamine); free(process_snapshot);
    int rootless=access("/var/jb",F_OK)==0;
    int ssh=port_open(22),frida=port_open(27042),zxtouch=port_open(6000);
    unsigned long long revision=0; int revision_available=ledger_current_revision(&revision);
    char response[8192]={0};
    snprintf(response,sizeof(response),
        "{\"schemaVersion\":1,\"generation\":%d,\"revision\":%llu,\"revisionAvailable\":%s,\"privilegeState\":\"%s\",\"dopamineAppProcessRunning\":%s,\"authoritativeProviderCatalog\":\"/v1/providers/catalog\",\"adapters\":["
        "{\"id\":\"roottools.execd\",\"state\":\"available\",\"implementation\":\"ios.jailbreak-daemon\",\"privilege\":\"uid0\",\"requiresUnlock\":false,\"supportsHeadless\":true,\"survivesAppExit\":true},"
        "{\"id\":\"ios.dopamine.rootless\",\"state\":\"%s\",\"implementation\":\"dopamine.rootless\",\"privilege\":\"jailbreak\",\"requiresUnlock\":false,\"supportsHeadless\":true,\"survivesAppExit\":true},"
        "{\"id\":\"ios.openssh\",\"state\":\"%s\",\"implementation\":\"openssh.loopback-device\",\"privilege\":\"root-service\",\"requiresUnlock\":false,\"supportsHeadless\":true,\"survivesAppExit\":true},"
        "{\"id\":\"ios.frida\",\"state\":\"%s\",\"implementation\":\"frida-server\",\"privilege\":\"runtime-inspection\",\"requiresUnlock\":false,\"supportsHeadless\":true,\"survivesAppExit\":true},"
        "{\"id\":\"ios.zxtouch\",\"state\":\"%s\",\"implementation\":\"zxtouch.loopback\",\"privilege\":\"ui-input\",\"requiresUnlock\":true,\"supportsHeadless\":true,\"survivesAppExit\":true}"
        "]}",
        getpid(),revision,revision_available?"true":"false",rootless&&getuid()==0?"jailbreak-root":"degraded",dopamine?"true":"false",
        rootless?"available":"unavailable",ssh?"available":"unavailable",frida?"available":"unavailable",zxtouch?"available":"unavailable");
    send_response(fd,200,"application/json",response);
}

static void send_frida_status(int fd) {
    char *response=rt_frida_status_json();
    if(!response){send_error(fd,503,"Frida runtime observation unavailable");return;}
    send_response(fd,200,"application/json",response);free(response);
}

static void send_ellekit_status(int fd) {
    char *response=rt_ellekit_status_json();
    if(!response){send_error(fd,503,"ElleKit runtime observation unavailable");return;}
    send_response(fd,200,"application/json",response);free(response);
}

static void send_provider_catalog(int fd) {
    char *response=rt_providers_json();
    if(!response){send_error(fd,500,"provider catalog unavailable");return;}
    send_response(fd,200,"application/json",response);
    free(response);
}

static void send_package_plan(int fd, const char *body) {
    char format[32]={0};
    if(!json_get_string(body,"format",format,sizeof(format))){send_error(fd,400,"format is required");return;}
    char *response=rt_package_plan_json(format);
    if(!response){send_error(fd,400,"unsupported package format");return;}
    send_response(fd,200,"application/json",response);
    free(response);
}

static void send_package_catalog(int fd) {
    char *response=rt_packages_json();
    if(!response){send_error(fd,503,"package staging catalog unavailable");return;}
    send_response(fd,200,"application/json",response);
    free(response);
}

static void send_package_history(int fd) {
    char *response=rt_package_history_json();
    if(!response){send_error(fd,503,"package history unavailable");return;}
    send_response(fd,200,"application/json",response);
    free(response);
}

static void send_principal_catalog(int fd) {
    char *response=rt_principals_json();
    if(!response){send_error(fd,503,"principal catalog unavailable");return;}
    send_response(fd,200,"application/json",response);
    free(response);
}

static void send_principal_grants(int fd, const char *body) {
    char principal_id[RT_PRINCIPAL_ID_CAP]={0};
    if(!json_get_string(body,"principalId",principal_id,sizeof(principal_id))){
        send_error(fd,400,"principalId is required");return;
    }
    char *response=rt_principal_grants_json(principal_id);
    if(!response){send_error(fd,404,"principal grants unavailable");return;}
    send_response(fd,200,"application/json",response);
    free(response);
}

static void send_self_update_status(int fd) {
    char *response=rt_updates_json();
    if(!response){send_error(fd,503,"self-update status unavailable");return;}
    send_response(fd,200,"application/json",response);
    free(response);
}

static void send_lock_state(int fd) {
    RTLockSnapshot snapshot=device_lock_snapshot();
    char lock_source[256]={0},screen_source[256]={0};
    json_escape(snapshot.lock_source,lock_source,sizeof(lock_source));
    json_escape(snapshot.screen_source,screen_source,sizeof(screen_source));
    char response[2048]={0};
    snprintf(response,sizeof(response),
        "{\"schemaVersion\":1,\"lockState\":\"%s\",\"locked\":%s,\"lockRawState\":%llu,\"lockSource\":\"%s\","
        "\"screenState\":\"%s\",\"screenBlanked\":%s,\"screenRawState\":%llu,\"screenSource\":\"%s\","
        "\"headlessExecutionReady\":%s,\"uiExecutionReady\":%s}",
        lock_state_name(snapshot),snapshot.lock_known?(snapshot.locked?"true":"false"):"null",
        (unsigned long long)snapshot.lock_raw,lock_source,
        screen_state_name(snapshot),snapshot.screen_blank_known?(snapshot.screen_blanked?"true":"false"):"null",
        (unsigned long long)snapshot.screen_blank_raw,screen_source,
        (getuid()==0&&access("/var/jb",F_OK)==0)?"true":"false",ui_execution_ready(snapshot)?"true":"false");
    send_response(fd,200,"application/json",response);
}

static void send_automation_state(int fd) {
    RTLockSnapshot snapshot=device_lock_snapshot();
    int pending=task_active_count();
    int completed=task_count_state("completed");
    int failed=task_count_state("failed");
    int headless=getuid()==0&&access("/var/jb",F_OK)==0;
    char response[3072]={0};
    snprintf(response,sizeof(response),
        "{\"schemaVersion\":1,\"mode\":\"lock-aware\",\"lockState\":\"%s\",\"screenState\":\"%s\","
        "\"headlessExecutionReady\":%s,\"uiExecutionReady\":%s,\"interactiveInputReady\":%s,"
        "\"adapters\":{\"ssh\":%s,\"frida\":%s,\"zxtouch\":%s},"
        "\"queue\":{\"pending\":%d,\"completed\":%d,\"failed\":%d},"
        "\"policy\":{\"bypassDevicePasscode\":false,\"uiJobsWaitForUnlock\":true,\"headlessJobsMayRunLocked\":true}}",
        lock_state_name(snapshot),screen_state_name(snapshot),headless?"true":"false",ui_execution_ready(snapshot)?"true":"false",
        (ui_execution_ready(snapshot)&&port_open(6000))?"true":"false",
        port_open(22)?"true":"false",port_open(27042)?"true":"false",port_open(6000)?"true":"false",
        pending<0?0:pending,completed<0?0:completed,failed<0?0:failed);
    send_response(fd,200,"application/json",response);
}

static void send_automation_queue(int fd) {
    sqlite3 *db=NULL; if(!ledger_open(&db)){send_error(fd,503,"automation queue unavailable");return;}
    sqlite3_stmt *statement=NULL;
    int rc=sqlite3_prepare_v2(db,
        "SELECT task_id,kind,target,state,attempt_count,created_at,updated_at,result,error "
        "FROM device_tasks ORDER BY created_at DESC LIMIT 100",-1,&statement,NULL);
    if(rc!=SQLITE_OK){sqlite3_close(db);send_error(fd,500,"automation queue query failed");return;}
    char *response=calloc(1,49152); if(!response){sqlite3_finalize(statement);sqlite3_close(db);send_error(fd,500,"automation queue allocation failed");return;}
    size_t used=0; int row=0; int n=snprintf(response,49152,"{\"schemaVersion\":1,\"jobs\":[");
    if(n<0){free(response);sqlite3_finalize(statement);sqlite3_close(db);send_error(fd,500,"automation queue encoding failed");return;} used=(size_t)n;
    while((rc=sqlite3_step(statement))==SQLITE_ROW){
        const char *job=(const char*)sqlite3_column_text(statement,0),*kind=(const char*)sqlite3_column_text(statement,1);
        const char *target=(const char*)sqlite3_column_text(statement,2),*state=(const char*)sqlite3_column_text(statement,3);
        int attempts=sqlite3_column_int(statement,4); sqlite3_int64 created=sqlite3_column_int64(statement,5),updated=sqlite3_column_int64(statement,6);
        const char *result=(const char*)sqlite3_column_text(statement,7),*error=(const char*)sqlite3_column_text(statement,8);
        char ejob[256]={0},ekind[128]={0},etarget[512]={0},estate[128]={0},eresult[1024]={0},eerror[1024]={0};
        json_escape(job?job:"",ejob,sizeof(ejob));json_escape(kind?kind:"",ekind,sizeof(ekind));json_escape(target?target:"",etarget,sizeof(etarget));
        json_escape(state?state:"",estate,sizeof(estate));json_escape(result?result:"",eresult,sizeof(eresult));json_escape(error?error:"",eerror,sizeof(eerror));
        n=snprintf(response+used,49152-used,
            "%s{\"jobId\":\"%s\",\"kind\":\"%s\",\"target\":\"%s\",\"state\":\"%s\",\"attemptCount\":%d,\"createdAt\":%lld,\"updatedAt\":%lld,\"result\":%s%s%s,\"error\":%s%s%s}",
            row?",":"",ejob,ekind,etarget,estate,attempts,(long long)created,(long long)updated,
            result?"\"":"null",result?eresult:"",result?"\"":"",error?"\"":"null",error?eerror:"",error?"\"":"");
        if(n<0||(size_t)n>=49152-used)break; used+=(size_t)n; row++;
    }
    sqlite3_finalize(statement);sqlite3_close(db);
    n=snprintf(response+used,49152-used,"],\"count\":%d}",row);
    if(n<0||(size_t)n>=49152-used){free(response);send_error(fd,500,"automation queue response too large");return;}
    send_response(fd,200,"application/json",response);free(response);
}

static void send_task_catalog(int fd, const char *caller, int owner) {
    sqlite3 *db=NULL;if(!ledger_open(&db)){send_error(fd,503,"task catalog unavailable");return;}
    sqlite3_stmt *statement=NULL;
    const char *sql=owner?
        "SELECT task_id,capability_id,kind,target,caller,state,requires_ui,attempt_count,created_at,updated_at,result,error "
        "FROM device_tasks ORDER BY created_at DESC LIMIT 200":
        "SELECT task_id,capability_id,kind,target,caller,state,requires_ui,attempt_count,created_at,updated_at,result,error "
        "FROM device_tasks WHERE caller=?1 ORDER BY created_at DESC LIMIT 200";
    int rc=sqlite3_prepare_v2(db,sql,-1,&statement,NULL);
    if(rc==SQLITE_OK&&!owner)sqlite3_bind_text(statement,1,caller,-1,SQLITE_TRANSIENT);
    if(rc!=SQLITE_OK){sqlite3_close(db);send_error(fd,500,"task catalog query failed");return;}
    char *response=calloc(1,98304);
    if(!response){sqlite3_finalize(statement);sqlite3_close(db);send_error(fd,500,"task catalog allocation failed");return;}
    size_t used=0;int row=0;int n=snprintf(response,98304,"{\"schemaVersion\":1,\"tasks\":[");
    if(n<0){free(response);sqlite3_finalize(statement);sqlite3_close(db);send_error(fd,500,"task catalog encoding failed");return;}used=(size_t)n;
    while((rc=sqlite3_step(statement))==SQLITE_ROW){
        const char *task=(const char*)sqlite3_column_text(statement,0),*capability=(const char*)sqlite3_column_text(statement,1);
        const char *kind=(const char*)sqlite3_column_text(statement,2),*target=(const char*)sqlite3_column_text(statement,3);
        const char *task_caller=(const char*)sqlite3_column_text(statement,4),*state=(const char*)sqlite3_column_text(statement,5);
        int requires_ui=sqlite3_column_int(statement,6),attempts=sqlite3_column_int(statement,7);
        sqlite3_int64 created=sqlite3_column_int64(statement,8),updated=sqlite3_column_int64(statement,9);
        const char *result=(const char*)sqlite3_column_text(statement,10),*error=(const char*)sqlite3_column_text(statement,11);
        char etask[256]={0},ecap[384]={0},ekind[128]={0},etarget[512]={0},ecaller[256]={0},estate[128]={0},eresult[1024]={0},eerror[1024]={0};
        json_escape(task?task:"",etask,sizeof(etask));json_escape(capability?capability:"",ecap,sizeof(ecap));
        json_escape(kind?kind:"",ekind,sizeof(ekind));json_escape(target?target:"",etarget,sizeof(etarget));
        json_escape(task_caller?task_caller:"",ecaller,sizeof(ecaller));json_escape(state?state:"",estate,sizeof(estate));
        json_escape(result?result:"",eresult,sizeof(eresult));json_escape(error?error:"",eerror,sizeof(eerror));
        n=snprintf(response+used,98304-used,
            "%s{\"taskId\":\"%s\",\"capabilityId\":\"%s\",\"kind\":\"%s\",\"target\":\"%s\",\"caller\":\"%s\",\"state\":\"%s\",\"requiresUI\":%s,\"attemptCount\":%d,\"createdAt\":%lld,\"updatedAt\":%lld,\"result\":%s%s%s,\"error\":%s%s%s}",
            row?",":"",etask,ecap,ekind,etarget,ecaller,estate,requires_ui?"true":"false",attempts,(long long)created,(long long)updated,
            result?"\"":"null",result?eresult:"",result?"\"":"",error?"\"":"null",error?eerror:"",error?"\"":"");
        if(n<0||(size_t)n>=98304-used)break;used+=(size_t)n;row++;
        if(used>94000)break;
    }
    sqlite3_finalize(statement);sqlite3_close(db);
    n=snprintf(response+used,98304-used,"],\"count\":%d}",row);
    if(n<0||(size_t)n>=98304-used){free(response);send_error(fd,500,"task catalog response too large");return;}
    send_response(fd,200,"application/json",response);free(response);
}

static void send_hello(int fd, RTAuthRole role, const char *caller) {
    char machine[64]={0},osbuild[64]={0};
    sysctl_string("hw.machine",machine,sizeof(machine));
    sysctl_string("kern.osversion",osbuild,sizeof(osbuild));
    int rootless=access("/var/jb",F_OK)==0;
    unsigned long long revision=0; int revision_available=ledger_current_revision(&revision);
    char escaped_caller[256]={0};json_escape(caller?caller:"authenticated-client",escaped_caller,sizeof(escaped_caller));
    char response[4096]={0};
    snprintf(response,sizeof(response),
        "{\"service\":\"roottools.device-service\",\"schemaVersion\":%d,\"daemonVersion\":\"%s\","
        "\"authenticatedRole\":\"%s\",\"authenticatedCaller\":\"%s\",\"platform\":\"ios\",\"machine\":\"%s\",\"osBuild\":\"%s\","
        "\"privilegeState\":\"%s\",\"generation\":%d,\"revision\":%llu,\"revisionAvailable\":%s,\"capabilityCount\":%zu,"
        "\"features\":{\"typedActions\":true,\"commandGateway\":true,\"namedPrincipals\":true,\"ownerPolicy\":true,\"permissionProfiles\":true,\"developerMode\":true,\"performanceSnapshot\":true,\"durableIdempotency\":true,"
        "\"expectedRevision\":true,\"eventAudit\":true,\"durableTasks\":true,\"semanticUIAutomation\":true,\"runtimeAdapters\":true,\"runtimeSemanticObservation\":true,\"providerRegistry\":true,\"packageProviderPlanning\":true,\"packageController\":true,\"packageLifecycle\":true,\"selfUpdater\":true,\"packageChunkBytes\":262144,\"lockAwareAutomation\":true,\"deferredUIJobs\":true,\"tccReadOnly\":true,\"rawPrivilegedShell\":false}}",
        SERVICE_SCHEMA_VERSION,VERSION,auth_role_name(role),escaped_caller,machine,osbuild,
        rootless&&getuid()==0?"jailbreak-root":"degraded",getpid(),revision,revision_available?"true":"false",rt_capability_count());
    send_response(fd,200,"application/json",response);
}

static int copy_regular_file_nofollow(const char *source, const char *destination) {
    int in=open(source,O_RDONLY|O_NOFOLLOW);
    if(in<0)return 0;
    struct stat st;
    if(fstat(in,&st)!=0||!S_ISREG(st.st_mode)){close(in);return 0;}
    int out=open(destination,O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0600);
    if(out<0){close(in);return 0;}
    char buffer[65536];
    int ok=1;
    for(;;){
        ssize_t n=read(in,buffer,sizeof(buffer));
        if(n==0)break;
        if(n<0){ok=0;break;}
        size_t offset=0;
        while(offset<(size_t)n){
            ssize_t written=write(out,buffer+offset,(size_t)n-offset);
            if(written<=0){ok=0;break;}
            offset+=(size_t)written;
        }
        if(!ok)break;
    }
    if(ok&&fsync(out)!=0)ok=0;
    close(out); close(in);
    if(!ok)unlink(destination);
    return ok;
}

static int tcc_snapshot_open(const char *db_path, sqlite3 **db_out, char *detail, size_t detail_cap) {
    const char *snapshot_dir=getenv("ROOTTOOLS_TCC_SNAPSHOT_DIR");
    if(!snapshot_dir||!snapshot_dir[0])snapshot_dir="/var/mobile/Library/RootTools/tcc-snapshot";
    if(mkdir("/var/mobile/Library/RootTools",0700)!=0&&errno!=EEXIST&&strstr(snapshot_dir,"/var/mobile/Library/RootTools/")==snapshot_dir){
        snprintf(detail,detail_cap,"snapshot root mkdir failed: %s",strerror(errno));
        return 0;
    }
    if(mkdir(snapshot_dir,0700)!=0&&errno!=EEXIST){
        snprintf(detail,detail_cap,"snapshot mkdir failed: %s",strerror(errno));
        return 0;
    }
    char snapshot_db[1536]={0},snapshot_wal[1536]={0},snapshot_shm[1536]={0};
    char source_wal[1536]={0},source_shm[1536]={0};
    int n=snprintf(snapshot_db,sizeof(snapshot_db),"%s/TCC.db",snapshot_dir);
    if(n<=0||(size_t)n>=sizeof(snapshot_db))return 0;
    snprintf(snapshot_wal,sizeof(snapshot_wal),"%s/TCC.db-wal",snapshot_dir);
    snprintf(snapshot_shm,sizeof(snapshot_shm),"%s/TCC.db-shm",snapshot_dir);
    snprintf(source_wal,sizeof(source_wal),"%s-wal",db_path);
    snprintf(source_shm,sizeof(source_shm),"%s-shm",db_path);
    unlink(snapshot_db);unlink(snapshot_wal);unlink(snapshot_shm);
    if(!copy_regular_file_nofollow(db_path,snapshot_db)){
        snprintf(detail,detail_cap,"snapshot DB copy failed: %s",strerror(errno));
        return 0;
    }
    if(access(source_wal,R_OK)==0&&!copy_regular_file_nofollow(source_wal,snapshot_wal)){
        snprintf(detail,detail_cap,"snapshot WAL copy failed: %s",strerror(errno));
        return 0;
    }
    if(access(source_shm,R_OK)==0&&!copy_regular_file_nofollow(source_shm,snapshot_shm)){
        snprintf(detail,detail_cap,"snapshot SHM copy failed: %s",strerror(errno));
        return 0;
    }
    sqlite3 *db=NULL;
    int rc=sqlite3_open_v2(snapshot_db,&db,SQLITE_OPEN_READWRITE|SQLITE_OPEN_NOMUTEX,NULL);
    if(rc!=SQLITE_OK){
        snprintf(detail,detail_cap,"snapshot sqlite open rc=%d error=%s",rc,db?sqlite3_errmsg(db):"unavailable");
        if(db)sqlite3_close(db);
        return 0;
    }
    sqlite3_exec(db,"PRAGMA query_only=ON",NULL,NULL,NULL);
    *db_out=db;
    snprintf(detail,detail_cap,"snapshot:%s",snapshot_db);
    return 1;
}

static void send_tcc_permissions(int fd) {
    const char *db_path=getenv("ROOTTOOLS_TCC_DB");
    if(!db_path||!db_path[0])db_path="/var/mobile/Library/TCC/TCC.db";
    sqlite3 *db=NULL;
    int force_snapshot=0;
    const char *force=getenv("ROOTTOOLS_TCC_FORCE_SNAPSHOT");
    if(force&&force[0]&&!strcmp(force,"1"))force_snapshot=1;
    int rc=SQLITE_CANTOPEN;
    char source_detail[1536]={0};
    if(!force_snapshot)rc=sqlite3_open_v2(db_path,&db,SQLITE_OPEN_READONLY|SQLITE_OPEN_NOMUTEX,NULL);
    if(!force_snapshot&&rc!=SQLITE_OK){
        if(db){sqlite3_close(db);db=NULL;}
        // iOS commonly keeps TCC.db in WAL mode. A read-only helper can fail
        // to open the live database if SQLite attempts locking/sidecar access.
        // Retry as an immutable URI: this is strictly read-only and avoids
        // creating journal/shm files from the privileged daemon.
        char uri[1536]={0};
        int n=snprintf(uri,sizeof(uri),"file:%s?mode=ro&immutable=1",db_path);
        if(n>0&&(size_t)n<sizeof(uri))
            rc=sqlite3_open_v2(uri,&db,SQLITE_OPEN_READONLY|SQLITE_OPEN_URI|SQLITE_OPEN_NOMUTEX,NULL);
    }
    if(!force_snapshot&&rc==SQLITE_OK){
        snprintf(source_detail,sizeof(source_detail),"live:%s",db_path);
    }else{
        if(db){sqlite3_close(db);db=NULL;}
        if(!tcc_snapshot_open(db_path,&db,source_detail,sizeof(source_detail))){
            char error[2048]={0};
            snprintf(error,sizeof(error),"TCC database unavailable; %s",source_detail[0]?source_detail:"snapshot unavailable");
            send_error(fd,503,error);return;
        }
    }
    const char *sql="SELECT service,client,auth_value,auth_reason,last_modified FROM access ORDER BY service,client LIMIT 512";
    sqlite3_stmt *statement=NULL;
    rc=sqlite3_prepare_v2(db,sql,-1,&statement,NULL);
    if(rc!=SQLITE_OK){
        char error[2048]={0};
        snprintf(error,sizeof(error),"TCC schema unavailable; source=%s rc=%d error=%s",source_detail,rc,sqlite3_errmsg(db));
        sqlite3_close(db);send_error(fd,503,error);return;
    }

    char *response=calloc(1,65536);
    if(!response){sqlite3_finalize(statement);sqlite3_close(db);send_error(fd,500,"allocation failed");return;}
    size_t used=0; int row=0;
    char escaped_source[2048]={0};json_escape(source_detail,escaped_source,sizeof(escaped_source));
    int n=snprintf(response,65536,"{\"schemaVersion\":1,\"source\":\"%s\",\"records\":[",escaped_source);
    if(n<0){free(response);sqlite3_finalize(statement);sqlite3_close(db);send_error(fd,500,"encoding failed");return;}
    used=(size_t)n;
    while((rc=sqlite3_step(statement))==SQLITE_ROW){
        const unsigned char *service=sqlite3_column_text(statement,0);
        const unsigned char *client=sqlite3_column_text(statement,1);
        int auth_value=sqlite3_column_int(statement,2),auth_reason=sqlite3_column_int(statement,3);
        sqlite3_int64 last_modified=sqlite3_column_int64(statement,4);
        char eservice[512]={0},eclient[1024]={0};
        json_escape(service?(const char*)service:"",eservice,sizeof(eservice));
        json_escape(client?(const char*)client:"",eclient,sizeof(eclient));
        n=snprintf(response+used,65536-used,
            "%s{\"service\":\"%s\",\"client\":\"%s\",\"authValue\":%d,\"authReason\":%d,\"lastModified\":%lld}",
            row?",":"",eservice,eclient,auth_value,auth_reason,(long long)last_modified);
        if(n<0||(size_t)n>=65536-used)break;
        used+=(size_t)n; row++;
        if(used>61000)break;
    }
    sqlite3_finalize(statement); sqlite3_close(db);
    n=snprintf(response+used,65536-used,"],\"count\":%d}",row);
    if(n<0||(size_t)n>=65536-used){free(response);send_error(fd,500,"TCC response too large");return;}
    send_response(fd,200,"application/json",response); free(response);
}

static int zxtouch_device_info_value(int subtask, char *out, size_t cap) {
    int fd=socket(AF_INET,SOCK_STREAM,0); if(fd<0)return 0;
    struct timeval timeout={.tv_sec=0,.tv_usec=700000};
    setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
    struct sockaddr_in addr={0}; addr.sin_family=AF_INET; addr.sin_port=htons(6000); addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    if(connect(fd,(struct sockaddr*)&addr,sizeof(addr))!=0){close(fd);return 0;}
    char request[32]={0}; int request_len=snprintf(request,sizeof(request),"25%d\r\n",subtask);
    if(request_len<=0||send(fd,request,(size_t)request_len,0)!=request_len){close(fd);return 0;}
    char response[512]={0}; ssize_t n=recv(fd,response,sizeof(response)-1,0); close(fd);
    if(n<=0)return 0; response[n]=0;
    if(response[0]!='0')return 0;
    char *payload=strstr(response,";;"); if(!payload)return 0; payload+=2;
    while(*payload==' '||*payload=='\t')payload++;
    size_t length=strcspn(payload,"\r\n"); if(length==0||length>=cap)return 0;
    memcpy(out,payload,length); out[length]=0; return 1;
}

static int send_all_bytes(int fd, const char *bytes, size_t length) {
    size_t sent=0;
    while(sent<length){
        ssize_t n=send(fd,bytes+sent,length-sent,0);
        if(n<0&&errno==EINTR)continue;
        if(n<=0)return 0;
        sent+=(size_t)n;
    }
    return 1;
}

static int zxtouch_connect(void) {
    int fd=socket(AF_INET,SOCK_STREAM,0);if(fd<0)return -1;
    struct timeval timeout={.tv_sec=2,.tv_usec=0};
    setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
    struct sockaddr_in addr={0};addr.sin_family=AF_INET;addr.sin_port=htons(6000);addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    if(connect(fd,(struct sockaddr*)&addr,sizeof(addr))!=0){close(fd);return -1;}
    return fd;
}

static int zxtouch_command_ok(const char *request) {
    int fd=zxtouch_connect();if(fd<0)return 0;
    size_t length=strlen(request);
    if(!send_all_bytes(fd,request,length)){close(fd);return 0;}
    char response[1024]={0};ssize_t n=recv(fd,response,sizeof(response)-1,0);close(fd);
    return n>0&&response[0]=='0';
}

static int zxtouch_screen_geometry(int *width, int *height) {
    char size_value[128]={0};
    if(!zxtouch_device_info_value(1,size_value,sizeof(size_value)))return 0;
    char *save=NULL,*w=strtok_r(size_value,";",&save),*h=strtok_r(NULL,";",&save);
    if(!w||!h)return 0;
    char *wend=NULL,*hend=NULL;double wd=strtod(w,&wend),hd=strtod(h,&hend);
    if(wend==w||hend==h||wd<=0||hd<=0||wd>100000||hd>100000)return 0;
    *width=(int)wd;*height=(int)hd;return 1;
}

static int zxtouch_point_valid(int x, int y) {
    int width=0,height=0;
    return zxtouch_screen_geometry(&width,&height)&&x>=0&&y>=0&&x<width&&y<height;
}

static int zxtouch_touch_event(int fd, int touch_type, int finger, int x, int y) {
    if(touch_type<0||touch_type>2||finger<0||finger>19||x<0||y<0||x>9999||y>9999)return 0;
    char request[64]={0};
    int n=snprintf(request,sizeof(request),"101%d%02d%05d%05d\r\n",touch_type,finger,x*10,y*10);
    return n>0&&(size_t)n<sizeof(request)&&send_all_bytes(fd,request,(size_t)n);
}

static int zxtouch_tap_point(int x, int y) {
    if(!zxtouch_point_valid(x,y))return 0;
    int fd=zxtouch_connect();if(fd<0)return 0;
    int ok=zxtouch_touch_event(fd,1,0,x,y);
    if(ok)usleep(80000);
    if(ok)ok=zxtouch_touch_event(fd,0,0,x,y);
    if(ok)usleep(50000);
    close(fd);
    int width=0,height=0;
    return ok&&zxtouch_screen_geometry(&width,&height);
}

static int utf8_char_length(unsigned char lead) {
    if(lead<0x80)return 1;
    if((lead&0xE0)==0xC0)return 2;
    if((lead&0xF0)==0xE0)return 3;
    if((lead&0xF8)==0xF0)return 4;
    return 0;
}

static int zxtouch_insert_text(const char *text) {
    size_t length=text?strlen(text):0;
    if(!length||length>1024)return 0;
    size_t offset=0;int count=0;
    while(offset<length){
        unsigned char lead=(unsigned char)text[offset];
        int char_len=utf8_char_length(lead);
        if(char_len<=0||offset+(size_t)char_len>length||count>=256)return 0;
        for(int i=1;i<char_len;i++)if(((unsigned char)text[offset+(size_t)i]&0xC0)!=0x80)return 0;
        if(char_len==1&&(lead<0x20||lead==0x7f||lead=='\r'||lead=='\n'))return 0;
        char character[8]={0};memcpy(character,text+offset,(size_t)char_len);
        char request[64]={0};int n=snprintf(request,sizeof(request),"241;;%s\r\n",character);
        if(n<=0||(size_t)n>=sizeof(request)||!zxtouch_command_ok(request))return 0;
        offset+=(size_t)char_len;count++;
    }
    return 1;
}

static int zxtouch_swipe_points(int sx,int sy,int ex,int ey,int duration_ms,int steps) {
    if(duration_ms<50||duration_ms>5000||steps<1||steps>60||!zxtouch_point_valid(sx,sy)||!zxtouch_point_valid(ex,ey))return 0;
    int fd=zxtouch_connect();if(fd<0)return 0;
    int interval_us=(duration_ms*1000)/(steps+1);
    int ok=zxtouch_touch_event(fd,1,1,sx,sy);
    if(ok)usleep((useconds_t)interval_us);
    for(int i=1;ok&&i<=steps;i++){
        double progress=(double)i/(double)(steps+1);
        int x=(int)(sx+(ex-sx)*progress),y=(int)(sy+(ey-sy)*progress);
        ok=zxtouch_touch_event(fd,2,1,x,y);
        if(ok)usleep((useconds_t)interval_us);
    }
    if(ok)ok=zxtouch_touch_event(fd,0,1,ex,ey);
    close(fd);
    int width=0,height=0;
    return ok&&zxtouch_screen_geometry(&width,&height);
}

static void send_ui_observe(int fd) {
    char size_value[128]={0},orientation_value[128]={0},scale_value[128]={0};
    if(!zxtouch_device_info_value(1,size_value,sizeof(size_value))||
       !zxtouch_device_info_value(2,orientation_value,sizeof(orientation_value))||
       !zxtouch_device_info_value(3,scale_value,sizeof(scale_value))){send_error(fd,503,"UI observation provider unavailable");return;}
    char *save=NULL,*w=strtok_r(size_value,";",&save),*h=strtok_r(NULL,";",&save);
    if(!w||!h){send_error(fd,500,"invalid UI geometry");return;}
    RTLockSnapshot snapshot=device_lock_snapshot();
    char orientation[256]={0};json_escape(orientation_value,orientation,sizeof(orientation));
    char response[2048]={0};
    snprintf(response,sizeof(response),
        "{\"schemaVersion\":1,\"providerId\":\"ui.zxtouch\",\"lockState\":\"%s\",\"screenState\":\"%s\",\"uiExecutionReady\":%s,"
        "\"screen\":{\"width\":%.0f,\"height\":%.0f,\"scale\":%.4g,\"orientation\":\"%s\"}}",
        lock_state_name(snapshot),screen_state_name(snapshot),ui_execution_ready(snapshot)?"true":"false",
        strtod(w,NULL),strtod(h,NULL),strtod(scale_value,NULL),orientation);
    send_response(fd,200,"application/json",response);
}

static void send_ui_screen_info(int fd) {
    char size_value[128]={0},orientation_value[128]={0},scale_value[128]={0};
    if(!zxtouch_device_info_value(1,size_value,sizeof(size_value)) ||
       !zxtouch_device_info_value(2,orientation_value,sizeof(orientation_value)) ||
       !zxtouch_device_info_value(3,scale_value,sizeof(scale_value))){
        send_error(fd,503,"ZXTouch screen adapter unavailable"); return;
    }
    char *save=NULL; char *width_text=strtok_r(size_value,";",&save); char *height_text=strtok_r(NULL,";",&save);
    if(!width_text||!height_text){send_error(fd,500,"unexpected ZXTouch screen size response");return;}
    char *width_end=NULL,*height_end=NULL,*scale_end=NULL;
    double width=strtod(width_text,&width_end),height=strtod(height_text,&height_end),scale=strtod(scale_value,&scale_end);
    if(width_end==width_text||height_end==height_text||scale_end==scale_value||width<=0||height<=0||scale<=0){
        send_error(fd,500,"invalid ZXTouch screen geometry"); return;
    }
    char orientation[256]={0}; json_escape(orientation_value,orientation,sizeof(orientation));
    char response[1024]={0};
    snprintf(response,sizeof(response),
        "{\"ok\":true,\"screen\":{\"width\":%.0f,\"height\":%.0f,\"scale\":%.4g,\"orientation\":\"%s\",\"implementation\":\"ios.zxtouch\"}}",
        width,height,scale,orientation);
    send_response(fd,200,"application/json",response);
}

static int process_name_exists(const char *wanted) {
    size_t length=0; int mib[4]={CTL_KERN,KERN_PROC,KERN_PROC_ALL,0};
    if(sysctl(mib,4,NULL,&length,NULL,0)!=0||!length)return 0;
    struct kinfo_proc *items=malloc(length); if(!items)return 0;
    if(sysctl(mib,4,items,&length,NULL,0)!=0){free(items);return 0;}
    size_t count=length/sizeof(struct kinfo_proc); int found=0;
    for(size_t i=0;i<count;i++) if(!strcmp(items[i].kp_proc.p_comm,wanted)){found=1;break;}
    free(items); return found;
}

static int wait_process_name(const char *name, int should_exist) {
    for(int i=0;i<20;i++){
        int exists=process_name_exists(name);
        if(exists==should_exist)return 1;
        usleep(50000);
    }
    return process_name_exists(name)==should_exist;
}

static int wait_pid_gone(pid_t pid) {
    for(int i=0;i<20;i++){
        if(!process_info(pid,NULL,NULL,0))return 1;
        usleep(50000);
    }
    return !process_info(pid,NULL,NULL,0);
}

static int verify_file_contents(const char *path, const char *expected) {
    int in=open(path,O_RDONLY|O_NOFOLLOW); if(in<0)return 0;
    struct stat st; if(fstat(in,&st)!=0||st.st_size<0||st.st_size>16384){close(in);return 0;}
    size_t expected_len=strlen(expected); if((size_t)st.st_size!=expected_len){close(in);return 0;}
    char *buffer=calloc(1,expected_len+1); if(!buffer){close(in);return 0;}
    ssize_t n=read(in,buffer,expected_len); close(in);
    int ok=n==(ssize_t)expected_len&&!memcmp(buffer,expected,expected_len); free(buffer); return ok;
}

static void execute_app_launch(const char *body, RTActionExecution *execution) {
    char bundle[256]={0}, executable[256]={0};
    if(!json_get_string(body,"bundleID",bundle,sizeof(bundle))||!safe_bundle_id(bundle)){
        snprintf(execution->target,sizeof(execution->target),"invalid bundleID");
        snprintf(execution->message,sizeof(execution->message),"Invalid bundle identifier"); return;
    }
    snprintf(execution->target,sizeof(execution->target),"%s",bundle);
    if(!executable_for_bundle(bundle,executable,sizeof(executable))){snprintf(execution->message,sizeof(execution->message),"Could not resolve app executable");return;}
    execution->executed=1;
    char *argv[]={(char*)"uiopen",(char*)"--bundleid",bundle,NULL}; int rc=fixed_spawn_wait("/var/jb/usr/bin/uiopen",argv,NULL,0);
    execution->post_checked=1; execution->post_passed=rc==0&&wait_process_name(executable,1);
    snprintf(execution->post_detail,sizeof(execution->post_detail),execution->post_passed?"process %s is present":"process %s was not observed",executable);
    execution->ok=rc==0&&execution->post_passed;
    snprintf(execution->result,sizeof(execution->result),execution->ok?"success":"failed");
    snprintf(execution->message,sizeof(execution->message),execution->ok?"Launched %s and verified process":"Launch failed or post-condition failed for %s",bundle);
}

static void execute_app_terminate(const char *body, RTActionExecution *execution) {
    char bundle[256]={0}, executable[256]={0};
    if(!json_get_string(body,"bundleID",bundle,sizeof(bundle))||!safe_bundle_id(bundle)){snprintf(execution->target,sizeof(execution->target),"invalid bundleID");snprintf(execution->message,sizeof(execution->message),"Invalid bundle identifier");return;}
    snprintf(execution->target,sizeof(execution->target),"%s",bundle);
    if(!executable_for_bundle(bundle,executable,sizeof(executable))){snprintf(execution->message,sizeof(execution->message),"Could not resolve app executable");return;}
    if(critical_process_name(executable)){snprintf(execution->result,sizeof(execution->result),"denied");snprintf(execution->message,sizeof(execution->message),"Denied: critical application executable");return;}
    execution->executed=1;
    char *argv[]={(char*)"killall",(char*)"-TERM",executable,NULL}; int rc=fixed_spawn_wait("/var/jb/usr/bin/killall",argv,NULL,0);
    execution->post_checked=1; execution->post_passed=wait_process_name(executable,0);
    snprintf(execution->post_detail,sizeof(execution->post_detail),execution->post_passed?"process %s is absent":"process %s is still present",executable);
    execution->ok=rc==0&&execution->post_passed;
    snprintf(execution->result,sizeof(execution->result),execution->ok?"success":"failed");
    snprintf(execution->message,sizeof(execution->message),execution->ok?"Terminated %s and verified absence":"Terminate failed or post-condition failed for %s",bundle);
}

static void execute_process_terminate(const char *body, RTActionExecution *execution) {
    long raw=0; if(!json_get_int(body,"pid",&raw)||raw<=100||raw>999999){snprintf(execution->target,sizeof(execution->target),"invalid pid");snprintf(execution->message,sizeof(execution->message),"PID outside allowed range");return;}
    pid_t pid=(pid_t)raw; uid_t uid=0; char name[128]={0};
    if(!process_info(pid,&uid,name,sizeof(name))){snprintf(execution->target,sizeof(execution->target),"pid=%d",pid);snprintf(execution->message,sizeof(execution->message),"Process not found");return;}
    snprintf(execution->target,sizeof(execution->target),"pid=%d uid=%u %s",pid,uid,name);
    if(uid==0||critical_process_name(name)){snprintf(execution->result,sizeof(execution->result),"denied");snprintf(execution->message,sizeof(execution->message),"Denied: root or critical process");return;}
    execution->executed=1;
    int rc=kill(pid,SIGTERM);
    execution->post_checked=1; execution->post_passed=rc==0&&wait_pid_gone(pid);
    snprintf(execution->post_detail,sizeof(execution->post_detail),execution->post_passed?"pid %d is absent":"pid %d is still present",pid);
    execution->ok=rc==0&&execution->post_passed;
    snprintf(execution->result,sizeof(execution->result),execution->ok?"success":"failed");
    snprintf(execution->message,sizeof(execution->message),execution->ok?"SIGTERM sent and process exit verified":"SIGTERM failed or process remained alive");
}

static void execute_file_write(const char *body, RTActionExecution *execution) {
    char scope[32]={0},name[128]={0},content[20000]={0},path[1024]={0};
    if(!json_get_string(body,"scope",scope,sizeof(scope))||!json_get_string(body,"name",name,sizeof(name))||!json_get_string(body,"content",content,sizeof(content))){snprintf(execution->target,sizeof(execution->target),"invalid file request");snprintf(execution->message,sizeof(execution->message),"Missing file fields");return;}
    if(strlen(content)>16384||!build_file_path(scope,name,path,sizeof(path))){snprintf(execution->target,sizeof(execution->target),"%s/%s",scope,name);snprintf(execution->message,sizeof(execution->message),"File request outside allowed RootTools scope");return;}
    snprintf(execution->target,sizeof(execution->target),"%s",path);
    execution->executed=1;
    int out=open(path,O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0640); if(out<0){snprintf(execution->message,sizeof(execution->message),"Write failed: %s",strerror(errno));return;}
    size_t len=strlen(content); ssize_t n=write(out,content,len); fsync(out); close(out);
    execution->post_checked=1; execution->post_passed=n==(ssize_t)len&&verify_file_contents(path,content);
    snprintf(execution->post_detail,sizeof(execution->post_detail),execution->post_passed?"read-back matched %zu bytes":"read-back mismatch",len);
    execution->ok=n==(ssize_t)len&&execution->post_passed;
    snprintf(execution->result,sizeof(execution->result),execution->ok?"success":"failed");
    snprintf(execution->message,sizeof(execution->message),execution->ok?"Wrote %zu bytes and verified read-back":"Write or post-condition failed",len);
}

static void execute_file_read(const char *body, RTActionExecution *execution) {
    char scope[32]={0},name[128]={0},path[1024]={0};
    if(!json_get_string(body,"scope",scope,sizeof(scope))||!json_get_string(body,"name",name,sizeof(name))||!build_file_path(scope,name,path,sizeof(path))){snprintf(execution->target,sizeof(execution->target),"invalid file request");snprintf(execution->message,sizeof(execution->message),"File request outside allowed RootTools scope");return;}
    snprintf(execution->target,sizeof(execution->target),"%s",path);
    execution->executed=1;
    int in=open(path,O_RDONLY|O_NOFOLLOW); if(in<0){snprintf(execution->message,sizeof(execution->message),"File not found");return;}
    struct stat st; if(fstat(in,&st)!=0||st.st_size<0||st.st_size>32768){close(in);snprintf(execution->message,sizeof(execution->message),"File is unavailable or exceeds 32 KiB");return;}
    char *raw=calloc(1,(size_t)st.st_size+2); ssize_t n=read(in,raw,(size_t)st.st_size+1); close(in); if(n<0){free(raw);snprintf(execution->message,sizeof(execution->message),"File read failed");return;} raw[n]=0;
    execution->output=raw; execution->post_checked=1; execution->post_passed=1; execution->ok=1;
    snprintf(execution->result,sizeof(execution->result),"success"); snprintf(execution->post_detail,sizeof(execution->post_detail),"bounded read completed"); snprintf(execution->message,sizeof(execution->message),"Read allowed RootTools file");
}

static void copy_package_operation(const char *package_id, const RTPackageOperation *package, RTActionExecution *execution) {
    snprintf(execution->target,sizeof(execution->target),"package=%s",package_id?package_id:"unknown");
    execution->ok=package->ok;
    execution->executed=package->executed;
    execution->post_checked=package->post_checked;
    execution->post_passed=package->post_passed;
    snprintf(execution->result,sizeof(execution->result),"%s",package->result);
    snprintf(execution->message,sizeof(execution->message),"%s",package->message);
    snprintf(execution->post_detail,sizeof(execution->post_detail),"%s",package->post_detail);
    if(package->output[0])execution->output=strdup(package->output);
}

static void execute_package_stage_begin(const char *body, RTActionExecution *execution) {
    char package_id[96]={0},name[192]={0},format[16]={0},identifier[256]={0},sha256[65]={0};
    long total_size=0;
    RTPackageOperation op;
    if(!json_get_string(body,"packageId",package_id,sizeof(package_id))||
       !json_get_string(body,"name",name,sizeof(name))||
       !json_get_string(body,"format",format,sizeof(format))||
       !json_get_string(body,"expectedIdentifier",identifier,sizeof(identifier))||
       !json_get_string(body,"sha256",sha256,sizeof(sha256))||
       !json_get_int(body,"totalSize",&total_size)){
        snprintf(execution->target,sizeof(execution->target),"invalid package staging request");
        snprintf(execution->message,sizeof(execution->message),"Package staging metadata is incomplete");
        return;
    }
    rt_package_begin(package_id,name,format,identifier,(long long)total_size,sha256,&op);
    copy_package_operation(package_id,&op,execution);
}

static void execute_package_stage_chunk(const char *body, RTActionExecution *execution) {
    char package_id[96]={0};
    long offset=0;
    if(!json_get_string(body,"packageId",package_id,sizeof(package_id))||!json_get_int(body,"offset",&offset)){
        snprintf(execution->target,sizeof(execution->target),"invalid package chunk request");
        snprintf(execution->message,sizeof(execution->message),"packageId and offset are required");
        return;
    }
    char *encoded=json_get_alloc_string(body,"data",360000);
    if(!encoded){snprintf(execution->target,sizeof(execution->target),"package=%s",package_id);snprintf(execution->message,sizeof(execution->message),"Package chunk data is missing or too large");return;}
    size_t decoded_length=0;
    unsigned char *decoded=base64_decode(encoded,&decoded_length);
    free(encoded);
    if(!decoded){snprintf(execution->target,sizeof(execution->target),"package=%s",package_id);snprintf(execution->message,sizeof(execution->message),"Package chunk base64 is invalid");return;}
    RTPackageOperation op;
    rt_package_append(package_id,(long long)offset,decoded,decoded_length,&op);
    free(decoded);
    copy_package_operation(package_id,&op,execution);
}

static void execute_package_stage_commit(const char *body, RTActionExecution *execution) {
    char package_id[96]={0};
    if(!json_get_string(body,"packageId",package_id,sizeof(package_id))){snprintf(execution->message,sizeof(execution->message),"packageId is required");return;}
    RTPackageOperation op;rt_package_commit(package_id,&op);copy_package_operation(package_id,&op,execution);
}

static void execute_package_discard(const char *body, RTActionExecution *execution) {
    char package_id[96]={0};
    if(!json_get_string(body,"packageId",package_id,sizeof(package_id))){snprintf(execution->message,sizeof(execution->message),"packageId is required");return;}
    RTPackageOperation op;rt_package_discard(package_id,&op);copy_package_operation(package_id,&op,execution);
}

static void execute_package_install_deb(const char *body, RTActionExecution *execution) {
    char package_id[96]={0};
    if(!json_get_string(body,"packageId",package_id,sizeof(package_id))){snprintf(execution->message,sizeof(execution->message),"packageId is required");return;}
    RTPackageOperation op;rt_package_install_deb(package_id,&op);copy_package_operation(package_id,&op,execution);
}

static void execute_package_install_ipa(const char *body, RTActionExecution *execution) {
    char package_id[96]={0};
    if(!json_get_string(body,"packageId",package_id,sizeof(package_id))){snprintf(execution->message,sizeof(execution->message),"packageId is required");return;}
    RTPackageOperation op;rt_package_install_ipa(package_id,&op);copy_package_operation(package_id,&op,execution);
}

static void execute_package_rollback_deb(const char *body, RTActionExecution *execution) {
    char package_id[96]={0};
    if(!json_get_string(body,"packageId",package_id,sizeof(package_id))){snprintf(execution->message,sizeof(execution->message),"packageId is required");return;}
    RTPackageOperation op;rt_package_rollback_deb(package_id,&op);copy_package_operation(package_id,&op,execution);
}

static void execute_package_rollback_ipa(const char *body, RTActionExecution *execution) {
    char package_id[96]={0};
    if(!json_get_string(body,"packageId",package_id,sizeof(package_id))){snprintf(execution->message,sizeof(execution->message),"packageId is required");return;}
    RTPackageOperation op;rt_package_rollback_ipa(package_id,&op);copy_package_operation(package_id,&op,execution);
}

static void execute_package_uninstall_deb(const char *body, RTActionExecution *execution) {
    char package_id[96]={0};
    if(!json_get_string(body,"packageId",package_id,sizeof(package_id))){snprintf(execution->message,sizeof(execution->message),"packageId is required");return;}
    RTPackageOperation op;rt_package_uninstall_deb(package_id,&op);copy_package_operation(package_id,&op,execution);
}

static void execute_package_uninstall_ipa(const char *body, RTActionExecution *execution) {
    char package_id[96]={0};
    if(!json_get_string(body,"packageId",package_id,sizeof(package_id))){snprintf(execution->message,sizeof(execution->message),"packageId is required");return;}
    RTPackageOperation op;rt_package_uninstall_ipa(package_id,&op);copy_package_operation(package_id,&op,execution);
}

static void execute_self_update_schedule(const char *body, const char *request_id, RTActionExecution *execution) {
    char package_id[96]={0};
    if(!json_get_string(body,"packageId",package_id,sizeof(package_id))){snprintf(execution->message,sizeof(execution->message),"packageId is required");return;}
    RTUpdateOperation update;
    rt_update_schedule(request_id,package_id,&update);
    snprintf(execution->target,sizeof(execution->target),"package=%s update=%s",package_id,request_id);
    execution->ok=update.ok;execution->executed=update.executed;execution->post_checked=update.post_checked;execution->post_passed=update.post_passed;
    snprintf(execution->result,sizeof(execution->result),"%s",update.result);
    snprintf(execution->message,sizeof(execution->message),"%s",update.message);
    snprintf(execution->post_detail,sizeof(execution->post_detail),"%s",update.post_detail);
    if(update.output[0])execution->output=strdup(update.output);
}

static void execute_agent_rotate(RTActionExecution *execution) {
    unsigned char random_bytes[24]; arc4random_buf(random_bytes,sizeof(random_bytes));
    char token[sizeof(random_bytes)*2+1]={0};
    for(size_t i=0;i<sizeof(random_bytes);i++)snprintf(token+i*2,3,"%02x",random_bytes[i]);
    snprintf(execution->target,sizeof(execution->target),"trusted-agent-credential");
    execution->executed=1;
    if(!persist_runtime_agent_token(token)){
        snprintf(execution->result,sizeof(execution->result),"failed");
        snprintf(execution->message,sizeof(execution->message),"Agent credential rotation could not be persisted"); return;
    }
    char readback[160]={0}; int runtime_file=0;
    execution->post_checked=1;
    execution->post_passed=load_runtime_agent_token(readback,sizeof(readback),&runtime_file)&&runtime_file&&
        constant_time_token_equal(readback,strlen(readback),token);
    snprintf(execution->post_detail,sizeof(execution->post_detail),execution->post_passed?"runtime Agent credential replaced and verified":"credential read-back verification failed");
    execution->ok=execution->post_passed;
    snprintf(execution->result,sizeof(execution->result),execution->ok?"success":"failed");
    snprintf(execution->message,sizeof(execution->message),execution->ok?"Trusted Agent credential rotated; old Agent token is now invalid":"Agent credential rotation failed verification");
    if(execution->ok)execution->output=strdup(token);
}

static void execute_principal_create(const char *body, RTActionExecution *execution) {
    char principal_id[RT_PRINCIPAL_ID_CAP]={0};
    char kind[RT_PRINCIPAL_KIND_CAP]={0};
    char display_name[RT_PRINCIPAL_NAME_CAP]={0};
    if(!json_get_string(body,"principalId",principal_id,sizeof(principal_id))||
       !json_get_string(body,"kind",kind,sizeof(kind))||
       !json_get_string(body,"displayName",display_name,sizeof(display_name))){
        snprintf(execution->message,sizeof(execution->message),"principalId, kind and displayName are required");
        return;
    }
    char token[RT_PRINCIPAL_TOKEN_CAP]={0},error[256]={0};
    snprintf(execution->target,sizeof(execution->target),"principal=%s kind=%s",principal_id,kind);
    execution->executed=1;
    if(!rt_principal_create(principal_id,kind,display_name,token,sizeof(token),error,sizeof(error))){
        snprintf(execution->result,sizeof(execution->result),"failed");
        snprintf(execution->message,sizeof(execution->message),"%s",error[0]?error:"Principal could not be created");
        return;
    }
    execution->post_checked=1;
    char verified_id[RT_PRINCIPAL_ID_CAP]={0},verified_kind[RT_PRINCIPAL_KIND_CAP]={0};
    execution->post_passed=rt_principal_authenticate(token,verified_id,sizeof(verified_id),verified_kind,sizeof(verified_kind))&&
        !strcmp(verified_id,principal_id)&&!strcmp(verified_kind,kind);
    execution->ok=execution->post_passed;
    snprintf(execution->result,sizeof(execution->result),execution->ok?"success":"failed");
    snprintf(execution->message,sizeof(execution->message),execution->ok?"Trusted command principal created":"Principal credential verification failed");
    snprintf(execution->post_detail,sizeof(execution->post_detail),execution->post_passed?"new principal credential authenticates":"new credential did not authenticate");
    if(execution->ok)execution->output=strdup(token);
}

static void execute_principal_revoke(const char *body, RTActionExecution *execution) {
    char principal_id[RT_PRINCIPAL_ID_CAP]={0};
    if(!json_get_string(body,"principalId",principal_id,sizeof(principal_id))){
        snprintf(execution->message,sizeof(execution->message),"principalId is required");
        return;
    }
    char error[256]={0};
    snprintf(execution->target,sizeof(execution->target),"principal=%s",principal_id);
    execution->executed=1;
    execution->post_checked=1;
    execution->post_passed=rt_principal_revoke(principal_id,error,sizeof(error));
    execution->ok=execution->post_passed;
    snprintf(execution->result,sizeof(execution->result),execution->ok?"success":"failed");
    snprintf(execution->message,sizeof(execution->message),"%s",execution->ok?"Trusted command principal revoked":(error[0]?error:"Principal could not be revoked"));
    snprintf(execution->post_detail,sizeof(execution->post_detail),execution->ok?"principal state is revoked":"active principal state was not changed");
}

static void execute_principal_grant(const char *body, RTActionExecution *execution) {
    char principal_id[RT_PRINCIPAL_ID_CAP]={0},capability_id[256]={0};
    long expires_at=0;
    if(!json_get_string(body,"principalId",principal_id,sizeof(principal_id))||
       !json_get_string(body,"grantedCapabilityId",capability_id,sizeof(capability_id))){
        snprintf(execution->message,sizeof(execution->message),"principalId and grantedCapabilityId are required");return;
    }
    if(json_has_key(body,"expiresAt")&&!json_get_int(body,"expiresAt",&expires_at)){
        snprintf(execution->message,sizeof(execution->message),"expiresAt must be an integer timestamp");return;
    }
    const RTCapability *granted=rt_capability_find(capability_id);
    if(!granted||granted->risk>RT_RISK_R1||!granted->enabled){
        snprintf(execution->target,sizeof(execution->target),"principal=%s capability=%s",principal_id,capability_id);
        snprintf(execution->result,sizeof(execution->result),"denied");
        snprintf(execution->message,sizeof(execution->message),"Only compiled R0/R1 capabilities may be granted to a principal");return;
    }
    char error[256]={0};
    snprintf(execution->target,sizeof(execution->target),"principal=%s capability=%s",principal_id,capability_id);
    execution->executed=1;
    execution->post_checked=1;
    int persisted=rt_principal_grant(principal_id,capability_id,(long long)expires_at,error,sizeof(error));
    execution->post_passed=persisted&&rt_principal_capability_allowed(principal_id,capability_id);
    execution->ok=execution->post_passed;
    snprintf(execution->result,sizeof(execution->result),execution->ok?"granted":"failed");
    snprintf(execution->message,sizeof(execution->message),"%s",execution->ok?"Principal capability grant persisted":(error[0]?error:"Principal grant failed"));
    snprintf(execution->post_detail,sizeof(execution->post_detail),execution->ok?"grant is active":"grant is not active");
}

static void execute_principal_ungrant(const char *body, RTActionExecution *execution) {
    char principal_id[RT_PRINCIPAL_ID_CAP]={0},capability_id[256]={0};
    if(!json_get_string(body,"principalId",principal_id,sizeof(principal_id))||
       !json_get_string(body,"grantedCapabilityId",capability_id,sizeof(capability_id))){
        snprintf(execution->message,sizeof(execution->message),"principalId and grantedCapabilityId are required");return;
    }
    char error[256]={0};
    snprintf(execution->target,sizeof(execution->target),"principal=%s capability=%s",principal_id,capability_id);
    execution->executed=1;
    execution->post_checked=1;
    int removed=rt_principal_ungrant(principal_id,capability_id,error,sizeof(error));
    execution->post_passed=removed&&!rt_principal_capability_allowed(principal_id,capability_id);
    execution->ok=execution->post_passed;
    snprintf(execution->result,sizeof(execution->result),execution->ok?"ungranted":"failed");
    snprintf(execution->message,sizeof(execution->message),"%s",execution->ok?"Principal capability grant removed":(error[0]?error:"Principal grant removal failed"));
    snprintf(execution->post_detail,sizeof(execution->post_detail),execution->ok?"grant is absent":"grant remains active");
}

static void execute_policy_set_mode(const char *body, RTActionExecution *execution) {
    char mode[32]={0};
    if(!json_get_string(body,"mode",mode,sizeof(mode))){
        snprintf(execution->message,sizeof(execution->message),"mode is required");return;
    }
    if(strcmp(mode,"restricted")&&strcmp(mode,"standard")&&strcmp(mode,"developer")){
        snprintf(execution->target,sizeof(execution->target),"mode=%s",mode);
        snprintf(execution->result,sizeof(execution->result),"denied");
        snprintf(execution->message,sizeof(execution->message),"mode must be restricted, standard, or developer");return;
    }
    char before[32]={0},after[32]={0};
    (void)rt_policy_mode_get(before,sizeof(before));
    snprintf(execution->target,sizeof(execution->target),"mode=%s",mode);
    execution->executed=1;
    execution->post_checked=1;
    int persisted=rt_policy_set_mode(mode);
    execution->post_passed=persisted&&rt_policy_mode_get(after,sizeof(after))&&!strcmp(after,mode);
    execution->ok=execution->post_passed;
    snprintf(execution->result,sizeof(execution->result),execution->ok?"success":"failed");
    snprintf(execution->message,sizeof(execution->message),execution->ok?"Execution policy mode changed":"Execution policy mode could not be persisted");
    snprintf(execution->post_detail,sizeof(execution->post_detail),execution->ok?"mode=%s previous=%s":"policy mode verification failed",mode,before[0]?before:"unknown");
}

static int ui_text_valid(const char *text) {
    size_t length=text?strlen(text):0;
    if(!length||length>1024)return 0;
    for(size_t i=0;i<length;i++){
        unsigned char c=(unsigned char)text[i];
        if(c<0x20||c==0x7f)return 0;
    }
    return 1;
}

static void execute_ui_tap_submit(const char *body, const char *task_id, const char *caller, RTActionExecution *execution) {
    long x=0,y=0;
    if(!json_get_int(body,"x",&x)||!json_get_int(body,"y",&y)||x<0||y<0||x>100000||y>100000){
        snprintf(execution->message,sizeof(execution->message),"x and y must be non-negative integer screen coordinates");return;
    }
    char payload[256]={0},target[128]={0},state[64]={0};
    snprintf(payload,sizeof(payload),"{\"x\":%ld,\"y\":%ld}",x,y);
    snprintf(target,sizeof(target),"point=%ld,%ld",x,y);
    snprintf(execution->target,sizeof(execution->target),"task=%s %s",task_id,target);
    execution->executed=1;execution->post_checked=1;
    execution->post_passed=task_enqueue_ui_action(task_id,"device.ui.tap","ui.tap",target,caller,payload)&&
        task_state(task_id,state,sizeof(state))&&!strcmp(state,"queued");
    execution->ok=execution->post_passed;
    snprintf(execution->result,sizeof(execution->result),execution->ok?"queued":"failed");
    snprintf(execution->message,sizeof(execution->message),execution->ok?"UI tap task queued":"UI tap task could not be persisted");
    snprintf(execution->post_detail,sizeof(execution->post_detail),execution->ok?"task is durable and queued":"task persistence verification failed");
    if(execution->ok)execution->output=strdup(task_id);
}

static void execute_ui_type_submit(const char *body, const char *task_id, const char *caller, RTActionExecution *execution) {
    char text[1025]={0};
    if(!json_get_string(body,"text",text,sizeof(text))||!ui_text_valid(text)){
        snprintf(execution->message,sizeof(execution->message),"text must contain 1-1024 bytes without control characters");return;
    }
    char escaped[2049]={0},payload[2304]={0},state[64]={0},target[128]={0};
    json_escape(text,escaped,sizeof(escaped));
    snprintf(payload,sizeof(payload),"{\"text\":\"%s\"}",escaped);
    snprintf(target,sizeof(target),"text-bytes=%zu",strlen(text));
    snprintf(execution->target,sizeof(execution->target),"task=%s %s",task_id,target);
    execution->executed=1;execution->post_checked=1;
    execution->post_passed=task_enqueue_ui_action(task_id,"device.ui.type","ui.type",target,caller,payload)&&
        task_state(task_id,state,sizeof(state))&&!strcmp(state,"queued");
    execution->ok=execution->post_passed;
    snprintf(execution->result,sizeof(execution->result),execution->ok?"queued":"failed");
    snprintf(execution->message,sizeof(execution->message),execution->ok?"UI text task queued":"UI text task could not be persisted");
    snprintf(execution->post_detail,sizeof(execution->post_detail),execution->ok?"task is durable and queued":"task persistence verification failed");
    if(execution->ok)execution->output=strdup(task_id);
}

static void execute_ui_swipe_submit(const char *body, const char *task_id, const char *caller, RTActionExecution *execution) {
    long sx=0,sy=0,ex=0,ey=0,duration=0,steps=0;
    if(!json_get_int(body,"startX",&sx)||!json_get_int(body,"startY",&sy)||!json_get_int(body,"endX",&ex)||!json_get_int(body,"endY",&ey)||
       !json_get_int(body,"durationMs",&duration)||!json_get_int(body,"steps",&steps)||
       sx<0||sy<0||ex<0||ey<0||sx>100000||sy>100000||ex>100000||ey>100000||duration<50||duration>5000||steps<1||steps>60){
        snprintf(execution->message,sizeof(execution->message),"Swipe requires valid integer coordinates, durationMs 50-5000 and steps 1-60");return;
    }
    char payload[512]={0},state[64]={0},target[192]={0};
    snprintf(payload,sizeof(payload),"{\"startX\":%ld,\"startY\":%ld,\"endX\":%ld,\"endY\":%ld,\"durationMs\":%ld,\"steps\":%ld}",sx,sy,ex,ey,duration,steps);
    snprintf(target,sizeof(target),"swipe=%ld,%ld->%ld,%ld",sx,sy,ex,ey);
    snprintf(execution->target,sizeof(execution->target),"task=%s %s",task_id,target);
    execution->executed=1;execution->post_checked=1;
    execution->post_passed=task_enqueue_ui_action(task_id,"device.ui.swipe","ui.swipe",target,caller,payload)&&
        task_state(task_id,state,sizeof(state))&&!strcmp(state,"queued");
    execution->ok=execution->post_passed;
    snprintf(execution->result,sizeof(execution->result),execution->ok?"queued":"failed");
    snprintf(execution->message,sizeof(execution->message),execution->ok?"UI swipe task queued":"UI swipe task could not be persisted");
    snprintf(execution->post_detail,sizeof(execution->post_detail),execution->ok?"task is durable and queued":"task persistence verification failed");
    if(execution->ok)execution->output=strdup(task_id);
}

static void execute_queue_app_launch(const char *body, const char *task_id, const char *caller, RTActionExecution *execution) {
    char bundle[256]={0};
    if(!json_get_string(body,"bundleID",bundle,sizeof(bundle))||!safe_bundle_id(bundle)){
        snprintf(execution->target,sizeof(execution->target),"invalid bundleID");
        snprintf(execution->message,sizeof(execution->message),"Invalid bundle identifier"); return;
    }
    snprintf(execution->target,sizeof(execution->target),"task=%s bundle=%s",task_id,bundle);
    execution->executed=1;
    if(!task_enqueue_app_launch(task_id,bundle,caller)){
        snprintf(execution->message,sizeof(execution->message),"Durable app launch task could not be persisted");return;
    }
    // Keep the legacy queue table as a compatibility mirror while v0.13 clients migrate.
    (void)automation_enqueue_app_launch(task_id,bundle);
    char state[64]={0};
    execution->post_checked=1;
    execution->post_passed=task_state(task_id,state,sizeof(state))&&!strcmp(state,"queued");
    snprintf(execution->post_detail,sizeof(execution->post_detail),execution->post_passed?"task is durable and queued":"task persistence verification failed");
    execution->ok=execution->post_passed;
    snprintf(execution->result,sizeof(execution->result),execution->ok?"queued":"failed");
    snprintf(execution->message,sizeof(execution->message),execution->ok?"App launch task queued; it waits for an unlocked visible UI when necessary":"Failed to queue app launch task");
    if(execution->ok)execution->output=strdup(task_id);
}

static void execute_automation_cancel(const char *body, const char *caller, int owner, RTActionExecution *execution) {
    char task_id[128]={0},state[64]={0};
    if(!json_get_string(body,"taskId",task_id,sizeof(task_id)))
        (void)json_get_string(body,"jobID",task_id,sizeof(task_id));
    if(!safe_request_id(task_id)){
        snprintf(execution->target,sizeof(execution->target),"invalid taskId");
        snprintf(execution->message,sizeof(execution->message),"Valid taskId is required");return;
    }
    snprintf(execution->target,sizeof(execution->target),"task=%s",task_id);
    if(!task_state(task_id,state,sizeof(state))){snprintf(execution->message,sizeof(execution->message),"Device task not found");return;}
    if(strcmp(state,"queued")&&strcmp(state,"waiting_for_unlock")&&strcmp(state,"retrying")){
        snprintf(execution->result,sizeof(execution->result),"denied");
        snprintf(execution->message,sizeof(execution->message),"Only queued, waiting or retrying tasks can be cancelled");return;
    }
    execution->executed=1;
    int cancelled=task_cancel(task_id,caller,owner);
    if(cancelled)(void)automation_cancel_pending(task_id);
    execution->post_checked=1;
    execution->post_passed=cancelled&&task_state(task_id,state,sizeof(state))&&!strcmp(state,"cancelled");
    snprintf(execution->post_detail,sizeof(execution->post_detail),execution->post_passed?"task state is cancelled":"task remained active or caller does not own it");
    execution->ok=execution->post_passed;
    snprintf(execution->result,sizeof(execution->result),execution->ok?"success":"failed");
    snprintf(execution->message,sizeof(execution->message),execution->ok?"Device task cancelled":"Task cancellation failed verification");
}

static int automation_update_job(const char *job_id, const char *state, const char *result, const char *error, int increment_attempt) {
    sqlite3 *db=NULL; if(!ledger_open(&db))return 0;
    sqlite3_stmt *statement=NULL;
    int rc=sqlite3_prepare_v2(db,
        "UPDATE automation_jobs SET state=?1,attempt_count=attempt_count+?2,updated_at=?3,result=?4,error=?5 WHERE job_id=?6",
        -1,&statement,NULL);
    if(rc==SQLITE_OK){
        sqlite3_bind_text(statement,1,state,-1,SQLITE_TRANSIENT);sqlite3_bind_int(statement,2,increment_attempt?1:0);
        sqlite3_bind_int64(statement,3,(sqlite3_int64)time(NULL));
        if(result)sqlite3_bind_text(statement,4,result,-1,SQLITE_TRANSIENT);else sqlite3_bind_null(statement,4);
        if(error)sqlite3_bind_text(statement,5,error,-1,SQLITE_TRANSIENT);else sqlite3_bind_null(statement,5);
        sqlite3_bind_text(statement,6,job_id,-1,SQLITE_TRANSIENT);rc=sqlite3_step(statement);
    }
    int changed=sqlite3_changes(db);sqlite3_finalize(statement);sqlite3_close(db);return rc==SQLITE_DONE&&changed==1;
}

static void automation_recover_incomplete_jobs(void) {
    sqlite3 *db=NULL; if(!ledger_open(&db))return;
    sqlite3_exec(db,"UPDATE automation_jobs SET state='pending',updated_at=strftime('%s','now'),error='recovered after daemon restart' WHERE state='running'",NULL,NULL,NULL);
    sqlite3_close(db);
}

static void automation_tick(void) {
    RTLockSnapshot snapshot=device_lock_snapshot();
    char task_id[128]={0},kind[64]={0},target[256]={0},caller[192]={0},capability_id[256]={0},payload[2304]={0},state[64]={0};
    int attempts=0,requires_ui=0;
    if(!task_next(task_id,sizeof(task_id),kind,sizeof(kind),target,sizeof(target),caller,sizeof(caller),capability_id,sizeof(capability_id),payload,sizeof(payload),&attempts,&requires_ui,state,sizeof(state)))return;

    const RTCapability *task_capability=rt_capability_find(capability_id);
    RTPolicyDecision task_policy=rt_policy_evaluate(task_capability,0);
    if(!task_capability||!task_policy.allowed||!principal_grant_allows(caller,capability_id)){
        task_update(task_id,"failed",NULL,"task authorization no longer permits execution",0);
        if(!strcmp(kind,"app.launch"))(void)automation_update_job(task_id,"failed",NULL,"task authorization no longer permits execution",0);
        return;
    }
    if(requires_ui&&!ui_execution_ready(snapshot)){
        if(strcmp(state,"waiting_for_unlock"))task_update(task_id,"waiting_for_unlock",NULL,"waiting for unlocked visible UI",0);
        return;
    }

    const char *execution_provider=!strcmp(kind,"app.launch")?"ui.springboard":
        (!strncmp(kind,"ui.",3)?"ui.zxtouch":NULL);
    const RTProvider *provider=execution_provider?rt_provider_find(execution_provider):NULL;
    if(!provider||!rt_provider_available(provider)){
        const char *next=attempts+1>=3?"failed":"retrying";
        task_update(task_id,next,NULL,"required task execution provider is unavailable",1);
        if(!strcmp(kind,"app.launch"))(void)automation_update_job(task_id,attempts+1>=3?"failed":"pending",NULL,"required task execution provider is unavailable",1);
        return;
    }

    task_update(task_id,"running",NULL,NULL,0);
    if(!strcmp(kind,"app.launch")){
        (void)automation_update_job(task_id,"running",NULL,NULL,0);
        if(!safe_bundle_id(target)){task_update(task_id,"failed",NULL,"invalid stored bundle identifier",1);(void)automation_update_job(task_id,"failed",NULL,"invalid stored bundle identifier",1);return;}
        char executable[256]={0};
        if(!executable_for_bundle(target,executable,sizeof(executable))){
            const char *next=attempts+1>=3?"failed":"retrying";
            task_update(task_id,next,NULL,"could not resolve app executable",1);
            (void)automation_update_job(task_id,attempts+1>=3?"failed":"pending",NULL,"could not resolve app executable",1);return;
        }
        char *argv[]={(char*)"uiopen",(char*)"--bundleid",target,NULL};
        int rc=fixed_spawn_wait("/var/jb/usr/bin/uiopen",argv,NULL,0);
        int passed=rc==0&&wait_process_name(executable,1);
        if(passed){task_update(task_id,"completed","application process observed",NULL,1);(void)automation_update_job(task_id,"completed","application process observed",NULL,1);return;}
        const char *next=attempts+1>=3?"failed":"retrying";
        task_update(task_id,next,NULL,"launch failed or process was not observed",1);
        (void)automation_update_job(task_id,attempts+1>=3?"failed":"pending",NULL,"launch failed or process was not observed",1);
        return;
    }

    if(!strcmp(kind,"ui.tap")){
        long x=0,y=0;
        int ok=json_get_int(payload,"x",&x)&&json_get_int(payload,"y",&y)&&zxtouch_tap_point((int)x,(int)y);
        task_update(task_id,ok?"completed":"failed",ok?"tap sequence delivered and UI adapter re-observed":NULL,
            ok?NULL:"tap effect is indeterminate; task was not retried",1);
        return;
    }
    if(!strcmp(kind,"ui.type")){
        char text[1025]={0};
        int ok=json_get_string(payload,"text",text,sizeof(text))&&ui_text_valid(text)&&zxtouch_insert_text(text);
        task_update(task_id,ok?"completed":"failed",ok?"ZXTouch acknowledged all text characters":NULL,
            ok?NULL:"text insertion may be partial; task was not retried",1);
        return;
    }
    if(!strcmp(kind,"ui.swipe")){
        long sx=0,sy=0,ex=0,ey=0,duration=0,steps=0;
        int ok=json_get_int(payload,"startX",&sx)&&json_get_int(payload,"startY",&sy)&&json_get_int(payload,"endX",&ex)&&json_get_int(payload,"endY",&ey)&&
            json_get_int(payload,"durationMs",&duration)&&json_get_int(payload,"steps",&steps)&&
            zxtouch_swipe_points((int)sx,(int)sy,(int)ex,(int)ey,(int)duration,(int)steps);
        task_update(task_id,ok?"completed":"failed",ok?"swipe sequence delivered and UI adapter re-observed":NULL,
            ok?NULL:"swipe effect is indeterminate; task was not retried",1);
        return;
    }
    task_update(task_id,"failed",NULL,"unsupported task kind",1);
}

static pid_t self_update_child=0;

static void self_update_tick(void) {
    if(self_update_child>0){
        int status=0;pid_t reaped=waitpid(self_update_child,&status,WNOHANG);
        if(reaped==0)return;
        self_update_child=0;
    }
    char request_id[128]={0};
    if(!rt_update_claim_pending(request_id,sizeof(request_id)))return;
    char updater[1024]={0};
    if(!rt_provider_resolve_executable("roottools.updater",updater,sizeof(updater))){
        rt_update_mark(request_id,"failed",NULL,NULL,"independent updater executable unavailable");return;
    }
    char *argv[]={(char*)"roottools-updater",(char*)"--request",request_id,NULL};
    pid_t pid=0;int rc=posix_spawn(&pid,updater,NULL,NULL,argv,environ);
    if(rc!=0){rt_update_mark(request_id,"failed",NULL,NULL,"independent updater could not be spawned");return;}
    self_update_child=pid;
}

static void route_capability(
    int fd,
    const RTCapability *capability,
    const char *body,
    const char *caller,
    int trusted_confirmation_source
) {
    if(!capability){send_error(fd,404,"unknown capability");return;}
    RTActionContext context; load_action_context(body,caller,trusted_confirmation_source,&context);
    if(!safe_request_id(context.request_id)){send_error(fd,400,"requestId contains unsupported characters or length");return;}
    long expected_revision_raw=0; int has_expected_revision=json_has_key(body,"expectedRevision");
    if(has_expected_revision&&(!json_get_int(body,"expectedRevision",&expected_revision_raw)||expected_revision_raw<0)){
        send_error(fd,400,"expectedRevision must be a non-negative integer");return;
    }
    RTLedgerReservation reservation=ledger_reserve(context.request_id,context.caller,capability->id,body);
    if(reservation.kind==RT_LEDGER_REPLAY){
        char *replayed=receipt_mark_replayed(reservation.receipt);
        if(!replayed){free(reservation.receipt);send_error(fd,500,"cached receipt is invalid");return;}
        send_response(fd,200,"application/json",replayed); free(replayed); free(reservation.receipt); return;
    }
    if(reservation.kind==RT_LEDGER_CONFLICT){send_error(fd,409,reservation.detail);return;}
    if(reservation.kind==RT_LEDGER_INDETERMINATE){send_error(fd,409,reservation.detail);return;}
    if(reservation.kind==RT_LEDGER_ERROR){send_error(fd,500,reservation.detail[0]?reservation.detail:"idempotency ledger failure");return;}
    if(has_expected_revision){
        unsigned long long current_revision=0;
        if(!ledger_current_revision(&current_revision)){send_error(fd,500,"execution revision unavailable; request remains indeterminate");return;}
        if((unsigned long long)expected_revision_raw!=current_revision){
            RTPolicyDecision stale={0,0,"stale_revision","expectedRevision does not match current device execution revision"};
            RTActionExecution stale_execution; execution_init(&stale_execution);
            snprintf(stale_execution.target,sizeof(stale_execution.target),"revision=%llu",current_revision);
            snprintf(stale_execution.result,sizeof(stale_execution.result),"stale_revision");
            snprintf(stale_execution.message,sizeof(stale_execution.message),"Expected revision %ld but current revision is %llu",expected_revision_raw,current_revision);
            send_action_receipt(fd,capability,&context,stale,&stale_execution); execution_free(&stale_execution); return;
        }
    }
    if(!principal_grant_allows(context.caller,capability->id)){
        RTPolicyDecision ungranted={0,0,"principal_grant_required","capability is not granted to this command principal"};
        RTActionExecution denied_execution; execution_init(&denied_execution);
        snprintf(denied_execution.target,sizeof(denied_execution.target),"principal-grant");
        snprintf(denied_execution.result,sizeof(denied_execution.result),"denied");
        snprintf(denied_execution.message,sizeof(denied_execution.message),"Capability %s is not granted to this principal",capability->id);
        send_action_receipt(fd,capability,&context,ungranted,&denied_execution); execution_free(&denied_execution); return;
    }
    RTPolicyDecision decision=rt_policy_evaluate(capability,context.confirmed);
    RTActionExecution execution; execution_init(&execution);

    if(!decision.allowed){
        snprintf(execution.target,sizeof(execution.target),"policy");
        snprintf(execution.result,sizeof(execution.result),decision.confirmation_required?"confirmation_required":"denied");
        snprintf(execution.message,sizeof(execution.message),"%s",decision.reason);
        send_action_receipt(fd,capability,&context,decision,&execution); execution_free(&execution); return;
    }

    const char *provider_id=rt_provider_for_capability(capability->id);
    const RTProvider *provider=provider_id?rt_provider_find(provider_id):NULL;
    int provider_required_now=strcmp(capability->id,"device.automation.queue-app-launch")!=0;
    if(provider_required_now&&provider&&!rt_provider_available(provider)){
        snprintf(execution.target,sizeof(execution.target),"provider=%s",provider_id);
        snprintf(execution.result,sizeof(execution.result),"provider_unavailable");
        snprintf(execution.message,sizeof(execution.message),"Required provider %s is unavailable",provider_id);
        send_action_receipt(fd,capability,&context,decision,&execution); execution_free(&execution); return;
    }

    unsigned long long start_revision=0;
    if(!ledger_current_revision(&start_revision)||
       !ledger_append_event(context.request_id,capability->id,context.caller,"started",start_revision)){
        snprintf(execution.target,sizeof(execution.target),"event-ledger");
        snprintf(execution.result,sizeof(execution.result),"failed");
        snprintf(execution.message,sizeof(execution.message),"Execution was not started because lifecycle event persistence failed");
        send_action_receipt(fd,capability,&context,decision,&execution); execution_free(&execution); return;
    }

    const char *legacy_action=capability->legacy_action;
    if(legacy_action&&!strcmp(legacy_action,"app.launch")) execute_app_launch(body,&execution);
    else if(legacy_action&&!strcmp(legacy_action,"app.terminate")) execute_app_terminate(body,&execution);
    else if(legacy_action&&!strcmp(legacy_action,"process.terminate")) execute_process_terminate(body,&execution);
    else if(legacy_action&&!strcmp(legacy_action,"file.write")) execute_file_write(body,&execution);
    else if(legacy_action&&!strcmp(legacy_action,"file.read")) execute_file_read(body,&execution);
    else if(legacy_action&&!strcmp(legacy_action,"agent.rotate")) execute_agent_rotate(&execution);
    else if(legacy_action&&!strcmp(legacy_action,"principal.create")) execute_principal_create(body,&execution);
    else if(legacy_action&&!strcmp(legacy_action,"principal.revoke")) execute_principal_revoke(body,&execution);
    else if(legacy_action&&!strcmp(legacy_action,"principal.grant")) execute_principal_grant(body,&execution);
    else if(legacy_action&&!strcmp(legacy_action,"principal.ungrant")) execute_principal_ungrant(body,&execution);
    else if(legacy_action&&!strcmp(legacy_action,"policy.set-mode")) execute_policy_set_mode(body,&execution);
    else if(legacy_action&&!strcmp(legacy_action,"automation.queue-app-launch")) execute_queue_app_launch(body,context.request_id,context.caller,&execution);
    else if(legacy_action&&!strcmp(legacy_action,"task.submit-app-launch")) execute_queue_app_launch(body,context.request_id,context.caller,&execution);
    else if(legacy_action&&!strcmp(legacy_action,"automation.cancel")) execute_automation_cancel(body,context.caller,trusted_confirmation_source,&execution);
    else if(legacy_action&&!strcmp(legacy_action,"task.cancel")) execute_automation_cancel(body,context.caller,trusted_confirmation_source,&execution);
    else if(legacy_action&&!strcmp(legacy_action,"ui.tap")) execute_ui_tap_submit(body,context.request_id,context.caller,&execution);
    else if(legacy_action&&!strcmp(legacy_action,"ui.type")) execute_ui_type_submit(body,context.request_id,context.caller,&execution);
    else if(legacy_action&&!strcmp(legacy_action,"ui.swipe")) execute_ui_swipe_submit(body,context.request_id,context.caller,&execution);
    else if(legacy_action&&!strcmp(legacy_action,"package.stage.begin")) execute_package_stage_begin(body,&execution);
    else if(legacy_action&&!strcmp(legacy_action,"package.stage.chunk")) execute_package_stage_chunk(body,&execution);
    else if(legacy_action&&!strcmp(legacy_action,"package.stage.commit")) execute_package_stage_commit(body,&execution);
    else if(legacy_action&&!strcmp(legacy_action,"package.discard")) execute_package_discard(body,&execution);
    else if(legacy_action&&!strcmp(legacy_action,"package.install-deb")) execute_package_install_deb(body,&execution);
    else if(legacy_action&&!strcmp(legacy_action,"package.install-ipa")) execute_package_install_ipa(body,&execution);
    else if(legacy_action&&!strcmp(legacy_action,"package.rollback-deb")) execute_package_rollback_deb(body,&execution);
    else if(legacy_action&&!strcmp(legacy_action,"package.rollback-ipa")) execute_package_rollback_ipa(body,&execution);
    else if(legacy_action&&!strcmp(legacy_action,"package.uninstall-deb")) execute_package_uninstall_deb(body,&execution);
    else if(legacy_action&&!strcmp(legacy_action,"package.uninstall-ipa")) execute_package_uninstall_ipa(body,&execution);
    else if(legacy_action&&!strcmp(legacy_action,"self-update.schedule")) execute_self_update_schedule(body,context.request_id,&execution);
    else {snprintf(execution.result,sizeof(execution.result),"denied");snprintf(execution.message,sizeof(execution.message),"No executor registered");}

    send_action_receipt(fd,capability,&context,decision,&execution); execution_free(&execution);
}

static void route_action(int fd, const char *legacy_action, const char *body, const char *caller, int trusted_confirmation_source) {
    route_capability(fd,rt_capability_find_action(legacy_action),body,caller,trusted_confirmation_source);
}

static void route_action_request(int fd, const char *body, const char *caller, int trusted_confirmation_source) {
    char capability_id[256]={0};
    if(!json_get_string(body,"capabilityId",capability_id,sizeof(capability_id))){send_error(fd,400,"missing capabilityId");return;}
    route_capability(fd,rt_capability_find(capability_id),body,caller,trusted_confirmation_source);
}

static void handle_capability_set(int fd, const char *body, RTAuthRole role) {
    if(role!=RT_AUTH_ADMIN){send_error(fd,403,"capability policy can only be changed by the on-device owner UI");return;}
    char capability_id[256]={0}; int enabled=0;
    if(!json_get_string(body,"capabilityId",capability_id,sizeof(capability_id)) ||
       !json_get_bool(body,"enabled",&enabled)){send_error(fd,400,"capabilityId and enabled are required");return;}
    const RTCapability *capability=rt_capability_find(capability_id);
    if(!capability){send_error(fd,404,"unknown capability");return;}
    if(enabled && (!capability->enabled || capability->risk==RT_RISK_R3)){
        send_error(fd,403,"hard-disabled capability cannot be enabled at runtime");return;
    }
    int before=rt_capability_effective_enabled(capability);
    if(!rt_capability_set_enabled(capability_id,enabled)){send_error(fd,500,"failed to persist capability policy");return;}
    int after=rt_capability_effective_enabled(capability);
    if(before!=after){
        unsigned long long revision=0;
        if(!ledger_increment_revision(&revision)){send_error(fd,500,"capability policy changed but execution revision could not be persisted");return;}
    }
    send_capability_catalog(fd);
}

static void handle(int fd) {
    char *req=calloc(1,MAX_REQUEST); if(!req)return; ssize_t n=read_request(fd,req,MAX_REQUEST); if(n<=0){free(req);return;} if(n==-2){send_error(fd,400,"request body too large");free(req);return;}
    char caller[192]={0};
    RTAuthRole auth_role=request_auth_role(req,caller,sizeof(caller));
    if (auth_role==RT_AUTH_NONE) { send_response(fd, 401, "application/json", "{\"error\":\"unauthorized\"}"); free(req); return; }
    int trusted_confirmation_source=auth_role==RT_AUTH_ADMIN;
    char method[16]={0}, path[256]={0}; sscanf(req, "%15s %255s", method, path); const char *body=request_body(req);

    if (!strcmp(path, "/v1/hello") && !strcmp(method,"GET")) {
        send_hello(fd,auth_role,caller); free(req); return;
    }
    if (!strcmp(path, "/v1/status") && !strcmp(method,"GET")) {
        if(!authorize_read_capability(fd,"device.status.observe",caller)){free(req);return;}
        struct utsname u; uname(&u);
        char machine[64]={0}, osbuild[64]={0}; sysctl_string("hw.machine", machine, sizeof(machine)); sysctl_string("kern.osversion", osbuild, sizeof(osbuild));
        int dopamine = 0; char *proc = processes_text(&dopamine); free(proc);
        RTLockSnapshot lock_snapshot=device_lock_snapshot();
        int pending_jobs=task_active_count();
        char response[3072];
        snprintf(response, sizeof(response),
            "{\"daemonVersion\":\"%s\",\"uid\":%d,\"machine\":\"%s\",\"osBuild\":\"%s\",\"kernel\":\"%s\",\"cpuCount\":%d,\"memoryBytes\":%llu,\"rootFreeBytes\":%llu,\"varFreeBytes\":%llu,\"jailbreakRootless\":%s,\"dopamineRunning\":%s,\"sshReady\":%s,\"fridaReady\":%s,\"zxTouchReady\":%s,"
            "\"lockState\":\"%s\",\"deviceLocked\":%s,\"screenState\":\"%s\",\"screenBlanked\":%s,\"headlessExecutionReady\":%s,\"uiExecutionReady\":%s,\"automationPendingCount\":%d}",
            VERSION, getuid(), machine, osbuild, u.release, sysctl_int("hw.ncpu"), sysctl_u64("hw.memsize"), free_bytes("/"), free_bytes("/var"),
            access("/var/jb", F_OK)==0 ? "true":"false", dopamine ? "true":"false", port_open(22)?"true":"false", port_open(27042)?"true":"false", port_open(6000)?"true":"false",
            lock_state_name(lock_snapshot),lock_snapshot.lock_known?(lock_snapshot.locked?"true":"false"):"null",
            screen_state_name(lock_snapshot),lock_snapshot.screen_blank_known?(lock_snapshot.screen_blanked?"true":"false"):"null",
            (getuid()==0&&access("/var/jb",F_OK)==0)?"true":"false",ui_execution_ready(lock_snapshot)?"true":"false",pending_jobs<0?0:pending_jobs);
        send_response(fd, 200, "application/json", response); free(req); return;
    }
    if (!strcmp(method,"GET") && !strcmp(path, "/v1/performance")) { if(authorize_read_capability(fd,"device.performance.observe",caller)) send_performance(fd); }
    else if (!strcmp(method,"GET") && !strcmp(path, "/v1/runtime")) { if(authorize_read_capability(fd,"device.runtime.observe",caller)) send_text_payload(fd, runtime_text()); }
    else if (!strcmp(method,"GET") && !strcmp(path, "/v1/runtime/catalog")) { if(authorize_read_capability(fd,"device.runtime.adapters",caller)) send_runtime_catalog(fd); }
    else if (!strcmp(method,"GET") && !strcmp(path, "/v1/runtime/frida")) { if(authorize_read_capability(fd,"device.runtime.frida.observe",caller)) send_frida_status(fd); }
    else if (!strcmp(method,"GET") && !strcmp(path, "/v1/runtime/ellekit")) { if(authorize_read_capability(fd,"device.runtime.ellekit.observe",caller)) send_ellekit_status(fd); }
    else if (!strcmp(method,"GET") && !strcmp(path, "/v1/providers/catalog")) { if(authorize_read_capability(fd,"device.providers.read",caller)) send_provider_catalog(fd); }
    else if (!strcmp(method,"POST") && !strcmp(path, "/v1/package/plan")) { if(authorize_read_capability(fd,"device.package.plan",caller)) send_package_plan(fd,body); }
    else if (!strcmp(method,"GET") && !strcmp(path, "/v1/packages/catalog")) { if(authorize_read_capability(fd,"device.package.list",caller)) send_package_catalog(fd); }
    else if (!strcmp(method,"GET") && !strcmp(path, "/v1/packages/history")) { if(authorize_read_capability(fd,"device.package.history",caller)) send_package_history(fd); }
    else if (!strcmp(method,"GET") && !strcmp(path, "/v1/principals/catalog")) {
        if(auth_role!=RT_AUTH_ADMIN)send_error(fd,403,"principal catalog is owner-only");
        else if(authorize_read_capability(fd,"device.principal.list",caller))send_principal_catalog(fd);
    }
    else if (!strcmp(method,"POST") && !strcmp(path, "/v1/principals/grants")) {
        if(auth_role!=RT_AUTH_ADMIN)send_error(fd,403,"principal grants are owner-only");
        else if(authorize_read_capability(fd,"device.principal.grants.read",caller))send_principal_grants(fd,body);
    }
    else if (!strcmp(method,"GET") && !strcmp(path, "/v1/policy")) { if(authorize_read_capability(fd,"device.policy.read",caller)) send_policy_status(fd); }
    else if (!strcmp(method,"GET") && !strcmp(path, "/v1/self-update/status")) { if(authorize_read_capability(fd,"device.self-update.status",caller)) send_self_update_status(fd); }
    else if (!strcmp(method,"GET") && !strcmp(path, "/v1/device/lock-state")) { if(authorize_read_capability(fd,"device.lock.observe",caller)) send_lock_state(fd); }
    else if (!strcmp(method,"GET") && !strcmp(path, "/v1/automation/state")) { if(authorize_read_capability(fd,"device.automation.observe",caller)) send_automation_state(fd); }
    else if (!strcmp(method,"GET") && !strcmp(path, "/v1/automation/queue")) { if(authorize_read_capability(fd,"device.automation.queue.read",caller)) send_automation_queue(fd); }
    else if (!strcmp(method,"GET") && !strcmp(path, "/v1/tasks/catalog")) { if(authorize_read_capability(fd,"device.task.list",caller)) send_task_catalog(fd,caller,auth_role==RT_AUTH_ADMIN); }
    else if (!strcmp(method,"GET") && !strcmp(path, "/v1/ui/screen-info")) { if(authorize_read_capability(fd,"device.ui.screen-info",caller)) send_ui_screen_info(fd); }
    else if (!strcmp(method,"GET") && !strcmp(path, "/v1/ui/observe")) { if(authorize_read_capability(fd,"device.ui.observe",caller)) send_ui_observe(fd); }
    else if (!strcmp(method,"GET") && !strcmp(path, "/v1/apps")) { if(authorize_read_capability(fd,"device.app.list",caller)) send_text_payload(fd, apps_text()); }
    else if (!strcmp(method,"GET") && !strcmp(path, "/v1/apps/catalog")) { if(authorize_read_capability(fd,"device.app.list",caller)) send_app_catalog(fd); }
    else if (!strcmp(method,"POST") && !strcmp(path, "/v1/inspect/app")) { if(authorize_read_capability(fd,"device.app.inspect",caller)) send_app_inspect(fd,body); }
    else if (!strcmp(method,"GET") && !strcmp(path, "/v1/processes")) { if(authorize_read_capability(fd,"device.process.list",caller)) send_text_payload(fd, processes_text(NULL)); }
    else if (!strcmp(method,"GET") && !strcmp(path, "/v1/processes/catalog")) { if(authorize_read_capability(fd,"device.process.list",caller)) send_process_catalog(fd); }
    else if (!strcmp(method,"POST") && !strcmp(path, "/v1/inspect/process")) { if(authorize_read_capability(fd,"device.process.inspect",caller)) send_process_inspect(fd,body); }
    else if (!strcmp(method,"GET") && !strcmp(path, "/v1/permissions/tcc")) { if(authorize_read_capability(fd,"device.permission.tcc",caller)) send_tcc_permissions(fd); }
    else if (!strcmp(method,"GET") && !strcmp(path, "/v1/files")) { if(auth_role!=RT_AUTH_ADMIN)send_error(fd,403,"broad filesystem view is owner-only");else if(authorize_read_capability(fd,"device.fs.observe",caller))send_text_payload(fd, files_text()); }
    else if (!strcmp(method,"GET") && !strcmp(path, "/v1/fs/scopes")) { if(authorize_read_capability(fd,"device.fs.scopes",caller)) send_fs_scopes(fd); }
    else if (!strcmp(method,"POST") && !strcmp(path, "/v1/fs/list")) { if(authorize_read_capability(fd,"device.fs.list",caller)) send_fs_list(fd,body); }
    else if (!strcmp(method,"GET") && !strcmp(path, "/v1/network")) { if(authorize_read_capability(fd,"device.network.observe",caller)) send_text_payload(fd, network_text()); }
    else if (!strcmp(method,"GET") && !strcmp(path, "/v1/network/catalog")) { if(authorize_read_capability(fd,"device.network.observe",caller)) send_network_catalog(fd); }
    else if (!strcmp(method,"GET") && !strcmp(path, "/v1/diagnostics")) { if(authorize_read_capability(fd,"device.diagnostics.observe",caller)) send_text_payload(fd, diagnostics_text()); }
    else if (!strcmp(method,"GET") && !strcmp(path, "/v1/capabilities")) send_text_payload(fd, capabilities_text());
    else if (!strcmp(method,"GET") && !strcmp(path, "/v1/capabilities/catalog")) send_capability_catalog(fd);
    else if (!strcmp(method,"POST") && !strcmp(path, "/v1/capabilities/set")) handle_capability_set(fd,body,auth_role);
    else if (!strcmp(method,"GET") && !strcmp(path, "/v1/audit")) { if(authorize_read_capability(fd,"device.audit.read",caller)) send_text_payload(fd, audit_text()); }
    else if (!strcmp(method,"POST") && !strcmp(path, "/v1/events/replay")) { if(authorize_read_capability(fd,"device.events.read",caller)) send_event_replay(fd,body); }
    else if (!strcmp(method,"POST") && !strcmp(path,"/v1/commands/submit")) route_action_request(fd,body,caller,trusted_confirmation_source);
    else if (!strcmp(method,"POST") && !strcmp(path,"/v1/action")) route_action_request(fd,body,caller,trusted_confirmation_source);
    else if (!strcmp(method,"POST") && !strcmp(path,"/v1/actions/app-launch")) route_action(fd,"app.launch",body,caller,trusted_confirmation_source);
    else if (!strcmp(method,"POST") && !strcmp(path,"/v1/actions/app-terminate")) route_action(fd,"app.terminate",body,caller,trusted_confirmation_source);
    else if (!strcmp(method,"POST") && !strcmp(path,"/v1/actions/process-terminate")) route_action(fd,"process.terminate",body,caller,trusted_confirmation_source);
    else if (!strcmp(method,"POST") && !strcmp(path,"/v1/actions/file-write")) route_action(fd,"file.write",body,caller,trusted_confirmation_source);
    else if (!strcmp(method,"POST") && !strcmp(path,"/v1/actions/file-read")) route_action(fd,"file.read",body,caller,trusted_confirmation_source);
    else send_response(fd, 404, "application/json", "{\"error\":\"not_found\"}");
    free(req);
}

int main(void) {
    setenv("PATH","/var/jb/bin:/var/jb/usr/bin:/var/jb/sbin:/var/jb/usr/sbin:/usr/bin:/bin:/usr/sbin:/sbin",1);
    ensure_action_dirs();
    automation_recover_incomplete_jobs();
    task_recover_incomplete();
    int s = socket(AF_INET, SOCK_STREAM, 0); if (s < 0) return 2;
    fcntl(s,F_SETFD,FD_CLOEXEC);
    int one=1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr={0}; addr.sin_family=AF_INET; addr.sin_port=htons(listen_port()); addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    if (bind(s, (struct sockaddr*)&addr, sizeof(addr)) != 0) return 3;
    if (listen(s, 16) != 0) return 4;
    for (;;) {
        fd_set rfds; FD_ZERO(&rfds); FD_SET(s,&rfds); struct timeval tv={.tv_sec=1,.tv_usec=0};
        int ready=select(s+1,&rfds,NULL,NULL,&tv);
        if(ready>0&&FD_ISSET(s,&rfds)){int c=accept(s,NULL,NULL);if(c>=0){handle(c);close(c);}}
        automation_tick();
        self_update_tick();
    }
}
