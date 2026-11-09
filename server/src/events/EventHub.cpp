#include <events/EventHub.hpp>

#include <algorithm>

namespace pgdemo::server {

EventHub::Subscription::Subscription(std::set<std::string> channels, std::size_t capacity)
    : channels_(std::move(channels))
    , capacity_(capacity)
{
}

std::optional<Event> EventHub::Subscription::pop(std::chrono::milliseconds timeout)
{
    std::unique_lock lock(mtx_);
    cv_.wait_for(lock, timeout, [&] { return closed_ || !queue_.empty(); });
    if (queue_.empty()) return std::nullopt;
    Event e = std::move(queue_.front());
    queue_.pop_front();
    return e;
}

bool EventHub::Subscription::closed() const
{
    std::lock_guard lock(mtx_);
    return closed_;
}

std::uint64_t EventHub::Subscription::dropped() const
{
    std::lock_guard lock(mtx_);
    return dropped_;
}

void EventHub::Subscription::push(const Event& e)
{
    {
        std::lock_guard lock(mtx_);
        if (closed_) return;
        if (queue_.size() >= capacity_)
        {
            queue_.pop_front();
            ++dropped_;
        }
        queue_.push_back(e);
    }
    cv_.notify_one();
}

void EventHub::Subscription::close()
{
    {
        std::lock_guard lock(mtx_);
        closed_ = true;
    }
    cv_.notify_all();
}

const std::set<std::string>& EventHub::all_channels()
{
    static const std::set<std::string> kAll{"dummy1", "dummy2", "dummy3", "config", "error"};
    return kAll;
}

std::shared_ptr<EventHub::Subscription> EventHub::subscribe(std::set<std::string> channels,
                                                            std::size_t capacity)
{
    auto sub = std::shared_ptr<Subscription>(new Subscription(std::move(channels), capacity));
    std::lock_guard lock(mtx_);
    if (closed_) sub->close();
    else subs_.push_back(sub);
    return sub;
}

void EventHub::unsubscribe(const std::shared_ptr<Subscription>& sub)
{
    std::lock_guard lock(mtx_);
    std::erase(subs_, sub);
}

void EventHub::publish(const std::string& channel, std::string data)
{
    std::lock_guard lock(mtx_);
    if (closed_) return;
    const Event e{++next_id_, channel, std::move(data)};
    for (const auto& s : subs_)
    {
        if (s->channels().contains(channel)) s->push(e);
    }
}

void EventHub::close_all()
{
    std::lock_guard lock(mtx_);
    closed_ = true;
    for (const auto& s : subs_) s->close();
    subs_.clear();
}

std::size_t EventHub::subscribers() const
{
    std::lock_guard lock(mtx_);
    return subs_.size();
}

std::uint64_t EventHub::published() const
{
    std::lock_guard lock(mtx_);
    return next_id_;
}

} // namespace pgdemo::server
