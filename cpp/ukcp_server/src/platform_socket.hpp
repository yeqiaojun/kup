#pragma once

#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#define UKCP_PLATFORM_WINDOWS 1
#include <WinSock2.h>
#include <WS2tcpip.h>
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
#define UKCP_PLATFORM_WINDOWS 0
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
#endif

namespace ukcp {

struct Endpoint {
        sockaddr_storage storage{};
        socklen_t length{0};

        [[nodiscard]] bool valid() const noexcept { return length > 0; }
        [[nodiscard]] std::string ToString() const;
        [[nodiscard]] bool Equals(const Endpoint &other) const noexcept;
};

struct Datagram {
        Endpoint endpoint{};
        std::size_t size{0};
};

bool ParseListenAddress(std::string_view text, sockaddr_in &out);
bool OpenUdpSocket(const sockaddr_in &addr, SocketHandle &out, std::string &error);
bool OpenConnectedUdpSocket(const sockaddr_in &local_addr, const Endpoint &remote, SocketHandle &out, std::string &error);
void CloseSocket(SocketHandle fd) noexcept;
bool SetNonBlocking(SocketHandle fd, std::string &error);
bool SendDatagram(SocketHandle fd, const Endpoint &endpoint, const std::uint8_t *data, std::size_t size);
bool SendConnectedDatagram(SocketHandle fd, const std::uint8_t *data, std::size_t size);
Datagram ReceiveDatagram(SocketHandle fd, std::uint8_t *buffer, std::size_t capacity, std::string &error);
Datagram ReceiveConnectedDatagram(SocketHandle fd, std::uint8_t *buffer, std::size_t capacity, std::string &error);
std::uint64_t NowMs() noexcept;

class Poller {
      public:
        struct Impl;

        Poller();
        ~Poller();

        Poller(const Poller &) = delete;
        Poller &operator=(const Poller &) = delete;

        bool Open(SocketHandle socket_fd, std::string &error);
        bool Register(SocketHandle socket_fd, std::string &error);
        void Unregister(SocketHandle socket_fd) noexcept;
        bool Wait(std::chrono::milliseconds timeout, std::vector<SocketHandle> &ready, std::string &error);
        void Close() noexcept;

      private:
        std::mutex mutex_;
        Impl *impl_{nullptr};
};

} // namespace ukcp
