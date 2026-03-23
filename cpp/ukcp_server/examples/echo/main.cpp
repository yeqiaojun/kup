#include <atomic>
#include <csignal>
#include <iostream>
#include <thread>

#include "ukcp/handler.hpp"
#include "ukcp/server.hpp"
#include "ukcp/session.hpp"

namespace {

class EchoHandler final : public ukcp::Handler {
      public:
        bool Auth(std::uint32_t sess_id, const std::string &remote_addr, std::span<const std::uint8_t> payload) override {
                const bool ok = std::string(payload.begin(), payload.end()) == "auth";
                std::cout << "auth sess=" << sess_id << " remote=" << remote_addr << " ok=" << ok << '\n';
                return ok;
        }

        void OnSessionOpen(ukcp::Session &session) override {
                std::cout << "session open sess=" << session.id() << " remote=" << session.RemoteAddrString() << '\n';
        }

        void OnUDP(ukcp::Session &session, std::uint32_t packet_seq, std::span<const std::uint8_t> payload) override {
                std::string text(payload.begin(), payload.end());
                std::cout << "udp sess=" << session.id() << " seq=" << packet_seq << " payload=" << text << '\n';
                session.SendKcp(payload);
        }

        void OnKCP(ukcp::Session &session, std::span<const std::uint8_t> payload) override {
                std::string text(payload.begin(), payload.end());
                std::cout << "kcp sess=" << session.id() << " payload=" << text << '\n';
                session.SendKcp(payload);
        }

        void OnSessionClose(ukcp::Session &session, const std::string &reason) override {
                std::cout << "session close sess=" << session.id() << " reason=" << reason << '\n';
        }
};

std::atomic<bool> g_running{true};

void OnSignal(int) { g_running.store(false); }

} // namespace

int main(int argc, char **argv) {
        std::string listen = "127.0.0.1:9000";
        if (argc > 1) { listen = argv[1]; }

        std::signal(SIGINT, OnSignal);
        std::signal(SIGTERM, OnSignal);

        EchoHandler handler;
        ukcp::Server server(listen, handler, ukcp::Config{});
        if (!server.Start()) {
                std::cerr << "failed to start echo server on " << listen << '\n';
                return 1;
        }

        std::cout << "listening on " << listen << '\n';
        while (g_running.load()) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
        server.Close();
        return 0;
}
