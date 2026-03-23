#include "platform_socket.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <thread>
#include <unordered_map>

#if !UKCP_PLATFORM_WINDOWS
#include <linux/io_uring.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#endif

namespace ukcp {

#if !UKCP_PLATFORM_WINDOWS

namespace {

int IoUringSetup(unsigned entries, io_uring_params* params)
{
    return static_cast<int>(::syscall(__NR_io_uring_setup, entries, params));
}

int IoUringEnter(int ring_fd, unsigned to_submit, unsigned min_complete, unsigned flags)
{
    return static_cast<int>(
        ::syscall(__NR_io_uring_enter, ring_fd, to_submit, min_complete, flags, nullptr, 0));
}

std::string LastErrno()
{
    return std::strerror(errno);
}

}  // namespace

struct Poller::LinuxImpl {
    int ring_fd{-1};
    void* sq_ring_ptr{nullptr};
    std::size_t sq_ring_size{0};
    void* cq_ring_ptr{nullptr};
    std::size_t cq_ring_size{0};
    io_uring_sqe* sqes{nullptr};
    std::size_t sqes_size{0};

    std::uint32_t* sq_head{nullptr};
    std::uint32_t* sq_tail{nullptr};
    std::uint32_t* sq_ring_mask{nullptr};
    std::uint32_t* sq_ring_entries{nullptr};
    std::uint32_t* sq_flags{nullptr};
    std::uint32_t* sq_array{nullptr};

    std::uint32_t* cq_head{nullptr};
    std::uint32_t* cq_tail{nullptr};
    std::uint32_t* cq_ring_mask{nullptr};
    std::uint32_t* cq_ring_entries{nullptr};
    io_uring_cqe* cqes{nullptr};

