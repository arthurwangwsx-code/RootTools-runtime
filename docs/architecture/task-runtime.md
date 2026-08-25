# Device Task Runtime

## Role

The Command Gateway answers whether a command is accepted. The Task Runtime owns work that may outlive the HTTP request, the RootTools UI, the host connection, or the current lock state.

The seam is:

`Command Gateway -> Durable Task -> Executor -> Post-condition -> Task Result`

## Durable identity

Each task stores:

- `taskId` — derived from the idempotent command request ID;
- `capabilityId` — semantic authority that created the task;
- `kind` and typed target;
- authenticated `caller` / Principal identity;
- `requiresUI`;
- attempt count and timestamps;
- result or error.

No token or arbitrary executable is persisted.

## State machine

`queued -> waiting_for_unlock -> running -> completed`

Transient execution failure may become `retrying` and is capped by the executor. Terminal alternatives are `failed` and `cancelled`.

On daemon restart, an interrupted `running` task is returned to `queued` with an explicit recovery diagnostic. UI-required work becomes `waiting_for_unlock` instead of trying to bypass the passcode.

## Ownership

Owner UI may view/cancel all tasks. Agent-class callers see only tasks whose stored caller identity matches their authenticated identity. Non-owner cancellation likewise applies only to the caller's own task.

## v0.13 executor

The first concrete task executor is `app.launch`. It resolves the app executable, uses the fixed SpringBoard `uiopen` provider, observes the resulting process as the post-condition, retries a bounded number of times, and waits for an unlocked visible UI when necessary.

v0.14 adds `ui.tap`, `ui.type`, and `ui.swipe` task kinds. Before any queued task executes, RootTools re-evaluates the current global capability policy and the current Named Principal grant. Revoking a grant therefore stops work that was queued earlier but has not yet executed.

UI input differs from app launch retry semantics. Once an input sequence has started, an indeterminate failure is terminal rather than automatically retried because a repeated tap/swipe/text insertion could duplicate a side effect.

The legacy `automation_jobs` table/API is retained only as a migration mirror for older clients. New clients use `device.task.submit-app-launch`, `device.task.cancel`, and `/v1/tasks/catalog`.
