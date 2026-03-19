# UkcpSharp Client Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add a Unity-friendly C# client library that follows the current UKCP server flow and supports repeated UDP sends plus KCP on one port.

**Architecture:** Build a small `netstandard2.1` library with one public client class, one datagram socket abstraction, one protocol header helper, and one KCP adapter. Reuse the proven low-level `kcp2k.Kcp` implementation and wrap it in the current UKCP session/auth rules. Drive all network processing from a `Poll()` method for Unity compatibility.

**Tech Stack:** .NET SDK 10, `netstandard2.1`, xUnit, vendored `kcp2k` low-level source

---

### Task 1: Project bootstrap

**Files:**
- Create: `D:/tkcp/ukcp/csharp/UkcpSharp/UkcpSharp.csproj`
- Create: `D:/tkcp/ukcp/csharp/UkcpSharp.Tests/UkcpSharp.Tests.csproj`

**Step 1: Set up solution and project references**

Add the library and test project to the solution and reference the library from tests.

### Task 2: Write failing tests

**Files:**
- Create: `D:/tkcp/ukcp/csharp/UkcpSharp.Tests/UkcpHeaderTests.cs`
- Create: `D:/tkcp/ukcp/csharp/UkcpSharp.Tests/UkcpClientTests.cs`

**Step 1: Write a failing test for the 12-byte header layout**

Verify little-endian encode/decode.

**Step 2: Write a failing test for repeated UDP sending**

Verify one logical UDP send emits `repeat` datagrams with the same packet sequence.

**Step 3: Write a failing test for KCP uplink/downlink on one socket**

Use a fake datagram socket and real KCP peers to verify KCP send and receive behavior.

**Step 4: Run tests to verify they fail**

Run: `dotnet test csharp/UkcpSharp.slnx`

Expected: fail because the client types do not exist yet.

### Task 3: Implement the client

**Files:**
- Create: `D:/tkcp/ukcp/csharp/UkcpSharp/UkcpClient.cs`
- Create: `D:/tkcp/ukcp/csharp/UkcpSharp/UkcpClientConfig.cs`
- Create: `D:/tkcp/ukcp/csharp/UkcpSharp/UkcpHeader.cs`
- Create: `D:/tkcp/ukcp/csharp/UkcpSharp/IDatagramSocket.cs`
- Create: `D:/tkcp/ukcp/csharp/UkcpSharp/SocketDatagramSocket.cs`
- Create: `D:/tkcp/ukcp/csharp/UkcpSharp/Internal/Kcp2k/*.cs`

**Step 1: Implement the protocol header**

Keep it fixed-size and explicit.

**Step 2: Implement the client state machine**

Support connect, auth, repeated UDP send, KCP send, and poll-driven receive.

**Step 3: Integrate the low-level KCP peer**

Wrap outbound KCP segments with the UKCP header and feed inbound KCP payloads into the peer.

### Task 4: Verify

**Files:**
- Test: `D:/tkcp/ukcp/csharp/UkcpSharp.Tests/*.cs`

**Step 1: Run the full test suite**

Run: `dotnet test csharp/UkcpSharp.slnx`

Expected: pass.
