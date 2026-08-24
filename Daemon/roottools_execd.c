#include <arpa/inet.h>
#include <CommonCrypto/CommonDigest.h>
#include <CoreFoundation/CoreFoundation.h>
#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
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

#define PORT 45821
#define VERSION "0.4.0"
#define SERVICE_SCHEMA_VERSION 1
#define ADMIN_TOKEN "__ROOTTOOLS_TOKEN__"
#define AGENT_TOKEN "__ROOTTOOLS_AGENT_TOKEN__"
#define MAX_REQUEST 65536
#define MAX_ACTION_BODY 24576
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
        "INSERT OR IGNORE INTO service_meta(key,value) VALUES('revision',0);";
    char *error=NULL;
    rc=sqlite3_exec(db,schema,NULL,NULL,&error);
    sqlite3_free(error);
    if(rc!=SQLITE_OK){sqlite3_close(db);return 0;}
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

static int automation_job_count(const char *state) {
    sqlite3 *db=NULL; if(!ledger_open(&db))return -1;
    sqlite3_stmt *statement=NULL;
    int rc=sqlite3_prepare_v2(db,
        state?"SELECT COUNT(*) FROM automation_jobs WHERE state=?1":"SELECT COUNT(*) FROM automation_jobs",
        -1,&statement,NULL);
    if(rc!=SQLITE_OK){sqlite3_close(db);return -1;}
    if(state)sqlite3_bind_text(statement,1,state,-1,SQLITE_TRANSIENT);
    rc=sqlite3_step(statement); int count=-1;
    if(rc==SQLITE_ROW)count=sqlite3_column_int(statement,0);
    sqlite3_finalize(statement); sqlite3_close(db); return count;
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

static int automation_job_state(const char *job_id, char *state, size_t state_cap) {
    sqlite3 *db=NULL; if(!ledger_open(&db))return 0;
    sqlite3_stmt *statement=NULL;
    int rc=sqlite3_prepare_v2(db,"SELECT state FROM automation_jobs WHERE job_id=?1",-1,&statement,NULL);
    if(rc==SQLITE_OK){sqlite3_bind_text(statement,1,job_id,-1,SQLITE_TRANSIENT);rc=sqlite3_step(statement);}
    int ok=0;
    if(rc==SQLITE_ROW){const char *value=(const char*)sqlite3_column_text(statement,0);if(value){snprintf(state,state_cap,"%s",value);ok=1;}}
    sqlite3_finalize(statement); sqlite3_close(db); return ok;
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
    char rid[128]={0}, cid[256]={0}, action[128]={0}, c[128]={0}, t[512]={0}, r[128]={0}, p[128]={0}, m[1024]={0}, pd[1024]={0};
    json_escape(request_id, rid, sizeof(rid));
    json_escape(capability ? capability->id : "unknown", cid, sizeof(cid));
    json_escape(capability && capability->legacy_action ? capability->legacy_action : "unknown", action, sizeof(action));
    json_escape(caller, c, sizeof(c)); json_escape(target, t, sizeof(t)); json_escape(result, r, sizeof(r));
    json_escape(policy, p, sizeof(p)); json_escape(message, m, sizeof(m)); json_escape(post_detail, pd, sizeof(pd));
    fprintf(f,
            "{\"time\":%lld,\"requestId\":\"%s\",\"auditId\":\"%s\",\"capabilityId\":\"%s\",\"action\":\"%s\",\"risk\":\"%s\",\"caller\":\"%s\",\"target\":\"%s\",\"ok\":%s,\"executed\":%s,\"revision\":%llu,\"result\":\"%s\",\"policy\":\"%s\",\"message\":\"%s\",\"postCondition\":{\"checked\":%s,\"passed\":%s,\"detail\":\"%s\"}}\n",
            (long long)time(NULL), rid, audit_id, cid, action, capability ? rt_risk_name(capability->risk) : "R3", c, t,
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
    appendf(out, 65536, "SYSTEM APPS\n"); scan_app_dir(out, 65536, "/Applications", 0);
    appendf(out, 65536, "\nJAILBREAK APPS\n"); scan_app_dir(out, 65536, "/var/jb/Applications", 0);
    appendf(out, 65536, "\nUSER APPS\n"); scan_app_dir(out, 65536, "/var/containers/Bundle/Application", 1);
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

static void send_error(int fd, int code, const char *message) {
    char escaped[1024]={0}, body[1200]={0}; json_escape(message, escaped, sizeof(escaped));
    snprintf(body,sizeof(body),"{\"ok\":false,\"error\":\"%s\"}",escaped); send_response(fd,code,"application/json",body);
}

static int authorize_read_capability(int fd, const char *capability_id) {
    const RTCapability *capability=rt_capability_find(capability_id);
    if(!capability){send_error(fd,500,"read capability missing from registry");return 0;}
    RTPolicyDecision decision=rt_policy_evaluate(capability,0);
    if(!decision.allowed){send_error(fd,403,decision.reason?decision.reason:"read capability denied");return 0;}
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

    char rid[256]={0}, cid[512]={0}, action[256]={0}, caller[256]={0}, target[2048]={0}, result[128]={0}, message[2048]={0}, post_detail[2048]={0};
    json_escape(context->request_id,rid,sizeof(rid)); json_escape(capability->id,cid,sizeof(cid));
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
        "{\"ok\":%s,\"executed\":%s,\"replayed\":false,\"revision\":%llu,\"requestId\":\"%s\",\"auditId\":\"%s\",\"capabilityId\":\"%s\",\"action\":\"%s\",\"risk\":\"%s\",\"caller\":\"%s\",\"target\":\"%s\",\"result\":\"%s\",\"policy\":\"%s\",\"message\":\"%s\",\"postCondition\":{\"checked\":%s,\"passed\":%s,\"detail\":\"%s\"}%s%s%s}",
        execution->ok?"true":"false",execution->executed?"true":"false",revision,rid,audit_id,cid,action,rt_risk_name(capability->risk),caller,target,result,policy,message,
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

static RTAuthRole request_auth_role(const char *req) {
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
            if(constant_time_token_equal(value,value_len,ADMIN_TOKEN)) return RT_AUTH_ADMIN;
            char agent_token[160]={0};
            if(load_runtime_agent_token(agent_token,sizeof(agent_token),NULL)&&agent_token[0]&&
               constant_time_token_equal(value,value_len,agent_token)) return RT_AUTH_AGENT;
            return RT_AUTH_NONE;
        }
        line=end+2;
    }
    return RT_AUTH_NONE;
}

static const char *auth_caller(RTAuthRole role) {
    switch(role){
        case RT_AUTH_ADMIN: return "roottools-ui";
        case RT_AUTH_AGENT: return "trusted-host-agent";
        case RT_AUTH_NONE: break;
    }
    return "unauthenticated";
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
    context->confirmed=trusted_confirmation_source&&requested_confirmation;
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
    size_t display_cap
) {
    CFStringRef path_string=CFStringCreateWithCString(kCFAllocatorDefault,path,kCFStringEncodingUTF8);
    if(!path_string)return 0;
    CFURLRef url=CFURLCreateWithFileSystemPath(kCFAllocatorDefault,path_string,kCFURLPOSIXPathStyle,true);
    CFRelease(path_string); if(!url)return 0;
    CFBundleRef bundle=CFBundleCreate(kCFAllocatorDefault,url); CFRelease(url); if(!bundle)return 0;
    CFStringRef identifier=CFBundleGetIdentifier(bundle);
    CFTypeRef executable_value=CFBundleGetValueForInfoDictionaryKey(bundle,kCFBundleExecutableKey);
    CFTypeRef display_value=CFBundleGetValueForInfoDictionaryKey(bundle,CFSTR("CFBundleDisplayName"));
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
    char bundle_id[256]={0},executable[256]={0},display[512]={0};
    if(!app_metadata(path,bundle_id,sizeof(bundle_id),executable,sizeof(executable),display,sizeof(display)))return 0;
    char ebundle[512]={0},eexec[512]={0},edisplay[1024]={0},epath[2048]={0};
    json_escape(bundle_id,ebundle,sizeof(ebundle)); json_escape(executable,eexec,sizeof(eexec));
    json_escape(display,edisplay,sizeof(edisplay)); json_escape(path,epath,sizeof(epath));
    int n=snprintf(response+*used,cap-*used,
        "%s{\"bundleID\":\"%s\",\"executable\":\"%s\",\"displayName\":\"%s\",\"source\":\"%s\",\"path\":\"%s\",\"running\":%s,\"critical\":%s}",
        row?",":"",ebundle,eexec,edisplay,source,epath,process_name_exists(executable)?"true":"false",critical_process_name(executable)?"true":"false");
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

static void send_app_catalog(int fd) {
    char *response=calloc(1,65536); if(!response){send_error(fd,500,"app catalog allocation failed");return;}
    size_t used=0; int row=0; int n=snprintf(response,65536,"{\"schemaVersion\":1,\"generation\":%d,\"applications\":[",getpid());
    if(n<0){free(response);send_error(fd,500,"app catalog encoding failed");return;} used=(size_t)n;
    scan_app_catalog_root(response,65536,&used,&row,"/Applications","system",0);
    scan_app_catalog_root(response,65536,&used,&row,"/var/jb/Applications","jailbreak",0);
    scan_app_catalog_root(response,65536,&used,&row,"/var/containers/Bundle/Application","user",1);
    n=snprintf(response+used,65536-used,"],\"count\":%d}",row);
    if(n<0||(size_t)n>=65536-used){free(response);send_error(fd,500,"app catalog too large");return;}
    send_response(fd,200,"application/json",response); free(response);
}

static void send_process_inspect(int fd, const char *body) {
    long raw=0;
    if(!json_get_int(body,"pid",&raw)||raw<=0||raw>999999){send_error(fd,400,"valid pid is required");return;}
    uid_t uid=0; char name[128]={0};
    if(!process_info((pid_t)raw,&uid,name,sizeof(name))){send_error(fd,404,"process not found");return;}
    char escaped[256]={0}, response[1024]={0}; json_escape(name,escaped,sizeof(escaped));
    snprintf(response,sizeof(response),
        "{\"ok\":true,\"process\":{\"pid\":%ld,\"uid\":%u,\"command\":\"%s\",\"critical\":%s,\"privileged\":%s}}",
        raw,uid,escaped,critical_process_name(name)?"true":"false",uid==0?"true":"false");
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
    char bundle[256]={0}, executable[256]={0};
    if(!json_get_string(body,"bundleID",bundle,sizeof(bundle))||!safe_bundle_id(bundle)){send_error(fd,400,"valid bundleID is required");return;}
    if(!executable_for_bundle(bundle,executable,sizeof(executable))){send_error(fd,404,"application could not be resolved");return;}
    char escaped_executable[512]={0}; json_escape(executable,escaped_executable,sizeof(escaped_executable));
    char response[1400]={0};
    snprintf(response,sizeof(response),
        "{\"ok\":true,\"application\":{\"bundleID\":\"%s\",\"executable\":\"%s\",\"running\":%s,\"critical\":%s}}",
        bundle,escaped_executable,process_name_exists(executable)?"true":"false",critical_process_name(executable)?"true":"false");
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

static void send_runtime_catalog(int fd) {
    int dopamine=0; char *process_snapshot=processes_text(&dopamine); free(process_snapshot);
    int rootless=access("/var/jb",F_OK)==0;
    int ssh=port_open(22),frida=port_open(27042),zxtouch=port_open(6000);
    unsigned long long revision=0; int revision_available=ledger_current_revision(&revision);
    char response[8192]={0};
    snprintf(response,sizeof(response),
        "{\"schemaVersion\":1,\"generation\":%d,\"revision\":%llu,\"revisionAvailable\":%s,\"privilegeState\":\"%s\",\"dopamineAppProcessRunning\":%s,\"adapters\":["
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
    int pending=automation_job_count("pending");
    int completed=automation_job_count("completed");
    int failed=automation_job_count("failed");
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
        "SELECT job_id,kind,target,state,attempt_count,created_at,updated_at,result,error "
        "FROM automation_jobs ORDER BY created_at DESC LIMIT 100",-1,&statement,NULL);
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

static void send_hello(int fd, RTAuthRole role) {
    char machine[64]={0},osbuild[64]={0};
    sysctl_string("hw.machine",machine,sizeof(machine));
    sysctl_string("kern.osversion",osbuild,sizeof(osbuild));
    int rootless=access("/var/jb",F_OK)==0;
    unsigned long long revision=0; int revision_available=ledger_current_revision(&revision);
    char response[4096]={0};
    snprintf(response,sizeof(response),
        "{\"service\":\"roottools.device-service\",\"schemaVersion\":%d,\"daemonVersion\":\"%s\","
        "\"authenticatedRole\":\"%s\",\"platform\":\"ios\",\"machine\":\"%s\",\"osBuild\":\"%s\","
        "\"privilegeState\":\"%s\",\"generation\":%d,\"revision\":%llu,\"revisionAvailable\":%s,\"capabilityCount\":%zu,"
        "\"features\":{\"typedActions\":true,\"ownerPolicy\":true,\"durableIdempotency\":true,"
        "\"expectedRevision\":true,\"eventAudit\":true,\"runtimeAdapters\":true,\"lockAwareAutomation\":true,\"deferredUIJobs\":true,\"tccReadOnly\":true,\"rawPrivilegedShell\":false}}",
        SERVICE_SCHEMA_VERSION,VERSION,auth_role_name(role),machine,osbuild,
        rootless&&getuid()==0?"jailbreak-root":"degraded",getpid(),revision,revision_available?"true":"false",rt_capability_count());
    send_response(fd,200,"application/json",response);
}

static void send_tcc_permissions(int fd) {
    const char *db_path=getenv("ROOTTOOLS_TCC_DB");
    if(!db_path||!db_path[0])db_path="/var/mobile/Library/TCC/TCC.db";
    sqlite3 *db=NULL;
    int rc=sqlite3_open_v2(db_path,&db,SQLITE_OPEN_READONLY|SQLITE_OPEN_NOMUTEX,NULL);
    if(rc!=SQLITE_OK){
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
    if(rc!=SQLITE_OK){if(db)sqlite3_close(db);send_error(fd,503,"TCC database unavailable");return;}
    const char *sql="SELECT service,client,auth_value,auth_reason,last_modified FROM access ORDER BY service,client LIMIT 512";
    sqlite3_stmt *statement=NULL;
    rc=sqlite3_prepare_v2(db,sql,-1,&statement,NULL);
    if(rc!=SQLITE_OK){sqlite3_close(db);send_error(fd,503,"TCC schema unavailable");return;}

    char *response=calloc(1,65536);
    if(!response){sqlite3_finalize(statement);sqlite3_close(db);send_error(fd,500,"allocation failed");return;}
    size_t used=0; int row=0;
    int n=snprintf(response,65536,"{\"schemaVersion\":1,\"records\":[");
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

static void execute_queue_app_launch(const char *body, const char *job_id, RTActionExecution *execution) {
    char bundle[256]={0};
    if(!json_get_string(body,"bundleID",bundle,sizeof(bundle))||!safe_bundle_id(bundle)){
        snprintf(execution->target,sizeof(execution->target),"invalid bundleID");
        snprintf(execution->message,sizeof(execution->message),"Invalid bundle identifier"); return;
    }
    snprintf(execution->target,sizeof(execution->target),"job=%s bundle=%s",job_id,bundle);
    execution->executed=1;
    if(!automation_enqueue_app_launch(job_id,bundle)){
        snprintf(execution->message,sizeof(execution->message),"Deferred app launch could not be persisted");return;
    }
    char state[64]={0};
    execution->post_checked=1;
    execution->post_passed=automation_job_state(job_id,state,sizeof(state))&&!strcmp(state,"pending");
    snprintf(execution->post_detail,sizeof(execution->post_detail),execution->post_passed?"job is durable and pending":"job persistence verification failed");
    execution->ok=execution->post_passed;
    snprintf(execution->result,sizeof(execution->result),execution->ok?"queued":"failed");
    snprintf(execution->message,sizeof(execution->message),execution->ok?"App launch queued until the device UI is unlocked and visible":"Failed to queue app launch");
    if(execution->ok)execution->output=strdup(job_id);
}

static void execute_automation_cancel(const char *body, RTActionExecution *execution) {
    char job_id[128]={0},state[64]={0};
    if(!json_get_string(body,"jobID",job_id,sizeof(job_id))||!safe_request_id(job_id)){
        snprintf(execution->target,sizeof(execution->target),"invalid jobID");
        snprintf(execution->message,sizeof(execution->message),"Valid jobID is required");return;
    }
    snprintf(execution->target,sizeof(execution->target),"job=%s",job_id);
    if(!automation_job_state(job_id,state,sizeof(state))){snprintf(execution->message,sizeof(execution->message),"Automation job not found");return;}
    if(strcmp(state,"pending")){
        snprintf(execution->result,sizeof(execution->result),"denied");
        snprintf(execution->message,sizeof(execution->message),"Only pending automation jobs can be cancelled");return;
    }
    execution->executed=1;
    int cancelled=automation_cancel_pending(job_id);
    execution->post_checked=1;
    execution->post_passed=cancelled&&automation_job_state(job_id,state,sizeof(state))&&!strcmp(state,"cancelled");
    snprintf(execution->post_detail,sizeof(execution->post_detail),execution->post_passed?"job state is cancelled":"job remained active");
    execution->ok=execution->post_passed;
    snprintf(execution->result,sizeof(execution->result),execution->ok?"success":"failed");
    snprintf(execution->message,sizeof(execution->message),execution->ok?"Pending automation job cancelled":"Automation cancellation failed verification");
}

static int automation_next_pending(char *job_id, size_t job_cap, char *kind, size_t kind_cap, char *target, size_t target_cap, int *attempts) {
    sqlite3 *db=NULL; if(!ledger_open(&db))return 0;
    sqlite3_stmt *statement=NULL;
    int rc=sqlite3_prepare_v2(db,
        "SELECT job_id,kind,target,attempt_count FROM automation_jobs WHERE state='pending' ORDER BY created_at LIMIT 1",
        -1,&statement,NULL);
    if(rc==SQLITE_OK)rc=sqlite3_step(statement);
    int ok=0;
    if(rc==SQLITE_ROW){
        const char *j=(const char*)sqlite3_column_text(statement,0),*k=(const char*)sqlite3_column_text(statement,1),*t=(const char*)sqlite3_column_text(statement,2);
        if(j&&k&&t){snprintf(job_id,job_cap,"%s",j);snprintf(kind,kind_cap,"%s",k);snprintf(target,target_cap,"%s",t);if(attempts)*attempts=sqlite3_column_int(statement,3);ok=1;}
    }
    sqlite3_finalize(statement);sqlite3_close(db);return ok;
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
    if(!ui_execution_ready(snapshot))return;
    char job_id[128]={0},kind[64]={0},target[256]={0};int attempts=0;
    if(!automation_next_pending(job_id,sizeof(job_id),kind,sizeof(kind),target,sizeof(target),&attempts))return;
    if(strcmp(kind,"app.launch")){automation_update_job(job_id,"failed",NULL,"unsupported automation job kind",1);return;}
    if(!safe_bundle_id(target)){automation_update_job(job_id,"failed",NULL,"invalid stored bundle identifier",1);return;}
    automation_update_job(job_id,"running",NULL,NULL,0);
    char executable[256]={0};
    if(!executable_for_bundle(target,executable,sizeof(executable))){
        automation_update_job(job_id,attempts+1>=3?"failed":"pending",NULL,"could not resolve app executable",1);return;
    }
    char *argv[]={(char*)"uiopen",(char*)"--bundleid",target,NULL};
    int rc=fixed_spawn_wait("/var/jb/usr/bin/uiopen",argv,NULL,0);
    int passed=rc==0&&wait_process_name(executable,1);
    if(passed){automation_update_job(job_id,"completed","application process observed",NULL,1);return;}
    automation_update_job(job_id,attempts+1>=3?"failed":"pending",NULL,"launch failed or process was not observed",1);
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
    RTPolicyDecision decision=rt_policy_evaluate(capability,context.confirmed);
    RTActionExecution execution; execution_init(&execution);

    if(!decision.allowed){
        snprintf(execution.target,sizeof(execution.target),"policy");
        snprintf(execution.result,sizeof(execution.result),decision.confirmation_required?"confirmation_required":"denied");
        snprintf(execution.message,sizeof(execution.message),"%s",decision.reason);
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
    else if(legacy_action&&!strcmp(legacy_action,"automation.queue-app-launch")) execute_queue_app_launch(body,context.request_id,&execution);
    else if(legacy_action&&!strcmp(legacy_action,"automation.cancel")) execute_automation_cancel(body,&execution);
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
    RTAuthRole auth_role=request_auth_role(req);
    if (auth_role==RT_AUTH_NONE) { send_response(fd, 401, "application/json", "{\"error\":\"unauthorized\"}"); free(req); return; }
    const char *caller=auth_caller(auth_role);
    int trusted_confirmation_source=auth_role==RT_AUTH_ADMIN;
    char method[16]={0}, path[256]={0}; sscanf(req, "%15s %255s", method, path); const char *body=request_body(req);

    if (!strcmp(path, "/v1/hello") && !strcmp(method,"GET")) {
        send_hello(fd,auth_role); free(req); return;
    }
    if (!strcmp(path, "/v1/status") && !strcmp(method,"GET")) {
        struct utsname u; uname(&u);
        char machine[64]={0}, osbuild[64]={0}; sysctl_string("hw.machine", machine, sizeof(machine)); sysctl_string("kern.osversion", osbuild, sizeof(osbuild));
        int dopamine = 0; char *proc = processes_text(&dopamine); free(proc);
        RTLockSnapshot lock_snapshot=device_lock_snapshot();
        int pending_jobs=automation_job_count("pending");
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
    if (!strcmp(method,"GET") && !strcmp(path, "/v1/runtime")) { if(authorize_read_capability(fd,"device.runtime.observe")) send_text_payload(fd, runtime_text()); }
    else if (!strcmp(method,"GET") && !strcmp(path, "/v1/runtime/catalog")) { if(authorize_read_capability(fd,"device.runtime.adapters")) send_runtime_catalog(fd); }
    else if (!strcmp(method,"GET") && !strcmp(path, "/v1/device/lock-state")) { if(authorize_read_capability(fd,"device.lock.observe")) send_lock_state(fd); }
    else if (!strcmp(method,"GET") && !strcmp(path, "/v1/automation/state")) { if(authorize_read_capability(fd,"device.automation.observe")) send_automation_state(fd); }
    else if (!strcmp(method,"GET") && !strcmp(path, "/v1/automation/queue")) { if(authorize_read_capability(fd,"device.automation.queue.read")) send_automation_queue(fd); }
    else if (!strcmp(method,"GET") && !strcmp(path, "/v1/ui/screen-info")) { if(authorize_read_capability(fd,"device.ui.screen-info")) send_ui_screen_info(fd); }
    else if (!strcmp(method,"GET") && !strcmp(path, "/v1/apps")) { if(authorize_read_capability(fd,"device.app.list")) send_text_payload(fd, apps_text()); }
    else if (!strcmp(method,"GET") && !strcmp(path, "/v1/apps/catalog")) { if(authorize_read_capability(fd,"device.app.list")) send_app_catalog(fd); }
    else if (!strcmp(method,"POST") && !strcmp(path, "/v1/inspect/app")) { if(authorize_read_capability(fd,"device.app.inspect")) send_app_inspect(fd,body); }
    else if (!strcmp(method,"GET") && !strcmp(path, "/v1/processes")) { if(authorize_read_capability(fd,"device.process.list")) send_text_payload(fd, processes_text(NULL)); }
    else if (!strcmp(method,"GET") && !strcmp(path, "/v1/processes/catalog")) { if(authorize_read_capability(fd,"device.process.list")) send_process_catalog(fd); }
    else if (!strcmp(method,"POST") && !strcmp(path, "/v1/inspect/process")) { if(authorize_read_capability(fd,"device.process.inspect")) send_process_inspect(fd,body); }
    else if (!strcmp(method,"GET") && !strcmp(path, "/v1/permissions/tcc")) { if(authorize_read_capability(fd,"device.permission.tcc")) send_tcc_permissions(fd); }
    else if (!strcmp(method,"GET") && !strcmp(path, "/v1/files")) { if(auth_role!=RT_AUTH_ADMIN)send_error(fd,403,"broad filesystem view is owner-only");else if(authorize_read_capability(fd,"device.fs.observe"))send_text_payload(fd, files_text()); }
    else if (!strcmp(method,"GET") && !strcmp(path, "/v1/fs/scopes")) { if(authorize_read_capability(fd,"device.fs.scopes")) send_fs_scopes(fd); }
    else if (!strcmp(method,"POST") && !strcmp(path, "/v1/fs/list")) { if(authorize_read_capability(fd,"device.fs.list")) send_fs_list(fd,body); }
    else if (!strcmp(method,"GET") && !strcmp(path, "/v1/network")) { if(authorize_read_capability(fd,"device.network.observe")) send_text_payload(fd, network_text()); }
    else if (!strcmp(method,"GET") && !strcmp(path, "/v1/network/catalog")) { if(authorize_read_capability(fd,"device.network.observe")) send_network_catalog(fd); }
    else if (!strcmp(method,"GET") && !strcmp(path, "/v1/diagnostics")) { if(authorize_read_capability(fd,"device.diagnostics.observe")) send_text_payload(fd, diagnostics_text()); }
    else if (!strcmp(method,"GET") && !strcmp(path, "/v1/capabilities")) send_text_payload(fd, capabilities_text());
    else if (!strcmp(method,"GET") && !strcmp(path, "/v1/capabilities/catalog")) send_capability_catalog(fd);
    else if (!strcmp(method,"POST") && !strcmp(path, "/v1/capabilities/set")) handle_capability_set(fd,body,auth_role);
    else if (!strcmp(method,"GET") && !strcmp(path, "/v1/audit")) { if(authorize_read_capability(fd,"device.audit.read")) send_text_payload(fd, audit_text()); }
    else if (!strcmp(method,"POST") && !strcmp(path, "/v1/events/replay")) { if(authorize_read_capability(fd,"device.events.read")) send_event_replay(fd,body); }
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
    ensure_action_dirs();
    automation_recover_incomplete_jobs();
    int s = socket(AF_INET, SOCK_STREAM, 0); if (s < 0) return 2;
    int one=1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr={0}; addr.sin_family=AF_INET; addr.sin_port=htons(listen_port()); addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    if (bind(s, (struct sockaddr*)&addr, sizeof(addr)) != 0) return 3;
    if (listen(s, 16) != 0) return 4;
    for (;;) {
        fd_set rfds; FD_ZERO(&rfds); FD_SET(s,&rfds); struct timeval tv={.tv_sec=1,.tv_usec=0};
        int ready=select(s+1,&rfds,NULL,NULL,&tv);
        if(ready>0&&FD_ISSET(s,&rfds)){int c=accept(s,NULL,NULL);if(c>=0){handle(c);close(c);}}
        automation_tick();
    }
}
