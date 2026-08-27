# RootTools Independent Self-Updater

## Why it is separate

`roottools-execd` cannot safely replace its own executable and launchd definition while it is still responsible for persisting and returning the ActionReceipt for that operation. Self-update therefore uses a two-process protocol:

`Owner -> device.self-update.schedule -> roottools-execd -> durable queued state -> ActionReceipt -> connection closes -> roottools-updater`

The updater is a fixed RootTools binary. There is no shell/script/update-command input in the protocol.

## Preconditions

- staged package state is `ready`;
- format is `deb`;
- detected DEB Package ID is exactly `com.arthur.roottools`;
- SHA-256 and package identity were already verified by Package Controller;
- `roottools-updater` provider is available;
- R2 owner confirmation is authenticated by the daemon;
- only one `queued/launching/running` self-update may exist.

Generic `device.package.install-deb`, rollback, and uninstall reject `com.arthur.roottools`; self-update is the only mutation path for RootTools itself.

## Durable state

`update_controller` stores recent update requests independently from the normal Action ledger. States are:

- `queued` — owner-confirmed request is durable;
- `launching` — daemon claimed it after returning the receipt;
- `running` — updater has started preflight/extraction;
- `succeeded` — expected new daemon version passed health check;
- `failed` — failure occurred before a system switch;
- `rolled_back` — new version failed health and previous files/daemon were restored;
- `rollback_failed` — neither new nor previous daemon passed final health verification.

`GET /v1/self-update/status` exposes the bounded status history.

## Payload execution policy

The updater does **not** run DEB maintainer scripts and does not invoke generic `dpkg -i` for RootTools self-update. It uses fixed `dpkg-deb -x` only to extract package data into a private work directory, then rejects every symlink/special file and every regular file outside:

- `/var/jb/Applications/RootTools.app/**`
- `/var/jb/usr/local/bin/roottools-execd`
- `/var/jb/usr/local/bin/roottools-updater`
- `/var/jb/Library/LaunchDaemons/com.arthur.roottools.execd.plist`
- `/var/jb/Library/LaunchDaemons/com.arthur.roottools.updater.plist`

Required App/daemon/updater/plist files must all be present.

## Switch and rollback

Candidates are copied to hidden siblings on the target filesystem and signed before the serving daemon is stopped. Each current target is renamed to a request-scoped rollback sibling, then the prepared candidate is renamed into place.

The updater then bootstraps `com.arthur.roottools.execd`, refreshes the App registration, and polls authenticated `/v1/hello`. The Debian revision suffix (`0.8.0-1`) is normalized to the daemon version (`0.8.0`) for health verification.

If the new daemon does not become healthy, the updater stops it, restores the request-scoped rollback siblings, boots the previous daemon, and verifies the previous version. Existing rollback siblings are never overwritten by a new update attempt.

## Recovery boundary

The updater has a non-KeepAlive launchd job with `RunAtLoad`. A queued request that survived a jailbreak/bootstrap restart can be claimed by the helper. Full recovery from power loss in the middle of the multi-file switch remains a P7 production-hardening item; v0.8 records `launching/running` state so that recovery can be made deterministic without guessing.

## Reference-device bootstrap migration status

The one-time physical **v0.9.0 -> v0.21.0** bootstrap migration is now proven on the reference iOS 16 Dopamine device.

The old v0.9 updater had two rootless-bootstrap defects:

1. its launchd environment omitted the Procursus tool paths, so `dpkg-deb` could not resolve `tar`;
2. it addressed the RootTools LaunchDaemons in the macOS-style `system` domain rather than Dopamine's foreground-user domain.

`Scripts/migrate-v09-updater-path.py` resolves the one-time chicken-and-egg problem without adding a generic privileged execution surface. The Owner still creates the typed R2 self-update request against a staged, verified `com.arthur.roottools` DEB. The host then proves that its local DEB SHA-256 matches that staged record, extracts only the candidate updater from that package, and lets that fixed updater execute the existing allowlisted replacement/health/rollback protocol.

Physical evidence after migration:

- `/v1/status` reported `daemonVersion=0.21.0` and `uid=0`;
- the update ledger recorded the v0.21 request as `succeeded` / `new daemon healthy`;
- a clean daemon restart retained v0.21;
- the foreground Root Tools App remained a valid bundle and was recoverable through LaunchServices/uicache registration.

### v0.22 dispatcher hardening

v0.21 exposed a second, later lifecycle issue: the daemon still spawned `roottools-updater` as a child process. On Dopamine, stopping the execd launchd service during a version switch can terminate that child, leaving the durable update row at `running/switching` before any target swap.

v0.22 changes dispatch so `roottools-execd` kicks the separately registered `com.arthur.roottools.updater` launchd job instead of owning the updater process directly. This keeps the updater alive across execd bootout and adds more explicit switch/rollback diagnostics. v0.22 must be physically qualified separately; source readiness is not equivalent to a completed device upgrade.

Foreground App registration is also now treated as a production update concern. A healthy daemon alone is not a complete Owner-facing update if SpringBoard/LaunchServices cannot discover the Root Tools app.
