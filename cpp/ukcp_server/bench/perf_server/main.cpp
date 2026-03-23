#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "quiescence.hpp"
#include "ukcp/handler.hpp"
#include "ukcp/server.hpp"
#include "ukcp/session.hpp"

namespace {

enum class Mode {
        Fixed,
        Room,
};

struct Config {
        Mode mode{Mode::Fixed};
        std::string listen{"127.0.0.1:39220"};
        int seconds{10};
        int start_delay_ms{1000};
        int warmup_ms{30000};
        int drain_ms{2000};
        int downlink_pps{15};
        int downlink_payload_bytes{15};
        int rooms{500};
        int room_size{10};
        int room_fps{15};
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
                } else if (arg == "--listen") {
                        config.listen = next(i);
                } else if (arg == "--seconds") {
                        config.seconds = std::stoi(next(i));
                } else if (arg == "--start-delay-ms") {
                        config.start_delay_ms = std::stoi(next(i));
                } else if (arg == "--warmup-ms") {
                        config.warmup_ms = std::stoi(next(i));
                } else if (arg == "--drain-ms") {
                        config.drain_ms = std::stoi(next(i));
                } else if (arg == "--downlink-pps") {
                        config.downlink_pps = std::stoi(next(i));
                } else if (arg == "--downlink-payload-bytes") {
                        config.downlink_payload_bytes = std::stoi(next(i));
                } else if (arg == "--rooms") {
                        config.rooms = std::stoi(next(i));
                } else if (arg == "--room-size") {
                        config.room_size = std::stoi(next(i));
                } else if (arg == "--room-fps") {
                        config.room_fps = std::stoi(next(i));
                } else {
                        throw std::runtime_error("unknown argument: " + std::string(arg));
                }
        }

        config.seconds = (std::max)(config.seconds, 1);
        config.start_delay_ms = (std::max)(config.start_delay_ms, 0);
        config.warmup_ms = (std::max)(config.warmup_ms, 0);
        config.drain_ms = (std::max)(config.drain_ms, 0);
        config.downlink_pps = (std::max)(config.downlink_pps, 1);
        config.downlink_payload_bytes = (std::max)(config.downlink_payload_bytes, 8);
        config.rooms = (std::max)(config.rooms, 1);
        config.room_size = (std::max)(config.room_size, 1);
        config.room_fps = (std::max)(config.room_fps, 1);
        return config;
}

class PerfHandler final : public ukcp::Handler {
      public:
        explicit PerfHandler(const Config &config) : config_(config) {}

        bool Auth(std::uint32_t, const std::string &, std::span<const std::uint8_t> payload) override {
                return std::string(payload.begin(), payload.end()) == "auth";
        }

        void OnSessionOpen(ukcp::Session &session) override {
                if (config_.mode != Mode::Room) { return; }

                std::lock_guard lock(rooms_mutex_);
                const std::uint32_t room_id = RoomId(session.id());
                rooms_[room_id].push_back(session.id());
                sess_to_room_[session.id()] = room_id;
        }

        void OnUDP(ukcp::Session &, std::uint32_t, std::span<const std::uint8_t>) override { recv_udp.fetch_add(1, std::memory_order_relaxed); }

        void OnKCP(ukcp::Session &, std::span<const std::uint8_t> payload) override {
                if (!payload.empty() && (payload[0] == static_cast<std::uint8_t>('S') || payload[0] == static_cast<std::uint8_t>('K'))) {
                        recv_kcp.fetch_add(1, std::memory_order_relaxed);
                }
        }

        void OnSessionClose(ukcp::Session &session, const std::string &) override {
                if (config_.mode != Mode::Room) { return; }

                std::lock_guard lock(rooms_mutex_);
                auto room_it = sess_to_room_.find(session.id());
                if (room_it == sess_to_room_.end()) { return; }

                const std::uint32_t room_id = room_it->second;
                sess_to_room_.erase(room_it);

                auto members_it = rooms_.find(room_id);
                if (members_it == rooms_.end()) { return; }
                auto &members = members_it->second;
                members.erase(std::remove(members.begin(), members.end(), session.id()), members.end());
                if (members.empty()) { rooms_.erase(members_it); }
        }

