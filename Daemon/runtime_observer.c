#include "runtime_observer.h"
#include "provider_registry.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <unistd.h>

typedef struct {
    int running;
    int pid;
    unsigned int uid;
    char command[128];
} RTProcessFact;

static const char *dpkg_status_path(void) {
    const char *override=getenv("ROOTTOOLS_DPKG_STATUS");
    return override&&override[0]?override:"/var/jb/Library/dpkg/status";
}

static int process_named(const char *wanted, RTProcessFact *fact) {
    if(fact)memset(fact,0,sizeof(*fact));
    size_t length=0;int mib[4]={CTL_KERN,KERN_PROC,KERN_PROC_ALL,0};
    if(sysctl(mib,4,NULL,&length,NULL,0)!=0||!length)return 0;
    struct kinfo_proc *items=malloc(length);if(!items)return 0;
    if(sysctl(mib,4,items,&length,NULL,0)!=0){free(items);return 0;}
    size_t count=length/sizeof(struct kinfo_proc);int found=0;
    for(size_t i=0;i<count;i++){
        const char *name=items[i].kp_proc.p_comm;
        if(strcmp(name,wanted))continue;
        if(fact){
            fact->running=1;fact->pid=items[i].kp_proc.p_pid;fact->uid=items[i].kp_eproc.e_ucred.cr_uid;
            snprintf(fact->command,sizeof(fact->command),"%s",name);
        }
        found=1;break;
    }
    free(items);return found;
}

static int port_reachable(int port) {
    int fd=socket(AF_INET,SOCK_STREAM,0);if(fd<0)return 0;
    struct timeval tv={.tv_sec=0,.tv_usec=150000};
    setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof(tv));
    struct sockaddr_in addr={0};addr.sin_family=AF_INET;addr.sin_port=htons((uint16_t)port);addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    int rc=connect(fd,(struct sockaddr*)&addr,sizeof(addr));close(fd);return rc==0;
}

static void trim(char *text) {
    if(!text)return;size_t n=strlen(text);
    while(n&&isspace((unsigned char)text[n-1]))text[--n]=0;
    size_t start=0;while(text[start]&&isspace((unsigned char)text[start]))start++;
    if(start)memmove(text,text+start,strlen(text+start)+1);
}

static int contains_case_insensitive(const char *haystack, const char *needle) {
    if(!haystack||!needle)return 0;size_t n=strlen(needle);if(!n)return 1;
    for(const char *p=haystack;*p;p++)if(!strncasecmp(p,needle,n))return 1;
    return 0;
}

static int dpkg_package_version(const char *needle, char *package_id, size_t package_cap, char *version, size_t version_cap) {
    FILE *f=fopen(dpkg_status_path(),"r");if(!f)return 0;
    char line[2048],current_package[256]={0},current_version[256]={0},current_name[256]={0},current_status[256]={0};
    int found=0;
    while(fgets(line,sizeof(line),f)){
        if(line[0]=='\n'||line[0]=='\r'){
            if(current_package[0]&&current_version[0]&&contains_case_insensitive(current_status,"install ok installed")&&
               (contains_case_insensitive(current_package,needle)||contains_case_insensitive(current_name,needle))){found=1;break;}
            current_package[0]=current_version[0]=current_name[0]=current_status[0]=0;continue;
        }
        char *colon=strchr(line,':');if(!colon)continue;*colon=0;char *value=colon+1;trim(line);trim(value);
        if(!strcmp(line,"Package"))snprintf(current_package,sizeof(current_package),"%s",value);
        else if(!strcmp(line,"Version"))snprintf(current_version,sizeof(current_version),"%s",value);
        else if(!strcmp(line,"Name"))snprintf(current_name,sizeof(current_name),"%s",value);
        else if(!strcmp(line,"Status"))snprintf(current_status,sizeof(current_status),"%s",value);
    }
    if(!found&&current_package[0]&&current_version[0]&&contains_case_insensitive(current_status,"install ok installed")&&
       (contains_case_insensitive(current_package,needle)||contains_case_insensitive(current_name,needle)))found=1;
    fclose(f);
    if(!found)return 0;
    if(package_id&&package_cap)snprintf(package_id,package_cap,"%s",current_package);
    if(version&&version_cap)snprintf(version,version_cap,"%s",current_version);
    return 1;
}

