#ifndef ROOTTOOLS_REMOTE_ACCESS_CONTROLLER_H
#define ROOTTOOLS_REMOTE_ACCESS_CONTROLLER_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int enabled;
    int transport_available;
    int listener_active;
    int port;
    int64_t expires_at;
    char principal_id[128];
    char bind_address[64];
    char listener_error[192];
} RTRemoteAccessState;

void rt_remote_access_init(void);
void rt_remote_access_tick(void);
int rt_remote_access_snapshot(RTRemoteAccessState *state);
int rt_remote_access_configure(
    int enabled,
    const char *principal_id,
    int duration_minutes,
    char *error_out,
    size_t error_cap
);
int rt_remote_access_allows_principal(const char *principal_id);
void rt_remote_access_set_listener_status(int active, const char *error);
char *rt_remote_access_json(void);

#endif
