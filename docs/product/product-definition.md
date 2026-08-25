# RootTools Product Definition

## Positioning

RootTools is a resident privileged device runtime for a personally owned iPhone. It has three product roles at the same time:

1. **Device manager** — device health, applications, processes, packages, files, permissions, runtime providers and diagnostics.
2. **Command node** — a long-lived receiver for typed commands from the on-device UI, a trusted Mac host, AiBox, and future network Skills.
3. **Execution plane** — policy-controlled UID 0 execution, lock-aware automation, provider routing, post-condition verification, receipts, audit and recovery.

RootTools is deliberately **not** the reasoning Agent. AiBox or another Agent decides *what* should be done; RootTools decides whether the requested device action is allowed and *how* to execute it safely on the device.

The product invariant is:

`Caller -> Command Gateway -> Identity / Policy -> Capability Router -> Provider -> Post-condition -> Receipt / Audit`

The caller never receives a generic root shell, provider argv, Frida script executor, or unrestricted filesystem path.

## Supported callers

### RootTools owner UI

- local owner/admin identity;
- can inspect all device management surfaces;
- can approve R2 actions;
- can manage capability policy, credentials and recovery.

### Trusted Mac host

- Device Service / future `ios` CLI;
- USB/usbmux is the preferred local transport;
- Tailscale/SSH may be transport providers, not privilege interfaces;
- uses scoped Agent identity by default and Owner only for explicit administration.

### AiBox

- local iOS client adapter or host-proxied adapter;
- reuses AiBox `DeviceExecution*` scope, generation, revision and idempotency semantics;
- maps AiBox device operations to RootTools capabilities;
- never imports RootTools implementation models into AiBox feature packages.

### Network Skill / automation relay

- future outbound relay initiated by the UID 0 runtime;
- remote callers receive short-lived scoped grants rather than the owner token;
- no privileged listener is exposed directly to the public internet.

## Product domains

| Domain | User value | Runtime responsibility |
| --- | --- | --- |
| Overview | Is this device healthy and ready? | health, lock/headless state, provider summary |
| Device | Manage apps and system resources | app/process/package/files/network/runtime |
| Tasks | Know what the phone is doing | command queue, automation, schedules, receipts |
| Agents | Know who may control the device | caller identity, credentials, grants, AiBox/host/skill integrations |
| Settings | Control risk and maintenance | capabilities, providers, permissions, updater, diagnostics, audit |

## Product quality bar

RootTools must behave like infrastructure while looking like a first-party device-management product:

- stable bottom-tab information architecture;
- no developer-only grid as the primary navigation model;
- every destructive action explains risk and post-condition;
- current device truth is visible before actions are offered;
- empty/error/degraded states are explicit;
- the App may exit without stopping the privileged runtime;
- lock state, host disconnect and provider loss are first-class states;
- every command has an identity, lifecycle and receipt.

## Non-goals

- bypassing the device passcode;
- public-network root shell;
- arbitrary caller-provided executable or argv;
- arbitrary Frida/ElleKit scripts from Agent input;
- merging AiBox reasoning/session state into RootTools daemon state.
