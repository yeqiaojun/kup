#include "server_internal.hpp"

#include <algorithm>
#include <unordered_set>

namespace ukcp {

namespace {

SendReport SendToSessionIds(Server &server, const std::vector<std::uint32_t> &sess_ids, std::span<const std::uint8_t> payload, bool raw_udp,
                            std::uint32_t packet_seq) {
        SendReport report{};
        if (sess_ids.size() <= 32) {
                std::vector<std::uint32_t> unique;
                unique.reserve(sess_ids.size());
                for (std::uint32_t sess_id : sess_ids) {
                        if (std::find(unique.begin(), unique.end(), sess_id) != unique.end()) { continue; }
                        unique.push_back(sess_id);
                        ++report.attempted;
                        Session *session = server.FindSession(sess_id);
                        const bool sent = session != nullptr && (raw_udp ? session->SendRawUdp(packet_seq, payload) : session->Send(payload));
                        if (sent) {
                                ++report.sent;
                        } else {
                                ++report.failed;
                        }
                }
                return report;
        }

        std::unordered_set<std::uint32_t> seen;
        seen.reserve(sess_ids.size());
        for (std::uint32_t sess_id : sess_ids) {
                if (!seen.insert(sess_id).second) { continue; }
                ++report.attempted;
                Session *session = server.FindSession(sess_id);
                const bool sent = session != nullptr && (raw_udp ? session->SendRawUdp(packet_seq, payload) : session->Send(payload));
                if (sent) {
                        ++report.sent;
                } else {
                        ++report.failed;
                }
        }
        return report;
}

SendReport SendToSessionList(const std::vector<Session *> &sessions, std::span<const std::uint8_t> payload, bool raw_udp, std::uint32_t packet_seq) {
        SendReport report{};
        report.attempted = static_cast<int>(sessions.size());
        for (Session *session : sessions) {
                const bool sent = raw_udp ? session->SendRawUdp(packet_seq, payload) : session->Send(payload);
                if (sent) {
                        ++report.sent;
                } else {
                        ++report.failed;
                }
        }
        return report;
}

} // namespace

bool Server::SendToSess(std::uint32_t sess_id, std::span<const std::uint8_t> payload) {
        Session *session = FindSession(sess_id);
        return session != nullptr && session->Send(payload);
}

SendReport Server::SendToMultiSess(const std::vector<std::uint32_t> &sess_ids, std::span<const std::uint8_t> payload) {
        return SendToSessionIds(*this, sess_ids, payload, false, 0);
}

SendReport Server::SendToAll(std::span<const std::uint8_t> payload) {
        auto &sessions = SessionScratch();
        sessions.clear();
        {
                std::shared_lock lock(impl_->mutex);
                sessions.reserve(impl_->sessions.size());
                for (auto &[_, session] : impl_->sessions) { sessions.push_back(session.get()); }
        }
        return SendToSessionList(sessions, payload, false, 0);
}

bool Server::SendRawUdpToSess(std::uint32_t sess_id, std::uint32_t packet_seq, std::span<const std::uint8_t> payload) {
        Session *session = FindSession(sess_id);
        return session != nullptr && session->SendRawUdp(packet_seq, payload);
}

SendReport Server::SendRawUdpToMultiSess(const std::vector<std::uint32_t> &sess_ids, std::uint32_t packet_seq, std::span<const std::uint8_t> payload) {
        return SendToSessionIds(*this, sess_ids, payload, true, packet_seq);
}

SendReport Server::SendRawUdpToAll(std::uint32_t packet_seq, std::span<const std::uint8_t> payload) {
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
