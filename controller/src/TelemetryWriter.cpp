#include "TelemetryWriter.hpp"

#include <log/Log.hpp>

#include <chrono>
#include <format>
#include <type_traits>
#include <utility>
#include <vector>

namespace pgdemo::controller {

namespace {

constexpr std::string_view kLog = "fleet";
constexpr std::size_t kNodeCapacity = 256;
constexpr std::size_t kBatch = 64;
constexpr auto kFlush = std::chrono::milliseconds(100);

} // namespace

TelemetryWriter::TelemetryWriter(db::DatabaseManager& db) : db_(db) {}

TelemetryWriter::~TelemetryWriter()
{
    shutdown();
}

void TelemetryWriter::push(const std::string& name, Item sample)
{
    std::lock_guard map_lock(map_mu_);
    if (stopped_) return;
    auto& slot = queues_[name];
    if (!slot)
    {
        slot = std::make_unique<Queue>();
        Queue* raw = slot.get();
        raw->thread = std::jthread([this, raw, name] { run(*raw, name); });
    }
    std::lock_guard queue_lock(slot->mu);
    if (slot->samples.size() >= kNodeCapacity)
    {
        ++slot->dropped;
        return;
    }
    slot->samples.push_back(std::move(sample));
    if (slot->samples.size() >= kBatch) slot->cv.notify_one();
}

void TelemetryWriter::forget(const std::string& name)
{
    std::unique_ptr<Queue> queue;
    {
        std::lock_guard lock(map_mu_);
        const auto it = queues_.find(name);
        if (it == queues_.end()) return;
        queue = std::move(it->second);
        queues_.erase(it);
    }
    stop_and_join(std::move(queue));
}

void TelemetryWriter::shutdown()
{
    std::map<std::string, std::unique_ptr<Queue>> all;
    {
        std::lock_guard lock(map_mu_);
        if (stopped_) return;
        stopped_ = true;
        all.swap(queues_);
    }
    for (auto& [_, queue] : all) stop_and_join(std::move(queue));
}

void TelemetryWriter::stop_and_join(std::unique_ptr<Queue> queue)
{
    if (!queue) return;
    {
        std::lock_guard lock(queue->mu);
        queue->stopping = true;
    }
    queue->cv.notify_one();
    if (queue->thread.joinable()) queue->thread.join();
}

void TelemetryWriter::run(Queue& queue, std::string name)
{
    for (;;)
    {
        try
        {
            std::deque<Item> taken;
            std::size_t dropped = 0;
            bool stop = false;
            {
                std::unique_lock lock(queue.mu);
                queue.cv.wait_for(lock, kFlush, [&] { return queue.stopping || queue.samples.size() >= kBatch; });
                taken.swap(queue.samples);
                dropped = queue.dropped;
                queue.dropped = 0;
                stop = queue.stopping;
            }
            if (dropped != 0)
                log::warn(kLog, std::format("{}: telemetry queue full; dropped {} sample(s)", name, dropped));
            if (!taken.empty()) flush(name, taken);
            if (stop) return;
        }
        catch (const std::exception& e)
        {
            log::error(kLog, name + ": telemetry thread: " + e.what());
        }
    }
}

void TelemetryWriter::flush(const std::string& name, std::deque<Item>& taken)
{
    std::vector<db::DummySample> scalar;
    std::vector<db::TrackSample> gps;
    std::vector<db::XySample> xy;
    std::vector<db::XyzSample> xyz;
    while (!taken.empty())
    {
        auto item = std::move(taken.front());
        taken.pop_front();
        std::visit(
            [&](auto&& sample) {
                using T = std::decay_t<decltype(sample)>;
                if constexpr (std::is_same_v<T, db::DummySample>) scalar.push_back(std::move(sample));
                else if constexpr (std::is_same_v<T, db::TrackSample>) gps.push_back(std::move(sample));
                else if constexpr (std::is_same_v<T, db::XySample>) xy.push_back(std::move(sample));
                else xyz.push_back(std::move(sample));
            },
            std::move(item));
    }
    try
    {
        db_.write_telemetry_batch(scalar, gps, xy, xyz);
    }
    catch (const std::exception& e)
    {
        log::warn(kLog, name + ": telemetry batch not stored: " + e.what());
    }
}

} // namespace pgdemo::controller
