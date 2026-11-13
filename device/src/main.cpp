// pgdemo-device: one simulated device, a ROS 2 worker node.
//
// The manager starts it (fork + execv) or it is started on another host in
// the same ROS domain. It never opens a database. It publishes pgdemo/worker
// and subscribes to pgdemo/manager, proves its identity with its private key
// (or first obtains a certificate, when enrolling or rotating), then streams
// samples until the manager says bye or a signal arrives.

#include "ConfigCheck.hpp"
#include "Stream.hpp"

#include <pki/CertVerifier.hpp>
#include <pki/Certificate.hpp>
#include <pki/DeviceKey.hpp>
#include <pki/FactoryChannel.hpp>
#include <pki/PufTable.hpp>
#include <pki/SecurityPolicy.hpp>

#include <log/Log.hpp>
#include <pgdemo/Topics.hpp>

#include <pgdemo_msgs/msg/bye.hpp>
#include <pgdemo_msgs/msg/certificate.hpp>
#include <pgdemo_msgs/msg/challenge.hpp>
#include <pgdemo_msgs/msg/config.hpp>
#include <pgdemo_msgs/msg/crl.hpp>
#include <pgdemo_msgs/msg/config_applied.hpp>
#include <pgdemo_msgs/msg/csr.hpp>
#include <pgdemo_msgs/msg/error.hpp>
#include <pgdemo_msgs/msg/gps_sample.hpp>
#include <pgdemo_msgs/msg/heartbeat.hpp>
#include <pgdemo_msgs/msg/hello.hpp>
#include <pgdemo_msgs/msg/proof.hpp>
#include <pgdemo_msgs/msg/rejected.hpp>
#include <pgdemo_msgs/msg/scalar_sample.hpp>
#include <pgdemo_msgs/msg/xy_sample.hpp>
#include <pgdemo_msgs/msg/xyz_sample.hpp>

#include <rclcpp/rclcpp.hpp>

#include <span>

#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>

