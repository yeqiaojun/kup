#pragma once

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "ikcp.h"
#include "platform_socket.hpp"
#include "test_framework.hpp"
#include "ukcp/handler.hpp"
#include "ukcp/protocol.hpp"
#include "ukcp/server.hpp"
#include "ukcp/session.hpp"

namespace ukcp::test {

inline std::vector<std::uint8_t> Bytes(std::string_view text) { return std::vector<std::uint8_t>(text.begin(), text.end()); }

inline std::filesystem::path FindBuiltBinary(std::string_view name) {
        const auto cwd = std::filesystem::current_path();
        const std::vector<std::filesystem::path> candidates{
                cwd / name,
                cwd / "cpp" / "ukcp_server" / "build" / name,
                cwd / "cpp" / "ukcp_server" / "build-split-wsl" / name,
                cwd / "cpp" / "ukcp_server" / "build-split-win" / "Debug" / name,
                cwd / "build" / name,
                cwd / "build-split-wsl" / name,
                cwd / "build-split-win" / "Debug" / name,
                cwd.parent_path() / name,
        };
        for (const auto &candidate : candidates) {
                if (std::filesystem::exists(candidate)) { return candidate; }
        }
        return {};
}

template <class Predicate> inline void WaitUntil(Predicate &&predicate, std::chrono::milliseconds timeout, std::string_view message) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
                if (predicate()) { return; }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        throw std::runtime_error(std::string(message));
}

struct EventQueue {
        std::mutex mutex;
        std::condition_variable cv;
        std::queue<std::string> values;

        void Push(std::string value) {
                {
                        std::lock_guard lock(mutex);
                        values.push(std::move(value));
                }
                cv.notify_one();
        }

        std::string WaitFor(std::chrono::milliseconds timeout) {
                std::unique_lock lock(mutex);
                const bool ok = cv.wait_for(lock, timeout, [&] { return !values.empty(); });
                Require(ok, "timed out waiting for event");
                std::string value = std::move(values.front());
                values.pop();
                return value;
        }
};

class RecordingHandler final : public Handler {
      public:
        bool Auth(std::uint32_t sess_id, const std::string &remote_addr, std::span<const std::uint8_t> payload) override {
                last_auth_remote = remote_addr;
                return accept_auth && sess_id == expected_sess_id && std::vector<std::uint8_t>(payload.begin(), payload.end()) == expected_auth_payload;
        }

        void OnSessionOpen(Session &session) override {
                ++open_count;
                session_open.Push(std::to_string(session.id()));
        }

        void OnUDP(Session &session, std::uint32_t packet_seq, std::span<const std::uint8_t> payload) override {
                (void)session;
                ++udp_count;
                udp_events.Push(std::to_string(packet_seq) + ":" + std::string(payload.begin(), payload.end()));
        }

        void OnKCP(Session &session, std::span<const std::uint8_t> payload) override {
                (void)session;
                ++kcp_count;
                kcp_events.Push(std::string(payload.begin(), payload.end()));
        }

        void OnSessionClose(Session &session, const std::string &reason) override {
                (void)session;
                ++close_count;
                close_events.Push(reason);
        }

        std::uint32_t expected_sess_id{0};
        std::vector<std::uint8_t> expected_auth_payload = Bytes("auth");
        bool accept_auth{true};
        std::string last_auth_remote;
        std::atomic<int> open_count{0};
        std::atomic<int> close_count{0};
        std::atomic<int> udp_count{0};
        std::atomic<int> kcp_count{0};
        EventQueue session_open;
        EventQueue close_events;
        EventQueue udp_events;
        EventQueue kcp_events;
};

class TestKcpClient {
      public:
        struct UdpMessage {
                std::uint32_t packet_seq{0};
                std::string payload;
        };

