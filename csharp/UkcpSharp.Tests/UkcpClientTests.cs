using System;
using System.Collections.Generic;
using System.Linq;
using UkcpSharp;
using Xunit;

namespace UkcpSharp.Tests;

public sealed class UkcpClientTests
{
    [Fact]
    public void SendUdp_RequiresAuthToBeStarted()
    {
        var socket = new FakeDatagramSocket();
        var client = CreateClient(socket, 7001);
        client.Connect();

        Assert.Throws<InvalidOperationException>(() => client.SendUdp(7, new byte[] { 1, 2, 3 }, 3));
    }

    [Fact]
    public void SendUdp_RepeatsSameLogicalPacket()
    {
        var socket = new FakeDatagramSocket();
        var client = CreateClient(socket, 7002);
        client.Connect();
        client.SendAuth(new byte[] { 0x41, 0x55, 0x54, 0x48 });

        socket.ClearSent();
        client.SendUdp(11, new byte[] { 0xAA, 0xBB }, 3);

        Assert.Equal(3, socket.Sent.Count);
        foreach (var datagram in socket.Sent)
        {
            Assert.True(UkcpHeader.TryDecode(datagram.AsSpan(0, UkcpHeader.Size).ToArray(), out var header));
            Assert.Equal(UkcpMessageType.Udp, header.MessageType);
            Assert.Equal((ushort)2, header.BodyLength);
            Assert.Equal(7002u, header.SessId);
            Assert.Equal(11u, header.PacketSeq);
            Assert.Equal(new byte[] { 0xAA, 0xBB }, datagram.Skip(UkcpHeader.Size).ToArray());
        }
    }

    [Fact]
    public void SendKcp_ProducesDatagramsThatRemotePeerCanRead()
    {
        var socket = new FakeDatagramSocket();
        var client = CreateClient(socket, 7003);
        client.Connect();

        client.SendKcp(new byte[] { 0x10, 0x11, 0x12 });

        var remote = new LoopbackKcpPeer(7003);
        foreach (var datagram in socket.Sent)
        {
            remote.InputWrapped(datagram);
        }

        Assert.Equal(new byte[] { 0x10, 0x11, 0x12 }, remote.Receive());
    }

    [Fact]
    public void Poll_RaisesKcpMessageForInboundWrappedPackets()
    {
        var socket = new FakeDatagramSocket();
        var client = CreateClient(socket, 7004);
        client.Connect();

        byte[]? received = null;
        client.KcpMessage += payload => received = payload;

        var remote = new LoopbackKcpPeer(7004);
        foreach (var datagram in remote.SendWrapped(new byte[] { 0x21, 0x22, 0x23 }))
        {
            socket.QueueReceive(datagram);
        }

        client.Poll();

        Assert.Equal(new byte[] { 0x21, 0x22, 0x23 }, received);
    }

    [Fact]
    public void Reconnect_SwapsSocketAndKeepsSending()
    {
        var socket1 = new FakeDatagramSocket();
        var socket2 = new FakeDatagramSocket();
        var sockets = new Queue<FakeDatagramSocket>(new[] { socket1, socket2 });
        var client = new UkcpClient(new UkcpClientConfig
        {
            Host = "127.0.0.1",
            Port = 9000,
            SessId = 7005,
            SocketFactory = () => sockets.Dequeue()
        });

        client.Connect();
        client.SendAuth(new byte[] { 0x41, 0x55, 0x54, 0x48 });
        Assert.NotEmpty(socket1.Sent);

        client.Reconnect();
        client.SendUdp(9, new byte[] { 0x31, 0x32, 0x33 });

        Assert.True(socket1.ClosedCalled);
        Assert.NotEmpty(socket2.Sent);
    }

    private static UkcpClient CreateClient(FakeDatagramSocket socket, uint sessId)
    {
        return new UkcpClient(new UkcpClientConfig
        {
            Host = "127.0.0.1",
            Port = 9000,
            SessId = sessId,
            SocketFactory = () => socket
        });
    }

    private sealed class FakeDatagramSocket : IDatagramSocket
    {
        private readonly Queue<byte[]> _received = new Queue<byte[]>();

        public readonly List<byte[]> Sent = new List<byte[]>();

        public void Connect(string host, int port)
        {
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
    }

    private sealed class LoopbackKcpPeer
    {
        private readonly uint _sessId;
        private readonly kcp2k.Kcp _kcp;
        private readonly List<byte[]> _segments = new List<byte[]>();

        public LoopbackKcpPeer(uint sessId)
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
            _kcp.SetMtu(1200);
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
            Assert.True(UkcpHeader.TryDecode(datagram.AsSpan(0, UkcpHeader.Size).ToArray(), out var header));
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
