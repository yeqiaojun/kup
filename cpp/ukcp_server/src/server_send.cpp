#include "server_internal.hpp"

#include <algorithm>
#include <unordered_set>

namespace ukcp {

namespace {

bool SendToSessionIds(Server &server, const std::vector<std::uint32_t> &sess_ids, std::span<const std::uint8_t> payload, bool raw_udp, std::uint32_t packet_seq) {
        bool all_sent = true;
        if (sess_ids.size() <= 32) {
                std::vector<std::uint32_t> unique;
                unique.reserve(sess_ids.size());
                for (std::uint32_t sess_id : sess_ids) {
                        if (std::find(unique.begin(), unique.end(), sess_id) != unique.end()) { continue; }
                        unique.push_back(sess_id);
                        Session *session = server.FindSession(sess_id);
                        const bool sent = session != nullptr && (raw_udp ? session->SendUdp(packet_seq, payload) : session->SendKcp(payload));
                        all_sent = all_sent && sent;
                }
                return all_sent;
        }

        std::unordered_set<std::uint32_t> seen;
        seen.reserve(sess_ids.size());
        for (std::uint32_t sess_id : sess_ids) {
                if (!seen.insert(sess_id).second) { continue; }
                Session *session = server.FindSession(sess_id);
                const bool sent = session != nullptr && (raw_udp ? session->SendUdp(packet_seq, payload) : session->SendKcp(payload));
                all_sent = all_sent && sent;
        }
        return all_sent;
}

bool SendToSessionList(const std::vector<Session *> &sessions, std::span<const std::uint8_t> payload, bool raw_udp, std::uint32_t packet_seq) {
        bool all_sent = true;
        for (Session *session : sessions) {
                const bool sent = raw_udp ? session->SendUdp(packet_seq, payload) : session->SendKcp(payload);
                all_sent = all_sent && sent;
        }
        return all_sent;
}

} // namespace

bool Server::SendKcpToSess(std::uint32_t sess_id, std::span<const std::uint8_t> payload) {
        Session *session = FindSession(sess_id);
        return session != nullptr && session->SendKcp(payload);
}

bool Server::SendKcpToMultiSess(const std::vector<std::uint32_t> &sess_ids, std::span<const std::uint8_t> payload) {
        return SendToSessionIds(*this, sess_ids, payload, false, 0);
}

bool Server::SendKcpToAll(std::span<const std::uint8_t> payload) {
        auto &sessions = SessionScratch();
        sessions.clear();
        {
                std::shared_lock lock(impl_->mutex);
                sessions.reserve(impl_->sessions.size());
                for (auto &[_, session] : impl_->sessions) { sessions.push_back(session.get()); }
        }
        return SendToSessionList(sessions, payload, false, 0);
}

bool Server::SendUdpToSess(std::uint32_t sess_id, std::uint32_t packet_seq, std::span<const std::uint8_t> payload) {
        Session *session = FindSession(sess_id);
        return session != nullptr && session->SendUdp(packet_seq, payload);
}

bool Server::SendUdpToMultiSess(const std::vector<std::uint32_t> &sess_ids, std::uint32_t packet_seq, std::span<const std::uint8_t> payload) {
        return SendToSessionIds(*this, sess_ids, payload, true, packet_seq);
}

bool Server::SendUdpToAll(std::uint32_t packet_seq, std::span<const std::uint8_t> payload) {
        auto &sessions = SessionScratch();
        sessions.clear();
        {
                std::shared_lock lock(impl_->mutex);
                sessions.reserve(impl_->sessions.size());
                for (auto &[_, session] : impl_->sessions) { sessions.push_back(session.get()); }
        }
        return SendToSessionList(sessions, payload, true, packet_seq);
}

} // namespace ukcp
