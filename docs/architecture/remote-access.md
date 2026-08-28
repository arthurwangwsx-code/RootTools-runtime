# Remote Access

## Goal

Remote Access lets the Owner explicitly hand a device to a trusted remote Host while keeping transport separate from privilege.

The intended path is:

`Host -> Tailnet -> bounded Remote Session -> Named Host Principal -> Command Gateway -> capability policy`

It is not:

`Internet -> public UID 0 listener -> root shell`

## Session lifecycle

The Owner opens Root Tools, chooses an active Named `host` Principal and starts a session with a bounded lifetime. The daemon discovers a Tailscale IPv4 address in `100.64.0.0/10` and binds the remote listener only to that address.

The session ends when:

- the Owner stops it;
- its expiry time is reached;
- the selected Principal is revoked or becomes invalid;
- the required Tailscale address is no longer available.

## Authentication

The loopback listener keeps normal local Owner/Agent/Principal behavior. The Remote Access listener is stricter:

- Owner token: rejected;
- legacy Agent token: rejected;
- any Named Principal other than the selected Host: rejected;
- selected active Host Principal: authenticated, then still subject to exact capability grants.

Remote Access never turns Developer Mode into remote authority. Developer Mode remains a local Owner convenience.

## Authority

Starting a session does not add grants. The selected Host receives only the R0/R1 capabilities already granted to that Principal. R2 remains outside persistent remote grants.

Runtime conditions also remain in force: UI-required work waits for an unlocked visible and thermally eligible device.

## Network boundary

RootTools does not use `0.0.0.0` for Remote Access and does not treat ordinary LAN, hotel Wi-Fi or cellular-assigned addresses as approved remote bindings. Tailscale provides the private overlay; RootTools provides identity, policy and semantic execution.

If Tailscale is offline before a Remote Session can be started, there is no external path by which RootTools can make the unreachable phone reconnect itself. The Owner must restore one transport bootstrap (for example, open/connect Tailscale or reconnect USB) before remote control becomes possible.
