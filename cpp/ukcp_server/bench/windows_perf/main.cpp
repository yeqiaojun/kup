#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "ikcp.h"
#include "platform_socket.hpp"
#include "ukcp/handler.hpp"
#include "ukcp/protocol.hpp"
#include "ukcp/server.hpp"
#include "ukcp/session.hpp"

namespace {

using namespace std::chrono_literals;

std::uint32_t ReadUint32LE(const std::uint8_t *buf) {
        return static_cast<std::uint32_t>(buf[0]) | (static_cast<std::uint32_t>(buf[1]) << 8) | (static_cast<std::uint32_t>(buf[2]) << 16) |
               (static_cast<std::uint32_t>(buf[3]) << 24);
}

std::uint64_t ReadUint64LE(const std::uint8_t *buf) {
        return static_cast<std::uint64_t>(buf[0]) | (static_cast<std::uint64_t>(buf[1]) << 8) | (static_cast<std::uint64_t>(buf[2]) << 16) |
               (static_cast<std::uint64_t>(buf[3]) << 24) | (static_cast<std::uint64_t>(buf[4]) << 32) | (static_cast<std::uint64_t>(buf[5]) << 40) |
               (static_cast<std::uint64_t>(buf[6]) << 48) | (static_cast<std::uint64_t>(buf[7]) << 56);
}

void WriteUint32LE(std::uint8_t *buf, std::uint32_t value) {
        buf[0] = static_cast<std::uint8_t>(value);
        buf[1] = static_cast<std::uint8_t>(value >> 8);
        buf[2] = static_cast<std::uint8_t>(value >> 16);
        buf[3] = static_cast<std::uint8_t>(value >> 24);
}

void WriteUint64LE(std::uint8_t *buf, std::uint64_t value) {
        buf[0] = static_cast<std::uint8_t>(value);
        buf[1] = static_cast<std::uint8_t>(value >> 8);
        buf[2] = static_cast<std::uint8_t>(value >> 16);
        buf[3] = static_cast<std::uint8_t>(value >> 24);
        buf[4] = static_cast<std::uint8_t>(value >> 32);
        buf[5] = static_cast<std::uint8_t>(value >> 40);
        buf[6] = static_cast<std::uint8_t>(value >> 48);
        buf[7] = static_cast<std::uint8_t>(value >> 56);
}

std::uint64_t NowUs() {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

struct BenchConfig {
        std::string listen_addr{"127.0.0.1:39210"};
        std::string mode{"saturating"};
        int clients{64};
        int seconds{5};
        int client_workers{4};
        int kcp_payload_bytes{24};
        int udp_payload_bytes{16};
        int udp_repeat{3};
        int max_outstanding_kcp{32};
        int broadcast_count{1000};
        int broadcast_payload_bytes{32};
        int uplink_pps{8};
        int downlink_pps{15};
        int uplink_payload_bytes{10};
        int downlink_payload_bytes{15};
};

struct PhaseReport {
        double seconds{0.0};
        std::uint64_t sent_kcp{0};
        std::uint64_t sent_udp{0};
        std::uint64_t recv_kcp_echo{0};
        std::uint64_t recv_udp_echo{0};
        std::uint64_t recv_broadcast{0};
        ukcp::ServerStatsSnapshot server_start{};
        ukcp::ServerStatsSnapshot server_end{};
        std::vector<std::uint64_t> rtt_us;
};

struct FixedRateReport {
        double seconds{0.0};
        std::uint64_t expected_uplink{0};
        std::uint64_t sent_uplink{0};
        std::uint64_t server_received_uplink{0};
        std::uint64_t expected_downlink{0};
        std::uint64_t server_sent_downlink_ticks{0};
        std::uint64_t received_downlink{0};
        ukcp::ServerStatsSnapshot server_start{};
        ukcp::ServerStatsSnapshot server_end{};
};

struct Percentiles {
        double p50_ms{0.0};
        double p95_ms{0.0};
        double p99_ms{0.0};
        double max_ms{0.0};
};

Percentiles ComputePercentiles(std::vector<std::uint64_t> samples) {
        Percentiles out{};
        if (samples.empty()) { return out; }

        std::sort(samples.begin(), samples.end());
        const auto at = [&](double q) -> double {
                const std::size_t index =
                        static_cast<std::size_t>(std::clamp(q * static_cast<double>(samples.size() - 1), 0.0, static_cast<double>(samples.size() - 1)));
                return static_cast<double>(samples[index]) / 1000.0;
        };
        out.p50_ms = at(0.50);
        out.p95_ms = at(0.95);
        out.p99_ms = at(0.99);
        out.max_ms = static_cast<double>(samples.back()) / 1000.0;
        return out;
}

class EchoHandler final : public ukcp::Handler {
      public:
        bool Auth(std::uint32_t, const std::string &, std::span<const std::uint8_t> payload) override {
                return std::string(payload.begin(), payload.end()) == "auth";
        }

        void OnUDP(ukcp::Session &session, std::uint32_t, std::span<const std::uint8_t> payload) override { session.Send(payload); }

        void OnKCP(ukcp::Session &session, std::span<const std::uint8_t> payload) override {
                if (!payload.empty() && payload[0] == static_cast<std::uint8_t>('B')) { return; }
                session.Send(payload);
        }
};

class FixedRateHandler final : public ukcp::Handler {
      public:
        bool Auth(std::uint32_t, const std::string &, std::span<const std::uint8_t> payload) override {
                return std::string(payload.begin(), payload.end()) == "auth";
        }

        void OnKCP(ukcp::Session &, std::span<const std::uint8_t> payload) override {
                if (!payload.empty() && payload[0] == static_cast<std::uint8_t>('S')) { received_uplink.fetch_add(1, std::memory_order_relaxed); }
        }

        std::atomic<std::uint64_t> received_uplink{0};
};

class BenchClient {
      public:
        BenchClient(std::string server_addr, std::uint32_t sess_id, const BenchConfig &config)
            : server_addr_(std::move(server_addr)), sess_id_(sess_id), config_(config) {
                if (!OpenSocket()) { throw std::runtime_error("failed to open bench socket"); }
                kcp_ = ikcp_create(sess_id_, this);
                if (kcp_ == nullptr) { throw std::runtime_error("failed to create bench kcp"); }
                kcp_->output = &Output;
                ikcp_nodelay(kcp_, 1, 10, 2, 1);
                ikcp_wndsize(kcp_, 128, 128);
                ikcp_setmtu(kcp_, 1200);
        }

        ~BenchClient() {
                if (kcp_ != nullptr) {
                        ikcp_release(kcp_);
                        kcp_ = nullptr;
                }
                ukcp::CloseSocket(socket_);
        }

        BenchClient(const BenchClient &) = delete;
        BenchClient &operator=(const BenchClient &) = delete;

        void SendAuth() {
                const std::string auth = "auth";
                SendKcpInternal(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(auth.data()), auth.size()), ukcp::HeaderFlags::Connect);
        }

        void RunEchoPhase(PhaseReport &report, std::chrono::steady_clock::time_point deadline) {
                std::uint32_t seq = 1;
                while (std::chrono::steady_clock::now() < deadline) {
                        if (outstanding_kcp_ >= config_.max_outstanding_kcp) {
                                Pump(1ms, report);
                                continue;
                        }

                        SendKcpMessage(seq, report);
                        for (int i = 0; i < config_.udp_repeat; ++i) { SendUdpMessage(seq, report); }
                        Pump(0ms, report);
                        ++seq;
                }

                const auto drain_deadline = std::chrono::steady_clock::now() + 500ms;
                while (std::chrono::steady_clock::now() < drain_deadline && outstanding_kcp_ > 0) { Pump(1ms, report); }
        }

        void RunBroadcastPhase(PhaseReport &report, std::uint64_t expected_messages, std::chrono::steady_clock::time_point deadline) {
                while (report.recv_broadcast < expected_messages && std::chrono::steady_clock::now() < deadline) { Pump(1ms, report); }
        }

        void SendFixedRateUplink(std::uint32_t seq, FixedRateReport &report) {
                std::vector<std::uint8_t> payload(static_cast<std::size_t>(config_.uplink_payload_bytes), 0);
                payload[0] = static_cast<std::uint8_t>('S');
                WriteUint32LE(payload.data() + 1, seq);
                SendKcpInternal(payload, ukcp::HeaderFlags::None);
                ++report.sent_uplink;
        }

        void PumpFixedRate(FixedRateReport &report, std::chrono::milliseconds idle_sleep) {
                bool received_any = false;
                while (true) {
                        std::string error;
                        ukcp::Datagram datagram = ukcp::ReceiveDatagram(socket_, recv_buffer_.data(), recv_buffer_.size(), error);
                        if (!error.empty()) { throw std::runtime_error(error); }
                        if (datagram.size == 0) { break; }

                        received_any = true;
                        ukcp::Header header{};
                        std::span<const std::uint8_t> body;
                        if (!ukcp::Header::SplitPacket(std::span<const std::uint8_t>(recv_buffer_.data(), datagram.size), header, body)) { continue; }
                        if (header.msg_type != ukcp::MsgType::Kcp) { continue; }
                        if (ikcp_input(kcp_, reinterpret_cast<const char *>(body.data()), static_cast<long>(body.size())) != 0) { continue; }
                        ikcp_update(kcp_, static_cast<IUINT32>(ukcp::NowMs()));
                        DrainFixedRateMessages(report);
                }

                if (!received_any && idle_sleep.count() > 0) { std::this_thread::sleep_for(idle_sleep); }
        }

      private:
        static int Output(const char *buf, int len, ikcpcb *, void *user) {
                auto *self = static_cast<BenchClient *>(user);
                ukcp::Header header{};
                header.msg_type = ukcp::MsgType::Kcp;
                header.flags = self->output_flags_;
                header.body_len = static_cast<std::uint16_t>(len);
                header.sess_id = self->sess_id_;
                header.packet_seq = 0;

                std::vector<std::uint8_t> packet(ukcp::Header::kSize + static_cast<std::size_t>(len));
                if (!header.EncodeTo(packet)) { return -1; }
                std::copy(reinterpret_cast<const std::uint8_t *>(buf), reinterpret_cast<const std::uint8_t *>(buf) + len,
                          packet.begin() + static_cast<std::ptrdiff_t>(ukcp::Header::kSize));
                return ukcp::SendDatagram(self->socket_, self->server_endpoint_, packet.data(), packet.size()) ? 0 : -1;
        }

        bool OpenSocket() {
                sockaddr_in local{};
                if (!ukcp::ParseListenAddress("127.0.0.1:0", local)) { return false; }
                std::string error;
                if (!ukcp::OpenUdpSocket(local, socket_, error)) { return false; }

                sockaddr_in server_addr{};
                if (!ukcp::ParseListenAddress(server_addr_, server_addr)) { return false; }
                std::memcpy(&server_endpoint_.storage, &server_addr, sizeof(server_addr));
                server_endpoint_.length = sizeof(server_addr);
                return true;
        }

        void SendKcpInternal(std::span<const std::uint8_t> payload, ukcp::HeaderFlags flags) {
                output_flags_ = flags;
                const int rc = ikcp_send(kcp_, reinterpret_cast<const char *>(payload.data()), static_cast<int>(payload.size()));
                if (rc < 0) { throw std::runtime_error("bench ikcp_send failed"); }
                ikcp_update(kcp_, static_cast<IUINT32>(ukcp::NowMs()));
                output_flags_ = ukcp::HeaderFlags::None;
        }

        void SendKcpMessage(std::uint32_t seq, PhaseReport &report) {
                std::vector<std::uint8_t> payload(static_cast<std::size_t>(config_.kcp_payload_bytes), 0);
                payload[0] = static_cast<std::uint8_t>('K');
                WriteUint32LE(payload.data() + 1, seq);
                WriteUint64LE(payload.data() + 5, NowUs());
                SendKcpInternal(payload, ukcp::HeaderFlags::None);
                ++outstanding_kcp_;
                ++report.sent_kcp;
        }

        void SendUdpMessage(std::uint32_t seq, PhaseReport &report) {
                std::vector<std::uint8_t> payload(static_cast<std::size_t>(config_.udp_payload_bytes), 0);
                payload[0] = static_cast<std::uint8_t>('U');
                WriteUint32LE(payload.data() + 1, seq);
                WriteUint32LE(payload.data() + 5, sess_id_);

                ukcp::Header header{};
                header.msg_type = ukcp::MsgType::Udp;
                header.flags = ukcp::HeaderFlags::None;
                header.body_len = static_cast<std::uint16_t>(payload.size());
                header.sess_id = sess_id_;
                header.packet_seq = seq;

                std::vector<std::uint8_t> packet(ukcp::Header::kSize + payload.size());
                if (!header.EncodeTo(packet)) { throw std::runtime_error("bench udp header encode failed"); }
                std::copy(payload.begin(), payload.end(), packet.begin() + static_cast<std::ptrdiff_t>(ukcp::Header::kSize));
                if (!ukcp::SendDatagram(socket_, server_endpoint_, packet.data(), packet.size())) { throw std::runtime_error("bench udp send failed"); }
                ++report.sent_udp;
        }

        void Pump(std::chrono::milliseconds idle_sleep, PhaseReport &report) {
                bool received_any = false;
                while (true) {
                        std::string error;
                        ukcp::Datagram datagram = ukcp::ReceiveDatagram(socket_, recv_buffer_.data(), recv_buffer_.size(), error);
                        if (!error.empty()) { throw std::runtime_error(error); }
                        if (datagram.size == 0) { break; }

                        received_any = true;
                        ukcp::Header header{};
                        std::span<const std::uint8_t> body;
                        if (!ukcp::Header::SplitPacket(std::span<const std::uint8_t>(recv_buffer_.data(), datagram.size), header, body)) { continue; }
                        if (header.msg_type != ukcp::MsgType::Kcp) { continue; }

                        if (ikcp_input(kcp_, reinterpret_cast<const char *>(body.data()), static_cast<long>(body.size())) != 0) { continue; }
                        ikcp_update(kcp_, static_cast<IUINT32>(ukcp::NowMs()));
                        DrainMessages(report);
                }

                if (!received_any && idle_sleep.count() > 0) { std::this_thread::sleep_for(idle_sleep); }
        }

        void DrainMessages(PhaseReport &report) {
                while (true) {
                        const int size = ikcp_peeksize(kcp_);
                        if (size < 0) { return; }
                        recv_scratch_.resize(static_cast<std::size_t>(size));
                        const int rc = ikcp_recv(kcp_, reinterpret_cast<char *>(recv_scratch_.data()), size);
                        if (rc < 0) { return; }

                        const auto *data = recv_scratch_.data();
                        if (rc <= 0) { continue; }

                        switch (static_cast<char>(data[0])) {
                        case 'K':
                                if (rc >= 13) {
                                        const std::uint64_t sent_us = ReadUint64LE(data + 5);
                                        const std::uint64_t now_us = NowUs();
                                        report.rtt_us.push_back(now_us >= sent_us ? now_us - sent_us : 0);
                                }
                                if (outstanding_kcp_ > 0) { --outstanding_kcp_; }
                                ++report.recv_kcp_echo;
                                break;
                        case 'U': ++report.recv_udp_echo; break;
                        case 'B': ++report.recv_broadcast; break;
                        default: break;
                        }
                }
        }

        void DrainFixedRateMessages(FixedRateReport &report) {
                while (true) {
                        const int size = ikcp_peeksize(kcp_);
                        if (size < 0) { return; }
                        recv_scratch_.resize(static_cast<std::size_t>(size));
                        const int rc = ikcp_recv(kcp_, reinterpret_cast<char *>(recv_scratch_.data()), size);
                        if (rc < 0) { return; }
                        if (rc <= 0) { continue; }
                        if (recv_scratch_[0] == static_cast<std::uint8_t>('D')) { ++report.received_downlink; }
                }
        }

        std::string server_addr_;
        std::uint32_t sess_id_{0};
        const BenchConfig &config_;
        SocketHandle socket_{kInvalidSocket};
        ukcp::Endpoint server_endpoint_{};
        ikcpcb *kcp_{nullptr};
        ukcp::HeaderFlags output_flags_{ukcp::HeaderFlags::None};
        std::vector<std::uint8_t> recv_buffer_{std::vector<std::uint8_t>(4096)};
        std::vector<std::uint8_t> recv_scratch_{};
        std::size_t outstanding_kcp_{0};
};

BenchConfig ParseArgs(int argc, char **argv) {
        BenchConfig config{};
        for (int i = 1; i < argc; ++i) {
                const std::string_view arg(argv[i]);
                auto next = [&](int &index) -> const char * {
                        if (index + 1 >= argc) { throw std::runtime_error("missing value for argument"); }
                        return argv[++index];
                };

                if (arg == "--listen") {
                        config.listen_addr = next(i);
                } else if (arg == "--mode") {
                        config.mode = next(i);
                } else if (arg == "--clients") {
                        config.clients = std::stoi(next(i));
                } else if (arg == "--client-workers") {
                        config.client_workers = std::stoi(next(i));
                } else if (arg == "--seconds") {
                        config.seconds = std::stoi(next(i));
                } else if (arg == "--kcp-bytes") {
                        config.kcp_payload_bytes = std::stoi(next(i));
                } else if (arg == "--udp-bytes") {
                        config.udp_payload_bytes = std::stoi(next(i));
                } else if (arg == "--udp-repeat") {
                        config.udp_repeat = std::stoi(next(i));
                } else if (arg == "--broadcast-count") {
                        config.broadcast_count = std::stoi(next(i));
                } else if (arg == "--broadcast-bytes") {
                        config.broadcast_payload_bytes = std::stoi(next(i));
                } else if (arg == "--max-outstanding-kcp") {
                        config.max_outstanding_kcp = std::stoi(next(i));
                } else if (arg == "--uplink-pps") {
                        config.uplink_pps = std::stoi(next(i));
                } else if (arg == "--downlink-pps") {
                        config.downlink_pps = std::stoi(next(i));
                } else if (arg == "--uplink-payload-bytes") {
                        config.uplink_payload_bytes = std::stoi(next(i));
                } else if (arg == "--downlink-payload-bytes") {
                        config.downlink_payload_bytes = std::stoi(next(i));
                } else {
                        throw std::runtime_error("unknown argument: " + std::string(arg));
                }
        }

        config.clients = (std::max)(config.clients, 1);
        config.client_workers = (std::max)(config.client_workers, 1);
        config.seconds = (std::max)(config.seconds, 1);
        config.kcp_payload_bytes = (std::max)(config.kcp_payload_bytes, 16);
        config.udp_payload_bytes = (std::max)(config.udp_payload_bytes, 12);
        config.udp_repeat = (std::max)(config.udp_repeat, 0);
        config.broadcast_count = (std::max)(config.broadcast_count, 1);
        config.broadcast_payload_bytes = (std::max)(config.broadcast_payload_bytes, 8);
        config.max_outstanding_kcp = (std::max)(config.max_outstanding_kcp, 1);
        config.uplink_pps = (std::max)(config.uplink_pps, 1);
        config.downlink_pps = (std::max)(config.downlink_pps, 1);
        config.uplink_payload_bytes = (std::max)(config.uplink_payload_bytes, 8);
        config.downlink_payload_bytes = (std::max)(config.downlink_payload_bytes, 8);
        return config;
}

void WaitForAllSessions(ukcp::Server &server, std::vector<std::unique_ptr<BenchClient>> &clients) {
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (std::chrono::steady_clock::now() < deadline) {
                if (server.Stats().active_sessions == clients.size()) { return; }
                for (auto &client : clients) {
                        PhaseReport throwaway{};
                        client->RunBroadcastPhase(throwaway, 0, std::chrono::steady_clock::now() + 5ms);
                }
                std::this_thread::sleep_for(10ms);
        }
        throw std::runtime_error("timed out waiting for authenticated sessions");
}

void PrintRateLine(std::string_view label, std::uint64_t packets, std::uint64_t bytes, double seconds) {
        const double pps = seconds > 0.0 ? static_cast<double>(packets) / seconds : 0.0;
        const double mbps = seconds > 0.0 ? (static_cast<double>(bytes) * 8.0 / 1'000'000.0) / seconds : 0.0;
        std::cout << "  " << label << ": packets=" << packets << " bytes=" << bytes << " pps=" << static_cast<std::uint64_t>(pps) << " mbps=" << std::fixed
                  << std::setprecision(2) << mbps << '\n';
}

void PrintEchoSummary(const PhaseReport &report) {
        const auto rtt = ComputePercentiles(report.rtt_us);
        const auto recv_packets = report.server_end.recv_packets - report.server_start.recv_packets;
        const auto recv_bytes = report.server_end.recv_bytes - report.server_start.recv_bytes;
        const auto sent_packets = report.server_end.sent_packets - report.server_start.sent_packets;
        const auto sent_bytes = report.server_end.sent_bytes - report.server_start.sent_bytes;

        std::cout << "\n[ECHO PHASE]\n";
        std::cout << "  duration_s=" << std::fixed << std::setprecision(2) << report.seconds << " sent_kcp=" << report.sent_kcp
                  << " sent_udp=" << report.sent_udp << " recv_kcp_echo=" << report.recv_kcp_echo << " recv_udp_echo=" << report.recv_udp_echo << '\n';
        PrintRateLine("server ingress", recv_packets, recv_bytes, report.seconds);
        PrintRateLine("server egress", sent_packets, sent_bytes, report.seconds);
        std::cout << "  rtt_ms: p50=" << std::fixed << std::setprecision(3) << rtt.p50_ms << " p95=" << rtt.p95_ms << " p99=" << rtt.p99_ms
                  << " max=" << rtt.max_ms << " samples=" << report.rtt_us.size() << '\n';
}

void PrintBroadcastSummary(const PhaseReport &report, std::uint64_t expected) {
        const auto sent_packets = report.server_end.sent_packets - report.server_start.sent_packets;
        const auto sent_bytes = report.server_end.sent_bytes - report.server_start.sent_bytes;
        std::cout << "\n[BROADCAST PHASE]\n";
        std::cout << "  duration_s=" << std::fixed << std::setprecision(2) << report.seconds << " recv_broadcast=" << report.recv_broadcast
                  << " expected=" << expected << '\n';
        PrintRateLine("server broadcast", sent_packets, sent_bytes, report.seconds);
}

void MergeInto(PhaseReport &dst, PhaseReport &&src) {
        dst.sent_kcp += src.sent_kcp;
        dst.sent_udp += src.sent_udp;
        dst.recv_kcp_echo += src.recv_kcp_echo;
        dst.recv_udp_echo += src.recv_udp_echo;
        dst.recv_broadcast += src.recv_broadcast;
        if (!src.rtt_us.empty()) { dst.rtt_us.insert(dst.rtt_us.end(), src.rtt_us.begin(), src.rtt_us.end()); }
}

void MergeInto(FixedRateReport &dst, FixedRateReport &&src) {
        dst.sent_uplink += src.sent_uplink;
        dst.received_downlink += src.received_downlink;
}

void PrintFixedRateSummary(const FixedRateReport &report) {
        const auto recv_packets = report.server_end.recv_packets - report.server_start.recv_packets;
        const auto recv_bytes = report.server_end.recv_bytes - report.server_start.recv_bytes;
        const auto sent_packets = report.server_end.sent_packets - report.server_start.sent_packets;
        const auto sent_bytes = report.server_end.sent_bytes - report.server_start.sent_bytes;

        std::cout << "\n[FIXED RATE SCENARIO]\n";
        std::cout << "  duration_s=" << std::fixed << std::setprecision(2) << report.seconds << " expected_uplink=" << report.expected_uplink
                  << " sent_uplink=" << report.sent_uplink << " server_received_uplink=" << report.server_received_uplink << '\n';
        std::cout << "  expected_downlink=" << report.expected_downlink << " server_sent_ticks=" << report.server_sent_downlink_ticks
                  << " received_downlink=" << report.received_downlink << '\n';
        PrintRateLine("server ingress", recv_packets, recv_bytes, report.seconds);
        PrintRateLine("server egress", sent_packets, sent_bytes, report.seconds);

        const double uplink_delivery =
                report.expected_uplink > 0 ? static_cast<double>(report.server_received_uplink) * 100.0 / static_cast<double>(report.expected_uplink) : 0.0;
        const double downlink_delivery =
                report.expected_downlink > 0 ? static_cast<double>(report.received_downlink) * 100.0 / static_cast<double>(report.expected_downlink) : 0.0;
        std::cout << "  uplink_delivery_pct=" << std::fixed << std::setprecision(2) << uplink_delivery << " downlink_delivery_pct=" << downlink_delivery << '\n';
}

int RunFixedRateScenario(const BenchConfig &config) {
        FixedRateHandler handler;
        ukcp::Server server(config.listen_addr, handler, ukcp::Config{});
        if (!server.Start()) {
                std::cerr << "failed to start server\n";
                return 1;
        }

        std::vector<std::unique_ptr<BenchClient>> clients;
        clients.reserve(static_cast<std::size_t>(config.clients));
        for (int i = 0; i < config.clients; ++i) {
                auto client = std::make_unique<BenchClient>(config.listen_addr, static_cast<std::uint32_t>(10000 + i), config);
                client->SendAuth();
                clients.push_back(std::move(client));
        }
        WaitForAllSessions(server, clients);

        FixedRateReport report{};
        report.server_start = server.Stats();
        report.expected_uplink =
                static_cast<std::uint64_t>(config.clients) * static_cast<std::uint64_t>(config.uplink_pps) * static_cast<std::uint64_t>(config.seconds);
        report.expected_downlink =
                static_cast<std::uint64_t>(config.clients) * static_cast<std::uint64_t>(config.downlink_pps) * static_cast<std::uint64_t>(config.seconds);

        const auto start = std::chrono::steady_clock::now();
        const auto deadline = start + std::chrono::seconds(config.seconds);
        const auto uplink_interval = std::chrono::microseconds(1'000'000 / config.uplink_pps);
        const auto downlink_interval = std::chrono::microseconds(1'000'000 / config.downlink_pps);

        std::vector<std::thread> workers;
        std::vector<FixedRateReport> worker_reports(static_cast<std::size_t>(config.client_workers));
        workers.reserve(static_cast<std::size_t>(config.client_workers));
        for (int worker = 0; worker < config.client_workers; ++worker) {
                workers.emplace_back([&, worker] {
                        const std::size_t begin = static_cast<std::size_t>(worker) * clients.size() / config.client_workers;
                        const std::size_t end = static_cast<std::size_t>(worker + 1) * clients.size() / config.client_workers;
                        std::vector<std::uint32_t> seqs(end - begin, 1);
                        std::vector<std::uint64_t> sent_counts(end - begin, 0);
                        std::vector<std::chrono::steady_clock::time_point> next_due(end - begin, start);

                        auto &worker_report = worker_reports[static_cast<std::size_t>(worker)];
                        while (std::chrono::steady_clock::now() < deadline) {
                                const auto now = std::chrono::steady_clock::now();
                                for (std::size_t i = begin; i < end; ++i) {
                                        const std::size_t local = i - begin;
                                        while (sent_counts[local] < static_cast<std::uint64_t>(config.uplink_pps * config.seconds) && next_due[local] <= now) {
                                                clients[i]->SendFixedRateUplink(seqs[local]++, worker_report);
                                                ++sent_counts[local];
                                                next_due[local] += uplink_interval;
                                        }
                                        clients[i]->PumpFixedRate(worker_report, 0ms);
                                }
                                std::this_thread::sleep_for(1ms);
                        }

                        const auto drain_deadline = std::chrono::steady_clock::now() + 3s;
                        while (std::chrono::steady_clock::now() < drain_deadline) {
                                bool made_progress = false;
                                for (std::size_t i = begin; i < end; ++i) {
                                        const auto before = worker_report.received_downlink;
                                        clients[i]->PumpFixedRate(worker_report, 0ms);
                                        if (worker_report.received_downlink != before) { made_progress = true; }
                                }
                                if (!made_progress) { std::this_thread::sleep_for(1ms); }
                        }
                });
        }

        std::atomic<std::uint64_t> server_ticks{0};
        std::thread downlink_thread([&] {
                std::vector<std::uint8_t> payload(static_cast<std::size_t>(config.downlink_payload_bytes), 0);
                payload[0] = static_cast<std::uint8_t>('D');
                auto next_due = start;
                while (std::chrono::steady_clock::now() < deadline) {
                        const auto now = std::chrono::steady_clock::now();
                        if (now < next_due) {
                                std::this_thread::sleep_for(1ms);
                                continue;
                        }
                        const std::uint32_t tick = static_cast<std::uint32_t>(server_ticks.fetch_add(1, std::memory_order_relaxed) + 1);
                        WriteUint32LE(payload.data() + 1, tick);
                        server.SendToAll(payload);
                        next_due += downlink_interval;
                }
        });

        for (auto &worker : workers) { worker.join(); }
        downlink_thread.join();

        for (auto &worker_report : worker_reports) { MergeInto(report, std::move(worker_report)); }
        report.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        report.server_received_uplink = handler.received_uplink.load(std::memory_order_relaxed);
        report.server_sent_downlink_ticks = server_ticks.load(std::memory_order_relaxed);
        report.server_end = server.Stats();
        PrintFixedRateSummary(report);

        std::cout << "\n[SERVER STATS]\n";
        const auto final_stats = server.Stats();
        std::cout << "  active_sessions=" << final_stats.active_sessions << " recv_packets=" << final_stats.recv_packets
                  << " sent_packets=" << final_stats.sent_packets << '\n';
        return 0;
}

} // namespace

