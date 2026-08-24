# RootTools iOS

Independent iOS 16+ jailbreak device control plane for personal devices.

## Architecture direction

RootTools is evolving from a privileged toolbox into a policy-controlled iOS Device Control Plane. The daemon owns capability metadata, risk policy, semantic action routing, audit receipts, and post-condition verification. UI, future automation, and future Agent adapters call typed capabilities rather than receiving a raw privileged shell.

- Long-term roadmap: `docs/roadmap/long-term-roadmap.md`
- Control Plane architecture: `docs/architecture/control-plane.md`
- P1 implementation plan: `docs/phases/p1-control-plane.md`

The first milestone intentionally kept the privileged surface read-only: device health, jailbreak runtime, apps, processes, selected root filesystem views, network observation, and diagnostics.

The second milestone adds a deliberately small set of typed privileged operations: app launch/terminate, bounded non-root process termination, UTF-8 reads/writes inside dedicated RootTools directories, capability/risk inspection, and an append-only privileged audit log. The UI still talks only to a loopback-only typed root daemon; it does not expose a general root shell to the app or an Agent.

The policy is intentionally asymmetric: R0 observation is broad, R1 writes are reversible and scoped, R2 process termination requires explicit UI confirmation and is rejected for UID 0/critical processes, and R3 device-critical operations are not exposed.

See `docs/architecture/v0.2-controlled-actions.md` for the capability matrix and physical-device validation contract.

P1 Control Plane is now complete on the `v0.3.0` development line: registry-driven Capability/Policy/Router core, unified action receipts, daemon-side R2 confirmation, post-condition verification, structured execution events, credential-role separation, and v0.2 action URLs retained as compatibility adapters. The final validation record includes a documented waiver for destructive host-side reruns blocked by the platform tool safety layer: `docs/validation/v0.3-p1-control-plane.md`.

The `v0.4.0` line starts the lock-aware automation foundation. The UID 0 daemon now exposes typed lock/display readiness, distinguishes headless execution from interactive UI readiness, and persists deferred UI jobs in its SQLite control-plane store. The first deferred verb is `device.automation.queue-app-launch`: when the phone is locked or the display is blanked the job remains pending, and the daemon executes it only after the UI becomes available. Device passcode bypass is explicitly out of policy. See `docs/phases/p4-lock-aware-automation.md`.

## Build and install

```bash
bash Scripts/install-jailbreak.sh
```

Target layout on a rootless Dopamine device:

- App: `/var/jb/Applications/RootTools.app`
- Daemon: `/var/jb/usr/local/bin/roottools-execd`
- launchd: `/var/jb/Library/LaunchDaemons/com.arthur.roottools.execd.plist`
- API: `127.0.0.1:45821`, authenticated token generated locally at build time

