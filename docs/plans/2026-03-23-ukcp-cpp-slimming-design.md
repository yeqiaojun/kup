# UKCP C++ Server Slimming Design

**Goal:** Slim down the C++ UKCP server implementation while keeping the current transport behavior, public API, fixed-port model, connected UDP session sockets, and Linux `io_uring` polling path.

## Why This Refactor

- `cpp/ukcp_server/src/server.cpp` is carrying too many responsibilities at once:
  - session lifecycle
  - auth and pending activation
  - listener/session receive dispatch
  - KCP scheduling
  - stats
  - bulk send APIs
- `cpp/ukcp_server/src/platform_epoll.cpp` still reflects an old name even though it now contains the unified Linux/Windows poller behavior.
- `cpp/ukcp_server/src/session.cpp` repeats packet wrapping logic for KCP and raw UDP sends.

The user wants a simple network framework, so the target is not “more architecture”. The target is smaller files, clearer ownership, and direct code.

## Chosen Approach

- Keep the existing public API shape:
  - `Session::Send`
  - `Session::SendRawUdp`
  - `Server::SendToSess`
  - `Server::SendToMultiSess`
  - `Server::SendToAll`
  - `Server::SendRawUdpToSess`
  - `Server::SendRawUdpToMultiSess`
  - `Server::SendRawUdpToAll`
- Keep one shared internal state header:
  - `cpp/ukcp_server/src/server_internal.hpp`
- Split implementation by responsibility into a few direct `.cpp` files:
  - `server.cpp`
  - `server_lifecycle.cpp`
  - `server_receive.cpp`
  - `server_send.cpp`
  - `server_update.cpp`
  - `platform_poller.cpp`
- Remove the misleading `platform_epoll.cpp` name.
- Keep the `Poller` abstraction small and flat.

## File Layout

### `server.cpp`

- Thin public entrypoints only.
- `Server` constructor, destructor, and small forwarding methods stay here.

### `server_lifecycle.cpp`

- Start/close logic
- session activation and replacement
- shared lifecycle cleanup helpers
- stats snapshot

### `server_receive.cpp`

- listener receive loop
- session receive loop
- auth and pending KCP handling
- packet decode and dispatch

### `server_send.cpp`

- bulk send helpers
- raw UDP send helpers
- shared packet wrapping helper used by session send paths
- `Server::Send*` implementations

### `server_update.cpp`

- `ScheduleSessionUpdate`
- scheduled KCP update sweep
- update thread loop

### `platform_poller.cpp`

- Linux `io_uring` readable-fd polling
- Windows `select` fallback
- no protocol logic
- no session logic

## Non-Goals

- No public protocol change
- No gameplay packet format change
- No move from connected UDP session sockets
- No switch from `io_uring` back to `epoll`
- No new abstraction layer for transports
- No send/recv `io_uring` implementation

## Explicit Simplifications

- Keep shared state in `server_internal.hpp` instead of introducing new classes.
- Use plain helper functions instead of strategy objects or inheritance.
- Keep Linux and Windows implementations together under one `Poller` API.
- Merge duplicated bulk-send loops with one direct helper instead of templates or generic policy layers.
- Merge KCP/raw UDP packet wrapping with one small packet builder helper driven by `MsgType`.

## Validation

- Keep the focused poller test.
- Keep auth, reconnect, raw UDP, echo, split perf smoke, and C# interop tests.
- Run the full WSL suite.
- Run the Windows suite from the VS build output.
