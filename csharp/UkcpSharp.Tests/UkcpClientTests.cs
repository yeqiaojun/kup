using System;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using UkcpSharp;
using Xunit;

namespace UkcpSharp.Tests;

public sealed class UkcpClientTests
{
    [Fact]
    public void Config_DefaultMtuIs1024()
    {
        var config = new UkcpClientConfig();

        Assert.Equal(1024u, config.Mtu);
    }

    [Fact]
    public void Connect_SendsAuthPacketWithConnectFlag()
    {
        var socket = new FakeDatagramSocket();
        var client = CreateClient(socket, 7001);

        Assert.True(client.Connect(new byte[] { 0x41, 0x55, 0x54, 0x48 }));
        Assert.True(socket.ConnectedCalled);
        Assert.NotEmpty(socket.Sent);
        Assert.True(UkcpHeader.TryDecode(socket.Sent[0], out var header));
        Assert.Equal(UkcpMessageType.Kcp, header.MessageType);
        Assert.Equal(UkcpHeaderFlags.Connect, header.Flags);
    }

    [Fact]
    public void Connect_ConfiguresKcpMtuAsTransportMinusHeader()
    {
        var socket = new FakeDatagramSocket();
        var client = CreateClient(socket, 7002);

        Assert.True(client.Connect(new byte[] { 0x41, 0x55, 0x54, 0x48 }));

        var kcp = Assert.IsType<kcp2k.Kcp>(typeof(UkcpClient).GetField("_kcp", BindingFlags.Instance | BindingFlags.NonPublic)!.GetValue(client));
        uint mtu = ReadUIntField(kcp, "mtu");
        uint mss = ReadUIntField(kcp, "mss");

        Assert.Equal(1024u - (uint)UkcpHeader.Size, mtu);
        Assert.Equal(1024u - (uint)UkcpHeader.Size - (uint)kcp2k.Kcp.OVERHEAD, mss);
    }

    [Fact]
    public void SendUdp_SendsSingleLogicalPacket()
    {
        var socket = new FakeDatagramSocket();
        var client = CreateClient(socket, 7003);
        Assert.True(client.Connect(new byte[] { 0x41, 0x55, 0x54, 0x48 }));

        socket.ClearSent();
        Assert.True(client.SendUdp(11, new byte[] { 0xAA, 0xBB }));

        Assert.Single(socket.Sent);
        Assert.True(UkcpHeader.TryDecode(socket.Sent[0], out var header));
        Assert.Equal(UkcpMessageType.Udp, header.MessageType);
        Assert.Equal((ushort)2, header.BodyLength);
        Assert.Equal(7003u, header.SessId);
        Assert.Equal(11u, header.PacketSeq);
        Assert.Equal(new byte[] { 0xAA, 0xBB }, socket.Sent[0].Skip(UkcpHeader.Size).ToArray());
    }

    [Fact]
    public void SendKcp_ProducesDatagramsThatRemotePeerCanRead()
    {
        var socket = new FakeDatagramSocket();
        var client = CreateClient(socket, 7004);
        Assert.True(client.Connect(new byte[] { 0x41, 0x55, 0x54, 0x48 }));

        var remote = new LoopbackKcpPeer(7004, 1024u);
        foreach (var datagram in socket.Sent)
        {
            remote.InputWrapped(datagram);
        }
        Assert.Equal(new byte[] { 0x41, 0x55, 0x54, 0x48 }, remote.Receive());

        socket.ClearSent();
        Assert.True(client.SendKcp(new byte[] { 0x10, 0x11, 0x12 }));
        foreach (var datagram in socket.Sent)
        {
            remote.InputWrapped(datagram);
        }

        Assert.Equal(new byte[] { 0x10, 0x11, 0x12 }, remote.Receive());
    }

    [Fact]
    public void Poll_Recv_ReturnsInboundKcpPayload()
    {
        var socket = new FakeDatagramSocket();
        var client = CreateClient(socket, 7005);
        Assert.True(client.Connect(new byte[] { 0x41, 0x55, 0x54, 0x48 }));

        var remote = new LoopbackKcpPeer(7005, 1024u);
        foreach (var datagram in remote.SendWrapped(new byte[] { 0x21, 0x22, 0x23 }))
        {
            socket.QueueReceive(datagram);
        }

        Assert.True(client.Poll());
        Assert.True(client.Recv(out var received));
        Assert.Equal(new byte[] { 0x21, 0x22, 0x23 }, received);
    }

    [Fact]
    public void Poll_Recv_ReturnsInboundUdpPayloadWithoutHeader()
    {
        var socket = new FakeDatagramSocket();
        var client = CreateClient(socket, 7006);
        Assert.True(client.Connect(new byte[] { 0x41, 0x55, 0x54, 0x48 }));

        byte[] packet = new UkcpHeader(UkcpMessageType.Udp, UkcpHeaderFlags.None, 3, 7006u, 99).Wrap(new byte[] { 0x31, 0x32, 0x33 });
        socket.QueueReceive(packet);

        Assert.True(client.Poll());
        Assert.True(client.Recv(out var received));
        Assert.Equal(new byte[] { 0x31, 0x32, 0x33 }, received);
    }

