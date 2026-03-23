#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ukcp {

enum class MsgType : std::uint8_t {
        Udp = 1,
        Kcp = 2,
};

enum class HeaderFlags : std::uint8_t {
        None = 0,
        Connect = 1 << 0,
};

constexpr HeaderFlags operator|(HeaderFlags lhs, HeaderFlags rhs) noexcept {
        return static_cast<HeaderFlags>(static_cast<std::uint8_t>(lhs) | static_cast<std::uint8_t>(rhs));
}

constexpr HeaderFlags operator&(HeaderFlags lhs, HeaderFlags rhs) noexcept {
        return static_cast<HeaderFlags>(static_cast<std::uint8_t>(lhs) & static_cast<std::uint8_t>(rhs));
}

struct Header {
        static constexpr std::size_t kSize = 12;

        MsgType msg_type{MsgType::Udp};
        HeaderFlags flags{HeaderFlags::None};
        std::uint16_t body_len{0};
        std::uint32_t sess_id{0};
        std::uint32_t packet_seq{0};

        [[nodiscard]] bool EncodeTo(std::span<std::uint8_t> dst) const noexcept;
        [[nodiscard]] std::array<std::uint8_t, kSize> Encode() const noexcept;

        static bool Decode(std::span<const std::uint8_t> src, Header &out) noexcept;
        static bool SplitPacket(std::span<const std::uint8_t> packet, Header &out_header, std::span<const std::uint8_t> &out_body) noexcept;
};

} // namespace ukcp
