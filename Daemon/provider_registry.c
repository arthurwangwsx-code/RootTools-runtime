#include "provider_registry.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

static const RTProvider kProviders[] = {
    {"roottools.execd", "RootTools privileged daemon", RT_PROVIDER_CONTROL, "roottools.uid0-daemon", 100, 1, 0, 1, RT_PROVIDER_PROBE_ALWAYS, NULL, 0},
    {"roottools.updater", "RootTools independent updater", RT_PROVIDER_CONTROL, "roottools.one-shot-updater", 100, 1, 0, 1, RT_PROVIDER_PROBE_PATH, "/var/jb/usr/local/bin/roottools-updater", 0},
    {"ios.darwin", "Darwin native APIs", RT_PROVIDER_NATIVE, "ios.darwin", 100, 1, 0, 1, RT_PROVIDER_PROBE_ALWAYS, NULL, 0},
    {"jailbreak.dopamine", "Dopamine rootless jailbreak", RT_PROVIDER_JAILBREAK, "dopamine.rootless", 100, 1, 0, 1, RT_PROVIDER_PROBE_PATH, "/var/jb", 0},
    {"bootstrap.procursus", "Procursus rootless bootstrap", RT_PROVIDER_JAILBREAK, "procursus.rootless", 95, 1, 0, 1, RT_PROVIDER_PROBE_PATH, "/var/jb/usr/bin/dpkg", 0},
    {"package.sileo", "Sileo package manager", RT_PROVIDER_PACKAGE, "sileo.deb", 90, 0, 1, 0, RT_PROVIDER_PROBE_APP_BUNDLE, "Sileo.app", 0},
    {"package.trollstore", "TrollStore application installer", RT_PROVIDER_PACKAGE, "trollstore.ipa", 90, 0, 1, 0, RT_PROVIDER_PROBE_APP_BUNDLE, "TrollStore.app", 0},
    {"runtime.frida", "Frida runtime instrumentation", RT_PROVIDER_RUNTIME, "frida-server", 80, 1, 0, 1, RT_PROVIDER_PROBE_PORT, NULL, 27042},
    {"runtime.ellekit", "ElleKit tweak runtime", RT_PROVIDER_RUNTIME, "ellekit", 75, 1, 0, 1, RT_PROVIDER_PROBE_PATH, "/var/jb/usr/lib/libellekit.dylib", 0},
    {"transport.openssh", "OpenSSH transport", RT_PROVIDER_TRANSPORT, "openssh", 70, 1, 0, 1, RT_PROVIDER_PROBE_PORT, NULL, 22},
    {"ui.springboard", "SpringBoard application control", RT_PROVIDER_UI, "uiopen", 90, 0, 1, 1, RT_PROVIDER_PROBE_PATH, "/var/jb/usr/bin/uiopen", 0},
    {"ui.zxtouch", "ZXTouch input adapter", RT_PROVIDER_UI, "zxtouch", 70, 0, 1, 1, RT_PROVIDER_PROBE_PORT, NULL, 6000},
    {"permission.tcc", "iOS TCC database", RT_PROVIDER_PERMISSION, "ios.tcc.sqlite", 100, 1, 0, 1, RT_PROVIDER_PROBE_PATH, "/var/mobile/Library/TCC/TCC.db", 0},
};

typedef struct {
    const char *capability_id;
    const char *provider_id;
} RTProviderBinding;

