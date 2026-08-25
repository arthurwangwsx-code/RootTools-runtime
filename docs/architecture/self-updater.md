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

The reference iOS 16 device currently runs the older v0.9.0 daemon/updater. A physical self-update attempt established the following boundary:

1. a newer RootTools DEB can be staged and SHA-256 verified;
2. owner-confirmed `device.self-update.schedule` is accepted and persisted;
3. the independent updater reaches preflight without replacing the serving runtime;
4. the old launchd environment does not include the Procursus bootstrap tool paths;
5. `dpkg-deb` therefore cannot resolve its `tar` dependency and metadata validation fails;
6. the update fails before switch and the healthy v0.9.0 daemon remains running.

Newer source initializes the correct rootless bootstrap `PATH`, but that fix is inside the newer updater itself. This is therefore an updater-bootstrap migration problem, not a reason to expose a generic privileged command channel.

The remaining goal is one trusted migration from the physical v0.9 updater to the current updater while preserving R2 owner authorization, fixed payload validation, health verification and rollback. After that migration, normal RootTools upgrades should use the typed Self-Updater rather than repeated manual Sileo/Filza installation.
