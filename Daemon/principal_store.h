#ifndef ROOTTOOLS_PRINCIPAL_STORE_H
#define ROOTTOOLS_PRINCIPAL_STORE_H

#include <stddef.h>

#define RT_PRINCIPAL_ID_CAP 96
#define RT_PRINCIPAL_KIND_CAP 32
#define RT_PRINCIPAL_NAME_CAP 128
#define RT_PRINCIPAL_TOKEN_CAP 64

int rt_principal_create(
    const char *principal_id,
    const char *kind,
    const char *display_name,
    char *token_out,
    size_t token_cap,
    char *error_out,
    size_t error_cap
);

int rt_principal_revoke(
    const char *principal_id,
    char *error_out,
    size_t error_cap
);

int rt_principal_authenticate(
    const char *token,
    char *principal_id_out,
    size_t principal_id_cap,
    char *kind_out,
    size_t kind_cap
);
int rt_principal_active_kind(const char *principal_id, char *kind_out, size_t kind_cap);

int rt_principal_grant(
    const char *principal_id,
    const char *capability_id,
    long long expires_at,
    char *error_out,
    size_t error_cap
);

int rt_principal_ungrant(
    const char *principal_id,
    const char *capability_id,
    char *error_out,
    size_t error_cap
);

int rt_principal_capability_allowed(
    const char *principal_id,
    const char *capability_id
);

char *rt_principal_grants_json(const char *principal_id);
char *rt_principals_json(void);

#endif
