# Changelog

## Unreleased

### Changed

- Local Owner and Agent credentials are created and normalized with owner-only (`0600`) permissions.
- The local release workflow rejects non-canonical origins, repositories and upstream branches.
- Package, daemon, App and physical-verifier versions are derived from the repository `VERSION` file.
- DEB tar/gzip/ar metadata uses `SOURCE_DATE_EPOCH` (defaulting to the Git commit time) for reproducible release artifacts.
- A three-phase physical qualification tool now proves least-privilege Remote Access over USB preparation, enforced off-USB execution, and stop/expiry/revoke cleanup.

### Security

- Credential loading rejects symbolic links and malformed token contents.
- Repository boundaries now explicitly separate the iOS Runtime, Android toolbox and legacy migration checkout.
- Packaging rejects stale build stamps, App-version drift and caller-supplied versions that do not match `VERSION`.
- Repeated packaging of the same committed inputs is byte-for-byte stable.

## 0.22.0-3

### Added

- Owner-initiated Remote Access sessions over a Tailscale IPv4 address.
- Remote Access UI for selecting a Named Host Principal and bounded session duration.
- Remote listener authentication restricted to the selected Named Host Principal.
- Remote Access controller tests and HTTP contract coverage.

### Changed

- Self-Updater dispatch is hardened around its independent launchd job.
- Self-Updater health now includes foreground app registration/discoverability expectations.
- Locked-device task scheduling avoids head-of-line starvation between queued UI tasks.
- Release production is local-first; GitHub is the artifact host rather than the iOS build machine.

### Security

- Remote listener rejects Owner and legacy Agent credentials.
- Remote listener never binds to ordinary Wi-Fi/cellular addresses or `0.0.0.0`.
- Remote Principal grants remain restricted to compiled R0/R1 capabilities.

## 0.21.0-1

- Completed the one-time trusted v0.9 bootstrap migration on the reference Dopamine device.
- Corrected Dopamine rootless launchd-domain handling.
- Hardened host USB tooling so `pymobiledevice3` is only required as a fallback.
- Recorded foreground app registration as a deployment post-condition.

## 0.20.0-1

- Added product-level Scoped Files Manager over declared `mobile` and `bootstrap` roots.
- Added nested no-follow path traversal, metadata, bounded text read/edit/create and symlink protections.

## 0.19.0-1

- Added Remote Worker mode, battery/thermal observation, low-brightness target and thermal gating of UI work.

## Earlier milestones

See `docs/validation/` and `docs/handoff/CURRENT_STATE.md` for the full v0.2–v0.18 milestone history.
