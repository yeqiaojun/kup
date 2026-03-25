#include "ukcp/client.hpp"

#include <algorithm>
#include <chrono>
#include <deque>
#include <limits>
#include <span>
#include <utility>
#include <vector>

#include "ikcp.h"
#include "platform_socket.hpp"
#include "ukcp/protocol.hpp"

namespace ukcp {

struct ClientImpl {
        std::string server_addr;
        std::uint32_t sess_id{0};
        Config config{};
        SocketHandle socket_fd{kInvalidSocket};
        Poller poller{};
        ikcpcb *kcp{nullptr};
        HeaderFlags output_flags{HeaderFlags::None};
        std::uint64_t next_update_ms{0};
        bool connected{false};
        std::vector<std::uint8_t> read_buffer;
        std::deque<std::vector<std::uint8_t>> recv_messages;
};

namespace {

constexpr int kMinKcpMtu = 50;
constexpr std::size_t kMaxKcpMessageFragments = 127;

int KcpMtuFromTransportMtu(int mtu) { return mtu - static_cast<int>(Header::kSize); }

std::size_t MaxKcpPayloadSize(const ikcpcb &kcp) {
        if (kcp.mss == 0) { return 0; }
        return static_cast<std::size_t>(kcp.mss) * kMaxKcpMessageFragments;
}

void ConfigureKcp(ikcpcb &kcp, const Config &config) {
        ikcp_nodelay(&kcp, config.kcp.no_delay, config.kcp.interval, config.kcp.resend, config.kcp.no_congestion);
        ikcp_wndsize(&kcp, config.kcp.send_window, config.kcp.recv_window);
        const int kcp_mtu = KcpMtuFromTransportMtu(config.kcp.mtu);
        if (kcp_mtu < kMinKcpMtu) { return; }
        ikcp_setmtu(&kcp, kcp_mtu);
}

bool EncodeWrappedConnectedPacket(SocketHandle socket_fd, MsgType msg_type, HeaderFlags flags, std::uint32_t sess_id, std::uint32_t packet_seq,
                                  std::span<const std::uint8_t> payload) {
        if (socket_fd == kInvalidSocket || payload.size() > static_cast<std::size_t>(std::numeric_limits<std::uint16_t>::max())) { return false; }

        Header header{};
        header.msg_type = msg_type;
        header.flags = flags;
        header.body_len = static_cast<std::uint16_t>(payload.size());
        header.sess_id = sess_id;
        header.packet_seq = packet_seq;

        std::vector<std::uint8_t> packet(Header::kSize + payload.size());
        if (!header.EncodeTo(packet)) { return false; }
        std::copy(payload.begin(), payload.end(), packet.begin() + static_cast<std::ptrdiff_t>(Header::kSize));
        return SendConnectedDatagram(socket_fd, packet.data(), packet.size());
}

int ClientOutput(const char *buf, int len, ikcpcb *, void *user) {
        auto *impl = static_cast<ClientImpl *>(user);
        if (impl == nullptr || !impl->connected || impl->socket_fd == kInvalidSocket || len < 0) { return -1; }

        const auto payload = std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(buf), static_cast<std::size_t>(len));
        return EncodeWrappedConnectedPacket(impl->socket_fd, MsgType::Kcp, impl->output_flags, impl->sess_id, 0, payload) ? 0 : -1;
}

void ReleaseClient(ClientImpl &impl) {
        impl.connected = false;
        impl.next_update_ms = 0;
        impl.output_flags = HeaderFlags::None;
        impl.recv_messages.clear();
        impl.poller.Close();
        if (impl.kcp != nullptr) {
                ikcp_release(impl.kcp);
                impl.kcp = nullptr;
        }
        if (impl.socket_fd != kInvalidSocket) {
                CloseSocket(impl.socket_fd);
                impl.socket_fd = kInvalidSocket;
        }
}

void UpdateSchedule(ClientImpl &impl, std::uint64_t now_ms) {
        if (!impl.connected || impl.kcp == nullptr || ikcp_waitsnd(impl.kcp) <= 0) {
                impl.next_update_ms = 0;
                return;
        }
        impl.next_update_ms = static_cast<std::uint64_t>(ikcp_check(impl.kcp, static_cast<IUINT32>(now_ms)));
}

void DrainKcpMessages(ClientImpl &impl) {
        std::vector<std::uint8_t> scratch;
        while (true) {
                const int size = ikcp_peeksize(impl.kcp);
                if (size < 0) { return; }
                scratch.resize(static_cast<std::size_t>(size));
                const int received = ikcp_recv(impl.kcp, reinterpret_cast<char *>(scratch.data()), size);
                if (received < 0) { return; }
                impl.recv_messages.emplace_back(scratch.begin(), scratch.begin() + received);
        }
}

bool DriveKcp(ClientImpl &impl, std::uint64_t now_ms) {
        if (!impl.connected || impl.kcp == nullptr) { return false; }
        ikcp_update(impl.kcp, static_cast<IUINT32>(now_ms));
        DrainKcpMessages(impl);
        UpdateSchedule(impl, now_ms);
        return true;
}

bool DrainSocket(ClientImpl &impl) {
        bool progressed = false;
        std::string error;
        while (true) {
                error.clear();
                Datagram datagram = ReceiveConnectedDatagram(impl.socket_fd, impl.read_buffer.data(), impl.read_buffer.size(), error);
                if (!error.empty() || datagram.size == 0) { return progressed; }

                Header header{};
                std::span<const std::uint8_t> body;
                if (!Header::SplitPacket(std::span<const std::uint8_t>(impl.read_buffer.data(), datagram.size), header, body) || header.sess_id != impl.sess_id) {
                        continue;
                }

                progressed = true;
                if (header.msg_type == MsgType::Udp) {
                        impl.recv_messages.emplace_back(body.begin(), body.end());
                        continue;
                }

                if (ikcp_input(impl.kcp, reinterpret_cast<const char *>(body.data()), static_cast<long>(body.size())) != 0) { continue; }
                DriveKcp(impl, NowMs());
        }
}

std::chrono::milliseconds ComputePollTimeout(const ClientImpl &impl, std::chrono::milliseconds timeout, std::uint64_t now_ms) {
        if (timeout.count() <= 0) { return std::chrono::milliseconds(0); }
        if (impl.next_update_ms == 0 || impl.next_update_ms <= now_ms) { return std::chrono::milliseconds(0); }
        const auto until_update = std::chrono::milliseconds(impl.next_update_ms - now_ms);
        return (std::min)(timeout, until_update);
}

bool OpenClientSocket(ClientImpl &impl) {
        sockaddr_in remote_addr{};
        if (!ParseListenAddress(impl.server_addr, remote_addr)) { return false; }

        Endpoint remote{};
        std::memcpy(&remote.storage, &remote_addr, sizeof(remote_addr));
        remote.length = sizeof(remote_addr);

        sockaddr_in local_addr{};
        if (!ParseListenAddress("0.0.0.0:0", local_addr)) { return false; }

        std::string error;
        if (!OpenConnectedUdpSocket(local_addr, remote, impl.socket_fd, error)) { return false; }
        return impl.poller.Open(impl.socket_fd, error);
}

} // namespace

