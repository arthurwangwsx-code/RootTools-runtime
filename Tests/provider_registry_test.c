#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "provider_registry.h"

int main(void) {
    assert(rt_provider_count() >= 10);
    const RTProvider *daemon = rt_provider_find("roottools.execd");
    assert(daemon != NULL);
    assert(rt_provider_available(daemon) == 1);
    assert(!strcmp(rt_provider_for_capability("device.app.launch"), "ui.springboard"));
    assert(!strcmp(rt_provider_for_capability("device.permission.tcc"), "permission.tcc"));
    assert(!strcmp(rt_provider_for_capability("device.package.uninstall-deb"), "bootstrap.procursus"));
    assert(!strcmp(rt_provider_for_capability("device.package.rollback-ipa"), "package.trollstore"));
    assert(!strcmp(rt_provider_for_capability("device.self-update.schedule"), "roottools.updater"));
    assert(!strcmp(rt_provider_for_capability("device.runtime.frida.observe"), "runtime.frida"));
    assert(!strcmp(rt_provider_for_capability("device.runtime.ellekit.observe"), "runtime.ellekit"));
    assert(!strcmp(rt_provider_for_capability("device.policy.read"), "roottools.execd"));
    assert(!strcmp(rt_provider_for_capability("device.policy.set-mode"), "roottools.execd"));
    assert(!strcmp(rt_provider_for_capability("device.performance.observe"), "ios.darwin"));
    assert(!strcmp(rt_provider_for_capability("device.remote-worker.observe"), "roottools.execd"));
    assert(!strcmp(rt_provider_for_capability("device.remote-worker.configure"), "roottools.execd"));
    assert(!strcmp(rt_provider_for_capability("device.task.submit-app-launch"), "roottools.execd"));
    assert(!strcmp(rt_provider_for_capability("device.task.cancel"), "roottools.execd"));
    assert(!strcmp(rt_provider_for_capability("device.ui.observe"), "ui.zxtouch"));
    assert(!strcmp(rt_provider_for_capability("device.ui.tap"), "roottools.execd"));
    assert(!strcmp(rt_provider_for_capability("device.ui.type"), "roottools.execd"));
    assert(!strcmp(rt_provider_for_capability("device.ui.swipe"), "roottools.execd"));
    assert(!strcmp(rt_provider_for_capability("device.principal.list"), "roottools.execd"));
    assert(!strcmp(rt_provider_for_capability("device.principal.create"), "roottools.execd"));
    assert(!strcmp(rt_provider_for_capability("device.principal.revoke"), "roottools.execd"));

    char *catalog = rt_providers_json();
    assert(catalog != NULL);
    assert(strstr(catalog, "\"schemaVersion\":1") != NULL);
    assert(strstr(catalog, "\"package.trollstore\"") != NULL);
    assert(strstr(catalog, "\"jailbreak.dopamine\"") != NULL);
    free(catalog);

    char *deb = rt_package_plan_json("deb");
    assert(deb != NULL);
    assert(strstr(deb, "\"selectedProviderId\":\"bootstrap.procursus\"") != NULL);
    assert(strstr(deb, "\"typedPackageOnly\":true") != NULL);
    free(deb);

    char *ipa = rt_package_plan_json("ipa");
    assert(ipa != NULL);
    assert(strstr(ipa, "\"selectedProviderId\":\"package.trollstore\"") != NULL);
    free(ipa);
    assert(rt_package_plan_json("unknown") == NULL);

    puts("provider_registry_test: PASS");
    return 0;
}
