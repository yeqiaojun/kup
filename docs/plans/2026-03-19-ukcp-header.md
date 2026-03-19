# UKCP Header Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Build a minimal Go package that encodes, decodes, and validates the shared 12-byte UDP/KCP header.

**Architecture:** Keep the protocol package small: one header type, one parse function, and one packet split helper. Tests drive the binary contract first, then implementation fills only the required behavior.

**Tech Stack:** Go 1.26, standard library only

---

### Task 1: Bootstrap module

**Files:**
- Create: `D:/tkcp/ukcp/go.mod`

**Step 1: Create the module**

Use module path `ukcp`.

### Task 2: Header contract tests

**Files:**
- Create: `D:/tkcp/ukcp/protocol/header_test.go`

**Step 1: Write a failing test for header encode/decode**

Verify a header round-trips with little-endian byte layout.

**Step 2: Run the test to verify it fails**

Run: `go test ./...`

Expected: fail because protocol code does not exist yet.

### Task 3: Minimal implementation

**Files:**
- Create: `D:/tkcp/ukcp/protocol/header.go`

**Step 1: Implement the fixed-size header**

Add constants, `Header`, `MarshalBinary`, `UnmarshalHeader`, and `SplitPacket`.

**Step 2: Keep validation explicit**

Reject short packets, invalid message type, and payload length mismatch.

### Task 4: Verification

**Files:**
- Test: `D:/tkcp/ukcp/protocol/header_test.go`

**Step 1: Run the full test suite**

Run: `go test ./...`

Expected: pass.
