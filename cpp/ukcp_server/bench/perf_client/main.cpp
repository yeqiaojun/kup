#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <sys/epoll.h>
#endif

#include "ikcp.h"
#include "platform_socket.hpp"
#include "quiescence.hpp"
#include "ukcp/protocol.hpp"

namespace {

using namespace std::chrono_literals;

enum class Mode {
        Fixed,
        Room,
};

struct Config {
        Mode mode{Mode::Fixed};
        std::string server{"127.0.0.1:39220"};
        int clients{5000};
        int client_workers{4};
        int seconds{10};
        int auth_warmup_ms{5000};
        int drain_ms{8000};
        int uplink_pps{8};
        int downlink_pps{15};
        int uplink_payload_bytes{10};
        int downlink_payload_bytes{15};
        int room_size{10};
        int room_fps{15};
        int kcp_pps{3};
        int udp_pps{5};
};

struct ScheduledEvent {
        std::uint32_t offset_ms{0};
        bool is_udp{false};
        std::uint32_t seq{0};
};

void WriteUint32LE(std::uint8_t *buf, std::uint32_t value) {
        buf[0] = static_cast<std::uint8_t>(value);
        buf[1] = static_cast<std::uint8_t>(value >> 8);
        buf[2] = static_cast<std::uint8_t>(value >> 16);
        buf[3] = static_cast<std::uint8_t>(value >> 24);
}

Mode ParseMode(std::string_view value) {
        if (value == "fixed") { return Mode::Fixed; }
        if (value == "room") { return Mode::Room; }
        throw std::runtime_error("unknown mode: " + std::string(value));
}

Config ParseArgs(int argc, char **argv) {
        Config config{};
        for (int i = 1; i < argc; ++i) {
                const std::string_view arg(argv[i]);
                auto next = [&](int &index) -> const char * {
                        if (index + 1 >= argc) { throw std::runtime_error("missing value for argument"); }
                        return argv[++index];
                };

                if (arg == "--mode") {
                        config.mode = ParseMode(next(i));
                } else if (arg == "--server") {
                        config.server = next(i);
                } else if (arg == "--clients") {
                        config.clients = std::stoi(next(i));
                } else if (arg == "--client-workers") {
                        config.client_workers = std::stoi(next(i));
                } else if (arg == "--seconds") {
                        config.seconds = std::stoi(next(i));
                } else if (arg == "--auth-warmup-ms") {
                        config.auth_warmup_ms = std::stoi(next(i));
                } else if (arg == "--drain-ms") {
                        config.drain_ms = std::stoi(next(i));
                } else if (arg == "--uplink-pps") {
                        config.uplink_pps = std::stoi(next(i));
                } else if (arg == "--downlink-pps") {
                        config.downlink_pps = std::stoi(next(i));
                } else if (arg == "--uplink-payload-bytes") {
                        config.uplink_payload_bytes = std::stoi(next(i));
                } else if (arg == "--downlink-payload-bytes") {
                        config.downlink_payload_bytes = std::stoi(next(i));
                } else if (arg == "--room-size") {
                        config.room_size = std::stoi(next(i));
                } else if (arg == "--room-fps") {
                        config.room_fps = std::stoi(next(i));
                } else if (arg == "--kcp-pps") {
                        config.kcp_pps = std::stoi(next(i));
                } else if (arg == "--udp-pps") {
                        config.udp_pps = std::stoi(next(i));
                } else {
                        throw std::runtime_error("unknown argument: " + std::string(arg));
                }
        }

        config.clients = (std::max)(config.clients, 1);
        config.client_workers = (std::max)(config.client_workers, 1);
        config.seconds = (std::max)(config.seconds, 1);
        config.auth_warmup_ms = (std::max)(config.auth_warmup_ms, 0);
        config.drain_ms = (std::max)(config.drain_ms, 1000);
        config.uplink_pps = (std::max)(config.uplink_pps, 1);
        config.downlink_pps = (std::max)(config.downlink_pps, 1);
        config.uplink_payload_bytes = (std::max)(config.uplink_payload_bytes, 8);
        config.downlink_payload_bytes = (std::max)(config.downlink_payload_bytes, 8);
        config.room_size = (std::max)(config.room_size, 1);
        config.room_fps = (std::max)(config.room_fps, 1);
        config.kcp_pps = (std::max)(config.kcp_pps, 1);
        config.udp_pps = (std::max)(config.udp_pps, 1);
        return config;
}

std::vector<ScheduledEvent> BuildRoomSchedule(std::uint32_t sess_id, const Config &cfg) {
        std::mt19937 rng(sess_id ^ 0x9e3779b9U);
        std::uniform_int_distribution<int> offset_dist(0, 999);
        std::vector<ScheduledEvent> events;
        events.reserve(static_cast<std::size_t>(cfg.seconds) * static_cast<std::size_t>(cfg.kcp_pps + cfg.udp_pps));

        std::uint32_t next_kcp_seq = 1;
        std::uint32_t next_udp_seq = 1;
        for (int second = 0; second < cfg.seconds; ++second) {
                const std::uint32_t base = static_cast<std::uint32_t>(second * 1000);
                for (int i = 0; i < cfg.kcp_pps; ++i) {
                        events.push_back(ScheduledEvent{
                                base + static_cast<std::uint32_t>(offset_dist(rng)),
                                false,
                                next_kcp_seq++,
                        });
                }
                for (int i = 0; i < cfg.udp_pps; ++i) {
                        events.push_back(ScheduledEvent{
                                base + static_cast<std::uint32_t>(offset_dist(rng)),
                                true,
                                next_udp_seq++,
                        });
                }
        }

        std::sort(events.begin(), events.end(), [](const ScheduledEvent &lhs, const ScheduledEvent &rhs) {
                if (lhs.offset_ms != rhs.offset_ms) { return lhs.offset_ms < rhs.offset_ms; }
                return static_cast<int>(lhs.is_udp) < static_cast<int>(rhs.is_udp);
        });
        return events;
}

class PerfClient {
      public:
        PerfClient(std::string server_addr, std::uint32_t sess_id, const Config &config)
            : server_addr_(std::move(server_addr)), sess_id_(sess_id), config_(config) {
                if (!OpenSocket()) { throw std::runtime_error("failed to open client socket"); }
                kcp_ = ikcp_create(sess_id_, this);
                if (kcp_ == nullptr) { throw std::runtime_error("failed to create client kcp"); }
                kcp_->output = &Output;
                ikcp_nodelay(kcp_, 1, 10, 2, 1);
                ikcp_wndsize(kcp_, 128, 128);
                ikcp_setmtu(kcp_, 1200);
        }

