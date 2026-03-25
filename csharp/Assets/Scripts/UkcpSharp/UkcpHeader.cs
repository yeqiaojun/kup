using System;

namespace UkcpSharp
{
    [Flags]
    public enum UkcpHeaderFlags : byte
    {
        None = 0,
        Connect = 1
    }

    public readonly struct UkcpHeader
    {
        public const int Size = 12;

        public UkcpHeader(UkcpMessageType messageType, UkcpHeaderFlags flags, ushort bodyLength, uint sessId, uint packetSeq)
        {
            MessageType = messageType;
            Flags = flags;
            BodyLength = bodyLength;
            SessId = sessId;
            PacketSeq = packetSeq;
        }

        public UkcpMessageType MessageType { get; }
        public UkcpHeaderFlags Flags { get; }
        public ushort BodyLength { get; }
        public uint SessId { get; }
        public uint PacketSeq { get; }

        public byte[] Encode()
        {
            byte[] buffer = new byte[Size];
            buffer[0] = (byte)MessageType;
            buffer[1] = (byte)Flags;
            WriteUInt16(buffer, 2, BodyLength);
            WriteUInt32(buffer, 4, SessId);
            WriteUInt32(buffer, 8, PacketSeq);
            return buffer;
        }

        public byte[] Wrap(byte[] payload)
        {
            if (payload == null) throw new ArgumentNullException(nameof(payload));
            byte[] packet = new byte[Size + payload.Length];
            byte[] header = Encode();
            Buffer.BlockCopy(header, 0, packet, 0, header.Length);
            Buffer.BlockCopy(payload, 0, packet, Size, payload.Length);
            return packet;
        }

        public static bool TryDecode(byte[] buffer, out UkcpHeader header)
        {
            return TryDecode(buffer, 0, buffer != null ? buffer.Length : 0, out header);
        }

        public static bool TryDecode(byte[] buffer, int offset, int length, out UkcpHeader header)
        {
            header = default(UkcpHeader);
            if (buffer == null || length < Size || offset < 0 || offset + Size > buffer.Length)
            {
                return false;
            }

            byte messageType = buffer[offset + 0];
            if (messageType != (byte)UkcpMessageType.Udp && messageType != (byte)UkcpMessageType.Kcp)
            {
                return false;
            }

            byte flags = buffer[offset + 1];
            ushort bodyLength = ReadUInt16(buffer, offset + 2);
            uint sessId = ReadUInt32(buffer, offset + 4);
            uint packetSeq = ReadUInt32(buffer, offset + 8);
            header = new UkcpHeader((UkcpMessageType)messageType, (UkcpHeaderFlags)flags, bodyLength, sessId, packetSeq);
            return true;
        }

        private static void WriteUInt16(byte[] buffer, int offset, ushort value)
        {
            buffer[offset + 0] = (byte)(value >> 0);
            buffer[offset + 1] = (byte)(value >> 8);
        }

        private static void WriteUInt32(byte[] buffer, int offset, uint value)
        {
            buffer[offset + 0] = (byte)(value >> 0);
            buffer[offset + 1] = (byte)(value >> 8);
            buffer[offset + 2] = (byte)(value >> 16);
            buffer[offset + 3] = (byte)(value >> 24);
        }

        private static ushort ReadUInt16(byte[] buffer, int offset)
        {
            ushort result = 0;
            result |= buffer[offset + 0];
            result |= (ushort)(buffer[offset + 1] << 8);
            return result;
        }

        private static uint ReadUInt32(byte[] buffer, int offset)
        {
            uint result = 0;
            result |= buffer[offset + 0];
            result |= (uint)(buffer[offset + 1] << 8);
            result |= (uint)(buffer[offset + 2] << 16);
            result |= (uint)(buffer[offset + 3] << 24);
            return result;
        }
    }
}
