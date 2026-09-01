# RootTools Repository Maintenance Boundaries

## Canonical ownership

RootTools has two active platform products, one private iOS artifact repository,
and one legacy migration checkout:

| Product | Canonical repository | Local checkout | Responsibility |
| --- | --- | --- | --- |
| iOS RootTools Runtime | `arthurwangwsx-code/RootTools-runtime` | `mobile-ios/RootTools-runtime` | iOS app, UID 0 daemon, updater, Runtime documentation and releases |
| Android Root Tools | `arthurwangwsx-code/RootTools-Android` | `mobile-android/RootTools` | Android application, privileged routing and Android releases |
| iOS personalized artifacts | `arthurwangwsx-code/RootTools-runtime-releases` (private) | none required | Credential-bearing DEB/IPA assets, checksums and source/build manifests only |
| Legacy iOS snapshot | none | `mobile-ios/RootTools` | Temporary recovery snapshot only; no development or release authority |

The legacy iOS checkout contains an older Runtime tree but its `origin` points at the historical `arthurwangwsx-code/RootTools` Android repository. Its local history and remote `main` are different products. Do not pull, merge, push or release from that checkout.

## iOS maintenance flow

1. Start all Runtime work from a clean `RootTools-runtime` checkout whose `main` tracks `origin/main`.
2. Keep each privileged change typed, risk classified, Provider owned, post-condition verified and independently revertible.
3. Run `bash Scripts/test.sh` and `git diff --check` for every change.
4. Run `bash Scripts/build.sh` and package a rootless DEB for Swift, daemon, updater or packaging changes.
5. Publish official artifacts only through `Scripts/release-local.sh`; the script requires canonical public source and a private artifact target (or a public maintainer-only draft fallback).
6. Record source/build evidence separately from simulator and physical-device evidence.
7. Do not mark a version physically qualified until the installed device version and the required transport/runtime regressions have passed.

Never mirror commits manually between the legacy and canonical iOS checkouts. If previously unknown legacy work is discovered, identify exact commits and transplant only reviewed changes into a new canonical branch.

## Credential boundary

`.roottools-token` and `.roottools-agent-token` are the reserved `installed`
profile. Named candidates live under ignored
`.roottools-credentials/<profile>/`. All credential files are checkout-local
secrets and must remain owner-readable only. Build and test tooling normalizes
them to mode `0600`, rejects symbolic links and validates their fixed
hexadecimal representation before embedding or using them.

The public source repository owns Git history and annotated source tags. The
private artifact repository is not a second source of truth; its Release must
carry a manifest that points back to the exact public-source commit and tree.
Never place personalized binaries in the historical mixed `RootTools` repository.

Do not copy, delete or rotate a legacy credential merely to make the checkouts look consistent. First identify which installed daemon or host workflow uses it, transition the canonical checkout explicitly, and verify authentication before retiring the old credential.

## Legacy retirement gate

Keep the legacy iOS snapshot recoverable until all of the following are true:

- the canonical Runtime commit has passed contract tests, Release build and rootless DEB inspection;
- the canonical package is installed on the reference device and reports the expected version and UID 0 daemon health;
- required USB and off-USB/Tailnet regressions pass;
- no workspace script or documentation depends on the legacy checkout path;
- any credential still used by the installed runtime has been explicitly migrated or retired.

After those gates pass, archive or remove the legacy checkout as a separate, explicitly authorized cleanup. The Android canonical checkout is never part of that cleanup.
