# RootTools Permission Model

## Goal

RootTools is a privileged device runtime, not a raw jailbreak shell. Permission decisions therefore compose multiple independent layers instead of treating possession of a credential as root authority.

The effective decision is:

`Hard Policy -> Owner Policy Mode -> Principal Grant -> R2 Approval -> Runtime Conditions -> Provider/Post-condition`

Every layer may narrow authority. No lower layer may widen a denial from an earlier layer.

## Layer 1 — Hard Policy

The Capability Registry compiled into `roottools-execd` is the maximum product surface.

- R3 capabilities remain hard disabled.
- `device.raw-shell` is never enabled by a runtime profile.
- Provider argv, arbitrary executable paths and arbitrary Frida/ElleKit scripts remain implementation details rather than caller-facing permissions.

Developer Mode cannot change Hard Policy.

## Layer 2 — Owner Policy Mode

The on-device Owner can switch the device-wide policy mode through the typed R2 capability `device.policy.set-mode`.

### Restricted

- enables compiled R0 observation capabilities;
- disables R1/R2 operations;
- keeps `device.policy.set-mode` available as an Owner recovery escape hatch;
- is intended for travel, troubleshooting and temporary lockdown.

### Standard

- enables all compiled non-R3 capabilities;
- R2 capabilities still require an explicit trusted Owner approval;
- remote principals remain constrained by their own grants.

### Developer

- enables all compiled non-R3 capabilities in one action;
- local Owner/UI R2 requests are treated as approved without an additional per-action confirmation;
- remote Host/App/Skill/Automation principals are **not** elevated;
- R3/raw shell remain hard blocked.

### Custom

Changing an individual capability switch marks the device policy as Custom. Custom retains the same hard-policy invariants while allowing manual narrowing.

## Layer 3 — Principal Grants

Named command principals authenticate identity only. New principals begin with zero delegated authority.

Persistent grants:

- are exact capability IDs;
- are limited to compiled R0/R1 capabilities;
- may expire;
- are checked for reads and mutations;
- are re-evaluated immediately before a durable task executes.

Developer Mode never adds, removes or bypasses Principal Grants.

For development convenience the UI and Mac client may apply the full grantable set (all compiled R0/R1 capabilities) as a client-side batch. The daemon still validates each grant independently.

## Layer 4 — R2 Approval

R2 represents system-impacting actions such as package installation, principal administration and process termination.

- Standard mode: explicit trusted Owner approval is required.
- Developer mode: only the authenticated local Owner may auto-approve R2.
- Named principals cannot self-assert approval with `confirmed=true`.
- Persistent R2 principal grants are not supported.

A future one-shot approval token may authorize a specific R2 task without weakening this model.

## Layer 5 — Runtime Conditions

Authority does not imply executability. The Task Runtime also checks current device conditions:

- lock/display state;
- UI readiness;
- provider availability;
- package/file scope validity;
- current capability enablement and Principal Grant;
- task lifecycle/idempotency state.

Locked UI work waits rather than bypassing the device passcode.

## Layer 6 — Provider and Post-condition

The Provider Plane chooses the fixed implementation for an allowed semantic capability. Success is recorded only after a post-condition is checked where applicable.

Receipts and audit records include capability, provider, authenticated caller, policy result and post-condition evidence.

## Owner UX

The Settings and Capabilities surfaces expose:

- Restricted / Standard / Developer policy profiles;
- per-capability Custom switches;
- immutable R3/raw-shell invariants;
- Developer Mode as an explicit high-privilege Owner convenience;
- per-principal R0/R1 grants in the Agents tab.

The UI must never describe Developer Mode as granting root to remote callers. It expands only the local Owner execution policy.
