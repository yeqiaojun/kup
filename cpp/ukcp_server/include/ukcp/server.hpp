#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "ukcp/config.hpp"

namespace ukcp {

class Handler;
class Session;
struct ServerImpl;

struct ServerStatsSnapshot {
        std::uint64_t recv_packets{0};
        std::uint64_t recv_bytes{0};
        std::uint64_t recv_kcp_packets{0};
        std::uint64_t recv_udp_packets{0};
        std::uint64_t sent_packets{0};
        std::uint64_t sent_bytes{0};
        std::uint64_t sent_kcp_packets{0};
        std::uint64_t sent_udp_packets{0};
        std::uint32_t active_sessions{0};
};

class Server {
      public:
        Server(std::string listen_addr, Handler &handler, Config config = {});
        ~Server();

        Server(const Server &) = delete;
        Server &operator=(const Server &) = delete;

        bool Start();
        void Close();

        [[nodiscard]] bool IsRunning() const noexcept;
        [[nodiscard]] Session *FindSession(std::uint32_t sess_id);
        [[nodiscard]] ServerStatsSnapshot Stats() const;

        bool SendKcpToSess(std::uint32_t sess_id, std::span<const std::uint8_t> payload);
        bool SendKcpToMultiSess(const std::vector<std::uint32_t> &sess_ids, std::span<const std::uint8_t> payload);
        bool SendKcpToAll(std::span<const std::uint8_t> payload);
        bool SendUdpToSess(std::uint32_t sess_id, std::uint32_t packet_seq, std::span<const std::uint8_t> payload);
        bool SendUdpToMultiSess(const std::vector<std::uint32_t> &sess_ids, std::uint32_t packet_seq, std::span<const std::uint8_t> payload);
        bool SendUdpToAll(std::uint32_t packet_seq, std::span<const std::uint8_t> payload);

      private:
        std::unique_ptr<ServerImpl> impl_;
};

} // namespace ukcp
