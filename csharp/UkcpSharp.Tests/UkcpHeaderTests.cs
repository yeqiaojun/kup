using System;
using UkcpSharp;
using Xunit;

namespace UkcpSharp.Tests;

public sealed class UkcpHeaderTests
{
    [Fact]
    public void Header_EncodesAndDecodesLittleEndianLayout()
    {
        var header = new UkcpHeader(UkcpMessageType.Kcp, UkcpHeaderFlags.Connect, 5, 0x11223344u, 0x55667788u);

        byte[] encoded = header.Encode();

        Assert.Equal(new byte[]
        {
            0x02, 0x01,
            0x05, 0x00,
            0x44, 0x33, 0x22, 0x11,
            0x88, 0x77, 0x66, 0x55
        }, encoded);

        Assert.True(UkcpHeader.TryDecode(encoded, out var decoded));
        Assert.Equal(UkcpMessageType.Kcp, decoded.MessageType);
        Assert.Equal(UkcpHeaderFlags.Connect, decoded.Flags);
        Assert.Equal((ushort)5, decoded.BodyLength);
        Assert.Equal(0x11223344u, decoded.SessId);
        Assert.Equal(0x55667788u, decoded.PacketSeq);
    }
}
