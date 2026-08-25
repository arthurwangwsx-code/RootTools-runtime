# RootTools Provider / Adapter Architecture

## Purpose

RootTools exposes stable semantic Device Ops while the underlying iOS implementation may come from Dopamine, Procursus, TrollStore, Sileo, Frida, ElleKit, SpringBoard, ZXTouch, or native Darwin APIs.

The product boundary is:

`RootTools.app / Agent / Automation -> Capability -> Policy -> Provider Binding -> Typed Provider Adapter -> Post-condition -> Receipt/Audit`

Callers never select a binary, argv, injected script, root shell, or jailbreak-specific path.

## Privilege model

`RootTools.app` remains a mobile process (UID 501). It is the owner UI and a presentation/control client, not the privileged implementation.

`roottools-execd` is the privileged execution plane managed by the jailbreak bootstrap. It owns:

- capability and risk policy;
- provider discovery and binding;
- semantic dispatch;
- post-condition verification;
- audit, idempotency, execution revision, and deferred jobs.

This separation allows an App installed through a normal or persistent installer to use privileged functionality without making the App itself an unrestricted root shell.

## Provider Registry

The provider registry is the authoritative map of implementation mechanisms. Each provider declares:

- stable provider ID;
- domain (`control`, `native`, `jailbreak`, `package`, `runtime`, `transport`, `ui`, `permission`);
- implementation family;
- priority;
- headless support;
- unlock requirement;
- whether it survives RootTools UI exit;
- a bounded availability probe.

The v0.5 registry includes:

- `roottools.execd`
- `ios.darwin`
- `jailbreak.dopamine`
- `bootstrap.procursus`
- `package.sileo`
- `package.trollstore`
- `runtime.frida`
- `runtime.ellekit`
- `transport.openssh`
- `ui.springboard`
- `ui.zxtouch`
- `permission.tcc`

`GET /v1/providers/catalog` is the authoritative runtime truth. The older `/v1/runtime/catalog` remains a compatibility summary and points callers to the provider catalog.

## Capability binding

Capabilities bind to provider IDs, not tools. Examples:

| Capability | Provider |
| --- | --- |
| `device.app.launch` | `ui.springboard` |
| `device.process.inspect` | `ios.darwin` |
| `device.permission.tcc` | `permission.tcc` |
| `device.ui.screen-info` | `ui.zxtouch` |
| `device.automation.queue-app-launch` | `ui.springboard` |

For an immediate operation, the router checks provider availability before calling the executor. A missing provider returns an auditable `provider_unavailable` result without executing the operation.

Deferred actions are allowed to persist while their eventual provider is temporarily unavailable; provider readiness is checked again when the worker attempts execution.

Every ActionReceipt and audit entry records `providerId`, so an operator can answer not just *what* semantic action ran but *which implementation family* performed it.

## Package providers

Package installation is intentionally separated from generic command execution.

`POST /v1/package/plan` accepts only a package format and resolves it to a typed provider plan:

- `.deb` -> `bootstrap.procursus`, with `package.sileo` as the interactive fallback;
- `.ipa` / `.tipa` -> `package.trollstore`.

The plan reports readiness and always declares owner confirmation. It also asserts:

- `rawShell=false`
- `arbitraryExecutable=false`
- `typedPackageOnly=true`

v0.5 does **not** expose an arbitrary package-path execution endpoint. A future package mutation capability must stage a bounded package in a RootTools-owned scope, validate its type/metadata, require R2 owner confirmation, invoke only the selected fixed provider adapter, and verify the installed package/application as its post-condition.

The TrollStore provider is deliberately an **application-install provider**, not the RootTools daemon host. The current upstream TrollStore root helper accepts a bounded install command and must run as root, while TrollStore's own documented limitations do not make it a general launch-daemon platform. RootTools therefore keeps the long-lived UID 0 service under the jailbreak/bootstrap provider and uses TrollStore only behind typed IPA/TIPA package operations.

For the future `trollstorehelper` adapter, RootTools will pin a specific helper contract/version rather than discover or forward arbitrary arguments. The current upstream helper parses `install`, optional installation-mode/force flags, and the IPA path; callers will never see or control that argv surface directly.

## Provider vs capability vs adapter

- **Capability**: user/Agent intent, e.g. `device.app.launch`.
- **Provider**: implementation family selected by RootTools, e.g. `ui.springboard`.
- **Adapter**: the concrete bounded implementation inside the daemon, e.g. fixed `uiopen --bundleid` invocation.

The Agent sees only capabilities. Provider selection is observable but not caller-controlled.

## Replacement model

Changing jailbreak or installer should not change Device Ops:

`Dopamine -> another jailbreak` means adding/rebinding a jailbreak provider.

`TrollStore -> another persistent installer` means adding/rebinding a package provider.

`ZXTouch -> another input backend` means replacing a UI provider/adapter.

This is the core compatibility contract for future device families and iOS versions.

## Security invariants

- no raw privileged shell in the product protocol;
- no caller-supplied executable or argv;
- provider binding is daemon-owned;
- provider availability is checked before immediate execution;
- policy/risk remains capability-owned and cannot be weakened by a provider;
- R2 confirmation remains daemon-enforced;
- R3 remains hard denied;
- success still requires post-condition verification;
- receipts/audit include both `capabilityId` and `providerId`.