        ~PerfClient() {
                if (kcp_ != nullptr) {
                        ikcp_release(kcp_);
                        kcp_ = nullptr;
                }
                ukcp::CloseSocket(socket_);
        }

        void SendAuth() {
                const std::string auth = "auth";
                SendKcpBuffer(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(auth.data()), auth.size()), ukcp::HeaderFlags::Connect);
        }

        void SendFixedSample(std::uint32_t seq) {
                std::vector<std::uint8_t> payload(static_cast<std::size_t>(config_.uplink_payload_bytes), 0);
                payload[0] = static_cast<std::uint8_t>('S');
                WriteUint32LE(payload.data() + 1, seq);
                SendKcpBuffer(payload, ukcp::HeaderFlags::None);
                ++sent_kcp_;
        }

        void SendRoomKcp(std::uint32_t seq) {
                std::vector<std::uint8_t> payload(static_cast<std::size_t>(config_.uplink_payload_bytes), 0);
                payload[0] = static_cast<std::uint8_t>('K');
                WriteUint32LE(payload.data() + 1, seq);
                SendKcpBuffer(payload, ukcp::HeaderFlags::None);
                ++sent_kcp_;
        }

        void SendRoomUdp(std::uint32_t seq) {
                ukcp::Header header{};
                header.msg_type = ukcp::MsgType::Udp;
                header.flags = ukcp::HeaderFlags::None;
                header.body_len = static_cast<std::uint16_t>(config_.uplink_payload_bytes);
                header.sess_id = sess_id_;
                header.packet_seq = seq;

                std::vector<std::uint8_t> packet(ukcp::Header::kSize + static_cast<std::size_t>(config_.uplink_payload_bytes), 0);
                if (!header.EncodeTo(packet)) { throw std::runtime_error("udp header encode failed"); }
                packet[ukcp::Header::kSize] = static_cast<std::uint8_t>('U');
                WriteUint32LE(packet.data() + ukcp::Header::kSize + 1, seq);
                if (!ukcp::SendDatagram(socket_, server_endpoint_, packet.data(), packet.size())) { return; }
                ++sent_udp_;
        }

