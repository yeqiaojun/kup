#include "server_internal.hpp"

#include <algorithm>

namespace ukcp {

Server::Server(std::string listen_addr, Handler &handler, Config config) : impl_(std::make_unique<ServerImpl>()) {
        impl_->listen_addr = std::move(listen_addr);
        impl_->handler = &handler;
        impl_->config = config;
        const std::int64_t update_interval_ms = static_cast<std::int64_t>(impl_->config.update_interval.count());
        const std::int64_t kcp_interval_ms = static_cast<std::int64_t>(impl_->config.kcp.interval);
        impl_->scheduler_interval_ms = static_cast<std::uint64_t>((std::max)(std::int64_t{1}, (std::min)(update_interval_ms, kcp_interval_ms)));
        impl_->read_buffer.resize(impl_->config.recv_buffer_size);
}

Server::~Server() { Close(); }

bool Server::SetMtu(int mtu) {
        if (KcpMtuFromTransportMtu(mtu) < kMinKcpMtu) { return false; }
        std::unique_lock lock(impl_->mutex);
        impl_->config.kcp.mtu = mtu;
        return true;
}

} // namespace ukcp
