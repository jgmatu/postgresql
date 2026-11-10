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
//   config_requested     --> drain PENDING --> validate --> commit --> config frame

#include <pthread.h>
#include <unistd.h>

#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

#include <boost/asio.hpp>

#include <db/DatabaseManager.hpp>
#include <log/Log.hpp>
#include <pgdemo/Topics.hpp>

#include <pgdemo_msgs/msg/bye.hpp>
#include <pgdemo_msgs/msg/certificate.hpp>
#include <pgdemo_msgs/msg/challenge.hpp>
#include <pgdemo_msgs/msg/config.hpp>
#include <pgdemo_msgs/msg/csr.hpp>
#include <pgdemo_msgs/msg/error.hpp>
#include <pgdemo_msgs/msg/gps_sample.hpp>
#include <pgdemo_msgs/msg/heartbeat.hpp>
#include <pgdemo_msgs/msg/hello.hpp>
#include <pgdemo_msgs/msg/proof.hpp>
#include <pgdemo_msgs/msg/rejected.hpp>
#include <pgdemo_msgs/msg/scalar_sample.hpp>

#include <rclcpp/rclcpp.hpp>

#include "Fleet.hpp"

namespace {

using namespace pgdemo;
namespace fs = std::filesystem;

constexpr std::string_view kLog = "controller";

struct Options
{
    std::string conninfo;
    std::size_t pool_size = db::DatabaseManager::kDefaultPoolSize;
    controller::FleetOptions fleet;
};

void usage()
{
    std::cout << "Usage: pgdemo-controller [options]\n"
                 "  --conninfo STR               libpq connection string (default: PG* environment)\n"
                 "  --pool-size N                query connections and database threads (default 8)\n"
                 "  --max-devices N              device processes run at once; the rest are UNMANAGED (default 16)\n"
                 "  --device-bin PATH            pgdemo-device executable (default: next to this binary)\n"
                 "  --devices-dir DIR            key directory of every device (default run/devices)\n"
                 "  --enrollment-timeout-s N     open enrollments older than this become EXPIRED (default 120)\n"
                 "  --pending-max-age-s N        PENDING requests older than this become TIMEOUT (default 600)\n"
                 "  --maintenance-interval-s N   partition maintenance period (default 3600)\n";
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
        else if (a == "--pool-size") o.pool_size = static_cast<std::size_t>(positive(a));
        else if (a == "--max-devices") o.fleet.max_devices = static_cast<std::size_t>(positive(a));
        else if (a == "--device-bin") o.fleet.device_bin = next();
        else if (a == "--devices-dir") devices_dir = next();
        else if (a == "--enrollment-timeout-s") o.fleet.enrollment_timeout = std::chrono::seconds(positive(a));
        else if (a == "--pending-max-age-s") o.fleet.pending_max_age = std::chrono::seconds(positive(a));
        else if (a == "--maintenance-interval-s") o.fleet.maintenance_interval = std::chrono::seconds(positive(a));
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

    // Blocked before any thread exists, so every thread inherits the mask; the
    // io thread unblocks them for Asio's signal_set. Children start with an
    // empty mask (see spawn_process).
    sigset_t sigs;
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGINT);
    sigaddset(&sigs, SIGTERM);
    sigaddset(&sigs, SIGCHLD);
    pthread_sigmask(SIG_BLOCK, &sigs, nullptr);
    std::signal(SIGPIPE, SIG_IGN);

