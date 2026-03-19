# UKCP Network Layer Design

## Goal

Build a single-port room-server network layer that accepts direct UDP packets and KCP packets on the same socket, routes both by `SessID`, and sends server payloads through KCP only.

## Architecture

- One server read loop owns socket ingress.
- Sessions are created lazily by `SessID`.
- Unknown `SessID` values are not activated by UDP.
- The first complete KCP message of an unknown `SessID` is passed to `Handler.Auth`.
- Only authenticated `SessID` values become active sessions.
- Each session has one goroutine that serializes its own state.
- Direct UDP packets are delivered to the session callback after duplicate suppression.
- KCP packets are fed into a per-session `kcp.KCP` state machine.
- Server downlink uses `Session.Send`, which always writes KCP segments wrapped by the shared 12-byte header.

## Public API

- `Listen(addr, handler, config)` for the common path
- `Serve(conn, handler, config)` when the caller owns the socket
- `Handler` callbacks:
  - `Auth(sessID, addr, payload) bool`
  - `OnSessionOpen(*Session)`
  - `OnUDP(*Session, packetSeq, payload)`
  - `OnKCP(*Session, payload)`
  - `OnSessionClose(*Session, err)`
- `Session.Send(payload)` for reliable downlink through KCP
- `Session.ID()` and `Session.RemoteAddr()`

## Processing Rules

- `MsgTypeUDP`:
  - if the session does not exist, drop the packet
  - update the session remote address
  - suppress duplicates with `SessID + PacketSeq`
  - deliver payload to `OnUDP`
- `MsgTypeKCP`:
  - if the session does not exist, feed payload into a pending KCP state
  - when the first complete KCP message arrives, call `Auth`
  - on `Auth=true`, promote the pending state into an active session
  - on `Auth=false`, discard the pending state
  - active sessions update the remote address, feed payload to `kcp.Input`, and deliver readable messages to `OnKCP`
- server outbound:
  - `Session.Send` queues data into the session goroutine
  - KCP output callback wraps raw KCP bytes with the shared header and writes to the socket

## Defaults

- KCP no-delay profile tuned for room traffic:
  - `NoDelay=1`
  - `Interval=10`
  - `Resend=2`
  - `NoCongestion=1`
  - `SendWindow=128`
  - `RecvWindow=128`
  - `MTU=1200`
- UDP duplicate suppression window: `512`

## Non-Goals

- FEC
- encryption
- fragmentation above KCP
- business protocol handling
- gateway features
