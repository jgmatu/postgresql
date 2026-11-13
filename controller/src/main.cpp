// pgdemo-controller: zone B process.
//
// Supervises one pgdemo-device worker per device (fork + execv). Each worker
// publishes pgdemo/worker and subscribes to pgdemo/manager, and proves its
// identity with its X.509 certificate. The controller is the only manager: it
// writes samples to the data plane and applies desired configuration from the
// device_config_pending queue. It never talks to the server; PostgreSQL is
// the only channel. A node started on another host in the same ROS domain is
// admitted the same way, and is not forked.
//
//   enrollment_requested --> start the device with --enroll / --rotate
//   enrollment_events    --> deliver the issued certificate to its process
//   device_events        --> stop / restart on delete, revocation, rotation
//   config_requested     --> forward PENDING on pgdemo/manager/config; the worker's
//                            config/applied is what commits SUCCESS or ERROR

#include <pthread.h>
#include <unistd.h>

#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>

#include <db/DatabaseManager.hpp>
#include <log/Log.hpp>

#include <rclcpp/rclcpp.hpp>

#include "Fleet.hpp"
#include "Manager.hpp"
#include "Origin.hpp"

namespace {

using namespace pgdemo;
namespace fs = std::filesystem;

constexpr std::string_view kLog = "controller";

struct Options
{
    std::string conninfo;
    controller::FleetOptions fleet;
    controller::OriginFiles origin;
};

void usage()
{
    std::cout << "Usage: pgdemo-controller [options]\n"
                 "  --conninfo STR               libpq connection string (default: PG* environment)\n"
                 "  --max-devices N              device processes run at once; the rest are UNMANAGED (default 16)\n"
                 "  --device-bin PATH            pgdemo-device executable (default: next to this binary)\n"
                 "  --devices-dir DIR            key directory of every device (default run/devices)\n"
                 "  --enrollment-timeout-s N     open enrollments older than this become EXPIRED (default 120)\n"
                 "  --pending-max-age-s N        PENDING requests older than this become TIMEOUT (default 600)\n"
                 "  --maintenance-interval-s N   partition maintenance period (default 3600)\n"
                 "  --manager-key PATH           manager key; absent leaves the running enrollment as it is\n"
                 "  --factory-cert PATH          factory root certificate (default run/factory/root.crt)\n"
                 "  --factory HOST:PORT          factory process (default 127.0.0.1:9443)\n";
}

/// pgdemo-device next to this executable, or in the build tree's device/.
std::string default_device_bin()
{
    std::error_code ec;
    const fs::path self = fs::read_symlink("/proc/self/exe", ec);
    if (ec) return "pgdemo-device";
    for (const auto& candidate : {self.parent_path() / "pgdemo-device",
                                  self.parent_path().parent_path() / "device" / "pgdemo-device"})
        if (fs::exists(candidate)) return candidate.string();
    return (self.parent_path() / "pgdemo-device").string();
}

Options parse(int argc, char** argv)
{
    Options o;
    std::string devices_dir = "run/devices";
    for (int i = 1; i < argc; ++i)
    {
        const std::string_view a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) throw std::invalid_argument(std::string(a) + " needs a value");
            return argv[++i];
        };
        auto positive = [&](std::string_view what) {
            const int v = std::stoi(next());
            if (v < 1) throw std::invalid_argument(std::string(what) + " must be positive");
            return v;
        };
        if (a == "--conninfo") o.conninfo = next();
        else if (a == "--max-devices") o.fleet.max_devices = static_cast<std::size_t>(positive(a));
        else if (a == "--device-bin") o.fleet.device_bin = next();
        else if (a == "--devices-dir") devices_dir = next();
        else if (a == "--enrollment-timeout-s") o.fleet.enrollment_timeout = std::chrono::seconds(positive(a));
        else if (a == "--pending-max-age-s") o.fleet.pending_max_age = std::chrono::seconds(positive(a));
        else if (a == "--maintenance-interval-s") o.fleet.maintenance_interval = std::chrono::seconds(positive(a));
        else if (a == "--manager-key") o.origin.manager_key = next();
        else if (a == "--factory-cert") o.origin.factory_cert = next();
        else if (a == "--factory")
        {
            const auto value = next();
            const auto colon = value.rfind(':');
            if (colon == std::string::npos || colon == 0) throw std::invalid_argument("--factory needs HOST:PORT");
            o.origin.factory_host = value.substr(0, colon);
            o.origin.factory_port = static_cast<std::uint16_t>(std::stoi(value.substr(colon + 1)));
        }
        else if (a == "-h" || a == "--help")
        {
            usage();
            std::exit(0);
        }
        else throw std::invalid_argument("unknown option " + std::string(a));
    }
    // Device names cannot start with a dot, so the lock file never clashes with a key directory.
    o.fleet.devices_dir = fs::absolute(devices_dir);
    o.fleet.device_bin = o.fleet.device_bin.empty() ? default_device_bin() : fs::absolute(o.fleet.device_bin).string();
    if (::access(o.fleet.device_bin.c_str(), X_OK) != 0)
        throw std::invalid_argument("device binary " + o.fleet.device_bin + " is not executable");
    return o;
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

    // Blocked before any thread exists, so every thread inherits the mask and
    // only the sigwait thread receives them. Children start with an empty mask
    // (see spawn_process).
    sigset_t sigs;
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGINT);
    sigaddset(&sigs, SIGTERM);
    sigaddset(&sigs, SIGCHLD);
    pthread_sigmask(SIG_BLOCK, &sigs, nullptr);
    std::signal(SIGPIPE, SIG_IGN);

    try
    {
        // One connection per device queue, plus one for session and enrollment writes.
        db::DatabaseManager db(opt.conninfo, "pgdemo-controller", opt.fleet.max_devices + 1);
        db.connect();
        controller::start_origin(db, opt.fleet, opt.origin);
        for (const auto p : db::kAllPayloads)
            for (const auto& name : db.ensure_history_partitions(p)) log::info(kLog, "created partition " + name);

        // `signals` outlives `manager`: the destructor sets `done` and then joins sigwait.
        controller::SignalState signals;
        controller::Manager manager(argc, argv);
        controller::TelemetryWriter writer(db);
        // Fork runs on this thread, and PR_SET_PDEATHSIG follows the thread that forked.
        controller::Fleet fleet(db, writer, opt.fleet, manager.downlink(), signals, [] { rclcpp::shutdown(); });
        manager.subscribe(fleet);
        fleet.acquire_lock();

        // Registration phase, then activation. The listener only queues; tick() drains it.
        for (const auto channel : {db::channel::kEnrollmentRequested, db::channel::kEnrollmentEvents,
                                   db::channel::kDeviceEvents, db::channel::kConfigRequested})
            db.register_listen_async(std::string(channel),
                                     [&fleet](boost::json::object msg) { fleet.notify(std::move(msg)); });
        db.run_listener_loop();

        // LISTEN is active before boot() reads devices and enrollments, so
        // anything committed in between is either read or announced.
        fleet.boot();
        manager.arm(signals);
        rclcpp::spin(manager.node());

        // Shutdown already flushed every queue. Stop the listener before the
        // fleet it notifies goes away.
        db.disconnect();
        return fleet.exit_code();
    }
    catch (const std::exception& e)
    {
        log::error(kLog, e.what());
        return 1;
    }
}
