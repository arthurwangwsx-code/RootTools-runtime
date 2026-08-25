# RootTools Product Information Architecture

## Navigation model

The primary shell uses five bottom tabs. Tabs represent stable user intents rather than implementation modules.

### 1. Overview

Answers: **Is this device ready? Is anything wrong? What is it doing?**

- device identity and OS;
- root daemon/version;
- CPU/memory/free space;
- lock/headless/UI readiness;
- provider health;
- pending task count;
- security invariant summary;
- recent important failures/updates in a later iteration.

### 2. Device

Answers: **What is installed/running and how do I manage the phone?**

- Applications;
- Packages (DEB / IPA / TIPA);
- Processes;
- RootTools filesystem scopes;
- Network;
- Jailbreak/runtime facts.

### 3. Tasks

Answers: **What commands are running, waiting or finished?**

- controlled actions;
- lock-aware queue;
- future schedules/triggers/workflows;
- recent jobs;
- execution receipts and audit drill-down.

Tasks are the human-facing representation of the Command Gateway and Automation Runtime. They should not expose provider-specific commands.

### 4. Agents

Answers: **Who can control this device and what are they allowed to do?**

- RootTools owner UI;
- trusted Mac hosts;
- AiBox Agent integration;
- future network Skills / relay principals;
- credential lifecycle;
- principal-scoped grants and approval policy;
- online/offline/revoked status.

The Agent tab is about **control identity**, not Agent reasoning configuration. AiBox remains the home of personas, models, sessions, memory and tool planning.

### 5. Settings

Answers: **What privilege exists and how is this runtime maintained?**

- Capability policy;
- Provider catalog;
- TCC/permission facts;
- updater/recovery;
- diagnostics;
- audit export/retention;
- developer diagnostics behind an explicit advanced section.

## Visual direction

Use a native device-management visual language:

- system appearance instead of forced dark mode;
- grouped background and layered cards;
- large navigation titles per tab;
- small status pills only for state, not decoration;
- one accent color for interactive emphasis;
- destructive color only for destructive actions;
- compact monospaced text only for IDs/logs/receipts;
- avoid presenting the capability registry as the home screen.

## Interaction rules

1. Read state before offering a state-changing action.
2. Show whether an action can run headless or needs unlock.
3. For R2 actions, explain target, provider and post-condition in confirmation UI.
4. Long work becomes a Task and survives App navigation/exit.
5. App exit must never imply daemon/task termination.
6. Provider errors are translated to product-level recovery guidance.
7. Agent/Skill callers appear as principals with grants; tokens are not the UI model.

## Immediate implementation

The first product-shell increment replaces the single Dashboard grid as the primary navigation model with `Overview / Device / Tasks / Agents / Settings`. Existing feature views remain reusable destinations while deeper task/principal models are built behind them.
