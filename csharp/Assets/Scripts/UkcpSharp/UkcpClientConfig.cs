using System;

namespace UkcpSharp
{
    public sealed class UkcpClientConfig
    {
        public int ReceiveBufferSize { get; set; } = 2048;
        public uint Mtu { get; set; } = 1024;
        public uint NoDelay { get; set; } = 1;
        public uint Interval { get; set; } = 10;
        public int Resend { get; set; } = 2;
        public bool NoCongestion { get; set; } = true;
        public uint SendWindow { get; set; } = 128;
        public uint ReceiveWindow { get; set; } = 128;
        public Func<IDatagramSocket> SocketFactory { get; set; }

        internal IDatagramSocket CreateSocket()
        {
            return SocketFactory != null ? SocketFactory() : new SocketDatagramSocket();
        }
    }
}
