#include "remote_worker_controller.h"

#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define RT_REMOTE_WORKER_DEFAULT_PATH "/var/mobile/Library/RootTools/remote-worker.conf"
#define RT_REMOTE_WORKER_ASSERTION_ON 255u

typedef int32_t RTIOReturn;
typedef uint32_t RTIOPMAssertionID;

typedef RTIOReturn (*RTIOPMAssertionCreateWithNameFn)(CFStringRef type, uint32_t level, CFStringRef name, RTIOPMAssertionID *assertion_id);
typedef RTIOReturn (*RTIOPMAssertionReleaseFn)(RTIOPMAssertionID assertion_id);

typedef struct {
    void *handle;
    RTIOPMAssertionCreateWithNameFn assertion_create;
    RTIOPMAssertionReleaseFn assertion_release;
} RTIOKitAPI;

static RTRemoteWorkerState g_state;
static RTIOKitAPI g_iokit;
static RTIOPMAssertionID g_assertion_id;
static int g_controller_owns_inhibit;
static time_t g_last_tick;
static time_t g_last_battery_sample;

extern char **environ;

static const char *state_path(void) {
    const char *override = getenv("ROOTTOOLS_REMOTE_WORKER_STATE_PATH");
    return override && override[0] ? override : RT_REMOTE_WORKER_DEFAULT_PATH;
}

static int env_int(const char *name, int *value) {
    const char *raw = getenv(name);
    if (!raw || !raw[0]) return 0;
    char *end = NULL;
    long parsed = strtol(raw, &end, 10);
    if (!end || *end != 0 || parsed < -1000000 || parsed > 1000000) return 0;
    *value = (int)parsed;
    return 1;
}

static void default_config(RTRemoteWorkerConfig *config) {
    memset(config, 0, sizeof(*config));
    config->enabled = 0;
    config->dim_percent = 1;
    config->charge_floor_percent = 70;
    config->charge_ceiling_percent = 80;
    config->thermal_pause_centi_c = 4000;
    config->thermal_resume_centi_c = 3700;
    config->charge_control_enabled = 0;
}

static int config_valid(const RTRemoteWorkerConfig *config, char *error, size_t error_cap) {
    if (!config) {
        if (error && error_cap) snprintf(error, error_cap, "configuration is missing");
        return 0;
    }
    if ((config->enabled != 0 && config->enabled != 1) ||
        (config->charge_control_enabled != 0 && config->charge_control_enabled != 1)) {
        if (error && error_cap) snprintf(error, error_cap, "boolean configuration values must be 0 or 1");
        return 0;
    }
    if (config->dim_percent < 1 || config->dim_percent > 20) {
        if (error && error_cap) snprintf(error, error_cap, "dimPercent must be between 1 and 20");
        return 0;
    }
    if (config->charge_floor_percent < 40 || config->charge_floor_percent > 79 ||
        config->charge_ceiling_percent < config->charge_floor_percent + 5 ||
        config->charge_ceiling_percent > 90) {
        if (error && error_cap) snprintf(error, error_cap, "charge window must be 40-90%% with at least 5%% hysteresis");
        return 0;
    }
    if (config->thermal_resume_centi_c < 2500 || config->thermal_resume_centi_c > 3900 ||
        config->thermal_pause_centi_c < config->thermal_resume_centi_c + 200 ||
        config->thermal_pause_centi_c > 4500) {
        if (error && error_cap) snprintf(error, error_cap, "thermal thresholds require 25-45C with at least 2C hysteresis");
        return 0;
    }
    return 1;
}

static int ensure_parent_directory(const char *path) {
    char parent[1024] = {0};
    size_t length = strlen(path);
    if (!length || length >= sizeof(parent)) return 0;
    memcpy(parent, path, length + 1);
    char *slash = strrchr(parent, '/');
    if (!slash || slash == parent) return 1;
    *slash = 0;
    if (mkdir(parent, 0750) == 0 || errno == EEXIST) return 1;
    return 0;
}

