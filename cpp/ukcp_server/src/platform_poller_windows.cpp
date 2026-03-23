#include "platform_socket.hpp"

#include <algorithm>

namespace ukcp {

struct Poller::Impl {
        std::vector<SocketHandle> sockets;
};

Poller::Poller() = default;
Poller::~Poller() { Close(); }

bool Poller::Open(SocketHandle socket_fd, std::string &error) {
        std::lock_guard lock(mutex_);
        if (impl_ == nullptr) { impl_ = new Impl(); }
        impl_->sockets.clear();
        impl_->sockets.push_back(socket_fd);
        error.clear();
        return true;
}

bool Poller::Register(SocketHandle socket_fd, std::string &error) {
        std::lock_guard lock(mutex_);
        if (impl_ == nullptr) { impl_ = new Impl(); }
        if (std::find(impl_->sockets.begin(), impl_->sockets.end(), socket_fd) == impl_->sockets.end()) { impl_->sockets.push_back(socket_fd); }
        error.clear();
        return true;
}

void Poller::Unregister(SocketHandle socket_fd) noexcept {
        std::lock_guard lock(mutex_);
        if (impl_ == nullptr) { return; }
        impl_->sockets.erase(std::remove(impl_->sockets.begin(), impl_->sockets.end(), socket_fd), impl_->sockets.end());
}

bool Poller::Wait(std::chrono::milliseconds timeout, std::vector<SocketHandle> &ready, std::string &error) {
        ready.clear();
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (ready.empty()) {
                std::vector<SocketHandle> sockets_copy;
                {
                        std::lock_guard lock(mutex_);
                        if (impl_ == nullptr) { return false; }
                        sockets_copy = impl_->sockets;
                }
                if (sockets_copy.empty()) { return false; }

                fd_set set;
                FD_ZERO(&set);
                SocketHandle max_fd = 0;
                for (SocketHandle socket_fd : sockets_copy) {
                        FD_SET(socket_fd, &set);
                        max_fd = (std::max)(max_fd, socket_fd);
                }

                timeval tv{};
                tv.tv_sec = 0;
                tv.tv_usec = 1000;
                const int rc = select(static_cast<int>(max_fd + 1), &set, nullptr, nullptr, &tv);
                if (rc < 0) {
                        error = "select failed";
                        return false;
                }
                if (rc > 0) {
                        for (SocketHandle socket_fd : sockets_copy) {
                                if (FD_ISSET(socket_fd, &set)) { ready.push_back(socket_fd); }
                        }
                        return !ready.empty();
                }
                if (timeout.count() == 0 || std::chrono::steady_clock::now() >= deadline) { return false; }
        }
        return true;
}

void Poller::Close() noexcept {
        std::lock_guard lock(mutex_);
        delete impl_;
        impl_ = nullptr;
}

} // namespace ukcp
