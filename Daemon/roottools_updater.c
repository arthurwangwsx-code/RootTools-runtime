#include "package_controller.h"
#include "update_controller.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define ADMIN_TOKEN "__ROOTTOOLS_TOKEN__"
#define DAEMON_PORT 45821

static const char *path_override(const char *env, const char *fallback) {
    const char *value=getenv(env);
    return value&&value[0]?value:fallback;
}

static const char *app_path(void){return path_override("ROOTTOOLS_CURRENT_APP","/var/jb/Applications/RootTools.app");}
static const char *daemon_path(void){return path_override("ROOTTOOLS_CURRENT_DAEMON","/var/jb/usr/local/bin/roottools-execd");}
static const char *updater_path(void){return path_override("ROOTTOOLS_CURRENT_UPDATER","/var/jb/usr/local/bin/roottools-updater");}
static const char *daemon_plist(void){return path_override("ROOTTOOLS_DAEMON_PLIST","/var/jb/Library/LaunchDaemons/com.arthur.roottools.execd.plist");}
static const char *updater_plist(void){return path_override("ROOTTOOLS_UPDATER_PLIST","/var/jb/Library/LaunchDaemons/com.arthur.roottools.updater.plist");}
static const char *work_root(void){return path_override("ROOTTOOLS_UPDATE_WORK_ROOT","/var/mobile/Library/RootTools/self-update-work");}
static const char *dpkg_deb(void){return path_override("ROOTTOOLS_DPKG_DEB","/var/jb/usr/bin/dpkg-deb");}
static const char *ldid_path(void){return path_override("ROOTTOOLS_LDID","/var/jb/usr/bin/ldid");}
static const char *uicache_path(void){return path_override("ROOTTOOLS_UICACHE","/var/jb/usr/bin/uicache");}

static const char *launchctl_path(void) {
    const char *override=getenv("ROOTTOOLS_LAUNCHCTL");
    if(override&&override[0])return override;
    if(access("/bin/launchctl",X_OK)==0)return "/bin/launchctl";
    if(access("/usr/bin/launchctl",X_OK)==0)return "/usr/bin/launchctl";
    return "/var/jb/bin/launchctl";
}

static int safe_id(const char *value) {
    size_t n=value?strlen(value):0;if(!n||n>120)return 0;
    for(size_t i=0;i<n;i++){
        unsigned char c=(unsigned char)value[i];
        if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='.'||c=='_'||c=='-'))return 0;
    }
    return 1;
}

static int spawn_timeout(const char *program, char *const argv[], int timeout_seconds) {
    pid_t pid=0;int rc=posix_spawn(&pid,program,NULL,NULL,argv,environ);if(rc!=0)return rc;
    int status=0;
    for(int elapsed=0;elapsed<timeout_seconds*10;elapsed++){
        pid_t w=waitpid(pid,&status,WNOHANG);
        if(w==pid)return WIFEXITED(status)?WEXITSTATUS(status):128;
        if(w<0)return errno;
        usleep(100000);
    }
    kill(pid,SIGKILL);waitpid(pid,&status,0);return 124;
}

static int mkdir_one(const char *path, mode_t mode) {
    return mkdir(path,mode)==0||errno==EEXIST;
}

static int mkdir_parents(const char *path, mode_t mode) {
    char copy[1536];
    if(!path||strlen(path)>=sizeof(copy))return 0;
    snprintf(copy,sizeof(copy),"%s",path);
    for(char *p=copy+1;*p;p++){
        if(*p!='/')continue;*p=0;
        if(!mkdir_one(copy,mode)){*p='/';return 0;}*p='/';
    }
    return mkdir_one(copy,mode);
}

