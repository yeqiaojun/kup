# UKCP C++ Room Performance Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Extend the split-process C++ benchmark so it can model `5000` players in `500` concurrent `10`-player MOBA rooms for `60s`.

**Architecture:** Reuse the existing `ukcp_perf_server` and `ukcp_perf_client` binaries. Add a room-mode traffic model that maps sessions into fixed-size rooms, sends `15 Hz` KCP room frames only to room members, and generates deterministic random per-player KCP and UDP uplink traffic.

**Tech Stack:** C++23, existing UKCP C++ server/session code, vendored `ikcp`, CMake, repo-local test framework, WSL `/usr/bin/time`

---

### Task 1: Add a failing room-mode split-perf smoke test

**Files:**
- Modify: `cpp/ukcp_server/tests/split_perf_test.cpp`

**Step 1: Write the failing test**

- Add a second smoke test that launches:
  - `ukcp_perf_server --mode room --rooms 2 --room-size 10 --seconds 3 --room-fps 5`
  - `ukcp_perf_client --mode room --clients 20 --client-workers 2 --seconds 2 --room-size 10 --kcp-pps 3 --udp-pps 5`
- Assert the client output contains the room benchmark summary marker.

**Step 2: Run the test to verify it fails**

Run: `D:/tkcp/ukcp/cpp/ukcp_server/build/ukcp_server_tests.exe`
Expected: FAIL because room mode is not implemented yet.

### Task 2: Implement room-mode server benchmark

**Files:**
- Modify: `cpp/ukcp_server/bench/perf_server/main.cpp`

**Step 1: Add room-mode argument parsing**

- Add `--mode`, `--rooms`, `--room-size`, and `--room-fps`.
- Keep the existing fixed-rate mode as default.

**Step 2: Implement room traffic model**

- Track authenticated sessions.
- Build room membership from session IDs.
- Replace full broadcast with per-room `SendToMultiSess`.
- Count received KCP and UDP inputs separately.
- Print room-mode stats.

**Step 3: Build and verify**

Run: `cmake --build cpp/ukcp_server/build --config Debug`
Expected: build succeeds.

### Task 3: Implement room-mode client benchmark

**Files:**
- Modify: `cpp/ukcp_server/bench/perf_client/main.cpp`

**Step 1: Add room-mode argument parsing**

- Add `--mode`, `--room-size`, `--kcp-pps`, and `--udp-pps`.
- Keep existing fixed-rate mode as default.

**Step 2: Implement deterministic random uplink scheduling**

- For each logical client and each test second, schedule:
  - `3` KCP sends at random offsets
  - `5` UDP sends at random offsets
- Keep KCP auth first.
- Count received room-frame messages.

**Step 3: Print room benchmark summary**

- Include expected/sent KCP
- expected/sent UDP
- actual received room frames
- expected room frames based on server actual ticks if available

### Task 4: Make the smoke test pass

**Files:**
- Modify: `cpp/ukcp_server/tests/split_perf_test.cpp` only if process handling needs small follow-up edits

**Step 1: Run the room smoke test**

Run: `D:/tkcp/ukcp/cpp/ukcp_server/build/ukcp_server_tests.exe`
Expected: room-mode smoke test passes.

**Step 2: Run full regression**

Run: `dotnet test csharp/UkcpSharp.slnx`
Run: `wsl.exe bash -lc "cd /mnt/d/tkcp/ukcp && cmake --build cpp/ukcp_server/build-wsl-cmake && cd cpp/ukcp_server/build-wsl-cmake && ctest --output-on-failure"`
Expected: all green.

### Task 5: Run the realistic WSL room benchmark

**Files:**
- No code changes required unless verification exposes a bug

**Step 1: Run split-process room benchmark**

Run:
`wsl.exe bash -lc "cd /mnt/d/tkcp/ukcp/cpp/ukcp_server/build-wsl-cmake && (/usr/bin/time -f 'elapsed_s=%e\nuser_s=%U\nsys_s=%S\ncpu_pct=%P\nmax_rss_kb=%M' ./ukcp_perf_server --listen 127.0.0.1:39220 --mode room --rooms 500 --room-size 10 --seconds 60 --room-fps 15 >server.out 2>server.time) & server_pid=$!; sleep 1; ./ukcp_perf_client --server 127.0.0.1:39220 --mode room --clients 5000 --client-workers 4 --seconds 60 --room-size 10 --kcp-pps 3 --udp-pps 5 >client.out; wait $server_pid; cat server.out; cat server.time; cat client.out"`

**Step 2: Summarize**

- Report server-only CPU and RSS
- Report KCP/UDP uplink counts
- Report room-frame send/receive ratio
