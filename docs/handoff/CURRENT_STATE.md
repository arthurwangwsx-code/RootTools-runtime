# RootTools Current State

> Living handoff document. Read this before changing RootTools and update it whenever a stable Git milestone, physical-device deployment boundary, or active workstream changes.

Last refreshed: **2026-08-28 +08:00**

## 1. Product position

RootTools is the policy-controlled privileged device runtime for a personally owned jailbroken iPhone. The SwiftUI app is the Owner control surface; the UID 0 daemon is the persistent execution node.

Canonical flow:

`RootTools UI / trusted Host / future Skill -> Command Gateway -> authenticated Principal -> policy/grants -> durable task -> fixed Provider -> post-condition -> receipt/audit`

RootTools is deliberately **not** a generic jailbreak shell. R3 and `device.raw-shell` remain hard-disabled. AiBox integration remains deferred while RootTools itself is hardened.

## 2. Stable milestone chain

| Version | Commit / state | Stable capability |
| --- | --- | --- |
| v0.10 | `b22f231` | Five-tab product shell + canonical Command Gateway |
| v0.11 | `b43c28e` | Named command principals |
| v0.12 | `1556600` | Exact R0/R1 Principal grants |
| v0.13 | `6e9c852` | Durable Device Task Runtime |
| v0.14 | `c27ce90` | Semantic UI observe/tap/type/swipe foundation |
| v0.15 | `d6bf5e8` | Permission profiles + Developer Mode |
| v0.16 | `59f2b5c` | Structured performance/resource observation |
| v0.17 | `d5b1231` | Product App inventory + process resource metrics |
| v0.18 | `ad66662` | Device-wide read-only Procursus/dpkg inventory |
| v0.19 | `168c379` | Remote Worker Mode |
| v0.20 | `3f01efd` | Product-level Scoped Files Manager |
| v0.21 | `946f894` | Trusted v0.9 bootstrap migration + Dopamine launchd-domain fixes |
| v0.22 | `f0559ec` | Remote Access session + updater hardening + task scheduler fairness |

The physical v0.21 deployment is the first post-v0.9 RootTools runtime that has been proven healthy on the reference device. v0.22 is source/contract/Release-build validated but still requires physical deployment and off-USB Tailnet qualification.

## 3. Reference physical-device truth

Verified through Device Service on 2026-08-27:

- hardware: iPhone 13 Pro Max (`iPhone14,3`)
- iOS: 16.3.1 / build `20D67`
- jailbreak: Dopamine rootless
- physical daemon: **v0.21.0**
- daemon privilege: **UID 0**
- device state during qualification: unlocked / screen visible
- SSH: ready
- Frida: ready
- ZXTouch: ready
- headless execution: ready
- UI execution: ready while unlocked
- Tailscale iOS app: installed, v1.102.3, running as mobile UID 501
- current Wi-Fi address observed during qualification: local-LAN only; do not treat it as a durable remote endpoint

The v0.9 bootstrap migration completed successfully through a verified candidate updater. The update ledger records the v0.21 request as `succeeded` with `new daemon healthy`.

The Root Tools foreground app is also present and usable. During the migration/update work the App bundle remained on disk but temporarily disappeared from the Home Screen because LaunchServices/SpringBoard registration was stale. `uicache` re-registration plus SpringBoard refresh restored the visible app. Treat foreground App registration as an update post-condition, not merely a packaging detail.

Do not hard-code the physical UDID in docs or source. Discover it through the host tooling.

## 4. Stable source capabilities through v0.22

### 4.1 Permission and trust model

Effective authority composes:

`Hard Policy -> Owner Policy Mode -> Principal Grant -> R2 Approval -> Runtime Conditions -> Provider/Post-condition`

Owner profiles:

- Restricted
- Standard
- Developer
- Custom

Developer Mode enables the compiled non-R3 Owner surface and local Owner R2 auto-approval. It does **not** grant R3/raw shell and does **not** widen Host/App/Skill/Automation Principal grants.

### 4.2 Device observation

Structured observation includes:

- system/daemon health and version
- jailbreak/provider readiness
- lock/display/headless/UI readiness
- uptime/load average
- VM memory distribution
- root/var free storage
- daemon RSS
- process/task counts
- network interface facts
- TCC/runtime diagnostics