        std::vector<std::vector<std::uint32_t>> SnapshotRooms() const {
                std::lock_guard lock(rooms_mutex_);
                std::vector<std::vector<std::uint32_t>> snapshot;
                snapshot.reserve(rooms_.size());
                for (const auto &[_, members] : rooms_) {
                        if (!members.empty()) { snapshot.push_back(members); }
                }
                return snapshot;
        }

        std::size_t ActiveRooms() const {
                std::lock_guard lock(rooms_mutex_);
                return rooms_.size();
        }

        std::atomic<std::uint64_t> recv_kcp{0};
        std::atomic<std::uint64_t> recv_udp{0};

      private:
        std::uint32_t RoomId(std::uint32_t sess_id) const {
                const std::uint32_t bucket = sess_id / static_cast<std::uint32_t>(config_.room_size);
                return config_.rooms > 0 ? bucket % static_cast<std::uint32_t>(config_.rooms) : bucket;
        }

        const Config &config_;
        mutable std::mutex rooms_mutex_;
        std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> rooms_;
        std::unordered_map<std::uint32_t, std::uint32_t> sess_to_room_;
};

void SleepUntil(std::chrono::steady_clock::time_point due) {
        const auto now = std::chrono::steady_clock::now();
        if (due > now) { std::this_thread::sleep_until(due); }
}

void DrainSentPackets(ukcp::Server &server, std::chrono::milliseconds quiet_period, std::chrono::milliseconds max_wait,
                      std::chrono::milliseconds poll_interval = std::chrono::milliseconds(1)) {
        ukcp::QuiescenceDrain drain(quiet_period);
        const auto deadline = std::chrono::steady_clock::now() + max_wait;
        while (server.IsRunning()) {
                const auto now = std::chrono::steady_clock::now();
                drain.Observe(server.Stats().sent_kcp_packets, now);
                if (drain.Done(now) || now >= deadline) { return; }
                std::this_thread::sleep_for(poll_interval);
        }
}

int RunFixedMode(const Config &cfg, ukcp::Server &server, PerfHandler &handler) {
        std::vector<std::uint8_t> payload(static_cast<std::size_t>(cfg.downlink_payload_bytes), 0);
        payload[0] = static_cast<std::uint8_t>('D');

        const auto interval = std::chrono::microseconds(1'000'000 / cfg.downlink_pps);
        const auto start = std::chrono::steady_clock::now();
        const auto deadline = start + std::chrono::seconds(cfg.seconds);
        auto next_due = start;
        std::uint32_t tick = 0;

        while (std::chrono::steady_clock::now() < deadline) {
                SleepUntil(next_due);
                if (std::chrono::steady_clock::now() >= deadline) { break; }
                WriteUint32LE(payload.data() + 1, ++tick);
                server.SendToAll(payload);
                next_due += interval;
        }

        if (cfg.drain_ms > 0) {
                const auto quiet_period = std::chrono::milliseconds(cfg.drain_ms);
                DrainSentPackets(server, quiet_period, quiet_period + std::chrono::seconds(3));
        }

        const auto stats = server.Stats();
        std::cout << "[PERF SERVER]\n";
        std::cout << "  recv_uplink=" << handler.recv_kcp.load(std::memory_order_relaxed) << '\n';
        std::cout << "  recv_udp=" << handler.recv_udp.load(std::memory_order_relaxed) << '\n';
        std::cout << "  downlink_ticks=" << tick << '\n';
        std::cout << "  active_sessions=" << stats.active_sessions << '\n';
        std::cout << "  recv_packets=" << stats.recv_packets << " recv_bytes=" << stats.recv_bytes << " recv_kcp_packets=" << stats.recv_kcp_packets << '\n';
        std::cout << "  sent_packets=" << stats.sent_packets << " sent_bytes=" << stats.sent_bytes << " sent_kcp_packets=" << stats.sent_kcp_packets << '\n';
        return 0;
}

