using System;
using System.Net;
using System.Net.Sockets;

namespace UkcpSharp
{
    public sealed class SocketDatagramSocket : IDatagramSocket
    {
        private Socket _socket;

        public void Connect(string host, int port)
        {
            if (_socket != null)
            {
                return;
            }

            IPAddress[] addresses = Dns.GetHostAddresses(host);
            if (addresses.Length == 0)
            {
                throw new SocketException((int)SocketError.HostNotFound);
            }

            _socket = new Socket(addresses[0].AddressFamily, SocketType.Dgram, ProtocolType.Udp);
            _socket.Blocking = false;
            _socket.Connect(new IPEndPoint(addresses[0], port));
        }

        public void Send(byte[] datagram, int length)
        {
            if (_socket == null) throw new InvalidOperationException("Socket is not connected.");
            _socket.Send(datagram, 0, length, SocketFlags.None);
        }

        public bool TryReceive(byte[] buffer, out int length)
        {
            if (_socket == null)
            {
                length = 0;
                return false;
            }

            try
            {
                length = _socket.Receive(buffer, 0, buffer.Length, SocketFlags.None);
                return length > 0;
            }
            catch (SocketException ex) when (ex.SocketErrorCode == SocketError.WouldBlock || ex.SocketErrorCode == SocketError.IOPending || ex.SocketErrorCode == SocketError.NoBufferSpaceAvailable)
            {
                length = 0;
                return false;
            }
        }

        public void Close()
        {
            if (_socket == null)
            {
                return;
            }

            try
            {
                _socket.Close();
            }
            finally
            {
                _socket = null;
            }
        }
    }
}
