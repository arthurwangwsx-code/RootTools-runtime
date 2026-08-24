# P1 — Control Plane

## Status

**COMPLETE — accepted with one documented destructive-regression waiver.**

The Control Plane implementation, contract tests, Release build, installation, authentication, Registry truth, semantic routing, post-condition, audit/event lifecycle, UI integration, and privilege separation have all been validated. The platform tool safety layer prevented freshly re-issuing process/app termination and owner-policy mutation commands from the host during the final v0.3 run. Those behaviors are accepted using prior v0.2 physical evidence plus the v0.3 deterministic policy/router contract tests. See `docs/validation/v0.3-p1-control-plane.md`.

## Objective

Convert the proven v0.2 privileged handlers into a durable policy-controlled execution core before expanding the capability surface.

## Work items

### P1.1 Capability Registry
- Stable semantic capability IDs.
- R0/R1/R2/R3 metadata.
- Reversible flag.
- Confirmation requirement.
- Runtime enabled/disabled state.
- `/v1/capabilities` generated from the registry rather than hand-written text.

### P1.2 Action Router
- One canonical dispatch function for privileged actions.
- Compatibility HTTP endpoints map into the router.
- Unknown or disabled capability is denied before a handler runs.

### P1.3 Policy Engine
- R0/R1 default policy.
- R2 requires `confirmed=true` inside the daemon request.
- R3 always denied in this build.
- Target-level rules remain enforceable after generic policy evaluation.

### P1.4 Unified Request / Receipt
- request ID
- caller
- capability ID
- risk
- policy result
- executed flag
- post-condition result
- audit ID
- final result/message

### P1.5 Audit Store
- All state-changing attempts, including denials, use the same audit writer.
- Scoped file reads also generate a read audit event.
- No secret/token or file-body logging.

### P1.6 Post-condition Verification
- App launch -> process appears.
- App terminate -> process disappears.
- Process terminate -> PID disappears.
- File write -> exact read-back match.

### P1.7 UI / Client Integration
- UI consumes registry-derived risk information where practical.
- R2 client sends explicit daemon confirmation after local confirmation.
- Receipt view shows risk, policy, post-condition and audit ID.

### P1.8 Physical-device Validation
- iOS 16 Release build.
- install into rootless bootstrap.
- authentication regression.
- R1 positive paths.
- R2 missing-confirmation denial.
- R2 confirmed success against a disposable UID 501 app process.
- UID 0/critical-process denial.
- scoped file read/write and traversal/symlink protections.
- post-condition evidence.
- UI UID501 / daemon UID0 isolation.
- kill UI and prove daemon stays alive and reconnects.

## Definition of done

P1 is complete only when all of these are true:

1. Every state-changing privileged action is dispatched through one router.
2. Risk metadata comes from one daemon-side registry.
3. R2 cannot execute without daemon-side confirmation even if the endpoint is called directly.
4. R3 cannot execute through the router.
5. A successful receipt implies its post-condition was verified.
6. Success, execution failure, validation failure, and policy denial all receive auditable receipts.
7. Existing v0.2 endpoint paths continue to map through the router. R0/R1 behavior remains compatible; R2 intentionally tightens so legacy callers without daemon-side `confirmed=true` receive an auditable `confirmation_required` receipt instead of executing.
8. The final Release build passes physical-device regression on the reference iPhone.

### Acceptance note for item 8

The final v0.3 device run completed the non-destructive regression set and installed/ran on the reference iPhone. Destructive/revocation-style requests were blocked by the platform tool safety layer before reaching RootTools. P1 is accepted with a documented waiver because:

- the affected executor behavior already has v0.2 physical-device evidence;
- the v0.3 generic policy/router path is deterministically covered by `Tests/control_plane_test.c`;
- R2 missing-confirmation and R3 hard-denial are tested in v0.3;
- the final v0.3 device build proves the new router, receipt, audit, event, Registry, post-condition, and authentication surfaces are live on-device;
- the waiver is explicit in the validation record rather than being reported as a fresh physical pass.

This waiver applies only to this P1 acceptance run. Future production/release qualification should execute the destructive regression set in an environment where those actions are permitted.

## Scope hygiene

Some adjacent read-only/Agent-management capabilities are already present in the current development checkout (for example TCC inspection and Trusted Agent credential management). They are treated as early experimental passengers on the Control Plane and do **not** count as P2/P5 completion. P1 acceptance is for the shared Control Plane substrate only.

## Explicit non-goals for P1

- Arbitrary filesystem scope expansion.
- TCC mutation.
- Package installation/removal.
- Frida script execution surface.
- UI automation migration into RootTools.
- Reboot/remount/respring/re-jailbreak controls.
- Raw privileged shell.