int main(int argc, char **argv) {
        try {
                const BenchConfig config = ParseArgs(argc, argv);
                if (config.mode == "fixedrate") { return RunFixedRateScenario(config); }
                if (config.mode != "saturating") {
                        std::cerr << "unknown mode: " << config.mode << '\n';
                        return 1;
                }
                EchoHandler handler;
                ukcp::Server server(config.listen_addr, handler, ukcp::Config{});
                if (!server.Start()) {
                        std::cerr << "failed to start server\n";
                        return 1;
                }

                std::vector<std::unique_ptr<BenchClient>> clients;
                clients.reserve(static_cast<std::size_t>(config.clients));
                for (int i = 0; i < config.clients; ++i) {
                        auto client = std::make_unique<BenchClient>(config.listen_addr, static_cast<std::uint32_t>(10000 + i), config);
                        client->SendAuth();
                        clients.push_back(std::move(client));
                }
                WaitForAllSessions(server, clients);

                PhaseReport echo_report{};
                echo_report.server_start = server.Stats();
                const auto echo_start = std::chrono::steady_clock::now();
                const auto echo_deadline = echo_start + std::chrono::seconds(config.seconds);
                std::vector<std::thread> threads;
                std::vector<PhaseReport> echo_client_reports(clients.size());
                threads.reserve(clients.size());
                for (std::size_t i = 0; i < clients.size(); ++i) {
                        threads.emplace_back([&client = clients[i], &report = echo_client_reports[i], echo_deadline] {
                                client->RunEchoPhase(report, echo_deadline);
                        });
                }
                for (auto &thread : threads) { thread.join(); }
                for (auto &report : echo_client_reports) { MergeInto(echo_report, std::move(report)); }
                echo_report.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - echo_start).count();
                echo_report.server_end = server.Stats();
                PrintEchoSummary(echo_report);

                PhaseReport broadcast_report{};
                broadcast_report.server_start = server.Stats();
                const auto broadcast_start = std::chrono::steady_clock::now();
                const std::uint64_t expected_broadcast = static_cast<std::uint64_t>(config.clients) * static_cast<std::uint64_t>(config.broadcast_count);
                std::vector<std::thread> broadcast_threads;
                std::vector<PhaseReport> broadcast_client_reports(clients.size());
                broadcast_threads.reserve(clients.size());
                const auto broadcast_wait_seconds = (std::max)(5, config.broadcast_count / 100);
                const auto broadcast_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(broadcast_wait_seconds);
                for (std::size_t i = 0; i < clients.size(); ++i) {
                        broadcast_threads.emplace_back([&client = clients[i], &report = broadcast_client_reports[i], &config, broadcast_deadline] {
                                client->RunBroadcastPhase(report, static_cast<std::uint64_t>(config.broadcast_count), broadcast_deadline);
                        });
                }

                std::vector<std::uint8_t> broadcast_payload(static_cast<std::size_t>(config.broadcast_payload_bytes), 0);
                broadcast_payload[0] = static_cast<std::uint8_t>('B');
                for (int i = 0; i < config.broadcast_count; ++i) {
                        WriteUint32LE(broadcast_payload.data() + 1, static_cast<std::uint32_t>(i));
                        const auto report = server.SendToAll(broadcast_payload);
                        if (report.failed != 0) {
                                std::cerr << "broadcast send failed: failed=" << report.failed << '\n';
                                return 1;
                        }
                }
                for (auto &thread : broadcast_threads) { thread.join(); }
                for (auto &report : broadcast_client_reports) { MergeInto(broadcast_report, std::move(report)); }
                broadcast_report.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - broadcast_start).count();
                broadcast_report.server_end = server.Stats();
                PrintBroadcastSummary(broadcast_report, expected_broadcast);

                std::cout << "\n[SERVER STATS]\n";
                const auto final_stats = server.Stats();
                std::cout << "  active_sessions=" << final_stats.active_sessions << " recv_packets=" << final_stats.recv_packets
                          << " sent_packets=" << final_stats.sent_packets << '\n';
                if (broadcast_report.recv_broadcast != expected_broadcast) { std::cout << "  warning=broadcast_not_fully_drained\n"; }
                return 0;
        } catch (const std::exception &ex) {
                std::cerr << ex.what() << '\n';
                return 1;
        }
}