static int persist_config(const RTRemoteWorkerConfig *config) {
    const char *path = state_path();
    if (!ensure_parent_directory(path)) return 0;
    char temp[1100] = {0};
    int written = snprintf(temp, sizeof(temp), "%s.tmp", path);
    if (written <= 0 || (size_t)written >= sizeof(temp)) return 0;
    int fd = open(temp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0640);
    if (fd < 0) return 0;
    char body[512] = {0};
    int length = snprintf(body, sizeof(body),
        "version=1\n"
        "enabled=%d\n"
        "dim_percent=%d\n"
        "charge_floor_percent=%d\n"
        "charge_ceiling_percent=%d\n"
        "thermal_pause_centi_c=%d\n"
        "thermal_resume_centi_c=%d\n"
        "charge_control_enabled=%d\n",
        config->enabled,
        config->dim_percent,
        config->charge_floor_percent,
        config->charge_ceiling_percent,
        config->thermal_pause_centi_c,
        config->thermal_resume_centi_c,
        config->charge_control_enabled);
    if (length <= 0 || (size_t)length >= sizeof(body)) { close(fd); unlink(temp); return 0; }
    ssize_t n = write(fd, body, (size_t)length);
    int sync_ok = fsync(fd) == 0;
    close(fd);
    if (n != length || !sync_ok || rename(temp, path) != 0) { unlink(temp); return 0; }
    return 1;
}

static void load_config(RTRemoteWorkerConfig *config) {
    default_config(config);
    FILE *stream = fopen(state_path(), "r");
    if (!stream) return;
    char line[160] = {0};
    while (fgets(line, sizeof(line), stream)) {
        char key[96] = {0};
        int value = 0;
        if (sscanf(line, "%95[^=]=%d", key, &value) != 2) continue;
        if (!strcmp(key, "enabled")) config->enabled = value;
        else if (!strcmp(key, "dim_percent")) config->dim_percent = value;
        else if (!strcmp(key, "charge_floor_percent")) config->charge_floor_percent = value;
        else if (!strcmp(key, "charge_ceiling_percent")) config->charge_ceiling_percent = value;
        else if (!strcmp(key, "thermal_pause_centi_c")) config->thermal_pause_centi_c = value;
        else if (!strcmp(key, "thermal_resume_centi_c")) config->thermal_resume_centi_c = value;
        else if (!strcmp(key, "charge_control_enabled")) config->charge_control_enabled = value;
    }
    fclose(stream);
    if (!config_valid(config, NULL, 0)) default_config(config);
}

static void load_iokit(void) {
    if (g_iokit.handle) return;
    g_iokit.handle = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_LAZY | RTLD_LOCAL);
    if (!g_iokit.handle) return;
    g_iokit.assertion_create = (RTIOPMAssertionCreateWithNameFn)dlsym(g_iokit.handle, "IOPMAssertionCreateWithName");
    g_iokit.assertion_release = (RTIOPMAssertionReleaseFn)dlsym(g_iokit.handle, "IOPMAssertionRelease");
}

static const char *text_property_value(const char *text, const char *key) {
    if (!text || !key) return NULL;
    char needle[128] = {0};
    int n = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (n <= 0 || (size_t)n >= sizeof(needle)) return NULL;
    const char *cursor = strstr(text, needle);
    if (!cursor) return NULL;
    cursor = strchr(cursor + n, '=');
    if (!cursor) return NULL;
    cursor++;
    while (*cursor == ' ' || *cursor == '\t') cursor++;
    return cursor;
}

static int text_property_int(const char *text, const char *key, int *out) {
    const char *value = text_property_value(text, key);
    if (!value || !out) return 0;
    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (end == value || parsed < -100000000L || parsed > 100000000L) return 0;
    *out = (int)parsed;
    return 1;
}