static const char *first_executable(const char *const *paths) {
    for(size_t i=0;paths[i];i++)if(access(paths[i],X_OK)==0)return paths[i];
    return NULL;
}

static const char *json_string_or_null(const char *value, char *buffer, size_t cap) {
    if(!value||!value[0])return "null";
    size_t used=0;if(cap<3)return "null";buffer[used++]='"';
    for(size_t i=0;value[i]&&used+2<cap;i++){
        unsigned char c=(unsigned char)value[i];
        if(c=='"'||c=='\\')buffer[used++]='\\';
        if(c>=0x20)buffer[used++]=(char)c;
    }
    if(used+1>=cap)return "null";buffer[used++]='"';buffer[used]=0;return buffer;
}

char *rt_frida_status_json(void) {
    const RTProvider *provider=rt_provider_find("runtime.frida");int provider_available=provider&&rt_provider_available(provider);
    RTProcessFact process;process_named("frida-server",&process);
    char package[256]={0},version[256]={0};int package_known=dpkg_package_version("frida",package,sizeof(package),version,sizeof(version));
    const char *paths[]={"/var/jb/usr/sbin/frida-server","/var/jb/usr/bin/frida-server","/var/jb/usr/lib/frida/frida-server",NULL};
    const char *server_path=first_executable(paths);char path_json[1024],package_json[600],version_json[600];
    char *out=calloc(1,4096);if(!out)return NULL;
    snprintf(out,4096,
        "{\"schemaVersion\":1,\"providerId\":\"runtime.frida\",\"state\":\"%s\",\"port\":27042,\"protocolReachable\":%s,"
        "\"serverPath\":%s,\"process\":{\"running\":%s,\"pid\":%d,\"uid\":%u,\"command\":\"%s\"},"
        "\"package\":{\"known\":%s,\"id\":%s,\"version\":%s},"
        "\"policy\":{\"headlessObservation\":true,\"scriptExecutionExposed\":false,\"arbitraryAttachExposed\":false}}",
        provider_available?"available":"unavailable",port_reachable(27042)?"true":"false",
        json_string_or_null(server_path,path_json,sizeof(path_json)),process.running?"true":"false",process.pid,process.uid,process.command,
        package_known?"true":"false",json_string_or_null(package,package_json,sizeof(package_json)),json_string_or_null(version,version_json,sizeof(version_json)));
    return out;
}

char *rt_ellekit_status_json(void) {
    const RTProvider *provider=rt_provider_find("runtime.ellekit");int available=provider&&rt_provider_available(provider);
    const char *library="/var/jb/usr/lib/libellekit.dylib";
    const char *loader="/var/jb/usr/libexec/ellekit/loader";
    const char *injector="/var/jb/usr/lib/ellekit/libinjector.dylib";
    const char *pspawn="/var/jb/usr/lib/ellekit/pspawn.dylib";
    const char *safemode="/var/jb/usr/lib/ellekit/MobileSafety.dylib";
    const char *tweak_dir="/var/jb/usr/lib/TweakInject";
    char package[256]={0},version[256]={0};int package_known=dpkg_package_version("ellekit",package,sizeof(package),version,sizeof(version));
    char package_json[600],version_json[600];char *out=calloc(1,4096);if(!out)return NULL;
    snprintf(out,4096,
        "{\"schemaVersion\":1,\"providerId\":\"runtime.ellekit\",\"state\":\"%s\","
        "\"components\":{\"library\":%s,\"loader\":%s,\"injector\":%s,\"pspawn\":%s,\"safeMode\":%s,\"tweakInjectDirectory\":%s},"
        "\"package\":{\"known\":%s,\"id\":%s,\"version\":%s},"
        "\"policy\":{\"headlessObservation\":true,\"rawHookAPIExposed\":false,\"arbitraryInjectionExposed\":false}}",
        available?"available":"unavailable",access(library,R_OK)==0?"true":"false",access(loader,X_OK)==0?"true":"false",
        access(injector,R_OK)==0?"true":"false",access(pspawn,R_OK)==0?"true":"false",access(safemode,R_OK)==0?"true":"false",
        access(tweak_dir,F_OK)==0?"true":"false",package_known?"true":"false",
        json_string_or_null(package,package_json,sizeof(package_json)),json_string_or_null(version,version_json,sizeof(version_json)));
    return out;
}
