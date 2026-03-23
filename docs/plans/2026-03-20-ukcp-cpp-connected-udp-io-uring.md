# UKCP C++ Connected UDP + io_uring Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Move authenticated Linux sessions onto per-session connected UDP sockets, replace Linux `epoll` with `io_uring`, keep Windows fallback support, and raise the room benchmark default KCP rate to `15/s`.

**Architecture:** The fixed listener UDP socket remains the auth entrypoint. After auth succeeds, the server allocates a connected child UDP socket per session, registers it with the platform reactor, and routes all established traffic through that fd. Linux uses a small `io_uring` receive reactor; Windows keeps a simpler fallback.

**Tech Stack:** C++23, POSIX UDP sockets, `io_uring`, WinSock `select`, ikcp, CMake, existing local test framework.

---

### Task 1: Add failing coverage for connected-session socket flow

**Files:**
- Modify: `cpp/ukcp_server/tests/server_test.cpp`
- Modify: `cpp/ukcp_server/tests/test_support.hpp`
- Test: `cpp/ukcp_server/tests/server_test.cpp`

**Step 1: Write the failing test**

- Add a test that authenticates, sends KCP and UDP, reconnects with the same `sess_id`, then verifies the replacement session still receives packets and the old one is closed.
- Extend test helpers only as needed to observe reconnect and echo behavior.

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir cpp/ukcp_server/build-wsl-cmake --output-on-failure -R ukcp_server_tests`

Expected: existing suite fails or new assertions fail because the server still uses the old single-fd model.

**Step 3: Write minimal implementation hooks**

- No production implementation yet.
- Only keep the test/helper additions needed to express the expected behavior.

**Step 4: Run test to verify the failure is stable**

Run: `ctest --test-dir cpp/ukcp_server/build-wsl-cmake --output-on-failure -R ukcp_server_tests`

Expected: same failing assertions, no flaky behavior.

### Task 2: Refactor socket helpers for listener vs connected session sockets

**Files:**
- Modify: `cpp/ukcp_server/src/platform_socket.hpp`
- Modify: `cpp/ukcp_server/src/platform_socket.cpp`
- Test: `cpp/ukcp_server/tests/server_test.cpp`

**Step 1: Write the failing test**

- Keep the Task 1 tests as the driver.

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir cpp/ukcp_server/build-wsl-cmake --output-on-failure -R ukcp_server_tests`

Expected: FAIL.

**Step 3: Write minimal implementation**

- Add helper functions for:
  - listener socket open
  - connected child socket open bound to the same local port
  - connected `send`
  - connected `recv`
  - listener `recvfrom`
- Preserve Windows compatibility.

**Step 4: Run tests**

Run: `cmake --build cpp/ukcp_server/build-wsl-cmake -j`

Expected: build succeeds even though behavior tests may still fail.

### Task 3: Replace Linux epoll poller with a small io_uring reactor

**Files:**
- Modify: `cpp/ukcp_server/src/platform_socket.hpp`
- Modify: `cpp/ukcp_server/src/platform_socket.cpp`
- Replace: `cpp/ukcp_server/src/platform_epoll.cpp`
- Modify: `cpp/ukcp_server/CMakeLists.txt`
- Test: `cpp/ukcp_server/tests/server_test.cpp`

**Step 1: Write the failing test**

- Reuse Task 1 failures.

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir cpp/ukcp_server/build-wsl-cmake --output-on-failure -R ukcp_server_tests`

Expected: FAIL.

**Step 3: Write minimal implementation**

- Add a reactor that can:
  - open on the listener fd
  - register session fds
  - unregister session fds
  - return receive completions with enough identity to dispatch correctly
- Linux implementation uses `io_uring`.
- Windows fallback keeps the simpler select-based path.

**Step 4: Run tests**

Run: `cmake --build cpp/ukcp_server/build-wsl-cmake -j`

Expected: build succeeds.

### Task 4: Move session activation and steady-state traffic onto child sockets

**Files:**
- Modify: `cpp/ukcp_server/src/server_internal.hpp`
- Modify: `cpp/ukcp_server/src/server.cpp`
- Modify: `cpp/ukcp_server/src/session.cpp`
- Test: `cpp/ukcp_server/tests/server_test.cpp`
- Test: `cpp/ukcp_server/tests/echo_test.cpp`

**Step 1: Write the failing test**

- Reuse the session/reconnect tests from earlier tasks.

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir cpp/ukcp_server/build-wsl-cmake --output-on-failure -R ukcp_server_tests`

Expected: FAIL.

**Step 3: Write minimal implementation**

- Give each `SessionImpl` ownership of its connected child fd.
- On auth success:
  - create child socket
  - register it with the reactor
  - attach it to the session
- On session close/replacement/server shutdown:
  - unregister child fd
  - close child fd
- Route KCP output through the child fd.
- Drop unauthenticated UDP as before.

**Step 4: Run tests**

Run: `ctest --test-dir cpp/ukcp_server/build-wsl-cmake --output-on-failure -R ukcp_server_tests`

Expected: PASS for updated server tests.

### Task 5: Raise benchmark defaults and keep interop green

**Files:**
- Modify: `cpp/ukcp_server/bench/perf_client/main.cpp`
- Modify: `cpp/ukcp_server/tests/interop_csharp_test.cpp`
- Test: `cpp/ukcp_server/tests/interop_csharp_test.cpp`

**Step 1: Write the failing expectation**

- Update default room-mode KCP rate expectation from `3` to `15`.

**Step 2: Run relevant tests**

Run: `ctest --test-dir cpp/ukcp_server/build-wsl-cmake --output-on-failure -R "ukcp_server_tests"`

Expected: any default-dependent expectations fail until the benchmark default is updated.

**Step 3: Write minimal implementation**

- Change `kcp_pps` default to `15`.
- Keep UDP default unchanged.
- Adjust any output parsing/tests that assume the old rate.

**Step 4: Run tests**

Run: `ctest --test-dir cpp/ukcp_server/build-wsl-cmake --output-on-failure -R "ukcp_server_tests"`

Expected: PASS.

### Task 6: Verify builds and run the requested benchmark

**Files:**
- Modify: none unless verification exposes defects
- Test: `cpp/ukcp_server/build-vs2026-fullscan`
- Test: `cpp/ukcp_server/build-wsl-cmake`

**Step 1: Build Windows**

Run: `cmake --build cpp/ukcp_server/build-vs2026-fullscan --config Debug`

Expected: build succeeds.

**Step 2: Run Windows tests**

Run: `ctest --test-dir cpp/ukcp_server/build-vs2026-fullscan -C Debug --output-on-failure`

Expected: PASS.

**Step 3: Build WSL/Linux**

Run: `wsl bash -lc "cd /mnt/d/tkcp/ukcp/cpp/ukcp_server/build-wsl-cmake && cmake --build . -j"`

Expected: build succeeds.

**Step 4: Run WSL/Linux tests**

Run: `wsl bash -lc "cd /mnt/d/tkcp/ukcp/cpp/ukcp_server/build-wsl-cmake && ctest --output-on-failure"`

Expected: PASS.

**Step 5: Run WSL room benchmark**

Run: `wsl bash -lc "cd /mnt/d/tkcp/ukcp/cpp/ukcp_server && ./bench/run_perf_case.sh room 5000 60"`

Expected: benchmark completes and prints server CPU, RSS, and delivery metrics.
