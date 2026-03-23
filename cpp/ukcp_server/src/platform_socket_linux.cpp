#include "platform_socket.hpp"

#include <cerrno>

namespace ukcp {

namespace {

std::string LastSocketError() { return std::strerror(errno); }

bool ConfigureUdpSocket(SocketHandle fd, std::string &error) {
        if (!SetNonBlocking(fd, error)) { return false; }

        int value = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &value, sizeof(value));
#if defined(SO_REUSEPORT)
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &value, sizeof(value));
#endif

        const int buffer_bytes = 4 * 1024 * 1024;
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buffer_bytes, sizeof(buffer_bytes));
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buffer_bytes, sizeof(buffer_bytes));
        return true;
}

Datagram ReceiveCommon(SocketHandle fd, std::uint8_t *buffer, std::size_t capacity, std::string &error, bool connected) {
        Datagram out{};
        if (!connected) { out.endpoint.length = sizeof(out.endpoint.storage); }

        const auto rc = connected ? ::recv(fd, buffer, capacity, 0)
                                  : ::recvfrom(fd, buffer, capacity, 0, reinterpret_cast<sockaddr *>(&out.endpoint.storage), &out.endpoint.length);
        if (rc < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) { return out; }
                error = LastSocketError();
                return out;
        }

        out.size = static_cast<std::size_t>(rc);
        return out;
}

} // namespace

bool SetNonBlocking(SocketHandle fd, std::string &error) {
        const int flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
                error = LastSocketError();
                return false;
        }
        return true;
}

bool OpenUdpSocket(const sockaddr_in &addr, SocketHandle &out, std::string &error) {
        const SocketHandle fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (fd == kInvalidSocket) {
                error = LastSocketError();
                return false;
        }

        if (!ConfigureUdpSocket(fd, error)) {
                CloseSocket(fd);
                return false;
        }

        if (::bind(fd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0) {
                error = LastSocketError();
                CloseSocket(fd);
                return false;
        }

        out = fd;
        return true;
}

bool OpenConnectedUdpSocket(const sockaddr_in &local_addr, const Endpoint &remote, SocketHandle &out, std::string &error) {
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

        if (::bind(fd, reinterpret_cast<const sockaddr *>(&local_addr), sizeof(local_addr)) != 0) {
                error = LastSocketError();
                CloseSocket(fd);
                return false;
        }

        if (::connect(fd, reinterpret_cast<const sockaddr *>(&remote.storage), remote.length) != 0) {
                error = LastSocketError();
                CloseSocket(fd);
                return false;
        }

        out = fd;
        return true;
}

void CloseSocket(SocketHandle fd) noexcept {
        if (fd != kInvalidSocket) { close(fd); }
}

bool SendDatagram(SocketHandle fd, const Endpoint &endpoint, const std::uint8_t *data, std::size_t size) {
        if (!endpoint.valid()) { return false; }
        const auto rc = ::sendto(fd, data, size, 0, reinterpret_cast<const sockaddr *>(&endpoint.storage), endpoint.length);
        return rc >= 0;
}

bool SendConnectedDatagram(SocketHandle fd, const std::uint8_t *data, std::size_t size) { return ::send(fd, data, size, 0) >= 0; }

Datagram ReceiveDatagram(SocketHandle fd, std::uint8_t *buffer, std::size_t capacity, std::string &error) {
        return ReceiveCommon(fd, buffer, capacity, error, false);
}

Datagram ReceiveConnectedDatagram(SocketHandle fd, std::uint8_t *buffer, std::size_t capacity, std::string &error) {
        return ReceiveCommon(fd, buffer, capacity, error, true);
}

} // namespace ukcp
