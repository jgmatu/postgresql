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

Producer::Generator Producer::generator_for(const db::Device& device) noexcept
{
    if (device.payload == db::Payload::Gps) return Generator::GpsOrbit;
    if (device.type_id == "sawtooth") return Generator::Sawtooth;
    return Generator::Sine;
}

Producer::Producer(db::DatabaseManager& db, db::Device device, const db::AppliedConfig& initial)
    : db_(db)
    , device_(std::move(device))
    , generator_(generator_for(device_))
    , phase_(static_cast<double>(device_.device_id % 8) * std::numbers::pi / 4.0)
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
    log::info(kLog, std::format("{}: rate_ms={} amplitude={} active={}", device_.name, config.rate_ms,
                                config.amplitude, config.is_active));
}

void Producer::write_tick(std::int64_t seq)
{
    const double a = amplitude_.load();
    const double angle = 2.0 * std::numbers::pi * static_cast<double>(seq) / 40.0 + phase_;
    switch (generator_)
    {
    case Generator::GpsOrbit:
    {
        // Madrid. Bearing 0 is north.
        constexpr double kOriginLon = -3.7038;
        constexpr double kOriginLat = 40.4168;
        db_.write_gps_sample(db::TrackSample{device_.device_id, kOriginLon, kOriginLat, a, angle, seq});
        break;
    }
    case Generator::Sawtooth:
        db_.write_scalar_sample(db::DummySample{
            device_.device_id, a * static_cast<double>(seq % 50) / 50.0 + 0.05 * a * noise_(rng_), seq});
        break;
    case Generator::Sine:
        db_.write_scalar_sample(db::DummySample{device_.device_id, a * std::sin(angle), seq});
        break;
    }
}

void Producer::run(std::stop_token st)
{
    log::info(kLog, std::format("{}: producer started (device_id {}, type {})", device_.name,
                                device_.device_id, device_.type_id));

    std::int64_t seq = 0;
    while (!st.stop_requested())
    {
        if (active_.load())
        {
            ++seq;
            try
            {
                write_tick(seq);
                written_.fetch_add(1);
            }
            catch (const std::exception& e)
            {
                log::error(kLog, std::format("{}: write failed: {}", device_.name, e.what()));
            }
        }

        // Sleep one period, or less if a new configuration arrives.
        std::unique_lock lock(wake_mtx_);
        const std::uint64_t seen = generation_;
        wake_cv_.wait_for(lock, st, std::chrono::milliseconds(rate_ms_.load()),
                          [&] { return generation_ != seen; });
    }
    log::info(kLog, std::format("{}: producer stopped after {} sample(s)", device_.name, written_.load()));
}

} // namespace pgdemo::controller
