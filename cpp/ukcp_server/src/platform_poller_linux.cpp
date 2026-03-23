#include "platform_socket.hpp"

#include <array>
#include <cerrno>
#include <climits>
#include <cstring>
#include <unordered_set>

#include <sys/epoll.h>

namespace ukcp {

struct Poller::Impl {
        int epoll_fd{-1};
        std::unordered_set<SocketHandle> registered_sockets;
};

namespace {

std::string LastErrno() { return std::strerror(errno); }

bool CreateEpoll(Poller::Impl &impl, std::string &error) {
        impl.epoll_fd = epoll_create1(EPOLL_CLOEXEC);
        if (impl.epoll_fd < 0) {
                error = LastErrno();
                return false;
        }
        return true;
}

void DestroyEpoll(Poller::Impl &impl) noexcept {
        impl.registered_sockets.clear();
        if (impl.epoll_fd >= 0) {
                close(impl.epoll_fd);
                impl.epoll_fd = -1;
        }
}

bool AddEpollSocket(Poller::Impl &impl, SocketHandle socket_fd, std::string &error) {
        epoll_event event{};
        event.events = EPOLLIN;
        event.data.fd = socket_fd;
        if (epoll_ctl(impl.epoll_fd, EPOLL_CTL_ADD, socket_fd, &event) != 0) {
                error = LastErrno();
                return false;
        }
        impl.registered_sockets.insert(socket_fd);
        return true;
}

void RemoveEpollSocket(Poller::Impl &impl, SocketHandle socket_fd) noexcept {
        if (impl.registered_sockets.erase(socket_fd) == 0 || impl.epoll_fd < 0) { return; }
        epoll_ctl(impl.epoll_fd, EPOLL_CTL_DEL, socket_fd, nullptr);
}

bool WaitEpoll(int epoll_fd, std::chrono::milliseconds timeout, std::vector<SocketHandle> &ready, std::string &error) {
        std::array<epoll_event, 256> events{};
        const auto timeout_ms = timeout.count() > static_cast<std::int64_t>(INT_MAX) ? INT_MAX : static_cast<int>(timeout.count());
        const int count = epoll_wait(epoll_fd, events.data(), static_cast<int>(events.size()), timeout_ms);
        if (count < 0) {
                if (errno == EINTR) { return false; }
                error = LastErrno();
                return false;
        }
        for (int i = 0; i < count; ++i) { ready.push_back(events[static_cast<std::size_t>(i)].data.fd); }
        return !ready.empty();
}

} // namespace

Poller::Poller() = default;
Poller::~Poller() { Close(); }

bool Poller::Open(SocketHandle socket_fd, std::string &error) {
        std::lock_guard lock(mutex_);
        if (impl_ != nullptr) {
                DestroyEpoll(*impl_);
                delete impl_;
                impl_ = nullptr;
        }

        impl_ = new Impl();
        if (!CreateEpoll(*impl_, error)) {
                DestroyEpoll(*impl_);
                delete impl_;
                impl_ = nullptr;
                return false;
        }
        if (!AddEpollSocket(*impl_, socket_fd, error)) {
                DestroyEpoll(*impl_);
                delete impl_;
                impl_ = nullptr;
                return false;
        }
        return true;
}

bool Poller::Register(SocketHandle socket_fd, std::string &error) {
        std::lock_guard lock(mutex_);
        if (impl_ == nullptr) {
                error = "poller not open";
                return false;
        }
        if (impl_->registered_sockets.contains(socket_fd)) { return true; }
        return AddEpollSocket(*impl_, socket_fd, error);
}

void Poller::Unregister(SocketHandle socket_fd) noexcept {
        std::lock_guard lock(mutex_);
        if (impl_ == nullptr) { return; }
        RemoveEpollSocket(*impl_, socket_fd);
}

bool Poller::Wait(std::chrono::milliseconds timeout, std::vector<SocketHandle> &ready, std::string &error) {
        ready.clear();

        int epoll_fd = -1;
        {
                std::lock_guard lock(mutex_);
                if (impl_ == nullptr) { return false; }
                epoll_fd = impl_->epoll_fd;
        }
        return WaitEpoll(epoll_fd, timeout, ready, error);
}

void Poller::Close() noexcept {
        std::lock_guard lock(mutex_);
        if (impl_ == nullptr) { return; }
        DestroyEpoll(*impl_);
        delete impl_;
        impl_ = nullptr;
}

} // namespace ukcp
