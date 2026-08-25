# RootTools Long-Term Roadmap

## Product definition

RootTools is the privileged device control plane for a personally owned iOS device. It is not a generic jailbreak shell and it is not an Agent runtime. Higher layers such as AiBox, an Agent, automation workflows, or the local UI call semantic Device Ops. RootTools owns privilege routing, policy, execution, post-condition verification, audit, and recovery.

The architectural invariant is:

`Caller -> Device Ops -> RootTools Control Plane -> Typed Privileged Adapter -> iOS/jailbreak runtime`

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

### P3 — Jailbreak & Runtime Platform

Normalize Dopamine, Procursus, launchd, Sileo, Frida, ElleKit, TrollStore, SSH, and ZXTouch behind semantic runtime capabilities. Never expose raw Frida/ElleKit scripting as a model-facing primitive.

**v0.5 Provider Plane foundation implemented:** RootTools now has a daemon-owned Provider Registry, capability-to-provider bindings, provider availability gates, `providerId` in receipts/audit, package-provider planning for DEB/IPA/TIPA, and an owner-facing Providers screen. **v0.6 adds the typed Package Controller:** bounded RootTools-owned staging, SHA-256 and package identity verification, R2 owner-confirmed fixed dpkg/TrollStore adapters, Mac CLI upload/install, and iOS Packages UI. **v0.7 adds managed uninstall, retained-artifact rollback, and package lifecycle history. v0.8 adds the independent RootTools self-updater with version health-check and sibling rollback. v0.9 adds read-only semantic Frida/ElleKit runtime observation while keeping scripts, arbitrary attach, hooks, and injection outside the protocol.** See `docs/architecture/provider-adapter.md`, `docs/architecture/package-controller.md`, `docs/architecture/self-updater.md`, `docs/architecture/runtime-observation.md`, and `docs/phases/p3-provider-platform.md`.

P3 remains **IN PROGRESS** until remaining concrete executors are fully extracted into provider-specific modules, package rollback/self-update is isolated, semantic Frida/ElleKit operations exist, and physical-device qualification is complete.

### P4 — UI Automation

Implement `observe -> act -> observe -> verify`: Accessibility first, screenshot/vision as supplementation, ZXTouch as coordinate fallback, and fresh-observation/post-condition checks. Background/virtual scene research comes after the foreground path is stable.

Lock-aware foundation started in v0.4: typed lock/display observation, headless-vs-UI readiness, and a durable `queue until unlock` execution path are implemented before broader foreground/virtual-scene automation. The daemon never bypasses the device passcode; locked UI work is deferred while headless work remains available.

### P5 — Agent Device Ops

Expose stable verbs such as `device_info`, `device_app_launch`, `device_process_inspect`, `device_fs_read`, and `device_runtime_inspect`. The Agent never receives RootTools internals or arbitrary shell access.

### P6 — Automation Runtime

Make the phone a durable execution node with Trigger, Workflow, Step, Run, Retry/Timeout, Result, Rollback, and battery/resource policy.

### P7 — Production 1.0

Reboot/re-jailbreak recovery, crash-loop safe mode, protocol/schema versioning, atomic self-update with rollback, compatibility matrix, and multi-device identity/transport.

## Long-term domain organization

Organize code by domain, not screen:

`Device / App / Process / Filesystem / Permission / Package / Runtime / Network / UI Automation / Automation`

Each domain converges on:

`Observer / Controller / Capability / Policy / Action / Audit / Test`

## Near-term order

1. Finish P3 provider hardening: extract concrete adapters and add the typed Package Controller.
2. Continue P2 read/inspect breadth without introducing raw write primitives.
3. Bring Accessibility + ZXTouch semantic UI actions behind the Provider Plane in P4.
4. Add P5 Agent Device Ops only through stable capability contracts; never expose provider internals as executable primitives.

