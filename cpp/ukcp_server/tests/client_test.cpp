#include "test_support.hpp"

#include "kcp_limits.hpp"
#include "ukcp/client.hpp"
#include "ukcp/server.hpp"

namespace {

class EchoHandler final : public ukcp::Handler {
      public:
        bool Auth(std::uint32_t, const std::string &, std::span<const std::uint8_t> payload) override {
                return std::string(payload.begin(), payload.end()) == "auth";
        }

        void OnKCP(ukcp::Session &session, std::span<const std::uint8_t> payload) override {
                ++kcp_count;
                session.SendKcp(payload);
        }

        std::atomic<int> kcp_count{0};
};

class AcceptHandler final : public ukcp::Handler {
      public:
        bool Auth(std::uint32_t, const std::string &, std::span<const std::uint8_t> payload) override {
                return std::string(payload.begin(), payload.end()) == "auth";
        }
};

} // namespace

UKCP_TEST(UkcpClient_CanConnectSendAndReceiveKcp) {
        EchoHandler handler;
        ukcp::Server server("127.0.0.1:39130", handler, ukcp::Config{});
        UKCP_REQUIRE(server.Start());

        ukcp::Client client("127.0.0.1:39130", 1030, ukcp::Config{});
        UKCP_REQUIRE(client.Connect(ukcp::test::Bytes("auth")));
        ukcp::test::WaitUntil([&] { return server.FindSession(1030) != nullptr; }, std::chrono::milliseconds(2000), "client auth did not open session");

        UKCP_REQUIRE(client.SendKcp(ukcp::test::Bytes("hello")));

        std::vector<std::uint8_t> received;
        ukcp::test::WaitUntil(
                [&] {
                        client.Poll(std::chrono::milliseconds(20));
                        return client.Recv(received);
                },
                std::chrono::milliseconds(2000), "client did not receive echoed kcp");

        UKCP_REQUIRE(std::string(received.begin(), received.end()) == "hello");
        UKCP_REQUIRE(handler.kcp_count.load() == 1);
        server.Close();
}

UKCP_TEST(UkcpClient_CanReceiveServerPushAndUdp) {
        AcceptHandler handler;
        ukcp::Server server("127.0.0.1:39131", handler, ukcp::Config{});
        UKCP_REQUIRE(server.Start());

        ukcp::Client client("127.0.0.1:39131", 1031, ukcp::Config{});
        UKCP_REQUIRE(client.Connect(ukcp::test::Bytes("auth")));
        ukcp::test::WaitUntil([&] { return server.FindSession(1031) != nullptr; }, std::chrono::milliseconds(2000), "client auth did not open session");

        UKCP_REQUIRE(server.SendKcpToSess(1031, ukcp::test::Bytes("server-kcp")));
        UKCP_REQUIRE(server.SendUdpToSess(1031, 77, ukcp::test::Bytes("server-udp")));

        std::vector<std::uint8_t> first_payload;
        std::vector<std::uint8_t> second_payload;
        bool got_first = false;
        bool got_second = false;
        ukcp::test::WaitUntil(
                [&] {
                        client.Poll(std::chrono::milliseconds(20));
                        if (!got_first) { got_first = client.Recv(first_payload); }
                        if (!got_second) { got_second = client.Recv(second_payload); }
                        return got_first && got_second;
                },
                std::chrono::milliseconds(2000), "client did not receive server push packets");

        const std::string first(first_payload.begin(), first_payload.end());
        const std::string second(second_payload.begin(), second_payload.end());
        UKCP_REQUIRE((first == "server-kcp" && second == "server-udp") || (first == "server-udp" && second == "server-kcp"));
        server.Close();
}

UKCP_TEST(UkcpClient_CloseStopsFurtherSends) {
        AcceptHandler handler;
        ukcp::Server server("127.0.0.1:39132", handler, ukcp::Config{});
        UKCP_REQUIRE(server.Start());

        ukcp::Client client("127.0.0.1:39132", 1032, ukcp::Config{});
        UKCP_REQUIRE(client.Connect(ukcp::test::Bytes("auth")));
        ukcp::test::WaitUntil([&] { return server.FindSession(1032) != nullptr; }, std::chrono::milliseconds(2000), "client auth did not open session");

        client.Close();
        UKCP_REQUIRE(!client.SendKcp(ukcp::test::Bytes("after-close")));
        UKCP_REQUIRE(!client.SendUdp(88, ukcp::test::Bytes("after-close")));
        server.Close();
}

UKCP_TEST(UkcpClient_RejectsPayloadAboveConfiguredMtuLimit) {
        AcceptHandler handler;
        ukcp::Config config{};
        config.kcp.mtu = 1024;

        ukcp::Server server("127.0.0.1:39133", handler, config);
        UKCP_REQUIRE(server.Start());

        ukcp::Client client("127.0.0.1:39133", 1033, config);
        UKCP_REQUIRE(client.Connect(ukcp::test::Bytes("auth")));
        ukcp::test::WaitUntil([&] { return server.FindSession(1033) != nullptr; }, std::chrono::milliseconds(2000), "client auth did not open session");

        std::vector<std::uint8_t> payload(ukcp::MaxKcpPayloadSizeForTransportMtu(config.kcp.mtu) + 1, static_cast<std::uint8_t>('x'));
        UKCP_REQUIRE(!client.SendKcp(payload));
        server.Close();
}
