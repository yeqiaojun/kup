#include "test_support.hpp"
#include "server_internal.hpp"

using ukcp::Config;
using ukcp::Handler;
using ukcp::HeaderFlags;
using ukcp::Server;
using ukcp::Session;
using ukcp::test::Bytes;
using ukcp::test::RecordingHandler;
using ukcp::test::TestKcpClient;
using ukcp::test::WaitUntil;

namespace {

class EchoStatsHandler final : public Handler {
      public:
        bool Auth(std::uint32_t, const std::string &, std::span<const std::uint8_t> payload) override {
                return std::string(payload.begin(), payload.end()) == "auth";
        }

        void OnUDP(Session &session, std::uint32_t, std::span<const std::uint8_t> payload) override {
                ++udp_count;
                session.Send(payload);
        }

        void OnKCP(Session &session, std::span<const std::uint8_t> payload) override {
                ++kcp_count;
                session.Send(payload);
        }

        std::atomic<int> udp_count{0};
        std::atomic<int> kcp_count{0};
};

class RawUdpPushHandler final : public Handler {
      public:
        bool Auth(std::uint32_t, const std::string &, std::span<const std::uint8_t> payload) override {
                return std::string(payload.begin(), payload.end()) == "auth";
        }

        void OnKCP(Session &session, std::span<const std::uint8_t> payload) override {
                ++kcp_count;
                raw_udp_sent = session.SendRawUdp(88, payload);
        }

        std::atomic<int> kcp_count{0};
        std::atomic<bool> raw_udp_sent{false};
};

} // namespace

UKCP_TEST(Server_RejectsUdpBeforeAuth) {
        RecordingHandler handler;
        handler.expected_sess_id = 1001;
        Server server("127.0.0.1:39101", handler, Config{});
        UKCP_REQUIRE(server.Start());

        TestKcpClient client("127.0.0.1:39101", 1001);
        client.SendUdp(7, "ignored", 2);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        UKCP_REQUIRE(handler.udp_count.load() == 0);
        server.Close();
}

UKCP_TEST(Socket_ConnectedChildSocketSharesPortAndExchangesDatagrams) {
#if UKCP_PLATFORM_WINDOWS
        return;
#else
        sockaddr_in server_addr{};
        UKCP_REQUIRE(ukcp::ParseListenAddress("127.0.0.1:39110", server_addr));

        SocketHandle listener = kInvalidSocket;
        std::string error;
        UKCP_REQUIRE(ukcp::OpenUdpSocket(server_addr, listener, error));

        sockaddr_in client_addr{};
        UKCP_REQUIRE(ukcp::ParseListenAddress("127.0.0.1:0", client_addr));
        SocketHandle client = kInvalidSocket;
        UKCP_REQUIRE(ukcp::OpenUdpSocket(client_addr, client, error));

        ukcp::Endpoint server_endpoint{};
        std::memcpy(&server_endpoint.storage, &server_addr, sizeof(server_addr));
        server_endpoint.length = sizeof(server_addr);

        const auto client_hello = Bytes("client-hello");
        UKCP_REQUIRE(ukcp::SendDatagram(client, server_endpoint, client_hello.data(), client_hello.size()));

        std::vector<std::uint8_t> buffer(256);
        ukcp::Datagram listener_datagram{};
        WaitUntil(
                [&] {
                        error.clear();
                        listener_datagram = ukcp::ReceiveDatagram(listener, buffer.data(), buffer.size(), error);
                        return listener_datagram.size == client_hello.size();
                },
                std::chrono::milliseconds(2000), "listener did not receive client datagram");

        SocketHandle session_socket = kInvalidSocket;
        UKCP_REQUIRE(ukcp::OpenConnectedUdpSocket(server_addr, listener_datagram.endpoint, session_socket, error));

        const auto server_hello = Bytes("server-hello");
        UKCP_REQUIRE(ukcp::SendConnectedDatagram(session_socket, server_hello.data(), server_hello.size()));

        ukcp::Datagram client_datagram{};
        WaitUntil(
                [&] {
                        error.clear();
                        client_datagram = ukcp::ReceiveDatagram(client, buffer.data(), buffer.size(), error);
                        return client_datagram.size == server_hello.size();
                },
                std::chrono::milliseconds(2000), "client did not receive child-socket datagram");

        std::string_view echoed(reinterpret_cast<const char *>(buffer.data()), client_datagram.size);
        UKCP_REQUIRE(echoed == "server-hello");

#if !UKCP_PLATFORM_WINDOWS
        const auto server_reply = Bytes("client-to-child");
        UKCP_REQUIRE(ukcp::SendDatagram(client, server_endpoint, server_reply.data(), server_reply.size()));

        ukcp::Datagram child_datagram{};
        WaitUntil(
                [&] {
                        error.clear();
                        child_datagram = ukcp::ReceiveConnectedDatagram(session_socket, buffer.data(), buffer.size(), error);
                        return child_datagram.size == server_reply.size();
                },
                std::chrono::milliseconds(2000), "child socket did not receive connected datagram");

        std::string_view received(reinterpret_cast<const char *>(buffer.data()), child_datagram.size);
        UKCP_REQUIRE(received == "client-to-child");
#endif

        ukcp::CloseSocket(session_socket);
        ukcp::CloseSocket(client);
        ukcp::CloseSocket(listener);
#endif
}

