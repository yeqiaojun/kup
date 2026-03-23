# UKCP C++ Server Slimming Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Refactor the C++ UKCP server into smaller, more direct source files without changing its public behavior or transport model.

**Architecture:** Keep one shared internal state header and split implementation by responsibility: lifecycle, receive, send, update, and poller. Preserve the current fixed-port listener, connected UDP child sockets, Linux `io_uring` poller, Windows fallback, and all public server/session send APIs.

**Tech Stack:** C++23, ikcp, UDP sockets, Linux `io_uring`, WinSock `select`, CMake, local test framework.

---

### Task 1: Lock the current behavior with slimming-focused regression tests

**Files:**
- Modify: `cpp/ukcp_server/tests/server_test.cpp`
- Modify: `cpp/ukcp_server/tests/test_support.hpp`
- Test: `cpp/ukcp_server/tests/server_test.cpp`

**Step 1: Write the failing test**

- Add or keep focused tests that assert:
  - the poller reports readable listener sockets
  - auth reaches the handler
  - raw UDP send works from `Session`
  - raw UDP send works from `Server`

**Step 2: Run test to verify it fails when behavior breaks**

Run: `wsl bash -lc "cd /mnt/d/tkcp/ukcp/cpp/ukcp_server/build-wsl-cmake && timeout 30s ./ukcp_server_tests"`

Expected: red if the refactor breaks receive/send flow.

**Step 3: Keep only minimal test helpers**

- Do not add a new test framework.
- Keep helpers direct and transport-specific.

**Step 4: Re-run and confirm the test set is stable**

Run: `wsl bash -lc "cd /mnt/d/tkcp/ukcp/cpp/ukcp_server/build-wsl-cmake && timeout 30s ./ukcp_server_tests"`

Expected: current behavior still green before structural changes continue.

### Task 2: Rename and simplify the poller implementation file

**Files:**
- Create: `cpp/ukcp_server/src/platform_poller.cpp`
- Delete: `cpp/ukcp_server/src/platform_epoll.cpp`
- Modify: `cpp/ukcp_server/src/platform_socket.hpp`
- Modify: `cpp/ukcp_server/CMakeLists.txt`
- Test: `cpp/ukcp_server/tests/server_test.cpp`

**Step 1: Keep the poller regression test as the driver**

- Use the readable-listener test as the red/green guard.

**Step 2: Run the specific poller test**

Run: `wsl bash -lc "cd /mnt/d/tkcp/ukcp/cpp/ukcp_server/build-wsl-cmake && timeout 8s env UKCP_TEST_FILTER=Poller_ReportsReadableUdpListener ./ukcp_server_tests"`

Expected: PASS before and after rename.

**Step 3: Move code without changing behavior**

- Rename the file to `platform_poller.cpp`.
- Keep Linux `io_uring` and Windows `select` in the same file.
- Remove naming tied to `epoll`.

**Step 4: Rebuild and rerun the focused poller test**

Run: `wsl bash -lc "cd /mnt/d/tkcp/ukcp/cpp/ukcp_server/build-wsl-cmake && cmake --build . -j2 --target ukcp_server_tests && timeout 8s env UKCP_TEST_FILTER=Poller_ReportsReadableUdpListener ./ukcp_server_tests"`

Expected: PASS.

### Task 3: Split server receive and update responsibilities out of `server.cpp`

**Files:**
- Create: `cpp/ukcp_server/src/server_receive.cpp`
- Create: `cpp/ukcp_server/src/server_update.cpp`
- Modify: `cpp/ukcp_server/src/server_internal.hpp`
- Modify: `cpp/ukcp_server/src/server.cpp`
- Test: `cpp/ukcp_server/tests/server_test.cpp`

**Step 1: Keep auth and reconnect tests as the driver**

- Use:
  - `Server_AuthCallbackReceivesFirstKcpMessage`
  - `Server_HardReconnectReplacesSession`
  - `Server_FastReconnectKeepsSessionOpen`

**Step 2: Run the focused tests**

Run: `wsl bash -lc "cd /mnt/d/tkcp/ukcp/cpp/ukcp_server/build-wsl-cmake && timeout 20s env UKCP_TEST_FILTER=Server_AuthCallbackReceivesFirstKcpMessage ./ukcp_server_tests"`

