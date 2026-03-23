# Active KCP Scheduling And Quiescence Drain Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Reduce room benchmark loss caused by deferred KCP flushes and fixed benchmark drain cutoffs.

**Architecture:** Replace the server's global `ikcp_update` sweep with per-session active scheduling keyed by next due time. Replace fixed post-run sleeps in the benchmark with quiescence-based drain checks so late in-flight KCP traffic is not counted as loss.

**Tech Stack:** C++23, ikcp, existing `ukcp_server` test harness and split-process perf tools.

---

### Task 1: Add observable tests

**Files:**
- Modify: `cpp/ukcp_server/tests/server_test.cpp`
- Create: `cpp/ukcp_server/tests/quiescence_test.cpp`
- Modify: `cpp/ukcp_server/CMakeLists.txt`

**Step 1: Write the failing tests**

- Add a server test that sets a long `update_interval` and verifies echoed KCP data is flushed in under that interval.
- Add unit tests for a quiescence drain helper that resets only when progress advances.

**Step 2: Run tests to verify they fail**

Run: `ukcp_server_tests.exe` filtered to the new tests.
Expected: build or runtime failure before implementation exists.

### Task 2: Implement active KCP scheduling

**Files:**
- Modify: `cpp/ukcp_server/src/server_internal.hpp`
- Modify: `cpp/ukcp_server/src/server.cpp`
- Modify: `cpp/ukcp_server/src/session.cpp`

**Step 1: Add scheduling state**

- Add per-session scheduled due time and schedule token.
- Add a server-side priority queue plus wake condition variable.

**Step 2: Replace full sweep**

- Wake the update thread only for due active sessions.
- Re-schedule sessions based on `ikcp_check` when `ikcp_waitsnd` stays non-zero.
- Schedule immediate flush after `Session::Send`.

### Task 3: Implement quiescence-based benchmark drain

**Files:**
- Create: `cpp/ukcp_server/src/quiescence.hpp`
- Modify: `cpp/ukcp_server/bench/perf_server/main.cpp`
- Modify: `cpp/ukcp_server/bench/perf_client/main.cpp`

**Step 1: Add helper**

- Implement a small header-only helper that marks completion after a quiet period with no counter progress.

**Step 2: Use helper in benchmarks**

- Server drain tracks outbound `sent_kcp_packets`.
- Client room drain tracks aggregate received room frames.

### Task 4: Verify

**Files:**
- None

**Step 1: Run test suite**

Run: `ukcp_server_tests.exe`
Expected: all tests pass.

**Step 2: Run split perf smoke**

Run: `ukcp_server_tests.exe` with `UKCP_TEST_FILTER=SplitPerf`.
Expected: fixed and room smoke both pass.
