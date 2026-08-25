# RootTools iOS

Independent iOS 16+ jailbreak device control plane for personal devices.

## Architecture direction

RootTools is evolving from a privileged toolbox into a policy-controlled iOS Device Control Plane. The daemon owns capability metadata, risk policy, semantic action routing, audit receipts, and post-condition verification. UI, future automation, and future Agent adapters call typed capabilities rather than receiving a raw privileged shell.

- Product definition: `docs/product/product-definition.md`
- Long-term roadmap: `docs/roadmap/long-term-roadmap.md`
- Control Plane architecture: `docs/architecture/control-plane.md`
- Command Gateway: `docs/architecture/command-gateway.md`
- Provider / Adapter architecture: `docs/architecture/provider-adapter.md`
- Package Controller architecture: `docs/architecture/package-controller.md`
- Product information architecture: `docs/ux/product-information-architecture.md`
- P1 implementation plan: `docs/phases/p1-control-plane.md`

The first milestone intentionally kept the privileged surface read-only: device health, jailbreak runtime, apps, processes, selected root filesystem views, network observation, and diagnostics.

The second milestone adds a deliberately small set of typed privileged operations: app launch/terminate, bounded non-root process termination, UTF-8 reads/writes inside dedicated RootTools directories, capability/risk inspection, and an append-only privileged audit log. The UI still talks only to a loopback-only typed root daemon; it does not expose a general root shell to the app or an Agent.

The policy is intentionally asymmetric: R0 observation is broad, R1 writes are reversible and scoped, R2 process termination requires explicit UI confirmation and is rejected for UID 0/critical processes, and R3 device-critical operations are not exposed.

See `docs/architecture/v0.2-controlled-actions.md` for the capability matrix and physical-device validation contract.

P1 Control Plane is now complete on the `v0.3.0` development line: registry-driven Capability/Policy/Router core, unified action receipts, daemon-side R2 confirmation, post-condition verification, structured execution events, credential-role separation, and v0.2 action URLs retained as compatibility adapters. The final validation record includes a documented waiver for destructive host-side reruns blocked by the platform tool safety layer: `docs/validation/v0.3-p1-control-plane.md`.

The `v0.4.0` line starts the lock-aware automation foundation. The UID 0 daemon now exposes typed lock/display readiness, distinguishes headless execution from interactive UI readiness, and persists deferred UI jobs in its SQLite control-plane store. The first deferred verb is `device.automation.queue-app-launch`: when the phone is locked or the display is blanked the job remains pending, and the daemon executes it only after the UI becomes available. Device passcode bypass is explicitly out of policy. See `docs/phases/p4-lock-aware-automation.md`.

The `v0.5.0` line adds the Provider Plane. Capabilities are now bound by the daemon to implementation providers such as Dopamine/Procursus, TrollStore/Sileo, Darwin, Frida/ElleKit, SpringBoard, ZXTouch, and TCC. `GET /v1/providers/catalog` is the implementation truth source; Action receipts/audit identify `providerId`; and `POST /v1/package/plan` resolves DEB and IPA/TIPA formats without exposing raw shell or arbitrary executable control. The iOS UI includes a Providers screen with readiness and package routing previews.

The `v0.6.0` line adds the typed Package Controller. Mac clients and the iOS owner UI can stage `.deb`, `.ipa`, and `.tipa` files into a RootTools-owned store using bounded chunks, SHA-256 verification, and package-identity inspection. Installation is an R2 owner-confirmed semantic action: DEB is routed only to the fixed Procursus `dpkg` adapter and verified with `dpkg-query`; IPA/TIPA is routed only to the pinned TrollStore helper contract and verified through the installed bundle record. Callers cannot provide package filesystem paths, executables, or argv. RootTools self-update remains a separate updater concern because replacing the currently serving daemon can interrupt its own receipt lifecycle.