static int remove_tree(const char *path) {
    struct stat st;if(lstat(path,&st)!=0)return errno==ENOENT;
    if(S_ISLNK(st.st_mode)||S_ISREG(st.st_mode))return unlink(path)==0;
    if(!S_ISDIR(st.st_mode))return 0;
    DIR *dir=opendir(path);if(!dir)return 0;struct dirent *entry;int ok=1;
    while(ok&&(entry=readdir(dir))){
        if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,".."))continue;
        char child[1536];int n=snprintf(child,sizeof(child),"%s/%s",path,entry->d_name);
        if(n<=0||(size_t)n>=sizeof(child)||!remove_tree(child))ok=0;
    }
    closedir(dir);return ok&&rmdir(path)==0;
}

static int copy_file(const char *src, const char *dst, mode_t mode) {
    int in=open(src,O_RDONLY|O_NOFOLLOW);if(in<0)return 0;
    int out=open(dst,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,mode&07777);if(out<0){close(in);return 0;}
    unsigned char buffer[65536];ssize_t n;int ok=1;
    while((n=read(in,buffer,sizeof(buffer)))>0){
        ssize_t used=0;while(used<n){ssize_t w=write(out,buffer+used,(size_t)(n-used));if(w<=0){ok=0;break;}used+=w;}
        if(!ok)break;
    }
    if(n<0)ok=0;if(ok&&fsync(out)!=0)ok=0;
    close(in);close(out);if(!ok)unlink(dst);return ok;
}

static int copy_tree(const char *src, const char *dst) {
    struct stat st;if(lstat(src,&st)!=0||S_ISLNK(st.st_mode))return 0;
    if(S_ISREG(st.st_mode))return copy_file(src,dst,st.st_mode);
    if(!S_ISDIR(st.st_mode)||mkdir(dst,st.st_mode&07777)!=0)return 0;
    DIR *dir=opendir(src);if(!dir){rmdir(dst);return 0;}struct dirent *entry;int ok=1;
    while(ok&&(entry=readdir(dir))){
        if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,".."))continue;
        char child_src[1536],child_dst[1536];
        int a=snprintf(child_src,sizeof(child_src),"%s/%s",src,entry->d_name);
        int b=snprintf(child_dst,sizeof(child_dst),"%s/%s",dst,entry->d_name);
        if(a<=0||b<=0||(size_t)a>=sizeof(child_src)||(size_t)b>=sizeof(child_dst)||!copy_tree(child_src,child_dst))ok=0;
    }
    closedir(dir);if(!ok)remove_tree(dst);return ok;
}

static int allowed_extracted_file(const char *relative) {
    const char *app="var/jb/Applications/RootTools.app/";
    if(!strncmp(relative,app,strlen(app))&&relative[strlen(app)]!=0)return 1;
    return !strcmp(relative,"var/jb/usr/local/bin/roottools-execd")||
           !strcmp(relative,"var/jb/usr/local/bin/roottools-updater")||
           !strcmp(relative,"var/jb/Library/LaunchDaemons/com.arthur.roottools.execd.plist")||
           !strcmp(relative,"var/jb/Library/LaunchDaemons/com.arthur.roottools.updater.plist");
}

static int validate_tree_recursive(const char *root, const char *path) {
    struct stat st;if(lstat(path,&st)!=0||S_ISLNK(st.st_mode))return 0;
    if(S_ISREG(st.st_mode)){
        size_t root_len=strlen(root);const char *relative=path+root_len;while(*relative=='/')relative++;
        return allowed_extracted_file(relative);
    }
    if(!S_ISDIR(st.st_mode))return 0;
    DIR *dir=opendir(path);if(!dir)return 0;struct dirent *entry;int ok=1;
    while(ok&&(entry=readdir(dir))){
        if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,".."))continue;
        char child[1536];int n=snprintf(child,sizeof(child),"%s/%s",path,entry->d_name);
        if(n<=0||(size_t)n>=sizeof(child)||!validate_tree_recursive(root,child))ok=0;
    }
    closedir(dir);return ok;
}