        void Pump(std::chrono::milliseconds idle_sleep = 0ms) {
                bool any = false;
                while (true) {
                        std::string error;
                        ukcp::Datagram datagram = ukcp::ReceiveDatagram(socket_, recv_buffer_.data(), recv_buffer_.size(), error);
                        if (!error.empty()) { throw std::runtime_error(error); }
                        if (datagram.size == 0) { break; }
                        any = true;

                        ukcp::Header header{};
                        std::span<const std::uint8_t> body;
                        if (!ukcp::Header::SplitPacket(std::span<const std::uint8_t>(recv_buffer_.data(), datagram.size), header, body)) { continue; }
                        if (header.msg_type != ukcp::MsgType::Kcp) { continue; }

                        if (ikcp_input(kcp_, reinterpret_cast<const char *>(body.data()), static_cast<long>(body.size())) != 0) { continue; }
                        ikcp_update(kcp_, static_cast<IUINT32>(ukcp::NowMs()));
                        Drain();
                }

                if (!any && idle_sleep.count() > 0) { std::this_thread::sleep_for(idle_sleep); }
        }

        std::uint64_t sent_kcp() const noexcept { return sent_kcp_; }
        std::uint64_t sent_udp() const noexcept { return sent_udp_; }
        std::uint64_t recv_downlink() const noexcept { return recv_downlink_; }
        SocketHandle socket() const noexcept { return socket_; }

      private:
        static int Output(const char *buf, int len, ikcpcb *, void *user) {
                auto *self = static_cast<PerfClient *>(user);
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

        void SendKcpBuffer(std::span<const std::uint8_t> payload, ukcp::HeaderFlags flags) {
                output_flags_ = flags;
                const int rc = ikcp_send(kcp_, reinterpret_cast<const char *>(payload.data()), static_cast<int>(payload.size()));
                if (rc < 0) { throw std::runtime_error("client ikcp_send failed"); }
                ikcp_update(kcp_, static_cast<IUINT32>(ukcp::NowMs()));
                output_flags_ = ukcp::HeaderFlags::None;
        }

        void Drain() {
                while (true) {
                        const int size = ikcp_peeksize(kcp_);
                        if (size < 0) { return; }
                        recv_scratch_.resize(static_cast<std::size_t>(size));
                        const int rc = ikcp_recv(kcp_, reinterpret_cast<char *>(recv_scratch_.data()), size);
                        if (rc < 0) { return; }
                        if (rc > 0 && (recv_scratch_[0] == static_cast<std::uint8_t>('D') || recv_scratch_[0] == static_cast<std::uint8_t>('R'))) {
                                ++recv_downlink_;
                        }
                }
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

        std::string server_addr_;
        std::uint32_t sess_id_{0};
        const Config &config_;
        SocketHandle socket_{kInvalidSocket};
        ukcp::Endpoint server_endpoint_{};
        ikcpcb *kcp_{nullptr};
        ukcp::HeaderFlags output_flags_{ukcp::HeaderFlags::None};
        std::vector<std::uint8_t> recv_buffer_{std::vector<std::uint8_t>(4096)};
        std::vector<std::uint8_t> recv_scratch_{};
        std::uint64_t sent_kcp_{0};
        std::uint64_t sent_udp_{0};
        std::uint64_t recv_downlink_{0};
};

struct WorkerReport {
        std::uint64_t sent_kcp{0};
        std::uint64_t sent_udp{0};
        std::uint64_t recv_downlink{0};
};

void DrainClientRoomTraffic(std::span<std::unique_ptr<PerfClient>> clients, WorkerReport &report, std::chrono::milliseconds quiet_period) {
        ukcp::QuiescenceDrain drain(quiet_period);
        while (true) {
                bool progress = false;
                std::uint64_t recv_total = 0;
                for (auto &client : clients) {
                        const auto before = client->recv_downlink();
                        client->Pump(0ms);
                        const auto after = client->recv_downlink();
                        recv_total += after;
                        progress = progress || after != before;
                }

                const auto now = std::chrono::steady_clock::now();
                drain.Observe(recv_total, now);
                if (drain.Done(now)) { break; }
                if (!progress) { std::this_thread::sleep_for(1ms); }
        }

        for (auto &client : clients) {
                report.sent_kcp += client->sent_kcp();
                report.sent_udp += client->sent_udp();
                report.recv_downlink += client->recv_downlink();
        }
}

#if !UKCP_PLATFORM_WINDOWS
class WorkerEpoll {
      public:
        WorkerEpoll() = default;

        ~WorkerEpoll() {
                if (epoll_fd_ >= 0) {
                        close(epoll_fd_);
                        epoll_fd_ = -1;
                }
        }

        void Open() {
                epoll_fd_ = epoll_create1(0);
                if (epoll_fd_ < 0) { throw std::runtime_error("epoll_create1 failed"); }
        }

        void Add(SocketHandle socket, std::uint32_t local_index) {
                epoll_event event{};
                event.events = EPOLLIN;
                event.data.u32 = local_index;
                if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, socket, &event) != 0) { throw std::runtime_error("epoll_ctl add failed"); }
        }

