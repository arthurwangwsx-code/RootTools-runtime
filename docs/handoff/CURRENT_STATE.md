# RootTools Current State

> Living handoff document. Every Agent working on RootTools should read this file before changing code and update it when a stable milestone, device deployment boundary, or active workstream changes.

Last refreshed: **2026-08-26 00:47 +08:00**

## 1. Product position

RootTools is a **policy-controlled privileged iOS Device Runtime** for a jailbroken personal device. The SwiftUI app is the owner-facing control surface; the UID 0 daemon is the persistent execution node.

The product is not a generic jailbreak shell and must not become one.

Canonical flow:

`RootTools UI / Mac Host / future Skill -> Command Gateway -> authenticated principal -> policy/grants -> durable task -> provider -> fixed privileged implementation -> post-condition -> receipt/audit`

The current product shell is:

- `Overview`
- `Device`
- `Tasks`
- `Agents`
- `Settings`

The current priority is **RootTools itself**. AiBox integration is intentionally paused until the RootTools runtime, permission model, update path, device management and automation surfaces are mature.

## 2. Stable Git checkpoint

Latest stable code checkpoint:

`ad66662 feat: add v0.18 installed package inventory`

Recent milestone chain:

| Version | Commit | Stable capability |
| --- | --- | --- |
| v0.10 | `b22f231` | Five-tab product shell + canonical Command Gateway |
| v0.11 | `b43c28e` | Named command principals |
| v0.12 | `1556600` | Per-principal R0/R1 capability grants |
| v0.13 | `6e9c852` | Durable Device Task Runtime |
| v0.14 | `c27ce90` | Semantic UI observe/tap/type/swipe foundation |
| v0.15 | `d6bf5e8` | Permission profiles + Developer Mode |
| v0.16 | `59f2b5c` | Structured device performance/resource observation |
| v0.17 | `d5b1231` | Product App inventory/detail + process resource metrics |
| v0.18 | `ad66662` | Read-only installed Procursus/dpkg package inventory |

Source/package default at the stable checkpoint is **v0.18.0 / 0.18.0-1**.

## 3. Reference physical-device state

Reference device facts currently verified from Device Service:

- hardware: iPhone 13 Pro Max (`iPhone14,3`)
- iOS: 16.3.1 / build `20D67`
- jailbreak: Dopamine rootless
- daemon privilege: UID 0
- SSH: ready
- Frida: ready
- ZXTouch: ready
- headless execution: ready
- UI execution: ready when unlocked

**Important deployment boundary:** the physical device still runs **RootTools daemon v0.9.0**. Do not claim v0.10-v0.18 physical validation until `/v1/status` reports the new version after a trusted deployment.

Do not hard-code the device identifier in docs or code. Discover the connected device through `Scripts/device_service.py` / pymobiledevice3 tooling.

## 4. Architecture invariants

These are product invariants, not temporary implementation choices.

### 4.1 No raw privileged execution surface

Never expose:

- arbitrary shell;
- caller-supplied executable path;
- caller-supplied argv;
- arbitrary Frida scripts/attach;
- arbitrary ElleKit hook/injection;
- direct UID 0 inheritance for Agent callers.

Callers request semantic capabilities. The daemon chooses a fixed implementation provider.

### 4.2 Risk model

- **R0**: observation/read-only;
- **R1**: bounded/reversible/scoped mutation;
- **R2**: owner-authorized sensitive mutation;
- **R3**: device-critical or unrestricted privileged execution, hard-disabled.

`device.raw-shell` and R3 remain hard-disabled even in Developer Mode.

### 4.3 Principal identity and grants

Named principals represent `host`, `app`, `skill`, or `automation` callers.

- creating a principal creates identity/authentication only;
- new principals begin with **zero delegated capability grants**;
- persistent grants may target exact compiled **R0/R1** capability IDs only;
- grants may expire;
- revocation/expiry is re-checked before queued work executes;
- a remote principal cannot self-assert R2 owner approval.

### 4.4 Owner permission profiles

v0.15 defines four owner policy modes:

- **Restricted**: observation-oriented recovery surface;
- **Standard**: normal R0/R1 plus explicit R2 confirmation;
- **Developer**: enables the full compiled non-R3 Owner surface and allows the **local Owner UI only** to auto-approve R2;
- **Custom**: manual per-capability policy changes.

Developer Mode does **not** widen any Named Principal grant.

### 4.5 Durable task runtime

Canonical task states include:

- `queued`
- `waiting_for_unlock`
- `running`
- `retrying`
- `completed`
- `failed`
- `cancelled`

