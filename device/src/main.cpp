// pgdemo-device: one simulated device, started by the controller (fork + execv).
//
// It connects to the controller's Unix socket, proves its identity with its
// private key (or first obtains a certificate, when enrolling or rotating),
// then streams samples until the controller says bye or a signal arrives.
// One thread, Boost.Asio C++20 coroutines.

#include "Stream.hpp"

#include <pki/CertVerifier.hpp>
#include <pki/Certificate.hpp>
#include <pki/DeviceKey.hpp>
#include <wire/Frame.hpp>

#include <log/Log.hpp>

#include <boost/asio.hpp>

#include <sys/stat.h>

#include <csignal>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <string>

namespace pgdemo::device {
namespace {

namespace asio = boost::asio;
namespace fs = std::filesystem;
using asio::awaitable;
using asio::use_awaitable;
using Socket = asio::local::stream_protocol::socket;

/// Exit codes the controller logs.
enum Exit : int {
    kOk = 0,         ///< bye from the controller, or SIGTERM/SIGINT
    kFailed = 1,     ///< protocol or I/O error
    kUsage = 2,      ///< bad arguments
    kRejected = 3,   ///< enrollment refused by the CA
    kNoIdentity = 4, ///< no key or certificate on disk
};

void usage(std::ostream& out)
{
    out << "Usage: pgdemo-device --name NAME --type TYPE --payload scalar|gps --socket PATH --dir DIR\n"
           "                     [--enroll REQUEST_ID | --rotate REQUEST_ID]\n"
           "\n"
           "  --name      device name; also the CN of its certificate\n"
           "  --type      device type (selects the generator)\n"
           "  --payload   scalar or gps\n"
           "  --socket    controller socket\n"
           "  --dir       key directory: key.pem and cert.pem\n"
           "  --enroll    generate a key, send a CSR and store the issued certificate\n"
           "  --rotate    same, replacing the current key once the new certificate arrives\n";
}

struct Options
{
    std::string name;
    std::string type;
    std::string payload;
    std::string socket;
    fs::path dir;
    std::string mode = "run";
    std::int64_t request_id = 0;
};

struct UsageError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

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
        else if (arg == "--socket") o.socket = value;
        else if (arg == "--dir") o.dir = value;
        else if (arg == "--enroll" || arg == "--rotate")
        {
            o.mode = arg.substr(2);
            o.request_id = std::stoll(value);
        }
        else throw UsageError(std::format("unknown option {}", arg));
    }
    if (o.name.empty() || o.type.empty() || o.socket.empty() || o.dir.empty())
        throw UsageError("--name, --type, --socket and --dir are required");
    if (o.payload != "scalar" && o.payload != "gps") throw UsageError("--payload must be scalar or gps");
    return o;
}

/// The controller refused us: enrollment rejected, or bye during the handshake.
struct Refused : std::runtime_error
{
    Refused(int c, const std::string& what) : std::runtime_error(what), code(c) {}
    int code;
};

std::string read_file(const fs::path& p)
{
    std::ifstream in(p);
    if (!in) throw std::runtime_error("cannot read " + p.string());
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void write_file(const fs::path& p, const std::string& data)
{
    const fs::path tmp = p.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) throw std::runtime_error("cannot write " + tmp.string());
        out << data;
    }
    fs::rename(tmp, p);
}

class Device
{
public:
    Device(asio::io_context& io, Options opts)
        : opts_(std::move(opts))
        , socket_(io)
        , stream_(socket_, generator_for(opts_.type, opts_.payload), "device " + opts_.name)
    {
    }

    awaitable<int> run()
    {
        fs::create_directories(opts_.dir);
        ::chmod(opts_.dir.c_str(), 0700);

        co_await socket_.async_connect(asio::local::stream_protocol::endpoint(opts_.socket), use_awaitable);
        boost::json::object hello{{"type", "hello"}, {"device", opts_.name}, {"mode", opts_.mode}};
        if (opts_.request_id != 0) hello["request_id"] = opts_.request_id;
        co_await wire::write_frame(socket_, hello);

        if (opts_.mode != "run") co_await obtain_certificate();

        if (!fs::exists(key_path()) || !fs::exists(cert_path()))
        {
            co_await wire::write_frame(socket_, {{"type", "error"},
                                                 {"code", "NO_IDENTITY"},
                                                 {"detail", "no key.pem or cert.pem in " + opts_.dir.string()}});
            throw Refused(kNoIdentity, "no key or certificate; the device must be enrolled first");
        }
        const auto key = pki::DeviceKey::load(key_path().string());
        const std::string cert = read_file(cert_path());

        co_await prove(key, cert);
        co_await wait_config();
        log::info(tag(), std::format("online (device_id {}, rate_ms {}, amplitude {}, active {})",
                                     stream_.device_id(), stream_.rate_ms(), stream_.amplitude(), stream_.active()));

        co_await stream_.run();
        co_return kOk;
    }

private:
    [[nodiscard]] fs::path key_path() const { return opts_.dir / "key.pem"; }
    [[nodiscard]] fs::path cert_path() const { return opts_.dir / "cert.pem"; }
    [[nodiscard]] std::string tag() const { return "device " + opts_.name; }

