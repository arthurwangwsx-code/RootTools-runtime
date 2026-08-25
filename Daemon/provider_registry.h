#ifndef ROOTTOOLS_PROVIDER_REGISTRY_H
#define ROOTTOOLS_PROVIDER_REGISTRY_H

#include <stddef.h>

typedef enum {
    RT_PROVIDER_CONTROL = 0,
    RT_PROVIDER_NATIVE = 1,
    RT_PROVIDER_JAILBREAK = 2,
    RT_PROVIDER_PACKAGE = 3,
    RT_PROVIDER_RUNTIME = 4,
    RT_PROVIDER_TRANSPORT = 5,
    RT_PROVIDER_UI = 6,
    RT_PROVIDER_PERMISSION = 7,
} RTProviderDomain;

typedef enum {
    RT_PROVIDER_PROBE_ALWAYS = 0,
    RT_PROVIDER_PROBE_PATH = 1,
    RT_PROVIDER_PROBE_PORT = 2,
    RT_PROVIDER_PROBE_APP_BUNDLE = 3,
} RTProviderProbeKind;

typedef struct {
    const char *id;
    const char *title;
    RTProviderDomain domain;
    const char *implementation;
    int priority;
    int supports_headless;
    int requires_unlock;
    int survives_app_exit;
    RTProviderProbeKind probe_kind;
    const char *probe_value;
    int probe_port;
} RTProvider;

const RTProvider *rt_provider_find(const char *id);
const RTProvider *rt_provider_at(size_t index);
size_t rt_provider_count(void);
const char *rt_provider_domain_name(RTProviderDomain domain);
int rt_provider_available(const RTProvider *provider);
const char *rt_provider_for_capability(const char *capability_id);
char *rt_providers_json(void);
char *rt_package_plan_json(const char *format);

#endif
