# Installing RootTools Runtime

RootTools Runtime targets iOS 16+ Dopamine/rootless jailbreak environments.

## Recommended install

1. Open the latest release on GitHub.
2. Download `roottools_<version>_iphoneos-arm64.deb` and `SHA256SUMS`.
3. Verify the checksum when possible.
4. Install the DEB using a compatible rootless package manager such as Sileo.
5. Open **Root Tools** from the Home Screen.
6. Confirm that Overview reports the privileged runtime online and `UID 0`.

The package installs:

- `/var/jb/Applications/RootTools.app`
- `/var/jb/usr/local/bin/roottools-execd`
- `/var/jb/usr/local/bin/roottools-updater`
- `/var/jb/Library/LaunchDaemons/com.arthur.roottools.execd.plist`
- `/var/jb/Library/LaunchDaemons/com.arthur.roottools.updater.plist`

The package registers the app with `uicache` and starts the launchd jobs in the Dopamine foreground-user bootstrap domain.

## Upgrade

For an already-modern RootTools installation, prefer the in-product typed Self-Updater when it is available and healthy. The Self-Updater verifies a staged `com.arthur.roottools` DEB, restricts extracted payload paths, signs candidate binaries, switches targets, checks the new daemon version and rolls back if health verification fails.

A downloaded DEB may also be installed explicitly through Sileo when recovering from a broken runtime or when the current device is not remotely reachable.

## Remote Access after installation

Remote Access requires a working Tailscale connection on the iPhone before a remote host can reach RootTools.

1. Connect Tailscale on the iPhone.
2. Open **Root Tools → Remote Access**.
3. Select an active Named Host Principal.
4. Choose a bounded session duration.
5. Start the Remote Session.

The remote listener binds only to a Tailscale IPv4 address and accepts only the selected Named Host Principal. It does not accept the Owner token and does not bind to ordinary public/Wi-Fi interfaces.

## Recovery notes

If the foreground icon disappears but the daemon remains healthy, re-register the app with `uicache` from an already trusted jailbreak shell. Modern updater qualification treats app registration as part of successful deployment.

If the daemon is unavailable after installation, inspect the two launchd jobs and their logs under `/var/mobile/Library/Logs/`. Avoid introducing a generic root listener as a recovery shortcut.

If the device has rebooted out of the jailbroken state, re-enable the jailbreak before expecting `/var/jb` services to become available.

## Integrity

Every official release includes `SHA256SUMS`. The checksum should match the downloaded DEB before installation.

## IPA / TrollStore app recovery

`RootTools_<version>.ipa` contains the Root Tools foreground app built from the
same source, version and credential inputs as the release DEB. Install it with
TrollStore when the app needs to be restored without replacing an already
matching daemon.

The IPA alone is not the full privileged Runtime. It does not install
`roottools-execd`, `roottools-updater`, or either launchd plist. If Overview does
not report the matching daemon online at UID 0, install the same release's DEB.
Do not pass this ad-hoc/TrollStore package to `ideviceinstaller`, which expects
a normal Apple development or distribution signature.

RootTools artifacts are personalized and are stored in a maintainer-only draft
when the source repository is public. Sign in to the authorized GitHub account
before downloading them.
