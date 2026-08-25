# Named Principal Trust Model

## Goal

RootTools must be able to answer **who issued this command** independently from how the command arrived. A Mac over USB, AiBox over loopback, and a future network Skill must not collapse into one anonymous Agent token.

v0.11 introduces named principals as the first durable caller-identity layer.

## Principal kinds

Supported principal kinds:

- `host` — Mac/PC Device Bridge installations;
- `app` — another app such as AiBox;
- `skill` — future remote/network Skill identity;
- `automation` — durable workflow/scheduler identity.

Example stable IDs:

```text
host:macbook-pro
app:aibox-main
skill:shopping-research
automation:nightly-device-check
```

## Credential storage

Creation is an R2 Owner action.

1. RootTools generates 24 random bytes.
2. The 48-character hexadecimal credential is returned once in the action receipt.
3. RootTools stores only SHA-256 of the credential in the principal SQLite store.
4. The principal catalog never exposes plaintext credentials or hashes.
5. Revocation changes the principal state to `revoked`; the credential stops authenticating immediately.

Default store:

`/var/mobile/Library/RootTools/principals.sqlite3`

## Authentication projection

The transport still supplies `X-RootTools-Token`, but authentication resolves it to a stable caller identity:

```text
Owner token             -> roottools-ui
Legacy Agent token      -> trusted-host-agent
Named principal token   -> principal:<principalId>
```

Caller identity in a request body is ignored for trust. Receipts, idempotency records, execution events and audit use the authenticated caller identity.

## Owner-only management

`GET /v1/principals/catalog` is Owner-only.

State-changing management goes through the canonical Command Gateway:

- `device.principal.create` — R2;
- `device.principal.revoke` — R2.

A named principal is authenticated as Agent class and therefore cannot self-confirm these R2 actions.

## v0.11 boundary

v0.11 separates **identity and credential lifecycle** but not yet per-principal authorization grants. All active named principals still operate inside the same global Agent capability surface and daemon risk policy.

The next increment adds daemon-owned grants such as:

```text
principalId
  -> allowed capability IDs / domains
  -> optional resource scope
  -> optional expiry
  -> optional unattended R1 policy
```

No future grant may enable a hard-disabled R3 capability or raw shell.

## Product mapping

The RootTools `Agents` tab is the human control center for these device-side identities. It is deliberately different from AiBox Agent personas:

- AiBox Agent = reasoning/persona/model/memory/tool plan;
- RootTools Principal = authenticated authority to request device capabilities.

One AiBox installation may host many AiBox personas while using one paired RootTools app principal.

## Mac administration

The typed Device Service client exposes owner-only principal management without adding a privileged shell:

```bash
python3 Scripts/device_service.py \
  --token-file .roottools-token \
  principal-create host:macbook-pro host "MacBook Pro" \
  --confirm \
  --save-to .roottools-macbook-principal

python3 Scripts/device_service.py \
  --token-file .roottools-token \
  principal-list

python3 Scripts/device_service.py \
  --token-file .roottools-token \
  principal-revoke host:macbook-pro \
  --confirm
```

The saved principal credential can then be supplied with `--token-file` for ordinary Agent-class Device Service calls. The Owner token remains reserved for administration/R2 approval.
