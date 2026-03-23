# UKCP C++ Platform Split Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Split the ukcp C++ server networking layer into separate Linux and Windows implementations without changing public behavior.

**Architecture:** Keep protocol, session, and server logic shared. Move socket and poller implementation details into platform-specific translation units selected by CMake. Linux remains `io_uring` + connected UDP child sockets. Windows remains simple UDP + `select`.

**Tech Stack:** C++23, CMake, Winsock, Linux sockets, `io_uring`, existing ukcp test suite.

---

### Task 1: Split platform file layout

**Files:**
- Modify: `cpp/ukcp_server/src/platform_socket.hpp`
- Create: `cpp/ukcp_server/src/platform_socket_common.cpp`
- Create: `cpp/ukcp_server/src/platform_socket_linux.cpp`
- Create: `cpp/ukcp_server/src/platform_socket_windows.cpp`
- Create: `cpp/ukcp_server/src/platform_poller_linux.cpp`
- Create: `cpp/ukcp_server/src/platform_poller_windows.cpp`
- Delete: `cpp/ukcp_server/src/platform_socket.cpp`
- Delete: `cpp/ukcp_server/src/platform_poller.cpp`
- Delete: `cpp/ukcp_server/src/platform_select.cpp`

**Step 1:** Keep only shared declarations in the header.

**Step 2:** Move `Endpoint`, `ParseListenAddress`, and `NowMs` shared code into `platform_socket_common.cpp`.

**Step 3:** Move Linux socket and `io_uring` poller logic into Linux-specific files.

**Step 4:** Move Windows socket and `select` poller logic into Windows-specific files.

### Task 2: Select source files by platform

**Files:**
- Modify: `cpp/ukcp_server/CMakeLists.txt`

**Step 1:** Build a small shared source list.

**Step 2:** Append Linux-only or Windows-only platform files through CMake platform checks.

**Step 3:** Keep public target names unchanged.

### Task 3: Verify no behavior regression

**Files:**
- Use: `cpp/ukcp_server/tests/server_test.cpp`
- Use: `cpp/ukcp_server/tests/echo_test.cpp`

**Step 1:** Build on Windows.

**Step 2:** Run `ukcp_server_tests` on Windows.

**Step 3:** Build in WSL.

**Step 4:** Run `ukcp_server_tests` in WSL.
