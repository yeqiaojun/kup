#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace ukcp {

class Session;

class Handler {
      public:
        virtual ~Handler() = default;

        virtual bool Auth(std::uint32_t sess_id, const std::string &remote_addr, std::span<const std::uint8_t> payload) {
                (void)sess_id;
                (void)remote_addr;
                (void)payload;
                return true;
        }

        virtual void OnSessionOpen(Session &session) { (void)session; }
        virtual void OnUDP(Session &session, std::uint32_t packet_seq, std::span<const std::uint8_t> payload) {
                (void)session;
                (void)packet_seq;
                (void)payload;
        }

        virtual void OnKCP(Session &session, std::span<const std::uint8_t> payload) {
                (void)session;
                (void)payload;
        }

        virtual void OnSessionClose(Session &session, const std::string &reason) {
                (void)session;
                (void)reason;
        }
};

} // namespace ukcp