static int required_payload_present(const char *root) {
    const char *suffixes[]={
        "var/jb/Applications/RootTools.app/RootTools",
        "var/jb/usr/local/bin/roottools-execd",
        "var/jb/usr/local/bin/roottools-updater",
        "var/jb/Library/LaunchDaemons/com.arthur.roottools.execd.plist",
        "var/jb/Library/LaunchDaemons/com.arthur.roottools.updater.plist",
    };
    for(size_t i=0;i<sizeof(suffixes)/sizeof(suffixes[0]);i++){
        char path[1536];int n=snprintf(path,sizeof(path),"%s/%s",root,suffixes[i]);
        struct stat st;if(n<=0||(size_t)n>=sizeof(path)||lstat(path,&st)!=0||!S_ISREG(st.st_mode)||S_ISLNK(st.st_mode))return 0;
    }
    return 1;
}

static int http_version(char *out, size_t cap) {
    int fd=socket(AF_INET,SOCK_STREAM,0);if(fd<0)return 0;
    struct timeval tv={.tv_sec=1,.tv_usec=0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof(tv));
    struct sockaddr_in addr={0};addr.sin_family=AF_INET;addr.sin_port=htons(DAEMON_PORT);addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    if(connect(fd,(struct sockaddr*)&addr,sizeof(addr))!=0){close(fd);return 0;}
    char request[512];int n=snprintf(request,sizeof(request),"GET /v1/hello HTTP/1.1\r\nHost: 127.0.0.1\r\nX-RootTools-Token: %s\r\nConnection: close\r\n\r\n",ADMIN_TOKEN);
    if(n<=0||send(fd,request,(size_t)n,0)!=n){close(fd);return 0;}
    char response[8192]={0};size_t used=0;ssize_t r;
    while(used+1<sizeof(response)&&(r=recv(fd,response+used,sizeof(response)-used-1,0))>0)used+=(size_t)r;
    close(fd);response[used]=0;
    const char *key="\"daemonVersion\":\"";char *p=strstr(response,key);if(!p)return 0;p+=strlen(key);char *end=strchr(p,'"');if(!end)return 0;
    size_t len=(size_t)(end-p);if(!len||len>=cap)return 0;memcpy(out,p,len);out[len]=0;return 1;
}

static int wait_version(const char *expected) {
    for(int i=0;i<60;i++){char version[64]={0};if(http_version(version,sizeof(version))&&!strcmp(version,expected))return 1;usleep(250000);}return 0;
}

static void daemon_version_from_package(const char *package_version, char *out, size_t cap) {
    snprintf(out,cap,"%s",package_version?package_version:"");
    char *dash=strrchr(out,'-');
    if(dash&&dash[1]){
        int numeric=1;for(char *p=dash+1;*p;p++)if(*p<'0'||*p>'9'){numeric=0;break;}
        if(numeric)*dash=0;
    }
}

static int sign_file(const char *path) {
    char *argv[]={(char*)"ldid",(char*)"-S",(char*)path,NULL};
    return access(ldid_path(),X_OK)==0&&spawn_timeout(ldid_path(),argv,20)==0;
}

static int bootout_daemon(void) {
    char *argv[]={(char*)"launchctl",(char*)"bootout",(char*)"system/com.arthur.roottools.execd",NULL};
    int rc=spawn_timeout(launchctl_path(),argv,20);return rc==0||rc==3||rc==5;
}

static int bootstrap_daemon(void) {
    char *argv[]={(char*)"launchctl",(char*)"bootstrap",(char*)"system",(char*)daemon_plist(),NULL};
    return spawn_timeout(launchctl_path(),argv,20)==0;
}

static void refresh_app(void) {
    if(access(uicache_path(),X_OK)!=0)return;
    char *argv[]={(char*)"uicache",(char*)"-p",(char*)app_path(),NULL};
    (void)spawn_timeout(uicache_path(),argv,30);
}

typedef struct {
    char current[1536];char candidate[1536];char rollback[1536];int swapped;
} Swap;

