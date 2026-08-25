# Rootless `.deb` deployment

RootTools normally deploys through `Scripts/install-jailbreak.sh`. Some host execution environments block privileged writes to `/var/jb` or do not provide `iproxy`, `idevice_id`, `ideviceinfo`, or host-side `ldid`. The project therefore also supports a one-time rootless Debian package for explicit on-device owner installation.

## Build

```bash
bash Scripts/build.sh
python3 Scripts/package-rootless-deb.py
```

Default output:

`build/packages/roottools_0.9.0-2_iphoneos-arm64.deb`

## Package allowlist

The package data archive contains only:

- `/var/jb/Applications/RootTools.app/**`
- `/var/jb/usr/local/bin/roottools-execd`
- `/var/jb/usr/local/bin/roottools-updater`
- `/var/jb/Library/LaunchDaemons/com.arthur.roottools.execd.plist`
- `/var/jb/Library/LaunchDaemons/com.arthur.roottools.updater.plist`

The build pre-signs the App executable, daemon, and independent updater on the
Mac using host `ldid` when available, otherwise macOS `codesign -s -`. This
avoids bootstrap failure when the target jailbreak does not yet have `ldid`.

The post-install script:

1. optionally refreshes those signatures with a device-side `ldid` when present;
2. otherwise keeps the build-time ad-hoc signatures;
3. fixes executable/ownership metadata;
4. reloads the RootTools daemon and registers the one-shot `system/com.arthur.roottools.updater` recovery job;
5. refreshes the RootTools app registration with `uicache`.

It does not expose or install a general root shell.

## Why this path exists

The package path remains the bootstrap deployment fallback. Starting with v0.8, once a trusted build containing `roottools-updater` is installed, later RootTools DEBs can be staged through the typed Package Controller and scheduled through the owner-confirmed self-update capability without host-side privileged shell replacement.
