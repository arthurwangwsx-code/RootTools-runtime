# Semantic Runtime Observation

## Goal

RootTools needs to understand runtime instrumentation health without exposing an instrumentation language to the App, Agent, or automation layer. v0.9 therefore adds read-only semantic facts for Frida and ElleKit.

The contract is:

`Caller -> R0 runtime capability -> roottools-execd -> fixed runtime observer -> structured facts`

It is deliberately **not**:

`Caller -> Frida JavaScript / arbitrary attach / ElleKit hook API`

## Frida

Capability: `device.runtime.frida.observe`

Endpoint: `GET /v1/runtime/frida`

Provider: `runtime.frida`

Facts include:

- provider state;
- loopback port `27042` protocol reachability;
- exact `frida-server` process presence, PID, UID, and command name;
- fixed candidate server executable paths;
- installed Frida package ID/version parsed from the rootless dpkg status database when available;
- explicit policy facts: `scriptExecutionExposed=false`, `arbitraryAttachExposed=false`.

No request field can carry a script, process target, module, memory address, or attach command.

## ElleKit

Capability: `device.runtime.ellekit.observe`

Endpoint: `GET /v1/runtime/ellekit`

Provider: `runtime.ellekit`

The observer checks only fixed rootless component locations:

- `/var/jb/usr/lib/libellekit.dylib`
- `/var/jb/usr/libexec/ellekit/loader`
- `/var/jb/usr/lib/ellekit/libinjector.dylib`
- `/var/jb/usr/lib/ellekit/pspawn.dylib`
- `/var/jb/usr/lib/ellekit/MobileSafety.dylib`
- `/var/jb/usr/lib/TweakInject`

It also reads the installed ElleKit package/version from dpkg status when available and explicitly returns `rawHookAPIExposed=false` and `arbitraryInjectionExposed=false`.

## Why package metadata is read directly

Observation should remain headless and should not need to spawn package-management commands merely to display runtime health. The observer parses the fixed Procursus dpkg status file read-only. Tests can override that path with `ROOTTOOLS_DPKG_STATUS` so version parsing is deterministic without mutating the device.

## Future runtime actions

Any future instrumentation mutation must be introduced as a narrow semantic capability with its own target validation, risk level, owner policy, audit, timeout, and post-condition. Adding this observer does not pre-authorize arbitrary Frida scripts, arbitrary process attach, generic method hooks, or caller-provided dylib injection.