The `v0.7.0` line completes the first managed package lifecycle. A newer managed install retires the previous verified artifact instead of deleting it; retained artifacts can be rolled back through an R2 owner-confirmed provider action. Managed DEB and TrollStore apps can be uninstalled with post-condition verification while their staged artifact remains available for reinstall. Package lifecycle events are available from `/v1/packages/history`, the Mac client exposes `package-history`, `package-uninstall`, and `package-rollback`, and the iOS Packages screen surfaces the same operations. Uninstall is intentionally limited to packages installed through RootTools rather than becoming a generic device-wide removal primitive.

The `v0.8.0` line adds the independent RootTools self-updater. `roottools-execd` only persists the R2 owner-confirmed update request and returns its normal ActionReceipt; after the connection closes, a separate `roottools-updater` process validates the staged `com.arthur.roottools` DEB, extracts only the allowlisted data payload, pre-signs candidate binaries, atomically swaps the RootTools App/daemon/updater/plists, and health-checks the new daemon. If the expected daemon version does not come online, the helper restores the previous sibling backups and restarts the old daemon. Generic package install/uninstall paths explicitly reject RootTools itself.

The `v0.9.0` line adds semantic runtime observation for Frida and ElleKit without turning instrumentation into a caller-facing execution surface. Frida observation reports provider/port readiness, the fixed `frida-server` process PID/UID, candidate server path, and installed package/version facts while explicitly reporting that script execution and arbitrary attach are not exposed. ElleKit observation reports the fixed rootless library/loader/injector/pspawn/safe-mode/TweakInject component facts and installed package/version while raw hook/injection APIs remain unavailable. Mac clients expose `frida-status` and `ellekit-status`, and the Providers screen shows the same runtime facts.

The `v0.10.0` line starts the product/runtime convergence: RootTools now boots into the stable `Overview / Device / Tasks / Agents / Settings` shell, and `POST /v1/commands/submit` is the canonical typed command ingress while `/v1/action` remains a compatibility adapter. The next increments deepen principal identity/grants and attach AiBox/Network adapters to this gateway rather than adding transport-specific executors.

The `v0.14.0` line adds semantic UI automation foundations. Callers use typed `device.ui.observe`, `device.ui.tap`, `device.ui.type`, and `device.ui.swipe`; they never see the ZXTouch wire protocol. Input actions become durable lock-aware tasks, re-check caller authority immediately before execution, and do not blindly retry indeterminate touch/text effects.

The `v0.15.0` line formalizes RootTools permissions into hard policy, Owner policy mode, Principal grants, R2 approval and runtime conditions. Restricted keeps observation plus the policy recovery switch, Standard keeps explicit R2 approval, and Developer enables the full compiled non-R3 Owner surface with local Owner R2 auto-approval. Named principals never inherit Developer Mode.

The `v0.16.0` line adds a structured performance/resource surface: uptime, load average, VM memory distribution, free storage, daemon resident memory, process count, active device tasks and Provider readiness. The Device tab exposes the same snapshot without falling back to shell parsing.

Host deployment tooling no longer requires libimobiledevice. When `iproxy`, `idevice_id`, or `ideviceinfo` are unavailable, the scripts use pymobiledevice3's usbmux/lockdown APIs directly for discovery, port forwarding, and device metadata. If host `ldid` is missing, the jailbreak install path signs the final App and daemon with the bootstrap's device-side `ldid` before launch.

## Build and install

```bash
bash Scripts/install-jailbreak.sh
```

If the host environment cannot perform privileged `/var/jb` deployment, build a rootless `.deb` for explicit owner installation in Sileo/Filza:

```bash
bash Scripts/build.sh
python3 Scripts/package-rootless-deb.py
```

See `docs/deployment/rootless-deb.md` for the fixed package allowlist and post-install behavior.

Target layout on a rootless Dopamine device:

- App: `/var/jb/Applications/RootTools.app`
- Daemon: `/var/jb/usr/local/bin/roottools-execd`
- launchd: `/var/jb/Library/LaunchDaemons/com.arthur.roottools.execd.plist`
- API: `127.0.0.1:45821`, authenticated token generated locally at build time

