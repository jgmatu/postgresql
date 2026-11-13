// pgdemo-device: one simulated device, a ROS 2 worker node.
//
// The manager starts it (fork + execv) or it is started on another host in
// the same ROS domain. It never opens a database. AppBootstrap prepares the
// PUF and the factory identity. The process then instantiates one telemetry
// worker, which publishes pgdemo/worker and subscribes to pgdemo/manager.

#include "AppBootstrap.hpp"
#include "GpsWorker.hpp"
#include "Options.hpp"
#include "ScalarWorker.hpp"
#include "XyWorker.hpp"
#include "XyzWorker.hpp"

#include <pki/SecurityPolicy.hpp>

#include <log/Log.hpp>

#include <rclcpp/rclcpp.hpp>

#include <csignal>
#include <format>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace pgdemo::device {
namespace {

struct UsageError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

void usage(std::ostream& out)
{
    out << "Usage: pgdemo-device --name NAME --type TYPE --payload scalar|gps|xy|xyz --dir DIR\n"
           "                     [--enroll REQUEST_ID | --rotate REQUEST_ID]\n"
           "\n"
           "  --name      device name; also the CN of its certificate\n"
           "  --type      device type (selects the generator)\n"
           "  --payload   scalar, gps, xy or xyz\n"
           "  --dir       key directory: key.pem and cert.pem\n"
           "  --enroll    generate a key, send a CSR and store the issued certificate\n"
           "  --rotate    same, replacing the current key once the new certificate arrives\n"
           "  --device-id id stored in devices.puf; a missing id is created and reused\n"
           "  --puf       path of devices.puf\n"
           "  --puf-dir   directory of this worker's seed file; created once and kept\n"
           "  --factory   HOST:PORT of the factory, used when cert.pem is absent\n"
           "  --factory-root  factory root certificate\n"
           "  --manager-cert  manager certificate used to verify its CRL\n";
}

Options parse_args(int argc, char** argv)
{
    Options o;
    for (int i = 1; i < argc; ++i)
    {
        const std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h")
        {
            usage(std::cout);
            std::exit(kOk);
        }
        if (i + 1 >= argc) throw UsageError(std::format("{} needs a value", arg));
        const std::string value = argv[++i];
        if (arg == "--name") o.name = value;
        else if (arg == "--type") o.type = value;
        else if (arg == "--payload") o.payload = value;
        else if (arg == "--dir") o.dir = value;
        else if (arg == "--enroll" || arg == "--rotate")
        {
            o.mode = arg.substr(2);
            o.request_id = std::stoll(value);
        }
        else if (arg == "--device-id") o.device_id = std::stoll(value);
        else if (arg == "--puf") o.puf = value;
        else if (arg == "--puf-dir") o.puf_dir = value;
        else if (arg == "--factory") o.factory = value;
        else if (arg == "--factory-root") o.factory_root = value;
        else if (arg == "--manager-cert") o.manager_cert = value;
        else throw UsageError(std::format("unknown option {}", arg));
    }
    if (o.name.empty() || o.type.empty() || o.dir.empty())
        throw UsageError("--name, --type and --dir are required");
    if (o.payload != "scalar" && o.payload != "gps" && o.payload != "xy" && o.payload != "xyz")
        throw UsageError("--payload must be scalar, gps, xy or xyz");
    return o;
}

std::shared_ptr<Worker> make_worker(Options opts, int& rc)
{
    switch (generator_for(opts.type, opts.payload))
    {
    case Generator::GpsOrbit:
        return std::make_shared<GpsWorker>(std::move(opts), rc);
    case Generator::Scatter:
        return std::make_shared<XyWorker>(std::move(opts), rc);
    case Generator::Cloud:
        return std::make_shared<XyzWorker>(std::move(opts), rc);
    case Generator::WhiteNoise:
        return std::make_shared<ScalarWorker>(std::move(opts), rc, std::make_unique<WhiteNoise>());
    case Generator::Sawtooth:
        return std::make_shared<ScalarWorker>(std::move(opts), rc, std::make_unique<Sawtooth>());
    case Generator::Sine:
        return std::make_shared<ScalarWorker>(std::move(opts), rc, std::make_unique<Sine>());
    }
    throw std::logic_error("unhandled generator");
}

} // namespace

int run_main(int argc, char** argv)
{
    Options opts;
    try
    {
        opts = parse_args(argc, argv);
    }
    catch (const std::exception& e)
    {
        std::cerr << "pgdemo-device: " << e.what() << "\n\n";
        usage(std::cerr);
        return kUsage;
    }

    std::signal(SIGPIPE, SIG_IGN);
    try
    {
        AppBootstrap bootstrap(opts);
        bootstrap.run();
    }
    catch (const std::exception& e)
    {
        pki::clear_ros_security();
        log::error("device", e.what());
        return kFailed;
    }
    rclcpp::InitOptions init;
    init.shutdown_on_signal = true;
    rclcpp::init(argc, argv, init);

    // A signal ends the spin without fail(); that is a clean exit. fail() overwrites this.
    int rc = kOk;
    try
    {
        auto node = make_worker(std::move(opts), rc);
        rclcpp::spin(node);
    }
    catch (const std::exception& e)
    {
        log::error("device", e.what());
        rc = kFailed;
    }
    if (rclcpp::ok()) rclcpp::shutdown();
    return rc;
}

} // namespace pgdemo::device

int main(int argc, char** argv) { return pgdemo::device::run_main(argc, argv); }
