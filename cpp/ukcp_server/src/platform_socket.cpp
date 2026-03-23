#include "platform_socket.hpp"

#include <chrono>
#include <cstdio>

namespace ukcp {

namespace {

std::string LastSocketError()
{
#if UKCP_PLATFORM_WINDOWS
    return "socket error " + std::to_string(WSAGetLastError());
#else
    return std::strerror(errno);
#endif
}

bool ConfigureUdpSocket(SocketHandle fd, std::string& error)
{
    if (!SetNonBlocking(fd, error)) {
        return false;
    }

    int value = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&value), sizeof(value));
#if !UKCP_PLATFORM_WINDOWS && defined(SO_REUSEPORT)
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &value, sizeof(value));
#endif

    const int buffer_bytes = 4 * 1024 * 1024;
    setsockopt(
        fd,
        SOL_SOCKET,
        SO_RCVBUF,
        reinterpret_cast<const char*>(&buffer_bytes),
        sizeof(buffer_bytes));
    setsockopt(
        fd,
        SOL_SOCKET,
        SO_SNDBUF,
        reinterpret_cast<const char*>(&buffer_bytes),
        sizeof(buffer_bytes));
    return true;
}

Datagram ReceiveCommon(
    SocketHandle fd,
    std::uint8_t* buffer,
    std::size_t capacity,
    std::string& error,
    bool connected)
{
    Datagram out{};
    if (!connected) {
        out.endpoint.length = sizeof(out.endpoint.storage);
    }

    const auto rc = connected
        ? ::recv(fd, reinterpret_cast<char*>(buffer), static_cast<int>(capacity), 0)
        : ::recvfrom(
              fd,
              reinterpret_cast<char*>(buffer),
              static_cast<int>(capacity),
              0,
              reinterpret_cast<sockaddr*>(&out.endpoint.storage),
              &out.endpoint.length);
    if (rc < 0) {
#if UKCP_PLATFORM_WINDOWS
        const int code = WSAGetLastError();
        if (code == WSAEWOULDBLOCK) {
            return out;
        }
#else
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return out;
        }
#endif
        error = LastSocketError();
        return out;
    }

    out.size = static_cast<std::size_t>(rc);
    return out;
}

}  // namespace

std::string Endpoint::ToString() const
{
    if (!valid()) {
        return {};
    }

    char host[INET6_ADDRSTRLEN] = {};
    std::uint16_t port = 0;
    if (storage.ss_family == AF_INET) {
        const auto* addr = reinterpret_cast<const sockaddr_in*>(&storage);
        inet_ntop(AF_INET, &addr->sin_addr, host, sizeof(host));
        port = ntohs(addr->sin_port);
    } else if (storage.ss_family == AF_INET6) {
        const auto* addr = reinterpret_cast<const sockaddr_in6*>(&storage);
        inet_ntop(AF_INET6, &addr->sin6_addr, host, sizeof(host));
        port = ntohs(addr->sin6_port);
    }
    return std::string(host) + ":" + std::to_string(port);
}

bool Endpoint::Equals(const Endpoint& other) const noexcept
{
    if (length != other.length || storage.ss_family != other.storage.ss_family) {
        return false;
    }
    return std::memcmp(&storage, &other.storage, length) == 0;
}

bool ParseListenAddress(std::string_view text, sockaddr_in& out)
{
    const auto split = text.rfind(':');
    if (split == std::string_view::npos) {
        return false;
    }

    const std::string host(text.substr(0, split));
    const std::string port_text(text.substr(split + 1));
    const int port = std::stoi(port_text);

    std::memset(&out, 0, sizeof(out));
    out.sin_family = AF_INET;
    out.sin_port = htons(static_cast<std::uint16_t>(port));
    if (host.empty() || host == "0.0.0.0") {
        out.sin_addr.s_addr = htonl(INADDR_ANY);
        return true;
    }
    return inet_pton(AF_INET, host.c_str(), &out.sin_addr) == 1;
}

bool SetNonBlocking(SocketHandle fd, std::string& error)
{
#if UKCP_PLATFORM_WINDOWS
    u_long mode = 1;
    if (ioctlsocket(fd, FIONBIO, &mode) != 0) {
        error = LastSocketError();
        return false;
    }
#else
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        error = LastSocketError();
        return false;
    }
#endif
    return true;
}

bool OpenUdpSocket(const sockaddr_in& addr, SocketHandle& out, std::string& error)
{
#if UKCP_PLATFORM_WINDOWS
    static bool initialized = false;
    if (!initialized) {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            error = LastSocketError();
            return false;
        }
        initialized = true;
    }
#endif
    const SocketHandle fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd == kInvalidSocket) {
        error = LastSocketError();
        return false;
    }

    if (!ConfigureUdpSocket(fd, error)) {
        CloseSocket(fd);
        return false;
    }

    if (::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        error = LastSocketError();
        CloseSocket(fd);
        return false;
    }

    out = fd;
    return true;
}

bool OpenConnectedUdpSocket(
    const sockaddr_in& local_addr,
    const Endpoint& remote,
    SocketHandle& out,
    std::string& error)
{
    if (!remote.valid()) {
        error = "invalid remote endpoint";
        return false;
    }

    const SocketHandle fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd == kInvalidSocket) {
        error = LastSocketError();
        return false;
    }

    if (!ConfigureUdpSocket(fd, error)) {
        CloseSocket(fd);
        return false;
    }

    if (::bind(fd, reinterpret_cast<const sockaddr*>(&local_addr), sizeof(local_addr)) != 0) {
        error = LastSocketError();
        CloseSocket(fd);
        return false;
    }

    if (::connect(fd, reinterpret_cast<const sockaddr*>(&remote.storage), remote.length) != 0) {
        error = LastSocketError();
        CloseSocket(fd);
        return false;
    }

    out = fd;
    return true;
}

void CloseSocket(SocketHandle fd) noexcept
{
    if (fd == kInvalidSocket) {
        return;
    }
#if UKCP_PLATFORM_WINDOWS
    closesocket(fd);
#else
    close(fd);
#endif
}

bool SendDatagram(
    SocketHandle fd,
    const Endpoint& endpoint,
    const std::uint8_t* data,
    std::size_t size)
{
    if (!endpoint.valid()) {
        return false;
    }
    const auto rc = ::sendto(
        fd,
        reinterpret_cast<const char*>(data),
        static_cast<int>(size),
        0,
        reinterpret_cast<const sockaddr*>(&endpoint.storage),
        endpoint.length);
    return rc >= 0;
}

bool SendConnectedDatagram(SocketHandle fd, const std::uint8_t* data, std::size_t size)
{
    const auto rc = ::send(fd, reinterpret_cast<const char*>(data), static_cast<int>(size), 0);
    return rc >= 0;
}

Datagram ReceiveDatagram(
    SocketHandle fd,
    std::uint8_t* buffer,
    std::size_t capacity,
    std::string& error)
{
    return ReceiveCommon(fd, buffer, capacity, error, false);
}

Datagram ReceiveConnectedDatagram(
    SocketHandle fd,
    std::uint8_t* buffer,
    std::size_t capacity,
    std::string& error)
{
    return ReceiveCommon(fd, buffer, capacity, error, true);
}

std::uint64_t NowMs() noexcept
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

}  // namespace ukcp
