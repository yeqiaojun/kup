using System.Text;
using UkcpSharp;

var options = ParseArgs(args);
using var tracker = new RunTracker(options.Count);

var client = new UkcpClient(new UkcpClientConfig
{
    Host = options.Host,
    Port = options.Port,
    SessId = options.SessId
});

client.KcpMessage += payload =>
{
    string text = Encoding.UTF8.GetString(payload);
    Console.WriteLine("recv " + text);
    tracker.OnReceive(text);
};
client.Error += message =>
{
    Console.Error.WriteLine("error " + message);
    tracker.OnError(message);
};
client.Closed += () =>
{
    Console.WriteLine("closed");
};

client.ConnectAndAuth(Encoding.UTF8.GetBytes("auth"));

Warmup(client, options.AuthWaitMs);

for (int i = 1; i <= options.Count; i++)
{
    string text = i.ToString();
    byte[] payload = Encoding.UTF8.GetBytes(text);
    Console.WriteLine("send kcp " + text);
    client.SendKcp(payload);
    tracker.OnSentKcp();
    client.Poll();
    Thread.Sleep(100);
    client.Poll();
    Console.WriteLine("send udp " + text + " x3");
    client.SendUdp((uint)i, payload, 3);
    tracker.OnSentUdp(3);
    client.Poll();
    Thread.Sleep(100);
    client.Poll();
}

var deadline = DateTime.UtcNow.AddMilliseconds(options.TimeoutMs);
while (DateTime.UtcNow < deadline && !tracker.IsComplete)
{
    client.Poll();
    Thread.Sleep(5);
}

client.Close();

Console.WriteLine(
    "RESULT sent_kcp=" + tracker.SentKcp +
    " sent_udp=" + tracker.SentUdp +
    " received=" + tracker.Received +
    " expected=" + tracker.ExpectedReceives);

return tracker.HasErrors || !tracker.IsComplete ? 1 : 0;

static void Warmup(UkcpClient client, int waitMs)
{
    var deadline = DateTime.UtcNow.AddMilliseconds(waitMs);
    while (DateTime.UtcNow < deadline)
    {
        client.Poll();
        Thread.Sleep(5);
    }
}

static Options ParseArgs(string[] args)
{
    var options = new Options();
    for (int i = 0; i < args.Length; i++)
    {
        switch (args[i])
        {
            case "--host":
                options.Host = args[++i];
                break;
            case "--port":
                options.Port = int.Parse(args[++i]);
                break;
            case "--sess":
                options.SessId = uint.Parse(args[++i]);
                break;
            case "--count":
                options.Count = int.Parse(args[++i]);
                break;
            case "--auth-wait-ms":
                options.AuthWaitMs = int.Parse(args[++i]);
                break;
            case "--timeout-ms":
                options.TimeoutMs = int.Parse(args[++i]);
                break;
            default:
                throw new ArgumentException("Unknown argument: " + args[i]);
        }
    }

    return options;
}

internal sealed class Options
{
    public string Host { get; set; } = "127.0.0.1";
    public int Port { get; set; } = 9000;
    public uint SessId { get; set; } = 1001;
    public int Count { get; set; } = 25;
    public int AuthWaitMs { get; set; } = 150;
    public int TimeoutMs { get; set; } = 70000;
}

internal sealed class RunTracker : IDisposable
{
    private readonly Dictionary<string, int> _receivedCounts = new Dictionary<string, int>();
    private readonly int _count;

    public RunTracker(int count)
    {
        _count = count;
    }

    public int SentKcp { get; private set; }
    public int SentUdp { get; private set; }
    public int Received { get; private set; }
    public int ExpectedReceives { get { return _count * 4; } }
    public bool HasErrors { get; private set; }
    public bool IsComplete { get { return Received >= ExpectedReceives && HasAllExpectedMessages(); } }

    public void OnSentKcp()
    {
        SentKcp++;
    }

    public void OnSentUdp(int repeat)
    {
        SentUdp += repeat;
    }

    public void OnReceive(string text)
    {
        Received++;
        if (!_receivedCounts.ContainsKey(text))
        {
            _receivedCounts[text] = 0;
        }

        _receivedCounts[text]++;
    }

    public void OnError(string message)
    {
        HasErrors = true;
    }

    private bool HasAllExpectedMessages()
    {
        for (int i = 1; i <= _count; i++)
        {
            string key = i.ToString();
            int count;
            if (!_receivedCounts.TryGetValue(key, out count) || count < 4)
            {
                return false;
            }
        }

        return true;
    }

    public void Dispose()
    {
    }
}