namespace pgdemo::device {
namespace {

namespace fs = std::filesystem;
namespace msg = pgdemo_msgs::msg;
using namespace std::chrono_literals;

/// Exit codes the manager logs.
enum Exit : int {
    kOk = 0,         ///< bye from the manager, or SIGTERM/SIGINT
    kFailed = 1,     ///< protocol or I/O error
    kUsage = 2,      ///< bad arguments
    kRejected = 3,   ///< enrollment refused by the CA
    kNoIdentity = 4, ///< no key or certificate on disk
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
           "  --factory   HOST:PORT of the factory, used when cert.pem is absent\n"
           "  --factory-root  factory root certificate\n"
           "  --manager-cert  manager certificate used to verify its CRL\n";
}

struct Options
{
    std::string name;
    std::string type;
    std::string payload;
    fs::path dir;
    std::string mode = "run";
    std::int64_t request_id = 0;
    std::optional<std::int64_t> device_id;
    fs::path puf;
    std::string factory;
    fs::path factory_root;
    fs::path manager_cert;
    bool manager_cert_ok = false;
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
        else if (arg == "--dir") o.dir = value;
        else if (arg == "--enroll" || arg == "--rotate")
        {
            o.mode = arg.substr(2);
            o.request_id = std::stoll(value);
        }
        else if (arg == "--device-id") o.device_id = std::stoll(value);
        else if (arg == "--puf") o.puf = value;
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

/// The manager refused us: enrollment rejected, or bye during the handshake.
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

std::pair<std::string, std::uint16_t> split_endpoint(const std::string& endpoint)
{
    const auto colon = endpoint.rfind(':');
    if (colon == std::string::npos || colon == 0) throw std::runtime_error("expected HOST:PORT, got " + endpoint);
    return {endpoint.substr(0, colon), static_cast<std::uint16_t>(std::stoi(endpoint.substr(colon + 1)))};
}

/// Derive the key from devices.puf when this worker has no key yet, and ask the
/// factory for a certificate when cert.pem is absent. An existing key.pem stays.
void prepare_identity(const Options& opts)
{
    const fs::path key_path = opts.dir / "key.pem";
    const fs::path cert_path = opts.dir / "cert.pem";
    if (opts.device_id && !opts.puf.empty() && !fs::exists(key_path))
    {
        const auto seed = pki::PufTable(opts.puf).seed_for(*opts.device_id);
        pki::DeviceKey::from_seed(seed).save(key_path.string());
        log::info("device", opts.name + ": derived the key from devices.puf");
    }
    if (!opts.factory.empty() && fs::exists(key_path) && !fs::exists(cert_path))
    {
        const auto [host, port] = split_endpoint(opts.factory);
        auto key = pki::DeviceKey::load(key_path.string());
        const auto pem = pki::request_signature(host, port, "worker", opts.name, key.self_signed_pem(opts.name));
        write_file(cert_path, pem);
        log::info("device", opts.name + ": factory signed the certificate");
    }
}

std::string node_name_for(std::string name);

/// Write the SROS2 enclave and export ROS_SECURITY_* when the stored certificate
/// chains to the factory root and the manager certificate is its own root. A
/// certificate from the demo CA leaves security off.
void arm_security(Options& opts)
{
    pki::clear_ros_security();
    const fs::path key_path = opts.dir / "key.pem";
    const fs::path cert_path = opts.dir / "cert.pem";
    if (opts.factory.empty() || opts.factory_root.empty() || opts.manager_cert.empty() || !fs::exists(key_path) ||
        !fs::exists(cert_path))
        return;
    const auto root = read_file(opts.factory_root);
    const auto cert = read_file(cert_path);
    const auto manager = read_file(opts.manager_cert);
    if (!pki::self_signed_root(manager, pki::kManagerCommonName))
    {
        log::info("device", opts.name + ": manager certificate is not its own root; SROS2 stays off");
        return;
    }
    opts.manager_cert_ok = true;
    pki::CertVerifier anchors({root});
    try
    {
        anchors.verify(cert, opts.name, {});
    }
    catch (const pki::PkiError& e)
    {
        if (e.code == "CERT_CHAIN" || e.code == "CERT_SUBJECT")
        {
            log::info("device", opts.name + ": certificate is not from the factory root; SROS2 stays off");
            return;
        }
        throw;
    }
    try
    {
        const auto [host, port] = split_endpoint(opts.factory);
        const auto governance = pki::request_signed_document(host, port, "governance", "", pki::governance_document());
        const auto permissions =
            pki::request_signed_document(host, port, "permissions", opts.name, pki::permissions_document(opts.name));
        const auto keystore = fs::absolute(opts.dir.parent_path().parent_path() / "keystore");
        const auto enclave = "/" + node_name_for(opts.name);
        pki::write_enclave(pki::enclave_directory(keystore, enclave), read_file(key_path), cert,
                           pki::identity_ca_bundle(root, manager), root, governance, permissions);
        pki::enable_ros_security(keystore, enclave);
        log::info("device", opts.name + ": SROS2 enclave " + pki::enclave_directory(keystore, enclave).string());
    }
    catch (const std::exception& e)
    {
        pki::clear_ros_security();
        log::warn("device", opts.name + ": SROS2 stays off: " + e.what());
    }
}

/// Node name from a device name. ROS names allow letters, digits and underscore.
std::string node_name_for(std::string name)
{
    for (char& c : name)
        if (c == '.' || c == '-') c = '_';
    return "pgdemo_device_" + name;
}

unsigned char mode_code(std::string_view mode)
{
    if (mode == "enroll") return msg::Hello::ENROLL;
    if (mode == "rotate") return msg::Hello::ROTATE;
    return msg::Hello::RUN;
}

/// Where the handshake is. `Announce` repeats `hello` until `challenge`.
enum class Phase { Announce, Cert, Challenge, Config, Run, Done };

/**
 * @brief One worker. It publishes `pgdemo/worker` and subscribes to `pgdemo/manager`.
 *
 * `hello` is transient-local and repeated once a second until the challenge
 * arrives, so a manager that starts later still sees the node. Manager messages
 * for another device are ignored.
 */
class DeviceNode : public rclcpp::Node
{
public:
    DeviceNode(Options opts, int& exit_code)
        : rclcpp::Node(node_name_for(opts.name))
        , opts_(std::move(opts))
        , exit_code_(exit_code)
        , stream_([this](SampleStream::Emission msg) { publish_emission(std::move(msg)); },
                  generator_for(opts_.type, opts_.payload), "device " + opts_.name)
    {
        fs::create_directories(opts_.dir);
        ::chmod(opts_.dir.c_str(), 0700);
        if (fs::is_regular_file(cert_path())) own_serial_ = pki::inspect_certificate(read_file(cert_path())).serial;

        const auto qos = rclcpp::QoS(rclcpp::KeepLast(16)).reliable();
        const auto hello_qos = rclcpp::QoS(rclcpp::KeepLast(1)).reliable().transient_local();
        hello_ = create_publisher<msg::Hello>(topics::kHello, hello_qos);
        heartbeat_ = create_publisher<msg::Heartbeat>(topics::kHeartbeat, qos);
        error_ = create_publisher<msg::Error>(topics::kError, qos);
        csr_ = create_publisher<msg::Csr>(topics::kCsr, qos);
        proof_ = create_publisher<msg::Proof>(topics::kProof, qos);
        scalar_ = create_publisher<msg::ScalarSample>(topics::kScalar, qos);
        gps_ = create_publisher<msg::GpsSample>(topics::kGps, qos);
        xy_ = create_publisher<msg::XySample>(topics::kXy, qos);
        xyz_ = create_publisher<msg::XyzSample>(topics::kXyz, qos);
        applied_ = create_publisher<msg::ConfigApplied>(topics::kConfigApplied, qos);

        bye_ = create_subscription<msg::Bye>(topics::kBye, qos, [this](msg::Bye::ConstSharedPtr m) { on_bye(*m); });
        rejected_ = create_subscription<msg::Rejected>(
            topics::kRejected, qos, [this](msg::Rejected::ConstSharedPtr m) { on_rejected(*m); });
        certificate_ = create_subscription<msg::Certificate>(
            topics::kCertificate, qos, [this](msg::Certificate::ConstSharedPtr m) { on_certificate(*m); });
        challenge_ = create_subscription<msg::Challenge>(
            topics::kChallenge, qos, [this](msg::Challenge::ConstSharedPtr m) { on_challenge(*m); });
        config_ = create_subscription<msg::Config>(
            topics::kConfig, qos, [this](msg::Config::ConstSharedPtr m) { on_config(*m); });
        if (opts_.manager_cert_ok)
            crl_ = create_subscription<msg::Crl>(topics::kCrl, qos, [this](msg::Crl::ConstSharedPtr m) { on_crl(*m); });

        if (opts_.mode != "run") phase_ = Phase::Cert;
        hello_timer_ = create_wall_timer(1s, [this] { announce(); });
        announce();
    }

private:
    [[nodiscard]] fs::path key_path() const { return opts_.dir / "key.pem"; }
    [[nodiscard]] fs::path cert_path() const { return opts_.dir / "cert.pem"; }
    [[nodiscard]] std::string tag() const { return "device " + opts_.name; }
    [[nodiscard]] bool mine(const std::string& device) const { return device == opts_.name; }

