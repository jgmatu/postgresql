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
 */
class Producer
{
public:
    Producer(db::DatabaseManager& db, db::DummyTable table, const db::AppliedConfig& initial, int items);
    ~Producer();

    Producer(const Producer&) = delete;
    Producer& operator=(const Producer&) = delete;

    void start();
    void stop();

    /// Thread-safe. Called from the listener thread after a commit.
    void apply(const db::AppliedConfig& config);

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
