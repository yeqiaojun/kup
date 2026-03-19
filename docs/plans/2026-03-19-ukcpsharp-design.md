# UkcpSharp Client Design

## Goal

Build a Unity-friendly C# client for the current UKCP protocol and server flow.

## Constraints

- Target `netstandard2.1` for easy Unity migration.
- Use one UDP socket for both direct UDP and KCP packets.
- First KCP message is the auth message.
- Direct UDP is only allowed after `SendAuth` has been called.
- Server replies are expected over KCP.
- Support repeated direct UDP sends with the same packet sequence.

## Architecture

- `UkcpClient` is the public API.
- `UkcpHeader` encodes and decodes the shared 12-byte header.
- `IDatagramSocket` abstracts socket I/O for tests and Unity portability.
- `Kcp2kPeer` adapts the low-level `kcp2k.Kcp` implementation.
- `Poll()` drives inbound reads, KCP updates, and event delivery from the calling thread.

## Public API

- `UkcpClient(UkcpClientConfig config)`
- `Connect()`
- `Close()`
- `SendAuth(byte[] payload)`
- `SendUdp(uint packetSeq, byte[] payload, int repeat)`
- `SendKcp(byte[] payload)`
- `Poll()`
- events:
  - `KcpMessage`
  - `UdpMessage`
  - `Error`
  - `Closed`

