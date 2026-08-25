# P3 — Jailbreak / Runtime Provider Platform

## Objective

Make jailbreak/runtime mechanisms replaceable implementation providers behind stable RootTools Device Ops.

## v0.5 implemented foundation

### Provider registry

- daemon-owned provider metadata and discovery;
- bounded probes for path, loopback service, and installed application presence;
- provider domains and priority metadata;
- capability -> provider binding table;
- authoritative `GET /v1/providers/catalog` endpoint.

### Router integration

- immediate semantic actions fail closed when their required provider is unavailable;
- deferred UI jobs may be persisted before the provider becomes ready;
- ActionReceipt and append-only audit records include `providerId`;
- provider choice cannot be supplied by the caller.

### Package provider planning

- `POST /v1/package/plan` supports `deb`, `ipa`, and `tipa`;
- DEB resolves to Procursus/dpkg with Sileo as interactive fallback;
- IPA/TIPA resolves to TrollStore;
- owner confirmation is part of the plan contract;
- raw shell/arbitrary executable execution remains excluded.

### v0.6 Package Controller

- RootTools-owned package staging directory and SQLite catalog;
- maximum package size 2 GiB and maximum decoded chunk size 256 KiB;
- sequential chunk offsets and `O_NOFOLLOW` staging files;
- streaming SHA-256 verification before a package becomes installable;
- automatic DEB Package-ID inspection with fixed `dpkg-deb`;
- automatic IPA/TIPA `CFBundleIdentifier` inspection from one top-level `Payload/*.app/Info.plist` using bounded in-daemon ZIP/DEFLATE parsing and CoreFoundation plist parsing;
- caller-supplied expected identifier is treated as an assertion and must match detected metadata;
- `device.package.install-deb` is R2 and uses only fixed Procursus `dpkg -i`, then `dpkg-query` post-condition verification;
- `device.package.install-ipa` is R2 and uses only the pinned TrollStore `trollstorehelper install custom` adapter, then installed-bundle verification;
- Mac CLI and iOS owner UI use the same semantic package staging/install protocol;
- staged package path and provider argv are never caller-controlled.

### v0.7 Managed package lifecycle

- prior verified active artifacts become `retained` rather than being deleted;
- `device.package.rollback-deb` / `device.package.rollback-ipa` reinstall retained artifacts through fixed providers;
- `device.package.uninstall-deb` / `device.package.uninstall-ipa` remove only RootTools-managed installed targets;
- `/v1/packages/history` records install/rollback/uninstall provider outcomes;
- rollback/uninstall remain R2 owner-confirmed and keep post-condition verification;
- generic uninstall for packages not installed through RootTools remains outside the protocol.

### v0.8 Independent self-updater

- `device.self-update.schedule` is an R2 owner-confirmed capability bound to `roottools.updater`;
- the daemon persists the update request and receipt before an independent process claims it;
- generic package install/uninstall/rollback reject `com.arthur.roottools`;
- the updater re-reads real DEB Package/Version metadata and extracts data with fixed `dpkg-deb -x` without executing maintainer scripts;
- the extracted tree is restricted to RootTools App/daemon/updater/launchd paths and rejects symlinks/special files;
- candidate binaries are signed before sibling swap;
- authenticated version-specific daemon health determines success or rollback;
- a RunAtLoad, non-KeepAlive updater job can claim a queued request after bootstrap restart.

### v0.9 Semantic runtime observation

- `device.runtime.frida.observe` reports fixed Frida server/process/port/package facts without accepting a script or attach target;
- `device.runtime.ellekit.observe` reports fixed rootless ElleKit component/package facts without exposing hook/injection APIs;
- runtime observation is R0/headless and provider-bound;
- Mac `frida-status` / `ellekit-status` and the Providers UI consume the same structured endpoints;
- policy responses explicitly state that arbitrary Frida script/attach and ElleKit hook/injection surfaces are not exposed.

### iOS UI

- Providers screen grouped by provider domain;
- readiness and implementation identity;
- headless/unlock/persistence metadata;
- DEB and IPA/TIPA routing previews;
- Dashboard provider-plane health summary.

## Validation

- `provider_registry_test` validates registry/binding/package-plan invariants.
- HTTP contract validates provider catalog, package plan, and hello feature negotiation.
- Existing Control Plane and HTTP suites remain green.
- iOS 16 Release build succeeds without requiring xcodegen on the current host.

## Remaining P3 increments

The Provider Plane foundation is complete, but P3 as a whole remains in progress. Next increments are deliberately ordered:

1. Extract the remaining monolithic app/process/filesystem executors into provider-specific implementation files without changing capability contracts.
2. Add provider compatibility/version metadata and fallback selection for alternate jailbreaks.
3. Extract the remaining monolithic app/process/filesystem executors into provider-specific implementation files without changing capability contracts.
4. Harden interrupted self-update recovery for power loss in the middle of a multi-file switch.
5. Introduce any future Frida/ElleKit mutation only as narrow semantic operations with separate risk/post-condition design.
6. Physical-device qualification of Provider Plane + Package Controller + Self-Updater + runtime observation once v0.9 deployment is available.

## Definition of done for full P3

- no semantic executor depends on a jailbreak/tool name outside its provider adapter;
- package install/upgrade is typed, staged, confirmed, audited, verified, and recoverable, including rollback/uninstall;
- at least one alternate implementation can replace a provider without changing the caller-facing capability;
- physical regression verifies provider selection, failure behavior, and recovery.
