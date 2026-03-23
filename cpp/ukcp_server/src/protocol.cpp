#include "ukcp/protocol.hpp"

namespace ukcp {

namespace {

void PutU16(std::span<std::uint8_t, 2> dst, std::uint16_t value) noexcept {
        dst[0] = static_cast<std::uint8_t>(value);
        dst[1] = static_cast<std::uint8_t>(value >> 8);
}

void PutU32(std::span<std::uint8_t, 4> dst, std::uint32_t value) noexcept {
        dst[0] = static_cast<std::uint8_t>(value);
        dst[1] = static_cast<std::uint8_t>(value >> 8);
        dst[2] = static_cast<std::uint8_t>(value >> 16);
        dst[3] = static_cast<std::uint8_t>(value >> 24);
}

std::uint16_t GetU16(std::span<const std::uint8_t, 2> src) noexcept { return static_cast<std::uint16_t>(src[0]) | (static_cast<std::uint16_t>(src[1]) << 8); }

std::uint32_t GetU32(std::span<const std::uint8_t, 4> src) noexcept {
        return static_cast<std::uint32_t>(src[0]) | (static_cast<std::uint32_t>(src[1]) << 8) | (static_cast<std::uint32_t>(src[2]) << 16) |
               (static_cast<std::uint32_t>(src[3]) << 24);
}

bool ValidMsgType(MsgType value) noexcept { return value == MsgType::Udp || value == MsgType::Kcp; }

} // namespace

bool Header::EncodeTo(std::span<std::uint8_t> dst) const noexcept {
        if (dst.size() < kSize || !ValidMsgType(msg_type)) { return false; }

        dst[0] = static_cast<std::uint8_t>(msg_type);
        dst[1] = static_cast<std::uint8_t>(flags);
        PutU16(std::span<std::uint8_t, 2>(dst.data() + 2, 2), body_len);
        PutU32(std::span<std::uint8_t, 4>(dst.data() + 4, 4), sess_id);
        PutU32(std::span<std::uint8_t, 4>(dst.data() + 8, 4), packet_seq);
        return true;
}

std::array<std::uint8_t, Header::kSize> Header::Encode() const noexcept {
        std::array<std::uint8_t, kSize> out{};
        EncodeTo(out);
        return out;
}

bool Header::Decode(std::span<const std::uint8_t> src, Header &out) noexcept {
        if (src.size() < kSize) { return false; }

        out.msg_type = static_cast<MsgType>(src[0]);
        out.flags = static_cast<HeaderFlags>(src[1]);
        out.body_len = GetU16(std::span<const std::uint8_t, 2>(src.data() + 2, 2));
        out.sess_id = GetU32(std::span<const std::uint8_t, 4>(src.data() + 4, 4));
        out.packet_seq = GetU32(std::span<const std::uint8_t, 4>(src.data() + 8, 4));
        return ValidMsgType(out.msg_type);
}

bool Header::SplitPacket(std::span<const std::uint8_t> packet, Header &out_header, std::span<const std::uint8_t> &out_body) noexcept {
        if (!Decode(packet, out_header)) { return false; }
        if (packet.size() != kSize + out_header.body_len) { return false; }

        out_body = packet.subspan(kSize, out_header.body_len);
        return true;
}

} // namespace ukcp
