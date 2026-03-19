# UKCP Fast Reconnect Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Let a live client survive short network changes by reusing its existing KCP state, while still allowing same-`sessID` hard reconnect to replace the session after a timeout or fresh auth.

**Architecture:** Keep the 12-byte header unchanged. The server tracks recent KCP activity per session and treats KCP from a new address within a short reconnect window as an address migration if the existing KCP state can decode it. If the session is idle past that window, new-address traffic falls back to the existing auth-and-replace path.

**Tech Stack:** Go, `xtaci/kcp-go/v5`, .NET 10 test client

---

### Task 1: Add failing server/client reconnect tests

**Files:**
- Modify: `client_test.go`
- Modify: `ukcp_test.go`

**Step 1: Write failing tests**

- Add a test where one live client authenticates, receives one downlink, then a second UDP socket reuses the same KCP state from a new address within the fast reconnect window and continues KCP + UDP traffic without `OnSessionClose/OnSessionOpen`.
- Add a test where the session is idle past the reconnect window and same-`sessID` traffic from a new address requires auth-and-replace.

**Step 2: Run targeted tests to verify failure**

Run: `go test -run "TestClientFastReconnect|TestServerReconnectAfterWindow"`  
Expected: FAIL because current server always treats new-address traffic as hard reconnect.

### Task 2: Implement session activity tracking and fast reconnect

**Files:**
- Modify: `config.go`
- Modify: `session.go`
- Modify: `server.go`

**Step 1: Add config knobs**

- Add `FastReconnectWindow` and `SessionIdleTimeout`.
- Provide conservative defaults for MOBA traffic.

**Step 2: Add minimal session state**

- Track last successful inbound KCP time.
- Expose a small helper to decide whether a session is still in the fast reconnect window.

**Step 3: Implement address migration path**

- If KCP arrives from a different address and the session is still warm, try feeding it into the existing KCP state.
- On successful KCP decode/input, update the remote address and continue with the same session.
- Keep UDP blocked on the new address until KCP has refreshed the session address.

**Step 4: Keep hard reconnect path**

- If the session is stale or the new-address KCP cannot continue the old state, fall back to pending auth and replacement.

### Task 3: Verify and document behavior

**Files:**
- Modify: `examples/echo/main.go` if logs need small clarification

**Step 1: Run verification**

Run: `go test ./...`  
Run: `dotnet test csharp/UkcpSharp.slnx`

**Step 2: Manual check**

- Run the echo server.
- Connect a client, then simulate reconnect by recreating socket/state for hard reconnect and by preserving state for fast reconnect.
