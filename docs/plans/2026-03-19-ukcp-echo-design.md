# UKCP Echo Example Design

## Goal

Add a runnable echo server example that demonstrates one-port UDP+KCP ingress and KCP-only egress.

## Behavior

- The example listens on one UDP port.
- Direct UDP packets are accepted through the shared 12-byte header.
- Duplicate direct UDP packets are suppressed by the core network layer.
- Every accepted payload is echoed back to the client through `Session.Send`, which means the reply always uses KCP.
- KCP uplink payloads are also echoed back through KCP.

## Structure

- `examples/echo/main.go`
  - parses listen address flag
  - starts `ukcp.Listen`
  - installs one small echo handler
- `examples/echo/echo_test.go`
  - starts the echo server on an ephemeral UDP port
  - creates a real client with `ukcp.Dial`
  - verifies repeated UDP uplink produces one KCP echo
  - verifies KCP uplink produces one KCP echo

