#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <signal.h>
#include <spawn.h>
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

#define PORT 45821
#define VERSION "0.2.0"
#define TOKEN "__ROOTTOOLS_TOKEN__"
#define MAX_REQUEST 65536
#define MAX_ACTION_BODY 24576
#define AUDIT_PATH "/var/mobile/Library/RootTools/audit.log"

extern char **environ;

static unsigned long audit_counter = 0;

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
    mkdir("/var/mobile/Library/RootTools/files", 0750);
    mkdir("/var/jb/etc/roottools", 0750);
}

static void make_audit_id(char *out, size_t cap) {
    audit_counter++;
    snprintf(out, cap, "%lld-%d-%lu", (long long)time(NULL), getpid(), audit_counter);
}

static void audit_action(const char *audit_id, const char *action, const char *target, int ok, const char *message) {
    ensure_action_dirs();
    FILE *f = fopen(AUDIT_PATH, "a");
    if (!f) return;
    char a[256]={0}, t[512]={0}, m[1024]={0};
    json_escape(action, a, sizeof(a)); json_escape(target, t, sizeof(t)); json_escape(message, m, sizeof(m));
    fprintf(f, "{\"time\":%lld,\"auditId\":\"%s\",\"action\":\"%s\",\"target\":\"%s\",\"ok\":%s,\"message\":\"%s\"}\n",
            (long long)time(NULL), audit_id, a, t, ok ? "true" : "false", m);
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
    char *out=calloc(1,8192);
    appendf(out,8192,
        "R0 READ ONLY\nstatus\nruntime\napps\nprocesses\nfiles\nnetwork\ndiagnostics\naudit\nfile.read (RootTools scopes only)\n\n"
        "R1 REVERSIBLE\napp.launch\napp.terminate\nfile.write (RootTools scopes only)\n\n"
        "R2 SYSTEM IMPACT\nprocess.terminate (SIGTERM, non-root only)\n\n"
        "R3 DEVICE CRITICAL\nnot exposed\n\nraw privileged shell: not exposed\n");
    return out;
}

static char *audit_text(void) {
    ensure_action_dirs();
    int fd=open(AUDIT_PATH,O_RDONLY|O_NOFOLLOW); if(fd<0) return strdup("No privileged actions recorded yet.\n");
    struct stat st; if(fstat(fd,&st)!=0){close(fd);return strdup("Audit log unavailable.\n");}
    off_t start=st.st_size>49152?st.st_size-49152:0; lseek(fd,start,SEEK_SET);
    char *out=calloc(1,50000); ssize_t n=read(fd,out,49152); close(fd); if(n<0){free(out);return strdup("Audit log read failed.\n");}
    out[n]=0; return out;
}

