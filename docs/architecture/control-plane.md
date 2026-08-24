# RootTools Control Plane Architecture

## Purpose

The Control Plane is the stable privileged boundary between callers and jailbreak/system mechanisms. It owns capability discovery, risk policy, typed dispatch, post-condition verification, and audit.

## Request flow

```text
SwiftUI / Device Ops / Automation
             |
             v
      ActionRequest
             |
             v
      Action Router
        |       |
        |       +--> Capability Registry
        |       +--> Policy Engine
        v
     Typed Handler
        |
        v
 Darwin API / fixed trusted adapter
        |
        v
 Post-condition verifier
        |
        v
 Audit Store -> ActionReceipt
```

## Capability Registry

The registry is the authoritative metadata source for privileged operations. A capability contains:

- `id`: stable semantic identifier.
- `risk`: R0/R1/R2/R3.
- `reversible`: whether the expected state can be restored cheaply.
- `confirmation`: `none`, `explicit`, or `localOnly`.
- `enabled`: whether this build/device may execute it.
- `description`: short operator-facing semantics.

Risk must never be inferred from endpoint names in the UI.

## Policy Engine

Default policy:

- **R0**: read-only. No explicit confirmation.
- **R1**: scoped/reversible state change. Audited, daemon policy required.
- **R2**: system-impacting. Daemon requires an explicit confirmation bit in the ActionRequest in addition to any UI confirmation.
- **R3**: device/data critical. Disabled in the model/automation surface and rejected by default.

Additional target-specific rules may be stricter than the registry. For example, process termination rejects UID 0 and critical process names even when the caller confirms R2.

## ActionRequest

P1 canonical request fields:

```json
{
  "requestId": "caller-generated-or-daemon-generated-id",
  "capabilityId": "device.process.terminate",
  "caller": "ui",
  "confirmed": true,
  "parameters": {}
}
```

Compatibility endpoints may construct this request internally.

## ActionReceipt

Every state-changing privileged attempt returns a receipt even when policy denies execution.

```json
{
  "ok": true,
  "requestId": "...",
  "auditId": "...",
  "capabilityId": "device.app.launch",
  "risk": "R1",
  "policy": "allowed",
  "executed": true,
  "postCondition": "verified",
  "message": "Application is running"
}
```

`ok=true` means policy allowed execution and the required post-condition was verified. It does not mean merely that a command returned zero.

## Audit Store

Audit entries are append-only JSON lines and record at minimum:

- timestamp
- requestId
- auditId
- caller
- capabilityId
- risk
- target summary
- policy result
- whether execution occurred
- post-condition result
- final success/failure
- message

The audit record must not contain secrets or full sensitive file contents.

## Post-condition verification

P1 rules:

- `device.app.launch`: verify the resolved executable has a mobile process after launch.
- `device.app.terminate`: verify the resolved executable no longer has a mobile process.
- `device.process.terminate`: verify the original PID disappears after SIGTERM within a bounded interval.
- `device.fs.write`: reopen and compare exact bytes after fsync.
- `device.fs.read`: read-only action; audit the scoped read but no state-change post-condition is required.

## Compatibility layer

The v0.2 endpoints remain accepted during P1:

- `/v1/actions/app-launch`
- `/v1/actions/app-terminate`
- `/v1/actions/process-terminate`
- `/v1/actions/file-read`
- `/v1/actions/file-write`

They map to the same registry, policy, router, audit, and verification path as the canonical action endpoint. There is no second privileged implementation path.

## Security invariants

- Loopback bind only.
- Build-time token authentication remains required.
- No `system()`/`popen()`.
- No caller-supplied executable path or argv.
- No arbitrary privileged shell endpoint.
- Filesystem mutations remain scope-based with path normalization and `O_NOFOLLOW`.
- R2 confirmation is checked by the daemon.
- R3 is rejected by the daemon.