static int text_property_bool(const char *text, const char *key, int *out) {
    const char *value = text_property_value(text, key);
    if (!value || !out) return 0;
    if (!strncmp(value, "Yes", 3) || !strncmp(value, "true", 4) || !strncmp(value, "1", 1)) { *out = 1; return 1; }
    if (!strncmp(value, "No", 2) || !strncmp(value, "false", 5) || !strncmp(value, "0", 1)) { *out = 0; return 1; }
    return 0;
}

static char *battery_ioreg_snapshot(void) {
    // Direct IORegistry access from a third-party rootless daemon is rejected
    // by iOS on the reference device. `/usr/sbin/ioreg` is an Apple-signed,
    // read-only bridge and is therefore the least-privileged stable source.
    int pipefd[2] = {-1, -1};
    if (pipe(pipefd) != 0) return NULL;
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) { close(pipefd[0]); close(pipefd[1]); return NULL; }
    (void)posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
    (void)posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDERR_FILENO);
    (void)posix_spawn_file_actions_addclose(&actions, pipefd[0]);
    char *argv[] = {(char *)"ioreg", (char *)"-r", (char *)"-c", (char *)"AppleSmartBattery", (char *)"-l", NULL};
    pid_t pid = 0;
    int spawn_status = posix_spawn(&pid, "/usr/sbin/ioreg", &actions, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pipefd[1]);
    if (spawn_status != 0) { close(pipefd[0]); return NULL; }

    size_t capacity = 131072, used = 0;
    char *buffer = calloc(1, capacity);
    if (!buffer) { close(pipefd[0]); (void)waitpid(pid, NULL, 0); return NULL; }
    while (used + 1 < capacity) {
        ssize_t n = read(pipefd[0], buffer + used, capacity - used - 1);
        if (n > 0) used += (size_t)n;
        if (n == 0) break;
        if (n < 0 && errno != EINTR) break;
    }
    close(pipefd[0]);
    buffer[used] = 0;
    int status = 0;
    if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0 || used == 0) {
        free(buffer);
        return NULL;
    }
    return buffer;
}

static int test_battery_override(void) {
    int capacity = 0;
    if (!env_int("ROOTTOOLS_TEST_BATTERY_PERCENT", &capacity)) return 0;
    g_state.battery_available = 1;
    g_state.battery_percent = capacity;
    g_state.battery_temperature_available = env_int("ROOTTOOLS_TEST_BATTERY_TEMPERATURE_CENTI_C", &g_state.battery_temperature_centi_c);
    (void)env_int("ROOTTOOLS_TEST_EXTERNAL_POWER", &g_state.external_power_connected);
    (void)env_int("ROOTTOOLS_TEST_IS_CHARGING", &g_state.charging);
    (void)env_int("ROOTTOOLS_TEST_INSTANT_AMPERAGE_MA", &g_state.instant_amperage_ma);
    (void)env_int("ROOTTOOLS_TEST_BATTERY_CYCLE_COUNT", &g_state.cycle_count);
    (void)env_int("ROOTTOOLS_TEST_FULL_CHARGE_CAPACITY_MAH", &g_state.full_charge_capacity_mah);
    (void)env_int("ROOTTOOLS_TEST_DESIGN_CAPACITY_MAH", &g_state.design_capacity_mah);
    g_state.system_load_available = env_int("ROOTTOOLS_TEST_SYSTEM_LOAD_MW", &g_state.system_load_milliwatts);
    if (g_state.design_capacity_mah > 0 && g_state.full_charge_capacity_mah > 0) {
        g_state.battery_health_percent = (g_state.full_charge_capacity_mah * 100) / g_state.design_capacity_mah;
    }
    int control_available = 0;
    if (env_int("ROOTTOOLS_TEST_CHARGE_CONTROL_AVAILABLE", &control_available)) g_state.charge_control_available = control_available != 0;
    return 1;
}

