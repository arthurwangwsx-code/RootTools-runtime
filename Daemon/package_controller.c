#include "package_controller.h"
#include "provider_registry.h"

#include <CommonCrypto/CommonDigest.h>
#include <CoreFoundation/CoreFoundation.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

extern char **environ;

#define RT_PACKAGE_MAX_SIZE ((long long)2 * 1024 * 1024 * 1024)
#define RT_PACKAGE_MAX_CHUNK (256 * 1024)

static const char *package_root(void) {
    const char *override=getenv("ROOTTOOLS_PACKAGE_ROOT");
    return override&&override[0]?override:"/var/mobile/Library/RootTools/packages";
}

static const char *package_db_path(void) {
    const char *override=getenv("ROOTTOOLS_PACKAGE_DB");
    return override&&override[0]?override:"/var/mobile/Library/RootTools/packages.sqlite3";
}

static int safe_token(const char *value, size_t max) {
    size_t n=value?strlen(value):0;
    if(!n||n>max)return 0;
    for(size_t i=0;i<n;i++){
        if(!(isalnum((unsigned char)value[i])||value[i]=='.'||value[i]=='-'||value[i]=='_'))return 0;
    }
    return strstr(value,"..") == NULL;
}

static int safe_package_name(const char *value) {
    size_t n=value?strlen(value):0;
    if(!n||n>180||!strcmp(value,".")||!strcmp(value,".."))return 0;
    for(size_t i=0;i<n;i++){
        unsigned char c=(unsigned char)value[i];
        if(c<0x20||c=='/'||c=='\\')return 0;
    }
    return 1;
}

static int valid_hash(const char *value) {
    if(!value||strlen(value)!=64)return 0;
    for(size_t i=0;i<64;i++)if(!isxdigit((unsigned char)value[i]))return 0;
    return 1;
}

static int format_matches_name(const char *format, const char *name) {
    const char *dot=strrchr(name,'.');
    if(!dot)return 0;
    if(!strcmp(format,"deb"))return !strcasecmp(dot,".deb");
    if(!strcmp(format,"ipa"))return !strcasecmp(dot,".ipa");
    if(!strcmp(format,"tipa"))return !strcasecmp(dot,".tipa");
    return 0;
}

static int valid_identifier(const char *format, const char *identifier) {
    if(!identifier||strlen(identifier)>=RT_PACKAGE_IDENTIFIER_CAP)return 0;
    if(!identifier[0])return 1;
    for(const char *p=identifier;*p;p++){
        if(!(isalnum((unsigned char)*p)||*p=='.'||*p=='-'||*p=='_'||(!strcmp(format,"deb")&&*p=='+')))return 0;
    }
    return 1;
}

static void operation_init(RTPackageOperation *op) {
    memset(op,0,sizeof(*op));
    snprintf(op->result,sizeof(op->result),"failed");
}

static int ensure_root(void) {
    const char *root=package_root();
    if(mkdir(root,0700)!=0&&errno!=EEXIST)return 0;
    chmod(root,0700);
    return 1;
}

static int package_path(const char *package_id, const char *format, char *out, size_t cap) {
    if(!safe_token(package_id,80)||!format||
       (strcmp(format,"deb")&&strcmp(format,"ipa")&&strcmp(format,"tipa")))return 0;
    int n=snprintf(out,cap,"%s/%s.%s",package_root(),package_id,format);
    return n>0&&(size_t)n<cap;
}

static int db_open(sqlite3 **out) {
    sqlite3 *db=NULL;
    int rc=sqlite3_open_v2(package_db_path(),&db,SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE|SQLITE_OPEN_FULLMUTEX,NULL);
    if(rc!=SQLITE_OK){if(db)sqlite3_close(db);return 0;}
    sqlite3_busy_timeout(db,1500);
    const char *schema=
        "PRAGMA journal_mode=DELETE;PRAGMA synchronous=FULL;"
        "CREATE TABLE IF NOT EXISTS staged_packages("
        "package_id TEXT PRIMARY KEY,name TEXT NOT NULL,format TEXT NOT NULL,expected_identifier TEXT NOT NULL,"
        "total_size INTEGER NOT NULL,received_size INTEGER NOT NULL,sha256 TEXT NOT NULL,state TEXT NOT NULL,"
        "created_at INTEGER NOT NULL,updated_at INTEGER NOT NULL,result TEXT,error TEXT);";
    char *error=NULL;
    rc=sqlite3_exec(db,schema,NULL,NULL,&error);
    sqlite3_free(error);
    if(rc!=SQLITE_OK){sqlite3_close(db);return 0;}
    *out=db;
    return 1;
}

