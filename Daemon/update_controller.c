#include "update_controller.h"
#include "package_controller.h"

#include <errno.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *update_db_path(void) {
    const char *override=getenv("ROOTTOOLS_UPDATE_DB");
    return override&&override[0]?override:"/var/mobile/Library/RootTools/self-update.sqlite3";
}

static int safe_id(const char *value, size_t max) {
    size_t n=value?strlen(value):0;
    if(!n||n>max)return 0;
    for(size_t i=0;i<n;i++){
        unsigned char c=(unsigned char)value[i];
        if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='.'||c=='_'||c=='-'))return 0;
    }
    return 1;
}

static int db_open(sqlite3 **out) {
    sqlite3 *db=NULL;
    int rc=sqlite3_open_v2(update_db_path(),&db,SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE|SQLITE_OPEN_FULLMUTEX,NULL);
    if(rc!=SQLITE_OK){if(db)sqlite3_close(db);return 0;}
    sqlite3_busy_timeout(db,1500);
    const char *schema=
        "PRAGMA journal_mode=DELETE;PRAGMA synchronous=FULL;"
        "CREATE TABLE IF NOT EXISTS self_updates("
        "request_id TEXT PRIMARY KEY,package_id TEXT NOT NULL,state TEXT NOT NULL,target_version TEXT NOT NULL DEFAULT '',"
        "result TEXT,error TEXT,created_at INTEGER NOT NULL,updated_at INTEGER NOT NULL);"
        "CREATE INDEX IF NOT EXISTS self_updates_state_idx ON self_updates(state,created_at);";
    char *error=NULL;
    rc=sqlite3_exec(db,schema,NULL,NULL,&error);
    sqlite3_free(error);
    if(rc!=SQLITE_OK){sqlite3_close(db);return 0;}
    *out=db;
    return 1;
}

static void operation_init(RTUpdateOperation *op) {
    memset(op,0,sizeof(*op));
    snprintf(op->result,sizeof(op->result),"failed");
}