    void publish_emission(SampleStream::Emission msg)
    {
        std::visit(
            [this](auto& m) {
                m.device = opts_.name;
                using T = std::decay_t<decltype(m)>;
                if constexpr (std::is_same_v<T, msg::ScalarSample>) scalar_->publish(m);
                else if constexpr (std::is_same_v<T, msg::GpsSample>) gps_->publish(m);
                else if constexpr (std::is_same_v<T, msg::XySample>) xy_->publish(m);
                else if constexpr (std::is_same_v<T, msg::XyzSample>) xyz_->publish(m);
                else heartbeat_->publish(m);
            },
            msg);
    }

    void publish_error(const std::string& code, const std::string& detail)
    {
        msg::Error err;
        err.device = opts_.name;
        err.code = code;
        err.detail = detail;
        error_->publish(err);
    }

    void fail(int code, const std::string& what)
    {
        if (phase_ == Phase::Done) return;
        phase_ = Phase::Done;
        exit_code_ = code;
        (code == kOk ? log::info : log::error)(tag(), what);
        hello_timer_.reset();
        sample_timer_.reset();
        rclcpp::shutdown();
    }

    void announce()
    {
        if (phase_ != Phase::Announce && phase_ != Phase::Cert) return;
        msg::Hello hello;
        hello.device = opts_.name;
        hello.mode = mode_code(opts_.mode);
        hello.request_id = opts_.request_id;
        hello_->publish(hello);
        // The CSR topic is volatile. Repeat the same request until the
        // certificate arrives, the way hello is repeated, so a subscriber that
        // matches late still receives it.
        if (phase_ == Phase::Cert) send_csr();
    }

