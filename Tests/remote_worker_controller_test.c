#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "remote_worker_controller.h"

int main(void) {
    char path[] = "/tmp/roottools-remote-worker-test.XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    unlink(path);

    assert(setenv("ROOTTOOLS_REMOTE_WORKER_STATE_PATH", path, 1) == 0);
    assert(setenv("ROOTTOOLS_TEST_ASSERTION_SUPPORTED", "1", 1) == 0);
    assert(setenv("ROOTTOOLS_TEST_BATTERY_PERCENT", "82", 1) == 0);
    assert(setenv("ROOTTOOLS_TEST_BATTERY_TEMPERATURE_CENTI_C", "3350", 1) == 0);
    assert(setenv("ROOTTOOLS_TEST_EXTERNAL_POWER", "1", 1) == 0);
    assert(setenv("ROOTTOOLS_TEST_IS_CHARGING", "0", 1) == 0);
    assert(setenv("ROOTTOOLS_TEST_SYSTEM_LOAD_MW", "760", 1) == 0);
    assert(setenv("ROOTTOOLS_TEST_BATTERY_CYCLE_COUNT", "924", 1) == 0);
    assert(setenv("ROOTTOOLS_TEST_FULL_CHARGE_CAPACITY_MAH", "3454", 1) == 0);
    assert(setenv("ROOTTOOLS_TEST_DESIGN_CAPACITY_MAH", "4325", 1) == 0);
    assert(setenv("ROOTTOOLS_TEST_CHARGE_CONTROL_AVAILABLE", "1", 1) == 0);

    rt_remote_worker_init();
    RTRemoteWorkerState state = rt_remote_worker_state();
    assert(state.config.enabled == 0);
    assert(state.config.dim_percent == 1);
    assert(state.assertion_active == 0);
    assert(state.battery_percent == 82);
    assert(state.battery_temperature_available == 1);
    assert(state.system_load_milliwatts == 760);

    RTRemoteWorkerConfig config = state.config;
    config.enabled = 1;
    config.charge_control_enabled = 1;
    char error[256] = {0};
    assert(rt_remote_worker_configure(&config, error, sizeof(error)) == 1);
    state = rt_remote_worker_state();
    assert(state.assertion_active == 1);
    assert(state.charge_inhibited == 1);
    assert(state.charge_control_verified == 1);
    assert(state.battery_health_percent == 79);
    assert(rt_remote_worker_ui_allowed() == 1);

    assert(setenv("ROOTTOOLS_TEST_BATTERY_TEMPERATURE_CENTI_C", "4050", 1) == 0);
    sleep(1);
    rt_remote_worker_tick();
    state = rt_remote_worker_state();
    assert(state.thermal_paused == 1);
    assert(state.assertion_active == 0);
    assert(rt_remote_worker_ui_allowed() == 0);

    assert(setenv("ROOTTOOLS_TEST_BATTERY_TEMPERATURE_CENTI_C", "3650", 1) == 0);
    sleep(1);
    rt_remote_worker_tick();
    state = rt_remote_worker_state();
    assert(state.thermal_paused == 0);
    assert(state.assertion_active == 1);

    char *json = rt_remote_worker_json();
    assert(json != NULL);
    assert(strstr(json, "\"enabled\":true") != NULL);
    assert(strstr(json, "\"systemLoadMilliwatts\":760") != NULL);
    assert(strstr(json, "\"temperatureAvailable\":true") != NULL);
    assert(strstr(json, "\"ceilingPercent\":80") != NULL);
    free(json);

    assert(unsetenv("ROOTTOOLS_TEST_BATTERY_TEMPERATURE_CENTI_C") == 0);
    sleep(1);
    rt_remote_worker_tick();
    state = rt_remote_worker_state();
    assert(state.battery_temperature_available == 0);
    assert(state.thermal_paused == 1);
    assert(state.assertion_active == 0);
    assert(rt_remote_worker_ui_allowed() == 0);
    assert(setenv("ROOTTOOLS_TEST_BATTERY_TEMPERATURE_CENTI_C", "3650", 1) == 0);
    sleep(1);
    rt_remote_worker_tick();
    state = rt_remote_worker_state();
    assert(state.thermal_paused == 0);
    assert(state.assertion_active == 1);

    config.enabled = 0;
    config.charge_control_enabled = 0;
    assert(rt_remote_worker_configure(&config, error, sizeof(error)) == 1);
    state = rt_remote_worker_state();
    assert(state.assertion_active == 0);
    assert(state.charge_inhibited == 0);

    rt_remote_worker_shutdown();
    unlink(path);
    puts("remote_worker_controller_test: PASS");
    return 0;
}