static int prepare_swap(const char *current, const char *source, const char *request_id, Swap *swap) {
    memset(swap,0,sizeof(*swap));snprintf(swap->current,sizeof(swap->current),"%s",current);
    int a=snprintf(swap->candidate,sizeof(swap->candidate),"%s.update-%s",current,request_id);
    int b=snprintf(swap->rollback,sizeof(swap->rollback),"%s.rollback-%s",current,request_id);
    if(a<=0||b<=0||(size_t)a>=sizeof(swap->candidate)||(size_t)b>=sizeof(swap->rollback))return 0;
    struct stat current_st;
    if(lstat(current,&current_st)!=0||S_ISLNK(current_st.st_mode))return 0;
    remove_tree(swap->candidate);
    if(access(swap->rollback,F_OK)==0)return 0;
    return copy_tree(source,swap->candidate);
}

static int apply_swap(Swap *swap) {
    if(access(swap->current,F_OK)==0&&rename(swap->current,swap->rollback)!=0)return 0;
    if(rename(swap->candidate,swap->current)!=0){if(access(swap->rollback,F_OK)==0)rename(swap->rollback,swap->current);return 0;}
    swap->swapped=1;return 1;
}

static void rollback_swap(Swap *swap) {
    if(!swap->swapped)return;
    remove_tree(swap->current);
    if(access(swap->rollback,F_OK)==0)rename(swap->rollback,swap->current);
    swap->swapped=0;
}

static void cleanup_swap(Swap *swap) {remove_tree(swap->candidate);remove_tree(swap->rollback);}