static const RTProviderBinding kBindings[] = {
    {"device.status.observe", "roottools.execd"},
    {"device.runtime.observe", "jailbreak.dopamine"},
    {"device.runtime.adapters", "roottools.execd"},
    {"device.runtime.frida.observe", "runtime.frida"},
    {"device.runtime.ellekit.observe", "runtime.ellekit"},
    {"device.providers.read", "roottools.execd"},
    {"device.package.plan", "roottools.execd"},
    {"device.package.list", "roottools.execd"},
    {"device.package.history", "roottools.execd"},
    {"device.self-update.status", "roottools.execd"},
    {"device.lock.observe", "ios.darwin"},
    {"device.automation.observe", "roottools.execd"},
    {"device.automation.queue.read", "roottools.execd"},
    {"device.task.list", "roottools.execd"},
    {"device.ui.screen-info", "ui.zxtouch"},
    {"device.app.list", "ios.darwin"},
    {"device.app.inspect", "ios.darwin"},
    {"device.process.list", "ios.darwin"},
    {"device.process.inspect", "ios.darwin"},
    {"device.permission.tcc", "permission.tcc"},
    {"device.fs.observe", "ios.darwin"},
    {"device.fs.list", "ios.darwin"},
    {"device.fs.scopes", "roottools.execd"},
    {"device.network.observe", "ios.darwin"},
    {"device.diagnostics.observe", "roottools.execd"},
    {"device.audit.read", "roottools.execd"},
    {"device.events.read", "roottools.execd"},
    {"device.principal.list", "roottools.execd"},
    {"device.principal.grants.read", "roottools.execd"},
    {"device.fs.read", "ios.darwin"},
    {"device.fs.write", "ios.darwin"},
    {"device.package.stage.begin", "roottools.execd"},
    {"device.package.stage.chunk", "roottools.execd"},
    {"device.package.stage.commit", "roottools.execd"},
    {"device.package.discard", "roottools.execd"},
    {"device.app.launch", "ui.springboard"},
    {"device.app.terminate", "ui.springboard"},
    {"device.automation.queue-app-launch", "ui.springboard"},
    {"device.automation.cancel", "roottools.execd"},
    {"device.task.submit-app-launch", "roottools.execd"},
    {"device.task.cancel", "roottools.execd"},
    {"device.process.terminate", "ios.darwin"},
    {"device.agent.rotate", "roottools.execd"},
    {"device.principal.create", "roottools.execd"},
    {"device.principal.revoke", "roottools.execd"},
    {"device.principal.grant", "roottools.execd"},
    {"device.principal.ungrant", "roottools.execd"},
    {"device.package.install-deb", "bootstrap.procursus"},
    {"device.package.install-ipa", "package.trollstore"},
    {"device.package.rollback-deb", "bootstrap.procursus"},
    {"device.package.rollback-ipa", "package.trollstore"},
    {"device.package.uninstall-deb", "bootstrap.procursus"},
    {"device.package.uninstall-ipa", "package.trollstore"},
    {"device.self-update.schedule", "roottools.updater"},
};

static int port_open(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int flags = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &flags, sizeof(flags));
    struct timeval tv = {.tv_sec = 0, .tv_usec = 120000};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    int rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    close(fd);
    return rc == 0;
}

static int find_bundle_in_root(const char *root, const char *bundle_name, int nested, char *out, size_t cap) {
    DIR *dir = opendir(root);
    if (!dir) return 0;
    struct dirent *entry;
    int found = 0;
    while (!found && (entry = readdir(dir))) {
        if (entry->d_name[0] == '.') continue;
        if (!strcmp(entry->d_name, bundle_name)) {
            if(out&&cap)snprintf(out,cap,"%s/%s",root,entry->d_name);
            found = 1; break;
        }
        if (!nested) continue;
        char child[1024];
        int n = snprintf(child, sizeof(child), "%s/%s", root, entry->d_name);
        if (n <= 0 || (size_t)n >= sizeof(child)) continue;
        DIR *sub = opendir(child);
        if (!sub) continue;
        struct dirent *subentry;
        while ((subentry = readdir(sub))) {
            if (!strcmp(subentry->d_name, bundle_name)) {
                if(out&&cap)snprintf(out,cap,"%s/%s",child,subentry->d_name);
                found = 1; break;
            }
        }
        closedir(sub);
    }
    closedir(dir);
    return found;
}

