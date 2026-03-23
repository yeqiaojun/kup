# UKCP C++ Connected UDP + io_uring Design

**Goal:** Rework the Linux C++ server so authenticated sessions move from the shared listener socket onto per-session connected UDP sockets, while keeping the existing fixed-port protocol and KCP/UDP semantics.

## Context

- Current Linux server uses one bound UDP socket plus `epoll`.
- All packets, including authenticated session traffic, enter through that one fd.
- KCP auth already happens on the first KCP packet.
- The user wants the fixed server port to stay unchanged.

## Chosen Approach

- Keep one fixed listener UDP socket bound to the configured server port.
- The listener socket only handles unauthenticated first KCP packets.
- After KCP auth succeeds, create one child UDP socket for that session:
  - `socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)`
  - `SO_REUSEADDR`
  - Linux: `SO_REUSEPORT`
  - `bind()` to the same local server address and port
  - `connect()` to the authenticated client endpoint
- After activation, all further traffic for that session uses the child socket:
  - inbound `recv()`
  - outbound `send()`
- Linux replaces `epoll` with a small `io_uring` reactor.
- Windows keeps a simple fallback path and does not attempt to mirror `io_uring`.

## Why This Approach

- It matches the required “virtual TCP-like UDP connection” model closely.
- It keeps the public protocol unchanged: same packet header, same single exposed port.
- Connected UDP removes repeated `sendto()` address work on the hot path.
- `io_uring` gives a cleaner multi-fd receive loop once per-session sockets exist.
- The design avoids heavy abstractions and keeps the existing server/session split intact.

## Rejected Alternatives

### 1. Keep one socket and only add `io_uring`

- Lowest code churn.
- Does not satisfy the required per-session connected UDP model.

### 2. Use connected UDP child sockets but keep `epoll`

- Smaller Linux refactor.
- Leaves another poller rewrite for later and duplicates effort.

### 3. Introduce worker sharding or a new transport abstraction first

- Could help future scaling.
- Too much structure for the immediate problem and increases migration risk.

## Data Flow

### Listener path

- Listener fd stays registered in the reactor.
- Only KCP packets with the connect/auth flag are accepted from unknown endpoints.
- UDP packets for unknown `sess_id` are dropped.
- If auth fails, pending state is destroyed and no child socket is created.

### Session path

- Auth success activates the session and creates a connected child fd.
- Session state stores that fd and keeps the authenticated remote endpoint for diagnostics.
- Session KCP input comes only from its child fd after activation.
- Session UDP ingress comes only from its child fd after activation.
- Server KCP output for that session goes through the child fd.

## Reconnect Semantics

- Existing session replacement behavior stays explicit and simple.
- A fresh auth on the same `sess_id` creates a new child socket and replaces the old session.
- The old session is closed, its child fd is unregistered, then closed.
- The short reconnect window logic stays for the existing KCP session flow where still applicable.
- UDP never opens a session by itself. A session must already exist.

## io_uring Model

- Linux adds a small reactor object dedicated to UDP receive readiness.
- One long-lived receive submission is kept armed for the listener fd.
- Each active session child fd also has one long-lived receive submission armed.
- Completion entries identify whether the packet came from:
  - the listener fd
  - a specific session fd
- Listener completions use `recvfrom`-style address capture.
- Session completions use connected `recv`.
- The initial version focuses on receive-side `io_uring`.
- Send path stays synchronous with `send()` / `sendto()` to keep the first cut smaller and easier to verify.

## Windows Fallback

- Windows keeps the current simpler polling model.
- Windows still supports:
  - fixed listener socket
  - per-session connected UDP child sockets
  - `select`-based receive loop
- No Linux-only assumptions leak into public APIs.

## Code Shape

- Extend `SessionImpl` with its connected child socket handle.
- Replace the single-socket poller assumptions with a reactor that can register and unregister session fds.
- Split socket helpers into:
  - listener socket open
  - connected child socket open
  - connected send / recv
  - unconnected sendto / recvfrom
- Keep server logic explicit inside `server.cpp`. Do not add a large transport layer.

## Testing

- Update unit/integration tests so auth still gates session creation.
- Add coverage that a session continues to echo after activation through its child socket.
- Keep reconnect and replacement behavior covered.
- Keep C# interop echo passing.
- Update the perf client default room-mode KCP send rate from `3/s` to `15/s`.

## Validation

- Build and run `ukcp_server_tests` on Windows.
- Build and run `ukcp_server_tests` in WSL/Linux.
- Run the WSL room benchmark with the new default `15` KCP packets per second.
- Report server-only CPU, RSS, and delivery numbers after the refactor.