int rt_update_schedule(const char *request_id, const char *package_id, RTUpdateOperation *op) {
    operation_init(op);
    if(!safe_id(request_id,120)||!safe_id(package_id,80)){
        snprintf(op->message,sizeof(op->message),"Invalid self-update request identifier");return 0;
    }
    RTPackageInfo package;
    if(!rt_package_get(package_id,&package)||strcmp(package.state,"ready")||strcmp(package.format,"deb")||
       strcmp(package.expected_identifier,"com.arthur.roottools")){
        snprintf(op->message,sizeof(op->message),"Self-update requires a ready verified RootTools DEB");return 0;
    }
    sqlite3 *db=NULL;
    if(!db_open(&db)){snprintf(op->message,sizeof(op->message),"Self-update ledger unavailable");return 0;}
    if(sqlite3_exec(db,"BEGIN IMMEDIATE",NULL,NULL,NULL)!=SQLITE_OK){sqlite3_close(db);return 0;}
    sqlite3_stmt *busy=NULL;
    int rc=sqlite3_prepare_v2(db,
        "SELECT COUNT(*) FROM self_updates WHERE state IN ('queued','launching','running')",-1,&busy,NULL);
    if(rc==SQLITE_OK)rc=sqlite3_step(busy);
    int active=(rc==SQLITE_ROW)?sqlite3_column_int(busy,0):1;
    sqlite3_finalize(busy);
    if(active>0){sqlite3_exec(db,"ROLLBACK",NULL,NULL,NULL);sqlite3_close(db);snprintf(op->result,sizeof(op->result),"busy");snprintf(op->message,sizeof(op->message),"Another RootTools self-update is active");return 0;}
    sqlite3_stmt *insert=NULL;
    rc=sqlite3_prepare_v2(db,
        "INSERT INTO self_updates(request_id,package_id,state,created_at,updated_at) VALUES(?1,?2,'queued',?3,?3)",
        -1,&insert,NULL);
    if(rc==SQLITE_OK){
        sqlite3_bind_text(insert,1,request_id,-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(insert,2,package_id,-1,SQLITE_TRANSIENT);
        sqlite3_bind_int64(insert,3,time(NULL));
        rc=sqlite3_step(insert);
    }
    sqlite3_finalize(insert);
    int committed=rc==SQLITE_DONE&&sqlite3_exec(db,"COMMIT",NULL,NULL,NULL)==SQLITE_OK;
    if(!committed)sqlite3_exec(db,"ROLLBACK",NULL,NULL,NULL);
    sqlite3_close(db);
    if(!committed){snprintf(op->message,sizeof(op->message),"Self-update request could not be persisted");return 0;}
    RTUpdateInfo verify;
    op->executed=1;op->post_checked=1;op->post_passed=rt_update_get(request_id,&verify)&&!strcmp(verify.state,"queued");
    op->ok=op->post_passed;
    snprintf(op->result,sizeof(op->result),op->ok?"queued":"failed");
    snprintf(op->message,sizeof(op->message),op->ok?"RootTools self-update queued for independent updater":"Self-update queue verification failed");
    snprintf(op->post_detail,sizeof(op->post_detail),op->ok?"durable updater state is queued":"queued state unavailable");
    if(op->ok)snprintf(op->output,sizeof(op->output),"%s",request_id);
    return op->ok;
}

int rt_update_claim_pending(char *request_id, size_t cap) {
    if(!request_id||cap<2)return 0;
    request_id[0]=0;
    sqlite3 *db=NULL;
    if(!db_open(&db)||sqlite3_exec(db,"BEGIN IMMEDIATE",NULL,NULL,NULL)!=SQLITE_OK){if(db)sqlite3_close(db);return 0;}
    sqlite3_stmt *select=NULL;
    int rc=sqlite3_prepare_v2(db,"SELECT request_id FROM self_updates WHERE state='queued' ORDER BY created_at LIMIT 1",-1,&select,NULL);
    if(rc==SQLITE_OK)rc=sqlite3_step(select);
    if(rc!=SQLITE_ROW){sqlite3_finalize(select);sqlite3_exec(db,"COMMIT",NULL,NULL,NULL);sqlite3_close(db);return 0;}
    const char *id=(const char*)sqlite3_column_text(select,0);
    if(!id||strlen(id)>=cap){sqlite3_finalize(select);sqlite3_exec(db,"ROLLBACK",NULL,NULL,NULL);sqlite3_close(db);return 0;}
    snprintf(request_id,cap,"%s",id);
    sqlite3_finalize(select);
    sqlite3_stmt *update=NULL;
    rc=sqlite3_prepare_v2(db,"UPDATE self_updates SET state='launching',updated_at=?1 WHERE request_id=?2 AND state='queued'",-1,&update,NULL);
    if(rc==SQLITE_OK){sqlite3_bind_int64(update,1,time(NULL));sqlite3_bind_text(update,2,request_id,-1,SQLITE_TRANSIENT);rc=sqlite3_step(update);}
    int changed=sqlite3_changes(db);
    sqlite3_finalize(update);
    int ok=rc==SQLITE_DONE&&changed==1&&sqlite3_exec(db,"COMMIT",NULL,NULL,NULL)==SQLITE_OK;
    if(!ok)sqlite3_exec(db,"ROLLBACK",NULL,NULL,NULL);
    sqlite3_close(db);
    if(!ok)request_id[0]=0;
    return ok;
}

int rt_update_get(const char *request_id, RTUpdateInfo *info) {
    if(!safe_id(request_id,120)||!info)return 0;
    sqlite3 *db=NULL;if(!db_open(&db))return 0;
    sqlite3_stmt *st=NULL;
    int rc=sqlite3_prepare_v2(db,
        "SELECT request_id,package_id,state,target_version,result,error,created_at,updated_at FROM self_updates WHERE request_id=?1",
        -1,&st,NULL);
    if(rc==SQLITE_OK){sqlite3_bind_text(st,1,request_id,-1,SQLITE_TRANSIENT);rc=sqlite3_step(st);}
    int found=rc==SQLITE_ROW;
    if(found){
        memset(info,0,sizeof(*info));
        snprintf(info->request_id,sizeof(info->request_id),"%s",(const char*)sqlite3_column_text(st,0));
        snprintf(info->package_id,sizeof(info->package_id),"%s",(const char*)sqlite3_column_text(st,1));
        snprintf(info->state,sizeof(info->state),"%s",(const char*)sqlite3_column_text(st,2));
        const char *version=(const char*)sqlite3_column_text(st,3);if(version)snprintf(info->target_version,sizeof(info->target_version),"%s",version);
        const char *result=(const char*)sqlite3_column_text(st,4);if(result)snprintf(info->result,sizeof(info->result),"%s",result);
        const char *error=(const char*)sqlite3_column_text(st,5);if(error)snprintf(info->error,sizeof(info->error),"%s",error);
        info->created_at=sqlite3_column_int64(st,6);info->updated_at=sqlite3_column_int64(st,7);
    }
    sqlite3_finalize(st);sqlite3_close(db);return found;
}

int rt_update_mark(const char *request_id, const char *state, const char *target_version,
                   const char *result, const char *error) {
    if(!safe_id(request_id,120)||!state||strlen(state)>31)return 0;
    sqlite3 *db=NULL;if(!db_open(&db))return 0;
    sqlite3_stmt *st=NULL;
    int rc=sqlite3_prepare_v2(db,
        "UPDATE self_updates SET state=?1,target_version=?2,result=?3,error=?4,updated_at=?5 WHERE request_id=?6",
        -1,&st,NULL);
    if(rc==SQLITE_OK){
        sqlite3_bind_text(st,1,state,-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(st,2,target_version?target_version:"",-1,SQLITE_TRANSIENT);
        if(result)sqlite3_bind_text(st,3,result,-1,SQLITE_TRANSIENT);else sqlite3_bind_null(st,3);
        if(error)sqlite3_bind_text(st,4,error,-1,SQLITE_TRANSIENT);else sqlite3_bind_null(st,4);
        sqlite3_bind_int64(st,5,time(NULL));sqlite3_bind_text(st,6,request_id,-1,SQLITE_TRANSIENT);
        rc=sqlite3_step(st);
    }
    int changed=sqlite3_changes(db);sqlite3_finalize(st);sqlite3_close(db);return rc==SQLITE_DONE&&changed==1;
}

static void json_escape(const char *src, char *dst, size_t cap) {
    size_t j=0;
    for(size_t i=0;src&&src[i]&&j+2<cap;i++){
        unsigned char c=(unsigned char)src[i];
        if(c=='"'||c=='\\'){dst[j++]='\\';dst[j++]=(char)c;}
        else if(c=='\n'){dst[j++]='\\';dst[j++]='n';}
        else if(c>=0x20)dst[j++]=(char)c;
    }
    dst[j]=0;
}

char *rt_updates_json(void) {
    sqlite3 *db=NULL;if(!db_open(&db))return NULL;
    sqlite3_stmt *st=NULL;
    int rc=sqlite3_prepare_v2(db,
        "SELECT request_id,package_id,state,target_version,result,error,created_at,updated_at FROM self_updates ORDER BY created_at DESC LIMIT 20",
        -1,&st,NULL);
    if(rc!=SQLITE_OK){sqlite3_close(db);return NULL;}
    char *out=calloc(1,32768);if(!out){sqlite3_finalize(st);sqlite3_close(db);return NULL;}
    size_t used=(size_t)snprintf(out,32768,"{\"schemaVersion\":1,\"updates\":[");int rows=0;
    while(sqlite3_step(st)==SQLITE_ROW){
        char request[256]={0},package[192]={0},state[96]={0},version[128]={0},result[256]={0},error[1024]={0};
        json_escape((const char*)sqlite3_column_text(st,0),request,sizeof(request));json_escape((const char*)sqlite3_column_text(st,1),package,sizeof(package));
        json_escape((const char*)sqlite3_column_text(st,2),state,sizeof(state));json_escape((const char*)sqlite3_column_text(st,3),version,sizeof(version));
        const char *r=(const char*)sqlite3_column_text(st,4),*e=(const char*)sqlite3_column_text(st,5);json_escape(r?r:"",result,sizeof(result));json_escape(e?e:"",error,sizeof(error));
        int n=snprintf(out+used,32768-used,
            "%s{\"requestId\":\"%s\",\"packageId\":\"%s\",\"state\":\"%s\",\"targetVersion\":\"%s\",\"result\":%s%s%s,\"error\":%s%s%s,\"createdAt\":%lld,\"updatedAt\":%lld}",
            rows?",":"",request,package,state,version,r?"\"":"null",r?result:"",r?"\"":"",e?"\"":"null",e?error:"",e?"\"":"",
            (long long)sqlite3_column_int64(st,6),(long long)sqlite3_column_int64(st,7));
        if(n<0||(size_t)n>=32768-used)break;used+=(size_t)n;rows++;
    }
    sqlite3_finalize(st);sqlite3_close(db);snprintf(out+used,32768-used,"],\"count\":%d}",rows);return out;
}