        TestKcpClient(std::string server_addr, std::uint32_t sess_id) : server_addr_(std::move(server_addr)), sess_id_(sess_id) {
                Require(OpenSocket(), "client socket open failed");
                kcp_ = ikcp_create(sess_id_, this);
                kcp_->output = &Output;
                ikcp_nodelay(kcp_, 1, 10, 2, 1);
                ikcp_wndsize(kcp_, 128, 128);
                ikcp_setmtu(kcp_, 1200);
        }

        ~TestKcpClient() {
                if (kcp_ != nullptr) {
                        ikcp_release(kcp_);
                        kcp_ = nullptr;
                }
                CloseSocket(socket_);
        }

        void SendAuth(std::string_view text) { SendKcpInternal(Bytes(text), HeaderFlags::Connect); }

        void SendKcp(std::string_view text) { SendKcpInternal(Bytes(text), HeaderFlags::None); }

        void SendUdp(std::uint32_t packet_seq, std::string_view text, int repeat = 1) {
                Header header{};
                header.msg_type = MsgType::Udp;
                header.flags = HeaderFlags::None;
                header.body_len = static_cast<std::uint16_t>(text.size());
                header.sess_id = sess_id_;
                header.packet_seq = packet_seq;

                std::vector<std::uint8_t> packet(Header::kSize + text.size());
                Require(header.EncodeTo(packet), "udp header encode failed");
                std::copy(text.begin(), text.end(), packet.begin() + static_cast<std::ptrdiff_t>(Header::kSize));
                for (int i = 0; i < repeat; ++i) { Require(SendDatagram(socket_, server_endpoint_, packet.data(), packet.size()), "udp send failed"); }
        }

        void Reconnect() {
                CloseSocket(socket_);
                socket_ = kInvalidSocket;
                Require(OpenSocket(), "client reconnect socket open failed");
        }

        void Poll(std::chrono::milliseconds timeout = std::chrono::milliseconds(50)) {
                const auto deadline = std::chrono::steady_clock::now() + timeout;
                std::vector<std::uint8_t> buffer(2048);
                while (std::chrono::steady_clock::now() < deadline) {
                        std::string error;
                        Datagram datagram = ReceiveDatagram(socket_, buffer.data(), buffer.size(), error);
                        if (!error.empty() || datagram.size == 0) {
                                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                                continue;
                        }

                        Header header{};
                        std::span<const std::uint8_t> body;
                        Require(Header::SplitPacket(std::span<const std::uint8_t>(buffer.data(), datagram.size), header, body), "received invalid packet");
                        if (header.msg_type != MsgType::Kcp) { continue; }

                        Require(ikcp_input(kcp_, reinterpret_cast<const char *>(body.data()), static_cast<long>(body.size())) == 0, "ikcp_input failed");
                        ikcp_update(kcp_, static_cast<IUINT32>(NowMs()));
                }
        }

        std::string WaitForKcp(std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) {
                const auto deadline = std::chrono::steady_clock::now() + timeout;
                std::vector<std::uint8_t> buffer(2048);
                while (std::chrono::steady_clock::now() < deadline) {
                        const int pending = ikcp_peeksize(kcp_);
                        if (pending >= 0) {
                                recv_scratch_.resize(static_cast<std::size_t>(pending));
                                const int rc = ikcp_recv(kcp_, reinterpret_cast<char *>(recv_scratch_.data()), pending);
                                Require(rc >= 0, "ikcp_recv failed");
                                return std::string(recv_scratch_.begin(), recv_scratch_.begin() + rc);
                        }

                        std::string error;
                        Datagram datagram = ReceiveDatagram(socket_, buffer.data(), buffer.size(), error);
                        if (datagram.size == 0) {
                                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                                continue;
                        }

                        Header header{};
                        std::span<const std::uint8_t> body;
                        Require(Header::SplitPacket(std::span<const std::uint8_t>(buffer.data(), datagram.size), header, body), "received invalid packet");
                        Require(header.msg_type == MsgType::Kcp, "expected kcp packet");
                        Require(ikcp_input(kcp_, reinterpret_cast<const char *>(body.data()), static_cast<long>(body.size())) == 0, "ikcp_input failed");
                        ikcp_update(kcp_, static_cast<IUINT32>(NowMs()));
                        const int size = ikcp_peeksize(kcp_);
                        if (size < 0) { continue; }

                        recv_scratch_.resize(static_cast<std::size_t>(size));
                        const int rc = ikcp_recv(kcp_, reinterpret_cast<char *>(recv_scratch_.data()), size);
                        Require(rc >= 0, "ikcp_recv failed");
                        return std::string(recv_scratch_.begin(), recv_scratch_.begin() + rc);
                }

                throw std::runtime_error("timed out waiting for kcp message");
        }

