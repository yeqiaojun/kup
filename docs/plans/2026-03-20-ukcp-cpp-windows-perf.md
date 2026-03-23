# UKCP C++ Windows Performance Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add a Windows-local benchmark for throughput and latency, expose lightweight server stats, and apply a small set of low-risk network-path optimizations.

**Architecture:** Extend the existing `cpp/ukcp_server` library with cheap atomic counters and a snapshot API, then add an in-process benchmark executable that drives mixed `KCP + UDP` traffic against the same single-port server. Optimize only the hot paths that currently allocate or do avoidable work on every send/broadcast loop.

**Tech Stack:** C++23, native sockets, vendored `ikcp`, CMake, existing repo-local test framework

---

### Task 1: Add failing tests for server stats and benchmark helpers

**Files:**
- Modify: `cpp/ukcp_server/include/ukcp/server.hpp`
- Modify: `cpp/ukcp_server/tests/server_test.cpp`
- Modify: `cpp/ukcp_server/tests/test_main.cpp`
- Create: `cpp/ukcp_server/tests/perf_stats_test.cpp`

**Step 1: Write the failing test**

- Add a test that authenticates a session, sends one KCP packet and repeated UDP packets, then asserts a `Server::Stats()` snapshot reports the expected received packet counts and active session count.
- Add a small test for RTT percentile/stat aggregation helper behavior if that helper lives outside the benchmark `main`.

**Step 2: Run test to verify it fails**

Run: `cpp/ukcp_server/build/ukcp_server_tests.exe 2>&1`
Expected: FAIL because `Server::Stats()` and/or the stats helper does not exist yet.

**Step 3: Write minimal implementation**

- Add `ServerStatsSnapshot` to the public API.
- Add only the fields needed by the benchmark.

**Step 4: Run test to verify it passes**

Run: `cpp/ukcp_server/build/ukcp_server_tests.exe 2>&1`
Expected: PASS for the new stats test and all existing tests.

### Task 2: Implement server-side counters and low-risk hot-path optimizations

**Files:**
- Modify: `cpp/ukcp_server/include/ukcp/server.hpp`
- Modify: `cpp/ukcp_server/src/server_internal.hpp`
- Modify: `cpp/ukcp_server/src/server.cpp`
- Modify: `cpp/ukcp_server/src/session.cpp`

**Step 1: Extend the failing test if needed**

- Tighten the stats assertions so they verify sent packet counts after echo/broadcast activity.

**Step 2: Run test to verify it fails**

Run: `cpp/ukcp_server/build/ukcp_server_tests.exe 2>&1`
Expected: FAIL until counters are incremented in the correct places.

**Step 3: Write minimal implementation**

- Add atomic counters to `ServerImpl`.
- Increment receive counters after a valid header is accepted.
- Increment send counters on successful wrapped KCP send.
- Add a reusable thread-local packet buffer for wrapped KCP output.
- Rework `SendToMultiSess` large-list dedupe to use `std::unordered_set`.
- Reuse scratch vectors in `SendToAll` and the update loop where it stays simple.

**Step 4: Run test to verify it passes**

Run: `cpp/ukcp_server/build/ukcp_server_tests.exe 2>&1`
Expected: PASS.

### Task 3: Add the Windows benchmark executable

**Files:**
- Modify: `cpp/ukcp_server/CMakeLists.txt`
- Create: `cpp/ukcp_server/bench/windows_perf/main.cpp`

**Step 1: Write the failing test or smoke path**

- Add the benchmark target to CMake and make the build fail until the source exists.

**Step 2: Run build to verify it fails**

Run: `cmake --build cpp/ukcp_server/build`
Expected: FAIL because the benchmark source file does not exist yet.

**Step 3: Write minimal implementation**

- Start the server in-process with an echo handler.
- Start configurable synthetic clients.
- Run:
  - mixed echo phase for RTT + ingress throughput
  - broadcast phase for `SendToAll` throughput
- Print a compact report with packets, bytes, PPS, BPS, and RTT percentiles.

**Step 4: Run build and smoke benchmark**

Run: `cmake --build cpp/ukcp_server/build`
Run: `cpp/ukcp_server/build/ukcp_windows_perf.exe --clients 64 --seconds 5`
Expected: build succeeds and benchmark prints a summary report.

### Task 4: Capture Windows baseline and verify no regressions

**Files:**
- Modify: `docs/plans/2026-03-20-ukcp-cpp-windows-perf-design.md` only if implementation meaning changes

**Step 1: Run full verification**

Run: `cpp/ukcp_server/build/ukcp_server_tests.exe 2>&1`
Run: `dotnet test csharp/UkcpSharp.slnx`
Run: `wsl.exe bash -lc "cd /mnt/d/tkcp/ukcp/cpp/ukcp_server/build-wsl-cmake && ctest --output-on-failure"`

**Step 2: Run the Windows benchmark and record the result**

Run: `cpp/ukcp_server/build/ukcp_windows_perf.exe --clients 64 --seconds 5`
Run: `cpp/ukcp_server/build/ukcp_windows_perf.exe --clients 256 --seconds 10`

**Step 3: Summarize**

- Report the measured throughput and RTT percentiles.
- Call out which optimizations were actually landed.