static void refresh_battery(void) {
    g_state.battery_available = 0;
    g_state.system_load_available = 0;
    g_state.battery_percent = 0;
    g_state.battery_temperature_centi_c = 0;
    g_state.battery_temperature_available = 0;
    g_state.external_power_connected = 0;
    g_state.charging = 0;
    g_state.instant_amperage_ma = 0;
    g_state.cycle_count = 0;
    g_state.full_charge_capacity_mah = 0;
    g_state.design_capacity_mah = 0;
    g_state.battery_health_percent = 0;
    g_state.system_load_milliwatts = 0;
    if (test_battery_override()) return;

    char *snapshot = battery_ioreg_snapshot();
    if (!snapshot) return;
    g_state.battery_available = 1;
    (void)text_property_int(snapshot, "CurrentCapacity", &g_state.battery_percent);
    int raw_temperature = 0;
    if (text_property_int(snapshot, "Temperature", &raw_temperature)) {
        // The AppleSmartBattery Temperature registry value is exposed in
        // hundredths of a degree Celsius (for example 3129 == 31.29 C).
        g_state.battery_temperature_centi_c = raw_temperature;
        g_state.battery_temperature_available = 1;
    }
    (void)text_property_bool(snapshot, "ExternalConnected", &g_state.external_power_connected);
    (void)text_property_bool(snapshot, "IsCharging", &g_state.charging);
    (void)text_property_int(snapshot, "InstantAmperage", &g_state.instant_amperage_ma);
    (void)text_property_int(snapshot, "CycleCount", &g_state.cycle_count);
    if (!text_property_int(snapshot, "AppleRawMaxCapacity", &g_state.full_charge_capacity_mah)) {
        (void)text_property_int(snapshot, "NominalChargeCapacity", &g_state.full_charge_capacity_mah);
    }
    (void)text_property_int(snapshot, "DesignCapacity", &g_state.design_capacity_mah);
    if (g_state.design_capacity_mah > 0 && g_state.full_charge_capacity_mah > 0) {
        g_state.battery_health_percent = (g_state.full_charge_capacity_mah * 100) / g_state.design_capacity_mah;
    }
    int inhibited = 0;
    if (text_property_bool(snapshot, "PredictiveChargingInhibit", &inhibited)) {
        g_state.charge_inhibited = inhibited != 0;
        g_state.charge_control_verified = 1;
    }
    g_state.system_load_available = text_property_int(snapshot, "SystemLoad", &g_state.system_load_milliwatts);
    // Writes remain disabled until a platform-safe, reversible charging helper
    // is proven on the physical device. Observation never requires that helper.
    g_state.charge_control_available = 0;
    free(snapshot);
}

static int test_assertion_override(int *supported) {
    int value = 0;
    if (!env_int("ROOTTOOLS_TEST_ASSERTION_SUPPORTED", &value)) return 0;
    *supported = value != 0;
    return 1;
}

static void release_assertion(void) {
    if (!g_state.assertion_active) return;
    int supported = 0;
    if (test_assertion_override(&supported)) {
        g_state.assertion_active = 0;
        g_assertion_id = 0;
        return;
    }
    if (g_iokit.assertion_release && g_assertion_id) (void)g_iokit.assertion_release(g_assertion_id);
    g_assertion_id = 0;
    g_state.assertion_active = 0;
}

static void acquire_assertion(void) {
    if (g_state.assertion_active) return;
    int supported = 0;
    if (test_assertion_override(&supported)) {
        g_state.assertion_supported = supported;
        if (supported) { g_assertion_id = 1; g_state.assertion_active = 1; }
        return;
    }
    load_iokit();
    g_state.assertion_supported = g_iokit.assertion_create != NULL && g_iokit.assertion_release != NULL;
    if (!g_state.assertion_supported) return;
    RTIOPMAssertionID assertion_id = 0;
    RTIOReturn result = g_iokit.assertion_create(
        CFSTR("PreventUserIdleDisplaySleep"),
        RT_REMOTE_WORKER_ASSERTION_ON,
        CFSTR("RootTools Remote Worker Mode"),
        &assertion_id);
    if (result == 0 && assertion_id != 0) {
        g_assertion_id = assertion_id;
        g_state.assertion_active = 1;
    }
}

