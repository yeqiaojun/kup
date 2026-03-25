#pragma once

#include <cstddef>

#include "ikcp.h"
#include "ukcp/protocol.hpp"

namespace ukcp {

constexpr int kMinKcpMtu = 50;
constexpr int kKcpOverhead = 24;

// Vendored ikcp_send rejects fragmented messages with count >= IKCP_WND_RCV.
constexpr std::size_t kMaxKcpMessageFragments = 127;

inline int KcpMtuFromTransportMtu(int transport_mtu) noexcept {
        return transport_mtu - static_cast<int>(Header::kSize);
}

inline std::size_t MaxKcpPayloadSizeForTransportMtu(int transport_mtu) noexcept {
        const int kcp_mtu = KcpMtuFromTransportMtu(transport_mtu);
        if (kcp_mtu < kMinKcpMtu) { return 0; }

        const int mss = kcp_mtu - kKcpOverhead;
        if (mss <= 0) { return 0; }
        return static_cast<std::size_t>(mss) * kMaxKcpMessageFragments;
}

} // namespace ukcp
