# UKCP Server Performance Design

**Goal:** Reduce server latency jitter and tail latency without changing protocol semantics or public handler APIs.

**Scope:** Optimize the existing single-port `UDP + KCP` server hot paths. Keep the current session model, wire format, and handler behavior.

## Targets

- Lower per-packet allocation pressure in the receive and send hot paths.
- Reduce extra scheduling delay between `Send`, `KCP.Update`, and packet flush.
- Keep CPU growth bounded to roughly 10-20%.

## Non-Goals

- No sharded event loop rewrite.
- No protocol size change.
- No handler API redesign.
- No gameplay/business changes.

## Chosen Approach

### 1. Buffer reuse in hot paths

- Add reusable scratch buffers for:
  - pending-auth KCP receive
  - session KCP receive
  - KCP downlink packet assembly
- Use `sync.Pool` for server-side packet assembly buffers so downlink send does not allocate `header + payload` every time.

### 2. More eager but bounded KCP flush

- When a session queues outbound KCP, trigger flush immediately instead of waiting for the next ticker edge.
- Keep the periodic ticker for retransmit/update progression.

### 3. Trim avoidable per-call allocations

- Avoid building temporary session lists for `SendToAll`.
- Avoid the temporary dedupe map in `SendToMultiSess` when the caller gives a short list.
- Keep data ownership safe; do not switch handlers to unsafe zero-copy semantics in this pass.

### 4. Add performance regression coverage

- Add focused Go benchmarks around:
  - session send path
  - server KCP uplink path
  - pending auth path
- Use `b.ReportAllocs()` so later changes can be judged against a baseline.

## Risk Notes

- Reusing buffers across handler callbacks would be faster, but unsafe because handler code may retain slices. This pass deliberately avoids that semantic change.
- Immediate flush can raise CPU slightly. That is acceptable within the agreed 10-20% budget.
