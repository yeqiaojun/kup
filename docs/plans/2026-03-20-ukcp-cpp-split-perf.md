# UKCP C++ Split-Process Performance Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add split-process performance server/client binaries so the UKCP server can be load-tested with server-only CPU measurement under WSL.

**Architecture:** Reuse the current UKCP protocol and server stats API, but separate the benchmark into two executables: one process runs only the server and fixed-rate downlink scheduler, while the other process runs synthetic clients and reports delivery. Measure server CPU externally with `/usr/bin/time` so client work no longer pollutes the CPU numbers.

**Tech Stack:** C++23, native sockets, vendored `ikcp`, CMake, existing repo-local test framework, WSL `/usr/bin/time`

---

### Task 1: Add a failing split-process smoke test

**Files:**
- Modify: `cpp/ukcp_server/CMakeLists.txt`
- Create: `cpp/ukcp_server/tests/split_perf_test.cpp`
- Modify: `cpp/ukcp_server/tests/test_main.cpp` only if test filtering needs adjustment

**Step 1: Write the failing test**

- Add a test that expects `ukcp_perf_server` and `ukcp_perf_client` binaries to exist.
- Launch the server on a test port with a short duration.
- Launch the client with a tiny scenario, for example `16` players, `2` worker threads, `2` seconds.
- Assert the client output contains the fixed-rate summary and exits with success.

**Step 2: Run test to verify it fails**

Run: `cpp/ukcp_server/build/ukcp_server_tests.exe 2>&1`
Expected: FAIL because the split-process binaries do not exist yet.

**Step 3: Write minimal implementation scaffolding**

- Add empty CMake targets or source placeholders as needed so the test can compile.

**Step 4: Run test to verify it still fails for the correct reason**

Run: `cpp/ukcp_server/build/ukcp_server_tests.exe 2>&1`
Expected: FAIL because the new binaries do not yet implement the scenario.

### Task 2: Implement `ukcp_perf_server`

**Files:**
- Modify: `cpp/ukcp_server/CMakeLists.txt`
- Create: `cpp/ukcp_server/bench/perf_server/main.cpp`

**Step 1: Add the build target**

- Add `ukcp_perf_server` to CMake and make the build fail until the source exists.

**Step 2: Run build to verify it fails**

Run: `cmake --build cpp/ukcp_server/build`
Expected: FAIL because `bench/perf_server/main.cpp` does not exist.

**Step 3: Write minimal implementation**

- Parse listen address, duration, downlink rate, and payload size.
- Start the server.
- Count logical uplink packets in the handler.
- Run a fixed-rate `SendToAll` loop for the requested duration.
- Print final stats and exit cleanly.

**Step 4: Run build to verify it passes**

Run: `cmake --build cpp/ukcp_server/build`
Expected: `ukcp_perf_server` builds successfully.

### Task 3: Implement `ukcp_perf_client`

**Files:**
- Modify: `cpp/ukcp_server/CMakeLists.txt`
- Create: `cpp/ukcp_server/bench/perf_client/main.cpp`

**Step 1: Add the build target**

- Add `ukcp_perf_client` to CMake and make the build fail until the source exists.

**Step 2: Run build to verify it fails**

Run: `cmake --build cpp/ukcp_server/build`
Expected: FAIL because `bench/perf_client/main.cpp` does not exist.

**Step 3: Write minimal implementation**

- Create logical clients with KCP auth.
- Distribute players over a configurable worker count.
- Send fixed-rate uplink packets.
- Receive fixed-rate downlink packets.
- Print expected, sent, received, and delivery percentages.

**Step 4: Run the smoke test**

Run: `cpp/ukcp_server/build/ukcp_server_tests.exe 2>&1`
Expected: split-process smoke test passes.

### Task 4: Verify split-process benchmarking under WSL and capture server CPU

**Files:**
- Modify: `docs/plans/2026-03-20-ukcp-cpp-split-perf-design.md` only if implementation meaning changes

**Step 1: Full regression verification**

Run: `cpp/ukcp_server/build/ukcp_server_tests.exe 2>&1`
Run: `dotnet test csharp/UkcpSharp.slnx`
Run: `wsl.exe bash -lc "cd /mnt/d/tkcp/ukcp && cmake --build cpp/ukcp_server/build-wsl-cmake && cd cpp/ukcp_server/build-wsl-cmake && ctest --output-on-failure"`

**Step 2: Run the requested split-process scenario in WSL**

Run:
`wsl.exe bash -lc "cd /mnt/d/tkcp/ukcp/cpp/ukcp_server/build-wsl-cmake && (/usr/bin/time -f 'elapsed_s=%e\nuser_s=%U\nsys_s=%S\ncpu_pct=%P\nmax_rss_kb=%M' ./ukcp_perf_server --listen 127.0.0.1:39220 --seconds 10 --downlink-pps 15 --downlink-payload-bytes 15 >server.out 2>server.time) & server_pid=$!; sleep 1; ./ukcp_perf_client --server 127.0.0.1:39220 --clients 5000 --client-workers 4 --seconds 10 --uplink-pps 8 --downlink-pps 15 --uplink-payload-bytes 10 --downlink-payload-bytes 15 >client.out; wait $server_pid; cat server.out; cat server.time; cat client.out"`

**Step 3: Summarize**

- Report server CPU, RSS, server packet stats, and client delivery ratios.
- State clearly that the CPU number now belongs to the server process only.
