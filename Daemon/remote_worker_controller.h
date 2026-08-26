#ifndef ROOTTOOLS_REMOTE_WORKER_CONTROLLER_H
#define ROOTTOOLS_REMOTE_WORKER_CONTROLLER_H

#include <stddef.h>

typedef struct {
    int enabled;
    int dim_percent;
    int charge_floor_percent;
    int charge_ceiling_percent;
    int thermal_pause_centi_c;
    int thermal_resume_centi_c;
    int charge_control_enabled;
} RTRemoteWorkerConfig;

typedef struct {
    RTRemoteWorkerConfig config;
    int assertion_supported;
    int assertion_active;
    int thermal_paused;
    int battery_available;
    int battery_percent;
    int battery_temperature_centi_c;
    int battery_temperature_available;
    int external_power_connected;
    int charging;
    int instant_amperage_ma;
    int cycle_count;
    int full_charge_capacity_mah;
    int design_capacity_mah;
    int battery_health_percent;
    int system_load_milliwatts;
    int system_load_available;
    int charge_control_available;
    int charge_control_verified;
    int charge_inhibited;
    int charge_control_error;
} RTRemoteWorkerState;

void rt_remote_worker_init(void);
void rt_remote_worker_tick(void);
void rt_remote_worker_shutdown(void);
RTRemoteWorkerState rt_remote_worker_state(void);
int rt_remote_worker_configure(const RTRemoteWorkerConfig *config, char *error, size_t error_cap);
int rt_remote_worker_ui_allowed(void);
char *rt_remote_worker_json(void);

#endif
