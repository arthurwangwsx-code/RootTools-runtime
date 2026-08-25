# RootTools Package Controller

## Goal

Provide an ADB-like package workflow for a jailbroken iPhone without turning RootTools into a remote root shell.

The package flow is:

`Mac / RootTools.app -> stage metadata -> bounded chunks -> SHA-256 + package identity -> ready -> R2 owner confirmation -> fixed package provider -> post-condition -> receipt/audit`

No caller supplies a device filesystem path, executable, command, argv, maintainer-script command, or TrollStore helper flag.

## Supported formats

- `.deb`: rootless jailbreak package, provider `bootstrap.procursus`.
- `.ipa` / `.tipa`: persistent application package, provider `package.trollstore`.

## Staging contract

Package staging lives under a RootTools-owned directory and a dedicated SQLite catalog. Defaults:

- package bytes: `/var/mobile/Library/RootTools/packages/<packageId>.<deb|ipa|tipa>`
- metadata database: `/var/mobile/Library/RootTools/packages.sqlite3`

Only the daemon derives the final staging path from a validated opaque package ID.

Limits/invariants:

- package maximum: 2 GiB;
- decoded chunk maximum: 256 KiB;
- chunks must be strictly sequential;
- files use `O_NOFOLLOW` and mode `0600`;
- each chunk is synced before the received-size ledger advances;
- staging starts in `uploading`, moves to `ready` only after SHA-256 verification;
- hash mismatch or package-identity mismatch moves the record to `failed`;
- a non-installed staged package can be explicitly discarded.

## Package identity

The host/UI may provide an optional expected identifier. If present, it is an assertion, not a source of truth.

DEB:

- fixed `dpkg-deb -f <staged-file> Package` extracts the package ID.

IPA/TIPA:

- the daemon parses the ZIP central directory itself and must find exactly one top-level `Payload/*.app/Info.plist`;
- stored and DEFLATE-compressed plist entries are read with bounded zlib decompression;
- CoreFoundation parses XML or binary plist data;
- `CFBundleIdentifier` becomes the package identity.

If the caller omitted the identifier, the detected identifier is persisted into the staging record. If a caller supplied one and it differs, the package is rejected before installation.

## Install capabilities

### `device.package.install-deb` — R2

Requirements:

- state is `ready`;
- format is `deb`;
- owner/admin confirmation is authenticated by the daemon;
- DEB Package ID still matches staged metadata at install time;
- Procursus provider resolves an executable fixed `dpkg`.

Execution:

If Procursus `apt-get` is available, RootTools uses fixed `apt-get install -y --no-remove <daemon-derived-staged.deb>` so configured repositories may satisfy dependencies without allowing automatic package removal. If `apt-get` is unavailable, the adapter falls back to fixed `dpkg -i <daemon-derived-staged.deb>`.

Post-condition:

fixed `dpkg-query -W -f=${Status} <expected-package-id>` must report `install ok installed`.

Package-manager child processes have a bounded 180-second execution window. Metadata/status probes use shorter bounds. A hung package/helper is terminated rather than permanently blocking the privileged daemon.

### `device.package.install-ipa` — R2

Requirements:

- state is `ready`;
- format is `ipa` or `tipa`;
- daemon-authenticated owner confirmation;
- TrollStore provider resolves the root helper from the installed TrollStore app bundle.

Execution is pinned to:

`trollstorehelper install custom <daemon-derived-staged-file>`

The helper argv is internal and not caller-configurable.

Post-condition:

the expected bundle ID must resolve through the installed application database (`uicache -i`).

## Managed lifecycle (v0.7)

RootTools keeps verified package artifacts after a successful install. When a newer managed artifact for the same identifier becomes active, the prior `installed` record becomes `retained`. This gives RootTools a bounded rollback primitive without snapshotting arbitrary system files.

- `installed -> retained` happens only when a different verified artifact for the same identifier installs successfully.
- `retained -> installed` is an R2 rollback through the same fixed provider and post-condition path.
- `installed -> uninstalled` is an R2 managed uninstall; the verified artifact remains available for explicit reinstall.
- DEB rollback may add `--allow-downgrades` only to the fixed apt install path. Ordinary installs do not.
- TrollStore uninstall is pinned to `trollstorehelper uninstall <managed-bundle-id>`; callers never supply an app path.
- `/v1/packages/history` records package install/rollback/uninstall events in addition to the global privileged Action audit.

Uninstall intentionally applies only to a RootTools package record in `installed` state. RootTools does not expose a generic device-wide uninstall-by-identifier primitive.

## Host workflow

`Scripts/device_service.py` provides the ADB-like typed client:

```text
package-stage <file> [--identifier ...]
package-list
package-history
package-install <packageId> --confirm
package-uninstall <packageId> --confirm
package-rollback <packageId> --confirm
package-discard <packageId>
```

The stage command computes SHA-256 locally, creates a random package ID, uploads in 256 KiB chunks, and commits the verified package. Installation uses the package record to select DEB vs IPA/TIPA; the host never sends a provider path.

Typical Mac flow:

```bash
python3 Scripts/device_service.py package-stage ./Example.ipa
python3 Scripts/device_service.py package-list
python3 Scripts/device_service.py --token-file .roottools-token package-install <packageId> --confirm
```

The default host credential is the lower-privilege Agent credential. The install step intentionally requires the owner/admin token file in addition to `--confirm`; an Agent cannot promote its own staged package into an R2 install.

The two-step `package-stage` -> `package-install --confirm` split is deliberate: transport/staging is R1, while package execution is a separate R2 owner-confirmed decision. A convenience wrapper must not silently collapse that approval boundary.

## iOS owner UI

The Packages screen uses the system file importer. It:

- hashes and stages the selected file;
- lists package state and detected identity;
- shows provider-aware install confirmation;
- invokes the same R2 install capability as the host;
- allows R2 uninstall for the active managed artifact;
- allows R2 rollback for a retained verified artifact;
- shows recent package lifecycle history;
- supports discard for non-installed staged records.

`RootTools.app` remains UID 501. Package installation remains in the UID 0 daemon.

## Self-update boundary

Installing a DEB that replaces `roottools-execd` may terminate the process that is currently producing the install receipt. Therefore RootTools self-update is intentionally not considered complete merely because generic DEB install exists.

v0.8 implements this boundary with the independent `roottools-updater` described in `docs/architecture/self-updater.md`. The serving daemon now:

1. accepts only a ready `com.arthur.roottools` DEB;
2. persists an R2 owner-confirmed queued update and completes its normal receipt;
3. launches the updater only after the request connection has closed;
4. lets the updater perform a RootTools-only payload switch and authenticated version health check;
5. restores request-scoped sibling backups if the new daemon is unhealthy.

Generic install/uninstall/rollback explicitly reject RootTools itself. Full recovery from power loss during the multi-file switch remains production-hardening work rather than being hidden inside the normal package path.

## Security properties

- no arbitrary remote shell;
- no arbitrary device path;
- no arbitrary executable or argv;
- package bytes are hash-bound before install;
- package identity is inspected before install;
- R2 is owner-confirmed at the daemon, not trusted from Agent input;
- installation goes through a provider binding owned by RootTools;
- post-condition determines success;
- all semantic requests retain idempotency, revision, audit, and `providerId` semantics.
