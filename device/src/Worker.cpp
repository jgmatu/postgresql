#include "Worker.hpp"

#include "ConfigCheck.hpp"
#include "Files.hpp"

#include <pki/Bytes.hpp>
#include <pki/CertVerifier.hpp>
#include <pki/Certificate.hpp>

#include <log/Log.hpp>
#include <pgdemo/Topics.hpp>

#include <pgdemo_msgs/msg/heartbeat.hpp>

#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <format>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace pgdemo::device {

namespace msg = pgdemo_msgs::msg;
using namespace std::chrono_literals;

namespace {

/// The manager refused us: enrollment rejected, or bye during the handshake.
struct Refused : std::runtime_error
{
    Refused(int c, const std::string& what) : std::runtime_error(what), code(c) {}
    int code;
};

unsigned char mode_code(std::string_view mode)
{
    if (mode == "enroll") return msg::Hello::ENROLL;
    if (mode == "rotate") return msg::Hello::ROTATE;
    return msg::Hello::RUN;
}

std::unique_ptr<SampleGenerator> require_generator(std::unique_ptr<SampleGenerator> generator)
{
    if (!generator) throw std::invalid_argument("a worker needs a generator");
    return generator;
}

} // namespace

Worker::Worker(Options opts, int& exit_code, std::unique_ptr<SampleGenerator> generator)
    : rclcpp::Node(node_name_for(opts.name))
    , opts_(std::move(opts))
    , exit_code_(exit_code)
    , generator_(require_generator(std::move(generator)))
    , stream_([this](SampleStream::Emission emission) { publish_emission(std::move(emission)); }, *generator_,
              "device " + opts_.name)
{
    std::filesystem::create_directories(opts_.dir);
    ::chmod(opts_.dir.c_str(), 0700);
    const std::filesystem::path identity = std::filesystem::is_regular_file(factory_cert_path()) ? factory_cert_path()
                                                                                                  : cert_path();
    if (std::filesystem::is_regular_file(identity)) own_serial_ = pki::inspect_certificate(read_file(identity)).serial;

    const auto qos = reliable_qos();
    const auto config_qos = rclcpp::QoS(rclcpp::KeepLast(64)).reliable();
    const auto hello_qos = rclcpp::QoS(rclcpp::KeepLast(1)).reliable().transient_local();
    hello_ = create_publisher<msg::Hello>(topics::kHello, hello_qos);
    heartbeat_ = create_publisher<msg::Heartbeat>(topics::kHeartbeat, qos);
    error_ = create_publisher<msg::Error>(topics::kError, qos);
    csr_ = create_publisher<msg::Csr>(topics::kCsr, qos);
    proof_ = create_publisher<msg::Proof>(topics::kProof, qos);
    applied_ = create_publisher<msg::ConfigApplied>(topics::kConfigApplied, config_qos);

    bye_ = create_subscription<msg::Bye>(topics::kBye, qos, [this](msg::Bye::ConstSharedPtr m) { on_bye(*m); });
    rejected_ = create_subscription<msg::Rejected>(topics::kRejected, qos,
                                                   [this](msg::Rejected::ConstSharedPtr m) { on_rejected(*m); });
    certificate_ = create_subscription<msg::Certificate>(
        topics::kCertificate, qos, [this](msg::Certificate::ConstSharedPtr m) { on_certificate(*m); });
    challenge_ = create_subscription<msg::Challenge>(topics::kChallenge, qos,
                                                     [this](msg::Challenge::ConstSharedPtr m) { on_challenge(*m); });
    config_ = create_subscription<msg::Config>(topics::kConfig, config_qos,
                                               [this](msg::Config::ConstSharedPtr m) { on_config(*m); });
    if (opts_.manager_cert_ok)
        crl_ = create_subscription<msg::Crl>(topics::kCrl, qos, [this](msg::Crl::ConstSharedPtr m) { on_crl(*m); });

    if (opts_.mode != "run") phase_ = Phase::Cert;
    hello_timer_ = create_wall_timer(1s, [this] { announce(); });
    announce();
}

Worker::~Worker() = default;

void Worker::trace_sample(const std::string& line)
{
    const int rate = std::max(stream_.rate_ms(), 1);
    if (samples_since_trace_ != 0 && samples_since_trace_ * rate < 2000)
    {
        ++samples_since_trace_;
        return;
    }
    samples_since_trace_ = 1;
    log::info(tag(), line);
}

void Worker::publish_emission(SampleStream::Emission emission)
{
    std::visit(
        [this](auto&& item) {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, msg::Heartbeat>)
            {
                item.device = opts_.name;
                heartbeat_->publish(item);
            }
            else
                publish_sample(Sample{std::move(item)});
        },
        std::move(emission));
}

void Worker::publish_error(const std::string& code, const std::string& detail)
{
    msg::Error err;
    err.device = opts_.name;
    err.code = code;
    err.detail = detail;
    error_->publish(err);
}

void Worker::fail(int code, const std::string& what)
{
    if (phase_ == Phase::Done) return;
    phase_ = Phase::Done;
    exit_code_ = code;
    (code == kOk ? log::info : log::error)(tag(), what);
    hello_timer_.reset();
    sample_timer_.reset();
    rclcpp::shutdown();
}

