# RootTools iOS

Independent iOS 16+ jailbreak device control plane for personal devices.

The first milestone intentionally kept the privileged surface read-only: device health, jailbreak runtime, apps, processes, selected root filesystem views, network observation, and diagnostics.

The second milestone adds a deliberately small set of typed privileged operations: app launch/terminate, bounded non-root process termination, UTF-8 reads/writes inside dedicated RootTools directories, capability/risk inspection, and an append-only privileged audit log. The UI still talks only to a loopback-only typed root daemon; it does not expose a general root shell to the app or an Agent.

The policy is intentionally asymmetric: R0 observation is broad, R1 writes are reversible and scoped, R2 process termination requires explicit UI confirmation and is rejected for UID 0/critical processes, and R3 device-critical operations are not exposed.

See `docs/architecture/v0.2-controlled-actions.md` for the capability matrix and physical-device validation contract.

## Build and install

```bash
bash Scripts/install-jailbreak.sh
```

Target layout on a rootless Dopamine device:

- App: `/var/jb/Applications/RootTools.app`
- Daemon: `/var/jb/usr/local/bin/roottools-execd`
- launchd: `/var/jb/Library/LaunchDaemons/com.arthur.roottools.execd.plist`
- API: `127.0.0.1:45821`, authenticated token generated locally at build time