    std::unordered_map<SocketHandle, std::uint64_t> active_tokens;
    std::unordered_map<std::uint64_t, SocketHandle> token_to_socket;
    std::uint64_t next_token{1};
    unsigned pending_submit{0};
};

namespace {

void UnmapIfNeeded(void*& ptr, std::size_t size) noexcept
{
    if (ptr != nullptr && ptr != MAP_FAILED) {
        munmap(ptr, size);
    }
    ptr = nullptr;
}

bool MapRing(Poller::LinuxImpl& impl, std::string& error)
{
    io_uring_params params{};
    params.flags = 0;

    impl.ring_fd = IoUringSetup(8192, &params);
    if (impl.ring_fd < 0) {
        error = LastErrno();
        return false;
    }

    impl.sq_ring_size = params.sq_off.array + params.sq_entries * sizeof(std::uint32_t);
    impl.cq_ring_size = params.cq_off.cqes + params.cq_entries * sizeof(io_uring_cqe);
    if ((params.features & IORING_FEAT_SINGLE_MMAP) != 0U) {
        impl.sq_ring_size = (std::max)(impl.sq_ring_size, impl.cq_ring_size);
        impl.cq_ring_size = impl.sq_ring_size;
    }
    impl.sqes_size = params.sq_entries * sizeof(io_uring_sqe);

    impl.sq_ring_ptr = mmap(
        nullptr,
        impl.sq_ring_size,
        PROT_READ | PROT_WRITE,
        MAP_SHARED | MAP_POPULATE,
        impl.ring_fd,
        IORING_OFF_SQ_RING);
    if (impl.sq_ring_ptr == MAP_FAILED) {
        error = LastErrno();
        impl.sq_ring_ptr = nullptr;
        return false;
    }

    if ((params.features & IORING_FEAT_SINGLE_MMAP) != 0U) {
        impl.cq_ring_ptr = impl.sq_ring_ptr;
    } else {
        impl.cq_ring_ptr = mmap(
            nullptr,
            impl.cq_ring_size,
            PROT_READ | PROT_WRITE,
            MAP_SHARED | MAP_POPULATE,
            impl.ring_fd,
            IORING_OFF_CQ_RING);
        if (impl.cq_ring_ptr == MAP_FAILED) {
            error = LastErrno();
            impl.cq_ring_ptr = nullptr;
            return false;
        }
    }

    impl.sqes = static_cast<io_uring_sqe*>(mmap(
        nullptr,
        impl.sqes_size,
        PROT_READ | PROT_WRITE,
        MAP_SHARED | MAP_POPULATE,
        impl.ring_fd,
        IORING_OFF_SQES));
    if (impl.sqes == MAP_FAILED) {
        error = LastErrno();
        impl.sqes = nullptr;
        return false;
    }

    auto* sq_base = static_cast<std::uint8_t*>(impl.sq_ring_ptr);
    impl.sq_head = reinterpret_cast<std::uint32_t*>(sq_base + params.sq_off.head);
    impl.sq_tail = reinterpret_cast<std::uint32_t*>(sq_base + params.sq_off.tail);
    impl.sq_ring_mask = reinterpret_cast<std::uint32_t*>(sq_base + params.sq_off.ring_mask);
    impl.sq_ring_entries = reinterpret_cast<std::uint32_t*>(sq_base + params.sq_off.ring_entries);
    impl.sq_flags = reinterpret_cast<std::uint32_t*>(sq_base + params.sq_off.flags);
    impl.sq_array = reinterpret_cast<std::uint32_t*>(sq_base + params.sq_off.array);

    auto* cq_base = static_cast<std::uint8_t*>(impl.cq_ring_ptr);
    impl.cq_head = reinterpret_cast<std::uint32_t*>(cq_base + params.cq_off.head);
    impl.cq_tail = reinterpret_cast<std::uint32_t*>(cq_base + params.cq_off.tail);
    impl.cq_ring_mask = reinterpret_cast<std::uint32_t*>(cq_base + params.cq_off.ring_mask);
    impl.cq_ring_entries = reinterpret_cast<std::uint32_t*>(cq_base + params.cq_off.ring_entries);
    impl.cqes = reinterpret_cast<io_uring_cqe*>(cq_base + params.cq_off.cqes);
    return true;
}

void DestroyRing(Poller::LinuxImpl& impl) noexcept
{
    if (impl.sq_ring_ptr != impl.cq_ring_ptr) {
        UnmapIfNeeded(impl.cq_ring_ptr, impl.cq_ring_size);
    }
    UnmapIfNeeded(impl.sq_ring_ptr, impl.sq_ring_size);
    if (impl.sqes != nullptr) {
        void* sqes = impl.sqes;
        impl.sqes = nullptr;
        munmap(sqes, impl.sqes_size);
    }
    if (impl.ring_fd >= 0) {
        close(impl.ring_fd);
        impl.ring_fd = -1;
    }
    impl.active_tokens.clear();
    impl.token_to_socket.clear();
    impl.pending_submit = 0;
}

io_uring_sqe* NextSqe(Poller::LinuxImpl& impl)
{
    const auto head = __atomic_load_n(impl.sq_head, __ATOMIC_ACQUIRE);
    const auto tail = __atomic_load_n(impl.sq_tail, __ATOMIC_RELAXED);
    if (tail - head >= *impl.sq_ring_entries) {
        return nullptr;
    }

    const auto index = tail & *impl.sq_ring_mask;
    io_uring_sqe* sqe = &impl.sqes[index];
    std::memset(sqe, 0, sizeof(*sqe));
    impl.sq_array[index] = index;
    __atomic_store_n(impl.sq_tail, tail + 1, __ATOMIC_RELEASE);
    ++impl.pending_submit;
    return sqe;
}

bool QueuePollAdd(Poller::LinuxImpl& impl, SocketHandle socket_fd, std::uint64_t token, std::string& error)
{
    io_uring_sqe* sqe = NextSqe(impl);
    if (sqe == nullptr) {
        error = "io_uring submission queue full";
        return false;
    }

    sqe->opcode = IORING_OP_POLL_ADD;
    sqe->fd = socket_fd;
    sqe->poll32_events = POLLIN;
    sqe->len = 0;
    sqe->user_data = token;
    return true;
}

bool SubmitPending(Poller::LinuxImpl& impl, std::string& error)
{
    if (impl.pending_submit == 0) {
        return true;
    }

    if (IoUringEnter(impl.ring_fd, impl.pending_submit, 0, 0) < 0) {
        error = LastErrno();
        return false;
    }
    impl.pending_submit = 0;
    return true;
}

void CollectReady(Poller::LinuxImpl& impl, std::vector<SocketHandle>& ready, std::string& error)
{
    const auto tail = __atomic_load_n(impl.cq_tail, __ATOMIC_ACQUIRE);
    auto head = __atomic_load_n(impl.cq_head, __ATOMIC_RELAXED);
    while (head != tail) {
        io_uring_cqe& cqe = impl.cqes[head & *impl.cq_ring_mask];
        const std::uint64_t token = cqe.user_data;
        auto token_it = impl.token_to_socket.find(token);
        if (token_it != impl.token_to_socket.end()) {
            const SocketHandle socket_fd = token_it->second;
            auto active_it = impl.active_tokens.find(socket_fd);
            if (active_it != impl.active_tokens.end() && active_it->second == token) {
                if (cqe.res >= 0) {
                    if (std::find(ready.begin(), ready.end(), socket_fd) == ready.end()) {
                        ready.push_back(socket_fd);
                    }
                    if (!QueuePollAdd(impl, socket_fd, token, error)) {
                        return;
                    }
                } else {
                    error = "io_uring poll failed: " + std::to_string(-cqe.res);
                    impl.active_tokens.erase(active_it);
                    impl.token_to_socket.erase(token_it);
                    return;
                }
            }
        }
        ++head;
    }
    __atomic_store_n(impl.cq_head, head, __ATOMIC_RELEASE);
}

}  // namespace

#endif

Poller::Poller() = default;
Poller::~Poller() { Close(); }

bool Poller::Open(SocketHandle socket_fd, std::string& error)
{
#if UKCP_PLATFORM_WINDOWS
    std::lock_guard lock(mutex_);
    sockets_.clear();
    sockets_.push_back(socket_fd);
    error.clear();
    return true;
#else
    std::lock_guard lock(mutex_);
    if (linux_ != nullptr) {
        DestroyRing(*linux_);
        delete linux_;
        linux_ = nullptr;
    }
    linux_ = new LinuxImpl();
    if (!MapRing(*linux_, error)) {
        DestroyRing(*linux_);
        delete linux_;
        linux_ = nullptr;
        return false;
    }
    const std::uint64_t token = linux_->next_token++;
    linux_->active_tokens.emplace(socket_fd, token);
    linux_->token_to_socket.emplace(token, socket_fd);
    if (!QueuePollAdd(*linux_, socket_fd, token, error)) {
        linux_->active_tokens.erase(socket_fd);
        linux_->token_to_socket.erase(token);
        DestroyRing(*linux_);
        delete linux_;
        linux_ = nullptr;
        return false;
    }
    if (!SubmitPending(*linux_, error)) {
        DestroyRing(*linux_);
        delete linux_;
        linux_ = nullptr;
        return false;
    }
    return true;
#endif
}

bool Poller::Register(SocketHandle socket_fd, std::string& error)
{
#if UKCP_PLATFORM_WINDOWS
    std::lock_guard lock(mutex_);
    if (std::find(sockets_.begin(), sockets_.end(), socket_fd) == sockets_.end()) {
        sockets_.push_back(socket_fd);
    }
    error.clear();
    return true;
#else
    std::lock_guard lock(mutex_);
    if (linux_ == nullptr) {
        error = "poller not open";
        return false;
    }
    if (linux_->active_tokens.find(socket_fd) != linux_->active_tokens.end()) {
        return true;
    }

    const std::uint64_t token = linux_->next_token++;
    linux_->active_tokens.emplace(socket_fd, token);
    linux_->token_to_socket.emplace(token, socket_fd);
    if (!QueuePollAdd(*linux_, socket_fd, token, error)) {
        linux_->active_tokens.erase(socket_fd);
        linux_->token_to_socket.erase(token);
        return false;
    }
    return SubmitPending(*linux_, error);
#endif
}

void Poller::Unregister(SocketHandle socket_fd) noexcept
{
#if UKCP_PLATFORM_WINDOWS
    std::lock_guard lock(mutex_);
    sockets_.erase(std::remove(sockets_.begin(), sockets_.end(), socket_fd), sockets_.end());
#else
    std::lock_guard lock(mutex_);
    if (linux_ == nullptr) {
        return;
    }
    auto it = linux_->active_tokens.find(socket_fd);
    if (it == linux_->active_tokens.end()) {
        return;
    }
    linux_->token_to_socket.erase(it->second);
    linux_->active_tokens.erase(it);
#endif
}

bool Poller::Wait(
    std::chrono::milliseconds timeout,
    std::vector<SocketHandle>& ready,
    std::string& error)
{
    ready.clear();
#if UKCP_PLATFORM_WINDOWS
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (ready.empty()) {
        std::vector<SocketHandle> sockets_copy;
        {
            std::lock_guard lock(mutex_);
            sockets_copy = sockets_;
        }
        if (sockets_copy.empty()) {
            return false;
        }

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
                if (FD_ISSET(socket_fd, &set)) {
                    ready.push_back(socket_fd);
                }
            }
            return !ready.empty();
        }
        if (timeout.count() == 0 || std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
    }
    return !ready.empty();
#else
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (ready.empty()) {
        {
            std::lock_guard lock(mutex_);
            if (linux_ == nullptr) {
                return false;
            }
            if (!SubmitPending(*linux_, error)) {
                return false;
            }
            CollectReady(*linux_, ready, error);
            if (!error.empty()) {
                return false;
            }
            if (!ready.empty()) {
                if (!SubmitPending(*linux_, error)) {
                    return false;
                }
                return true;
            }
        }
        if (timeout.count() == 0 || std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        int ring_fd = -1;
        {
            std::lock_guard lock(mutex_);
            if (linux_ == nullptr) {
                return false;
            }
            ring_fd = linux_->ring_fd;
        }
        const int rc = IoUringEnter(ring_fd, 0, 0, IORING_ENTER_GETEVENTS);
        if (rc < 0) {
            error = LastErrno();
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
#endif
}

void Poller::Close() noexcept
{
#if UKCP_PLATFORM_WINDOWS
    std::lock_guard lock(mutex_);
    sockets_.clear();
#else
    std::lock_guard lock(mutex_);
    if (linux_ == nullptr) {
        return;
    }
    DestroyRing(*linux_);
    delete linux_;
    linux_ = nullptr;
#endif
}

}  // namespace ukcp