### 4.3 Applications

The product can enumerate and inspect system, jailbreak and user applications with:

- display name
- Bundle ID
- version/build when available
- source
- bundle path/executable
- running/critical state
- typed launch and terminate actions

The reference phone currently exposes more than 200 application records through the structured App surface.

### 4.4 Processes

Structured process facts include PID/UID/command/critical/privileged state plus best-effort Darwin `proc_pid_rusage` metrics:

- resident memory / physical footprint
- user/system CPU time
- disk read/write
- page-ins
- idle/interrupt wakeups

Unsupported process metrics degrade with `metricsAvailable=false` rather than failing the entire catalog.

### 4.5 Packages

Package Controller supports:

- bounded DEB / IPA / TIPA staging
- SHA-256 verification
- package identity verification
- fixed install providers
- RootTools-managed uninstall
- retained-artifact rollback
- lifecycle history
- device-wide read-only Procursus/dpkg inventory
- separate RootTools Self-Updater

Global dpkg visibility is observation-only. It does not create authority for arbitrary device-wide uninstall.

### 4.6 Scoped Files Manager

v0.20 provides a product Files page over declared `mobile` and `bootstrap` scopes only:

- nested directory navigation
- metadata/search
- bounded text read/edit/create
- path-segment validation
- `openat` + `O_NOFOLLOW` traversal
- no absolute caller-selected root
- no symlink following

### 4.7 Remote Worker

v0.19 provides:

- `device.remote-worker.observe` (R0)
- `device.remote-worker.configure` (R2)
- bounded always-awake assertion
- Owner-configured low-brightness target
- battery percentage, charging/external-power, cycle count and battery-health observation
- battery temperature and thermal pause/resume hysteresis
- UI Task Runtime thermal gating
- fail-safe pause when required temperature telemetry is unavailable

Charge-control mutation remains unavailable until a reversible device-specific mechanism is physically verified.

### 4.8 Durable tasks and UI automation

Task states include `queued`, `waiting_for_unlock`, `running`, `retrying`, `completed`, `failed`, and `cancelled`.

The scheduler prefers runnable `queued`/`retrying` work ahead of already-blocked `waiting_for_unlock` work. This prevents one locked UI task from starving later queued UI tasks; all UI-required work can advance to the explicit waiting state while the phone is locked.

Stable UI semantics:

- `device.ui.observe`
- `device.ui.tap`
- `device.ui.type`
- `device.ui.swipe`

ZXTouch is an internal Provider. Locked UI work waits; RootTools does not bypass the passcode. Accessibility/selector/vision semantic targeting is still future P4 work.

## 5. v0.21 bootstrap migration result

The old physical v0.9 updater had two independent rootless deployment defects:

1. its launchd environment could not resolve Procursus `tar` for `dpkg-deb`;
2. it addressed Dopamine LaunchDaemons using the macOS-style `system` domain instead of the foreground-user bootstrap domain.

The one-time migration path preserves the RootTools trust model:

- the Owner still schedules the typed R2 `device.self-update.schedule` operation;
- the staged package must be a verified `com.arthur.roottools` DEB;
- local DEB SHA-256 must match the staged device record;
- the migration extracts only the candidate updater from that approved DEB;
- the updater still owns package identity validation, extraction allowlist, target swapping, health verification and rollback;
- no generic caller-supplied privileged command is added to the protocol.

Host tooling also now avoids importing `pymobiledevice3` when `idevice_id`/`iproxy` are already available, so standard libimobiledevice hosts do not fail merely because the Python fallback package is absent.

## 6. v0.22 source milestone

v0.22 is committed and source validated.

It includes:

- updater dispatch through the separately registered `com.arthur.roottools.updater` launchd job instead of relying on the serving execd child lifetime;
- clearer update transition/rollback diagnostics;
- foreground app registration/discoverability as a deployment health requirement;
- scheduler fairness so blocked `waiting_for_unlock` UI work does not starve later queued tasks;
- Owner-initiated Remote Access sessions bound only to a Tailscale IPv4 address;
- one selected active Named Host Principal per Remote Session;
- remote-listener rejection of Owner and legacy Agent credentials;
- bounded session expiry and automatic invalidation when the selected Principal is no longer valid.