        int Wait(epoll_event *events, int capacity, int timeout_ms) { return epoll_wait(epoll_fd_, events, capacity, timeout_ms); }

      private:
        int epoll_fd_{-1};
};
#endif

int RunFixedMode(const Config &cfg, std::vector<std::unique_ptr<PerfClient>> &clients) {
        std::vector<std::thread> workers;
        std::vector<WorkerReport> reports(static_cast<std::size_t>(cfg.client_workers));
        workers.reserve(static_cast<std::size_t>(cfg.client_workers));

        const auto start = std::chrono::steady_clock::now();
        const auto deadline = start + std::chrono::seconds(cfg.seconds);
        const auto interval = std::chrono::microseconds(1'000'000 / cfg.uplink_pps);

        for (int worker = 0; worker < cfg.client_workers; ++worker) {
                workers.emplace_back([&, worker] {
                        const std::size_t begin = static_cast<std::size_t>(worker) * clients.size() / cfg.client_workers;
                        const std::size_t end = static_cast<std::size_t>(worker + 1) * clients.size() / cfg.client_workers;
                        std::vector<std::uint32_t> seqs(end - begin, 1);
                        std::vector<std::uint64_t> sent_counts(end - begin, 0);
                        std::vector<std::chrono::steady_clock::time_point> next_due(end - begin, start);
                        auto &report = reports[static_cast<std::size_t>(worker)];
                        const auto expected_per_client = static_cast<std::uint64_t>(cfg.uplink_pps) * static_cast<std::uint64_t>(cfg.seconds);

                        while (std::chrono::steady_clock::now() < deadline) {
                                const auto now = std::chrono::steady_clock::now();
                                for (std::size_t i = begin; i < end; ++i) {
                                        const std::size_t local = i - begin;
                                        while (sent_counts[local] < expected_per_client && next_due[local] <= now) {
                                                clients[i]->SendFixedSample(seqs[local]++);
                                                ++sent_counts[local];
                                                next_due[local] += interval;
                                        }
                                        clients[i]->Pump(0ms);
                                }
                                std::this_thread::sleep_for(1ms);
                        }

                        const auto drain_deadline = std::chrono::steady_clock::now() + 3s;
                        while (std::chrono::steady_clock::now() < drain_deadline) {
                                bool progress = false;
                                for (std::size_t i = begin; i < end; ++i) {
                                        const auto before = clients[i]->recv_downlink();
                                        clients[i]->Pump(0ms);
                                        if (clients[i]->recv_downlink() != before) { progress = true; }
                                }
                                if (!progress) { std::this_thread::sleep_for(1ms); }
                        }

                        for (std::size_t i = begin; i < end; ++i) {
                                report.sent_kcp += clients[i]->sent_kcp();
                                report.recv_downlink += clients[i]->recv_downlink();
                        }
                });
        }

        for (auto &worker : workers) { worker.join(); }

        std::uint64_t sent_uplink = 0;
        std::uint64_t recv_downlink = 0;
        for (const auto &report : reports) {
                sent_uplink += report.sent_kcp;
                recv_downlink += report.recv_downlink;
        }

        const auto duration_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        const auto expected_uplink =
                static_cast<std::uint64_t>(cfg.clients) * static_cast<std::uint64_t>(cfg.uplink_pps) * static_cast<std::uint64_t>(cfg.seconds);
        const auto expected_downlink =
                static_cast<std::uint64_t>(cfg.clients) * static_cast<std::uint64_t>(cfg.downlink_pps) * static_cast<std::uint64_t>(cfg.seconds);
        const double uplink_pct = expected_uplink > 0 ? static_cast<double>(sent_uplink) * 100.0 / static_cast<double>(expected_uplink) : 0.0;
        const double downlink_pct = expected_downlink > 0 ? static_cast<double>(recv_downlink) * 100.0 / static_cast<double>(expected_downlink) : 0.0;

        std::cout << "[FIXED RATE CLIENT]\n";
        std::cout << "  duration_s=" << duration_s << '\n';
        std::cout << "  expected_uplink=" << expected_uplink << " sent_uplink=" << sent_uplink << '\n';
        std::cout << "  expected_downlink=" << expected_downlink << " recv_downlink=" << recv_downlink << '\n';
        std::cout << "  uplink_delivery_pct=" << uplink_pct << " downlink_delivery_pct=" << downlink_pct << '\n';
        return 0;
}