Client::Client(std::string server_addr, std::uint32_t sess_id, Config config) : impl_(std::make_unique<ClientImpl>()) {
        impl_->server_addr = std::move(server_addr);
        impl_->sess_id = sess_id;
        impl_->config = config;
        impl_->read_buffer.resize(config.recv_buffer_size);
}

Client::~Client() { Close(); }

bool Client::Connect(std::span<const std::uint8_t> auth_payload) {
        if (!impl_ || impl_->connected || auth_payload.empty()) { return false; }

        if (!OpenClientSocket(*impl_)) {
                ReleaseClient(*impl_);
                return false;
        }

        impl_->kcp = ikcp_create(impl_->sess_id, impl_.get());
        if (impl_->kcp == nullptr) {
                ReleaseClient(*impl_);
                return false;
        }

        ConfigureKcp(*impl_->kcp, impl_->config);
        impl_->kcp->output = &ClientOutput;
        impl_->connected = true;

        impl_->output_flags = HeaderFlags::Connect;
        const bool sent = SendKcp(auth_payload);
        impl_->output_flags = HeaderFlags::None;
        if (!sent) {
                ReleaseClient(*impl_);
                return false;
        }
        return true;
}

void Client::Close() {
        if (!impl_) { return; }
        ReleaseClient(*impl_);
}

bool Client::IsConnected() const noexcept { return impl_ != nullptr && impl_->connected; }

bool Client::SendKcp(std::span<const std::uint8_t> payload) {
        if (!impl_ || !impl_->connected || impl_->kcp == nullptr || payload.size() > MaxKcpPayloadSize(*impl_->kcp)) { return false; }
        if (ikcp_send(impl_->kcp, reinterpret_cast<const char *>(payload.data()), static_cast<int>(payload.size())) < 0) { return false; }
        return DriveKcp(*impl_, NowMs());
}

bool Client::SendUdp(std::uint32_t packet_seq, std::span<const std::uint8_t> payload) {
        if (!impl_ || !impl_->connected) { return false; }
        return EncodeWrappedConnectedPacket(impl_->socket_fd, MsgType::Udp, HeaderFlags::None, impl_->sess_id, packet_seq, payload);
}

bool Client::Poll(std::chrono::milliseconds timeout) {
        if (!impl_ || !impl_->connected || impl_->socket_fd == kInvalidSocket || impl_->kcp == nullptr) { return false; }

        bool progressed = false;
        const auto now_ms = NowMs();
        if (impl_->next_update_ms != 0 && impl_->next_update_ms <= now_ms) {
                progressed = DriveKcp(*impl_, now_ms) || progressed;
        }

        const auto wait_timeout = ComputePollTimeout(*impl_, timeout, now_ms);
        std::vector<SocketHandle> ready;
        std::string error;
        const bool readable = impl_->poller.Wait(wait_timeout, ready, error);
        if (!error.empty()) { return progressed; }
        if (readable) { progressed = DrainSocket(*impl_) || progressed; }

        const auto after_wait_ms = NowMs();
        if (impl_->next_update_ms != 0 && impl_->next_update_ms <= after_wait_ms) {
                progressed = DriveKcp(*impl_, after_wait_ms) || progressed;
        }
        return progressed;
}

bool Client::Recv(std::vector<std::uint8_t> &out) {
        if (!impl_ || impl_->recv_messages.empty()) { return false; }
        out = std::move(impl_->recv_messages.front());
        impl_->recv_messages.pop_front();
        return true;
}

} // namespace ukcp
