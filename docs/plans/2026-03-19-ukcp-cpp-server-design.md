# UKCP C++ Server Design

**Goal:** Build a C++ server implementation that matches the current Go server behavior for single-port `UDP + KCP`, echo example, and basic test coverage.

## Scope

- Implement server-side only.
- Keep wire protocol fully aligned with the Go implementation:
  - 12-byte header
  - `msgtype(1) + flags(1) + bodylen(2) + sessid(4) + packetseq(4)`
  - `FlagConnect` semantics
- Keep reconnect behavior aligned:
  - fast reconnect by reusing existing KCP state and changing remote address
  - hard reconnect by `Connect`-flagged auth replacing the old session
- Provide:
  - reusable server library
  - echo example
  - tests

## Chosen Implementation

### Runtime model

- Use native sockets.
- Linux primary implementation uses `epoll`.
- Windows compatibility implementation uses `select`.
- Keep the architecture close to the Go version:
  - one server receive loop on one UDP socket
  - per-session state object
  - handler callbacks matching the Go semantics
- Use `ikcp` for KCP state handling.

### Directory layout

- `cpp/ukcp_server/CMakeLists.txt`
- `cpp/ukcp_server/src/`
  - protocol
  - config
  - handler
  - session
  - server
  - platform poller
- `cpp/ukcp_server/examples/echo/`
- `cpp/ukcp_server/tests/`

### Public behavior

- `Auth`
- `OnSessionOpen`
- `OnUDP`
- `OnKCP`
- `OnSessionClose`
- `SendToSess`
- `SendToMultiSess`
- `SendToAll`

### Dependencies

- native sockets
- vendored `ikcp.c/.h`
- no heavy framework requirement for tests; keep tests simple and repo-local

## Non-Goals

- No C++ client in this pass
- No sharded multi-thread reactor
- No protocol redesign
- No FEC or encryption

## Testing

- Protocol header tests
- Session/reconnect behavior tests
- Echo example test
- Cross-language integration by running the C++ echo server against the existing C# client
