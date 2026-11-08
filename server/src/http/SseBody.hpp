#pragma once

#include <algorithm>
#include <chrono>
#include <cstring>
#include <format>
#include <memory>
#include <string>

#include <events/EventHub.hpp>

#include "oatpp/core/data/stream/Stream.hpp"

namespace pgdemo::server::http {

/**
 * @brief Server-Sent Events body (`text/event-stream`), pulled by Oat++.
 *
 * Runs on the connection's own thread (HttpConnectionHandler), so blocking in
 * `read()` only parks that connection. A comment line every 15 s keeps proxies
 * from timing out the stream and makes a dead client show up as a write error,
 * after which Oat++ drops the body and the destructor unsubscribes.
 */
class SseReadCallback : public oatpp::data::stream::ReadCallback
{
public:
    /// Idle time before a `: keepalive` comment line is sent.
    static constexpr std::chrono::seconds kKeepalive{15};

    /**
     * @param hub hub to unsubscribe from when the stream ends
     * @param sub subscription this stream drains
     */
    SseReadCallback(std::shared_ptr<EventHub> hub, std::shared_ptr<EventHub::Subscription> sub)
        : hub_(std::move(hub))
        , sub_(std::move(sub))
    {
        pending_ = "retry: 3000\n: connected\n\n";
    }

    /// Removes the subscription from the hub.
    ~SseReadCallback() override { hub_->unsubscribe(sub_); }

    /**
     * @brief Copy the next SSE frame into `buffer`, blocking until there is one.
     * @return bytes written, or 0 to end the stream after `EventHub::close_all()`
     */
    oatpp::v_io_size read(void* buffer, v_buff_size count, oatpp::async::Action&) override
    {
        while (offset_ >= pending_.size())
        {
            pending_.clear();
            offset_ = 0;
            if (const auto e = sub_->pop(kKeepalive))
            {
                pending_ = std::format("id: {}\nevent: {}\ndata: {}\n\n", e->id, e->channel, e->data);
            }
            else if (sub_->closed())
            {
                return 0;
            }
            else
            {
                pending_ = ": keepalive\n\n";
            }
        }
        const auto n = std::min<std::size_t>(static_cast<std::size_t>(count), pending_.size() - offset_);
        std::memcpy(buffer, pending_.data() + offset_, n);
        offset_ += n;
        return static_cast<oatpp::v_io_size>(n);
    }

private:
    std::shared_ptr<EventHub> hub_;
    std::shared_ptr<EventHub::Subscription> sub_;
    std::string pending_;
    std::size_t offset_{0};
};

} // namespace pgdemo::server::http
