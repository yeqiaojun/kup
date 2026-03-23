#include "server_internal.hpp"

#include <algorithm>

namespace ukcp {

namespace {

constexpr int kMinKcpMtu = 50;

int KcpMtuFromTransportMtu(int mtu) { return mtu - static_cast<int>(Header::kSize); }

std::string CloseReasonOrDefault(const std::string &reason) { return reason.empty() ? std::string("session closed") : reason; }

std::vector<std::uint8_t> &WrappedPacketBuffer(std::size_t size) {
        thread_local std::vector<std::uint8_t> packet;
        if (packet.size() < size) { packet.resize(size); }
        return packet;
}

bool EncodeWrappedPacket(MsgType msg_type, HeaderFlags flags, std::uint32_t sess_id, std::uint32_t packet_seq, std::span<const std::uint8_t> payload,
                         std::span<std::uint8_t> packet) {
        Header header{};
        header.msg_type = msg_type;
        header.flags = flags;
        header.body_len = static_cast<std::uint16_t>(payload.size());
        header.sess_id = sess_id;
        header.packet_seq = packet_seq;
        if (!header.EncodeTo(packet)) { return false; }
        std::copy(payload.begin(), payload.end(), packet.begin() + static_cast<std::ptrdiff_t>(Header::kSize));
        return true;
}

bool SendWrappedPacket(SocketHandle socket_fd, const Endpoint *endpoint, MsgType msg_type, HeaderFlags flags, std::uint32_t sess_id, std::uint32_t packet_seq,
                       std::span<const std::uint8_t> payload) {
        if (socket_fd == kInvalidSocket || (endpoint != nullptr && !endpoint->valid())) { return false; }

        auto &packet = WrappedPacketBuffer(Header::kSize + payload.size());
        auto packet_span = std::span<std::uint8_t>(packet.data(), Header::kSize + payload.size());
        if (!EncodeWrappedPacket(msg_type, flags, sess_id, packet_seq, payload, packet_span)) { return false; }
        return endpoint == nullptr ? SendConnectedDatagram(socket_fd, packet.data(), packet_span.size())
                                   : SendDatagram(socket_fd, *endpoint, packet.data(), packet_span.size());
}

} // namespace

Session::Session(std::unique_ptr<SessionImpl> impl) : impl_(std::move(impl)) {}
Session::~Session() = default;

std::uint32_t Session::id() const noexcept { return impl_ ? impl_->sess_id : 0; }

std::string Session::RemoteAddrString() const {
        if (!impl_) { return {}; }
        std::shared_lock lock(impl_->mutex);
        return impl_->remote.ToString();
}

SessionImpl *Session::raw_impl() noexcept { return impl_.get(); }

const SessionImpl *Session::raw_impl() const noexcept { return impl_.get(); }

bool Session::SendKcp(std::span<const std::uint8_t> payload) {
        if (!impl_) { return false; }

        {
                std::unique_lock lock(impl_->mutex);
                if (impl_->closed || impl_->kcp == nullptr) { return false; }
                if (impl_->remote.valid() == false) { return false; }
                if (payload.size() > MaxKcpPayloadSize(*impl_->kcp)) { return false; }

                if (ikcp_send(impl_->kcp, reinterpret_cast<const char *>(payload.data()), static_cast<int>(payload.size())) < 0) { return false; }
        }

        ScheduleSessionUpdate(*impl_->server, *impl_->public_session, NowMs());
        return true;
}

bool Session::SendUdp(std::uint32_t packet_seq, std::span<const std::uint8_t> payload) {
        if (!impl_) { return false; }

        bool sent = false;
        {
                std::shared_lock lock(impl_->mutex);
                if (impl_->closed || !impl_->remote.valid()) { return false; }
                sent = (impl_->socket_fd != kInvalidSocket) ? SendWrappedConnectedUdp(impl_->socket_fd, impl_->sess_id, packet_seq, payload)
                                                            : SendWrappedUdp(impl_->server->socket_fd, impl_->remote, impl_->sess_id, packet_seq, payload);
        }

        if (sent) {
                RecordSentUdpStats(*impl_->server, payload.size());
        }
        return sent;
}

void Session::Close(const std::string &reason) {
        if (!impl_) { return; }
        std::unique_lock lock(impl_->mutex);
        impl_->close_reason = CloseReasonOrDefault(reason);
        impl_->closed = true;
}

int SessionOutput(const char *buf, int len, ikcpcb *, void *user) {
        auto *impl = static_cast<SessionImpl *>(user);
        // KCP invokes output while the caller holds SessionImpl::mutex.
        if (impl == nullptr || impl->server == nullptr || impl->closed) { return 0; }
        // Use the per-session connected socket (send) instead of the
        // listener socket (sendto) so that each session's outbound traffic
        // flows through its own connected UDP fd.
        const bool sent =
                (impl->socket_fd != kInvalidSocket)
                        ? SendWrappedConnectedKcp(impl->socket_fd, impl->sess_id, HeaderFlags::None,
                                                  std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(buf), static_cast<std::size_t>(len)))
                        : SendWrappedKcp(impl->server->socket_fd, impl->remote, impl->sess_id, HeaderFlags::None,
                                         std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(buf), static_cast<std::size_t>(len)));
        if (sent) { RecordSentKcpStats(*impl->server, static_cast<std::size_t>(len)); }
        return 0;
}