static int load_info_db(sqlite3 *db, const char *package_id, RTPackageInfo *info) {
    sqlite3_stmt *st=NULL;
    int rc=sqlite3_prepare_v2(db,
        "SELECT package_id,name,format,expected_identifier,total_size,received_size,sha256,state FROM staged_packages WHERE package_id=?1",
        -1,&st,NULL);
    if(rc==SQLITE_OK){sqlite3_bind_text(st,1,package_id,-1,SQLITE_TRANSIENT);rc=sqlite3_step(st);}
    if(rc==SQLITE_ROW&&info){
        memset(info,0,sizeof(*info));
        snprintf(info->package_id,sizeof(info->package_id),"%s",(const char*)sqlite3_column_text(st,0));
        snprintf(info->name,sizeof(info->name),"%s",(const char*)sqlite3_column_text(st,1));
        snprintf(info->format,sizeof(info->format),"%s",(const char*)sqlite3_column_text(st,2));
        snprintf(info->expected_identifier,sizeof(info->expected_identifier),"%s",(const char*)sqlite3_column_text(st,3));
        info->total_size=sqlite3_column_int64(st,4);
        info->received_size=sqlite3_column_int64(st,5);
        snprintf(info->sha256,sizeof(info->sha256),"%s",(const char*)sqlite3_column_text(st,6));
        snprintf(info->state,sizeof(info->state),"%s",(const char*)sqlite3_column_text(st,7));
    }
    int found=rc==SQLITE_ROW;
    sqlite3_finalize(st);
    return found;
}

int rt_package_get(const char *package_id, RTPackageInfo *info) {
    sqlite3 *db=NULL;
    if(!safe_token(package_id,80)||!db_open(&db))return 0;
    int ok=load_info_db(db,package_id,info);
    sqlite3_close(db);
    return ok;
}

static int update_state(const char *id, const char *state, const char *result, const char *error) {
    sqlite3 *db=NULL;
    if(!db_open(&db))return 0;
    sqlite3_stmt *st=NULL;
    int rc=sqlite3_prepare_v2(db,
        "UPDATE staged_packages SET state=?1,updated_at=?2,result=?3,error=?4 WHERE package_id=?5",
        -1,&st,NULL);
    if(rc==SQLITE_OK){
        sqlite3_bind_text(st,1,state,-1,SQLITE_TRANSIENT);
        sqlite3_bind_int64(st,2,time(NULL));
        if(result)sqlite3_bind_text(st,3,result,-1,SQLITE_TRANSIENT);else sqlite3_bind_null(st,3);
        if(error)sqlite3_bind_text(st,4,error,-1,SQLITE_TRANSIENT);else sqlite3_bind_null(st,4);
        sqlite3_bind_text(st,5,id,-1,SQLITE_TRANSIENT);
        rc=sqlite3_step(st);
    }
    int changed=sqlite3_changes(db);
    sqlite3_finalize(st);
    sqlite3_close(db);
    return rc==SQLITE_DONE&&changed==1;
}

static int update_identifier(const char *id, const char *identifier) {
    sqlite3 *db=NULL;
    if(!db_open(&db))return 0;
    sqlite3_stmt *st=NULL;
    int rc=sqlite3_prepare_v2(db,"UPDATE staged_packages SET expected_identifier=?1,updated_at=?2 WHERE package_id=?3",-1,&st,NULL);
    if(rc==SQLITE_OK){
        sqlite3_bind_text(st,1,identifier,-1,SQLITE_TRANSIENT);
        sqlite3_bind_int64(st,2,time(NULL));
        sqlite3_bind_text(st,3,id,-1,SQLITE_TRANSIENT);
        rc=sqlite3_step(st);
    }
    int changed=sqlite3_changes(db);
    sqlite3_finalize(st);
    sqlite3_close(db);
    return rc==SQLITE_DONE&&changed==1;
}