UKCP_TEST(Poller_ReportsReadableUdpListener) {
        sockaddr_in server_addr{};
        UKCP_REQUIRE(ukcp::ParseListenAddress("127.0.0.1:39113", server_addr));

        SocketHandle listener = kInvalidSocket;
        std::string error;
        UKCP_REQUIRE(ukcp::OpenUdpSocket(server_addr, listener, error));

        ukcp::Poller poller;
        UKCP_REQUIRE(poller.Open(listener, error));

        sockaddr_in client_addr{};
        UKCP_REQUIRE(ukcp::ParseListenAddress("127.0.0.1:0", client_addr));
        SocketHandle client = kInvalidSocket;
        UKCP_REQUIRE(ukcp::OpenUdpSocket(client_addr, client, error));

        ukcp::Endpoint server_endpoint{};
        std::memcpy(&server_endpoint.storage, &server_addr, sizeof(server_addr));
        server_endpoint.length = sizeof(server_addr);

        const auto payload = Bytes("poller-ping");
        UKCP_REQUIRE(ukcp::SendDatagram(client, server_endpoint, payload.data(), payload.size()));

        std::vector<SocketHandle> ready;
        const bool readable = poller.Wait(std::chrono::milliseconds(500), ready, error);
        ukcp::test::Require(readable, error.empty() ? "poller wait timeout" : error);
        UKCP_REQUIRE(std::find(ready.begin(), ready.end(), listener) != ready.end());

        poller.Close();
        ukcp::CloseSocket(client);
        ukcp::CloseSocket(listener);
}

UKCP_TEST(Client_AuthPacketCarriesConnectFlagAndDecodesThroughKcp) {
        sockaddr_in server_addr{};
        UKCP_REQUIRE(ukcp::ParseListenAddress("127.0.0.1:39115", server_addr));

        SocketHandle listener = kInvalidSocket;
        std::string error;
        UKCP_REQUIRE(ukcp::OpenUdpSocket(server_addr, listener, error));

        TestKcpClient client("127.0.0.1:39115", 1015);
        client.SendAuth("auth");

        std::vector<std::uint8_t> buffer(2048);
        ukcp::Datagram datagram{};
        WaitUntil(
                [&] {
                        error.clear();
                        datagram = ukcp::ReceiveDatagram(listener, buffer.data(), buffer.size(), error);
                        return datagram.size > 0;
                },
                std::chrono::milliseconds(2000), "listener did not receive auth packet");

        ukcp::Header header{};
        std::span<const std::uint8_t> body;
        UKCP_REQUIRE(ukcp::Header::SplitPacket(std::span<const std::uint8_t>(buffer.data(), datagram.size), header, body));
        UKCP_REQUIRE(header.msg_type == ukcp::MsgType::Kcp);
        UKCP_REQUIRE((header.flags & ukcp::HeaderFlags::Connect) == ukcp::HeaderFlags::Connect);
        UKCP_REQUIRE(header.sess_id == 1015);

        ikcpcb *kcp = ikcp_create(1015, nullptr);
        UKCP_REQUIRE(kcp != nullptr);
        ukcp::Config cfg{};
        ukcp::ConfigureKcp(*kcp, cfg);

        UKCP_REQUIRE(ikcp_input(kcp, reinterpret_cast<const char *>(body.data()), static_cast<long>(body.size())) == 0);
        std::vector<std::uint8_t> scratch;
        const auto messages = ukcp::DrainKcpMessages(*kcp, scratch);
        UKCP_REQUIRE(messages.size() == 1);
        UKCP_REQUIRE(std::string(messages.front().begin(), messages.front().end()) == "auth");
        ikcp_release(kcp);
        ukcp::CloseSocket(listener);
}

