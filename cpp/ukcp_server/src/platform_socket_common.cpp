#include "platform_socket.hpp"

#include <chrono>

namespace ukcp {

std::string Endpoint::ToString() const {
        if (!valid()) { return {}; }

        char host[INET6_ADDRSTRLEN] = {};
        std::uint16_t port = 0;
        if (storage.ss_family == AF_INET) {
                const auto *addr = reinterpret_cast<const sockaddr_in *>(&storage);
                inet_ntop(AF_INET, &addr->sin_addr, host, sizeof(host));
                port = ntohs(addr->sin_port);
        } else if (storage.ss_family == AF_INET6) {
                const auto *addr = reinterpret_cast<const sockaddr_in6 *>(&storage);
                inet_ntop(AF_INET6, &addr->sin6_addr, host, sizeof(host));
                port = ntohs(addr->sin6_port);
        }
        return std::string(host) + ":" + std::to_string(port);
}

bool Endpoint::Equals(const Endpoint &other) const noexcept {
        if (length != other.length || storage.ss_family != other.storage.ss_family) { return false; }
        return std::memcmp(&storage, &other.storage, length) == 0;
}

bool ParseListenAddress(std::string_view text, sockaddr_in &out) {
        const auto split = text.rfind(':');
        if (split == std::string_view::npos) { return false; }

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

std::uint64_t NowMs() noexcept {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

} // namespace ukcp
