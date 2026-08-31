# Security Policy

RootTools Runtime executes privileged operations on jailbroken devices. Treat every new capability and transport as security-sensitive.

## Security invariants

- `device.raw-shell` and R3 remain hard-disabled.
- Callers request semantic capabilities; they do not provide executable paths, argv or scripts.
- Named Principals are identity records with zero grants by default.
- Persistent Principal grants are exact compiled R0/R1 capability IDs.
- R2 requires trusted Owner authorization and is not persistently delegated to remote Principals.
- Filesystem APIs use declared scopes and no-follow traversal rather than arbitrary absolute roots.
- Self-update accepts only verified RootTools packages and an allowlisted payload shape.
- Remote Access is Owner initiated, time bounded, Tailscale-only and restricted to one selected Named Host Principal.
- Remote Access rejects Owner and legacy Agent credentials.
- The daemon does not bypass the device passcode.

## Sensitive local material

The following files are local credentials and are excluded from Git:

- `.roottools-token`
- `.roottools-agent-token`

Do not paste their contents into issues, logs, screenshots, test fixtures or documentation.
Build and test tooling must create these files with owner-only (`0600`) permissions, reject symbolic links, and reject malformed token contents. Do not copy credentials between legacy and canonical checkouts; rotate or migrate them only as part of an explicitly verified device transition.

The current App, daemon and updater binaries embed matching personalized
credentials. A Git ignore rule protects source control but does not remove those
values from compiled artifacts. Do not publish RootTools binary assets from a
public repository. The local release workflow therefore requires a draft on the
public canonical repository; public binary distribution remains blocked until
the artifacts are moved to a private repository or credentials are provisioned
securely on-device.

## Reporting a vulnerability

For a security issue, avoid opening a public issue containing exploit details or credentials. Contact the repository owner privately through GitHub account contact information and provide a minimal reproduction, affected version and expected security boundary.

## Public repository note

Repository history has been checked for the local RootTools token files and common private-key/GitHub-token patterns before public publication. Contributors should still review every change for secrets before pushing.