UKCP_TEST(Server_HardReconnectReplacesSession) {
        RecordingHandler handler;
        handler.expected_sess_id = 1002;
        Server server("127.0.0.1:39102", handler, Config{});
        UKCP_REQUIRE(server.Start());

        {
                TestKcpClient client1("127.0.0.1:39102", 1002);
                client1.SendAuth("auth");
                WaitUntil([&] { return handler.open_count.load() == 1; }, std::chrono::milliseconds(2000), "first auth did not open session");
        }

        TestKcpClient client2("127.0.0.1:39102", 1002);
        client2.SendAuth("auth");
        WaitUntil([&] { return handler.open_count.load() == 2; }, std::chrono::milliseconds(2000), "hard reconnect did not open replacement session");
        WaitUntil([&] { return handler.close_count.load() == 1; }, std::chrono::milliseconds(2000), "hard reconnect did not close replaced session");
        client2.Poll(std::chrono::milliseconds(100));

        client2.SendKcp("hello");
        WaitUntil([&] { return handler.kcp_count.load() == 1; }, std::chrono::milliseconds(2000), "replacement session did not receive kcp payload");
        server.Close();
}

UKCP_TEST(Server_AuthCallbackReceivesFirstKcpMessage) {
        RecordingHandler handler;
        handler.expected_sess_id = 1014;
        Server server("127.0.0.1:39114", handler, Config{});
        UKCP_REQUIRE(server.Start());

        TestKcpClient client("127.0.0.1:39114", 1014);
        client.SendAuth("auth");

        WaitUntil([&] { return !handler.last_auth_remote.empty(); }, std::chrono::milliseconds(2000), "auth callback did not receive first kcp message");
        server.Close();
}

UKCP_TEST(Server_AuthPacketIncrementsRecvStats) {
        RecordingHandler handler;
        handler.expected_sess_id = 1016;
        Server server("127.0.0.1:39116", handler, Config{});
        UKCP_REQUIRE(server.Start());

        TestKcpClient client("127.0.0.1:39116", 1016);
        client.SendAuth("auth");

        WaitUntil([&] { return server.Stats().recv_packets >= 1; }, std::chrono::milliseconds(2000), "server did not drain auth packet from listener");
        server.Close();
}

UKCP_TEST(Server_FastReconnectKeepsSessionOpen) {
        RecordingHandler handler;
        handler.expected_sess_id = 1003;
        Config config{};
        config.fast_reconnect_window = std::chrono::milliseconds(2000);
        Server server("127.0.0.1:39103", handler, config);
        UKCP_REQUIRE(server.Start());

        TestKcpClient client("127.0.0.1:39103", 1003);
        client.SendAuth("auth");
        WaitUntil([&] { return handler.open_count.load() == 1; }, std::chrono::milliseconds(2000), "auth did not open session");
        client.Poll(std::chrono::milliseconds(100));

        client.Reconnect();
        client.SendKcp("after-fast-reconnect");
        WaitUntil([&] { return handler.kcp_count.load() == 1; }, std::chrono::milliseconds(2000), "fast reconnect payload was not received");
        UKCP_REQUIRE(handler.open_count.load() == 1);
        UKCP_REQUIRE(handler.close_count.load() == 0);
        server.Close();
}

