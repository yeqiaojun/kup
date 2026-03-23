# UKCP C++ Server Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Build a C++ server implementation that matches the current Go server behavior for single-port `UDP + KCP`, echo example, and basic tests.

**Architecture:** Use native sockets with a Linux `epoll` poller and Windows `select` fallback. Keep the runtime model close to the Go server: one UDP socket, per-session KCP state, pending-auth state, and the same reconnect and send semantics.

**Tech Stack:** C++26, native sockets, vendored `ikcp`, CMake

---

### Task 1: Create protocol and build skeleton

**Files:**
- Create: `cpp/ukcp_server/CMakeLists.txt`
- Create: `cpp/ukcp_server/include/ukcp/*.hpp`
- Create: `cpp/ukcp_server/src/*.cpp`
- Create: `cpp/ukcp_server/third_party/ikcp/ikcp.c`
- Create: `cpp/ukcp_server/third_party/ikcp/ikcp.h`

**Step 1: Write the failing protocol test**

- Add a small test executable for header encode/decode and expect the build to fail before the header implementation exists.

**Step 2: Run test to verify it fails**

Run: `cmake -S cpp/ukcp_server -B cpp/ukcp_server/build && cmake --build cpp/ukcp_server/build && ctest --test-dir cpp/ukcp_server/build`

**Step 3: Write minimal protocol/build implementation**

- Add protocol header struct and encode/decode.
- Add the base CMake targets and vendored `ikcp`.

**Step 4: Run test to verify it passes**

Run: same as Step 2.

### Task 2: Implement server/session/poller core

**Files:**
- Create: `cpp/ukcp_server/include/ukcp/config.hpp`
- Create: `cpp/ukcp_server/include/ukcp/handler.hpp`
- Create: `cpp/ukcp_server/include/ukcp/session.hpp`
- Create: `cpp/ukcp_server/include/ukcp/server.hpp`
- Create: `cpp/ukcp_server/src/config.cpp`
- Create: `cpp/ukcp_server/src/session.cpp`
- Create: `cpp/ukcp_server/src/server.cpp`
- Create: `cpp/ukcp_server/src/platform_epoll.cpp`
- Create: `cpp/ukcp_server/src/platform_select.cpp`

**Step 1: Write the failing reconnect/echo behavior tests**

- Add tests for auth, UDP drop before auth, hard reconnect replacement, and fast reconnect address migration.

**Step 2: Run tests to verify they fail**

Run: `ctest --test-dir cpp/ukcp_server/build --output-on-failure`

**Step 3: Write minimal implementation**

- Implement UDP socket loop.
- Implement session map, pending-auth map, KCP state, send APIs, and reconnect behavior.

**Step 4: Run tests to verify they pass**

Run: same as Step 2.

### Task 3: Add echo example and integration coverage

**Files:**
- Create: `cpp/ukcp_server/examples/echo/main.cpp`
- Create: `cpp/ukcp_server/tests/echo_test.cpp`
- Create: `cpp/ukcp_server/tests/interop_csharp_test.cpp`

**Step 1: Write the failing example/integration tests**

- Add an echo test against the C++ server.
- Add a C# interop test that starts the C++ echo server and reuses the existing C# console client.

**Step 2: Run tests to verify they fail**

Run: `ctest --test-dir cpp/ukcp_server/build --output-on-failure`

**Step 3: Write minimal implementation**

- Implement the echo example and process launcher-based integration test.

**Step 4: Run tests to verify they pass**

Run: same as Step 2.

### Task 4: Final verification

**Files:**
- Modify: `docs/plans/2026-03-19-ukcp-cpp-server-design.md` only if implementation meaning changes

**Step 1: Full verification**

Run: `cmake -S cpp/ukcp_server -B cpp/ukcp_server/build`
Run: `cmake --build cpp/ukcp_server/build`
Run: `ctest --test-dir cpp/ukcp_server/build --output-on-failure`
Run: `dotnet test csharp/UkcpSharp.slnx`