Do not describe v0.22 as physically qualified until the reference device reports v0.22 and an off-USB Tailnet session has passed the remote regression.

## 7. Network / off-USB execution boundary

RootTools currently has the pieces needed for remote operation, but the security architecture intentionally separates **transport** from **privilege**.

Today in v0.22 source:

- USB/usbmux is the qualified direct Host transport.
- SSH, Frida and ZXTouch are available locally on the jailbroken device.
- Tailscale is installed and running on the phone.
- the normal RootTools Device Service remains loopback-only;
- an Owner may explicitly start a bounded Remote Session;
- that listener binds only to a detected Tailscale IPv4 address and accepts only the selected Named Host Principal.

Therefore taking the phone away from USB does **not** mean exposing a UID 0 service to the public internet. The qualified direction is the private Tailnet session path. Remote R0/R1 work remains limited to exact Principal grants. R2 still requires Owner authorization or a future bounded one-shot/workflow approval mechanism. UI work also requires the phone to be unlocked and thermally eligible.

The latest off-USB discovery also established an important bootstrap fact: if Tailscale itself is already offline and there is no USB/LAN path, RootTools cannot remotely make an unreachable phone reconnect. The Owner must restore one transport bootstrap before a Remote Session can be reached.

## 8. Validation truth

Stable v0.19 and v0.20 have their own validation records under `docs/validation/`.

v0.22 source validation includes:

- complete `Scripts/test.sh`: PASS;
- Remote Access controller unit contract: PASS;
- provider/update/principal contracts: PASS;
- HTTP Remote Access authorization contract: PASS;
- iOS 16 Release build: `BUILD SUCCEEDED`;
- rootless package produced as `roottools_0.22.0-3_iphoneos-arm64.deb`.

Physical v0.21 evidence includes:

- `/v1/status`: `daemonVersion=0.21.0`, `uid=0`
- jailbreak rootless ready
- SSH / Frida / ZXTouch ready
- headless and unlocked UI execution ready
- performance endpoint returns structured data
- Scoped Files roots return correctly
- Remote Worker observation returns battery/thermal/power facts
- App/process/package catalogs return structured device inventories
- Self-Updater ledger records the v0.21 transition as `succeeded / new daemon healthy`
- foreground Root Tools app was re-registered and is visible/openable after SpringBoard refresh

Canonical source validation remains:

```bash
bash Scripts/test.sh
bash Scripts/build.sh
python3 Scripts/package-rootless-deb.py
python3 Scripts/device_service.py --token-file .roottools-token --compact status
```

Never print or commit token contents.

## 9. Build/development notes

- target remains iOS 16+;
- xcodegen is available at `/opt/homebrew/bin/xcodegen` on the current Host;
- Release Swift whole-module compilation can be CPU-heavy without being hung;
- regenerate the project from `project.yml` for clean validation when needed;
- the host usbmux fallback may use pymobiledevice3, but it is no longer an unconditional import dependency when libimobiledevice tools exist.
- canonical repository: `arthurwangwsx-code/RootTools-runtime`;
- repository visibility: public;
- full tests/build/release packaging run locally on the maintainer Mac;
- GitHub Actions is manual-only and limited to lightweight repository hygiene;
- GitHub Releases is the canonical public binary distribution channel.

## 10. Immediate priorities

Repository and credential maintenance hardening is source/build/package validated on the v0.23 development line. The canonical checkout now enforces owner-only token files and rejects release attempts from the historical mixed checkout. See `docs/validation/v0.23-maintenance-hardening.md`.

1. install the canonical Runtime candidate on the reference device;
2. qualify Tailscale Remote Session end-to-end with USB disconnected;
3. verify session expiry/stop/revoke behavior and R0/R1 remote task execution physically;
4. retire the frozen legacy iOS checkout only after credential migration and physical acceptance;
5. deepen Accessibility/selector/vision UI automation;
6. deepen Task Runtime into Workflow / Trigger / Retry / Result semantics;
7. continue reboot/re-jailbreak/crash-loop recovery and Production 1.0 hardening.

AiBox remains deferred until these RootTools runtime and remote-operation boundaries are stable.
