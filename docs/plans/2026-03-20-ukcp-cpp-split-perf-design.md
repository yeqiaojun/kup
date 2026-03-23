# UKCP C++ Split-Process Performance Design

**Goal:** Build a split-process performance harness for the C++ UKCP server so server CPU can be measured independently from synthetic clients under WSL.

## Scope

- Keep the existing in-process benchmark intact.
- Add a dedicated server benchmark binary and a dedicated client benchmark binary.
- Target the concrete scenario requested by the user:
  - `5000` players
  - `4` client worker threads
  - uplink `8` packets per second per player
  - downlink `15` packets per second per player
- Measure only server-process CPU, plus delivery ratios observed by the client process.

## Chosen Design

### Processes

- `ukcp_perf_server`
  - starts the current UKCP server on one UDP port
  - authenticates sessions by KCP
  - counts uplink KCP packets
  - drives fixed-rate downlink broadcast using `SendToAll`
  - prints compact final stats
- `ukcp_perf_client`
  - creates `N` logical players
  - spreads them across `M` worker threads
  - authenticates each player
  - sends fixed-rate uplink KCP messages
  - receives fixed-rate downlink KCP messages
  - prints expected, sent, received, and delivery ratios

### CPU measurement

- Run `ukcp_perf_server` under `/usr/bin/time`.
- Start `ukcp_perf_client` as a separate process after the server is listening.
- Stop the server after the test window and collect:
  - elapsed time
  - user time
  - system time
  - CPU percentage
  - RSS
- Treat those numbers as authoritative for server CPU because the client load is in a different process.

### Coordination model

- Keep coordination simple:
  - server starts and binds the port
  - client starts after a short delay
  - server runs for a configured duration plus drain window, then exits cleanly
- No controller daemon or RPC layer.

### Metrics

- Server side:
  - received packets and bytes
  - sent packets and bytes
  - received uplink logical messages
  - sent downlink ticks
  - active session count
  - process CPU and RSS from `/usr/bin/time`
- Client side:
  - expected uplink messages
  - actually sent uplink messages
  - expected downlink messages
  - actually received downlink messages
  - delivery percentages

## Non-Goals

- No protocol changes
- No gameplay logic
- No IOCP or multi-reactor redesign
- No distributed benchmark controller
- No JSON/report pipeline in this pass

## Validation

- Keep existing C++ tests passing.
- Add a small smoke integration test that launches the split server/client pair on Windows and verifies the client exits successfully.
- Build and smoke-test both binaries on WSL.
- Run the requested WSL scenario and report server CPU separately from client delivery results.
