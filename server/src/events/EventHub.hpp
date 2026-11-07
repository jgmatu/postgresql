#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace pgdemo::server {

/// One event fanned out to SSE subscribers.
struct Event
{
    std::uint64_t id{0};
    std::string channel; ///< SSE `event:` name: dummy1 | dummy2 | config | error
    std::string data;    ///< JSON text
};

/**
 * @brief Fan-out from the database listener thread to SSE streams.
 *
 * `publish` never blocks: each subscriber has a bounded queue, and when a slow client
 * falls behind the oldest events are dropped (and counted). The listener thread
 * therefore never waits on a network socket.
 */
class EventHub
{
public:
    class Subscription
    {
    public:
        /// Next event, `std::nullopt` on timeout. `closed()` tells whether to stop.
        std::optional<Event> pop(std::chrono::milliseconds timeout);
        [[nodiscard]] bool closed() const;
        [[nodiscard]] std::uint64_t dropped() const;
        [[nodiscard]] const std::set<std::string>& channels() const { return channels_; }

    private:
        friend class EventHub;
        Subscription(std::set<std::string> channels, std::size_t capacity);
        void push(const Event& e);
        void close();

        const std::set<std::string> channels_;
        const std::size_t capacity_;
        mutable std::mutex mtx_;
        std::condition_variable cv_;
        std::deque<Event> queue_;
        std::uint64_t dropped_{0};
        bool closed_{false};
    };

    static const std::set<std::string>& all_channels();

    std::shared_ptr<Subscription> subscribe(std::set<std::string> channels, std::size_t capacity = 512);
    void unsubscribe(const std::shared_ptr<Subscription>& sub);

    /// Thread-safe, non-blocking.
    void publish(const std::string& channel, std::string data);

    /// Wakes every stream so it can end (server shutdown).
    void close_all();

    [[nodiscard]] std::size_t subscribers() const;
    [[nodiscard]] std::uint64_t published() const;

private:
    mutable std::mutex mtx_;
    std::vector<std::shared_ptr<Subscription>> subs_;
    std::uint64_t next_id_{0};
    bool closed_{false};
};

} // namespace pgdemo::server