int RunRoomMode(const Config &cfg, std::vector<std::unique_ptr<PerfClient>> &clients) {
        std::vector<std::thread> workers;
        std::vector<WorkerReport> reports(static_cast<std::size_t>(cfg.client_workers));
        workers.reserve(static_cast<std::size_t>(cfg.client_workers));

        const auto start = std::chrono::steady_clock::now();
        const auto deadline = start + std::chrono::seconds(cfg.seconds);

        for (int worker = 0; worker < cfg.client_workers; ++worker) {
                workers.emplace_back([&, worker] {
                        const std::size_t begin = static_cast<std::size_t>(worker) * clients.size() / cfg.client_workers;
                        const std::size_t end = static_cast<std::size_t>(worker + 1) * clients.size() / cfg.client_workers;
                        std::vector<std::vector<ScheduledEvent>> schedules;
                        schedules.reserve(end - begin);
                        std::vector<std::size_t> next_event(end - begin, 0);
                        for (std::size_t i = begin; i < end; ++i) { schedules.push_back(BuildRoomSchedule(static_cast<std::uint32_t>(10000 + i), cfg)); }

                        auto &report = reports[static_cast<std::size_t>(worker)];
#if !UKCP_PLATFORM_WINDOWS
                        WorkerEpoll poller;
                        poller.Open();
                        std::vector<epoll_event> events(end - begin);
                        for (std::size_t i = begin; i < end; ++i) { poller.Add(clients[i]->socket(), static_cast<std::uint32_t>(i - begin)); }
#endif
                        while (std::chrono::steady_clock::now() < deadline) {
                                const auto elapsed_ms =
                                        static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)
                                                                           .count());
                                bool progress = false;
                                for (std::size_t i = begin; i < end; ++i) {
                                        const std::size_t local = i - begin;
                                        const auto &schedule = schedules[local];
                                        while (next_event[local] < schedule.size() && schedule[next_event[local]].offset_ms <= elapsed_ms) {
                                                const auto &event = schedule[next_event[local]];
                                                if (event.is_udp) {
                                                        clients[i]->SendRoomUdp(event.seq);
                                                } else {
                                                        clients[i]->SendRoomKcp(event.seq);
                                                }
                                                ++next_event[local];
                                                progress = true;
                                        }
                                }
#if !UKCP_PLATFORM_WINDOWS
                                const int ready = poller.Wait(events.data(), static_cast<int>(events.size()), progress ? 0 : 1);
                                for (int event_index = 0; event_index < ready; ++event_index) {
                                        clients[begin + static_cast<std::size_t>(events[event_index].data.u32)]->Pump(0ms);
                                        progress = true;
                                }
#else
                                for (std::size_t i = begin; i < end; ++i) {
                                        const auto before = clients[i]->recv_downlink();
                                        clients[i]->Pump(0ms);
                                        if (clients[i]->recv_downlink() != before) { progress = true; }
                                }
#endif
                                if (!progress) { std::this_thread::sleep_for(1ms); }
                        }

                        auto worker_clients = std::span<std::unique_ptr<PerfClient>>(clients).subspan(begin, end - begin);
                        DrainClientRoomTraffic(worker_clients, report, std::chrono::milliseconds(cfg.drain_ms));
                });
        }

        for (auto &worker : workers) { worker.join(); }

        std::uint64_t sent_kcp = 0;
        std::uint64_t sent_udp = 0;
        std::uint64_t recv_downlink = 0;
        for (const auto &report : reports) {
                sent_kcp += report.sent_kcp;
                sent_udp += report.sent_udp;
                recv_downlink += report.recv_downlink;
        }

        const auto duration_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        const auto expected_kcp = static_cast<std::uint64_t>(cfg.clients) * static_cast<std::uint64_t>(cfg.kcp_pps) * static_cast<std::uint64_t>(cfg.seconds);
        const auto expected_udp = static_cast<std::uint64_t>(cfg.clients) * static_cast<std::uint64_t>(cfg.udp_pps) * static_cast<std::uint64_t>(cfg.seconds);
        const auto expected_room_frames =
                static_cast<std::uint64_t>(cfg.clients) * static_cast<std::uint64_t>(cfg.room_fps) * static_cast<std::uint64_t>(cfg.seconds);
        const double room_recv_pct = expected_room_frames > 0 ? static_cast<double>(recv_downlink) * 100.0 / static_cast<double>(expected_room_frames) : 0.0;

        std::cout << "[ROOM CLIENT]\n";
        std::cout << "  duration_s=" << duration_s << '\n';
        std::cout << "  expected_kcp=" << expected_kcp << " sent_kcp=" << sent_kcp << '\n';
        std::cout << "  expected_udp=" << expected_udp << " sent_udp=" << sent_udp << '\n';
        std::cout << "  expected_room_frames=" << expected_room_frames << " recv_room_frames=" << recv_downlink << '\n';
        std::cout << "  room_recv_pct=" << room_recv_pct << '\n';
        return 0;
}

} // namespace

int main(int argc, char **argv) {
        try {
                const Config cfg = ParseArgs(argc, argv);
                std::vector<std::unique_ptr<PerfClient>> clients;
                clients.reserve(static_cast<std::size_t>(cfg.clients));
                for (int i = 0; i < cfg.clients; ++i) {
                        auto client = std::make_unique<PerfClient>(cfg.server, static_cast<std::uint32_t>(10000 + i), cfg);
                        client->SendAuth();
                        clients.push_back(std::move(client));
                }

                const auto auth_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(cfg.auth_warmup_ms);
                while (std::chrono::steady_clock::now() < auth_deadline) {
                        for (auto &client : clients) { client->Pump(0ms); }
                        std::this_thread::sleep_for(1ms);
                }

                return cfg.mode == Mode::Room ? RunRoomMode(cfg, clients) : RunFixedMode(cfg, clients);
        } catch (const std::exception &ex) {
                std::cerr << ex.what() << '\n';
                return 1;
        }
}
