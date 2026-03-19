namespace UkcpSharp
{
    public interface IDatagramSocket
    {
        void Connect(string host, int port);
        void Send(byte[] datagram, int length);
        bool TryReceive(byte[] buffer, out int length);
        void Close();
    }
}