    [Fact]
    public void SendKcp_ReturnsFalseWhenPayloadExceedsSingleMessageLimit()
    {
        var socket = new FakeDatagramSocket();
        var client = CreateClient(socket, 7007);
        Assert.True(client.Connect(new byte[] { 0x41, 0x55, 0x54, 0x48 }));

        socket.ClearSent();
        int maxPayload = checked((int)((1024u - (uint)UkcpHeader.Size - (uint)kcp2k.Kcp.OVERHEAD) * 127u));
        byte[] payload = new byte[maxPayload + 1];

        Assert.False(client.SendKcp(payload));
        Assert.Empty(socket.Sent);
    }

    [Fact]
    public void Close_StopsFurtherSends()
    {
        var socket = new FakeDatagramSocket();
        var client = CreateClient(socket, 7008);
        Assert.True(client.Connect(new byte[] { 0x41, 0x55, 0x54, 0x48 }));

        client.Close();

        Assert.False(client.IsConnected);
        Assert.True(socket.ClosedCalled);
        Assert.False(client.SendKcp(new byte[] { 0x01 }));
        Assert.False(client.SendUdp(7, new byte[] { 0x02 }));
        Assert.False(client.Poll());
        Assert.False(client.Recv(out _));
    }

    private static UkcpClient CreateClient(FakeDatagramSocket socket, uint sessId, UkcpClientConfig? config = null)
    {
        UkcpClientConfig effectiveConfig = config ?? new UkcpClientConfig();
        effectiveConfig.SocketFactory = () => socket;
        return new UkcpClient("127.0.0.1:9000", sessId, effectiveConfig);
    }

    private static uint ReadUIntField(object instance, string name)
    {
        return (uint)(typeof(kcp2k.Kcp).GetField(name, BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public)!.GetValue(instance)
            ?? throw new InvalidOperationException("missing field"));
    }

    private sealed class FakeDatagramSocket : IDatagramSocket
    {
        private readonly Queue<byte[]> _received = new Queue<byte[]>();

        public readonly List<byte[]> Sent = new List<byte[]>();

        public void Connect(string host, int port)
        {
            ConnectedCalled = true;
        }

        public void Send(byte[] datagram, int length)
        {
            var copy = new byte[length];
            Buffer.BlockCopy(datagram, 0, copy, 0, length);
            Sent.Add(copy);
        }

        public bool TryReceive(byte[] buffer, out int length)
        {
            if (_received.Count == 0)
            {
                length = 0;
                return false;
            }

            byte[] datagram = _received.Dequeue();
            Buffer.BlockCopy(datagram, 0, buffer, 0, datagram.Length);
            length = datagram.Length;
            return true;
        }

        public void QueueReceive(byte[] datagram)
        {
            _received.Enqueue(datagram);
        }

        public void ClearSent()
        {
            Sent.Clear();
        }

        public void Close()
        {
            ClosedCalled = true;
        }

        public bool ClosedCalled { get; private set; }
        public bool ConnectedCalled { get; private set; }
    }

    private sealed class LoopbackKcpPeer
    {
        private readonly uint _sessId;
        private readonly kcp2k.Kcp _kcp;
        private readonly List<byte[]> _segments = new List<byte[]>();

        public LoopbackKcpPeer(uint sessId, uint transportMtu)
        {
            _sessId = sessId;
            _kcp = new kcp2k.Kcp(sessId, (buffer, size) =>
            {
                var copy = new byte[size];
                Buffer.BlockCopy(buffer, 0, copy, 0, size);
                _segments.Add(copy);
            });
            _kcp.SetNoDelay(1, 10, 2, true);
            _kcp.SetWindowSize(128, 128);
            _kcp.SetMtu(transportMtu - (uint)UkcpHeader.Size);
        }

        public IReadOnlyList<byte[]> SendWrapped(byte[] payload)
        {
            _segments.Clear();
            Assert.Equal(0, _kcp.Send(payload, 0, payload.Length));
            _kcp.Update(NowMs());

            return _segments
                .Select(segment => new UkcpHeader(UkcpMessageType.Kcp, UkcpHeaderFlags.None, (ushort)segment.Length, _sessId, 0).Wrap(segment))
                .ToArray();
        }

        public void InputWrapped(byte[] datagram)
        {
            Assert.True(UkcpHeader.TryDecode(datagram, out var header));
            Assert.Equal(UkcpMessageType.Kcp, header.MessageType);
            byte[] body = datagram.Skip(UkcpHeader.Size).ToArray();
            Assert.Equal(0, _kcp.Input(body, 0, body.Length));
            _kcp.Update(NowMs());
        }

        public byte[] Receive()
        {
            int size = _kcp.PeekSize();
            Assert.True(size > 0);
            byte[] buffer = new byte[size];
            int received = _kcp.Receive(buffer, buffer.Length);
            Assert.Equal(size, received);
            return buffer;
        }

        private static uint NowMs()
        {
            return unchecked((uint)Environment.TickCount64);
        }
    }
}
