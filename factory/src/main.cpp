// pgdemo-factory: zone A process that holds the simulation root.
//
// scripts/gen-factory.sh writes run/factory/root.key before this process starts.
// If that key is missing, this process exits. It does not generate one. A worker
// sends a self-signed certificate; this process checks the signature and returns
// a new certificate for the same public key. The manager root is not issued here.
// The private key does not leave this process. The listener defaults to
// 127.0.0.1. --listen opens another address; the container uses that on a
// network the server is not attached to.

#include "AsioServer.hpp"

#include <pki/CertificateAuthority.hpp>

#include <log/Log.hpp>

#include <atomic>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {

using namespace pgdemo;
namespace fs = std::filesystem;

constexpr std::string_view kLog = "factory";
std::atomic<bool> g_stop{false};

void on_signal(int) { g_stop.store(true); }

struct Options
{
    std::string key = "run/factory/root.key";
    std::string cert = "run/factory/root.crt";
    std::string listen = "127.0.0.1";
    std::uint16_t port = 9443;
};

void usage()
{
    std::cout << "Usage: pgdemo-factory [options]\n"
                 "  --key PATH    factory root private key (default run/factory/root.key)\n"
                 "  --cert PATH   factory root certificate (default run/factory/root.crt)\n"
                 "  --listen ADDR IPv4 address to bind (default 127.0.0.1)\n"
                 "  --port N      TCP port (default 9443)\n";
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
        if (a == "--key") o.key = next();
        else if (a == "--cert") o.cert = next();
        else if (a == "--listen") o.listen = next();
        else if (a == "--port") o.port = static_cast<std::uint16_t>(std::stoul(next()));
        else if (a == "-h" || a == "--help")
        {
            usage();
            std::exit(0);
        }
        else throw std::invalid_argument("unknown option " + std::string(a));
    }
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
        std::cerr << "pgdemo-factory: " << e.what() << "\n";
        usage();
        return 2;
    }

    if (!fs::is_regular_file(opt.key) || !fs::is_regular_file(opt.cert))
    {
        log::error(kLog, "missing " + opt.key + " or " + opt.cert + "; run scripts/gen-factory.sh first");
        return 1;
    }

    try
    {
        const pki::CertificateAuthority ca(opt.key, opt.cert);
        std::signal(SIGINT, on_signal);
        std::signal(SIGTERM, on_signal);
        log::info(kLog, "signing on " + opt.listen + ":" + std::to_string(opt.port) + " (" + ca.fingerprint() + ")");
        pgdemo::factory::serve_factory_asio(opt.port, ca, g_stop, opt.listen);
        return 0;
    }
    catch (const std::exception& e)
    {
        log::error(kLog, e.what());
        return 1;
    }
}
