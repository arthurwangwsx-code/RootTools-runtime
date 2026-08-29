# Remote Access Physical Qualification Runbook

This runbook proves the v0.23 RootTools Runtime candidate on a physical Dopamine/rootless device. It separates USB bootstrap, genuinely off-USB Tailnet execution and USB cleanup so transport evidence cannot be confused with privileged-runtime evidence.

## Safety boundary

The qualification tool creates two temporary Host Principals:

- the selected Principal receives only `device.status.observe`, `device.performance.observe` and `device.app.launch`;
- the non-selected Principal receives zero grants and exists only to prove that the listener rejects the wrong identity;
- Owner and legacy Agent tokens are also tested for remote rejection;
- no R2/R3 capability, raw shell, arbitrary path or arbitrary executable authority is granted.

Principal credentials and the evidence state are written under ignored `build/qualification/` paths with mode `0600`. The evidence JSON records request/audit IDs but never credential values.

## Phase 0: install and USB regression

Connect the reference iPhone by USB, unlock it, trust the Mac, ensure Dopamine is active, open/connect Tailscale, and keep the display visible. Then run:

```bash
python3 Scripts/verify-device.py --install --full
```

This must report the daemon version derived from `VERSION`, UID 0, rootless readiness and the complete typed Device Service regression before Remote Access preparation starts.

## Phase 1: prepare over USB

```bash
python3 Scripts/qualify-remote-access.py prepare
```

The command:

1. validates daemon version, UID 0 and rootless state;
2. creates selected and non-selected temporary Host Principals;
3. grants the selected Principal exactly two R0 capabilities and one reversible R1 app-launch capability;
4. starts a bounded Tailscale-only session;
5. verifies selected-Principal status/performance plus Owner, Agent and non-selected-Principal rejection;
6. writes `build/qualification/remote-access-state.json` and two mode-`0600` Principal credential files.

Do not delete those files before cleanup. If preparation fails after Principal creation, the tool makes a best-effort stop/revoke attempt and leaves local credential evidence intact for diagnosis.

## Phase 2: prove off-USB execution

Physically unplug the iPhone from USB while keeping it unlocked and Tailscale online. Then run:

```bash
python3 Scripts/qualify-remote-access.py verify
```

The command fails if any USB iPhone remains visible. Over the recorded Tailnet address it then proves:

- selected Principal status and performance R0 reads;
- Root Tools foreground launch as the authorized R1 action;
- rejection of Owner, legacy Agent and non-selected Principal credentials.

Successful evidence advances the state file to `remote-verified`.

## Phase 3: stop, expiry and revoke

Reconnect and unlock the same iPhone, then run:

```bash
python3 Scripts/qualify-remote-access.py cleanup --verify-expiry
```

The command proves that:

1. explicit Owner stop closes remote authority;
2. a minimum five-minute session expires and closes remote authority;
3. revoking the selected Principal closes an active session;
4. the non-selected temporary Principal is also revoked.

Expiry verification takes slightly over five minutes and prints progress every 30 seconds. The final evidence phase is `complete`. Local revoked credential files are intentionally retained for explicit, auditable cleanup rather than silently deleted.

## Acceptance

Physical Remote Access qualification is complete only when both commands below have passed for the same candidate and the evidence state says `complete`:

```bash
python3 Scripts/verify-device.py --install --full
python3 Scripts/qualify-remote-access.py cleanup --verify-expiry
```

Source tests, a successful Release build, USB-only calls, an online Tailscale peer, or a `prepared` evidence file are not substitutes for this physical acceptance.
