# RootTools Long-Term Roadmap

## Product definition

RootTools is the resident privileged device runtime and command node for a personally owned iOS device. It is not a generic jailbreak shell and it is not the reasoning Agent. Higher layers such as AiBox, a Mac host, a network Skill, automation workflows, or the local UI submit semantic Device Ops. RootTools owns command ingress, caller trust, privilege routing, policy, execution, post-condition verification, audit, and recovery.

The architectural invariant is:

`Caller -> Command Gateway -> Device Ops -> RootTools Control Plane -> Typed Privileged Adapter -> iOS/jailbreak runtime`

No model-facing or automation-facing surface may expose arbitrary privileged shell execution.

## Principles

1. Semantic actions only; callers request `device.app.launch`, never command strings.
2. Least privilege; UI stays mobile/UID 501 and a minimal daemon performs privileged work.
3. Risk is data; R0/R1/R2/R3 is declared in the Capability Registry and enforced by the daemon.
4. Policy cannot be bypassed by UI bugs; privileged policy is enforced again in the daemon.
5. Success requires a post-condition; exit code zero is not sufficient for state-changing work.
6. Every privileged attempt is auditable, including policy denials.
7. Filesystem scopes replace arbitrary paths.
8. Frida, ElleKit, ZXTouch, SpringBoard tools, and Darwin APIs are replaceable adapters.
9. Agent-facing Device Ops remain stable when jailbreak internals change.
10. Recovery and rollback are preferred over capability breadth.
11. Caller identity is independent from transport; Mac, AiBox and network Skills converge on one Command Gateway.
12. Product navigation is organized by user intent (Overview / Device / Tasks / Agents / Settings), not by provider implementation.

## Phases

### P0 — Foundation — COMPLETE

Standalone iOS 16+ app, UID501 UI + UID0 daemon, loopback token authentication, R0 observation, typed R1/R2 operations, audit, R3/raw-shell denial, and physical-device validation.

### P1 — Control Plane — COMPLETE

Build the reusable platform core before adding more domains:

- Capability Registry as the single source of truth.
- Action Router as the only state-changing privileged path.
- Daemon-side Policy Engine.
- Unified ActionRequest and ActionReceipt.
- Unified append-only Audit Store.
- Post-condition verification for every state-changing action.
- Backward-compatible mapping from v0.2 action endpoints.
- Full physical-device regression.

Exit criteria and the documented destructive-regression waiver: `docs/phases/p1-control-plane.md` and `docs/validation/v0.3-p1-control-plane.md`.

### P2 — Device Management

App/process/filesystem/package/permission inspection first, then carefully scoped reversible mutation. Add app containers, entitlements, process metrics, filesystem scope registry, package metadata, and TCC inspection.

v0.16 adds the first structured performance/resource snapshot (uptime, load, VM memory distribution, storage, daemon RSS, process/task counts and Provider readiness) and a product-facing Performance screen. Battery/thermal and per-process historical metrics remain later P2 depth increments.

v0.17 deepens Device Management with version/build/source/path aware application inventory and best-effort Darwin per-process resource metrics. Applications and Processes now have structured management screens instead of raw engineering dumps.

v0.18 adds device-wide read-only Procursus/dpkg installed-package inventory. The product can search and inspect installed package metadata without converting that visibility into a generic uninstall primitive; package mutation remains limited to RootTools-managed lifecycle records.

v0.19 adds the first Remote Worker operating mode for a phone acting as a durable automation node: bounded always-awake assertion, owner-configured low-brightness target, battery/thermal/power observation, hysteresis thermal pause/resume and UI-task gating. Thermal telemetry is fail-safe: if RootTools cannot prove a battery temperature signal while worker mode is enabled, it releases the display assertion and holds UI work. Charge-control mutation stays unavailable until a reversible device-specific path is physically verified.

v0.20 adds a product-level scoped Files Manager over the existing filesystem capability plane. The browser supports nested relative paths inside declared `mobile`/`bootstrap` roots, metadata/search/navigation and bounded text create/read/edit. Every path segment is validated and traversed with no-follow directory/file descriptors; symlinks may be observed but are never followed. Arbitrary absolute filesystem roots remain outside the protocol.

### P3 — Jailbreak & Runtime Platform

Normalize Dopamine, Procursus, launchd, Sileo, Frida, ElleKit, TrollStore, SSH, and ZXTouch behind semantic runtime capabilities. Never expose raw Frida/ElleKit scripting as a model-facing primitive.

