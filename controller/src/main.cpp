// pgdemo-controller: zone B process.
//
// Writes the data plane (device_state and the partitioned telemetry tables) for
// every registered device and applies desired configuration from the
// device_config_pending queue. It never talks to the server; PostgreSQL is the
// only channel.
//
//   device_events    --> start / stop the device's Producer
//   config_requested --> drain PENDING --> validate --> commit applied | error
//                                                  \--> Producer::apply()

#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <format>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <db/DatabaseManager.hpp>
#include <log/Log.hpp>

#include "Producer.hpp"

namespace {

using namespace pgdemo;

constexpr std::string_view kLog = "controller";

template <typename F>
struct ScopeExit
{
    F fn;
    ~ScopeExit() { fn(); }
};

struct Options
{
    std::string conninfo;
    std::size_t pool_size = db::DatabaseManager::kDefaultPoolSize;
    int pending_max_age_s = 600;
    int maintenance_interval_s = 3600;
};

void usage()
{
    std::cout << "Usage: pgdemo-controller [options]\n"
                 "  --conninfo STR             libpq connection string (default: PG* environment)\n"
                 "  --pool-size N              query connections (default 8)\n"
                 "  --pending-max-age-s N      PENDING requests older than this become TIMEOUT (default 600)\n"
                 "  --maintenance-interval-s N partition maintenance period (default 3600)\n";
}

Options parse(int argc, char** argv)
{
    Options o;
    for (int i = 1; i < argc; ++i)
    {
        const std::string_view a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) throw std::invalid_argument(std::string(a) + " needs a value");
            return argv[++i];
        };
        if (a == "--conninfo") o.conninfo = next();
        else if (a == "--pool-size") o.pool_size = std::stoul(next());
        else if (a == "--pending-max-age-s") o.pending_max_age_s = std::stoi(next());
        else if (a == "--maintenance-interval-s") o.maintenance_interval_s = std::stoi(next());
        else if (a == "-h" || a == "--help") { usage(); std::exit(0); }
        else throw std::invalid_argument("unknown option " + std::string(a));
    }
    if (o.pool_size < 1) throw std::invalid_argument("--pool-size must be at least 1");
    if (o.pending_max_age_s < 1) throw std::invalid_argument("--pending-max-age-s must be positive");
    if (o.maintenance_interval_s < 1) throw std::invalid_argument("--maintenance-interval-s must be positive");
    return o;
}

/// Validation of a desired configuration. Pure: no side effects.
db::ConfigOutcome decide(const db::ConfigRequest& r)
{
    if (r.rate_ms < 50 || r.rate_ms > 10000)
        return db::ConfigOutcome::failure("INVALID_RATE",
                                          std::format("rate_ms {} outside [50, 10000]", r.rate_ms));
    if (!(r.amplitude > 0.0) || r.amplitude > 1000.0)
        return db::ConfigOutcome::failure("INVALID_AMPLITUDE",
                                          std::format("amplitude {} outside (0, 1000]", r.amplitude));
    return db::ConfigOutcome::success();
}

void ensure_partitions(db::DatabaseManager& db)
{
    for (const auto p : db::kAllPayloads)
    {
        for (const auto& name : db.ensure_history_partitions(p))
            log::info(kLog, "created partition " + name);
    }
}

/**
 * Running producers, one per device.
 *
 * Changed from the main thread (start-up) and from the listener thread
 * (`device_events`), read from the drain's `on_applied`. The mutex covers the
 * map only: database calls and `Producer::stop()` (which joins a thread) run
 * outside it.
 */
class Fleet
{
public:
    explicit Fleet(db::DatabaseManager& db) : db_(db) {}

    /// Initialise the device's applied configuration if needed and start its producer.
    void add(const db::Device& device)
    {
        {
            std::lock_guard lock(mtx_);
            if (producers_.contains(device.device_id)) return;
        }
        const auto cfg = db_.init_device_config(device.device_id);
        if (!cfg)
        {
            log::warn(kLog, std::format("{}: device vanished before it could start", device.name));
            return;
        }
        auto p = std::make_unique<controller::Producer>(db_, device, *cfg);
        std::lock_guard lock(mtx_);
        auto [it, inserted] = producers_.try_emplace(device.device_id, std::move(p));
        if (inserted) it->second->start();
    }