    /// Enroll or rotate: new key, CSR, wait for the certificate signed by the CA.
    awaitable<void> obtain_certificate()
    {
        const auto key = pki::DeviceKey::generate();
        const fs::path next = opts_.dir / "key.next.pem";
        key.save(next.string());
        co_await wire::write_frame(socket_, {{"type", "csr"}, {"csr", key.csr_pem(opts_.name)}});
        log::info(tag(), std::format("{}: CSR sent for request {}", opts_.mode, opts_.request_id));

        const auto msg = co_await wire::read_frame(socket_);
        const auto type = wire::type_of(msg);
        if (type != "cert")
        {
            fs::remove(next);
            const auto code = wire::get_string(msg, "code").value_or(type);
            const auto detail = wire::get_string(msg, "detail").value_or("");
            throw Refused(kRejected, std::format("certificate refused: {} {}", code, detail));
        }
        const auto cert = wire::get_string(msg, "cert").value_or("");

        // The certificate must carry our key before it replaces anything.
        const auto probe = pki::random_bytes(32);
        if (!pki::CertVerifier::verify_signature(cert, probe, key.sign(probe)))
        {
            fs::remove(next);
            throw std::runtime_error("issued certificate does not match the generated key");
        }
        write_file(opts_.dir / "cert.next.pem", cert);
        fs::rename(next, key_path());
        fs::rename(opts_.dir / "cert.next.pem", cert_path());
        log::info(tag(), std::format("certificate stored (serial {})", pki::inspect_certificate(cert).serial));
    }

    /// Challenge-response: sign the controller's nonce bound to our name and certificate.
    awaitable<void> prove(const pki::DeviceKey& key, const std::string& cert)
    {
        const auto msg = co_await wire::read_frame(socket_);
        if (wire::type_of(msg) == "bye")
            throw Refused(kOk, "controller refused: " + wire::get_string(msg, "reason").value_or(""));
        if (wire::type_of(msg) != "challenge") throw std::runtime_error("expected challenge, got " + wire::type_of(msg));
        const auto nonce = pki::base64_decode(wire::get_string(msg, "nonce").value_or(""));
        const auto fingerprint = pki::inspect_certificate(cert).fingerprint;
        const auto signature = key.sign(pki::proof_message(nonce, opts_.name, fingerprint));
        co_await wire::write_frame(socket_,
                                   {{"type", "proof"}, {"cert", cert}, {"signature", pki::base64_encode(signature)}});
    }

    awaitable<void> wait_config()
    {
        const auto msg = co_await wire::read_frame(socket_);
        if (wire::type_of(msg) == "bye")
            throw Refused(kOk, "controller refused: " + wire::get_string(msg, "reason").value_or(""));
        if (wire::type_of(msg) != "config") throw std::runtime_error("expected config, got " + wire::type_of(msg));
        stream_.apply(msg);
    }

    Options opts_;
    Socket socket_;
    SampleStream stream_;
};

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
    const std::string tag = "device " + opts.name;

    asio::io_context io(1);
    Device device(io, opts);
    int rc = kFailed;
    asio::co_spawn(io, device.run(), [&](std::exception_ptr ep, int result) {
        if (!ep)
        {
            rc = result;
            return;
        }
        try
        {
            std::rethrow_exception(ep);
        }
        catch (const Refused& e)
        {
            (e.code == kOk ? log::info : log::error)(tag, e.what());
            rc = e.code;
        }
        catch (const boost::system::system_error& e)
        {
            log::error(tag, std::string("connection: ") + e.what());
        }
        catch (const std::exception& e)
        {
            log::error(tag, e.what());
        }
    });
    io.run();
    return rc;
}

} // namespace pgdemo::device

int main(int argc, char** argv) { return pgdemo::device::run_main(argc, argv); }
