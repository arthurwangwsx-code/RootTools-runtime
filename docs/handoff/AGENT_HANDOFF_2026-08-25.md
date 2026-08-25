# RootTools Agent Handoff — 2026-08-25

This is the point-in-time handoff for continuing RootTools with multiple Agents. Read `docs/handoff/CURRENT_STATE.md` first; this file focuses on execution ownership and the next concrete actions.

## Handoff goal

Continue RootTools as the primary project. Do **not** spend the next milestone on AiBox integration.

The target product is a long-running privileged device runtime with:

- strong owner/principal permissions;
- Developer Mode for fast owner development;
- complete device/application/process/package/file/runtime management;
- durable command/task execution;
- lock-aware UI automation;
- safe remote-worker operation;
- trusted self-update and rollback;
- production diagnostics/recovery.

## Starting point

Stable HEAD before handoff docs:

`d5b1231 feat: add v0.17 device inventory and process metrics`

Physical reference device:

- iPhone 13 Pro Max / iOS 16.3.1;
- Dopamine rootless;
- RootTools daemon physically still **v0.9.0**;
- UID 0 daemon, SSH/Frida/ZXTouch ready.

Current checkout is dirty on purpose. Do not clean it.

## Recommended two-Agent split

### Agent A — RootTools Device Manager / Integration owner

Own the current dirty checkout and extract the already-started work into clean milestones.

Priority A1 — Installed Package Inventory:

1. finish `/v1/packages/installed` HTTP contract;
2. add Swift package inventory models/client;
3. add Installed Packages section/search/filter to Packages UI;
4. add host CLI command;
5. add validation doc and next version;
6. stage only Package Inventory hunks;
7. validate staged snapshot;
8. commit independently.

Priority A2 — Remote Worker:

1. review all existing `remote_worker_controller` behavior;
2. confirm R0 observe / R2 configure risk semantics;
3. verify thermal guard cannot silently broaden UI authority;
4. verify battery/charge-control observations fail closed when unsupported;
5. run unit/HTTP/iOS Release;
6. document physical-device boundaries separately;
7. commit independently.

Priority A3 — Files Manager:

Build a product UI over declared RootTools scopes only. Keep bounded read/write/list; no arbitrary root path API.

### Agent B — Self-Updater / production hardening owner

Use a **clean managed worktree** based on the latest stable/handoff commit. Avoid the dirty Package/Remote Worker integration files where possible.

Primary goal: solve the one-time **v0.9 updater -> current updater** migration without creating a raw privileged execution primitive.

Work items:

1. reproduce the old updater `PATH`/`tar` preflight failure from recorded evidence/tests;
2. define a trusted one-time bootstrap/migration mechanism that preserves semantic/R2 boundaries;
3. ensure updater candidate validation remains allowlist-only;
4. retain health-check and rollback semantics;
5. add deterministic recovery for interrupted `launching/running` update states where feasible;
6. make the resulting migration usable by the host without repeated Filza/Sileo installs;
7. document exactly which one-time owner approval remains necessary, if any;
8. do not claim success until `/v1/status` physically reports the new daemon version after migration.

Good isolation targets for Agent B include:

- `Daemon/roottools_updater.c`
- `Daemon/update_controller.c/.h`
- `docs/architecture/self-updater.md`
- self-update validation/deployment docs
- updater-specific tests

Coordinate before editing shared build/package scripts.

## Shared-file collision list

The current Package/Remote Worker WIP already overlaps in:

- `App/DaemonClient.swift`
- `App/DashboardView.swift`
- `App/Models.swift`
- `Daemon/control_plane.c`
- `Daemon/provider_registry.c`
- `Daemon/roottools_execd.c`
- `Scripts/build.sh`
- `Scripts/device_service.py`
- `Scripts/test.sh`
- `Tests/http_contract_test.py`
- `Tests/provider_registry_test.c`

Do not let two Agents concurrently edit these files in the same checkout.

## Safety and architecture checklist before every implementation

Before adding a new operation, answer all of these:

1. What is its semantic capability ID?
2. What is its risk: R0/R1/R2/R3?
3. Can it be narrowed to a fixed implementation rather than arbitrary argv/path/script?
4. Which Provider owns it?
5. Does a Named Principal need an explicit grant?
6. If queued, will policy/grant be re-checked before execution?
7. What is the post-condition?
8. What receipt/audit evidence is persisted?
9. What happens when the provider is unavailable?
10. What happens if the phone is locked, the daemon restarts, or execution becomes indeterminate?

If the proposed feature requires arbitrary root shell or caller-controlled executable/argv, redesign it as a typed semantic capability instead.

## Developer Mode handoff

Developer Mode is already implemented in stable v0.15 and is an owner convenience, not a security bypass.

Required invariant:

`Developer Mode = local Owner convenience`

It may enable all compiled non-R3 owner capabilities and auto-approve local Owner R2. It must **not**:

- enable R3/raw shell;
- grant anything to Host/Skill/Automation principals;
- let a remote caller turn `confirmed=true` into owner approval;
- bypass lock/passcode runtime conditions.

## Physical-device deployment rule

Source may advance faster than the phone. Always report the two versions separately.

At this handoff:

- stable source: v0.17.0;
- reference device: v0.9.0.

Do not say a new feature is physically validated because its source tests/build pass.

The next physical upgrade should preferably be a **consolidated stable build**, not one manual DEB per small milestone.

## Validation sequence for a milestone

1. run focused unit tests during implementation;
2. run `bash Scripts/test.sh`;
3. verify no unrelated Agent WIP is staged;
4. export/validate the exact staged snapshot if shared files were involved;
5. run iOS 16 Release from that exact snapshot;
6. build the rootless DEB;
7. update validation and current-state docs;
8. commit one coherent milestone;
9. only then attempt physical deployment when it is worth consolidating.

## Current known truth versus open questions

Known:

- the v0.9 physical daemon is healthy;
- typed package staging works on-device;
- owner self-update scheduling works;
- old updater fails safely in preflight because its environment cannot resolve Procursus `tar` for `dpkg-deb`;
- v0.15 permissions/Developer Mode, v0.16 performance and v0.17 App/Process surfaces pass source validation;
- current mixed WIP contract suite passes.

Open:

- final trusted updater migration path from physical v0.9;
- physical behavior of v0.15-v0.17 until deployed;
- final Remote Worker policy on real device thermal/battery edge cases;
- Accessibility/selector observation strategy beyond ZXTouch coordinates;
- production reboot/re-jailbreak/crash-loop recovery;
- exact workflow abstraction above single durable tasks.

## End-of-session handoff requirement

Every Agent should update `docs/handoff/CURRENT_STATE.md` before ending a substantial RootTools session, including:

- new stable commit/version;
- true physical-device version;
- active dirty WIP and file ownership;
- tests/build status;
- blockers;
- next three concrete actions.

This keeps RootTools continuity in the repository instead of in one long chat context.
