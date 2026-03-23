# UKCP C++ Windows Performance Design

**Goal:** Add a Windows-local performance benchmark for the C++ UKCP server, capture throughput and latency baselines, and land a small set of low-risk network-path optimizations.

## Scope

- Target the current C++ server implementation under `cpp/ukcp_server/`.
- Benchmark the Windows code path first.
- Measure both:
  - mixed ingress load from clients (`KCP + repeated UDP` on one port)
  - server egress load (`KCP echo` and `SendToAll`)
- Keep optimizations simple and production-friendly.

## Chosen Design

### Benchmark shape

- Add a dedicated benchmark executable under `cpp/ukcp_server/bench/windows_perf/`.
- Run the server in-process to remove external orchestration noise.
- Create multiple synthetic clients using the existing UKCP wire protocol:
  - each client authenticates by KCP
  - each loop sends one KCP packet
  - each loop sends `udp_repeat` raw UDP packets
  - server echoes everything back by KCP
- Add a second broadcast phase that uses `SendToAll` so the Windows send path is also measured directly.

### Metrics

- Wall-clock duration
- Sent/received packet counts
- Sent/received byte counts
- Approximate PPS and BPS
- KCP echo RTT percentiles: `p50`, `p95`, `p99`, `max`
- Active session count

### Server instrumentation

- Add a small stats snapshot API on `Server`.
- Maintain atomic counters in the server implementation for:
  - received packets and bytes
  - received KCP packets and UDP packets
  - sent KCP packets and bytes
- Keep this read-only and cheap so it can stay in production code.

### Planned optimizations

- Replace per-send wrapped-packet heap allocation with a reusable thread-local packet buffer on the KCP output path.
- Reduce `SendToMultiSess` duplicate filtering cost by switching to threshold-based dedupe:
  - tiny lists keep the current simple linear path
  - larger lists use `std::unordered_set`
- Reuse session pointer scratch storage in `SendToAll` and the update loop to avoid repeated short-lived allocations.
- Leave the Windows socket model as `select`; no IOCP in this pass.

## Non-Goals

- No reactor/threading redesign
- No IOCP or RIO
- No protocol changes
- No gameplay semantics
- No cross-machine benchmark harness

## Validation

- Add deterministic tests for the new server stats snapshot behavior.
- Keep existing reconnect and echo tests passing.
- Run the new Windows benchmark executable and record a baseline before and after the optimizations.
- Re-run C# interop and WSL/Linux tests to ensure no regression in the shared code path.
