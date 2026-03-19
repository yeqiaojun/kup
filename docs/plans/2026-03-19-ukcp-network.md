# UKCP Network Layer Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Build an open-box single-port UDP+KCP network layer for room servers.

**Architecture:** Keep ingress simple: one socket read loop, lazy session creation, and one goroutine per session to serialize KCP state, duplicate suppression, and outbound writes. Reuse the existing protocol header and wrap raw KCP bytes inside it.

**Tech Stack:** Go 1.26, standard library, `github.com/xtaci/kcp-go/v5`

---

### Task 1: Expand tests around the network contract

**Files:**
- Create: `D:/tkcp/ukcp/ukcp_test.go`

**Step 1: Write a failing test for UDP routing**

Assert that one direct UDP packet creates a session and reaches `OnUDP`.

**Step 2: Write a failing test for UDP duplicate suppression**

Assert that replaying the same `PacketSeq` for the same session is ignored.

**Step 3: Write a failing test for KCP uplink**

Build a low-level KCP client, wrap generated segments with the 12-byte header, inject them into the server, and assert that `OnKCP` receives the reassembled payload.

**Step 4: Write a failing test for KCP downlink**

Create a session, call `Session.Send`, capture the outgoing wrapped KCP packets, feed them into a client-side KCP instance, and assert that the payload is readable.

### Task 2: Implement the network layer

**Files:**
- Create: `D:/tkcp/ukcp/config.go`
- Create: `D:/tkcp/ukcp/handler.go`
- Create: `D:/tkcp/ukcp/server.go`
- Create: `D:/tkcp/ukcp/session.go`
- Create: `D:/tkcp/ukcp/dedupe.go`

**Step 1: Add config defaults and handler interface**

Keep the public API small and explicit.

**Step 2: Add server lifecycle and read loop**

Read packets, parse the shared header, create/find sessions, and route events.

**Step 3: Add session actor loop**

Serialize remote address updates, UDP duplicate suppression, KCP input, KCP updates, and reliable sends.

**Step 4: Add KCP wrapping**

Wrap KCP output bytes with the protocol header before writing to the shared socket.

### Task 3: Verify

**Files:**
- Test: `D:/tkcp/ukcp/ukcp_test.go`

**Step 1: Run the full test suite**

Run: `go test ./...`

Expected: pass.
