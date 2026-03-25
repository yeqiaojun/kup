using System;
using System.Collections.Generic;
using System.Threading;
using kcp2k;

namespace UkcpSharp
{
    public sealed class UkcpClient
    {
        private const uint MinKcpMtu = 50;
        private const uint MaxKcpMessageFragments = 127;

        private readonly UkcpClientConfig _config;
        private readonly string _host;
        private readonly int _port;
        private readonly byte[] _receiveBuffer;
        private readonly Queue<byte[]> _receivedMessages = new Queue<byte[]>();

        private readonly uint _sessId;
        private Kcp _kcp;
        private IDatagramSocket _socket;
        private bool _connected;
        private UkcpHeaderFlags _outputFlags;

        public UkcpClient(string serverAddress, uint sessId, UkcpClientConfig config = null)
        {
            if (string.IsNullOrWhiteSpace(serverAddress))
            {
                throw new ArgumentException("Server address is required.", nameof(serverAddress));
            }
            if (sessId == 0)
            {
                throw new ArgumentOutOfRangeException(nameof(sessId), "SessId must be non-zero.");
            }

            _config = config ?? new UkcpClientConfig();
            (_host, _port) = ParseServerAddress(serverAddress);
            _sessId = sessId;
            _receiveBuffer = new byte[_config.ReceiveBufferSize];
        }

        public uint SessId => _sessId;
        public bool IsConnected => _connected;

        public bool Connect(byte[] authPayload)
        {
            if (authPayload == null) throw new ArgumentNullException(nameof(authPayload));
            if (_connected || authPayload.Length == 0)
            {
                return false;
            }

            try
            {
                _socket = _config.CreateSocket();
                _socket.Connect(_host, _port);

                uint kcpMtu = KcpMtuFromTransportMtu(_config.Mtu);
                if (kcpMtu < MinKcpMtu)
                {
                        Close();
                        return false;
                }

                _kcp = CreateKcp(kcpMtu);
                _connected = true;
                _outputFlags = UkcpHeaderFlags.Connect;
                bool sent = SendKcpInternal(authPayload);
                _outputFlags = UkcpHeaderFlags.None;
                if (!sent)
                {
                    Close();
                }
                return sent;
            }
            catch
            {
                Close();
                return false;
            }
        }

        public void Close()
        {
            _connected = false;
            _outputFlags = UkcpHeaderFlags.None;
            _receivedMessages.Clear();
            if (_socket != null)
            {
                _socket.Close();
                _socket = null;
            }
            _kcp = null;
        }

        public bool SendKcp(byte[] payload)
        {
            if (payload == null) throw new ArgumentNullException(nameof(payload));
            return SendKcpInternal(payload);
        }

        public bool SendUdp(uint packetSeq, byte[] payload)
        {
            if (payload == null) throw new ArgumentNullException(nameof(payload));
            if (!_connected || _socket == null)
            {
                return false;
            }

            var header = new UkcpHeader(UkcpMessageType.Udp, UkcpHeaderFlags.None, checked((ushort)payload.Length), _sessId, packetSeq);
            byte[] packet = header.Wrap(payload);
            _socket.Send(packet, packet.Length);
            return true;
        }

        public bool Poll(int timeoutMs = 0)
        {
            if (!_connected || _socket == null || _kcp == null)
            {
                return false;
            }

            if (timeoutMs < 0)
            {
                timeoutMs = 0;
            }

            bool progressed = PollOnce();
            if (progressed || timeoutMs == 0)
            {
                return progressed;
            }

            DateTime deadline = DateTime.UtcNow.AddMilliseconds(timeoutMs);
            while (DateTime.UtcNow < deadline)
            {
                Thread.Sleep(1);
                progressed = PollOnce();
                if (progressed)
                {
                    return true;
                }
            }

            return false;
        }

        public bool Recv(out byte[] payload)
        {
            if (_receivedMessages.Count == 0)
            {
                payload = Array.Empty<byte>();
                return false;
            }

            payload = _receivedMessages.Dequeue();
            return true;
        }

