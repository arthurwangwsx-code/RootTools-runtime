#include "control_plane.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define RT_DEFAULT_POLICY_DIR "/var/mobile/Library/RootTools/disabled-capabilities"

static const RTCapability kCapabilities[] = {
    {"device.status.observe",      NULL,                "Device and daemon health",              RT_RISK_R0, 0, 1, 1},
    {"device.runtime.observe",     NULL,                "Jailbreak runtime state",               RT_RISK_R0, 0, 1, 1},
    {"device.runtime.adapters",    NULL,                "Structured runtime adapter state",      RT_RISK_R0, 0, 1, 1},
    {"device.runtime.frida.observe",NULL,               "Frida server and package facts",         RT_RISK_R0, 0, 1, 1},
    {"device.runtime.ellekit.observe",NULL,             "ElleKit component and package facts",    RT_RISK_R0, 0, 1, 1},
    {"device.providers.read",      NULL,                "Provider registry and capability bindings", RT_RISK_R0, 0, 1, 1},
    {"device.package.plan",        NULL,                "Resolve package format to a provider",  RT_RISK_R0, 0, 1, 1},
    {"device.package.list",        NULL,                "List RootTools staged packages",        RT_RISK_R0, 0, 1, 1},
    {"device.package.history",     NULL,                "Package install lifecycle history",      RT_RISK_R0, 0, 1, 1},
    {"device.self-update.status",  NULL,                "RootTools independent update status",    RT_RISK_R0, 0, 1, 1},
    {"device.lock.observe",        NULL,                "Lock and display readiness state",       RT_RISK_R0, 0, 1, 1},
    {"device.automation.observe",  NULL,                "Headless and UI automation readiness",  RT_RISK_R0, 0, 1, 1},
    {"device.automation.queue.read",NULL,               "Deferred automation job queue",          RT_RISK_R0, 0, 1, 1},
    {"device.task.list",           NULL,                "Durable device task ledger",              RT_RISK_R0, 0, 1, 1},
    {"device.ui.screen-info",      NULL,                "Screen geometry through UI adapter",    RT_RISK_R0, 0, 1, 1},
    {"device.ui.observe",          NULL,                "UI readiness and screen geometry",        RT_RISK_R0, 0, 1, 1},
    {"device.app.list",            NULL,                "Installed application inventory",       RT_RISK_R0, 0, 1, 1},
    {"device.app.inspect",         NULL,                "Inspect one application",               RT_RISK_R0, 0, 1, 1},
    {"device.process.list",        NULL,                "Process inventory",                     RT_RISK_R0, 0, 1, 1},
    {"device.process.inspect",     NULL,                "Inspect one process",                   RT_RISK_R0, 0, 1, 1},
    {"device.permission.tcc",      NULL,                "Inspect iOS TCC authorization facts",   RT_RISK_R0, 0, 1, 1},
    {"device.fs.observe",          NULL,                "Selected filesystem views",             RT_RISK_R0, 0, 1, 1},
    {"device.fs.list",             NULL,                "List one declared RootTools file scope",RT_RISK_R0, 0, 1, 1},
    {"device.fs.scopes",           NULL,                "Declared filesystem scopes",            RT_RISK_R0, 0, 1, 1},
    {"device.network.observe",     NULL,                "Network interfaces and adapters",       RT_RISK_R0, 0, 1, 1},
    {"device.diagnostics.observe", NULL,                "Privileged diagnostics snapshot",       RT_RISK_R0, 0, 1, 1},
    {"device.audit.read",          NULL,                "Privileged action receipts",             RT_RISK_R0, 0, 1, 1},
    {"device.events.read",         NULL,                "Execution lifecycle event replay",       RT_RISK_R0, 0, 1, 1},
    {"device.principal.list",      NULL,                "Trusted command principals",             RT_RISK_R0, 0, 1, 1},
    {"device.principal.grants.read",NULL,               "Trusted principal capability grants",    RT_RISK_R0, 0, 1, 1},
    {"device.fs.read",             "file.read",         "Read within RootTools file scopes",      RT_RISK_R0, 0, 1, 1},
    {"device.app.launch",          "app.launch",        "Launch an installed application",       RT_RISK_R1, 0, 1, 1},
    {"device.app.terminate",       "app.terminate",     "Terminate an installed application",    RT_RISK_R1, 0, 1, 1},
    {"device.automation.queue-app-launch", "automation.queue-app-launch", "Queue app launch until UI is ready", RT_RISK_R1, 0, 1, 1},
    {"device.automation.cancel",   "automation.cancel", "Cancel a pending automation job",       RT_RISK_R1, 0, 1, 1},
    {"device.task.submit-app-launch","task.submit-app-launch","Submit durable app launch task",   RT_RISK_R1, 0, 1, 1},
    {"device.task.cancel",         "task.cancel",       "Cancel a queued device task",            RT_RISK_R1, 0, 1, 1},
    {"device.ui.tap",              "ui.tap",            "Queue a typed screen tap",                RT_RISK_R1, 0, 1, 1},
    {"device.ui.type",             "ui.type",           "Queue typed text insertion",              RT_RISK_R1, 0, 1, 1},
    {"device.ui.swipe",            "ui.swipe",          "Queue a typed screen swipe",              RT_RISK_R1, 0, 1, 1},
    {"device.fs.write",            "file.write",        "Write within RootTools file scopes",     RT_RISK_R1, 0, 1, 1},
    {"device.package.stage.begin", "package.stage.begin","Create a bounded package staging slot", RT_RISK_R1, 0, 1, 1},
    {"device.package.stage.chunk", "package.stage.chunk","Append a bounded package chunk",        RT_RISK_R1, 0, 1, 1},
    {"device.package.stage.commit","package.stage.commit","Verify staged package SHA-256",        RT_RISK_R1, 0, 1, 1},
    {"device.package.discard",     "package.discard",   "Discard a staged package",              RT_RISK_R1, 0, 1, 1},
    {"device.process.terminate",   "process.terminate", "SIGTERM an allowed non-root process",    RT_RISK_R2, 1, 0, 1},
    {"device.agent.rotate",        "agent.rotate",      "Rotate trusted Agent credential",       RT_RISK_R2, 1, 0, 1},
    {"device.principal.create",    "principal.create",  "Create trusted command principal",      RT_RISK_R2, 1, 0, 1},
    {"device.principal.revoke",    "principal.revoke",  "Revoke trusted command principal",      RT_RISK_R2, 1, 0, 1},
    {"device.principal.grant",     "principal.grant",   "Grant R0/R1 capability to principal",    RT_RISK_R2, 1, 0, 1},
    {"device.principal.ungrant",   "principal.ungrant", "Remove capability grant from principal",RT_RISK_R2, 1, 0, 1},
    {"device.package.install-deb", "package.install-deb","Install a verified DEB with Procursus", RT_RISK_R2, 1, 0, 1},
    {"device.package.install-ipa", "package.install-ipa","Install a verified IPA/TIPA with TrollStore", RT_RISK_R2, 1, 0, 1},
    {"device.package.rollback-deb","package.rollback-deb","Rollback to a retained DEB artifact", RT_RISK_R2, 1, 0, 1},
    {"device.package.rollback-ipa","package.rollback-ipa","Rollback to a retained IPA/TIPA artifact", RT_RISK_R2, 1, 0, 1},
    {"device.package.uninstall-deb","package.uninstall-deb","Uninstall a managed DEB package",    RT_RISK_R2, 1, 0, 1},
    {"device.package.uninstall-ipa","package.uninstall-ipa","Uninstall a managed TrollStore app", RT_RISK_R2, 1, 0, 1},
    {"device.self-update.schedule","self-update.schedule","Schedule verified RootTools replacement", RT_RISK_R2, 1, 0, 1},
    {"device.raw-shell",           NULL,                "Arbitrary privileged shell",             RT_RISK_R3, 1, 0, 0},
    {"device.critical-control",    NULL,                "Device-critical privileged operations",  RT_RISK_R3, 1, 0, 0},
};

