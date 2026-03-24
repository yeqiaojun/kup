#include "server_internal.hpp"

#include <algorithm>

namespace ukcp {

namespace {

std::string ReasonReplaced() { return "session replaced"; }
std::string ReasonServerClosed() { return "server closed"; }

struct SessionResources {
        SocketHandle socket_fd{kInvalidSocket};
        ikcpcb *kcp{nullptr};
};

void FinalizeSessionClose(ServerImpl &impl, std::unique_ptr<Session> session, const std::string &close_reason) {
        if (session == nullptr) { return; }
        ReleaseSessionResources(impl, *session->raw_impl());
        impl.handler->OnSessionClose(*session, close_reason);
}

void CloseOwnedSession(ServerImpl &impl, std::unique_ptr<Session> session, const std::string &reason) {
        if (session == nullptr) { return; }
        session->Close(reason);
        std::string close_reason;
        {
                std::shared_lock lock(session->raw_impl()->mutex);
                close_reason = session->raw_impl()->close_reason;
        }
        FinalizeSessionClose(impl, std::move(session), close_reason);
}

SessionResources DetachSessionResources(SessionImpl &session_impl) {
        SessionResources resources{};
        std::unique_lock lock(session_impl.mutex);
        resources.socket_fd = session_impl.socket_fd;
        session_impl.socket_fd = kInvalidSocket;
        resources.kcp = session_impl.kcp;
        session_impl.kcp = nullptr;
        return resources;
}

void EraseSessionSocket(ServerImpl &impl, SocketHandle socket_fd) {
        if (socket_fd == kInvalidSocket) { return; }
        std::unique_lock lock(impl.mutex);
        impl.session_sockets.erase(socket_fd);
}

} // namespace

std::vector<Session *> &SessionScratch() {
        thread_local std::vector<Session *> scratch;
        return scratch;
}

void ReleaseSessionResources(ServerImpl &impl, SessionImpl &session_impl) {
        const auto resources = DetachSessionResources(session_impl);
        const auto socket_fd = resources.socket_fd;
        if (socket_fd != kInvalidSocket) {
                impl.poller.Unregister(socket_fd);
                EraseSessionSocket(impl, socket_fd);
                CloseSocket(socket_fd);
        }
        if (resources.kcp != nullptr) { ikcp_release(resources.kcp); }
}

Session *ActivatePending(ServerImpl &impl, PendingAuth &&pending) {
        auto session_impl = std::make_unique<SessionImpl>();
        session_impl->server = &impl;
        session_impl->sess_id = pending.sess_id;
        session_impl->remote = pending.remote;
        session_impl->kcp = pending.kcp;
        session_impl->recv_buffer = std::move(pending.recv_buffer);
        session_impl->last_kcp_ms = NowMs();
        if (session_impl->kcp != nullptr) {
                session_impl->kcp->user = session_impl.get();
                session_impl->kcp->output = &SessionOutput;
        }

        std::string error;
        if (!OpenConnectedUdpSocket(impl.listen_sockaddr, pending.remote, session_impl->socket_fd, error)) {
                if (session_impl->kcp != nullptr) {
                        ikcp_release(session_impl->kcp);
                        session_impl->kcp = nullptr;
                }
                return nullptr;
        }
        if (!impl.poller.Register(session_impl->socket_fd, error)) {
                CloseSocket(session_impl->socket_fd);
                session_impl->socket_fd = kInvalidSocket;
                if (session_impl->kcp != nullptr) {
                        ikcp_release(session_impl->kcp);
                        session_impl->kcp = nullptr;
                }
                return nullptr;
        }

        auto session = std::make_unique<Session>(std::move(session_impl));
        session->raw_impl()->public_session = session.get();

        std::unique_ptr<Session> replaced;
        Session *inserted_ptr = nullptr;
        {
                std::unique_lock lock(impl.mutex);
                auto it = impl.sessions.find(pending.sess_id);
                if (it != impl.sessions.end()) {
                        replaced = std::move(it->second);
                        impl.sessions.erase(it);
                }
                auto [inserted, ok] = impl.sessions.emplace(pending.sess_id, std::move(session));
                (void)ok;
                inserted_ptr = inserted->second.get();
                impl.session_sockets[inserted_ptr->raw_impl()->socket_fd] = pending.sess_id;
        }

        if (replaced != nullptr) {
                CloseOwnedSession(impl, std::move(replaced), ReasonReplaced());
        }

        impl.handler->OnSessionOpen(*inserted_ptr);
        return inserted_ptr;
}

void CloseSessionIfNeeded(ServerImpl &impl, Session &session) {
        SessionImpl *session_impl = session.raw_impl();
        if (session_impl == nullptr) { return; }
        {
                std::shared_lock lock(session_impl->mutex);
                if (!session_impl->closed) { return; }
        }

        std::unique_ptr<Session> owned;
        {
                std::unique_lock lock(impl.mutex);
                auto it = impl.sessions.find(session.id());
                if (it == impl.sessions.end() || it->second.get() != &session) { return; }
                owned = std::move(it->second);
                impl.sessions.erase(it);
        }

        std::string close_reason;
        {
                std::shared_lock lock(session_impl->mutex);
                close_reason = session_impl->close_reason;
        }
        FinalizeSessionClose(impl, std::move(owned), close_reason);
}

Session *ServerImpl::FindSessionLocked(std::uint32_t sess_id) const {
        auto it = sessions.find(sess_id);
        return it == sessions.end() ? nullptr : it->second.get();
}

Session *ServerImpl::FindSessionBySocketLocked(SocketHandle socket_fd) const {
        auto it = session_sockets.find(socket_fd);
        if (it == session_sockets.end()) { return nullptr; }
        return FindSessionLocked(it->second);
}

bool Server::Start() {
        if (impl_->running.load(std::memory_order_relaxed)) { return true; }

        if (!ParseListenAddress(impl_->listen_addr, impl_->listen_sockaddr)) { return false; }

        std::string error;
        if (!OpenUdpSocket(impl_->listen_sockaddr, impl_->socket_fd, error)) { return false; }
        if (!impl_->poller.Open(impl_->socket_fd, error)) {
                CloseSocket(impl_->socket_fd);
                impl_->socket_fd = kInvalidSocket;
                return false;
        }

        impl_->running.store(true, std::memory_order_relaxed);
        impl_->recv_thread = std::thread([impl = impl_.get()] { RunRecvLoop(*impl); });
        impl_->update_thread = std::thread([impl = impl_.get()] { RunUpdateLoop(*impl); });
        return true;
}

void Server::Close() {
        if (!impl_ || !impl_->running.exchange(false, std::memory_order_relaxed)) { return; }

        impl_->poller.Close();
        CloseSocket(impl_->socket_fd);
        impl_->socket_fd = kInvalidSocket;

        if (impl_->recv_thread.joinable()) { impl_->recv_thread.join(); }
        if (impl_->update_thread.joinable()) { impl_->update_thread.join(); }

        std::vector<std::unique_ptr<Session>> sessions;
        {
                std::unique_lock lock(impl_->mutex);
                for (auto &[_, pending] : impl_->pending) {
                        if (pending.kcp != nullptr) {
                                ikcp_release(pending.kcp);
                                pending.kcp = nullptr;
                        }
                }
                impl_->pending.clear();
                impl_->session_sockets.clear();
                for (auto &[_, session] : impl_->sessions) { sessions.push_back(std::move(session)); }
                impl_->sessions.clear();
        }

        for (auto &session : sessions) { CloseOwnedSession(*impl_, std::move(session), ReasonServerClosed()); }
}

bool Server::IsRunning() const noexcept { return impl_ && impl_->running.load(std::memory_order_relaxed); }

bool Server::CloseSession(std::uint32_t sess_id, const std::string &reason) {
        if (!impl_) { return false; }

        std::unique_ptr<Session> session;
        {
                std::unique_lock lock(impl_->mutex);
                auto it = impl_->sessions.find(sess_id);
                if (it == impl_->sessions.end()) { return false; }
                session = std::move(it->second);
                impl_->sessions.erase(it);
        }

        CloseOwnedSession(*impl_, std::move(session), reason);
        return true;
}

ServerStatsSnapshot Server::Stats() const {
        ServerStatsSnapshot snapshot{};
        if (!impl_) { return snapshot; }

#if UKCP_ENABLE_STATS
        snapshot.recv_packets = impl_->recv_packets.load(std::memory_order_relaxed);
        snapshot.recv_bytes = impl_->recv_bytes.load(std::memory_order_relaxed);
        snapshot.recv_kcp_packets = impl_->recv_kcp_packets.load(std::memory_order_relaxed);
        snapshot.recv_udp_packets = impl_->recv_udp_packets.load(std::memory_order_relaxed);
        snapshot.sent_packets = impl_->sent_packets.load(std::memory_order_relaxed);
        snapshot.sent_bytes = impl_->sent_bytes.load(std::memory_order_relaxed);
        snapshot.sent_kcp_packets = impl_->sent_kcp_packets.load(std::memory_order_relaxed);
        snapshot.sent_udp_packets = impl_->sent_udp_packets.load(std::memory_order_relaxed);
        {
                std::shared_lock lock(impl_->mutex);
                snapshot.active_sessions = static_cast<std::uint32_t>(impl_->sessions.size());
        }
#endif
        return snapshot;
}

Session *Server::FindSession(std::uint32_t sess_id) {
        std::shared_lock lock(impl_->mutex);
        return impl_->FindSessionLocked(sess_id);
}

} // namespace ukcp
