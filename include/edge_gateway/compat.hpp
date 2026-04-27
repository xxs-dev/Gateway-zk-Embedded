#pragma once

#if __cplusplus >= 201703L
#include <mutex>
#include <optional>
#include <shared_mutex>
namespace edge_gateway {
template <typename T>
using Optional = std::optional<T>;
constexpr auto NullOpt = std::nullopt;
using SharedMutex = std::shared_mutex;
using ReadLock = std::shared_lock<SharedMutex>;
using WriteLock = std::unique_lock<SharedMutex>;
}  // namespace edge_gateway
#else
#include <mutex>
#include <experimental/optional>
#include <shared_mutex>
namespace edge_gateway {
template <typename T>
using Optional = std::experimental::optional<T>;
constexpr auto NullOpt = std::experimental::nullopt;
using SharedMutex = std::shared_timed_mutex;
using ReadLock = std::shared_lock<SharedMutex>;
using WriteLock = std::unique_lock<SharedMutex>;
}  // namespace edge_gateway
#endif
