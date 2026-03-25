# UKCP Client Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add a minimal single-connection `ukcp::Client` API for connect/auth/send/poll/receive/close.

**Architecture:** Reuse the existing packet header, socket, poller, and KCP configuration code. Keep the public client API explicit and synchronous: one connected UDP socket, no background thread, no exposed epoll interface, and user-driven `Poll()`.

**Tech Stack:** C++23, existing `ikcp`, existing socket/poller abstraction, current lightweight test framework.

---

### Task 1: Define the public client API

**Files:**
- Create: `cpp/ukcp_server/include/ukcp/client.hpp`
- Modify: `cpp/ukcp_server/CMakeLists.txt`

**Step 1: Write the failing test**

Add a test that includes `ukcp/client.hpp` and uses a `ukcp::Client` object to connect and send.

**Step 2: Run test to verify it fails**

Run: `cmake --build cpp/ukcp_server/build-codex --config Release --target ukcp_server_tests`
Expected: compile failure because `Client` does not exist yet.

**Step 3: Write minimal implementation**

Add the public `Client` declaration and wire its `.cpp` into the library build.

**Step 4: Run test to verify it passes**

Run the same build command and expect the compile error to move to missing behavior instead of missing types.

### Task 2: Implement single-connection client behavior

**Files:**
- Create: `cpp/ukcp_server/src/client.cpp`
- Create: `cpp/ukcp_server/tests/client_test.cpp`
- Modify: `cpp/ukcp_server/CMakeLists.txt`

**Step 1: Write the failing test**

Add tests for:
- connect/auth succeeds against the existing test server
- `SendKcp` reaches the server
- server `SendKcpToSess` can be received by the client after `Poll()`
- closing the client stops further sends

**Step 2: Run test to verify it fails**

Run: `cmake --build cpp/ukcp_server/build-codex --config Release --target ukcp_server_tests`
Then: `$env:UKCP_TEST_FILTER='Client_'; & 'D:/kup/cpp/ukcp_server/build-codex/Release/ukcp_server_tests.exe'`
Expected: compile/runtime failure because the behavior is not implemented yet.

**Step 3: Write minimal implementation**

Use one connected UDP socket, one KCP control block, and explicit `Poll()`/receive queues. Do not add threads or callback layers.

**Step 4: Run test to verify it passes**

Run the same filtered client tests and expect all `Client_` tests to pass.

### Task 3: Run regression verification

**Files:**
- Verify only

**Step 1: Run focused regression**

Run: `$env:UKCP_TEST_FILTER='Server_|Client_|Echo_'; & 'D:/kup/cpp/ukcp_server/build-codex/Release/ukcp_server_tests.exe'`

**Step 2: Check result**

Expected: all matching tests pass.
