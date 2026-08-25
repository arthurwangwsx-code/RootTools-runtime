# Rootless `.deb` deployment

RootTools normally deploys through `Scripts/install-jailbreak.sh`. Some host execution environments block privileged writes to `/var/jb` or do not provide `iproxy`, `idevice_id`, `ideviceinfo`, or host-side `ldid`. The project therefore also supports a one-time rootless Debian package for explicit on-device owner installation.

## Build

```bash
bash Scripts/build.sh
python3 Scripts/package-rootless-deb.py
```

Default output:

`build/packages/roottools_0.7.0-1_iphoneos-arm64.deb`

## Package allowlist

The package data archive contains only:

- `/var/jb/Applications/RootTools.app/**`
- `/var/jb/usr/local/bin/roottools-execd`
- `/var/jb/Library/LaunchDaemons/com.arthur.roottools.execd.plist`

The post-install script:

1. signs the App executable and daemon with the jailbreak bootstrap's `/var/jb/usr/bin/ldid`;
2. fixes executable/ownership metadata;
3. reloads only `system/com.arthur.roottools.execd`;
4. refreshes the RootTools app registration with `uicache`.

It does not expose or install a general root shell.

## Why this path exists

The package path is a deployment fallback, not a normal Agent capability. Package installation requires explicit device-owner approval in Sileo/Filza and is intentionally outside the model-facing Device Ops API. Once a trusted RootTools build with a future typed self-update flow is installed, ordinary updates should no longer require host-side privileged shell deployment.