int rt_package_begin(const char *package_id, const char *name, const char *format,
                     const char *expected_identifier, long long total_size,
                     const char *sha256, RTPackageOperation *op) {
    operation_init(op);
    if(!safe_token(package_id,80)||!safe_package_name(name)||!format_matches_name(format,name)||
       !valid_identifier(format,expected_identifier)||total_size<=0||total_size>RT_PACKAGE_MAX_SIZE||!valid_hash(sha256)){
        snprintf(op->message,sizeof(op->message),"Invalid package staging metadata");
        return 0;
    }
    if(!ensure_root()){
        snprintf(op->message,sizeof(op->message),"Package staging root unavailable");
        return 0;
    }
    char path[1024]={0};
    if(!package_path(package_id,format,path,sizeof(path)))return 0;
    int fd=open(path,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);
    if(fd<0){
        snprintf(op->message,sizeof(op->message),"Package ID already exists or staging file cannot be created");
        return 0;
    }
    close(fd);
    sqlite3 *db=NULL;
    if(!db_open(&db)){
        unlink(path);
        snprintf(op->message,sizeof(op->message),"Package database unavailable");
        return 0;
    }
    sqlite3_stmt *st=NULL;
    int rc=sqlite3_prepare_v2(db,
        "INSERT INTO staged_packages(package_id,name,format,expected_identifier,total_size,received_size,sha256,state,created_at,updated_at) "
        "VALUES(?1,?2,?3,?4,?5,0,lower(?6),'uploading',?7,?7)",-1,&st,NULL);
    if(rc==SQLITE_OK){
        sqlite3_bind_text(st,1,package_id,-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(st,2,name,-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(st,3,format,-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(st,4,expected_identifier,-1,SQLITE_TRANSIENT);
        sqlite3_bind_int64(st,5,total_size);
        sqlite3_bind_text(st,6,sha256,-1,SQLITE_TRANSIENT);
        sqlite3_bind_int64(st,7,time(NULL));
        rc=sqlite3_step(st);
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    if(rc!=SQLITE_DONE){
        unlink(path);
        snprintf(op->message,sizeof(op->message),"Package staging metadata could not be persisted");
        return 0;
    }
    op->ok=1;op->executed=1;op->post_checked=1;op->post_passed=access(path,F_OK)==0;
    snprintf(op->result,sizeof(op->result),"staging");
    snprintf(op->message,sizeof(op->message),"Package staging initialized");
    snprintf(op->post_detail,sizeof(op->post_detail),"root-owned package slot created");
    snprintf(op->output,sizeof(op->output),"%s",package_id);
    return 1;
}

int rt_package_append(const char *package_id, long long offset,
                      const unsigned char *bytes, size_t length,
                      RTPackageOperation *op) {
    operation_init(op);
    if(!bytes||!length||length>RT_PACKAGE_MAX_CHUNK){snprintf(op->message,sizeof(op->message),"Invalid package chunk");return 0;}
    RTPackageInfo info;
    if(!rt_package_get(package_id,&info)||strcmp(info.state,"uploading")){snprintf(op->message,sizeof(op->message),"Package is not accepting chunks");return 0;}
    if(offset!=info.received_size||offset<0||offset+(long long)length>info.total_size){snprintf(op->message,sizeof(op->message),"Package chunk offset is not sequential");return 0;}
    char path[1024]={0};
    if(!package_path(package_id,info.format,path,sizeof(path)))return 0;
    int fd=open(path,O_WRONLY|O_NOFOLLOW);
    if(fd<0)return 0;
    ssize_t n=pwrite(fd,bytes,length,(off_t)offset);
    int sync_ok=fsync(fd)==0;
    close(fd);
    if(n!=(ssize_t)length||!sync_ok){snprintf(op->message,sizeof(op->message),"Package chunk write failed");return 0;}
    sqlite3 *db=NULL;
    if(!db_open(&db))return 0;
    sqlite3_stmt *st=NULL;
    int rc=sqlite3_prepare_v2(db,
        "UPDATE staged_packages SET received_size=?1,updated_at=?2 WHERE package_id=?3 AND received_size=?4 AND state='uploading'",
        -1,&st,NULL);
    if(rc==SQLITE_OK){
        sqlite3_bind_int64(st,1,offset+(long long)length);
        sqlite3_bind_int64(st,2,time(NULL));
        sqlite3_bind_text(st,3,package_id,-1,SQLITE_TRANSIENT);
        sqlite3_bind_int64(st,4,offset);
        rc=sqlite3_step(st);
    }
    int changed=sqlite3_changes(db);
    sqlite3_finalize(st);
    sqlite3_close(db);
    if(rc!=SQLITE_DONE||changed!=1){snprintf(op->message,sizeof(op->message),"Package chunk ledger update failed");return 0;}
    op->ok=1;op->executed=1;op->post_checked=1;op->post_passed=1;
    snprintf(op->result,sizeof(op->result),"staging");
    snprintf(op->message,sizeof(op->message),"Accepted %zu package bytes",length);
    snprintf(op->post_detail,sizeof(op->post_detail),"received=%lld",offset+(long long)length);
    return 1;
}

static int sha256_file(const char *path, char out[65]) {
    int fd=open(path,O_RDONLY|O_NOFOLLOW);
    if(fd<0)return 0;
    CC_SHA256_CTX ctx;
    CC_SHA256_Init(&ctx);
    unsigned char buffer[65536];
    ssize_t n=0;
    while((n=read(fd,buffer,sizeof(buffer)))>0)CC_SHA256_Update(&ctx,buffer,(CC_LONG)n);
    close(fd);
    if(n<0)return 0;
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_Final(digest,&ctx);
    for(int i=0;i<CC_SHA256_DIGEST_LENGTH;i++)snprintf(out+i*2,3,"%02x",digest[i]);
    out[64]=0;
    return 1;
}

static int inspect_package_identifier(const char *format, const char *path, char *out, size_t cap);

int rt_package_commit(const char *package_id, RTPackageOperation *op) {
    operation_init(op);
    RTPackageInfo info;
    if(!rt_package_get(package_id,&info)||strcmp(info.state,"uploading")){snprintf(op->message,sizeof(op->message),"Package is not an upload-complete candidate");return 0;}
    if(info.received_size!=info.total_size){snprintf(op->message,sizeof(op->message),"Package upload is incomplete");return 0;}
    char path[1024]={0},hash[65]={0};
    if(!package_path(package_id,info.format,path,sizeof(path))||!sha256_file(path,hash)){snprintf(op->message,sizeof(op->message),"Package hash could not be computed");return 0;}
    op->executed=1;op->post_checked=1;op->post_passed=!strcasecmp(hash,info.sha256);
    if(!op->post_passed){
        update_state(package_id,"failed",NULL,"sha256 mismatch");
        snprintf(op->result,sizeof(op->result),"hash_mismatch");
        snprintf(op->message,sizeof(op->message),"Package SHA-256 mismatch");
        snprintf(op->post_detail,sizeof(op->post_detail),"sha256 mismatch");
        return 0;
    }
    char detected_identifier[RT_PACKAGE_IDENTIFIER_CAP]={0};
    int metadata_available=inspect_package_identifier(info.format,path,detected_identifier,sizeof(detected_identifier));
    if(metadata_available){
        if(info.expected_identifier[0]&&strcmp(info.expected_identifier,detected_identifier)){
            update_state(package_id,"failed",NULL,"package identifier mismatch");
            snprintf(op->result,sizeof(op->result),"metadata_mismatch");
            snprintf(op->message,sizeof(op->message),"Package identifier does not match staged metadata");
            snprintf(op->post_detail,sizeof(op->post_detail),"detected identifier=%s",detected_identifier);
            return 0;
        }
        if(!info.expected_identifier[0]&&!update_identifier(package_id,detected_identifier)){
            snprintf(op->message,sizeof(op->message),"Detected package identifier could not be persisted");
            return 0;
        }
    } else if(!info.expected_identifier[0]){
        update_state(package_id,"failed",NULL,"package metadata unavailable");
        snprintf(op->result,sizeof(op->result),"metadata_unavailable");
        snprintf(op->message,sizeof(op->message),"Package identifier could not be inspected and no expected identifier was supplied");
        snprintf(op->post_detail,sizeof(op->post_detail),"hash verified but package metadata is unavailable");
        return 0;
    }
    if(!update_state(package_id,"ready","verified",NULL)){snprintf(op->message,sizeof(op->message),"Package ready state could not be persisted");return 0;}
    op->ok=1;
    snprintf(op->result,sizeof(op->result),"ready");
    snprintf(op->message,sizeof(op->message),"Package upload verified and ready");
    if(metadata_available)snprintf(op->post_detail,sizeof(op->post_detail),"sha256=%s identifier=%s",hash,detected_identifier);
    else snprintf(op->post_detail,sizeof(op->post_detail),"sha256=%s identifier=caller-attested",hash);
    snprintf(op->output,sizeof(op->output),"%s",package_id);
    return 1;
}

int rt_package_discard(const char *package_id, RTPackageOperation *op) {
    operation_init(op);
    RTPackageInfo info;
    if(!rt_package_get(package_id,&info)){snprintf(op->message,sizeof(op->message),"Package not found");return 0;}
    if(!strcmp(info.state,"installed")){snprintf(op->message,sizeof(op->message),"Installed package record cannot be discarded");return 0;}
    char path[1024]={0};
    if(!package_path(package_id,info.format,path,sizeof(path)))return 0;
    int rc=unlink(path);
    if(rc!=0&&errno!=ENOENT){snprintf(op->message,sizeof(op->message),"Package file could not be removed");return 0;}
    if(!update_state(package_id,"discarded","discarded",NULL)){snprintf(op->message,sizeof(op->message),"Discard state could not be persisted");return 0;}
    op->ok=1;op->executed=1;op->post_checked=1;op->post_passed=access(path,F_OK)!=0;
    snprintf(op->result,sizeof(op->result),"discarded");
    snprintf(op->message,sizeof(op->message),"Package staging discarded");
    snprintf(op->post_detail,sizeof(op->post_detail),"staged package file absent");
    return 1;
}

static int spawn_capture_bytes_timeout(const char *program, char *const argv[], unsigned char *out, size_t cap, size_t *out_length, int timeout_seconds) {
    int pipefd[2]={-1,-1};
    if(pipe(pipefd)!=0)return errno;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions,pipefd[1],STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions,pipefd[1],STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions,pipefd[0]);
    pid_t pid=0;
    int rc=posix_spawn(&pid,program,&actions,NULL,argv,environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pipefd[1]);
    if(rc!=0){close(pipefd[0]);return rc;}
    int flags=fcntl(pipefd[0],F_GETFL,0);
    if(flags>=0)fcntl(pipefd[0],F_SETFL,flags|O_NONBLOCK);
    size_t used=0;
    unsigned char buffer[4096];
    int status=0;
    int timed_out=0;
    time_t deadline=time(NULL)+(timeout_seconds>0?timeout_seconds:15);
    for(;;){
        for(;;){
            ssize_t n=read(pipefd[0],buffer,sizeof(buffer));
            if(n>0){
                if(out&&used<cap){
                    size_t copy=(size_t)n;
                    if(copy>cap-used)copy=cap-used;
                    memcpy(out+used,buffer,copy);
                    used+=copy;
                }
                continue;
            }
            if(n<0&&errno==EINTR)continue;
            break;
        }
        pid_t waited=waitpid(pid,&status,WNOHANG);
        if(waited==pid)break;
        if(waited<0){int saved=errno;kill(pid,SIGKILL);waitpid(pid,NULL,0);close(pipefd[0]);return saved;}
        if(time(NULL)>=deadline){
            kill(pid,SIGKILL);
            waitpid(pid,&status,0);
            timed_out=1;
            break;
        }
        usleep(50000);
    }
    for(;;){
        ssize_t n=read(pipefd[0],buffer,sizeof(buffer));
        if(n<=0)break;
        if(out&&used<cap){size_t copy=(size_t)n;if(copy>cap-used)copy=cap-used;memcpy(out+used,buffer,copy);used+=copy;}
    }
    close(pipefd[0]);
    if(out_length)*out_length=used;
    if(timed_out)return ETIMEDOUT;
    return WIFEXITED(status)?WEXITSTATUS(status):128;
}

static int spawn_capture_timeout(const char *program, char *const argv[], char *out, size_t cap, int timeout_seconds) {
    if(!out||cap<2)return EINVAL;
    size_t used=0;
    int rc=spawn_capture_bytes_timeout(program,argv,(unsigned char*)out,cap-1,&used,timeout_seconds);
    out[used]=0;
    return rc;
}

static int spawn_capture(const char *program, char *const argv[], char *out, size_t cap) {
    return spawn_capture_timeout(program,argv,out,cap,15);
}

static int inspect_deb_identifier(const char *path, char *out, size_t cap) {
    const char *tool="/var/jb/usr/bin/dpkg-deb";
    if(access(tool,X_OK)!=0)return 0;
    char output[2048]={0};
    char *argv[]={(char*)"dpkg-deb",(char*)"-f",(char*)path,(char*)"Package",NULL};
    int rc=spawn_capture(tool,argv,output,sizeof(output));
    if(rc!=0)return 0;
    output[strcspn(output,"\r\n")]=0;
    if(!valid_identifier("deb",output)||!output[0]||strlen(output)>=cap)return 0;
    snprintf(out,cap,"%s",output);
    return 1;
}

static unsigned short zip_le16(const unsigned char *p) {
    return (unsigned short)((unsigned short)p[0]|((unsigned short)p[1]<<8));
}

static unsigned int zip_le32(const unsigned char *p) {
    return (unsigned int)p[0]|((unsigned int)p[1]<<8)|((unsigned int)p[2]<<16)|((unsigned int)p[3]<<24);
}

static int pread_exact(int fd, off_t offset, unsigned char *out, size_t length) {
    size_t used=0;
    while(used<length){
        ssize_t n=pread(fd,out+used,length-used,offset+(off_t)used);
        if(n<=0)return 0;
        used+=(size_t)n;
    }
    return 1;
}

static int top_level_ipa_info_plist(const char *line) {
    const char *prefix="Payload/",*suffix="/Info.plist";
    size_t line_len=strlen(line),prefix_len=strlen(prefix),suffix_len=strlen(suffix);
    if(line_len<=prefix_len+suffix_len||strncmp(line,prefix,prefix_len)||strcmp(line+line_len-suffix_len,suffix))return 0;
    const char *relative=line+prefix_len;
    const char *slash=strchr(relative,'/');
    if(!slash||slash!=line+line_len-suffix_len)return 0;
    size_t app_len=(size_t)(slash-relative);
    return app_len>4&&!strncmp(slash-4,".app",4);
}

static int inflate_zip_entry(const unsigned char *compressed, size_t compressed_size,
                             unsigned int method, unsigned char *out, size_t out_size) {
    if(method==0){
        if(compressed_size!=out_size)return 0;
        memcpy(out,compressed,out_size);
        return 1;
    }
    if(method!=8)return 0;
    z_stream stream;
    memset(&stream,0,sizeof(stream));
    if(inflateInit2(&stream,-MAX_WBITS)!=Z_OK)return 0;
    stream.next_in=(Bytef*)compressed;
    stream.avail_in=(uInt)compressed_size;
    stream.next_out=(Bytef*)out;
    stream.avail_out=(uInt)out_size;
    int rc=inflate(&stream,Z_FINISH);
    int ok=rc==Z_STREAM_END&&stream.total_out==out_size;
    inflateEnd(&stream);
    return ok;
}

static int inspect_ipa_identifier(const char *path, char *out, size_t cap) {
    int fd=open(path,O_RDONLY|O_NOFOLLOW);
    if(fd<0)return 0;
    struct stat st;
    if(fstat(fd,&st)!=0||st.st_size<22){close(fd);return 0;}
    size_t tail_size=(size_t)st.st_size;
    if(tail_size>65557)tail_size=65557;
    unsigned char *tail=malloc(tail_size);
    if(!tail){close(fd);return 0;}
    off_t tail_offset=st.st_size-(off_t)tail_size;
    if(!pread_exact(fd,tail_offset,tail,tail_size)){free(tail);close(fd);return 0;}
    ssize_t eocd=-1;
    for(ssize_t i=(ssize_t)tail_size-22;i>=0;i--){
        if(zip_le32(tail+(size_t)i)==0x06054b50){
            unsigned short comment=zip_le16(tail+(size_t)i+20);
            if((size_t)i+22+(size_t)comment==tail_size){eocd=i;break;}
        }
    }
    if(eocd<0){free(tail);close(fd);return 0;}
    unsigned short entries=zip_le16(tail+(size_t)eocd+10);
    unsigned int cd_size=zip_le32(tail+(size_t)eocd+12);
    unsigned int cd_offset=zip_le32(tail+(size_t)eocd+16);
    free(tail);
    if(entries==0xffff||cd_size==0xffffffffU||cd_offset==0xffffffffU||
       cd_size>16*1024*1024||(unsigned long long)cd_offset+cd_size>(unsigned long long)st.st_size){close(fd);return 0;}
    unsigned char *central=malloc(cd_size?cd_size:1);
    if(!central||!pread_exact(fd,(off_t)cd_offset,central,cd_size)){free(central);close(fd);return 0;}

    unsigned int found_method=0,found_compressed=0,found_uncompressed=0,found_local_offset=0;
    int found=0;
    size_t position=0;
    for(unsigned int row=0;row<entries;row++){
        if(position+46>cd_size||zip_le32(central+position)!=0x02014b50){free(central);close(fd);return 0;}
        unsigned short method=zip_le16(central+position+10);
        unsigned int compressed_size=zip_le32(central+position+20);
        unsigned int uncompressed_size=zip_le32(central+position+24);
        unsigned short name_length=zip_le16(central+position+28);
        unsigned short extra_length=zip_le16(central+position+30);
        unsigned short comment_length=zip_le16(central+position+32);
        unsigned int local_offset=zip_le32(central+position+42);
        size_t row_size=46+(size_t)name_length+(size_t)extra_length+(size_t)comment_length;
        if(position+row_size>cd_size||name_length==0||name_length>=1024){free(central);close(fd);return 0;}
        char name[1024];
        memcpy(name,central+position+46,name_length);
        name[name_length]=0;
        if(top_level_ipa_info_plist(name)){
            if(found){free(central);close(fd);return 0;}
            found=1;
            found_method=method;
            found_compressed=compressed_size;
            found_uncompressed=uncompressed_size;
            found_local_offset=local_offset;
        }
        position+=row_size;
    }
    free(central);
    if(!found||found_uncompressed==0||found_uncompressed>4*1024*1024||found_compressed>4*1024*1024){close(fd);return 0;}
    unsigned char local[30];
    if(!pread_exact(fd,(off_t)found_local_offset,local,sizeof(local))||zip_le32(local)!=0x04034b50){close(fd);return 0;}
    unsigned short local_name_length=zip_le16(local+26);
    unsigned short local_extra_length=zip_le16(local+28);
    unsigned long long data_offset=(unsigned long long)found_local_offset+30ULL+local_name_length+local_extra_length;
    if(data_offset+found_compressed>(unsigned long long)st.st_size){close(fd);return 0;}
    unsigned char *compressed=malloc(found_compressed?found_compressed:1);
    unsigned char *plist_bytes=malloc(found_uncompressed);
    if(!compressed||!plist_bytes||!pread_exact(fd,(off_t)data_offset,compressed,found_compressed)){
        free(compressed);free(plist_bytes);close(fd);return 0;
    }
    close(fd);
    int inflated=inflate_zip_entry(compressed,found_compressed,found_method,plist_bytes,found_uncompressed);
    free(compressed);
    if(!inflated){free(plist_bytes);return 0;}
    size_t plist_length=found_uncompressed;
    CFDataRef data=CFDataCreate(kCFAllocatorDefault,plist_bytes,(CFIndex)plist_length);
    free(plist_bytes);
    if(!data)return 0;
    CFErrorRef error=NULL;
    CFPropertyListRef plist=CFPropertyListCreateWithData(kCFAllocatorDefault,data,kCFPropertyListImmutable,NULL,&error);
    CFRelease(data);
    if(error)CFRelease(error);
    if(!plist||CFGetTypeID(plist)!=CFDictionaryGetTypeID()){if(plist)CFRelease(plist);return 0;}
    CFStringRef value=(CFStringRef)CFDictionaryGetValue((CFDictionaryRef)plist,CFSTR("CFBundleIdentifier"));
    int ok=value&&CFGetTypeID(value)==CFStringGetTypeID()&&CFStringGetCString(value,out,(CFIndex)cap,kCFStringEncodingUTF8)&&valid_identifier("ipa",out)&&out[0];
    CFRelease(plist);
    return ok;
}

static int inspect_package_identifier(const char *format, const char *path, char *out, size_t cap) {
    if(!strcmp(format,"deb"))return inspect_deb_identifier(path,out,cap);
    if(!strcmp(format,"ipa")||!strcmp(format,"tipa"))return inspect_ipa_identifier(path,out,cap);
    return 0;
}

static int verify_deb_identifier(const char *path, const char *expected) {
    char detected[RT_PACKAGE_IDENTIFIER_CAP]={0};
    return inspect_deb_identifier(path,detected,sizeof(detected))&&!strcmp(detected,expected);
}

static int verify_deb_installed(const char *identifier) {
    const char *tool="/var/jb/usr/bin/dpkg-query";
    if(access(tool,X_OK)!=0)return 0;
    char output[2048]={0};
    char *argv[]={(char*)"dpkg-query",(char*)"-W",(char*)"-f=${Status}",(char*)identifier,NULL};
    int rc=spawn_capture(tool,argv,output,sizeof(output));
    return rc==0&&strstr(output,"install ok installed")!=NULL;
}

static int verify_app_installed(const char *bundle_id) {
    const char *tool="/var/jb/usr/bin/uicache";
    if(access(tool,X_OK)!=0)return 0;
    char output[8192]={0};
    char *argv[]={(char*)"uicache",(char*)"-i",(char*)bundle_id,NULL};
    int rc=spawn_capture(tool,argv,output,sizeof(output));
    return rc==0&&strstr(output,"Executable Name:")!=NULL;
}

int rt_package_install_deb(const char *package_id, RTPackageOperation *op) {
    operation_init(op);
    RTPackageInfo info;
    if(!rt_package_get(package_id,&info)||strcmp(info.state,"ready")||strcmp(info.format,"deb")){
        snprintf(op->message,sizeof(op->message),"DEB package is not ready");
        return 0;
    }
    char path[1024]={0};
    if(!package_path(package_id,info.format,path,sizeof(path))||!verify_deb_identifier(path,info.expected_identifier)){
        snprintf(op->result,sizeof(op->result),"metadata_mismatch");
        snprintf(op->message,sizeof(op->message),"DEB package identifier does not match staged metadata");
        return 0;
    }
    char dpkg[1024]={0};
    if(!rt_provider_resolve_executable("bootstrap.procursus",dpkg,sizeof(dpkg))){
        snprintf(op->result,sizeof(op->result),"provider_unavailable");
        snprintf(op->message,sizeof(op->message),"Procursus dpkg provider unavailable");
        return 0;
    }
    char output[8192]={0};
    op->executed=1;
    const char *apt_get="/var/jb/usr/bin/apt-get";
    int used_apt=access(apt_get,X_OK)==0;
    int rc=0;
    if(used_apt){
        char *argv[]={(char*)"apt-get",(char*)"install",(char*)"-y",(char*)"--no-remove",path,NULL};
        rc=spawn_capture_timeout(apt_get,argv,output,sizeof(output),180);
    }else{
        char *argv[]={(char*)"dpkg",(char*)"-i",path,NULL};
        rc=spawn_capture_timeout(dpkg,argv,output,sizeof(output),180);
    }
    op->post_checked=1;
    op->post_passed=rc==0&&verify_deb_installed(info.expected_identifier);
    op->ok=op->post_passed;
    snprintf(op->result,sizeof(op->result),op->ok?"success":"failed");
    snprintf(op->message,sizeof(op->message),op->ok?"DEB package installed and verified":"DEB installation failed or post-condition did not pass");
    if(op->post_passed)snprintf(op->post_detail,sizeof(op->post_detail),"%s completed; dpkg-query reports install ok installed",used_apt?"apt-get":"dpkg");
    else snprintf(op->post_detail,sizeof(op->post_detail),"%s exit=%d or package status mismatch",used_apt?"apt-get":"dpkg",rc);
    update_state(package_id,op->ok?"installed":"failed",op->ok?"installed":NULL,op->ok?NULL:"dpkg install/post-condition failed");
    return op->ok;
}

int rt_package_install_ipa(const char *package_id, RTPackageOperation *op) {
    operation_init(op);
    RTPackageInfo info;
    if(!rt_package_get(package_id,&info)||strcmp(info.state,"ready")||(strcmp(info.format,"ipa")&&strcmp(info.format,"tipa"))){
        snprintf(op->message,sizeof(op->message),"IPA/TIPA package is not ready");
        return 0;
    }
    char path[1024]={0},helper[1024]={0};
    if(!package_path(package_id,info.format,path,sizeof(path)))return 0;
    if(!rt_provider_resolve_executable("package.trollstore",helper,sizeof(helper))){
        snprintf(op->result,sizeof(op->result),"provider_unavailable");
        snprintf(op->message,sizeof(op->message),"TrollStore helper provider unavailable");
        return 0;
    }
    char output[8192]={0};
    char *argv[]={(char*)"trollstorehelper",(char*)"install",(char*)"custom",path,NULL};
    op->executed=1;
    int rc=spawn_capture_timeout(helper,argv,output,sizeof(output),180);
    int helper_success=rc==0||rc==182||rc==184;
    op->post_checked=1;
    op->post_passed=helper_success&&verify_app_installed(info.expected_identifier);
    op->ok=op->post_passed;
    snprintf(op->result,sizeof(op->result),op->ok?"success":"failed");
    snprintf(op->message,sizeof(op->message),op->ok?"TrollStore package installed and verified":"TrollStore install failed or installed bundle could not be verified");
    if(op->post_passed)snprintf(op->post_detail,sizeof(op->post_detail),"uicache resolves expected bundle identifier");
    else snprintf(op->post_detail,sizeof(op->post_detail),"helper exit=%d or bundle verification failed",rc);
    update_state(package_id,op->ok?"installed":"failed",op->ok?"installed":NULL,op->ok?NULL:"trollstorehelper/post-condition failed");
    return op->ok;
}

static void json_escape_small(const char *src, char *dst, size_t cap) {
    size_t j=0;
    for(size_t i=0;src&&src[i]&&j+2<cap;i++){
        unsigned char c=(unsigned char)src[i];
        if(c=='"'||c=='\\'){dst[j++]='\\';dst[j++]=(char)c;}
        else if(c>=0x20)dst[j++]=(char)c;
    }
    dst[j]=0;
}

char *rt_packages_json(void) {
    sqlite3 *db=NULL;
    if(!db_open(&db))return NULL;
    sqlite3_stmt *st=NULL;
    int rc=sqlite3_prepare_v2(db,
        "SELECT package_id,name,format,expected_identifier,total_size,received_size,sha256,state FROM staged_packages ORDER BY created_at DESC LIMIT 100",
        -1,&st,NULL);
    if(rc!=SQLITE_OK){sqlite3_close(db);return NULL;}
    char *out=calloc(1,65536);
    if(!out){sqlite3_finalize(st);sqlite3_close(db);return NULL;}
    size_t used=(size_t)snprintf(out,65536,"{\"schemaVersion\":1,\"packages\":[");
    int rows=0;
    while(sqlite3_step(st)==SQLITE_ROW){
        char id[192]={0},name[384]={0},format[32]={0},identifier[512]={0},hash[160]={0},state[96]={0};
        json_escape_small((const char*)sqlite3_column_text(st,0),id,sizeof(id));
        json_escape_small((const char*)sqlite3_column_text(st,1),name,sizeof(name));
        json_escape_small((const char*)sqlite3_column_text(st,2),format,sizeof(format));
        json_escape_small((const char*)sqlite3_column_text(st,3),identifier,sizeof(identifier));
        json_escape_small((const char*)sqlite3_column_text(st,6),hash,sizeof(hash));
        json_escape_small((const char*)sqlite3_column_text(st,7),state,sizeof(state));
        int n=snprintf(out+used,65536-used,
            "%s{\"packageId\":\"%s\",\"name\":\"%s\",\"format\":\"%s\",\"expectedIdentifier\":\"%s\",\"totalSize\":%lld,\"receivedSize\":%lld,\"sha256\":\"%s\",\"state\":\"%s\"}",
            rows?",":"",id,name,format,identifier,(long long)sqlite3_column_int64(st,4),(long long)sqlite3_column_int64(st,5),hash,state);
        if(n<0||(size_t)n>=65536-used)break;
        used+=(size_t)n;
        rows++;
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    snprintf(out+used,65536-used,"],\"count\":%d}",rows);
    return out;
}
