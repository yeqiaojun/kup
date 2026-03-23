#include "server_internal.hpp"

namespace ukcp {

namespace {

void QueueSessionUpdate(ServerImpl &impl, SessionImpl &session_impl, std::uint64_t due_ms) {
        (void)impl;
        if (session_impl.public_session == nullptr) { return; }
        {
                std::shared_lock lock(session_impl.mutex);
                if (session_impl.closed) { return; }
        }

        const auto current_due_ms = session_impl.next_update_ms.load(std::memory_order_relaxed);
        if (current_due_ms != 0 && current_due_ms <= due_ms) { return; }

        session_impl.next_update_ms.store(due_ms, std::memory_order_relaxed);
}

bool RunScheduledUpdate(ServerImpl &impl, Session &session, std::uint64_t now_ms) {
        SessionImpl *session_impl = session.raw_impl();
        if (session_impl == nullptr) { return false; }

        std::vector<std::vector<std::uint8_t>> messages;
        std::uint64_t next_due_ms = 0;
        {
                std::unique_lock lock(session_impl->mutex);
                if (session_impl->closed || session_impl->kcp == nullptr) {
                        session_impl->next_update_ms.store(0, std::memory_order_relaxed);
                        return false;
                }

                const auto scheduled_due = session_impl->next_update_ms.load(std::memory_order_relaxed);
                if (scheduled_due == 0 || scheduled_due > now_ms) { return false; }

                session_impl->next_update_ms.store(0, std::memory_order_relaxed);
                ikcp_update(session_impl->kcp, static_cast<IUINT32>(now_ms));
                messages = DrainKcpMessages(*session_impl->kcp, session_impl->recv_buffer);
                if (ikcp_waitsnd(session_impl->kcp) > 0) {
                        next_due_ms = static_cast<std::uint64_t>(ikcp_check(session_impl->kcp, static_cast<IUINT32>(now_ms)));
                }
        }
        if (next_due_ms != 0) { ScheduleSessionUpdate(impl, session, next_due_ms); }
        HandleMessages(impl, session, messages);
        CloseSessionIfNeeded(impl, session);
        return true;
}

} // namespace

void ScheduleSessionUpdate(ServerImpl &impl, Session &session, std::uint64_t due_ms) {
        SessionImpl *session_impl = session.raw_impl();
        if (session_impl == nullptr) { return; }
        QueueSessionUpdate(impl, *session_impl, due_ms);
}

void RunUpdateLoop(ServerImpl &impl) {
        const auto interval = std::chrono::milliseconds(impl.scheduler_interval_ms);
        auto next_due = std::chrono::steady_clock::now() + interval;
        while (impl.running.load(std::memory_order_relaxed)) {
                const auto now = std::chrono::steady_clock::now();
                if (next_due > now) { std::this_thread::sleep_until(next_due); }
                if (!impl.running.load(std::memory_order_relaxed)) { break; }

                const auto now_ms = NowMs();
                auto &sessions = SessionScratch();
                sessions.clear();
                {
                        std::shared_lock lock(impl.mutex);
                        sessions.reserve(impl.sessions.size());
                        for (auto &[_, session] : impl.sessions) { sessions.push_back(session.get()); }
                }

                for (Session *session : sessions) {
                        SessionImpl *session_impl = session->raw_impl();
                        if (session_impl == nullptr) { continue; }
                        const auto due_ms = session_impl->next_update_ms.load(std::memory_order_relaxed);
                        if (due_ms == 0 || due_ms > now_ms) { continue; }
                        RunScheduledUpdate(impl, *session, now_ms);
                }
                next_due += interval;
        }
}

} // namespace ukcp