static int app_bundle_exists(const char *bundle_name) {
    return find_bundle_in_root("/var/jb/Applications", bundle_name, 0, NULL, 0) ||
           find_bundle_in_root("/Applications", bundle_name, 0, NULL, 0) ||
           find_bundle_in_root("/var/containers/Bundle/Application", bundle_name, 1, NULL, 0);
}

static int find_app_bundle(const char *bundle_name, char *out, size_t cap) {
    return find_bundle_in_root("/var/jb/Applications", bundle_name, 0, out, cap) ||
           find_bundle_in_root("/Applications", bundle_name, 0, out, cap) ||
           find_bundle_in_root("/var/containers/Bundle/Application", bundle_name, 1, out, cap);
}

const RTProvider *rt_provider_find(const char *id) {
    if (!id) return NULL;
    for (size_t i = 0; i < sizeof(kProviders) / sizeof(kProviders[0]); i++) {
        if (!strcmp(kProviders[i].id, id)) return &kProviders[i];
    }
    return NULL;
}

const RTProvider *rt_provider_at(size_t index) {
    size_t count = sizeof(kProviders) / sizeof(kProviders[0]);
    return index < count ? &kProviders[index] : NULL;
}

size_t rt_provider_count(void) {
    return sizeof(kProviders) / sizeof(kProviders[0]);
}

const char *rt_provider_domain_name(RTProviderDomain domain) {
    switch (domain) {
        case RT_PROVIDER_CONTROL: return "control";
        case RT_PROVIDER_NATIVE: return "native";
        case RT_PROVIDER_JAILBREAK: return "jailbreak";
        case RT_PROVIDER_PACKAGE: return "package";
        case RT_PROVIDER_RUNTIME: return "runtime";
        case RT_PROVIDER_TRANSPORT: return "transport";
        case RT_PROVIDER_UI: return "ui";
        case RT_PROVIDER_PERMISSION: return "permission";
    }
    return "unknown";
}

int rt_provider_available(const RTProvider *provider) {
    if (!provider) return 0;
    if (!strcmp(provider->id,"bootstrap.procursus") ||
        !strcmp(provider->id,"package.trollstore") ||
        !strcmp(provider->id,"ui.springboard")) {
        char executable[1024]={0};
        return rt_provider_resolve_executable(provider->id,executable,sizeof(executable));
    }
    switch (provider->probe_kind) {
        case RT_PROVIDER_PROBE_ALWAYS: return 1;
        case RT_PROVIDER_PROBE_PATH: return provider->probe_value && access(provider->probe_value, F_OK) == 0;
        case RT_PROVIDER_PROBE_PORT: return provider->probe_port > 0 && port_open(provider->probe_port);
        case RT_PROVIDER_PROBE_APP_BUNDLE: return provider->probe_value && app_bundle_exists(provider->probe_value);
    }
    return 0;
}

int rt_provider_resolve_executable(const char *id, char *out, size_t cap) {
    if(!id||!out||cap<8)return 0;
    out[0]=0;
    if(!strcmp(id,"bootstrap.procursus")){
        snprintf(out,cap,"/var/jb/usr/bin/dpkg");
        return access(out,X_OK)==0;
    }
    if(!strcmp(id,"package.trollstore")){
        char app[1024]={0};
        if(!find_app_bundle("TrollStore.app",app,sizeof(app)))return 0;
        int n=snprintf(out,cap,"%s/trollstorehelper",app);
        return n>0&&(size_t)n<cap&&access(out,X_OK)==0;
    }
    if(!strcmp(id,"ui.springboard")){
        snprintf(out,cap,"/var/jb/usr/bin/uiopen");
        return access(out,X_OK)==0;
    }
    if(!strcmp(id,"roottools.updater")){
        snprintf(out,cap,"/var/jb/usr/local/bin/roottools-updater");
        return access(out,X_OK)==0;
    }
    return 0;
}

const char *rt_provider_for_capability(const char *capability_id) {
    if (!capability_id) return NULL;
    for (size_t i = 0; i < sizeof(kBindings) / sizeof(kBindings[0]); i++) {
        if (!strcmp(kBindings[i].capability_id, capability_id)) return kBindings[i].provider_id;
    }
    return NULL;
}

