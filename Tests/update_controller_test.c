#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <CommonCrypto/CommonDigest.h>

#include "package_controller.h"
#include "update_controller.h"

int main(void) {
    char root[]="/tmp/roottools-update-test-XXXXXX";
    assert(mkdtemp(root)!=NULL);
    char package_root[512],package_db[512],update_db[512];
    snprintf(package_root,sizeof(package_root),"%s/packages",root);
    snprintf(package_db,sizeof(package_db),"%s/packages.sqlite3",root);
    snprintf(update_db,sizeof(update_db),"%s/update.sqlite3",root);
    setenv("ROOTTOOLS_PACKAGE_ROOT",package_root,1);
    setenv("ROOTTOOLS_PACKAGE_DB",package_db,1);
    setenv("ROOTTOOLS_UPDATE_DB",update_db,1);

    const unsigned char payload[]="self-update-fixture";
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    char hash[65]={0};
    CC_SHA256(payload,(CC_LONG)(sizeof(payload)-1),digest);
    for(int i=0;i<CC_SHA256_DIGEST_LENGTH;i++)snprintf(hash+i*2,3,"%02x",digest[i]);
    RTPackageOperation package_op;
    assert(rt_package_begin("pkg-update","roottools.deb","deb","com.arthur.roottools",
        (long long)(sizeof(payload)-1),hash,&package_op));
    assert(rt_package_append("pkg-update",0,payload,sizeof(payload)-1,&package_op));
    assert(rt_package_commit("pkg-update",&package_op));
    const char *package_id="pkg-update";

    RTUpdateOperation update;
    assert(rt_update_schedule("update-request-1",package_id,&update));
    assert(update.ok&&update.executed&&update.post_passed);
    RTUpdateOperation busy;
    assert(!rt_update_schedule("update-request-2",package_id,&busy));
    assert(!strcmp(busy.result,"busy"));

    char request[128]={0};
    assert(rt_update_peek_pending(request,sizeof(request)));
    assert(!strcmp(request,"update-request-1"));
    memset(request,0,sizeof(request));
    assert(rt_update_claim_pending(request,sizeof(request)));
    assert(!strcmp(request,"update-request-1"));
    char none[128]={0};
    assert(!rt_update_peek_pending(none,sizeof(none)));
    RTUpdateInfo info;
    assert(rt_update_get(request,&info));
    assert(!strcmp(info.state,"launching"));
    assert(rt_update_mark(request,"running","0.8.0","started",NULL));
    assert(rt_update_mark(request,"succeeded","0.8.0","healthy",NULL));
    assert(rt_update_get(request,&info));
    assert(!strcmp(info.state,"succeeded"));
    assert(!strcmp(info.target_version,"0.8.0"));

    char *json=rt_updates_json();
    assert(json&&strstr(json,"update-request-1")&&strstr(json,"succeeded"));
    free(json);
    puts("update_controller_test: PASS");
    return 0;
}
