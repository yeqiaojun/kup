#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "ukcp/config.hpp"

namespace ukcp {

struct ClientImpl;

class Client {
      public:
        Client(std::string server_addr, std::uint32_t sess_id, Config config = {});
        ~Client();

        Client(const Client &) = delete;
        Client &operator=(const Client &) = delete;

        bool Connect(std::span<const std::uint8_t> auth_payload);
        void Close();

        [[nodiscard]] bool IsConnected() const noexcept;
        bool SendKcp(std::span<const std::uint8_t> payload);
        bool SendUdp(std::uint32_t packet_seq, std::span<const std::uint8_t> payload);
        bool Poll(std::chrono::milliseconds timeout = std::chrono::milliseconds{0});
        bool Recv(std::vector<std::uint8_t> &out);

      private:
        std::unique_ptr<ClientImpl> impl_;
};

} // namespace ukcp
