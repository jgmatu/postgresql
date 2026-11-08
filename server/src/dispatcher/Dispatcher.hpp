#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string_view>
#include <unordered_map>

namespace pgdemo::server {

/// Outcome returned by `Dispatcher::wait_for_response` or passed to `Dispatcher::dispatch`.
enum class ResponseStatus {
    PENDING,     ///< internal initial state
    SUCCESS,     ///< config_events: the controller applied the request
    ERROR,       ///< error_events: the controller rejected the request
    TIMEOUT,     ///< no dispatch before the deadline
    DB_ERROR,    ///< submit() failed; nothing was enqueued
    SYSTEM_FULL, ///< MAX_PENDING waiters already registered; nothing was enqueued
};

/**
 * @brief Synchronous correlation, by `request_id`, between a blocked request thread
 * and an asynchronous completion (a PostgreSQL NOTIFY handled on the listener thread).
 *
 * Each waiter keeps its `RequestContext` (condition variable + status) on its own
 * stack. The map only points at it while the waiter is inside `wait_for_response`,
 * and `dispatch` only touches it under `map_mtx`, so there is no dangling access.
 */
class Dispatcher
{
public:
    /// Default limit of concurrent waiters; beyond it requests get `SYSTEM_FULL`.
    static constexpr std::size_t MAX_PENDING = 500;

    /// @param max_pending concurrent waiters accepted before `SYSTEM_FULL`
    explicit Dispatcher(std::size_t max_pending = MAX_PENDING);

    /**
     * @brief Register `id`, then run `submit()`, then block until `dispatch(id, ...)`
     * or until `timeout_ms` passes.
     *
     * The wait is registered **before** `submit()` writes the request, so a completion
     * that arrives before this thread reaches the condition variable is not lost.
     * `submit()` runs without `map_mtx` held. If it throws, the registration is removed
     * and `DB_ERROR` is returned; capture the exception inside `submit` to inspect it.
     */
    ResponseStatus wait_for_response(std::uint64_t id, int timeout_ms,
                                     const std::function<void()>& submit);

    /// Register and wait. The request must not have been written yet (see above).
    ResponseStatus wait_for_response(std::uint64_t id, int timeout_ms);

    /// Wake the waiter of `id` with `status`. No-op if nobody waits for `id`.
    void dispatch(std::uint64_t id, ResponseStatus status);

    /// Number of registered waiters.
    std::size_t pending() const;

    /// Enumerator name, e.g. `"SUCCESS"`, for logs and JSON replies.
    static std::string_view status_to_string(ResponseStatus status) noexcept;

private:
    struct RequestContext;

    mutable std::mutex map_mtx;
    std::unordered_map<std::uint64_t, RequestContext*> pending_requests;
    const std::size_t max_pending_;
};

} // namespace pgdemo::server