const RTCapability *rt_capability_find(const char *id) {
    if (!id) return NULL;
    for (size_t i = 0; i < sizeof(kCapabilities) / sizeof(kCapabilities[0]); i++) {
        if (!strcmp(kCapabilities[i].id, id)) return &kCapabilities[i];
    }
    return NULL;
}

const RTCapability *rt_capability_find_action(const char *legacy_action) {
    if (!legacy_action) return NULL;
    for (size_t i = 0; i < sizeof(kCapabilities) / sizeof(kCapabilities[0]); i++) {
        if (kCapabilities[i].legacy_action && !strcmp(kCapabilities[i].legacy_action, legacy_action)) return &kCapabilities[i];
    }
    return NULL;
}

const RTCapability *rt_capability_at(size_t index) {
    size_t count = sizeof(kCapabilities) / sizeof(kCapabilities[0]);
    return index < count ? &kCapabilities[index] : NULL;
}

size_t rt_capability_count(void) {
    return sizeof(kCapabilities) / sizeof(kCapabilities[0]);
}

const char *rt_risk_name(RTRiskLevel risk) {
    switch (risk) {
        case RT_RISK_R0: return "R0";
        case RT_RISK_R1: return "R1";
        case RT_RISK_R2: return "R2";
        case RT_RISK_R3: return "R3";
    }
    return "R3";
}

static const char *rt_policy_dir(void) {
    const char *override = getenv("ROOTTOOLS_POLICY_DIR");
    return (override && override[0]) ? override : RT_DEFAULT_POLICY_DIR;
}

static int rt_capability_policy_path(const RTCapability *capability, char *out, size_t cap) {
    if (!capability || !out || cap < 8) return 0;
    for (const char *p = capability->id; *p; p++) {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_')) return 0;
    }
    int n = snprintf(out, cap, "%s/%s", rt_policy_dir(), capability->id);
    return n > 0 && (size_t)n < cap;
}