char *rt_providers_json(void) {
    char *out = calloc(1, 32768);
    if (!out) return NULL;
    size_t used = 0;
    int n = snprintf(out, 32768, "{\"schemaVersion\":1,\"providers\":[");
    if (n < 0) { free(out); return NULL; }
    used = (size_t)n;
    for (size_t i = 0; i < rt_provider_count(); i++) {
        const RTProvider *provider = rt_provider_at(i);
        int available = rt_provider_available(provider);
        n = snprintf(out + used, 32768 - used,
            "%s{\"id\":\"%s\",\"title\":\"%s\",\"domain\":\"%s\",\"implementation\":\"%s\",\"priority\":%d,\"state\":\"%s\",\"supportsHeadless\":%s,\"requiresUnlock\":%s,\"survivesAppExit\":%s}",
            i ? "," : "", provider->id, provider->title, rt_provider_domain_name(provider->domain),
            provider->implementation, provider->priority, available ? "available" : "unavailable",
            provider->supports_headless ? "true" : "false",
            provider->requires_unlock ? "true" : "false",
            provider->survives_app_exit ? "true" : "false");
        if (n < 0 || (size_t)n >= 32768 - used) { free(out); return NULL; }
        used += (size_t)n;
    }
    n = snprintf(out + used, 32768 - used, "],\"bindings\":[");
    if (n < 0 || (size_t)n >= 32768 - used) { free(out); return NULL; }
    used += (size_t)n;
    for (size_t i = 0; i < sizeof(kBindings) / sizeof(kBindings[0]); i++) {
        const RTProvider *provider = rt_provider_find(kBindings[i].provider_id);
        n = snprintf(out + used, 32768 - used,
            "%s{\"capabilityId\":\"%s\",\"providerId\":\"%s\",\"providerAvailable\":%s}",
            i ? "," : "", kBindings[i].capability_id, kBindings[i].provider_id,
            provider && rt_provider_available(provider) ? "true" : "false");
        if (n < 0 || (size_t)n >= 32768 - used) { free(out); return NULL; }
        used += (size_t)n;
    }
    n = snprintf(out + used, 32768 - used, "]}");
    if (n < 0 || (size_t)n >= 32768 - used) { free(out); return NULL; }
    return out;
}

char *rt_package_plan_json(const char *format) {
    if (!format) return NULL;
    const char *primary_id = NULL;
    const char *fallback_id = NULL;
    const char *mode = NULL;
    if (!strcmp(format, "deb")) {
        primary_id = "bootstrap.procursus";
        fallback_id = "package.sileo";
        mode = "rootless-package";
    } else if (!strcmp(format, "ipa") || !strcmp(format, "tipa")) {
        primary_id = "package.trollstore";
        mode = "persistent-app";
    } else {
        return NULL;
    }
    const RTProvider *primary = rt_provider_find(primary_id);
    const RTProvider *fallback = fallback_id ? rt_provider_find(fallback_id) : NULL;
    int primary_ready = primary && rt_provider_available(primary);
    int fallback_ready = fallback && rt_provider_available(fallback);
    char *out = calloc(1, 2048);
    if (!out) return NULL;
    snprintf(out, 2048,
        "{\"schemaVersion\":1,\"format\":\"%s\",\"mode\":\"%s\",\"selectedProviderId\":\"%s\",\"ready\":%s,\"requiresOwnerConfirmation\":true,\"fallbackProviderId\":%s%s%s,\"fallbackReady\":%s,\"policy\":{\"rawShell\":false,\"arbitraryExecutable\":false,\"typedPackageOnly\":true}}",
        format, mode, primary_id, primary_ready ? "true" : "false",
        fallback_id ? "\"" : "null", fallback_id ? fallback_id : "", fallback_id ? "\"" : "",
        fallback_ready ? "true" : "false");
    return out;
}
