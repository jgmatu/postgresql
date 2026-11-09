#pragma once

#include "Generators.hpp"

#include <wire/Frame.hpp>
#include <wire/Settle.hpp>

#include <log/Log.hpp>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <format>
#include <random>
#include <string>

namespace pgdemo::device {

/**
 * @brief Online phase of a device process, after the handshake: three
 * coroutines on one socket.
 *
 * - `produce()`, the only writer: a `sample` every `rate_ms` while active, a
 *   `heartbeat` every 5 s while paused;
 * - `listen()`: applies `config` frames (cancelling the producer's wait so the
 *   new rate takes effect at once) and returns on `bye`;
 * - `on_signal()`: returns on SIGINT or SIGTERM.
 *
 * `run()` ends when the first of them does; the `||` cancels the other two.
 * An EOF or protocol error from the controller ends it with that exception.
 */
class SampleStream
{
public:
    /// Connection to the controller.
    using Socket = boost::asio::local::stream_protocol::socket;

    /**
     * @param socket    authenticated connection; must outlive the stream
     * @param generator signal of this device type
     * @param tag       log prefix
     */
    SampleStream(Socket& socket, Generator generator, std::string tag)
        : socket_(socket)
        , timer_(socket.get_executor())
        , generator_(generator)
        , tag_(std::move(tag))
    {
    }

    /// Take `device_id`, `rate_ms`, `amplitude` and `is_active` from a `config` frame.
    void apply(const boost::json::object& msg)
    {
        device_id_ = static_cast<int>(wire::get_number(msg, "device_id").value_or(device_id_));
        rate_ms_ = static_cast<int>(wire::get_number(msg, "rate_ms").value_or(rate_ms_));
        amplitude_ = wire::get_number(msg, "amplitude").value_or(amplitude_);
        if (const auto* v = msg.if_contains("is_active"); v != nullptr && v->is_bool()) active_ = v->as_bool();
        rate_ms_ = std::max(rate_ms_, 1);
        phase_ = phase_for(device_id_);
    }

    /// Stream until bye, a signal, or a connection error (thrown).
    boost::asio::awaitable<void> run()
    {
        using namespace boost::asio::experimental::awaitable_operators;
        // settle(): an EOF in listen() must end the stream, not leave on_signal() waiting.
        wire::rethrow_winner(co_await (wire::settle(produce()) || wire::settle(listen()) || wire::settle(on_signal())));
    }

    /// `device_id` of the last configuration; it sets the phase.
    [[nodiscard]] int device_id() const noexcept { return device_id_; }
    /// Period between samples.
    [[nodiscard]] int rate_ms() const noexcept { return rate_ms_; }
    /// Peak value, or the orbit radius in metres for GPS.
    [[nodiscard]] double amplitude() const noexcept { return amplitude_; }
    /// False while paused: heartbeats only.
    [[nodiscard]] bool active() const noexcept { return active_; }
    /// Samples sent so far; the last one carried this `seq`.
    [[nodiscard]] std::int64_t samples() const noexcept { return seq_; }

private:
    boost::asio::awaitable<void> produce()
    {
        using boost::asio::use_awaitable;
        std::mt19937_64 rng(std::random_device{}());
        std::normal_distribution<double> noise(0.0, 1.0);
        auto last_frame = std::chrono::steady_clock::now();
        for (;;)
        {
            const auto now = std::chrono::steady_clock::now();
            if (active_)
            {
                co_await wire::write_frame(socket_, make_sample(generator_, ++seq_, amplitude_, phase_, noise(rng)));
                last_frame = now;
            }
            else if (now - last_frame >= std::chrono::seconds(5))
            {
                co_await wire::write_frame(socket_, {{"type", "heartbeat"}});
                last_frame = now;
            }
            timer_.expires_after(std::chrono::milliseconds(active_ ? rate_ms_ : std::min(rate_ms_, 1000)));
            co_await timer_.async_wait(boost::asio::as_tuple(use_awaitable));
        }
    }

    boost::asio::awaitable<void> listen()
    {
        for (;;)
        {
            const auto msg = co_await wire::read_frame(socket_);
            const auto type = wire::type_of(msg);
            if (type == "config")
            {
                apply(msg);
                log::info(tag_, std::format("config: rate_ms {} amplitude {} active {}", rate_ms_, amplitude_,
                                            active_));
                timer_.cancel();
            }
            else if (type == "bye")
            {
                log::info(tag_, "bye: " + wire::get_string(msg, "reason").value_or(""));
                co_return;
            }
            else
            {
                log::warn(tag_, "ignoring message " + type);
            }
        }
    }

    boost::asio::awaitable<void> on_signal()
    {
        boost::asio::signal_set signals(co_await boost::asio::this_coro::executor, SIGINT, SIGTERM);
        const int sig = co_await signals.async_wait(boost::asio::use_awaitable);
        log::info(tag_, std::format("signal {}, exiting after {} sample(s)", sig, seq_));
    }

    Socket& socket_;
    boost::asio::steady_timer timer_;
    Generator generator_;
    std::string tag_;
    int device_id_{0};
    int rate_ms_{1000};
    double amplitude_{1.0};
    bool active_{true};
    double phase_{0.0};
    std::int64_t seq_{0};
};

} // namespace pgdemo::device
