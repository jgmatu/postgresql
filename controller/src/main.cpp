// pgdemo-controller: zone B process.
//
// Writes the data plane (dummy1, dummy2 and their partitioned histories) and
// applies desired configuration from the dummy_config_pending queue. It never
// talks to the server; PostgreSQL is the only channel.
//
//   config_requested --> drain PENDING --> validate --> commit applied | error
//                                                  \--> Producer::apply()

#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <format>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <string_view>

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
    int items = 2;
    int pending_max_age_s = 600;
    int maintenance_interval_s = 3600;
};

void usage()
{
    std::cout << "Usage: pgdemo-controller [options]\n"
                 "  --conninfo STR             libpq connection string (default: PG* environment)\n"
                 "  --pool-size N              query connections (default 8)\n"
                 "  --items N                  items per table, 1..16 (default 2)\n"
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
        else if (a == "--items") o.items = std::stoi(next());
        else if (a == "--pending-max-age-s") o.pending_max_age_s = std::stoi(next());
        else if (a == "--maintenance-interval-s") o.maintenance_interval_s = std::stoi(next());
        else if (a == "-h" || a == "--help") { usage(); std::exit(0); }
        else throw std::invalid_argument("unknown option " + std::string(a));
    }
    if (o.pool_size < 1) throw std::invalid_argument("--pool-size must be at least 1");
    if (o.items < 1 || o.items > 16) throw std::invalid_argument("--items must be in 1..16");
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
    for (const auto t : db::kAllTables)
    {
        for (const auto& name : db.ensure_history_partitions(t))
            log::info(kLog, "created partition " + name);
    }
}

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

        // Producers start from the applied state, which survives restarts.
        std::map<db::DummyTable, std::unique_ptr<controller::Producer>> producers;
        for (const auto& cfg : db.fetch_applied_config())
        {
            producers[cfg.target] = std::make_unique<controller::Producer>(db, cfg.target, cfg, opt.items);
        }
        for (const auto t : db::kAllTables)
        {
            if (!producers.contains(t))
                throw std::runtime_error(std::format("dummy_config has no row for {}", db::table_name(t)));
        }

        // On every exit path: producers first (they write through db), then the
        // listener (its callback reaches the producers), then the connections.
        ScopeExit shutdown{[&] {
            for (auto& [_, p] : producers) p->stop();
            db.disconnect();
        }};

        const auto max_age = std::chrono::seconds(opt.pending_max_age_s);
        auto drain = [&] {
            const auto stats = db.drain_pending(
                decide, [&](const db::AppliedConfig& c) { producers.at(c.target)->apply(c); }, max_age);
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
        db.run_listener_loop();

        // LISTEN is active before this first drain, so a request committed in
        // between is either drained here or announced by a notification.
        drain();

        for (auto& [_, p] : producers) p->start();
        log::info(kLog, "running; Ctrl-C to stop");

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