int RunRoomMode(const Config &cfg, ukcp::Server &server, PerfHandler &handler) {
        std::vector<std::uint8_t> payload(static_cast<std::size_t>(cfg.downlink_payload_bytes), 0);
        payload[0] = static_cast<std::uint8_t>('R');

        const std::uint32_t expected_sessions = static_cast<std::uint32_t>(cfg.rooms * cfg.room_size);
        const auto warmup_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(cfg.warmup_ms);
        while (server.Stats().active_sessions < expected_sessions && std::chrono::steady_clock::now() < warmup_deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        const auto interval = std::chrono::microseconds(1'000'000 / cfg.room_fps);
        const int stripes = (std::min)(5, cfg.rooms);
        const auto sub_interval = interval / stripes;
        const auto start = std::chrono::steady_clock::now();
        const auto deadline = start + std::chrono::seconds(cfg.seconds);
        auto next_due = start;
        std::uint32_t tick_rounds = 0;
        std::uint64_t room_frames = 0;
        std::uint64_t downlink_packets = 0;

        while (std::chrono::steady_clock::now() < deadline) {
                const auto rooms = handler.SnapshotRooms();
                ++tick_rounds;
                for (int stripe = 0; stripe < stripes; ++stripe) {
                        SleepUntil(next_due);
                        if (std::chrono::steady_clock::now() >= deadline) { break; }
                        for (std::size_t room_index = static_cast<std::size_t>(stripe); room_index < rooms.size();
                             room_index += static_cast<std::size_t>(stripes)) {
                                const auto &members = rooms[room_index];
                                WriteUint32LE(payload.data() + 1, tick_rounds);
                                if (payload.size() >= 9) { WriteUint32LE(payload.data() + 5, members.front() / static_cast<std::uint32_t>(cfg.room_size)); }
                                const auto report = server.SendToMultiSess(members, payload);
                                ++room_frames;
                                downlink_packets += static_cast<std::uint64_t>(report.sent);
                        }
                        next_due += sub_interval;
                }
        }

        const auto quiet_period = std::chrono::milliseconds((std::max)(cfg.drain_ms, 8000));
        DrainSentPackets(server, quiet_period, quiet_period + std::chrono::seconds(5));

        const auto stats = server.Stats();
        std::cout << "[ROOM SERVER]\n";
        std::cout << "  recv_kcp=" << handler.recv_kcp.load(std::memory_order_relaxed) << '\n';
        std::cout << "  recv_udp=" << handler.recv_udp.load(std::memory_order_relaxed) << '\n';
        std::cout << "  tick_rounds=" << tick_rounds << '\n';
        std::cout << "  room_frames=" << room_frames << '\n';
        std::cout << "  room_packets=" << downlink_packets << '\n';
        std::cout << "  active_rooms=" << handler.ActiveRooms() << '\n';
        std::cout << "  active_sessions=" << stats.active_sessions << '\n';
        std::cout << "  recv_packets=" << stats.recv_packets << " recv_bytes=" << stats.recv_bytes << " recv_kcp_packets=" << stats.recv_kcp_packets
                  << " recv_udp_packets=" << stats.recv_udp_packets << '\n';
        std::cout << "  sent_packets=" << stats.sent_packets << " sent_bytes=" << stats.sent_bytes << " sent_kcp_packets=" << stats.sent_kcp_packets << '\n';
        return 0;
}

} // namespace

int main(int argc, char **argv) {
        try {
                const Config cfg = ParseArgs(argc, argv);
                PerfHandler handler(cfg);
                ukcp::Server server(cfg.listen, handler, ukcp::Config{});
                if (!server.Start()) {
                        std::cerr << "failed to start server\n";
                        return 1;
                }

                if (cfg.start_delay_ms > 0) { std::this_thread::sleep_for(std::chrono::milliseconds(cfg.start_delay_ms)); }

                const int rc = cfg.mode == Mode::Room ? RunRoomMode(cfg, server, handler) : RunFixedMode(cfg, server, handler);
                server.Close();
                return rc;
        } catch (const std::exception &ex) {
                std::cerr << ex.what() << '\n';
                return 1;
        }
}
