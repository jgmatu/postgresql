#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <random>
#include <thread>

#include <db/DatabaseManager.hpp>

namespace pgdemo::controller {

/**
 * @brief Writes synthetic samples for one device at the configured rate.
 *
 * The configuration in force comes from `device_config` (the applied state). The
 * controller calls `apply()` after it has committed a new applied configuration;
 * the change takes effect on the next tick, without waiting out the old period.
 *
 * The generator follows `device.type_id`. The phase depends on `device_id`, so
 * several devices of the same type do not overlap.
 * - `sine`: `amplitude * sin(2*pi*seq/40 + phase)`
 * - `sawtooth`: `amplitude * (seq % 50) / 50` plus Gaussian noise
 * - `gps_orbit`: orbit of radius `amplitude` metres around Madrid; PostGIS projects the point
 *
 * A type without a generator of its own gets `sine` (scalar) or `gps_orbit` (gps).
 */
class Producer
{
public:
    /**
     * @param db      shared manager; must outlive the producer
     * @param device  device this producer writes for
     * @param initial applied configuration to start from
     */
    Producer(db::DatabaseManager& db, db::Device device, const db::AppliedConfig& initial);
    /// Calls `stop()`.
    ~Producer();

    Producer(const Producer&) = delete;
    Producer& operator=(const Producer&) = delete;

    /// Start the writer thread. A second call is a no-op.
    void start();
    /// Wake the writer thread, ask it to stop, and join it. Idempotent.
    void stop();

    /// Thread-safe. Called from the listener thread after a commit.
    void apply(const db::AppliedConfig& config);

    /// Device this producer writes for.
    [[nodiscard]] const db::Device& device() const noexcept { return device_; }

    /// Rows committed by this producer.
    [[nodiscard]] std::int64_t samples_written() const noexcept { return written_.load(); }

private:
    enum class Generator { Sine, Sawtooth, GpsOrbit };

    static Generator generator_for(const db::Device& device) noexcept;
    void run(std::stop_token st);
    void write_tick(std::int64_t seq);

    db::DatabaseManager& db_;
    const db::Device device_;
    const Generator generator_;
    const double phase_;

    std::atomic<int> rate_ms_;
    std::atomic<double> amplitude_;
    std::atomic<bool> active_;
    std::atomic<std::int64_t> written_{0};

    std::mt19937_64 rng_;
    std::normal_distribution<double> noise_{0.0, 1.0};

    std::mutex wake_mtx_;
    std::condition_variable_any wake_cv_;
    std::uint64_t generation_{0};

    std::jthread thread_;
};

} // namespace pgdemo::controller