static int set_charge_inhibit(int inhibit) {
    int test_available = 0;
    if (env_int("ROOTTOOLS_TEST_CHARGE_CONTROL_AVAILABLE", &test_available)) {
        if (!test_available) return 0;
        g_state.charge_inhibited = inhibit != 0;
        g_state.charge_control_verified = 1;
        g_controller_owns_inhibit = inhibit != 0;
        return 1;
    }
    (void)inhibit;
    return 0;
}

static void update_thermal_state(void) {
    if (!g_state.config.enabled) {
        g_state.thermal_paused = 0;
        return;
    }
    if (!g_state.battery_available || !g_state.battery_temperature_available) {
        // Fail safe: never hold the display awake indefinitely when RootTools
        // cannot prove that battery temperature telemetry is available.
        g_state.thermal_paused = 1;
        return;
    }
    if (!g_state.thermal_paused && g_state.battery_temperature_centi_c >= g_state.config.thermal_pause_centi_c) {
        g_state.thermal_paused = 1;
    } else if (g_state.thermal_paused && g_state.battery_temperature_centi_c <= g_state.config.thermal_resume_centi_c) {
        g_state.thermal_paused = 0;
    }
}

static void update_charge_control(void) {
    g_state.charge_control_error = 0;
    if (!g_state.config.enabled || !g_state.config.charge_control_enabled) {
        if (g_controller_owns_inhibit && !set_charge_inhibit(0)) g_state.charge_control_error = 1;
        return;
    }
    if (!g_state.battery_available || !g_state.external_power_connected || !g_state.charge_control_available) return;
    int desired = g_state.charge_inhibited;
    if (g_state.thermal_paused || g_state.battery_percent >= g_state.config.charge_ceiling_percent) desired = 1;
    else if (g_state.battery_percent <= g_state.config.charge_floor_percent) desired = 0;
    if (desired != g_state.charge_inhibited || (desired && !g_controller_owns_inhibit)) {
        if (!set_charge_inhibit(desired)) g_state.charge_control_error = 1;
    }
}

static void apply_assertion_state(void) {
    if (g_state.config.enabled && !g_state.thermal_paused) acquire_assertion();
    else release_assertion();
}

void rt_remote_worker_init(void) {
    memset(&g_state, 0, sizeof(g_state));
    g_last_tick = 0;
    g_last_battery_sample = 0;
    g_controller_owns_inhibit = 0;
    g_assertion_id = 0;
    load_config(&g_state.config);
    load_iokit();
    g_state.assertion_supported = g_iokit.assertion_create != NULL && g_iokit.assertion_release != NULL;
    g_state.charge_control_available = 0;
    int test_supported = 0;
    if (test_assertion_override(&test_supported)) g_state.assertion_supported = test_supported;
    rt_remote_worker_tick();
}

void rt_remote_worker_tick(void) {
    time_t now = time(NULL);
    if (g_last_tick != 0 && now == g_last_tick) return;
    g_last_tick = now;
    int test_battery_percent = 0;
    int test_battery_active = env_int("ROOTTOOLS_TEST_BATTERY_PERCENT", &test_battery_percent);
    if (test_battery_active || g_last_battery_sample == 0 || now - g_last_battery_sample >= 15) {
        refresh_battery();
        g_last_battery_sample = now;
    }
    update_thermal_state();
    update_charge_control();
    apply_assertion_state();
}

void rt_remote_worker_shutdown(void) {
    release_assertion();
    if (g_controller_owns_inhibit) (void)set_charge_inhibit(0);
    if (g_iokit.handle) dlclose(g_iokit.handle);
    memset(&g_iokit, 0, sizeof(g_iokit));
    g_last_tick = 0;
    g_last_battery_sample = 0;
    g_controller_owns_inhibit = 0;
    g_assertion_id = 0;
}

RTRemoteWorkerState rt_remote_worker_state(void) {
    return g_state;
}

