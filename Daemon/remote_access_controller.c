#include "remote_access_controller.h"
#include "principal_store.h"

#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define RT_REMOTE_ACCESS_DEFAULT_PORT 45822
#define RT_REMOTE_ACCESS_MAX_MINUTES 480

static RTRemoteAccessState g_state;
static int g_initialized=0;

static const char *state_path(void) {
    const char *override=getenv("ROOTTOOLS_REMOTE_ACCESS_STATE_PATH");
    return override&&override[0]?override:"/var/mobile/Library/RootTools/remote-access.conf";
}

static int configured_port(void) {
    const char *override=getenv("ROOTTOOLS_REMOTE_ACCESS_PORT");
    if(override&&override[0]){
        long value=strtol(override,NULL,10);
        if(value>=1024&&value<=65535)return (int)value;
    }
    return RT_REMOTE_ACCESS_DEFAULT_PORT;
}

static int safe_principal_id(const char *value) {
    size_t n=value?strlen(value):0;
    if(n<3||n>=sizeof(g_state.principal_id))return 0;
    for(size_t i=0;i<n;i++){
        unsigned char c=(unsigned char)value[i];
        if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='.'||c=='_'||c=='-'||c==':'||c=='/'))return 0;
    }
    return 1;
}

static int ensure_parent(void) {
    char copy[512]={0};
    const char *path=state_path();
    if(strlen(path)>=sizeof(copy))return 0;
    snprintf(copy,sizeof(copy),"%s",path);
    char *slash=strrchr(copy,'/');
    if(!slash||slash==copy)return 1;
    *slash=0;
    char partial[512]={0};
    size_t used=0;
    for(const char *p=copy;*p;p++){
        if(used+2>=sizeof(partial))return 0;
        partial[used++]=*p;partial[used]=0;
        if(*p=='/'&&used>1){if(mkdir(partial,0750)!=0&&errno!=EEXIST)return 0;}
    }
    return mkdir(partial,0750)==0||errno==EEXIST;
}

static int tailnet_ipv4(char out[64]) {
    out[0]=0;
    const char *fixture=getenv("ROOTTOOLS_TEST_TAILNET_IPV4");
    if(fixture&&fixture[0]){
        struct in_addr parsed;
        if(inet_pton(AF_INET,fixture,&parsed)==1){snprintf(out,64,"%s",fixture);return 1;}
        return 0;
    }
    struct ifaddrs *interfaces=NULL;
    if(getifaddrs(&interfaces)!=0)return 0;
    int found=0;
    for(struct ifaddrs *item=interfaces;item;item=item->ifa_next){
        if(!item->ifa_addr||item->ifa_addr->sa_family!=AF_INET)continue;
        struct sockaddr_in *address=(struct sockaddr_in*)item->ifa_addr;
        uint32_t host=ntohl(address->sin_addr.s_addr);
        // Tailscale's IPv4 CGNAT allocation is 100.64.0.0/10.
        if((host&0xffc00000u)!=0x64400000u)continue;
        if(inet_ntop(AF_INET,&address->sin_addr,out,64)){found=1;break;}
    }
    freeifaddrs(interfaces);
    return found;
}

static int persist_state(void) {
    if(!ensure_parent())return 0;
    char temporary[640]={0};
    int n=snprintf(temporary,sizeof(temporary),"%s.tmp-%d",state_path(),getpid());
    if(n<=0||(size_t)n>=sizeof(temporary))return 0;
    FILE *file=fopen(temporary,"w");
    if(!file)return 0;
    fprintf(file,"enabled=%d\nprincipal=%s\nexpiresAt=%lld\n",g_state.enabled,g_state.principal_id,(long long)g_state.expires_at);
    int ok=fflush(file)==0&&fsync(fileno(file))==0;
    if(fclose(file)!=0)ok=0;
    if(ok&&rename(temporary,state_path())!=0)ok=0;
    if(!ok)unlink(temporary);
    return ok;
}

static void load_state(void) {
    memset(&g_state,0,sizeof(g_state));
    g_state.port=configured_port();
    FILE *file=fopen(state_path(),"r");
    if(!file)return;
    char line[512]={0};
    while(fgets(line,sizeof(line),file)){
        char *newline=strchr(line,'\n');if(newline)*newline=0;
        if(!strncmp(line,"enabled=",8))g_state.enabled=atoi(line+8)?1:0;
        else if(!strncmp(line,"principal=",10))snprintf(g_state.principal_id,sizeof(g_state.principal_id),"%s",line+10);
        else if(!strncmp(line,"expiresAt=",10))g_state.expires_at=strtoll(line+10,NULL,10);
    }
    fclose(file);
}

static int principal_is_active_host(const char *principal_id) {
    char kind[32]={0};
    return safe_principal_id(principal_id)&&rt_principal_active_kind(principal_id,kind,sizeof(kind))&&!strcmp(kind,"host");
}

static void normalize_runtime_state(void) {
    if(g_state.enabled){
        time_t now=time(NULL);
        if(g_state.expires_at<=now||!principal_is_active_host(g_state.principal_id)){
            g_state.enabled=0;g_state.expires_at=0;g_state.principal_id[0]=0;
            (void)persist_state();
        }
    }
    g_state.transport_available=tailnet_ipv4(g_state.bind_address);
    if(!g_state.transport_available)g_state.bind_address[0]=0;
}

