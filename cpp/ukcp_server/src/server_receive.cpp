#include "server_internal.hpp"

namespace ukcp {

namespace {

void RemovePendingLocked(ServerImpl &impl, std::uint32_t sess_id) {
        auto it = impl.pending.find(sess_id);
        if (it == impl.pending.end()) { return; }
        if (it->second.kcp != nullptr) {
                ikcp_release(it->second.kcp);
                it->second.kcp = nullptr;
        }
        impl.pending.erase(it);
}

PendingAuth &GetOrCreatePending(ServerImpl &impl, std::uint32_t sess_id, const Endpoint &endpoint) {
        auto it = impl.pending.find(sess_id);
        if (it != impl.pending.end()) {
                it->second.remote = endpoint;
                return it->second;
        }

        PendingAuth pending{};
        pending.sess_id = sess_id;
        pending.transport_mtu = impl.config.kcp.mtu;
        pending.remote = endpoint;
        pending.kcp = ikcp_create(sess_id, nullptr);
        pending.kcp->user = &pending;
        pending.kcp->output = &PendingOutput;
        ConfigureKcp(*pending.kcp, impl.config);

        auto [inserted, ok] = impl.pending.emplace(sess_id, std::move(pending));
        (void)ok;
        inserted->second.kcp->user = &inserted->second;
        return inserted->second;
}

void HandleListenerDatagram(ServerImpl &impl, const Datagram &datagram, std::span<const std::uint8_t> packet) {
        Header header{};
        std::span<const std::uint8_t> body;
        if (!DecodePacket(impl, packet, header, body)) { return; }

        Session *session = nullptr;
        {
                std::shared_lock lock(impl.mutex);
                session = impl.FindSessionLocked(header.sess_id);
        }

        if (session == nullptr) {
                if (header.msg_type == MsgType::Udp) { return; }
                HandlePendingKcp(impl, header.sess_id, datagram.endpoint, body);
                return;
        }

        SessionImpl *session_impl = session->raw_impl();
        bool same_endpoint = false;
        std::uint64_t last_kcp_ms = 0;
        {
                std::shared_lock lock(session_impl->mutex);
                same_endpoint = SameEndpoint(session_impl->remote, datagram.endpoint);
                last_kcp_ms = session_impl->last_kcp_ms;
        }
        if (!same_endpoint) {
                if (header.msg_type == MsgType::Kcp && (header.flags & HeaderFlags::Connect) == HeaderFlags::Connect) {
                        HandlePendingKcp(impl, header.sess_id, datagram.endpoint, body);
                        return;
                }
                if (header.msg_type == MsgType::Kcp && NowMs() - last_kcp_ms <= static_cast<std::uint64_t>(impl.config.fast_reconnect_window.count())) {
                        HandleKcpOnSession(impl, *session, datagram.endpoint, body);
                }
                CloseSessionIfNeeded(impl, *session);
                return;
        }

        if (header.msg_type == MsgType::Udp) {
                impl.handler->OnUDP(*session, header.packet_seq, body);
        } else {
                HandleKcpOnSession(impl, *session, datagram.endpoint, body);
        }
        CloseSessionIfNeeded(impl, *session);
}

void HandleSessionDatagram(ServerImpl &impl, Session &session, std::span<const std::uint8_t> packet) {
        Header header{};
        std::span<const std::uint8_t> body;
        if (!DecodePacket(impl, packet, header, body) || header.sess_id != session.id()) { return; }

        if (header.msg_type == MsgType::Udp) {
                impl.handler->OnUDP(session, header.packet_seq, body);
        } else {
                Endpoint remote{};
                {
                        std::shared_lock lock(session.raw_impl()->mutex);
                        remote = session.raw_impl()->remote;
                }
                HandleKcpOnSession(impl, session, remote, body);
        }
        CloseSessionIfNeeded(impl, session);
}

void DrainListenerSocket(ServerImpl &impl) {
        std::string error;
        while (impl.running.load(std::memory_order_relaxed)) {
                error.clear();
                Datagram datagram = ReceiveDatagram(impl.socket_fd, impl.read_buffer.data(), impl.read_buffer.size(), error);
                if (!error.empty() || datagram.size == 0) { return; }
                HandleListenerDatagram(impl, datagram, std::span<const std::uint8_t>(impl.read_buffer.data(), datagram.size));
        }
}

void DrainSessionSocket(ServerImpl &impl, SocketHandle socket_fd) {
        Session *session = nullptr;
        {
                std::shared_lock lock(impl.mutex);
                session = impl.FindSessionBySocketLocked(socket_fd);
        }
        if (session == nullptr) { return; }

        std::string error;
        while (impl.running.load(std::memory_order_relaxed)) {
                error.clear();
                Datagram datagram = ReceiveConnectedDatagram(socket_fd, impl.read_buffer.data(), impl.read_buffer.size(), error);
                if (!error.empty() || datagram.size == 0) { return; }
                HandleSessionDatagram(impl, *session, std::span<const std::uint8_t>(impl.read_buffer.data(), datagram.size));
        }
}

} // namespace

void HandleMessages(ServerImpl &impl, Session &session, const std::vector<std::vector<std::uint8_t>> &messages) {
        for (const auto &message : messages) { impl.handler->OnKCP(session, std::span<const std::uint8_t>(message.data(), message.size())); }
}

void HandlePendingKcp(ServerImpl &impl, std::uint32_t sess_id, const Endpoint &endpoint, std::span<const std::uint8_t> payload) {
        PendingAuth *pending_ptr = nullptr;
        {
                std::unique_lock lock(impl.mutex);
                auto &pending = GetOrCreatePending(impl, sess_id, endpoint);
                pending_ptr = &pending;
        }

        auto &pending = *pending_ptr;
        if (ikcp_input(pending.kcp, reinterpret_cast<const char *>(payload.data()), static_cast<long>(payload.size())) != 0) {
                std::unique_lock lock(impl.mutex);
                RemovePendingLocked(impl, sess_id);
                return;
        }

        const auto messages = DrainKcpMessages(*pending.kcp, pending.recv_buffer);
        if (messages.empty()) { return; }

        if (!impl.handler->Auth(sess_id, endpoint.ToString(), messages.front())) {
                std::unique_lock lock(impl.mutex);
                RemovePendingLocked(impl, sess_id);
                return;
        }

        PendingAuth activated{};
        {
                std::unique_lock lock(impl.mutex);
                auto it = impl.pending.find(sess_id);
                if (it == impl.pending.end()) { return; }
                activated = std::move(it->second);
                impl.pending.erase(it);
        }

        Session *session = ActivatePending(impl, std::move(activated));
        if (session == nullptr) { return; }

        std::uint64_t next_due_ms = 0;
        {
                std::unique_lock lock(session->raw_impl()->mutex);
                if (session->raw_impl()->kcp != nullptr && !session->raw_impl()->closed) {
                        const auto now_ms = NowMs();
                        ikcp_update(session->raw_impl()->kcp, static_cast<IUINT32>(now_ms));
                        if (ikcp_waitsnd(session->raw_impl()->kcp) > 0) {
                                next_due_ms = static_cast<std::uint64_t>(ikcp_check(session->raw_impl()->kcp, static_cast<IUINT32>(now_ms)));
                        }
                }
        }
        if (next_due_ms != 0) { ScheduleSessionUpdate(impl, *session, next_due_ms); }
}

void HandleKcpOnSession(ServerImpl &impl, Session &session, const Endpoint &endpoint, std::span<const std::uint8_t> payload) {
        std::vector<std::vector<std::uint8_t>> messages;
        std::uint64_t next_due_ms = 0;
        SessionImpl *session_impl = session.raw_impl();
        {
                std::unique_lock lock(session_impl->mutex);
                if (session_impl->kcp == nullptr || session_impl->closed) { return; }
                session_impl->remote = endpoint;
                if (ikcp_input(session_impl->kcp, reinterpret_cast<const char *>(payload.data()), static_cast<long>(payload.size())) != 0) { return; }
                const auto now_ms = NowMs();
                session_impl->last_kcp_ms = now_ms;
                ikcp_update(session_impl->kcp, static_cast<IUINT32>(now_ms));
                messages = DrainKcpMessages(*session_impl->kcp, session_impl->recv_buffer);
                if (ikcp_waitsnd(session_impl->kcp) > 0) {
                        next_due_ms = static_cast<std::uint64_t>(ikcp_check(session_impl->kcp, static_cast<IUINT32>(now_ms)));
                }
        }
        if (next_due_ms != 0) { ScheduleSessionUpdate(impl, session, next_due_ms); }
        HandleMessages(impl, session, messages);
}

bool DecodePacket(ServerImpl &impl, std::span<const std::uint8_t> packet, Header &header, std::span<const std::uint8_t> &body) {
        if (!Header::SplitPacket(packet, header, body)) { return false; }
        RecordRecvStats(impl, header.msg_type, packet.size());
        return true;
}

void RunRecvLoop(ServerImpl &impl) {
        std::string error;
        while (impl.running.load(std::memory_order_relaxed)) {
                error.clear();
                const bool readable = impl.poller.Wait(impl.config.update_interval, impl.ready_sockets, error);
                if (!impl.running.load(std::memory_order_relaxed)) { break; }
                if (!error.empty() || !readable) { continue; }

                for (SocketHandle socket_fd : impl.ready_sockets) {
                        if (socket_fd == impl.socket_fd) {
                                DrainListenerSocket(impl);
                        } else {
                                DrainSessionSocket(impl, socket_fd);
                        }
                        if (!impl.running.load(std::memory_order_relaxed)) { break; }
                }
        }
}

} // namespace ukcp