    /// Stop and forget the device's producer, if any.
    void remove(int device_id)
    {
        std::unique_ptr<controller::Producer> p;
        {
            std::lock_guard lock(mtx_);
            auto node = producers_.extract(device_id);
            if (node.empty()) return;
            p = std::move(node.mapped());
        }
        p->stop();
        log::info(kLog, std::format("{}: producer removed", p->device().name));
    }

    /// Forward a committed configuration to the device's producer.
    void apply(const db::AppliedConfig& c)
    {
        std::lock_guard lock(mtx_);
        if (const auto it = producers_.find(c.device_id); it != producers_.end()) it->second->apply(c);
    }

    void stop_all()
    {
        std::map<int, std::unique_ptr<controller::Producer>> all;
        {
            std::lock_guard lock(mtx_);
            all.swap(producers_);
        }
        for (auto& [_, p] : all) p->stop();
    }

    std::size_t size()
    {
        std::lock_guard lock(mtx_);
        return producers_.size();
    }

private:
    db::DatabaseManager& db_;
    std::mutex mtx_;
    std::map<int, std::unique_ptr<controller::Producer>> producers_;
};

} // namespace

int main(int argc, char** argv)
{
    Options opt;
    try
    {
        opt = parse(argc, argv);
    }
    catch (const std::exception& e)
    {
        std::cerr << "pgdemo-controller: " << e.what() << "\n";
        usage();
        return 2;
    }

    // Signals are handled synchronously by the main thread only. Block them
    // before any thread starts so every thread inherits the mask.
    sigset_t sigs;
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGINT);
    sigaddset(&sigs, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &sigs, nullptr);

    try
    {
        db::DatabaseManager db(opt.conninfo, "pgdemo-controller", opt.pool_size);
        db.connect();

        ensure_partitions(db);

        Fleet fleet(db);

        // On every exit path: producers first (they write through db), then the
        // listener (its callbacks reach the fleet), then the connections.
        ScopeExit shutdown{[&] {
            fleet.stop_all();
            db.disconnect();
        }};

        const auto max_age = std::chrono::seconds(opt.pending_max_age_s);
        auto drain = [&] {
            const auto stats = db.drain_pending(
                decide, [&](const db::AppliedConfig& c) { fleet.apply(c); }, max_age);
            if (stats.applied + stats.rejected + stats.expired > 0)
            {
                log::info(kLog, std::format("queue drained: {} applied, {} rejected, {} expired",
                                            stats.applied, stats.rejected, stats.expired));
            }
        };

        // Registration phase, then activation.
        db.register_listen_async(std::string(db::channel::kConfigRequested),
                                 [&](const boost::json::object& msg) {
                                     log::debug(kLog, "config_requested " + boost::json::serialize(msg));
                                     drain();
                                 });
        db.register_listen_async(
            std::string(db::channel::kDeviceEvents), [&](const boost::json::object& msg) {
                const auto* payload = msg.if_contains("payload");
                if (!payload || !payload->is_object()) return;
                const auto& o = payload->as_object();
                const auto* action = o.if_contains("action");
                const auto* id = o.if_contains("device_id");
                if (!action || !action->is_string() || !id || !id->is_int64()) return;
                const int device_id = static_cast<int>(id->as_int64());
                if (action->as_string() == "INSERT")
                {
                    if (const auto dev = db.load_device(device_id)) fleet.add(*dev);
                }
                else if (action->as_string() == "DELETE")
                {
                    fleet.remove(device_id);
                }
            });
        db.run_listener_loop();

        // LISTEN is active before these reads, so a device or request committed
        // in between is either seen here or announced by a notification.
        for (const auto& dev : db.load_devices()) fleet.add(dev);
        drain();

        log::info(kLog, std::format("running {} device(s); Ctrl-C to stop", fleet.size()));

        const timespec period{opt.maintenance_interval_s, 0};
        for (;;)
        {
            const int sig = sigtimedwait(&sigs, nullptr, &period);
            if (sig == SIGINT || sig == SIGTERM)
            {
                log::info(kLog, std::format("signal {} received, shutting down", sig));
                break;
            }
            if (!db.listener_alive())
            {
                log::error(kLog, "listener connection lost; exiting so a supervisor can restart us");
                return 1;
            }
            try
            {
                ensure_partitions(db);
            }
            catch (const std::exception& e)
            {
                log::error(kLog, std::string("partition maintenance failed: ") + e.what());
            }
        }

    }
    catch (const std::exception& e)
    {
        log::error(kLog, e.what());
        return 1;
    }
    return 0;
}
