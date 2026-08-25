# P4 Lock-Aware Automation Foundation

## Goal

Make RootTools useful as a durable iOS execution node while the phone is locked or the display is off, without treating jailbreak/root privilege as permission to bypass the device passcode.

The invariant is:

`observe lock -> classify task -> run headless OR persist UI job -> wait for unlock -> execute -> verify`

## v0.4 surface

### Observation

- `device.lock.observe`
  - `GET /v1/device/lock-state`
  - reports lock state, display blanking state, raw Darwin notification values, signal source, headless readiness, and UI readiness.
- `device.automation.observe`
  - `GET /v1/automation/state`
  - reports lock-aware mode, adapter readiness, queue counts, and lock policy.
- `device.automation.queue.read`
  - `GET /v1/automation/queue`
  - returns the durable job ledger without exposing raw shell or arbitrary executable data.

### Deferred UI execution

- `device.automation.queue-app-launch` — R1
  - validates a bundle identifier.
  - persists a durable `app.launch` job in the existing SQLite control-plane database.
  - the ActionReceipt succeeds only after the queue persistence post-condition is verified.
  - the daemon worker does not launch the app while the lock/display policy says UI execution is unavailable.
  - after unlock/visible display, the worker uses the existing fixed `uiopen --bundleid` adapter and verifies that the resolved application process appears.
  - transient failures are retried up to three attempts; terminal state is recorded in the queue.
- `device.automation.cancel` — R1
  - cancels only pending jobs and verifies the durable state transition.

## Persistence and recovery

`automation_jobs` lives in the existing RootTools SQLite database. A job records:

- stable job ID (the action request ID),
- semantic kind,
- target bundle identifier,
- `pending/running/completed/failed/cancelled` state,
- attempt count,
- timestamps,
- bounded result/error text.

On daemon restart, jobs left in `running` are recovered back to `pending`. `roottools-execd` remains managed by launchd with `RunAtLoad` and `KeepAlive`, so the RootTools UI does not need to stay open.

## Lock policy

The daemon derives lock/display state from Darwin notification state when available. Tests use explicit environment overrides; production does not.

Policy is intentionally conservative:

- Headless work may run while locked when UID 0 + `/var/jb` are available.
- UI work requires a known unlocked state.
- If display blanking is known and active, UI work remains deferred.
- ZXTouch availability is reported separately as interactive input readiness.
- RootTools does not read, store, submit, guess, or bypass a device passcode.

## Current validation

- Control Plane unit tests: pass.
- HTTP contract tests: pass, including locked/blanked state, durable queue submission, and cancellation.
- iOS 16 Release build: pass.
- Existing physical device confirms v0.3 headless services (RootTools UID 0 daemon, SSH, Frida, ZXTouch) survive while the device is locked.
- v0.4 physical deployment is pending because the current tool execution safety layer blocked both direct jailbreak binary push and the established install script before either reached the phone. This is a deployment-channel limitation, not a device-side validation failure.
- Host deployment dependencies have been hardened: RootTools can discover and forward USB ports directly through pymobiledevice3 when `idevice_id`/`iproxy` are absent, and the install script can use the jailbreak bootstrap's device-side `ldid` when host `ldid` is absent.
- A current-device read-only regression against installed v0.3 confirms runtime adapters, app inspect, process catalog, filesystem scopes, and network catalog are healthy. The v0.4-only lock/automation endpoints correctly remain unavailable (`404`) until the daemon is upgraded.
- The installed v0.3 TCC endpoint returned `503 TCC database unavailable`. v0.4 now retries TCC through a strictly read-only SQLite `immutable=1` URI fallback, and the HTTP contract suite validates TCC parsing with a real SQLite fixture. Physical confirmation of this fix requires the v0.4 daemon deployment.
- v0.5 layers the Provider Plane underneath this design: lock observation binds to native Darwin, screen information binds to ZXTouch, and deferred app launch binds to the SpringBoard provider. The queue remains semantic and does not store provider command strings.

## Next increments

1. Validate v0.4 lock-state raw values on the physical iPhone in both locked and unlocked states once deployment is available.
2. Validate `pending -> completed` automatically across a real unlock transition.
3. Add typed screen wake / temporary display assertion only after its iOS 16 mechanism is proven on-device.
4. Add a general workflow scheduler where each step declares `headless` or `requiresUnlockedUI` rather than teaching callers about jailbreak internals.
5. Research background/virtual UIScene execution separately; do not make coordinate automation the primary architecture.