void rt_remote_access_init(void) {
    if(g_initialized)return;
    g_initialized=1;
    load_state();
    normalize_runtime_state();
}

void rt_remote_access_tick(void) {
    if(!g_initialized)rt_remote_access_init();
    normalize_runtime_state();
}

int rt_remote_access_snapshot(RTRemoteAccessState *state) {
    if(!state)return 0;
    if(!g_initialized)rt_remote_access_init();
    normalize_runtime_state();
    *state=g_state;
    return 1;
}

int rt_remote_access_configure(
    int enabled,
    const char *principal_id,
    int duration_minutes,
    char *error_out,
    size_t error_cap
) {
    if(error_out&&error_cap)error_out[0]=0;
    if(!g_initialized)rt_remote_access_init();
    if(!enabled){
        g_state.enabled=0;g_state.principal_id[0]=0;g_state.expires_at=0;
        g_state.listener_active=0;g_state.listener_error[0]=0;
        if(!persist_state()){
            if(error_out&&error_cap)snprintf(error_out,error_cap,"remote session state could not be persisted");
            return 0;
        }
        return 1;
    }
    if(duration_minutes<5||duration_minutes>RT_REMOTE_ACCESS_MAX_MINUTES){
        if(error_out&&error_cap)snprintf(error_out,error_cap,"duration must be between 5 and %d minutes",RT_REMOTE_ACCESS_MAX_MINUTES);
        return 0;
    }
    if(!principal_is_active_host(principal_id)){
        if(error_out&&error_cap)snprintf(error_out,error_cap,"remote session requires an active Host principal");
        return 0;
    }
    char address[64]={0};
    if(!tailnet_ipv4(address)){
        if(error_out&&error_cap)snprintf(error_out,error_cap,"Tailscale IPv4 is not available; connect Tailscale before starting a remote session");
        return 0;
    }
    g_state.enabled=1;
    snprintf(g_state.principal_id,sizeof(g_state.principal_id),"%s",principal_id);
    g_state.expires_at=(int64_t)time(NULL)+(int64_t)duration_minutes*60;
    g_state.transport_available=1;
    snprintf(g_state.bind_address,sizeof(g_state.bind_address),"%s",address);
    g_state.listener_active=0;g_state.listener_error[0]=0;
    if(!persist_state()){
        g_state.enabled=0;g_state.principal_id[0]=0;g_state.expires_at=0;
        if(error_out&&error_cap)snprintf(error_out,error_cap,"remote session state could not be persisted");
        return 0;
    }
    return 1;
}

int rt_remote_access_allows_principal(const char *principal_id) {
    if(!g_initialized)rt_remote_access_init();
    normalize_runtime_state();
    return g_state.enabled&&principal_id&&g_state.principal_id[0]&&!strcmp(g_state.principal_id,principal_id);
}

void rt_remote_access_set_listener_status(int active, const char *error) {
    if(!g_initialized)rt_remote_access_init();
    g_state.listener_active=active?1:0;
    snprintf(g_state.listener_error,sizeof(g_state.listener_error),"%s",error?error:"");
}

static void json_escape(const char *input, char *out, size_t cap) {
    size_t used=0;
    for(const char *p=input?input:"";*p&&used+2<cap;p++){
        if(*p=='"'||*p=='\\'){out[used++]='\\';out[used++]=*p;}
        else if((unsigned char)*p>=32)out[used++]=*p;
    }
    out[used]=0;
}

char *rt_remote_access_json(void) {
    RTRemoteAccessState state;
    if(!rt_remote_access_snapshot(&state))return NULL;
    char principal[256]={0},address[128]={0},error[384]={0};
    json_escape(state.principal_id,principal,sizeof(principal));
    json_escape(state.bind_address,address,sizeof(address));
    json_escape(state.listener_error,error,sizeof(error));
    char *out=calloc(1,2048);if(!out)return NULL;
    char listener_error_json[512]={0};
    if(error[0])snprintf(listener_error_json,sizeof(listener_error_json),"\"%s\"",error);
    else snprintf(listener_error_json,sizeof(listener_error_json),"null");
    snprintf(out,2048,
        "{\"schemaVersion\":1,\"enabled\":%s,\"principalId\":\"%s\",\"expiresAt\":%lld,"
        "\"transport\":{\"kind\":\"tailscale\",\"available\":%s,\"bindAddress\":\"%s\",\"port\":%d,\"listenerActive\":%s,\"listenerError\":%s},"
        "\"policy\":{\"publicInternetListener\":false,\"ownerTokenAcceptedRemotely\":false,\"legacyAgentTokenAcceptedRemotely\":false,\"namedHostPrincipalRequired\":true,\"maxDurationMinutes\":%d}}",
        state.enabled?"true":"false",principal,(long long)state.expires_at,
        state.transport_available?"true":"false",address,state.port,state.listener_active?"true":"false",
        listener_error_json,RT_REMOTE_ACCESS_MAX_MINUTES);
    return out;
}
