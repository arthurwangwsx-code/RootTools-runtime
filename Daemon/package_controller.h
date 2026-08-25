#ifndef ROOTTOOLS_PACKAGE_CONTROLLER_H
#define ROOTTOOLS_PACKAGE_CONTROLLER_H

#include <stddef.h>

#define RT_PACKAGE_ID_CAP 96
#define RT_PACKAGE_NAME_CAP 192
#define RT_PACKAGE_FORMAT_CAP 16
#define RT_PACKAGE_IDENTIFIER_CAP 256
#define RT_PACKAGE_SHA256_CAP 65
#define RT_PACKAGE_STATE_CAP 32

typedef struct {
    char package_id[RT_PACKAGE_ID_CAP];
    char name[RT_PACKAGE_NAME_CAP];
    char format[RT_PACKAGE_FORMAT_CAP];
    char expected_identifier[RT_PACKAGE_IDENTIFIER_CAP];
    char sha256[RT_PACKAGE_SHA256_CAP];
    char state[RT_PACKAGE_STATE_CAP];
    long long total_size;
    long long received_size;
} RTPackageInfo;

typedef struct {
    int ok;
    int executed;
    int post_checked;
    int post_passed;
    char result[64];
    char message[1024];
    char post_detail[1024];
    char output[1024];
} RTPackageOperation;

int rt_package_begin(const char *package_id, const char *name, const char *format,
                     const char *expected_identifier, long long total_size,
                     const char *sha256, RTPackageOperation *operation);
int rt_package_append(const char *package_id, long long offset,
                      const unsigned char *bytes, size_t length,
                      RTPackageOperation *operation);
int rt_package_commit(const char *package_id, RTPackageOperation *operation);
int rt_package_discard(const char *package_id, RTPackageOperation *operation);
int rt_package_install_deb(const char *package_id, RTPackageOperation *operation);
int rt_package_install_ipa(const char *package_id, RTPackageOperation *operation);
int rt_package_rollback_deb(const char *package_id, RTPackageOperation *operation);
int rt_package_rollback_ipa(const char *package_id, RTPackageOperation *operation);
int rt_package_uninstall_deb(const char *package_id, RTPackageOperation *operation);
int rt_package_uninstall_ipa(const char *package_id, RTPackageOperation *operation);
int rt_package_get(const char *package_id, RTPackageInfo *info);
int rt_package_resolve_artifact(const char *package_id, const char *required_state,
                                RTPackageInfo *info, char *path, size_t path_cap);
int rt_package_deb_field(const char *package_id, const char *field, char *out, size_t cap);
int rt_package_mark_external_install(const char *package_id, const char *action,
                                     const char *provider_id);
char *rt_packages_json(void);
char *rt_installed_packages_json(void);
char *rt_package_history_json(void);

#endif
