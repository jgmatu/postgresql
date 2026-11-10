#pragma once

#include "Generators.hpp"

#include <log/Log.hpp>

#include <pgdemo_msgs/msg/bye.hpp>
#include <pgdemo_msgs/msg/config.hpp>
#include <pgdemo_msgs/msg/heartbeat.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <functional>
#include <random>
#include <string>
#include <variant>

namespace pgdemo::device {

/**
 * @brief Online phase of a device node: samples and heartbeats toward the manager.
 *
 * The node owns the timers. `tick()` emits at most one message for the instant it
 * is given, so a test can advance time without a ROS graph. `on_config()` applies
 * at once (the node then re-arms its timer at the new rate). `on_bye()` ends the process.
 */
class SampleStream
{
public:
    /// A sample or a heartbeat. The node adds the device name and the topic.
    using Emission = std::variant<pgdemo_msgs::msg::ScalarSample, pgdemo_msgs::msg::GpsSample, pgdemo_msgs::msg::XySample,
                                 pgdemo_msgs::msg::Heartbeat>;
    /// Publish one uplink message. Empty is allowed in a test that only reads state.
    using Send = std::function<void(Emission)>;

    /// What a manager message did to the stream.
    enum class Effect {
        Config, ///< rate, amplitude or pause changed; the node re-arms its timer
        Bye,    ///< the manager asked the process to exit
    };

    /**
     * @param send      publishes a message; empty is allowed in a test that only reads state
     * @param generator signal of this device type
     * @param tag       log prefix
     */
    SampleStream(Send send, Generator generator, std::string tag)
        : send_(std::move(send))
        , generator_(generator)
        , tag_(std::move(tag))
        , last_frame_(std::chrono::steady_clock::now())
    {
    }

    /// Take `device_id`, `rate_ms`, `amplitude` and `is_active` from a configuration.
    void apply(const pgdemo_msgs::msg::Config& msg)
    {
        device_id_ = msg.device_id;
        rate_ms_ = std::max(static_cast<int>(msg.rate_ms), 1);
        amplitude_ = msg.amplitude;
        active_ = msg.is_active;
        phase_ = phase_for(device_id_);
    }

    /// Apply configuration and log it.
    Effect on_config(const pgdemo_msgs::msg::Config& msg)
    {
        apply(msg);
        log::info(tag_, std::format("config: rate_ms {} amplitude {} active {}", rate_ms_, amplitude_, active_));
        return Effect::Config;
    }

    /// Remember `bye` and stop emitting.
    Effect on_bye(const pgdemo_msgs::msg::Bye& msg)
    {
        log::info(tag_, "bye: " + msg.reason);
        stopped_ = true;
        return Effect::Bye;
    }

    /**
     * @brief Emit the message due at `now`.
     *
     * Active: one sample. Paused: a heartbeat when 5 s have passed since the
     * last message, otherwise nothing. A stopped stream emits nothing.
     */
    void tick(std::chrono::steady_clock::time_point now)
    {
        if (stopped_ || !send_) return;
        if (active_)
        {
            std::normal_distribution<double> noise(0.0, 1.0);
            std::uniform_real_distribution<double> unit(0.0, 1.0);
            std::visit([this](auto sample) { send_(std::move(sample)); },
                       make_sample(generator_, ++seq_, amplitude_, phase_, noise(rng_), unit(rng_), unit(rng_)));
            last_frame_ = now;
            return;
        }
        if (now - last_frame_ >= std::chrono::seconds(5))
        {
            send_(pgdemo_msgs::msg::Heartbeat{});
            last_frame_ = now;
        }
    }

    /// The period the node should wait before the next `tick()`.
    [[nodiscard]] std::chrono::milliseconds period() const
    {
        const int ms = active_ ? rate_ms_ : std::min(rate_ms_, 1000);
        return std::chrono::milliseconds(ms);
    }

    /// True after `bye`.
    [[nodiscard]] bool stopped() const noexcept { return stopped_; }

    /// `device_id` of the last configuration; it sets the phase.
    [[nodiscard]] int device_id() const noexcept { return device_id_; }
    /// Period between samples.
    [[nodiscard]] int rate_ms() const noexcept { return rate_ms_; }
    /// Peak value, standard deviation, orbit radius in metres, or half-extent of the square.
    [[nodiscard]] double amplitude() const noexcept { return amplitude_; }
    /// False while paused: heartbeats only.
    [[nodiscard]] bool active() const noexcept { return active_; }
    /// Samples sent so far; the last one carried this `seq`.
    [[nodiscard]] std::int64_t samples() const noexcept { return seq_; }

private:
    Send send_;
    Generator generator_;
    std::string tag_;
    std::mt19937_64 rng_{std::random_device{}()};
    std::chrono::steady_clock::time_point last_frame_;
    int device_id_{0};
    int rate_ms_{1000};
    double amplitude_{1.0};
    bool active_{true};
    bool stopped_{false};
    double phase_{0.0};
    std::int64_t seq_{0};
};

} // namespace pgdemo::device
