#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "control_plane.h"

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--catalog") == 0) {
        char *json = rt_capabilities_json();
        assert(json != NULL);
        puts(json);
        free(json);
        return 0;
    }

    assert(rt_capability_count() >= 15);

    const RTCapability *launch = rt_capability_find("device.app.launch");
    assert(launch != NULL);
    assert(launch->risk == RT_RISK_R1);
    assert(launch->enabled == 1);
    assert(rt_capability_find_action("app.launch") == launch);

    const RTCapability *lock_observe = rt_capability_find("device.lock.observe");
    assert(lock_observe != NULL);
    assert(lock_observe->risk == RT_RISK_R0);
    assert(lock_observe->enabled == 1);

    const RTCapability *queue_launch = rt_capability_find("device.automation.queue-app-launch");
    assert(queue_launch != NULL);
    assert(queue_launch->risk == RT_RISK_R1);
    assert(queue_launch->reversible == 1);
    assert(rt_capability_find_action("automation.queue-app-launch") == queue_launch);

    char policy_dir[256];
    snprintf(policy_dir, sizeof(policy_dir), "/tmp/roottools-control-plane-%d", getpid());
    mkdir(policy_dir, 0700);
    setenv("ROOTTOOLS_POLICY_DIR", policy_dir, 1);
    assert(rt_capability_effective_enabled(launch) == 1);
    assert(rt_capability_set_enabled("device.app.launch", 0) == 1);
    assert(rt_capability_effective_enabled(launch) == 0);
    assert(rt_policy_evaluate(launch, 0).allowed == 0);
    assert(rt_capability_set_enabled("device.app.launch", 1) == 1);
    assert(rt_capability_effective_enabled(launch) == 1);

    const RTCapability *process = rt_capability_find("device.process.terminate");
    assert(process != NULL);
    RTPolicyDecision missing_confirmation = rt_policy_evaluate(process, 0);
    assert(missing_confirmation.allowed == 0);
    assert(missing_confirmation.confirmation_required == 1);
    RTPolicyDecision confirmed = rt_policy_evaluate(process, 1);
    assert(confirmed.allowed == 1);

    const RTCapability *raw_shell = rt_capability_find("device.raw-shell");
    assert(raw_shell != NULL);
    assert(raw_shell->risk == RT_RISK_R3);
    assert(raw_shell->enabled == 0);
    assert(rt_capability_set_enabled("device.raw-shell", 1) == 0);
    assert(rt_policy_evaluate(raw_shell, 1).allowed == 0);

    char *text = rt_capabilities_text();
    assert(text != NULL);
    assert(strstr(text, "device.process.terminate") != NULL);
    free(text);

    char *json = rt_capabilities_json();
    assert(json != NULL);
    assert(strstr(json, "\"schemaVersion\":1") != NULL);
    assert(strstr(json, "\"device.app.launch\"") != NULL);
    assert(strstr(json, "\"rawPrivilegedShellExposed\":false") != NULL);
    free(json);

    rmdir(policy_dir);
    unsetenv("ROOTTOOLS_POLICY_DIR");

    puts("control_plane_test: PASS");
    return 0;
}
