# P3 — Jailbreak / Runtime Provider Platform

## Objective

Make jailbreak/runtime mechanisms replaceable implementation providers behind stable RootTools Device Ops.

## v0.5 implemented foundation

### Provider registry

- daemon-owned provider metadata and discovery;
- bounded probes for path, loopback service, and installed application presence;
- provider domains and priority metadata;
- capability -> provider binding table;
- authoritative `GET /v1/providers/catalog` endpoint.

### Router integration

- immediate semantic actions fail closed when their required provider is unavailable;
- deferred UI jobs may be persisted before the provider becomes ready;
- ActionReceipt and append-only audit records include `providerId`;
- provider choice cannot be supplied by the caller.

### Package provider planning

- `POST /v1/package/plan` supports `deb`, `ipa`, and `tipa`;
- DEB resolves to Procursus/dpkg with Sileo as interactive fallback;
- IPA/TIPA resolves to TrollStore;
- owner confirmation is part of the plan contract;
- raw shell/arbitrary executable execution remains excluded.

### iOS UI

- Providers screen grouped by provider domain;
- readiness and implementation identity;
- headless/unlock/persistence metadata;
- DEB and IPA/TIPA routing previews;
- Dashboard provider-plane health summary.

## Validation

- `provider_registry_test` validates registry/binding/package-plan invariants.
- HTTP contract validates provider catalog, package plan, and hello feature negotiation.
- Existing Control Plane and HTTP suites remain green.
- iOS 16 Release build succeeds without requiring xcodegen on the current host.

## Remaining P3 increments

The Provider Plane foundation is complete, but P3 as a whole remains in progress. Next increments are deliberately ordered:

1. Extract concrete executors from the monolithic daemon into provider-specific implementation files without changing capability contracts.
2. Add a typed Package Controller with RootTools-owned staging, metadata validation, R2 confirmation, provider-specific install adapters, and post-condition/rollback handling.
3. Implement the already-researched TrollStore helper contract as a fixed provider adapter only after RootTools-owned package staging and metadata verification are in place; do not expose TrollStore helper argv to callers.
4. Add Frida/ElleKit semantic runtime operations, never general scripts.
5. Add provider compatibility/version metadata and fallback selection for alternate jailbreaks.
6. Physical-device qualification of `/v1/providers/catalog` and provider-bound receipts once v0.5 deployment is available.

## Definition of done for full P3

- no semantic executor depends on a jailbreak/tool name outside its provider adapter;
- package install/upgrade is typed, staged, confirmed, audited, verified, and recoverable;
- at least one alternate implementation can replace a provider without changing the caller-facing capability;
- physical regression verifies provider selection, failure behavior, and recovery.