int rt_remote_worker_configure(const RTRemoteWorkerConfig *config, char *error, size_t error_cap) {
    if (!config_valid(config, error, error_cap)) return 0;
    if (config->charge_control_enabled && !g_state.charge_control_available) {
        if (error && error_cap) snprintf(error, error_cap, "charge control is not verified on this device; use observe-only mode");
        return 0;
    }
    if (!persist_config(config)) {
        if (error && error_cap) snprintf(error, error_cap, "remote worker configuration could not be persisted");
        return 0;
    }
    g_state.config = *config;
    // Force an immediate sample/apply instead of waiting for the next daemon tick.
    g_last_tick = 0;
    rt_remote_worker_tick();
    return 1;
}

int rt_remote_worker_ui_allowed(void) {
    return !g_state.config.enabled || !g_state.thermal_paused;
}

char *rt_remote_worker_json(void) {
    rt_remote_worker_tick();
    char *out = calloc(1, 4096);
    if (!out) return NULL;
    const char *charge_state = "observe";
    if (g_state.config.charge_control_enabled) {
        if (!g_state.charge_control_available) charge_state = "unavailable";
        else if (g_state.charge_control_error) charge_state = "error";
        else if (g_state.charge_inhibited) charge_state = "inhibited";
        else charge_state = "allowed";
    }
    snprintf(out, 4096,
        "{\"schemaVersion\":1,\"enabled\":%s,\"mode\":\"%s\","
        "\"display\":{\"targetBrightnessPercent\":%d,\"strategy\":\"client-applied\",\"awakeAssertionSupported\":%s,\"awakeAssertionActive\":%s},"
        "\"battery\":{\"available\":%s,\"percent\":%d,\"temperatureCentiC\":%d,\"temperatureAvailable\":%s,\"externalPowerConnected\":%s,\"charging\":%s,\"instantAmperageMa\":%d,\"cycleCount\":%d,\"fullChargeCapacityMah\":%d,\"designCapacityMah\":%d,\"healthPercent\":%d},"
        "\"power\":{\"systemLoadAvailable\":%s,\"systemLoadMilliwatts\":%d},"
        "\"thermal\":{\"paused\":%s,\"pauseCentiC\":%d,\"resumeCentiC\":%d},"
        "\"chargeGuard\":{\"enabled\":%s,\"available\":%s,\"verified\":%s,\"inhibited\":%s,\"state\":\"%s\",\"floorPercent\":%d,\"ceilingPercent\":%d},"
        "\"policy\":{\"bypassPasscode\":false,\"thermalMayReleaseDisplayAssertion\":true,\"brightnessManagedByClient\":true}}",
        g_state.config.enabled ? "true" : "false",
        g_state.config.enabled ? (g_state.thermal_paused ? "thermal-paused" : "remote-worker") : "off",
        g_state.config.dim_percent,
        g_state.assertion_supported ? "true" : "false",
        g_state.assertion_active ? "true" : "false",
        g_state.battery_available ? "true" : "false",
        g_state.battery_percent,
        g_state.battery_temperature_centi_c,
        g_state.battery_temperature_available ? "true" : "false",
        g_state.external_power_connected ? "true" : "false",
        g_state.charging ? "true" : "false",
        g_state.instant_amperage_ma,
        g_state.cycle_count,
        g_state.full_charge_capacity_mah,
        g_state.design_capacity_mah,
        g_state.battery_health_percent,
        g_state.system_load_available ? "true" : "false",
        g_state.system_load_milliwatts,
        g_state.thermal_paused ? "true" : "false",
        g_state.config.thermal_pause_centi_c,
        g_state.config.thermal_resume_centi_c,
        g_state.config.charge_control_enabled ? "true" : "false",
        g_state.charge_control_available ? "true" : "false",
        g_state.charge_control_verified ? "true" : "false",
        g_state.charge_inhibited ? "true" : "false",
        charge_state,
        g_state.config.charge_floor_percent,
        g_state.config.charge_ceiling_percent);
    return out;
}
