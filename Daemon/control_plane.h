#ifndef ROOTTOOLS_CONTROL_PLANE_H
#define ROOTTOOLS_CONTROL_PLANE_H

#include <stddef.h>

typedef enum {
    RT_RISK_R0 = 0,
    RT_RISK_R1 = 1,
    RT_RISK_R2 = 2,
    RT_RISK_R3 = 3,
} RTRiskLevel;

typedef struct {
    const char *id;
    const char *legacy_action;
    const char *title;
    RTRiskLevel risk;
    int requires_confirmation;
    int reversible;
    int enabled;
} RTCapability;

typedef struct {
    int allowed;
    int confirmation_required;
    const char *policy;
    const char *reason;
} RTPolicyDecision;

const RTCapability *rt_capability_find(const char *id);
const RTCapability *rt_capability_find_action(const char *legacy_action);
const RTCapability *rt_capability_at(size_t index);
size_t rt_capability_count(void);
const char *rt_risk_name(RTRiskLevel risk);
int rt_capability_effective_enabled(const RTCapability *capability);
int rt_capability_set_enabled(const char *id, int enabled);
RTPolicyDecision rt_policy_evaluate(const RTCapability *capability, int confirmed);
char *rt_capabilities_text(void);
char *rt_capabilities_json(void);

#endif