int rt_capability_effective_enabled(const RTCapability *capability) {
    if (!capability || !capability->enabled || capability->risk == RT_RISK_R3) return 0;
    char path[1024];
    if (!rt_capability_policy_path(capability, path, sizeof(path))) return 0;
    return access(path, F_OK) != 0;
}

int rt_capability_set_enabled(const char *id, int enabled) {
    const RTCapability *capability = rt_capability_find(id);
    if (!capability) return 0;
    // Owner policy may only narrow or restore the compiled product surface.
    // It can never make R3/raw-shell executable at runtime.
    if (!capability->enabled || capability->risk == RT_RISK_R3) return enabled ? 0 : 1;

    char path[1024];
    if (!rt_capability_policy_path(capability, path, sizeof(path))) return 0;
    if (enabled) {
        if (unlink(path) == 0 || errno == ENOENT) return 1;
        return 0;
    }

    if (mkdir(rt_policy_dir(), 0750) != 0 && errno != EEXIST) return 0;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (fd < 0) return 0;
    size_t length = strlen(id);
    ssize_t written = write(fd, id, length);
    int sync_ok = fsync(fd) == 0;
    close(fd);
    return written == (ssize_t)length && sync_ok;
}

RTPolicyDecision rt_policy_evaluate(const RTCapability *capability, int confirmed) {
    if (!capability) return (RTPolicyDecision){0, 0, "deny", "unknown capability"};
    if (!capability->enabled || capability->risk == RT_RISK_R3) {
        return (RTPolicyDecision){0, 0, "deny", "capability blocked by daemon policy"};
    }
    if (!rt_capability_effective_enabled(capability)) {
        return (RTPolicyDecision){0, 0, "deny", "capability disabled by device owner policy"};
    }
    if (capability->requires_confirmation && !confirmed) {
        return (RTPolicyDecision){0, 1, "confirmation_required", "explicit confirmation required by daemon policy"};
    }
    return (RTPolicyDecision){1, 0, "allow", "allowed"};
}

char *rt_capabilities_text(void) {
    char *out = calloc(1, 24576);
    if (!out) return NULL;
    size_t used = 0;
    for (size_t i = 0; i < rt_capability_count(); i++) {
        const RTCapability *cap = rt_capability_at(i);
        int n = snprintf(out + used, 24576 - used,
            "%s  %-28s enabled=%s hard=%s confirm=%s reversible=%s  %s\n",
            rt_risk_name(cap->risk), cap->id,
            rt_capability_effective_enabled(cap) ? "yes" : "no",
            cap->enabled ? "yes" : "no",
            cap->requires_confirmation ? "yes" : "no",
            cap->reversible ? "yes" : "no",
            cap->title);
        if (n < 0 || (size_t)n >= 24576 - used) break;
        used += (size_t)n;
    }
    return out;
}

char *rt_capabilities_json(void) {
    char *out = calloc(1, 32768);
    if (!out) return NULL;
    size_t used = 0;
    int n = snprintf(out, 32768, "{\"schemaVersion\":1,\"capabilities\":[");
    if (n < 0) { free(out); return NULL; }
    used = (size_t)n;

    int r3_exposed = 0;
    int raw_shell_exposed = 0;
    for (size_t i = 0; i < rt_capability_count(); i++) {
        const RTCapability *cap = rt_capability_at(i);
        int effective_enabled = rt_capability_effective_enabled(cap);
        if (effective_enabled && cap->risk == RT_RISK_R3) r3_exposed = 1;
        if (effective_enabled && !strcmp(cap->id, "device.raw-shell")) raw_shell_exposed = 1;
        n = snprintf(out + used, 32768 - used,
            "%s{\"id\":\"%s\",\"legacyAction\":%s%s%s,\"title\":\"%s\",\"risk\":\"%s\",\"requiresConfirmation\":%s,\"reversible\":%s,\"hardEnabled\":%s,\"enabled\":%s}",
            i ? "," : "",
            cap->id,
            cap->legacy_action ? "\"" : "null",
            cap->legacy_action ? cap->legacy_action : "",
            cap->legacy_action ? "\"" : "",
            cap->title,
            rt_risk_name(cap->risk),
            cap->requires_confirmation ? "true" : "false",
            cap->reversible ? "true" : "false",
            cap->enabled ? "true" : "false",
            effective_enabled ? "true" : "false");
        if (n < 0 || (size_t)n >= 32768 - used) { free(out); return NULL; }
        used += (size_t)n;
    }

    n = snprintf(out + used, 32768 - used,
        "],\"invariants\":{\"r3Exposed\":%s,\"rawPrivilegedShellExposed\":%s}}",
        r3_exposed ? "true" : "false",
        raw_shell_exposed ? "true" : "false");
    if (n < 0 || (size_t)n >= 32768 - used) { free(out); return NULL; }
    return out;
}