    void send_csr()
    {
        try
        {
            if (!next_key_)
            {
                next_key_ = std::make_unique<pki::DeviceKey>(pki::DeviceKey::generate());
                next_key_->save((opts_.dir / "key.next.pem").string());
            }
            msg::Csr csr;
            csr.device = opts_.name;
            csr.csr = next_key_->csr_pem(opts_.name);
            csr_->publish(csr);
            if (!csr_sent_)
            {
                csr_sent_ = true;
                log::info(tag(), std::format("{}: CSR sent for request {}", opts_.mode, opts_.request_id));
            }
        }
        catch (const std::exception& e)
        {
            fail(kFailed, e.what());
        }
    }

    void store_certificate(const std::string& cert)
    {
        if (!next_key_) throw std::runtime_error("certificate arrived before the key was generated");
        const auto probe = pki::random_bytes(32);
        if (!pki::CertVerifier::verify_signature(cert, probe, next_key_->sign(probe)))
        {
            fs::remove(opts_.dir / "key.next.pem");
            throw std::runtime_error("issued certificate does not match the generated key");
        }
        write_file(opts_.dir / "cert.next.pem", cert);
        fs::rename(opts_.dir / "key.next.pem", key_path());
        fs::rename(opts_.dir / "cert.next.pem", cert_path());
        next_key_.reset();
        log::info(tag(), std::format("certificate stored (serial {})", pki::inspect_certificate(cert).serial));
    }

    void prove(const msg::Challenge& challenge)
    {
        if (!fs::exists(key_path()) || !fs::exists(cert_path()))
        {
            publish_error("NO_IDENTITY", "no key.pem or cert.pem in " + opts_.dir.string());
            throw Refused(kNoIdentity, "no key or certificate; the device must be enrolled first");
        }
        const auto key = pki::DeviceKey::load(key_path().string());
        const std::string cert = read_file(cert_path());
        const auto fingerprint = pki::inspect_certificate(cert).fingerprint;
        const auto signature = key.sign(pki::proof_message(challenge.nonce, opts_.name, fingerprint));
        if (signature.size() != 96)
            throw std::runtime_error(std::format("proof signature is {} bytes, expected 96", signature.size()));
        msg::Proof proof;
        proof.device = opts_.name;
        proof.cert = cert;
        std::copy(signature.begin(), signature.end(), proof.signature.begin());
        proof_->publish(proof);
        hello_timer_.reset();
        phase_ = Phase::Config;
    }

    void arm_samples()
    {
        sample_timer_.reset();
        sample_timer_ = create_wall_timer(stream_.period(), [this] {
            if (stream_.stopped())
            {
                fail(kOk, "stopped");
                return;
            }
            // A config that arrived while this wait was pending is already applied;
            // the timer was re-armed at the new period, so this tick is one sample.
            stream_.tick(std::chrono::steady_clock::now());
        });
    }

    void on_crl(const msg::Crl& msg)
    {
        try
        {
            const auto manager = read_file(opts_.manager_cert);
            const auto signature = pki::from_hex(msg.signature);
            const auto bytes = std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(msg.payload.data()),
                                                             msg.payload.size());
            if (!pki::CertVerifier::verify_signature(manager, bytes, signature))
            {
                log::warn(tag(), "manager CRL signature does not match the manager certificate");
                return;
            }
            const auto serials = pki::manager_crl_serials(msg.payload);
            if (!own_serial_.empty() && serials.contains(own_serial_))
                fail(kFailed, "certificate is on the manager CRL");
        }
        catch (const std::exception& e)
        {
            log::warn(tag(), std::string("manager CRL: ") + e.what());
        }
    }

    void on_bye(const msg::Bye& msg)
    {
        if (phase_ == Phase::Done || !mine(msg.device)) return;
        if (phase_ == Phase::Run) stream_.on_bye(msg);
        fail(kOk, "controller refused: " + msg.reason);
    }

    void on_rejected(const msg::Rejected& msg)
    {
        if (phase_ != Phase::Cert || !mine(msg.device)) return;
        try
        {
            fs::remove(opts_.dir / "key.next.pem");
            throw Refused(kRejected, std::format("certificate refused: {} {}", msg.code, msg.detail));
        }
        catch (const Refused& e)
        {
            fail(e.code, e.what());
        }
    }

