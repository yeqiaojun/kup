# UDP KCP Shared Header Design

## Goal

Define a minimal binary header for a single-port room server that accepts both direct UDP packets and KCP packets.

## Decisions

- Fixed header size: `12` bytes
- Byte order: `little-endian`
- `SessID` is aligned with KCP `conv`
- `MsgType` only distinguishes transport path
- `PacketSeq` is used for direct UDP duplicate suppression

## Header Layout

```text
+-------------+-------------+-------------------+-------------------+
| MsgType(2)  | BodyLen(2)  |   SessID/Conv(4)  |   PacketSeq(4)    |
+-------------+-------------+-------------------+-------------------+
```

## Message Types

- `1`: direct UDP payload, routed to player/session by `SessID`
- `2`: KCP payload, payload bytes are forwarded to the matching KCP session input

## Processing Rules

- Always parse the fixed 12-byte header first.
- Reject packets shorter than 12 bytes.
- Reject packets whose payload size does not equal `BodyLen`.
- For direct UDP multi-send, all duplicates of the same logical packet must keep the same `SessID`, `PacketSeq`, and payload.
- Server duplicate suppression key: `SessID + PacketSeq`.