UKCP_TEST(Server_StatsTrackIngressAndSessions) {
        EchoStatsHandler handler;
        Server server("127.0.0.1:39106", handler, Config{});
        UKCP_REQUIRE(server.Start());

        TestKcpClient client("127.0.0.1:39106", 1006);
        client.SendAuth("auth");
        client.Poll(std::chrono::milliseconds(100));

        client.SendKcp("kcp");
        client.SendUdp(17, "udp", 3);
        WaitUntil([&] { return handler.kcp_count.load() == 1 && handler.udp_count.load() == 3; }, std::chrono::milliseconds(2000),
                  "echo handler did not receive expected ingress packets");

        WaitUntil([&] { return server.Stats().sent_kcp_packets >= 4; }, std::chrono::milliseconds(2000),
                  "server stats did not observe expected outbound kcp packets");

        const auto stats = server.Stats();
        UKCP_REQUIRE(stats.active_sessions == 1);
        UKCP_REQUIRE(stats.recv_packets == 5);
        UKCP_REQUIRE(stats.recv_kcp_packets == 2);
        UKCP_REQUIRE(stats.recv_udp_packets == 3);
        UKCP_REQUIRE(stats.sent_kcp_packets >= 4);
        UKCP_REQUIRE(stats.sent_packets == stats.sent_kcp_packets);
        server.Close();
}

UKCP_TEST(Server_SendFlushesWithoutWaitingForGlobalSweep) {
        EchoStatsHandler handler;
        Config config{};
        config.update_interval = std::chrono::milliseconds(300);

        Server server("127.0.0.1:39109", handler, config);
        UKCP_REQUIRE(server.Start());

        TestKcpClient client("127.0.0.1:39109", 1009);
        client.SendAuth("auth");
        WaitUntil([&] { return handler.kcp_count.load() == 0 && server.Stats().active_sessions == 1; }, std::chrono::milliseconds(2000),
                  "auth did not open session");
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        client.Poll(std::chrono::milliseconds(20));

        const auto start = std::chrono::steady_clock::now();
        const auto sent_before = server.Stats().sent_kcp_packets;
        client.SendKcp("echo-now");
        const std::string echoed = client.WaitForKcp(std::chrono::milliseconds(150));
        const auto elapsed = std::chrono::steady_clock::now() - start;

        UKCP_REQUIRE(server.Stats().sent_kcp_packets > sent_before);
        UKCP_REQUIRE(echoed == "echo-now");
        UKCP_REQUIRE(elapsed < std::chrono::milliseconds(150));
        server.Close();
}

UKCP_TEST(Server_SessionCanSendRawUdp) {
        RawUdpPushHandler handler;
        Server server("127.0.0.1:39111", handler, Config{});
        UKCP_REQUIRE(server.Start());

        TestKcpClient client("127.0.0.1:39111", 1011);
        client.SendAuth("auth");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        client.SendKcp("raw-udp-push");
        WaitUntil([&] { return handler.kcp_count.load() == 1 && handler.raw_udp_sent.load(); }, std::chrono::milliseconds(2000), "handler did not send raw udp");

        const auto udp = client.WaitForUdp(std::chrono::milliseconds(2000));
        UKCP_REQUIRE(udp.packet_seq == 88);
        UKCP_REQUIRE(udp.payload == "raw-udp-push");
        server.Close();
}

UKCP_TEST(Server_CanSendRawUdpToSess) {
        RecordingHandler handler;
        handler.expected_sess_id = 1012;
        Server server("127.0.0.1:39112", handler, Config{});
        UKCP_REQUIRE(server.Start());

        TestKcpClient client("127.0.0.1:39112", 1012);
        client.SendAuth("auth");
        WaitUntil([&] { return handler.open_count.load() == 1; }, std::chrono::milliseconds(2000), "auth did not open session");

        UKCP_REQUIRE(server.SendRawUdpToSess(1012, 99, Bytes("server-raw-udp")));
        const auto udp = client.WaitForUdp(std::chrono::milliseconds(2000));
        UKCP_REQUIRE(udp.packet_seq == 99);
        UKCP_REQUIRE(udp.payload == "server-raw-udp");
        WaitUntil([&] { return server.Stats().sent_udp_packets >= 1; }, std::chrono::milliseconds(2000), "server stats did not observe raw udp send");
        server.Close();
}
