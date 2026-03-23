#pragma once

#include <atomic>
#include <chrono>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "ikcp.h"
#include "platform_socket.hpp"
#include "ukcp/config.hpp"
#include "ukcp/handler.hpp"
#include "ukcp/protocol.hpp"
#include "ukcp/server.hpp"
#include "ukcp/session.hpp"

namespace ukcp {

#ifndef UKCP_ENABLE_STATS
#define UKCP_ENABLE_STATS 0
#endif

struct SessionImpl {
        ServerImpl *server{nullptr};
        Session *public_session{nullptr};
        std::uint32_t sess_id{0};
        Endpoint remote{};
        SocketHandle socket_fd{kInvalidSocket};
        ikcpcb *kcp{nullptr};
        mutable std::shared_mutex mutex;
        std::vector<std::uint8_t> recv_buffer;
        std::uint64_t last_kcp_ms{0};
        std::atomic<std::uint64_t> next_update_ms{0};
        bool closed{false};
        std::string close_reason{"session closed"};
};

struct PendingAuth {
        std::uint32_t sess_id{0};
        Endpoint remote{};
        ikcpcb *kcp{nullptr};
        std::vector<std::uint8_t> recv_buffer;
};

struct ServerImpl {
        std::string listen_addr;
        Handler *handler{nullptr};
        Config config{};
        sockaddr_in listen_sockaddr{};
        SocketHandle socket_fd{kInvalidSocket};
        Poller poller{};

        std::atomic<bool> running{false};
        std::thread recv_thread{};
        std::thread update_thread{};

        mutable std::shared_mutex mutex;
        std::unordered_map<std::uint32_t, std::unique_ptr<Session>> sessions;
        std::unordered_map<std::uint32_t, PendingAuth> pending;
        std::unordered_map<SocketHandle, std::uint32_t> session_sockets;

#if UKCP_ENABLE_STATS
        std::atomic<std::uint64_t> recv_packets{0};
        std::atomic<std::uint64_t> recv_bytes{0};
        std::atomic<std::uint64_t> recv_kcp_packets{0};
        std::atomic<std::uint64_t> recv_udp_packets{0};
        std::atomic<std::uint64_t> sent_packets{0};
        std::atomic<std::uint64_t> sent_bytes{0};
        std::atomic<std::uint64_t> sent_kcp_packets{0};
        std::atomic<std::uint64_t> sent_udp_packets{0};
#endif
        std::uint64_t scheduler_interval_ms{10};

        std::vector<std::uint8_t> read_buffer;
        std::vector<SocketHandle> ready_sockets;

        [[nodiscard]] Session *FindSessionLocked(std::uint32_t sess_id) const;
        [[nodiscard]] Session *FindSessionBySocketLocked(SocketHandle socket_fd) const;
};

std::vector<Session *> &SessionScratch();

int SessionOutput(const char *buf, int len, ikcpcb *kcp, void *user);
int PendingOutput(const char *buf, int len, ikcpcb *kcp, void *user);

void ReleaseSessionResources(ServerImpl &impl, SessionImpl &session_impl);
Session *ActivatePending(ServerImpl &impl, PendingAuth &&pending);
void CloseSessionIfNeeded(ServerImpl &impl, Session &session);
void HandleMessages(ServerImpl &impl, Session &session, const std::vector<std::vector<std::uint8_t>> &messages);
void HandlePendingKcp(ServerImpl &impl, std::uint32_t sess_id, const Endpoint &endpoint, std::span<const std::uint8_t> payload);
void HandleKcpOnSession(ServerImpl &impl, Session &session, const Endpoint &endpoint, std::span<const std::uint8_t> payload);
bool DecodePacket(ServerImpl &impl, std::span<const std::uint8_t> packet, Header &header, std::span<const std::uint8_t> &body);
void RunRecvLoop(ServerImpl &impl);
void RunUpdateLoop(ServerImpl &impl);

void ConfigureKcp(ikcpcb &kcp, const Config &config);

std::vector<std::vector<std::uint8_t>> DrainKcpMessages(ikcpcb &kcp, std::vector<std::uint8_t> &scratch);

bool SendWrappedKcp(SocketHandle socket_fd, const Endpoint &endpoint, std::uint32_t sess_id, HeaderFlags flags, std::span<const std::uint8_t> payload);
bool SendWrappedConnectedKcp(SocketHandle socket_fd, std::uint32_t sess_id, HeaderFlags flags, std::span<const std::uint8_t> payload);
bool SendWrappedUdp(SocketHandle socket_fd, const Endpoint &endpoint, std::uint32_t sess_id, std::uint32_t packet_seq, std::span<const std::uint8_t> payload);
bool SendWrappedConnectedUdp(SocketHandle socket_fd, std::uint32_t sess_id, std::uint32_t packet_seq, std::span<const std::uint8_t> payload);

bool SameEndpoint(const Endpoint &a, const Endpoint &b) noexcept;

void ScheduleSessionUpdate(ServerImpl &impl, Session &session, std::uint64_t due_ms);

inline void RecordRecvStats(ServerImpl &impl, MsgType msg_type, std::size_t bytes) {
#if UKCP_ENABLE_STATS
        impl.recv_packets.fetch_add(1, std::memory_order_relaxed);
        impl.recv_bytes.fetch_add(bytes, std::memory_order_relaxed);
        if (msg_type == MsgType::Kcp) {
                impl.recv_kcp_packets.fetch_add(1, std::memory_order_relaxed);
        } else if (msg_type == MsgType::Udp) {
                impl.recv_udp_packets.fetch_add(1, std::memory_order_relaxed);
        }
#else
        (void)impl;
        (void)msg_type;
        (void)bytes;
#endif
}

inline void RecordSentKcpStats(ServerImpl &impl, std::size_t bytes) {
#if UKCP_ENABLE_STATS
        impl.sent_packets.fetch_add(1, std::memory_order_relaxed);
        impl.sent_kcp_packets.fetch_add(1, std::memory_order_relaxed);
        impl.sent_bytes.fetch_add(Header::kSize + static_cast<std::uint64_t>(bytes), std::memory_order_relaxed);
#else
        (void)impl;
        (void)bytes;
#endif
}

inline void RecordSentUdpStats(ServerImpl &impl, std::size_t bytes) {
#if UKCP_ENABLE_STATS
        impl.sent_packets.fetch_add(1, std::memory_order_relaxed);
        impl.sent_udp_packets.fetch_add(1, std::memory_order_relaxed);
        impl.sent_bytes.fetch_add(Header::kSize + static_cast<std::uint64_t>(bytes), std::memory_order_relaxed);
#else
        (void)impl;
        (void)bytes;
#endif
}

} // namespace ukcp