        private bool PollOnce()
        {
            bool progressed = DrainSocket();
            if (_kcp == null)
            {
                return progressed;
            }

            _kcp.Update(NowMs());
            return DrainKcp() || progressed;
        }

        private bool SendKcpInternal(byte[] payload)
        {
            if (!_connected || _socket == null || _kcp == null)
            {
                return false;
            }
            if (payload.Length > MaxKcpPayloadSize(_kcp))
            {
                return false;
            }

            int result = _kcp.Send(payload, 0, payload.Length);
            if (result != 0)
            {
                return false;
            }

            _kcp.Flush();
            _kcp.Update(NowMs());
            return true;
        }

        private void HandleKcpOutput(byte[] buffer, int size)
        {
            if (!_connected || _socket == null)
            {
                return;
            }

            byte[] segment = new byte[size];
            Buffer.BlockCopy(buffer, 0, segment, 0, size);
            var header = new UkcpHeader(UkcpMessageType.Kcp, _outputFlags, checked((ushort)size), _sessId, 0);
            byte[] packet = header.Wrap(segment);
            _socket.Send(packet, packet.Length);
        }

        private bool DrainSocket()
        {
            if (_socket == null || _kcp == null)
            {
                return false;
            }

            bool progressed = false;
            while (_socket.TryReceive(_receiveBuffer, out int length))
            {
                UkcpHeader header;
                if (!UkcpHeader.TryDecode(_receiveBuffer, 0, length, out header))
                {
                    continue;
                }

                if (header.SessId != _sessId || length != UkcpHeader.Size + header.BodyLength)
                {
                    continue;
                }

                progressed = true;
                if (header.MessageType == UkcpMessageType.Kcp)
                {
                    if (_kcp.Input(_receiveBuffer, UkcpHeader.Size, header.BodyLength) != 0)
                    {
                        continue;
                    }
                }
                else
                {
                    byte[] payload = new byte[header.BodyLength];
                    Buffer.BlockCopy(_receiveBuffer, UkcpHeader.Size, payload, 0, payload.Length);
                    _receivedMessages.Enqueue(payload);
                }
            }

            return progressed;
        }

        private bool DrainKcp()
        {
            if (_kcp == null)
            {
                return false;
            }

            bool progressed = false;
            while (true)
            {
                int size = _kcp.PeekSize();
                if (size <= 0)
                {
                    return progressed;
                }

                byte[] payload = new byte[size];
                int received = _kcp.Receive(payload, payload.Length);
                if (received <= 0)
                {
                    return progressed;
                }

                progressed = true;
                _receivedMessages.Enqueue(payload);
            }
        }

        private Kcp CreateKcp(uint kcpMtu)
        {
            var kcp = new Kcp(_sessId, HandleKcpOutput);
            kcp.SetNoDelay(_config.NoDelay, _config.Interval, _config.Resend, _config.NoCongestion);
            kcp.SetWindowSize(_config.SendWindow, _config.ReceiveWindow);
            kcp.SetMtu(kcpMtu);
            return kcp;
        }

        private static (string Host, int Port) ParseServerAddress(string serverAddress)
        {
            int separator = serverAddress.LastIndexOf(':');
            if (separator <= 0 || separator == serverAddress.Length - 1)
            {
                throw new ArgumentException("Server address must be host:port.", nameof(serverAddress));
            }

            string host = serverAddress.Substring(0, separator);
            if (!int.TryParse(serverAddress.Substring(separator + 1), out int port) || port <= 0 || port > 65535)
            {
                throw new ArgumentException("Server address must contain a valid port.", nameof(serverAddress));
            }

            return (host, port);
        }

        private static uint KcpMtuFromTransportMtu(uint mtu)
        {
            if (mtu <= UkcpHeader.Size)
            {
                return 0;
            }
            return mtu - UkcpHeader.Size;
        }

        private static int MaxKcpPayloadSize(Kcp kcp)
        {
            return checked((int)(kcp.mss * MaxKcpMessageFragments));
        }

        private static uint NowMs()
        {
            return unchecked((uint)Environment.TickCount);
        }
    }
}
