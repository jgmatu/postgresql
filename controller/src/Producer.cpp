#include "Producer.hpp"

#include <chrono>
#include <cmath>
#include <format>
#include <numbers>

#include <log/Log.hpp>

namespace pgdemo::controller {

namespace {
constexpr std::string_view kLog = "producer";
}

Producer::Producer(db::DatabaseManager& db, db::DummyTable table, const db::AppliedConfig& initial,
                   int items)
    : db_(db)
    , table_(table)
    , items_(items)
    , rate_ms_(initial.rate_ms)
    , amplitude_(initial.amplitude)
    , active_(initial.is_active)
    , rng_(std::random_device{}())
{
}

Producer::~Producer() { stop(); }

void Producer::start()
{
    if (thread_.joinable()) return;
    thread_ = std::jthread([this](std::stop_token st) { run(st); });
}

void Producer::stop()
{
    if (!thread_.joinable()) return;
    thread_.request_stop();
    wake_cv_.notify_all();
    thread_.join();
}

void Producer::apply(const db::AppliedConfig& config)
{
    rate_ms_.store(config.rate_ms);
    amplitude_.store(config.amplitude);
    active_.store(config.is_active);
    {
        std::lock_guard lock(wake_mtx_);
        ++generation_;
    }
    wake_cv_.notify_all();
    log::info(kLog, std::format("{}: rate_ms={} amplitude={} active={}", db::table_name(table_),
                                config.rate_ms, config.amplitude, config.is_active));
}

double Producer::sample(int item_id, std::int64_t seq)
{
    const double a = amplitude_.load();
    if (table_ == db::DummyTable::Dummy1)
    {
        const double phase = (item_id - 1) * std::numbers::pi / 4.0;
        return a * std::sin(2.0 * std::numbers::pi * static_cast<double>(seq) / 40.0 + phase);
    }
    return a * static_cast<double>(seq % 50) / 50.0 + 0.05 * a * noise_(rng_);
}

void Producer::run(std::stop_token st)
{
    const auto name = db::table_name(table_);
    log::info(kLog, std::format("{}: producer started ({} item(s))", name, items_));

    std::int64_t seq = 0;
    while (!st.stop_requested())
    {
        if (active_.load())
        {
            ++seq;
            for (int item = 1; item <= items_ && !st.stop_requested(); ++item)
            {
                try
                {
                    if (table_ == db::DummyTable::Dummy3)
                    {
                        // Madrid. Bearing 0 is north; each item is offset by pi/4.
                        constexpr double kOriginLon = -3.7038;
                        constexpr double kOriginLat = 40.4168;
                        const double phase = (item - 1) * std::numbers::pi / 4.0;
                        const double bearing =
                            2.0 * std::numbers::pi * static_cast<double>(seq) / 40.0 + phase;
                        db_.upsert_dummy_with_history(
                            table_, db::TrackSample{item, kOriginLon, kOriginLat, amplitude_.load(), bearing, seq});
                    }
                    else
                    {
                        db_.upsert_dummy_with_history(table_, db::DummySample{item, sample(item, seq), seq});
                    }
                    written_.fetch_add(1);
                }
                catch (const std::exception& e)
                {
                    log::error(kLog, std::format("{}: write failed: {}", name, e.what()));
                }
            }
        }

        // Sleep one period, or less if a new configuration arrives.
        std::unique_lock lock(wake_mtx_);
        const std::uint64_t seen = generation_;
        wake_cv_.wait_for(lock, st, std::chrono::milliseconds(rate_ms_.load()),
                          [&] { return generation_ != seen; });
    }
    log::info(kLog, std::format("{}: producer stopped after {} sample(s)", name, written_.load()));
}

} // namespace pgdemo::controller