static void send_response(int fd, int code, const char *type, const char *body) {
    const char *msg = code == 200 ? "OK" : code == 400 ? "Bad Request" : code == 401 ? "Unauthorized" : code == 403 ? "Forbidden" : code == 404 ? "Not Found" : "Internal Server Error";
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

static void send_error(int fd, int code, const char *message) {
    char escaped[1024]={0}, body[1200]={0}; json_escape(message, escaped, sizeof(escaped));
    snprintf(body,sizeof(body),"{\"ok\":false,\"error\":\"%s\"}",escaped); send_response(fd,code,"application/json",body);
}

static void send_action_receipt(int fd, const char *action, const char *target, int ok, const char *message) {
    char audit_id[96]={0}; make_audit_id(audit_id,sizeof(audit_id)); audit_action(audit_id,action,target,ok,message);
    char a[256]={0},m[1024]={0},body[1600]={0}; json_escape(action,a,sizeof(a)); json_escape(message,m,sizeof(m));
    snprintf(body,sizeof(body),"{\"ok\":%s,\"action\":\"%s\",\"message\":\"%s\",\"auditId\":\"%s\"}",ok?"true":"false",a,m,audit_id);
    send_response(fd,200,"application/json",body);
}

static void send_file_read_receipt(int fd, const char *target, int ok, const char *message, const char *output) {
    char audit_id[96]={0}; make_audit_id(audit_id,sizeof(audit_id)); audit_action(audit_id,"file.read",target,ok,message);
    size_t output_cap=strlen(output)*2+64; char *escaped_output=calloc(1,output_cap); json_escape(output,escaped_output,output_cap);
    char escaped_message[1024]={0}; json_escape(message,escaped_message,sizeof(escaped_message));
    size_t body_cap=strlen(escaped_output)+1800; char *body=calloc(1,body_cap);
    snprintf(body,body_cap,"{\"ok\":%s,\"action\":\"file.read\",\"message\":\"%s\",\"auditId\":\"%s\",\"output\":\"%s\"}",ok?"true":"false",escaped_message,audit_id,escaped_output);
    send_response(fd,200,"application/json",body); free(escaped_output); free(body);
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

static int request_has_token(const char *req) {
    const char *name="X-RootTools-Token:"; size_t name_len=strlen(name), token_len=strlen(TOKEN);
    const char *line=req;
    while(line && *line){
        const char *end=strstr(line,"\r\n"); if(!end) break; if(end==line) break;
        size_t len=(size_t)(end-line);
        if(len>=name_len && strncasecmp(line,name,name_len)==0){
            const char *value=line+name_len; const char *limit=end;
            while(value<limit && (*value==' '||*value=='\t')) value++;
            while(limit>value && (limit[-1]==' '||limit[-1]=='\t')) limit--;
            return (size_t)(limit-value)==token_len && memcmp(value,TOKEN,token_len)==0;
        }
        line=end+2;
    }
    return 0;
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
    if(!strcmp(scope,"mobile")){snprintf(out,cap,"/var/mobile/Library/RootTools/files/%s",name);return 1;}
    if(!strcmp(scope,"bootstrap")){snprintf(out,cap,"/var/jb/etc/roottools/%s",name);return 1;}
    return 0;
}

static int critical_process_name(const char *name);

static void handle_action_app_launch(int fd, const char *body) {
    char bundle[256]={0}; if(!json_get_string(body,"bundleID",bundle,sizeof(bundle))||!safe_bundle_id(bundle)){send_error(fd,400,"invalid bundleID");return;}
    char *argv[]={(char*)"uiopen",(char*)"--bundleid",bundle,NULL}; int rc=fixed_spawn_wait("/var/jb/usr/bin/uiopen",argv,NULL,0);
    char message[512]; snprintf(message,sizeof(message),rc==0?"Launched %s":"Failed to launch %s (exit %d)",bundle,rc);
    send_action_receipt(fd,"app.launch",bundle,rc==0,message);
}

static void handle_action_app_terminate(int fd, const char *body) {
    char bundle[256]={0}, executable[256]={0}; if(!json_get_string(body,"bundleID",bundle,sizeof(bundle))||!safe_bundle_id(bundle)){send_error(fd,400,"invalid bundleID");return;}
    if(!executable_for_bundle(bundle,executable,sizeof(executable))){send_action_receipt(fd,"app.terminate",bundle,0,"Could not resolve app executable");return;}
    if(critical_process_name(executable)){send_action_receipt(fd,"app.terminate",bundle,0,"Denied: critical application executable");return;}
    char *argv[]={(char*)"killall",(char*)"-TERM",executable,NULL}; int rc=fixed_spawn_wait("/var/jb/usr/bin/killall",argv,NULL,0);
    char message[512]; snprintf(message,sizeof(message),rc==0?"Terminated %s":"App was not running or could not be terminated: %s",bundle);
    send_action_receipt(fd,"app.terminate",bundle,rc==0,message);
}

static int critical_process_name(const char *name) {
    const char *deny[]={"launchd","kernel_task","SpringBoard","backboardd","roottools-execd",NULL};
    for(int i=0;deny[i];i++) if(!strcmp(name,deny[i]))return 1; return 0;
}

static void handle_action_process_terminate(int fd, const char *body) {
    long raw=0; if(!json_get_int(body,"pid",&raw)||raw<=100||raw>999999){send_error(fd,400,"pid outside allowed range");return;}
    pid_t pid=(pid_t)raw; uid_t uid=0; char name[128]={0}; if(!process_info(pid,&uid,name,sizeof(name))){send_action_receipt(fd,"process.terminate","unknown",0,"Process not found");return;}
    char target[256]; snprintf(target,sizeof(target),"pid=%d uid=%u %s",pid,uid,name);
    if(uid==0||critical_process_name(name)){send_action_receipt(fd,"process.terminate",target,0,"Denied: root or critical process");return;}
    int rc=kill(pid,SIGTERM); char message[512]; snprintf(message,sizeof(message),rc==0?"SIGTERM sent to pid %d (%s)":"Failed to terminate pid %d (%s): %s",pid,name,strerror(errno));
    send_action_receipt(fd,"process.terminate",target,rc==0,message);
}

static void handle_action_file_write(int fd, const char *body) {
    char scope[32]={0},name[128]={0},content[20000]={0},path[1024]={0};
    if(!json_get_string(body,"scope",scope,sizeof(scope))||!json_get_string(body,"name",name,sizeof(name))||!json_get_string(body,"content",content,sizeof(content))){send_error(fd,400,"missing file fields");return;}
    if(strlen(content)>16384||!build_file_path(scope,name,path,sizeof(path))){send_error(fd,400,"file request outside allowed RootTools scope");return;}
    int out=open(path,O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0640); int ok=0; char message[512];
    if(out>=0){size_t len=strlen(content); ssize_t n=write(out,content,len); fsync(out); close(out); ok=n==(ssize_t)len; snprintf(message,sizeof(message),ok?"Wrote %zu bytes to %s":"Short write to %s",len,path);} else snprintf(message,sizeof(message),"Write failed: %s",strerror(errno));
    send_action_receipt(fd,"file.write",path,ok,message);
}

static void handle_action_file_read(int fd, const char *body) {
    char scope[32]={0},name[128]={0},path[1024]={0};
    if(!json_get_string(body,"scope",scope,sizeof(scope))||!json_get_string(body,"name",name,sizeof(name))||!build_file_path(scope,name,path,sizeof(path))){send_error(fd,400,"file request outside allowed RootTools scope");return;}
    int in=open(path,O_RDONLY|O_NOFOLLOW); if(in<0){send_file_read_receipt(fd,path,0,"File not found","");return;} struct stat st; if(fstat(in,&st)!=0||st.st_size>32768){close(in);send_file_read_receipt(fd,path,0,"File is unavailable or exceeds 32 KiB","");return;}
    char *raw=calloc(1,(size_t)st.st_size+2); ssize_t n=read(in,raw,(size_t)st.st_size+1); close(in); if(n<0){free(raw);send_file_read_receipt(fd,path,0,"File read failed","");return;} raw[n]=0;
    send_file_read_receipt(fd,path,1,"Read allowed RootTools file",raw); free(raw);
}

static void handle(int fd) {
    char *req=calloc(1,MAX_REQUEST); if(!req)return; ssize_t n=read_request(fd,req,MAX_REQUEST); if(n<=0){free(req);return;} if(n==-2){send_error(fd,400,"request body too large");free(req);return;}
    if (!request_has_token(req)) { send_response(fd, 401, "application/json", "{\"error\":\"unauthorized\"}"); free(req); return; }
    char method[16]={0}, path[256]={0}; sscanf(req, "%15s %255s", method, path); const char *body=request_body(req);

    if (!strcmp(path, "/v1/status") && !strcmp(method,"GET")) {
        struct utsname u; uname(&u);
        char machine[64]={0}, osbuild[64]={0}; sysctl_string("hw.machine", machine, sizeof(machine)); sysctl_string("kern.osversion", osbuild, sizeof(osbuild));
        int dopamine = 0; char *proc = processes_text(&dopamine); free(proc);
        char response[2048];
        snprintf(response, sizeof(response),
            "{\"daemonVersion\":\"%s\",\"uid\":%d,\"machine\":\"%s\",\"osBuild\":\"%s\",\"kernel\":\"%s\",\"cpuCount\":%d,\"memoryBytes\":%llu,\"rootFreeBytes\":%llu,\"varFreeBytes\":%llu,\"jailbreakRootless\":%s,\"dopamineRunning\":%s,\"sshReady\":%s,\"fridaReady\":%s,\"zxTouchReady\":%s}",
            VERSION, getuid(), machine, osbuild, u.release, sysctl_int("hw.ncpu"), sysctl_u64("hw.memsize"), free_bytes("/"), free_bytes("/var"),
            access("/var/jb", F_OK)==0 ? "true":"false", dopamine ? "true":"false", port_open(22)?"true":"false", port_open(27042)?"true":"false", port_open(6000)?"true":"false");
        send_response(fd, 200, "application/json", response); free(req); return;
    }
    if (!strcmp(method,"GET") && !strcmp(path, "/v1/runtime")) send_text_payload(fd, runtime_text());
    else if (!strcmp(method,"GET") && !strcmp(path, "/v1/apps")) send_text_payload(fd, apps_text());
    else if (!strcmp(method,"GET") && !strcmp(path, "/v1/processes")) send_text_payload(fd, processes_text(NULL));
    else if (!strcmp(method,"GET") && !strcmp(path, "/v1/files")) send_text_payload(fd, files_text());
    else if (!strcmp(method,"GET") && !strcmp(path, "/v1/network")) send_text_payload(fd, network_text());
    else if (!strcmp(method,"GET") && !strcmp(path, "/v1/diagnostics")) send_text_payload(fd, diagnostics_text());
    else if (!strcmp(method,"GET") && !strcmp(path, "/v1/capabilities")) send_text_payload(fd, capabilities_text());
    else if (!strcmp(method,"GET") && !strcmp(path, "/v1/audit")) send_text_payload(fd, audit_text());
    else if (!strcmp(method,"POST") && !strcmp(path,"/v1/actions/app-launch")) handle_action_app_launch(fd,body);
    else if (!strcmp(method,"POST") && !strcmp(path,"/v1/actions/app-terminate")) handle_action_app_terminate(fd,body);
    else if (!strcmp(method,"POST") && !strcmp(path,"/v1/actions/process-terminate")) handle_action_process_terminate(fd,body);
    else if (!strcmp(method,"POST") && !strcmp(path,"/v1/actions/file-write")) handle_action_file_write(fd,body);
    else if (!strcmp(method,"POST") && !strcmp(path,"/v1/actions/file-read")) handle_action_file_read(fd,body);
    else send_response(fd, 404, "application/json", "{\"error\":\"not_found\"}");
    free(req);
}

int main(void) {
    ensure_action_dirs();
    int s = socket(AF_INET, SOCK_STREAM, 0); if (s < 0) return 2;
    int one=1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr={0}; addr.sin_family=AF_INET; addr.sin_port=htons(PORT); addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    if (bind(s, (struct sockaddr*)&addr, sizeof(addr)) != 0) return 3;
    if (listen(s, 16) != 0) return 4;
    for (;;) { int c=accept(s, NULL, NULL); if (c<0) continue; handle(c); close(c); }
}
