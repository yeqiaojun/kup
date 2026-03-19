using System;
using kcp2k;

namespace UkcpSharp
{
    public sealed class UkcpClient
    {
        private readonly UkcpClientConfig _config;
        private readonly Kcp _kcp;
        private readonly byte[] _receiveBuffer;

        private IDatagramSocket _socket;
        private bool _connected;
        private bool _authStarted;
        private UkcpHeaderFlags _outputFlags;

        public UkcpClient(UkcpClientConfig config)
        {
            _config = config ?? throw new ArgumentNullException(nameof(config));
            if (_config.SessId == 0)
            {
                throw new ArgumentOutOfRangeException(nameof(config), "SessId must be non-zero.");
            }

            _socket = _config.CreateSocket();
            _receiveBuffer = new byte[_config.PacketBufferSize];
            _kcp = new Kcp(_config.SessId, HandleKcpOutput);
            _kcp.SetNoDelay(_config.NoDelay, _config.Interval, _config.Resend, _config.NoCongestion);
            _kcp.SetWindowSize(_config.SendWindow, _config.ReceiveWindow);
            _kcp.SetMtu(_config.Mtu);
        }

        public uint SessId { get { return _config.SessId; } }
        public bool IsAuthStarted { get { return _authStarted; } }

        public event Action<byte[]>? KcpMessage;
        public event Action<byte[]>? UdpMessage;
        public event Action<string>? Error;
        public event Action? Closed;

        public void Connect()
        {
            if (_connected)
            {
                return;
            }

            _socket.Connect(_config.Host, _config.Port);
            _connected = true;
        }

        public void SendAuth(byte[] payload)
        {
            if (payload == null) throw new ArgumentNullException(nameof(payload));
            EnsureConnected();
            _authStarted = true;
            SendKcpInternal(payload, UkcpHeaderFlags.Connect);
        }

        public void SendUdp(uint packetSeq, byte[] payload, int repeat = 1)
        {
            if (payload == null) throw new ArgumentNullException(nameof(payload));
            EnsureConnected();
            if (!_authStarted)
            {
                throw new InvalidOperationException("Auth must start before sending UDP.");
            }

            if (repeat <= 0)
            {
                repeat = 1;
            }

            var header = new UkcpHeader(UkcpMessageType.Udp, UkcpHeaderFlags.None, checked((ushort)payload.Length), _config.SessId, packetSeq);
            byte[] packet = header.Wrap(payload);
            for (int i = 0; i < repeat; i++)
            {
                _socket.Send(packet, packet.Length);
            }
        }

        public void SendKcp(byte[] payload)
        {
            if (payload == null) throw new ArgumentNullException(nameof(payload));
            EnsureConnected();
            SendKcpInternal(payload, UkcpHeaderFlags.None);
        }

        public void Reconnect()
        {
            EnsureConnected();

            IDatagramSocket next = _config.CreateSocket();
            next.Connect(_config.Host, _config.Port);

            IDatagramSocket current = _socket;
            _socket = next;
            current.Close();
        }

        public void Poll()
        {
            if (!_connected)
            {
                return;
            }

            DrainSocket();
            _kcp.Update(NowMs());
            DrainKcp();
        }

        public void Close()
        {
            if (!_connected)
            {
                return;
            }

            _connected = false;
            _socket.Close();
            if (Closed != null) Closed();
        }

        private void SendKcpInternal(byte[] payload, UkcpHeaderFlags flags)
        {
            _outputFlags = flags;
            try
            {
                int result = _kcp.Send(payload, 0, payload.Length);
                if (result != 0)
                {
                    throw new InvalidOperationException("KCP send failed.");
                }

                _kcp.Update(NowMs());
            }
            finally
            {
                _outputFlags = UkcpHeaderFlags.None;
            }
        }

        private void HandleKcpOutput(byte[] buffer, int size)
        {
            if (!_connected)
            {
                return;
            }

            byte[] segment = new byte[size];
            Buffer.BlockCopy(buffer, 0, segment, 0, size);
            var header = new UkcpHeader(UkcpMessageType.Kcp, _outputFlags, checked((ushort)size), _config.SessId, 0);
            byte[] packet = header.Wrap(segment);
            _socket.Send(packet, packet.Length);
        }

        private void DrainSocket()
        {
            while (_socket.TryReceive(_receiveBuffer, out int length))
            {
                UkcpHeader header;
                if (!UkcpHeader.TryDecode(_receiveBuffer, 0, length, out header))
                {
                    if (Error != null) Error("Invalid UKCP header.");
                    continue;
                }

                if (header.SessId != _config.SessId || length != UkcpHeader.Size + header.BodyLength)
                {
                    continue;
                }

                if (header.MessageType == UkcpMessageType.Kcp)
                {
                    int result = _kcp.Input(_receiveBuffer, UkcpHeader.Size, header.BodyLength);
                    if (result != 0 && Error != null)
                    {
                        Error("KCP input failed.");
                    }
                }
                else
                {
                    byte[] payload = new byte[header.BodyLength];
                    Buffer.BlockCopy(_receiveBuffer, UkcpHeader.Size, payload, 0, payload.Length);
                    if (UdpMessage != null) UdpMessage(payload);
                }
            }
        }

        private void DrainKcp()
        {
            while (true)
            {
                int size = _kcp.PeekSize();
                if (size <= 0)
                {
                    return;
                }

                byte[] payload = new byte[size];
                int received = _kcp.Receive(payload, payload.Length);
                if (received <= 0)
                {
                    return;
                }

                if (KcpMessage != null) KcpMessage(payload);
            }
        }

        private void EnsureConnected()
        {
            if (!_connected)
            {
                throw new InvalidOperationException("Client is not connected.");
            }
        }

        private static uint NowMs()
        {
            return unchecked((uint)Environment.TickCount64);
        }
    }
}
