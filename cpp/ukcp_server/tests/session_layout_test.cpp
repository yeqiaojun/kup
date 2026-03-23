#include <atomic>
#include <concepts>
#include <shared_mutex>
#include <type_traits>

#include "server_internal.hpp"

namespace {

template <class T>
concept HasScheduleToken = requires(T value) { value.schedule_token; };

template <class T>
concept HasNextScheduleToken = requires(T value) { value.next_schedule_token; };

static_assert(std::is_same_v<std::remove_cvref_t<decltype(std::declval<ukcp::SessionImpl &>().mutex)>, std::shared_mutex>);
static_assert(std::is_same_v<std::remove_cvref_t<decltype(std::declval<ukcp::ServerImpl &>().mutex)>, std::shared_mutex>);
static_assert(std::is_same_v<std::remove_cvref_t<decltype(std::declval<ukcp::SessionImpl &>().last_kcp_ms)>, std::uint64_t>);
static_assert(std::is_same_v<std::remove_cvref_t<decltype(std::declval<ukcp::SessionImpl &>().closed)>, bool>);
static_assert(std::is_same_v<std::remove_cvref_t<decltype(std::declval<ukcp::SessionImpl &>().next_update_ms)>, std::atomic<std::uint64_t>>);
static_assert(!HasScheduleToken<ukcp::SessionImpl>);
static_assert(!HasNextScheduleToken<ukcp::ServerImpl>);

} // namespace
