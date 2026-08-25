# Semantic UI Automation Runtime

## Interface

RootTools exposes semantic device operations rather than the underlying input-tool protocol:

- `device.ui.observe` — screen geometry, orientation, lock/display state and UI readiness;
- `device.ui.tap` — typed integer screen point;
- `device.ui.type` — bounded UTF-8 text insertion;
- `device.ui.swipe` — typed start/end points, duration and interpolation steps.

The current provider is `ui.zxtouch`, but callers do not know ZXTouch task numbers, socket framing, touch-event encoding, finger indices or keyboard subtasks.

## Task semantics

Tap/type/swipe are R1 commands that create durable UI-required tasks. When the device is locked or blanked they become `waiting_for_unlock`. The daemon never attempts to bypass the passcode.

At execution time RootTools checks:

1. the capability still exists and is globally enabled;
2. a Named Principal still has the exact capability grant;
3. UI readiness is true;
4. the fixed UI provider is available;
5. coordinates are inside the current screen geometry.

## ZXTouch adapter

The adapter implements only fixed operations verified against the existing device automation path:

- task 10 touch down/move/up on one TCP session;
- task 24 / keyboard insert-text subtask 1;
- task 25 device-info subtasks for size/orientation/scale.

No caller-controlled ZXTouch task ID or raw payload exists in the RootTools protocol.

## Verification boundary

v0.14 verifies provider delivery/acknowledgement and performs a fresh screen-adapter observation after touch sequences. That proves the typed input path remained healthy, not that a specific app element changed state.

The next P4 layer adds Accessibility/selector observation and screenshot/vision evidence so a higher-level action can become `observe -> act -> observe -> verify` against an app-state post-condition rather than coordinate delivery alone.
