#include <dispatcher/Dispatcher.hpp>

#include <chrono>
#include <condition_variable>
#include <format>

#include <log/Log.hpp>

namespace pgdemo::server {

namespace {
constexpr std::string_view kLog = "dispatcher";
}

struct Dispatcher::RequestContext
{
    std::condition_variable cv;
    ResponseStatus status = ResponseStatus::PENDING;
    bool ready = false;
};

Dispatcher::Dispatcher(std::size_t max_pending)
    : max_pending_(max_pending)
{
}

std::string_view Dispatcher::status_to_string(ResponseStatus status) noexcept
{
    switch (status)
    {
        case ResponseStatus::PENDING: return "PENDING";
        case ResponseStatus::SUCCESS: return "SUCCESS";
        case ResponseStatus::ERROR: return "ERROR";
        case ResponseStatus::TIMEOUT: return "TIMEOUT";
        case ResponseStatus::DB_ERROR: return "DB_ERROR";
        case ResponseStatus::SYSTEM_FULL: return "SYSTEM_FULL";
    }
    return "UNKNOWN";
}

ResponseStatus Dispatcher::wait_for_response(std::uint64_t id, int timeout_ms)
{
    return wait_for_response(id, timeout_ms, {});
}

ResponseStatus Dispatcher::wait_for_response(std::uint64_t id, int timeout_ms,
                                             const std::function<void()>& submit)
{
    // The context lives on this thread's stack. It is reachable through the map
    // only between registration and the erase below, and dispatch() touches it
    // only while holding map_mtx.
    RequestContext ctx;
    std::unique_lock lock(map_mtx);

    if (pending_requests.size() >= max_pending_)
    {
        log::error(kLog, std::format("system full: {} waiters (limit {})", pending_requests.size(),
                                     max_pending_));
        return ResponseStatus::SYSTEM_FULL;
    }
    if (!pending_requests.try_emplace(id, &ctx).second)
    {
        log::error(kLog, std::format("request {} is already being waited for", id));
        return ResponseStatus::DB_ERROR;
    }

    if (submit)
    {
        lock.unlock();
        try
        {
            submit();
        }
        catch (const std::exception& e)
        {
            lock.lock();
            pending_requests.erase(id);
            log::error(kLog, std::format("submit for request {} failed: {}", id, e.what()));
            return ResponseStatus::DB_ERROR;
        }
        catch (...)
        {
            lock.lock();
            pending_requests.erase(id);
            log::error(kLog, std::format("submit for request {} failed", id));
            return ResponseStatus::DB_ERROR;
        }
        lock.lock();
    }

    log::debug(kLog, std::format("waiting for request {} ({} ms)", id, timeout_ms));
    const bool triggered =
        ctx.cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return ctx.ready; });

    const ResponseStatus final_status = triggered ? ctx.status : ResponseStatus::TIMEOUT;
    pending_requests.erase(id);
    log::info(kLog, std::format("request {} -> {}", id, status_to_string(final_status)));
    return final_status;
}

void Dispatcher::dispatch(std::uint64_t id, ResponseStatus status)
{
    std::lock_guard lock(map_mtx);
    const auto it = pending_requests.find(id);
    if (it == pending_requests.end())
    {
        log::debug(kLog, std::format("dispatch {} for request {}: nobody waiting",
                                     status_to_string(status), id));
        return;
    }
    it->second->status = status;
    it->second->ready = true;
    it->second->cv.notify_one();
}

std::size_t Dispatcher::pending() const
{
    std::lock_guard lock(map_mtx);
    return pending_requests.size();
}

} // namespace pgdemo::server