void Worker::announce()
{
    if (phase_ != Phase::Announce && phase_ != Phase::Cert) return;
    msg::Hello hello;
    hello.device = opts_.name;
    hello.mode = mode_code(opts_.mode);
    hello.request_id = opts_.request_id;
    const std::filesystem::path presented = std::filesystem::is_regular_file(factory_cert_path()) ? factory_cert_path()
                                                                                                  : cert_path();
    if (std::filesystem::is_regular_file(presented)) hello.certificate = read_file(presented);
    hello_->publish(hello);
    if (!hello_traced_)
    {
        hello_traced_ = true;
        log::info(tag(), std::format("handshake: hello {} request {}", opts_.mode, opts_.request_id));
    }
    // The CSR topic is volatile. Repeat the same request until the
    // certificate arrives, the way hello is repeated, so a subscriber that
    // matches late still receives it.
    if (phase_ == Phase::Cert) send_csr();
}

void Worker::send_csr()
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
            log::info(tag(), std::format("handshake: CSR sent for request {}", opts_.request_id));
        }
    }
    catch (const std::exception& e)
    {
        fail(kFailed, e.what());
    }
}

void Worker::store_certificate(const std::string& cert)
{
    if (!next_key_) throw std::runtime_error("certificate arrived before the key was generated");
    const auto probe = pki::random_bytes(32);
    if (!pki::CertVerifier::verify_signature(cert, probe, next_key_->sign(probe)))
    {
        std::filesystem::remove(opts_.dir / "key.next.pem");
        throw std::runtime_error("issued certificate does not match the generated key");
    }
    write_file(opts_.dir / "cert.next.pem", cert);
    std::filesystem::rename(opts_.dir / "key.next.pem", key_path());
    std::filesystem::rename(opts_.dir / "cert.next.pem", cert_path());
    next_key_.reset();
    log::info(tag(), std::format("handshake: certificate stored (serial {})", pki::inspect_certificate(cert).serial));
}

void Worker::prove(const msg::Challenge& challenge)
{
    if (!std::filesystem::exists(key_path()) || !std::filesystem::exists(cert_path()))
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

void Worker::arm_samples()
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

void Worker::on_crl(const msg::Crl& message)
{
    try
    {
        const auto manager = read_file(opts_.manager_cert);
        const auto signature = pki::from_hex(message.signature);
        const auto bytes = std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(message.payload.data()),
                                                         message.payload.size());
        if (!pki::CertVerifier::verify_signature(manager, bytes, signature))
        {
            log::warn(tag(), "manager CRL signature does not match the manager certificate");
            return;
        }
        const auto serials = pki::manager_crl_serials(message.payload);
        if (!own_serial_.empty() && serials.contains(own_serial_))
            fail(kFailed, "certificate is on the manager CRL");
    }
    catch (const std::exception& e)
    {
        log::warn(tag(), std::string("manager CRL: ") + e.what());
    }
}

void Worker::on_bye(const msg::Bye& message)
{
    if (phase_ == Phase::Done || !mine(message.device)) return;
    if (phase_ == Phase::Run) stream_.on_bye(message);
    fail(kOk, "controller refused: " + message.reason);
}

void Worker::on_rejected(const msg::Rejected& message)
{
    if (phase_ != Phase::Cert || !mine(message.device)) return;
    try
    {
        std::filesystem::remove(opts_.dir / "key.next.pem");
        throw Refused(kRejected, std::format("certificate refused: {} {}", message.code, message.detail));
    }
    catch (const Refused& e)
    {
        fail(e.code, e.what());
    }
}

void Worker::on_certificate(const msg::Certificate& message)
{
    if (phase_ != Phase::Cert || !mine(message.device)) return;
    try
    {
        store_certificate(message.cert);
        phase_ = Phase::Challenge;
    }
    catch (const std::exception& e)
    {
        fail(kFailed, e.what());
    }
}

void Worker::on_challenge(const msg::Challenge& message)
{
    if ((phase_ != Phase::Announce && phase_ != Phase::Challenge) || !mine(message.device)) return;
    try
    {
        log::info(tag(), "handshake: challenge");
        prove(message);
        log::info(tag(), "handshake: proof sent");
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

void Worker::publish_applied(const msg::Config& message, bool ok, std::string code, std::string detail)
{
    msg::ConfigApplied out;
    out.device = opts_.name;
    out.request_id = message.request_id;
    out.ok = ok;
    out.code = std::move(code);
    out.detail = std::move(detail);
    applied_->publish(out);
}

void Worker::on_config(const msg::Config& message)
{
    if ((phase_ != Phase::Config && phase_ != Phase::Run) || !mine(message.device)) return;
    if (const auto bad = check_config(static_cast<int>(message.rate_ms), message.amplitude))
    {
        log::warn(tag(), std::format("config refused: {} ({})", bad->code, bad->detail));
        publish_applied(message, false, bad->code, bad->detail);
        return;
    }
    const bool first = phase_ == Phase::Config;
    stream_.on_config(message);
    samples_since_trace_ = 0;
    publish_applied(message, true, {}, {});
    if (first)
    {
        phase_ = Phase::Run;
        log::info(tag(), std::format("handshake: online (device_id {}, rate_ms {}, amplitude {}, active {})",
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

} // namespace pgdemo::device
