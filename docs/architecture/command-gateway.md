# Command Gateway

## Goal

All external instructions converge on one deep module before privilege or provider selection. Transport-specific code authenticates a principal and submits a semantic command; it does not implement device actions.

```text
RootTools UI ─┐
Mac Host ─────┼──> Transport Adapter ──> Command Gateway ──> Policy ──> Capability Router
AiBox ────────┤                                            │
Network Skill ┘                                            └──> Receipt / Audit / Events
```

## Canonical interface

`POST /v1/commands/submit` is the canonical HTTP ingress starting with the post-v0.9 line. `/v1/action` remains a compatibility adapter.

The initial body keeps the existing typed capability contract:

```json
{
  "requestId": "stable-idempotency-key",
  "capabilityId": "device.app.launch",
  "expectedRevision": 42,
  "confirmed": false,
  "bundleID": "com.example.app"
}
```

Important properties:

- authenticated caller identity is derived from the credential/transport, never trusted from a body `caller` string;
- `requestId` is durable idempotency identity;
- `expectedRevision` is optimistic concurrency control;
- `confirmed=true` is meaningful only when the authenticated principal is allowed to provide confirmation;
- capability-specific parameters remain typed and bounded;
- success is determined by post-condition rather than process exit alone.

## Principal model

The existing `owner` and legacy `agent` roles remain compatibility classes. v0.11 persists a `principalId` and `principalKind` independently from transport:

```text
owner-ui
host:<device-or-key-id>
app:<bundle-id>
skill:<skill-id>
automation:<workflow-id>
```

Every principal receives grants over stable capability IDs. A transport may reconnect or change without changing principal identity.

v0.11 completes the identity/credential/revocation half of this model. Per-principal capability/resource grants remain the next increment; until then named principals share the existing Agent capability policy.

## Transport adapters

### Loopback HTTP

Used by RootTools UI and, later, a same-device AiBox adapter. The daemon remains loopback-only.

### USB / Mac Bridge

Mac forwards the same Device Service protocol over usbmux. This is the closest equivalent to an ADB server/client topology.

### Network relay

The privileged runtime establishes an outbound authenticated connection to a trusted relay. Remote Skills submit commands through that relay. The daemon does not bind a privileged public LAN/WAN port.

### Local app integration

AiBox on the same device uses loopback with a separately provisioned app principal. RootTools should support pairing/revocation rather than sharing the Owner token or a generic Host token.

## Command lifecycle

```text
received
  -> authenticated
  -> accepted / denied
  -> queued (optional)
  -> started
  -> completed / failed / cancelled
```

The existing execution event ledger already covers accepted/started/completed/failed. P10 Command Gateway work should extend it with principal and ingress metadata without changing capability semantics.

## Approval model

- R0: read-only; grant may allow unattended use.
- R1: reversible/scoped mutation; grant may allow unattended use when policy permits.
- R2: explicit owner approval, a pre-authorized one-shot grant, or a workflow policy with a bounded scope.
- R3: not exposed to Agent/Skill callers by default.

Remote callers may never convert their own boolean into Owner confirmation. Future unattended R2 must use a daemon-issued signed/one-shot approval artifact.

## Compatibility

Callers negotiate through `/v1/hello` feature flags. `commandGateway=true` means the canonical ingress is available. Existing `/v1/action` callers remain functional during migration.
