# UKCP Server Performance Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Reduce UKCP server latency jitter and hot-path allocations while preserving protocol and handler semantics.

**Architecture:** Keep the current session-per-goroutine model and optimize only the receive/send hot paths. Use reusable scratch buffers and a small shared packet pool, flush outbound KCP more eagerly, and add benchmarks so the changes are measurable.

**Tech Stack:** Go, `xtaci/kcp-go/v5`

---

### Task 1: Add baseline benchmarks

**Files:**
- Create: `server_bench_test.go`

**Step 1: Write benchmark coverage**

- Benchmark server KCP uplink.
- Benchmark session downlink send.
- Benchmark pending-auth KCP path.
- Call `b.ReportAllocs()`.

**Step 2: Run the benchmarks once**

Run: `go test -run ^$ -bench BenchmarkServer -benchmem ./...`

### Task 2: Reuse buffers in send/recv hot paths

**Files:**
- Modify: `server.go`
- Modify: `session.go`

**Step 1: Add reusable buffers**

- Add small reusable scratch buffers for pending auth receive and session KCP receive.
- Add a shared pool for outbound packet assembly buffers.

**Step 2: Use the buffers**

- Replace repeated `make([]byte, size)` in `handlePendingKCP`.
- Replace repeated `make([]byte, size)` in `drainKCP`.
- Replace repeated `make([]byte, len(header)+len(payload))` in KCP writes.

**Step 3: Verify behavior**

Run: `go test ./...`

### Task 3: Reduce avoidable scheduling delay

**Files:**
- Modify: `session.go`

**Step 1: Make outbound KCP flush immediate**

- After successful `kcp.Send`, call `Flush`/`Update` path immediately.
- Keep ticker-based progression for retransmit and ack timing.

**Step 2: Verify behavior**

Run: `go test ./...`

### Task 4: Trim multi-send overhead

**Files:**
- Modify: `server.go`

**Step 1: Remove avoidable temporary allocations**

- Special-case short `SendToMultiSess` input without building a map.
- Make `SendToAll` iterate directly under lock-safe snapshot logic with minimal allocation.

**Step 2: Verify behavior**

Run: `go test ./...`

### Task 5: Final verification

**Files:**
- Modify: `docs/plans/2026-03-19-ukcp-server-perf-design.md` only if implementation meaning changes

**Step 1: Full verification**

Run: `go test ./...`
Run: `dotnet test csharp/UkcpSharp.slnx`
Run: `go test -run ^$ -bench BenchmarkServer -benchmem ./...`
