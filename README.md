# RootTools Runtime

RootTools Runtime is a policy-controlled privileged device runtime for **iOS 16+ Dopamine/rootless jailbreak devices**. It combines an Owner-facing SwiftUI app with a persistent UID 0 daemon and exposes typed device capabilities instead of a general-purpose root shell.

> This repository is the canonical RootTools Runtime product line and release source. The older `RootTools` repository is no longer the distribution source for this runtime.

## What it does

RootTools currently provides:

- layered permission policy: Hard Policy → Owner Profile → Principal Grants → runtime conditions;
- Restricted / Standard / Developer / Custom Owner modes;
- structured device, performance, storage, battery, thermal, app and process observation;
- application launch/terminate and durable lock-aware task execution;
- read-only Procursus/dpkg package inventory;
- verified DEB / IPA / TIPA staging and RootTools-managed package lifecycle;
- scoped Files Manager over declared `mobile` and `bootstrap` roots;
- Remote Worker mode with display assertion, low-brightness target and thermal gating;
- semantic UI operations (`observe`, `tap`, `type`, `swipe`) through a fixed ZXTouch provider;
- independent RootTools self-update with allowlisted payload switching, health checks and rollback;
- opt-in Remote Access sessions bound to a Tailscale address and one Named Host Principal.

RootTools deliberately does **not** expose arbitrary caller-provided shell commands, executable paths, argv, Frida scripts, ElleKit injection or R3 device-critical execution.

## Architecture

```text
Owner UI / trusted Host / future Skill
                 │
                 ▼
          Command Gateway
                 │
      identity + grants + policy
                 │
                 ▼
          Durable Task Runtime
                 │
                 ▼
        fixed semantic Provider
                 │
                 ▼
       post-condition + audit
```

The foreground app runs as the normal mobile user. Privileged work is owned by `roottools-execd`, a persistent UID 0 daemon. Remote caller identity is independent from transport.

Key documents:

- [Product definition](docs/product/product-definition.md)
- [Current engineering state](docs/handoff/CURRENT_STATE.md)
- [Permission model](docs/architecture/permission-model.md)
- [Command Gateway](docs/architecture/command-gateway.md)
- [Task Runtime](docs/architecture/task-runtime.md)
- [Self-Updater](docs/architecture/self-updater.md)
- [Remote Access](docs/architecture/remote-access.md)
- [Remote Access physical qualification](docs/validation/remote-access-physical-runbook.md)
- [Repository maintenance boundaries](docs/maintenance/repository-boundaries.md)
- [Release process](docs/release-process.md)

## Releases and installation

Installable builds are uploaded to the repository's **Releases** page. Release assets include the complete rootless `.deb`, the matching TrollStore-compatible `.ipa`, `SHA256SUMS`, `SOURCE_BUILD_MANIFEST.json`, and `INSTALL.md`.

For a Dopamine/rootless device, the normal installation flow is:

1. Download the latest `roottools_<version>_iphoneos-arm64.deb` from Releases.
2. Verify the SHA-256 checksum if possible.
3. Install the DEB with a compatible rootless package manager such as Sileo, or with `dpkg` from an already trusted jailbreak shell.
4. Open **Root Tools** and verify that the daemon status is online and UID 0.

The IPA contains the foreground Owner app only. It is useful for TrollStore installation or app recovery, but it does not install the UID 0 daemon, updater, or launchd jobs. Install the matching DEB when the full RootTools Runtime is not already present.

See [INSTALL.md](INSTALL.md) for the complete procedure and recovery notes.

## Remote Access model

Remote Access is intentionally **Owner initiated**. The daemon does not expose a privileged listener on ordinary Wi-Fi/cellular interfaces or `0.0.0.0`.

When the Owner enables a Remote Session in the app:

- the listener is bound only to a detected Tailscale IPv4 address (`100.64.0.0/10`);
- one active Named `host` Principal is selected;
- the Owner token and legacy Agent token are rejected on the remote listener;
- the Host receives only its explicitly granted R0/R1 capabilities;
- the session automatically expires and can be stopped or invalidated by revoking the Principal;
- UI work still waits for an unlocked visible device.

This provides a controlled way to hand the device to a remote automation host without publishing a root service to the public internet.

## Local development

Requirements:

- macOS with Xcode and iPhoneOS SDK;
- `clang`, Swift toolchain and `xcodebuild`;
- `xcodegen` for project regeneration;
- optional `libimobiledevice` tools or `pymobiledevice3` for USB transport;
- a test device is required for physical jailbreak qualification.

Run the source contract suite:

```bash
bash Scripts/test.sh
```

Build the Release app, daemon and updater:

```bash
bash Scripts/build.sh
```

Build a rootless DEB:

```bash
python3 Scripts/package-rootless-deb.py
python3 Scripts/package-ipa.py
```

Local credentials generated by build/test tooling (`.roottools-token` and `.roottools-agent-token`) are ignored by Git, normalized to owner-only (`0600`) permissions, and must never be committed.

## Release philosophy

Full iOS Release builds run on the maintainer Mac instead of GitHub-hosted macOS runners. This keeps the build environment close to the physical-device qualification environment and avoids consuming hosted Actions macOS minutes.

The one-command release path is:

```bash
Scripts/release-local.sh 0.23.0-1 --draft
```

It runs tests, builds locally, packages the DEB and IPA from the same credential/version inputs, generates checksums and a source/build manifest, tags the exact commit and uploads the resulting artifacts to GitHub Releases.
The requested release version must match the repository [`VERSION`](VERSION) file; the same source drives the daemon-reported version, App version, package metadata and physical-device verifier.

RootTools binaries are personalized builds because the App and daemon contain matching device credentials. The canonical source repository is public, so its release script requires `--draft`; publishing binary assets requires an explicitly private release repository or a future credential-provisioning design that removes secrets from distributed binaries.

GitHub Actions is manual-only and limited to lightweight repository hygiene checks.

## Security

RootTools is privileged software. Review [SECURITY.md](SECURITY.md) before exposing any transport or adding capabilities. In particular:

- R3 and `device.raw-shell` remain hard-disabled;
- Named Principals start with zero grants;
- persistent grants are restricted to compiled R0/R1 capability IDs;
- Remote Access never authenticates with the Owner token;
- filesystem operations are scope-based rather than arbitrary absolute paths;
- update payloads are identity checked and allowlisted before switching.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). Changes to the privileged surface should include tests, capability/risk classification, Provider ownership, post-condition semantics and documentation.

## Project status

The current stable source line is **v0.22**. Physical-device qualification is tracked separately from source/build validation; see `docs/handoff/CURRENT_STATE.md` and `docs/validation/` for the exact boundary.