int PendingOutput(const char *buf, int len, ikcpcb *, void *user) {
        auto *pending = static_cast<PendingAuth *>(user);
        (void)buf;
        (void)len;
        (void)pending;
        return 0;
}

void ConfigureKcp(ikcpcb &kcp, const Config &config) {
        ikcp_nodelay(&kcp, config.kcp.no_delay, config.kcp.interval, config.kcp.resend, config.kcp.no_congestion);
        ikcp_wndsize(&kcp, config.kcp.send_window, config.kcp.recv_window);
        const int kcp_mtu = KcpMtuFromTransportMtu(config.kcp.mtu);
        if (kcp_mtu < kMinKcpMtu) { return; }
        ikcp_setmtu(&kcp, kcp_mtu);
}

std::vector<std::vector<std::uint8_t>> DrainKcpMessages(ikcpcb &kcp, std::vector<std::uint8_t> &scratch) {
        std::vector<std::vector<std::uint8_t>> out;
        while (true) {
                const int size = ikcp_peeksize(&kcp);
                if (size < 0) { return out; }
                if (scratch.size() < static_cast<std::size_t>(size)) { scratch.resize(static_cast<std::size_t>(size)); }
                const int received = ikcp_recv(&kcp, reinterpret_cast<char *>(scratch.data()), size);
                if (received < 0) { return out; }
                out.emplace_back(scratch.begin(), scratch.begin() + received);
        }
}

bool SendWrappedKcp(SocketHandle socket_fd, const Endpoint &endpoint, std::uint32_t sess_id, HeaderFlags flags, std::span<const std::uint8_t> payload) {
        return SendWrappedPacket(socket_fd, &endpoint, MsgType::Kcp, flags, sess_id, 0, payload);
}

bool SendWrappedConnectedKcp(SocketHandle socket_fd, std::uint32_t sess_id, HeaderFlags flags, std::span<const std::uint8_t> payload) {
        return SendWrappedPacket(socket_fd, nullptr, MsgType::Kcp, flags, sess_id, 0, payload);
}

bool SendWrappedUdp(SocketHandle socket_fd, const Endpoint &endpoint, std::uint32_t sess_id, std::uint32_t packet_seq, std::span<const std::uint8_t> payload) {
        return SendWrappedPacket(socket_fd, &endpoint, MsgType::Udp, HeaderFlags::None, sess_id, packet_seq, payload);
}

bool SendWrappedConnectedUdp(SocketHandle socket_fd, std::uint32_t sess_id, std::uint32_t packet_seq, std::span<const std::uint8_t> payload) {
        return SendWrappedPacket(socket_fd, nullptr, MsgType::Udp, HeaderFlags::None, sess_id, packet_seq, payload);
}

bool SameEndpoint(const Endpoint &a, const Endpoint &b) noexcept { return a.valid() && b.valid() && a.Equals(b); }

} // namespace ukcp