    void on_certificate(const msg::Certificate& msg)
    {
        if (phase_ != Phase::Cert || !mine(msg.device)) return;
        try
        {
            store_certificate(msg.cert);
            phase_ = Phase::Challenge;
        }
        catch (const std::exception& e)
        {
            fail(kFailed, e.what());
        }
    }

    void on_challenge(const msg::Challenge& msg)
    {
        if ((phase_ != Phase::Announce && phase_ != Phase::Challenge) || !mine(msg.device)) return;
        try
        {
            prove(msg);
        }
        catch (const Refused& e)
        {
            fail(e.code, e.what());
        }
        catch (const std::exception& e)
        {
            fail(kFailed, e.what());
        }
    }

    void publish_applied(const msg::Config& msg, bool ok, std::string code, std::string detail)
    {
        msg::ConfigApplied out;
        out.device = opts_.name;
        out.request_id = msg.request_id;
        out.ok = ok;
        out.code = std::move(code);
        out.detail = std::move(detail);
        applied_->publish(out);
    }

    void on_config(const msg::Config& msg)
    {
        if ((phase_ != Phase::Config && phase_ != Phase::Run) || !mine(msg.device)) return;
        if (const auto bad = check_config(static_cast<int>(msg.rate_ms), msg.amplitude))
        {
            log::warn(tag(), std::format("config refused: {} ({})", bad->code, bad->detail));
            publish_applied(msg, false, bad->code, bad->detail);
            return;
        }
        const bool first = phase_ == Phase::Config;
        stream_.on_config(msg);
        publish_applied(msg, true, {}, {});
        if (first)
        {
            phase_ = Phase::Run;
            log::info(tag(), std::format("online (device_id {}, rate_ms {}, amplitude {}, active {})",
                                         stream_.device_id(), stream_.rate_ms(), stream_.amplitude(), stream_.active()));
            // The first sample follows at once: the previous wait was the handshake.
            stream_.tick(std::chrono::steady_clock::now());
            arm_samples();
            return;
        }
        // Cancel the pending wait so the new rate applies now, not after the old one.
        stream_.tick(std::chrono::steady_clock::now());
        arm_samples();
    }

    Options opts_;
    int& exit_code_;
    SampleStream stream_;
    std::string own_serial_;
    Phase phase_{Phase::Announce};
    bool csr_sent_{false};
    std::unique_ptr<pki::DeviceKey> next_key_;
    rclcpp::Publisher<msg::Hello>::SharedPtr hello_;
    rclcpp::Publisher<msg::Heartbeat>::SharedPtr heartbeat_;
    rclcpp::Publisher<msg::Error>::SharedPtr error_;
    rclcpp::Publisher<msg::Csr>::SharedPtr csr_;
    rclcpp::Publisher<msg::Proof>::SharedPtr proof_;
    rclcpp::Publisher<msg::ScalarSample>::SharedPtr scalar_;
    rclcpp::Publisher<msg::GpsSample>::SharedPtr gps_;
    rclcpp::Publisher<msg::XySample>::SharedPtr xy_;
    rclcpp::Publisher<msg::XyzSample>::SharedPtr xyz_;
    rclcpp::Publisher<msg::ConfigApplied>::SharedPtr applied_;
    rclcpp::Subscription<msg::Bye>::SharedPtr bye_;
    rclcpp::Subscription<msg::Rejected>::SharedPtr rejected_;
    rclcpp::Subscription<msg::Certificate>::SharedPtr certificate_;
    rclcpp::Subscription<msg::Challenge>::SharedPtr challenge_;
    rclcpp::Subscription<msg::Config>::SharedPtr config_;
    rclcpp::Subscription<msg::Crl>::SharedPtr crl_;
    rclcpp::TimerBase::SharedPtr hello_timer_;
    rclcpp::TimerBase::SharedPtr sample_timer_;
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
    try
    {
        fs::create_directories(opts.dir);
        ::chmod(opts.dir.c_str(), 0700);
        prepare_identity(opts);
        arm_security(opts);
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
        auto node = std::make_shared<DeviceNode>(std::move(opts), rc);
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
