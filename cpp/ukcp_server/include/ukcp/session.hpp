#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace ukcp {

class Server;
struct SessionImpl;

class Session {
      public:
        explicit Session(std::unique_ptr<SessionImpl> impl);
        ~Session();

        Session(const Session &) = delete;
        Session &operator=(const Session &) = delete;

        [[nodiscard]] std::uint32_t id() const noexcept;
        [[nodiscard]] std::string RemoteAddrString() const;
        [[nodiscard]] SessionImpl *raw_impl() noexcept;
        [[nodiscard]] const SessionImpl *raw_impl() const noexcept;

        bool Send(std::span<const std::uint8_t> payload);
        bool SendRawUdp(std::uint32_t packet_seq, std::span<const std::uint8_t> payload);
        void Close(const std::string &reason = "session closed");

      private:
        friend class Server;
        std::unique_ptr<SessionImpl> impl_;
};

} // namespace ukcp
