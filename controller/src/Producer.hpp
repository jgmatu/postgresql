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
 * @brief Writes synthetic samples for one dummy table at the configured rate.
 *
 * The configuration in force comes from `dummy_config` (the applied state). The
 * controller calls `apply()` after it has committed a new applied configuration;
 * the change takes effect on the next tick, without waiting out the old period.
 *
 * - dummy1: `amplitude * sin(2*pi*seq/40 + phase(item))`
 * - dummy2: sawtooth `amplitude * (seq % 50) / 50` plus Gaussian noise
 * - dummy3: GPS orbit of radius `amplitude` metres; PostGIS projects the point
 */
class Producer
{
public:
    /**
     * @param db      shared manager; must outlive the producer
     * @param table   table this producer writes
     * @param initial applied configuration to start from
     * @param items   rows written per tick, `item_id` 1..items
     */
    Producer(db::DatabaseManager& db, db::DummyTable table, const db::AppliedConfig& initial, int items);
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

    /// Rows committed by this producer.
    [[nodiscard]] std::int64_t samples_written() const noexcept { return written_.load(); }

private:
    void run(std::stop_token st);
    double sample(int item_id, std::int64_t seq);

    db::DatabaseManager& db_;
    const db::DummyTable table_;
    const int items_;

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
