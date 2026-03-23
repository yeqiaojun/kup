#include "test_support.hpp"

#include "ukcp/server.hpp"

namespace {

class EchoHandler final : public ukcp::Handler {
      public:
        bool Auth(std::uint32_t, const std::string &, std::span<const std::uint8_t> payload) override {
                return std::string(payload.begin(), payload.end()) == "auth";
        }

        void OnUDP(ukcp::Session &session, std::uint32_t, std::span<const std::uint8_t> payload) override { session.SendKcp(payload); }

        void OnKCP(ukcp::Session &session, std::span<const std::uint8_t> payload) override { session.SendKcp(payload); }
};

} // namespace

UKCP_TEST(Echo_Server_EchoesKcp) {
        EchoHandler handler;
        ukcp::Server server("127.0.0.1:39104", handler, ukcp::Config{});
        UKCP_REQUIRE(server.Start());

        ukcp::test::TestKcpClient client("127.0.0.1:39104", 1101);
        client.SendAuth("auth");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        client.SendKcp("kcp-echo");
        UKCP_REQUIRE(client.WaitForKcp(std::chrono::milliseconds(2000)) == "kcp-echo");
        server.Close();
}
