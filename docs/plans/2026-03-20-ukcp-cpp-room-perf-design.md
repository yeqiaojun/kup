# UKCP C++ Room Performance Design

**Goal:** Add a room-oriented split-process benchmark that models MOBA room traffic more realistically than the existing full-broadcast benchmark.

## Scenario

- Total players: `5000`
- Room size: `10`
- Concurrent rooms: `500`
- Test duration: `60s`
- Per-room downlink: `15 Hz`
- Per-player uplink:
  - random `3` KCP packets per second
  - random `5` UDP packets per second

## Chosen Approach

- Keep the current split-process benchmark binaries:
  - `ukcp_perf_server`
  - `ukcp_perf_client`
- Add a room-mode path to those binaries rather than creating a second benchmark stack.
- Keep the existing fixed-rate benchmark path intact for comparison and regression safety.

## Server Model

- Sessions are assigned to rooms deterministically from `sess_id`.
- The benchmark server builds:
  - `sess_id -> room_id`
  - `room_id -> session list`
- Downlink no longer uses `SendToAll`.
- Instead, the server runs a fixed `15 Hz` room tick loop and sends one KCP frame to the `10` sessions in each room.
- The payload remains generic and small. No gameplay protocol is introduced.

## Client Model

- The benchmark client still creates `5000` logical clients.
- Each logical client authenticates by KCP first, then starts uplink traffic.
- Every client uses a deterministic RNG stream so tests are reproducible.
- For each second of the `60s` window, the client schedules:
  - `3` random KCP sends
  - `5` random UDP sends
- Downlink receive remains KCP-only, matching the current server behavior.

## Metrics

### Server

- active sessions
- received KCP uplink messages
- received UDP uplink messages
- room ticks executed
- downlink packets sent
- server-only CPU and RSS from `/usr/bin/time`

### Client

- expected and sent KCP uplink messages
- expected and sent UDP uplink messages
- expected room-frame receives based on server actual room ticks
- actual received room-frame messages
- receive ratio against server actual output

## Non-Goals

- No gameplay serialization
- No new transport features
- No room migration logic
- No cross-room fanout
- No extra dependencies

## Validation

- Add a small room-mode smoke test on the split-process benchmark.
- Keep existing Windows C++ tests passing.
- Keep existing C# tests passing.
- Rebuild and run `ctest` in WSL.
- Run the `5000` player, `500` room, `60s` WSL scenario and report server-only CPU and delivery.
