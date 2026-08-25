#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "principal_store.h"

int main(void) {
    char temp[] = "/tmp/roottools-principals-XXXXXX";
    int fd = mkstemp(temp);
    assert(fd >= 0);
    close(fd);
    unlink(temp);
    assert(setenv("ROOTTOOLS_PRINCIPAL_DB", temp, 1) == 0);

    char token[RT_PRINCIPAL_TOKEN_CAP] = {0};
    char error[256] = {0};
    assert(rt_principal_create(
        "host:test-mac",
        "host",
        "Test Mac",
        token,
        sizeof(token),
        error,
        sizeof(error)
    ) == 1);
    assert(strlen(token) == 48);

    char id[RT_PRINCIPAL_ID_CAP] = {0};
    char kind[RT_PRINCIPAL_KIND_CAP] = {0};
    assert(rt_principal_authenticate(token, id, sizeof(id), kind, sizeof(kind)) == 1);
    assert(!strcmp(id, "host:test-mac"));
    assert(!strcmp(kind, "host"));
    assert(rt_principal_authenticate("000000000000000000000000000000000000000000000000", id, sizeof(id), kind, sizeof(kind)) == 0);
    assert(rt_principal_capability_allowed("host:test-mac", "device.status.observe") == 0);
    assert(rt_principal_grant("host:test-mac", "device.status.observe", 0, error, sizeof(error)) == 1);
    assert(rt_principal_capability_allowed("host:test-mac", "device.status.observe") == 1);
    assert(rt_principal_grant("host:test-mac", "device.app.launch", (long long)time(NULL) + 60, error, sizeof(error)) == 1);
    assert(rt_principal_capability_allowed("host:test-mac", "device.app.launch") == 1);
    char *grants = rt_principal_grants_json("host:test-mac");
    assert(grants != NULL);
    assert(strstr(grants, "device.status.observe") != NULL);
    assert(strstr(grants, "device.app.launch") != NULL);
    assert(strstr(grants, "\"count\":2") != NULL);
    free(grants);
    assert(rt_principal_ungrant("host:test-mac", "device.app.launch", error, sizeof(error)) == 1);
    assert(rt_principal_capability_allowed("host:test-mac", "device.app.launch") == 0);

    char *catalog = rt_principals_json();
    assert(catalog != NULL);
    assert(strstr(catalog, "host:test-mac") != NULL);
    assert(strstr(catalog, "Test Mac") != NULL);
    assert(strstr(catalog, "\"kind\":\"host\"") != NULL);
    assert(strstr(catalog, "\"lastUsedAt\":") != NULL);
    assert(strstr(catalog, "\"grantCount\":1") != NULL);
    assert(strstr(catalog, token) == NULL);
    free(catalog);

    char duplicate[RT_PRINCIPAL_TOKEN_CAP] = {0};
    assert(rt_principal_create(
        "host:test-mac",
        "host",
        "Duplicate Mac",
        duplicate,
        sizeof(duplicate),
        error,
        sizeof(error)
    ) == 0);

    assert(rt_principal_revoke("host:test-mac", error, sizeof(error)) == 1);
    assert(rt_principal_authenticate(token, id, sizeof(id), kind, sizeof(kind)) == 0);
    catalog = rt_principals_json();
    assert(catalog != NULL);
    assert(strstr(catalog, "\"state\":\"revoked\"") != NULL);
    assert(strstr(catalog, "\"revokedAt\":null") == NULL);
    free(catalog);

    unlink(temp);
    puts("principal_store_test: PASS");
    return 0;
}