static int run_update(const char *request_id) {
    RTUpdateInfo update;
    if(!rt_update_get(request_id,&update)||strcmp(update.state,"launching"))return 2;
    if(!rt_update_mark(request_id,"running",NULL,"preflight",NULL))return 3;
    RTPackageInfo package;char artifact[1536]={0};
    if(!rt_package_resolve_artifact(update.package_id,"ready",&package,artifact,sizeof(artifact))||strcmp(package.format,"deb")||strcmp(package.expected_identifier,"com.arthur.roottools")){
        rt_update_mark(request_id,"failed",NULL,NULL,"verified RootTools DEB unavailable");return 4;
    }
    char package_name[128]={0},package_version[128]={0},target_version[64]={0};
    if(!rt_package_deb_field(update.package_id,"Package",package_name,sizeof(package_name))||strcmp(package_name,"com.arthur.roottools")||
       !rt_package_deb_field(update.package_id,"Version",package_version,sizeof(package_version))){
        rt_update_mark(request_id,"failed",NULL,NULL,"RootTools DEB metadata validation failed");return 5;
    }
    daemon_version_from_package(package_version,target_version,sizeof(target_version));
    if(!target_version[0]){rt_update_mark(request_id,"failed",NULL,NULL,"target daemon version unavailable");return 6;}
    rt_update_mark(request_id,"running",target_version,"extracting",NULL);

    char work[1536];int n=snprintf(work,sizeof(work),"%s/%s",work_root(),request_id);
    if(n<=0||(size_t)n>=sizeof(work)||!mkdir_parents(work_root(),0700)){rt_update_mark(request_id,"failed",target_version,NULL,"update work directory unavailable");return 7;}
    remove_tree(work);if(mkdir(work,0700)!=0){rt_update_mark(request_id,"failed",target_version,NULL,"update work directory could not be created");return 8;}
    char *extract_argv[]={(char*)"dpkg-deb",(char*)"-x",artifact,work,NULL};
    if(access(dpkg_deb(),X_OK)!=0||spawn_timeout(dpkg_deb(),extract_argv,60)!=0||!validate_tree_recursive(work,work)||!required_payload_present(work)){
        remove_tree(work);rt_update_mark(request_id,"failed",target_version,NULL,"self-update payload extraction/allowlist validation failed");return 9;
    }

    char source_app[1536],source_daemon[1536],source_updater[1536],source_daemon_plist[1536],source_updater_plist[1536];
    snprintf(source_app,sizeof(source_app),"%s/var/jb/Applications/RootTools.app",work);
    snprintf(source_daemon,sizeof(source_daemon),"%s/var/jb/usr/local/bin/roottools-execd",work);
    snprintf(source_updater,sizeof(source_updater),"%s/var/jb/usr/local/bin/roottools-updater",work);
    snprintf(source_daemon_plist,sizeof(source_daemon_plist),"%s/var/jb/Library/LaunchDaemons/com.arthur.roottools.execd.plist",work);
    snprintf(source_updater_plist,sizeof(source_updater_plist),"%s/var/jb/Library/LaunchDaemons/com.arthur.roottools.updater.plist",work);
    Swap swaps[5]={0};
    if(!prepare_swap(app_path(),source_app,request_id,&swaps[0])||!prepare_swap(daemon_path(),source_daemon,request_id,&swaps[1])||
       !prepare_swap(updater_path(),source_updater,request_id,&swaps[2])||!prepare_swap(daemon_plist(),source_daemon_plist,request_id,&swaps[3])||
       !prepare_swap(updater_plist(),source_updater_plist,request_id,&swaps[4])){
        for(int i=0;i<5;i++)cleanup_swap(&swaps[i]);remove_tree(work);rt_update_mark(request_id,"failed",target_version,NULL,"candidate preparation failed");return 10;
    }
    char candidate_app_exec[1536];snprintf(candidate_app_exec,sizeof(candidate_app_exec),"%s/RootTools",swaps[0].candidate);
    if(!sign_file(candidate_app_exec)||!sign_file(swaps[1].candidate)||!sign_file(swaps[2].candidate)){
        for(int i=0;i<5;i++)cleanup_swap(&swaps[i]);remove_tree(work);rt_update_mark(request_id,"failed",target_version,NULL,"candidate signing failed");return 11;
    }
    char old_version[64]={0};(void)http_version(old_version,sizeof(old_version));
    rt_update_mark(request_id,"running",target_version,"switching",NULL);
    (void)bootout_daemon();
    int applied=1;
    for(int i=0;i<5;i++)if(applied&&!apply_swap(&swaps[i]))applied=0;
    if(applied&&bootstrap_daemon()){refresh_app();if(wait_version(target_version)){
        for(int i=0;i<5;i++)cleanup_swap(&swaps[i]);remove_tree(work);
        rt_package_mark_external_install(update.package_id,"self-update","roottools.updater");
        rt_update_mark(request_id,"succeeded",target_version,"new daemon healthy",NULL);return 0;
    }}

    (void)bootout_daemon();
    for(int i=4;i>=0;i--)rollback_swap(&swaps[i]);
    for(int i=0;i<5;i++)cleanup_swap(&swaps[i]);
    int rollback_boot=bootstrap_daemon();refresh_app();
    int rollback_health=rollback_boot&&old_version[0]&&wait_version(old_version);
    remove_tree(work);
    if(rollback_health){rt_update_mark(request_id,"rolled_back",target_version,"previous daemon restored","new daemon failed health check");return 12;}
    rt_update_mark(request_id,"rollback_failed",target_version,NULL,"new daemon failed and previous daemon health could not be restored");return 13;
}

int main(int argc, char **argv) {
    setenv("PATH","/var/jb/bin:/var/jb/usr/bin:/var/jb/sbin:/var/jb/usr/sbin:/usr/bin:/bin:/usr/sbin:/sbin",1);
    char request_id[128]={0};
    if(argc==3&&!strcmp(argv[1],"--request")){
        if(!safe_id(argv[2]))return 64;snprintf(request_id,sizeof(request_id),"%s",argv[2]);
    }else if(argc==1){
        if(!rt_update_claim_pending(request_id,sizeof(request_id)))return 0;
    }else return 64;
    return run_update(request_id);
}