    try
    {
        db::DatabaseManager db(opt.conninfo, "pgdemo-controller", opt.pool_size);
        db.connect();
        for (const auto p : db::kAllPayloads)
            for (const auto& name : db.ensure_history_partitions(p)) log::info(kLog, "created partition " + name);

        // rclcpp must not take SIGINT: the fleet's signal_set owns shutdown.
        rclcpp::InitOptions ros_init;
        ros_init.shutdown_on_signal = false;
        rclcpp::init(argc, argv, ros_init);
        auto ros_node = std::make_shared<rclcpp::Node>("pgdemo_controller");
        const auto qos = rclcpp::QoS(rclcpp::KeepLast(16)).reliable();
        const auto hello_qos = rclcpp::QoS(rclcpp::KeepLast(1)).reliable().transient_local();
        namespace msg = pgdemo_msgs::msg;
        auto bye = ros_node->create_publisher<msg::Bye>(topics::kBye, qos);
        auto certificate = ros_node->create_publisher<msg::Certificate>(topics::kCertificate, qos);
        auto rejected = ros_node->create_publisher<msg::Rejected>(topics::kRejected, qos);
        auto challenge = ros_node->create_publisher<msg::Challenge>(topics::kChallenge, qos);
        auto config = ros_node->create_publisher<msg::Config>(topics::kConfig, qos);

        boost::asio::io_context io(1);
        boost::asio::thread_pool pool(opt.pool_size);
        controller::Downlink downlink;
        downlink.bye = [bye](msg::Bye m) { bye->publish(m); };
        downlink.certificate = [certificate](msg::Certificate m) { certificate->publish(m); };
        downlink.rejected = [rejected](msg::Rejected m) { rejected->publish(m); };
        downlink.challenge = [challenge](msg::Challenge m) { challenge->publish(m); };
        downlink.config = [config](msg::Config m) { config->publish(m); };
        controller::Fleet fleet(io, pool, db, opt.fleet, std::move(downlink));

        auto hello = ros_node->create_subscription<msg::Hello>(
            topics::kHello, hello_qos, [&fleet](msg::Hello::ConstSharedPtr m) { fleet.on_hello(*m); });
        auto heartbeat = ros_node->create_subscription<msg::Heartbeat>(
            topics::kHeartbeat, qos, [&fleet](msg::Heartbeat::ConstSharedPtr m) { fleet.on_uplink(*m); });
        auto error = ros_node->create_subscription<msg::Error>(
            topics::kError, qos, [&fleet](msg::Error::ConstSharedPtr m) { fleet.on_uplink(*m); });
        auto csr = ros_node->create_subscription<msg::Csr>(
            topics::kCsr, qos, [&fleet](msg::Csr::ConstSharedPtr m) { fleet.on_uplink(*m); });
        auto proof = ros_node->create_subscription<msg::Proof>(
            topics::kProof, qos, [&fleet](msg::Proof::ConstSharedPtr m) { fleet.on_uplink(*m); });
        auto scalar = ros_node->create_subscription<msg::ScalarSample>(
            topics::kScalar, qos, [&fleet](msg::ScalarSample::ConstSharedPtr m) { fleet.on_uplink(*m); });
        auto gps = ros_node->create_subscription<msg::GpsSample>(
            topics::kGps, qos, [&fleet](msg::GpsSample::ConstSharedPtr m) { fleet.on_uplink(*m); });
        fleet.acquire_lock();

        // Registration phase, then activation. Callbacks only post to the io thread.
        for (const auto channel : {db::channel::kEnrollmentRequested, db::channel::kEnrollmentEvents,
                                   db::channel::kDeviceEvents, db::channel::kConfigRequested})
            db.register_listen_async(std::string(channel),
                                     [&fleet](boost::json::object msg) { fleet.notify(std::move(msg)); });
        db.run_listener_loop();

        // LISTEN is active before boot() reads devices and enrollments, so
        // anything committed in between is either read or announced.
        fleet.start();
        std::jthread ros([&ros_node] { rclcpp::spin(ros_node); });
        // Destroyed before `ros`, so an exception still unblocks the spin.
        struct StopRos
        {
            ~StopRos() { rclcpp::shutdown(); }
        } stop_ros;
        pthread_sigmask(SIG_UNBLOCK, &sigs, nullptr);
        io.run();
        (void)stop_ros;

        // Database work still queued finishes; then the listener stops before
        // the fleet it posts to goes away.
        pool.join();
        db.disconnect();
        return fleet.exit_code();
    }
    catch (const std::exception& e)
    {
        log::error(kLog, e.what());
        return 1;
    }
}