UI work waits for an unlocked/visible device. RootTools does not bypass the passcode. Authorization is re-checked immediately before execution.

### 4.6 Provider plane

Semantic capability IDs bind to daemon-owned providers such as:

- Darwin/native APIs;
- Dopamine/Procursus;
- TrollStore/Sileo;
- Frida/ElleKit observation;
- SpringBoard;
- ZXTouch;
- TCC;
- RootTools control/updater providers.

Provider internals must remain invisible to Agent callers.

## 5. Stable capability surfaces through v0.18

### Device and runtime

- system/daemon health;
- lock/display readiness;
- jailbreak/provider status;
- network facts;
- TCC facts;
- filesystem scopes;
- diagnostics/audit/events;
- performance snapshot: uptime, load average, VM memory distribution, storage, daemon resident memory, process count, active tasks and Provider readiness.

### Applications

Structured App inventory/detail includes:

- display name;
- Bundle ID;
- version;
- build;
- source (`system`, `jailbreak`, `user`);
- bundle path;
- executable;
- running/critical state.

### Processes

Structured process management includes PID/UID/command/critical/privileged facts and, when Darwin exposes `proc_pid_rusage`, resource facts:

- resident bytes;
- physical footprint;
- user/system CPU time;
- disk read/write bytes;
- page-ins;
- idle/interrupt wakeups.

### Packages

Stable Package Controller already supports:

- DEB / IPA / TIPA bounded staging;
- SHA-256 verification;
- package identity verification;
- fixed Procursus/TrollStore install providers;
- managed uninstall;
- retained-artifact rollback;
- lifecycle history;
- read-only installed Procursus/dpkg inventory via `GET /v1/packages/installed`, including package ID, version, architecture, section, priority, first-line description, installed size and Essential metadata;
- separate RootTools Self-Updater path.

The installed-device inventory is observation only. Seeing a package in the dpkg inventory does not create a RootTools lifecycle record and does not authorize device-wide uninstall or rollback. Mutation remains limited to RootTools-managed package records.

### UI automation

Stable semantic foundation:

- `device.ui.observe`
- `device.ui.tap`
- `device.ui.type`
- `device.ui.swipe`

Current implementation uses fixed ZXTouch semantics internally. Accessibility/selector/vision-level element targeting is still future work.

## 6. Current uncommitted working tree

**Do not reset or clean the checkout.** The Installed Package Inventory workstream is now stable in `ad66662`; the checkout still contains the separate Remote Worker WIP in shared files.

At the time of this handoff, modified/untracked paths include:

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
- `App/RemoteWorkerView.swift` (new)
- `Daemon/remote_worker_controller.c/.h` (new)
- `Tests/remote_worker_controller_test.c` (new)

### 6.1 Workstream A — Installed Package Inventory

Completed and independently committed as **v0.18** in `ad66662`.

The stable implementation includes:

- `rt_installed_packages_json()` in Package Controller;
- read-only parsing of `/var/jb/Library/dpkg/status` with test-only `ROOTTOOLS_DPKG_STATUS` override;
- exact filtering to `Status: install ok installed`;
- `GET /v1/packages/installed` protected by the existing R0 `device.package.list` read capability;
- explicit `source=procursus-dpkg` and `status=installed` projection;
- HTTP and unit fixtures;
- Swift models and `DaemonClient` integration;
- searchable/filterable Installed-on-device section in the Packages product page;
- Mac Device Service command `package-installed`;
- validation record `docs/validation/v0.18-installed-package-inventory.md`;
- version/package bump to `0.18.0 / 0.18.0-1`.

Security boundary remains unchanged: global dpkg visibility does **not** expose global package mutation. Generic uninstall/rollback still operates only on RootTools-managed lifecycle records.

### 6.2 Workstream B — Remote Worker

Concurrent uncommitted WIP, **not part of v0.18**.

Current implementation includes:

- `device.remote-worker.observe` (R0);
- `device.remote-worker.configure` (R2);
- `remote_worker_controller.c/.h`;
- owner UI `RemoteWorkerView.swift`;
- always-on/display policy;
- low-brightness behavior;
- battery/temperature/cycle/health observations;
- charge-guard related state;
- thermal gating of UI work;
- host CLI entries;
- unit/HTTP/provider tests.

The latest mixed working-tree contract suite passes, including `remote_worker_controller_test`.

This workstream touches many shared integration files. It must be extracted into its own commit(s) and isolated from Package Inventory changes.

### 6.3 Files Manager

Not yet implemented as a product-level page.

Existing safe primitives are already present:

- declared `mobile` and `bootstrap` scopes;
- bounded list/read/write;
- safe filename validation;
- `O_NOFOLLOW` protections;
- no arbitrary caller-supplied filesystem root.

Next step is a structured scope browser/editor using only those declared capabilities. Do not turn Files Manager into an arbitrary root file explorer API.

## 7. Current Self-Updater migration blocker

The new Self-Updater architecture is correct, but the reference device runs the old **v0.9.0 updater**.

Observed physical failure:

1. Mac can stage a newer RootTools DEB through Device Service;
2. Owner R2 self-update scheduling succeeds and becomes durable;
3. old updater reaches preflight;
4. old launchd environment does not include the Procursus tool paths;
5. `dpkg-deb` cannot find its `tar` dependency;
6. update fails safely before system switch; v0.9.0 remains healthy.

Newer source initializes the correct rootless bootstrap `PATH`, but this is an **updater-updates-updater bootstrap problem**: the old updater cannot benefit from a fix that is only present in the new updater.

Do not bypass this by exposing or disguising a raw root command. The desired result is one controlled migration, after which future versions use typed self-update with health check and rollback.

See `docs/architecture/self-updater.md`.

## 8. Validation truth

Latest stable v0.18 was validated independently from the concurrent Remote Worker WIP:

- full C/unit/HTTP contract suite: PASS;
- iOS 16 Release: `BUILD SUCCEEDED`;
- rootless DEB packaging: PASS (`roottools_0.18.0-1_iphoneos-arm64.deb`);
- validation was performed from exact Git index tree `185fe1263c3da1b5d89abb88abfbf0e5710f431a` in an isolated managed worktree containing only the v0.18 staged changes;
- the generated validation DEB SHA-256 was `9176ec312b3245377719c3020c25a5dd2a9e8f15a242363785bcbeec3ccea93b`.

The **current mixed working tree** was also re-tested at handoff time and passes:

- `control_plane_test`
- `provider_registry_test`
- `package_controller_test`
- `update_controller_test`
- `runtime_observer_test`
- `remote_worker_controller_test`
- `principal_store_test`
- `package_builder_test`
- `http_contract_test`
- `RootTools contract tests`

The mixed tree was contract-tested before the v0.18 extraction, but the **stable v0.18 claim comes only from the isolated index snapshot**. Remote Worker still requires its own isolated validation before it becomes stable.

Canonical checks:

```bash
bash Scripts/test.sh
bash Scripts/build.sh
python3 Scripts/package-rootless-deb.py
python3 Scripts/device_service.py --token-file .roottools-token --compact status
```

Never print or commit token contents.

## 9. Build/development notes

- project target remains iOS 16+;
- `xcodegen` is currently available at `/opt/homebrew/bin/xcodegen`; the v0.18 isolated Release validation regenerated the project from `project.yml` rather than relying on the ignored local project;
- Release builds use Swift whole-module optimization and can be slow as product UI grows;
- do not interpret a long-running high-CPU `swift-frontend` as a hang without checking the process first;
- the ignored/local Xcode project may be changed by concurrent work; for clean validation, use an isolated worktree/index snapshot or regenerate the project when xcodegen is available.

## 10. Immediate RootTools priorities

Order of work after this handoff:

1. finish and independently commit Remote Worker from the remaining dirty WIP;
2. implement product-level scoped Files Manager;
3. solve the production-blocking v0.9 -> current Self-Updater bootstrap migration without exposing raw privileged execution;
4. deploy one consolidated stable build to the reference iPhone and run physical qualification;
5. deepen UI automation to Accessibility/selector/vision semantics;
6. deepen Task Runtime into multi-step Workflow/Trigger/Retry/Result semantics;
7. continue device-management completeness: package/app recovery, diagnostics, crash/reboot/bootstrap recovery and production 1.0 hardening.

AiBox and Network Skill integration remain deferred until the RootTools runtime itself reaches this maturity.

## 11. Commit discipline for concurrent Agents

- never use `git add -A` while independent WIP is present;
- do not reset/revert another Agent's files;
- inspect `git diff` and classify hunks before staging shared files;
- use explicit pathspec or index-blob/hunk staging when workstreams overlap;
- run tests from the **exact staged snapshot** before committing a mixed-file milestone;
- prefer managed worktrees for truly parallel development;
- keep each milestone versioned, documented and independently revertible.

The v0.18 extraction is the current reference example: Package Inventory hunks were selectively staged out of files concurrently modified by Remote Worker, the staged diff was checked for Remote Worker contamination, then the exact index snapshot independently passed contracts, iOS 16 Release, and rootless DEB packaging before commit.
