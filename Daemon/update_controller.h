#ifndef ROOTTOOLS_UPDATE_CONTROLLER_H
#define ROOTTOOLS_UPDATE_CONTROLLER_H

#include <stddef.h>

typedef struct {
    int ok;
    int executed;
    int post_checked;
    int post_passed;
    char result[64];
    char message[512];
    char post_detail[512];
    char output[128];
} RTUpdateOperation;

typedef struct {
    char request_id[128];
    char package_id[96];
    char state[32];
    char target_version[128];
    char result[128];
    char error[512];
    long long created_at;
    long long updated_at;
} RTUpdateInfo;

int rt_update_schedule(const char *request_id, const char *package_id, RTUpdateOperation *operation);
int rt_update_peek_pending(char *request_id, size_t cap);
int rt_update_claim_pending(char *request_id, size_t cap);
int rt_update_get(const char *request_id, RTUpdateInfo *info);
int rt_update_mark(const char *request_id, const char *state, const char *target_version,
                   const char *result, const char *error);
char *rt_updates_json(void);

#endif
