#include "principal_store.h"
#include "remote_access_controller.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    char db[]="/tmp/roottools-remote-access-principals.XXXXXX";
    char state[]="/tmp/roottools-remote-access-state.XXXXXX";
    int db_fd=mkstemp(db);assert(db_fd>=0);close(db_fd);unlink(db);
    int state_fd=mkstemp(state);assert(state_fd>=0);close(state_fd);unlink(state);
    setenv("ROOTTOOLS_PRINCIPAL_DB",db,1);
    setenv("ROOTTOOLS_REMOTE_ACCESS_STATE_PATH",state,1);
    setenv("ROOTTOOLS_TEST_TAILNET_IPV4","100.88.77.66",1);
    setenv("ROOTTOOLS_REMOTE_ACCESS_PORT","45999",1);

    char host_token[64]={0},error[256]={0};
    assert(rt_principal_create("host:remote-test","host","Remote Test Host",host_token,sizeof(host_token),error,sizeof(error)));
    char app_token[64]={0};
    assert(rt_principal_create("app:test","app","Test App",app_token,sizeof(app_token),error,sizeof(error)));

    rt_remote_access_init();
    RTRemoteAccessState snapshot;
    assert(rt_remote_access_snapshot(&snapshot));
    assert(!snapshot.enabled);
    assert(snapshot.transport_available);
    assert(!strcmp(snapshot.bind_address,"100.88.77.66"));
    assert(snapshot.port==45999);

    assert(!rt_remote_access_configure(1,"app:test",60,error,sizeof(error)));
    assert(strstr(error,"Host principal"));
    assert(rt_remote_access_configure(1,"host:remote-test",60,error,sizeof(error)));
    assert(rt_remote_access_snapshot(&snapshot));
    assert(snapshot.enabled);
    assert(!strcmp(snapshot.principal_id,"host:remote-test"));
    assert(snapshot.expires_at>0);
    assert(rt_remote_access_allows_principal("host:remote-test"));
    assert(!rt_remote_access_allows_principal("host:other"));

    rt_remote_access_set_listener_status(1,NULL);
    char *json=rt_remote_access_json();assert(json);
    assert(strstr(json,"\"kind\":\"tailscale\""));
    assert(strstr(json,"\"listenerActive\":true"));
    assert(strstr(json,"\"publicInternetListener\":false"));
    assert(strstr(json,"\"namedHostPrincipalRequired\":true"));
    free(json);

    assert(rt_principal_revoke("host:remote-test",error,sizeof(error)));
    rt_remote_access_tick();
    assert(rt_remote_access_snapshot(&snapshot));
    assert(!snapshot.enabled);

    assert(rt_remote_access_configure(0,"",0,error,sizeof(error)));
    unlink(db);unlink(state);
    puts("remote_access_controller_test: PASS");
    return 0;
}