**v0.5 Provider Plane foundation implemented:** RootTools now has a daemon-owned Provider Registry, capability-to-provider bindings, provider availability gates, `providerId` in receipts/audit, package-provider planning for DEB/IPA/TIPA, and an owner-facing Providers screen. **v0.6 adds the typed Package Controller:** bounded RootTools-owned staging, SHA-256 and package identity verification, R2 owner-confirmed fixed dpkg/TrollStore adapters, Mac CLI upload/install, and iOS Packages UI. **v0.7 adds managed uninstall, retained-artifact rollback, and package lifecycle history. v0.8 adds the independent RootTools self-updater with version health-check and sibling rollback. v0.9 adds read-only semantic Frida/ElleKit runtime observation while keeping scripts, arbitrary attach, hooks, and injection outside the protocol.** See `docs/architecture/provider-adapter.md`, `docs/architecture/package-controller.md`, `docs/architecture/self-updater.md`, `docs/architecture/runtime-observation.md`, and `docs/phases/p3-provider-platform.md`.

P3 remains **IN PROGRESS** until remaining concrete executors are fully extracted into provider-specific modules, package rollback/self-update is isolated, semantic Frida/ElleKit operations exist, and physical-device qualification is complete.

### P4 — UI Automation

Implement `observe -> act -> observe -> verify`: Accessibility first, screenshot/vision as supplementation, ZXTouch as coordinate fallback, and fresh-observation/post-condition checks. Background/virtual scene research comes after the foreground path is stable.

Lock-aware foundation started in v0.4: typed lock/display observation, headless-vs-UI readiness, and a durable `queue until unlock` execution path are implemented before broader foreground/virtual-scene automation. The daemon never bypasses the device passcode; locked UI work is deferred while headless work remains available.

v0.14 moves the first input operations behind semantic capability IDs and the durable Task Runtime. `device.ui.observe`, `device.ui.tap`, `device.ui.type`, and `device.ui.swipe` use the fixed ZXTouch adapter internally. Input tasks re-check grants/policy at execution time and wait for an unlocked visible UI. Coordinate input is explicitly the current fallback layer; selector/accessibility observation and visual effect verification remain the next P4 depth increment.

### P5 — Agent Device Ops

Expose stable verbs such as `device_info`, `device_app_launch`, `device_process_inspect`, `device_fs_read`, and `device_runtime_inspect`. The Agent never receives RootTools internals or arbitrary shell access.

v0.10 establishes the canonical Command Gateway, v0.11 adds named command principals, and v0.12 separates identity from authority with exact R0/R1 capability grants and optional expiry. One-shot R2 approval remains a later explicit approval primitive rather than a persistent grant.

### P6 — Automation Runtime

Make the phone a durable execution node with Trigger, Workflow, Step, Run, Retry/Timeout, Result, Rollback, and battery/resource policy.

v0.13 establishes the first canonical Device Task Ledger. It persists caller, capability, UI requirement, attempts and lifecycle state, recovers interrupted `running` tasks after daemon restart, and moves lock-aware app launch to the new task model. Broader task executors reuse this ledger rather than creating parallel queues.

v0.19 connects resource policy to the Task Runtime: Remote Worker thermal state can gate queued UI work without weakening headless execution or bypassing device lock policy.

### P7 — Production 1.0

Reboot/re-jailbreak recovery, crash-loop safe mode, protocol/schema versioning, atomic self-update with rollback, compatibility matrix, and multi-device identity/transport.

### P8 — Product Shell & Device Manager

Replace the engineering-dashboard information architecture with the stable five-tab product shell. Add product-quality app/package/process management, task status, principal management, maintenance surfaces, empty/degraded/error states, and a consistent interaction language for risk/post-condition.

### P9 — Command Gateway & Trusted Principals

Make `POST /v1/commands/submit` the canonical ingress, retain `/v1/action` as a compatibility adapter, separate principal identity from transport, add scoped grants, pair/revoke flows, and preserve one receipt/event model across RootTools UI, Mac Host, AiBox and future network Skills.

**v0.10 foundation:** canonical Command Gateway + compatibility ingress. **v0.11 foundation:** named host/app/skill/automation principals, independent hashed credentials, owner create/revoke, and authenticated principal identity in receipts/audit. **v0.12:** exact expiring R0/R1 Principal Grants. **v0.15:** formal layered permission model plus Restricted/Standard/Developer Owner profiles. P9 remains in progress until resource-scoped grants, pairing, one-shot R2 approval and credential rotation/expiry policy are complete.

### P10 — AiBox & Network Integration

Add an AiBox adapter over the existing `DeviceExecution*` contract, then an outbound trusted relay for network Skills. Neither integration receives raw shell, provider argv, owner token, or RootTools implementation models.

## Long-term domain organization

Organize code by domain, not screen:

`Device / App / Process / Filesystem / Permission / Package / Runtime / Network / UI Automation / Automation`

Each domain converges on:

`Observer / Controller / Capability / Policy / Action / Audit / Test`

## Near-term order

1. Stabilize the P8 five-tab product shell and device-management interaction model.
2. Finish P3/P4 provider hardening and semantic UI automation without introducing raw execution primitives.
3. Deepen P9 Command Gateway with principal identity/grants while keeping the existing capability router as the only executor seam.
4. Add the P10 AiBox adapter against AiBox `DeviceExecution*`, not a second RootTools-specific action model.
5. Add outbound network relay only after principal/grant/revocation and task/audit semantics are complete.

