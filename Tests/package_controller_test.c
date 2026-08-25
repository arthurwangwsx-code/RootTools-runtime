#include <CommonCrypto/CommonDigest.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "package_controller.h"

static void sha256_hex(const unsigned char *bytes, size_t length, char out[65]) {
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(bytes,(CC_LONG)length,digest);
    for(int i=0;i<CC_SHA256_DIGEST_LENGTH;i++)snprintf(out+i*2,3,"%02x",digest[i]);
    out[64]=0;
}

int main(void) {
    char temp[]="/tmp/roottools-packages-XXXXXX";
    assert(mkdtemp(temp)!=NULL);
    char root[512],db[512];
    snprintf(root,sizeof(root),"%s/files",temp);
    snprintf(db,sizeof(db),"%s/packages.sqlite3",temp);
    assert(setenv("ROOTTOOLS_PACKAGE_ROOT",root,1)==0);
    assert(setenv("ROOTTOOLS_PACKAGE_DB",db,1)==0);

    const unsigned char payload[]="RootTools staged package bytes";
    const size_t length=sizeof(payload)-1;
    char hash[65];sha256_hex(payload,length,hash);
    RTPackageOperation op;
    assert(rt_package_begin("pkg-test","example.deb","deb","com.example.roottools",(long long)length,hash,&op)==1);
    assert(op.ok==1&&op.executed==1&&op.post_passed==1);

    assert(rt_package_append("pkg-test",3,payload,4,&op)==0);
    size_t split=10;
    assert(rt_package_append("pkg-test",0,payload,split,&op)==1);
    assert(rt_package_append("pkg-test",(long long)split,payload+split,length-split,&op)==1);
    assert(rt_package_commit("pkg-test",&op)==1);
    assert(op.ok==1&&op.post_passed==1&&!strcmp(op.result,"ready"));

    RTPackageInfo info;
    assert(rt_package_get("pkg-test",&info)==1);
    assert(!strcmp(info.state,"ready"));
    assert(info.received_size==(long long)length);
    char *catalog=rt_packages_json();
    assert(catalog!=NULL&&strstr(catalog,"pkg-test")!=NULL&&strstr(catalog,"com.example.roottools")!=NULL);
    free(catalog);

    assert(rt_package_discard("pkg-test",&op)==1);
    assert(rt_package_get("pkg-test",&info)==1&&!strcmp(info.state,"discarded"));

    assert(rt_package_begin("bad-hash","broken.ipa","ipa","com.example.broken",(long long)length,
        "0000000000000000000000000000000000000000000000000000000000000000",&op)==1);
    assert(rt_package_append("bad-hash",0,payload,length,&op)==1);
    assert(rt_package_commit("bad-hash",&op)==0);
    assert(!strcmp(op.result,"hash_mismatch"));

    assert(rt_package_begin("self-update","roottools.deb","deb","com.arthur.roottools",(long long)length,hash,&op)==1);
    assert(rt_package_append("self-update",0,payload,length,&op)==1);
    assert(rt_package_commit("self-update",&op)==1);
    assert(rt_package_install_deb("self-update",&op)==0);
    assert(!strcmp(op.result,"self_update_required"));

    puts("package_controller_test: PASS");
    return 0;
}
