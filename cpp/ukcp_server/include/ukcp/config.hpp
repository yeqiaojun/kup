#pragma once

#include <chrono>
#include <cstddef>

namespace ukcp {

struct KcpConfig {
        int mtu{1200};
        int no_delay{1};
        int interval{10};
        int resend{2};
        int no_congestion{1};
        int send_window{128};
        int recv_window{128};
        bool ack_no_delay{true};
};

struct Config {
        std::size_t packet_size{2048};
        std::size_t recv_buffer_size{2048};
        std::chrono::milliseconds update_interval{10};
        std::chrono::milliseconds fast_reconnect_window{10000};
        KcpConfig kcp{};
};

} // namespace ukcp