Run: `wsl bash -lc "cd /mnt/d/tkcp/ukcp/cpp/ukcp_server/build-wsl-cmake && timeout 20s env UKCP_TEST_FILTER=Server_HardReconnectReplacesSession ./ukcp_server_tests"`

Expected: PASS before split.

**Step 3: Move only receive/update logic**

- Put listener/session read loops and auth dispatch in `server_receive.cpp`.
- Put `ScheduleSessionUpdate` and scheduled KCP sweep logic in `server_update.cpp`.
- Keep the public `Server` API file thin.

**Step 4: Re-run the focused tests**

Run: the same two commands above.

Expected: PASS.

### Task 4: Split lifecycle and send logic, and remove duplicated send loops

**Files:**
- Create: `cpp/ukcp_server/src/server_lifecycle.cpp`
- Create: `cpp/ukcp_server/src/server_send.cpp`
- Modify: `cpp/ukcp_server/src/server_internal.hpp`
- Modify: `cpp/ukcp_server/src/server.cpp`
- Modify: `cpp/ukcp_server/src/session.cpp`
- Modify: `cpp/ukcp_server/include/ukcp/server.hpp`
- Modify: `cpp/ukcp_server/include/ukcp/session.hpp`
- Test: `cpp/ukcp_server/tests/server_test.cpp`

**Step 1: Keep raw UDP and stats tests as the driver**

- Use:
  - `Server_SessionCanSendRawUdp`
  - `Server_CanSendRawUdpToSess`
  - `Server_StatsTrackIngressAndSessions`

**Step 2: Run the focused tests**

Run: `wsl bash -lc "cd /mnt/d/tkcp/ukcp/cpp/ukcp_server/build-wsl-cmake && timeout 20s env UKCP_TEST_FILTER=Server_SessionCanSendRawUdp ./ukcp_server_tests"`

Run: `wsl bash -lc "cd /mnt/d/tkcp/ukcp/cpp/ukcp_server/build-wsl-cmake && timeout 20s env UKCP_TEST_FILTER=Server_CanSendRawUdpToSess ./ukcp_server_tests"`

Expected: PASS before the move.

**Step 3: Refactor the send path**

- Move lifecycle helpers into `server_lifecycle.cpp`.
- Move bulk send APIs and send loop dedupe into `server_send.cpp`.
- Replace duplicate packet wrapping code in `session.cpp` with one shared builder helper that handles KCP and raw UDP.

**Step 4: Re-run the focused tests**

Run: the same focused commands plus:

`wsl bash -lc "cd /mnt/d/tkcp/ukcp/cpp/ukcp_server/build-wsl-cmake && timeout 20s env UKCP_TEST_FILTER=Server_StatsTrackIngressAndSessions ./ukcp_server_tests"`

Expected: PASS.

### Task 5: Update build files and remove obsolete source references

**Files:**
- Modify: `cpp/ukcp_server/CMakeLists.txt`
- Delete: obsolete source entries that no longer exist
- Test: build system targets only

**Step 1: Update source list**

- Replace the old monolithic source references with the new split files.

**Step 2: Build the library and tests**

Run: `wsl bash -lc "cd /mnt/d/tkcp/ukcp/cpp/ukcp_server/build-wsl-cmake && cmake --build . -j2 --target ukcp_server_tests"`

Expected: build succeeds.

### Task 6: Full regression on WSL and Windows

**Files:**
- Modify: none unless verification finds regressions
- Test: existing suite and Windows build output

**Step 1: Run the full WSL test suite**

Run: `wsl bash -lc "cd /mnt/d/tkcp/ukcp/cpp/ukcp_server/build-wsl-cmake && timeout 30s ./ukcp_server_tests"`

Expected: all tests PASS.

**Step 2: Build the Windows test target**

Run: `& 'C:\Program Files\Microsoft Visual Studio\18\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' --build cpp/ukcp_server/build-vs2026-fullscan --config Debug --target ukcp_server_tests`

Expected: build succeeds.

**Step 3: Run the Windows test binary**

Run: `& 'D:\tkcp\ukcp\cpp\ukcp_server\build-vs2026-fullscan\Debug\ukcp_server_tests.exe'`

Expected: all tests PASS.