        UdpMessage WaitForUdp(std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) {
                const auto deadline = std::chrono::steady_clock::now() + timeout;
                std::vector<std::uint8_t> buffer(2048);
                while (std::chrono::steady_clock::now() < deadline) {
                        std::string error;
                        Datagram datagram = ReceiveDatagram(socket_, buffer.data(), buffer.size(), error);
                        if (datagram.size == 0) {
                                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                                continue;
                        }

                        Header header{};
                        std::span<const std::uint8_t> body;
                        Require(Header::SplitPacket(std::span<const std::uint8_t>(buffer.data(), datagram.size), header, body), "received invalid packet");
                        if (header.msg_type == MsgType::Kcp) {
                                Require(ikcp_input(kcp_, reinterpret_cast<const char *>(body.data()), static_cast<long>(body.size())) == 0,
                                        "ikcp_input failed");
                                ikcp_update(kcp_, static_cast<IUINT32>(NowMs()));
                                continue;
                        }

                        UdpMessage message{};
                        message.packet_seq = header.packet_seq;
                        message.payload.assign(body.begin(), body.end());
                        return message;
                }

                throw std::runtime_error("timed out waiting for udp message");
        }

      private:
        static int Output(const char *buf, int len, ikcpcb *, void *user) {
                auto *self = static_cast<TestKcpClient *>(user);
                Header header{};
                header.msg_type = MsgType::Kcp;
                header.flags = self->output_flags_;
                header.body_len = static_cast<std::uint16_t>(len);
                header.sess_id = self->sess_id_;
                header.packet_seq = 0;

                std::vector<std::uint8_t> packet(Header::kSize + static_cast<std::size_t>(len));
                Require(header.EncodeTo(packet), "kcp header encode failed");
                std::copy(reinterpret_cast<const std::uint8_t *>(buf), reinterpret_cast<const std::uint8_t *>(buf) + len,
                          packet.begin() + static_cast<std::ptrdiff_t>(Header::kSize));
                Require(SendDatagram(self->socket_, self->server_endpoint_, packet.data(), packet.size()), "kcp send failed");
                return 0;
        }

        void SendKcpInternal(std::vector<std::uint8_t> payload, HeaderFlags flags) {
                output_flags_ = flags;
                Require(ikcp_send(kcp_, reinterpret_cast<const char *>(payload.data()), static_cast<int>(payload.size())) >= 0, "ikcp_send failed");
                ikcp_update(kcp_, static_cast<IUINT32>(NowMs()));
                output_flags_ = HeaderFlags::None;
        }

        bool OpenSocket() {
                sockaddr_in local{};
                if (!ParseListenAddress("127.0.0.1:0", local)) { return false; }
                std::string error;
                if (!OpenUdpSocket(local, socket_, error)) { return false; }

                sockaddr_in server_addr{};
                if (!ParseListenAddress(server_addr_, server_addr)) { return false; }
                std::memcpy(&server_endpoint_.storage, &server_addr, sizeof(server_addr));
                server_endpoint_.length = sizeof(server_addr);
                return true;
        }

        std::string server_addr_;
        std::uint32_t sess_id_{0};
        SocketHandle socket_{kInvalidSocket};
        Endpoint server_endpoint_{};
        ikcpcb *kcp_{nullptr};
        HeaderFlags output_flags_{HeaderFlags::None};
        std::vector<std::uint8_t> recv_scratch_;
};

} // namespace ukcp::test
