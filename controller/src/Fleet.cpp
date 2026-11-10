#include "Fleet.hpp"

#include "Settle.hpp"
#include "Spawn.hpp"

#include <pki/CertVerifier.hpp>
#include <pki/Certificate.hpp>

#include <log/Log.hpp>

#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/experimental/channel_error.hpp>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <climits>
#include <span>
#include <csignal>
#include <format>
#include <map>
#include <set>
#include <tuple>
#include <type_traits>

namespace pgdemo::controller {

namespace asio = boost::asio;
namespace fs = std::filesystem;
using namespace asio::experimental::awaitable_operators;
using namespace std::chrono_literals;
using asio::awaitable;
using asio::use_awaitable;
using Clock = std::chrono::steady_clock;

namespace {

constexpr std::string_view kLog = "fleet";

/// Handshake timeouts. The CSR waits longer: the device generates a key first.
constexpr auto kHelloTimeout = 5s;
constexpr auto kCsrTimeout = 30s;
constexpr auto kProofTimeout = 5s;
/// A device that sends nothing for this long (it heartbeats every 5 s) is killed.
constexpr auto kStuckAfter = 30s;
/// A process online for this long resets the restart backoff.
constexpr auto kStableAfter = 60s;
/// Shared telemetry queue. One lease writes at most this many samples.
constexpr std::size_t kTelemetryCapacity = 4096;
constexpr std::size_t kTelemetryBatch = 64;
constexpr auto kTelemetryFlush = 100ms;

std::uint8_t launch_mode(Launch l) noexcept
{
    switch (l)
    {
    case Launch::Enroll: return pgdemo_msgs::msg::Hello::ENROLL;
    case Launch::Rotate: return pgdemo_msgs::msg::Hello::ROTATE;
    case Launch::Run: break;
    }
    return pgdemo_msgs::msg::Hello::RUN;
}

std::string_view mode_name(std::uint8_t mode) noexcept
{
    if (mode == pgdemo_msgs::msg::Hello::ENROLL) return "enroll";
    if (mode == pgdemo_msgs::msg::Hello::ROTATE) return "rotate";
    return "run";
}

std::optional<std::string> field_string(const boost::json::object& o, std::string_view key)
{
    const auto* v = o.if_contains(key);
    if (v == nullptr || !v->is_string()) return std::nullopt;
    return std::string(v->as_string());
}

std::optional<double> field_number(const boost::json::object& o, std::string_view key)
{
    const auto* v = o.if_contains(key);
    if (v == nullptr || !v->is_number()) return std::nullopt;
    return v->to_number<double>();
}

std::string_view launch_name(Launch l) noexcept
{
    switch (l)
    {
    case Launch::Enroll: return "enroll";
    case Launch::Rotate: return "rotate";
    case Launch::Run: break;
    }
    return "run";
}

/// The device proved nothing, or the wrong thing. It stays down until its identity changes.
struct AuthFailure : std::runtime_error
{
    AuthFailure(std::string c, const std::string& detail) : std::runtime_error(detail), code(std::move(c)) {}
    std::string code;
};

/// The device may not run now (revoked, deleted, enrollment refused). `state`
/// is written to `device_session` when not empty.
struct Refusal : std::runtime_error
{
    Refusal(std::string s, const std::string& reason) : std::runtime_error(reason), state(std::move(s)) {}
    std::string state;
};

/// `op`, or an error after `timeout`. settle(): a failing `op` fails at once.
template <typename T>
awaitable<T> with_deadline(awaitable<T> op, Clock::duration timeout, std::string_view what)
{
    asio::steady_timer timer(co_await asio::this_coro::executor);
    timer.expires_after(timeout);
    auto result = co_await (settle(std::move(op)) || timer.async_wait(use_awaitable));
    if (result.index() == 1) throw std::runtime_error(std::format("timed out waiting for {}", what));
    auto [value, error] = std::get<0>(std::move(result));
    if (error) std::rethrow_exception(error);
    co_return std::move(*value);
}

/// Validation of a desired configuration. Pure: no side effects.
db::ConfigOutcome decide(const db::ConfigRequest& r)
{
    if (r.rate_ms < 50 || r.rate_ms > 10000)
        return db::ConfigOutcome::failure("INVALID_RATE", std::format("rate_ms {} outside [50, 10000]", r.rate_ms));
    if (!(r.amplitude > 0.0) || r.amplitude > 1000.0)
        return db::ConfigOutcome::failure("INVALID_AMPLITUDE",
                                          std::format("amplitude {} outside (0, 1000]", r.amplitude));
    return db::ConfigOutcome::success();
}

std::string describe_status(int status)
{
    if (WIFEXITED(status)) return std::format("exit {}", WEXITSTATUS(status));
    if (WIFSIGNALED(status)) return std::format("signal {}", WTERMSIG(status));
    return std::format("status {}", status);
}

} // namespace

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------

Session::Session(asio::any_io_executor ex, std::string name, Downlink downlink)
    : inbox(ex, 64)
    , decision(ex, 1)
    , last_frame(Clock::now())
    , name_(std::move(name))
    , downlink_(std::move(downlink))
{
}

void Session::send_bye(std::string reason)
{
    if (closed_ || !downlink_.bye) return;
    pgdemo_msgs::msg::Bye msg;
    msg.device = name_;
    msg.reason = std::move(reason);
    downlink_.bye(std::move(msg));
}

void Session::send_certificate(std::string pem)
{
    if (closed_ || !downlink_.certificate) return;
    pgdemo_msgs::msg::Certificate msg;
    msg.device = name_;
    msg.cert = std::move(pem);
    downlink_.certificate(std::move(msg));
}

void Session::send_rejected(std::string code, std::string detail)
{
    if (closed_ || !downlink_.rejected) return;
    pgdemo_msgs::msg::Rejected msg;
    msg.device = name_;
    msg.code = std::move(code);
    msg.detail = std::move(detail);
    downlink_.rejected(std::move(msg));
}

void Session::send_challenge(const std::array<unsigned char, 32>& nonce)
{
    if (closed_ || !downlink_.challenge) return;
    pgdemo_msgs::msg::Challenge msg;
    msg.device = name_;
    msg.nonce = nonce;
    downlink_.challenge(std::move(msg));
}

void Session::send_config(const db::AppliedConfig& config)
{
    if (closed_ || !downlink_.config) return;
    pgdemo_msgs::msg::Config msg;
    msg.device = name_;
    msg.device_id = config.device_id;
    msg.rate_ms = config.rate_ms;
    msg.amplitude = config.amplitude;
    msg.is_active = config.is_active;
    downlink_.config(std::move(msg));
}

void Session::post(Uplink msg)
{
    if (closed_) return;
    if (!inbox.try_send(boost::system::error_code{}, std::move(msg)))
        log::warn(kLog, "inbox full or closed; message dropped");
}

void Session::close()
{
    if (closed_) return;
    closed_ = true;
    inbox.close();
    decision.close();
}

// ---------------------------------------------------------------------------
// Fleet: set-up
// ---------------------------------------------------------------------------

Fleet::Fleet(asio::io_context& io, asio::thread_pool& pool, db::DatabaseManager& db, FleetOptions opts,
             Downlink downlink)
    : io_(io)
    , pool_(pool)
    , writes_(asio::make_strand(pool.get_executor()))
    , db_(db)
    , opts_(std::move(opts))
    , downlink_(std::move(downlink))
    , telemetry_timer_(io)
    , sigchld_(io, SIGCHLD)
    , stop_signals_(io, SIGINT, SIGTERM)
{
}

Fleet::~Fleet()
{
    if (lock_fd_ >= 0) ::close(lock_fd_);
}

void Fleet::acquire_lock()
{
    fs::create_directories(opts_.devices_dir);
    fs::permissions(opts_.devices_dir, fs::perms::owner_all, fs::perm_options::replace);
    const fs::path path = opts_.devices_dir / ".controller.lock";
    lock_fd_ = ::open(path.c_str(), O_CREAT | O_RDWR, 0600);
    if (lock_fd_ < 0) throw std::system_error(errno, std::generic_category(), "open " + path.string());
    // fcntl locks are per process and are not inherited across fork, so the
    // child's close_range does not release this one.
    struct flock lock{};
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    if (::fcntl(lock_fd_, F_SETLK, &lock) != 0)
        throw std::runtime_error("another controller holds " + path.string());
    log::info(kLog, std::format("manager topics pgdemo/manager, worker topics pgdemo/worker (max {} device process(es), binary {})",
                                opts_.max_devices, opts_.device_bin));
}

void Fleet::start()
{
    spawn(reap_loop(), "reaper");
    spawn(signal_loop(), "signals");
    spawn(boot(), "boot");
    spawn(heartbeat_loop(), "heartbeat");
    spawn(maintenance_loop(), "maintenance");
    spawn(telemetry_loop(), "telemetry");
}

void Fleet::spawn(awaitable<void> task, std::string what)
{
    asio::co_spawn(io_, std::move(task), [what = std::move(what)](std::exception_ptr ep) {
        if (!ep) return;
        try
        {
            std::rethrow_exception(ep);
        }
        catch (const std::exception& e)
        {
            log::error(kLog, what + ": " + e.what());
        }
    });
}

void Fleet::db_write(std::string what, std::function<void()> fn)
{
    asio::post(writes_, [what = std::move(what), fn = std::move(fn)] {
        try
        {
            fn();
        }
        catch (const std::exception& e)
        {
            log::warn(kLog, what + ": " + e.what());
        }
    });
}

void Fleet::notify(boost::json::object msg)
{
    asio::post(io_, [this, msg = std::move(msg)] {
        if (shutting_down_) return;
        const auto channel = field_string(msg, "channel").value_or("");
        const auto* p = msg.if_contains("payload");
        if (p == nullptr || !p->is_object()) return;
        const auto& o = p->as_object();
        const auto id = field_number(o, "request_id");

        if (channel == db::channel::kEnrollmentRequested && id)
            spawn(on_enrollment_requested(static_cast<std::int64_t>(*id)), "enrollment_requested");
        else if (channel == db::channel::kEnrollmentEvents && id)
            spawn(on_enrollment_decided(static_cast<std::int64_t>(*id)), "enrollment_events");
        else if (channel == db::channel::kDeviceEvents)
            spawn(on_device_event(field_string(o, "action").value_or(""), field_string(o, "device").value_or(""),
                                  static_cast<int>(field_number(o, "device_id").value_or(0))),
                  "device_events");
        else if (channel == db::channel::kConfigRequested)
            spawn(drain(), "config_requested");
    });
}

void Fleet::on_hello(pgdemo_msgs::msg::Hello hello)
{
    asio::post(io_, [this, hello = std::move(hello)]() mutable {
        if (shutting_down_) return;
        admit(std::move(hello));
    });
}

void Fleet::on_uplink(Uplink msg)
{
    asio::post(io_, [this, msg = std::move(msg)]() mutable {
        if (shutting_down_) return;
        const auto name = device_of(msg);
        if (name.empty()) return;
        if (Slot* s = find(name); s != nullptr && s->session) s->session->post(std::move(msg));
        else early_[name] = std::move(msg);
    });
}

void Fleet::enqueue_telemetry(TelemetryItem sample)
{
    if (telemetry_.size() >= kTelemetryCapacity)
    {
        ++telemetry_dropped_;
        return;
    }
    telemetry_.push_back(std::move(sample));
    if (telemetry_.size() >= kTelemetryBatch) telemetry_timer_.cancel();
}

void Fleet::send_bye(const std::string& device, const std::string& reason)
{
    if (!downlink_.bye) return;
    pgdemo_msgs::msg::Bye msg;
    msg.device = device;
    msg.reason = reason;
    downlink_.bye(std::move(msg));
}

// ---------------------------------------------------------------------------
// Coroutines
// ---------------------------------------------------------------------------

awaitable<void> Fleet::boot()
{
    co_await on_db([this] { db_.reset_sessions(); });
    co_await sweep();
    co_await drain();
    log::info(kLog, std::format("supervising {} device(s), {} process(es) started (max {}); Ctrl-C to stop",
                                slots_.size(), running(), opts_.max_devices));
}

awaitable<void> Fleet::reap_loop()
{
    for (;;)
    {
        auto [ec, sig] = co_await sigchld_.async_wait(asio::as_tuple(use_awaitable));
        if (ec) co_return;
        for (;;)
        {
            int status = 0;
            const pid_t pid = ::waitpid(-1, &status, WNOHANG);
            if (pid <= 0) break;
            on_exit(pid, status);
        }
    }
}

awaitable<void> Fleet::signal_loop()
{
    auto [ec, sig] = co_await stop_signals_.async_wait(asio::as_tuple(use_awaitable));
    if (ec) co_return;
    log::info(kLog, std::format("signal {} received, shutting down", sig));
    co_await shutdown();
}

awaitable<void> Fleet::heartbeat_loop()
{
    asio::steady_timer timer(io_);
    for (;;)
    {
        timer.expires_after(10s);
        co_await timer.async_wait(use_awaitable);
        if (shutting_down_) co_return;

        std::vector<int> seen;
        const auto now = Clock::now();
        for (auto& [name, s] : slots_)
        {
            if (!s.session || !s.session->online || !s.device) continue;
            if (now - s.session->last_frame > kStuckAfter)
            {
                log::warn(kLog, std::format("{}: no frames for {} s", name,
                                            std::chrono::duration_cast<std::chrono::seconds>(kStuckAfter).count()));
                // A child we forked is killed and reaped. A node started elsewhere is only released.
                if (s.pid != 0) ::kill(s.pid, SIGKILL);
                else stop_process(s, "no frames");
                continue;
            }
            seen.push_back(s.device->device_id);
        }
        if (!seen.empty()) db_write("last_seen", [this, seen] { db_.touch_sessions(seen); });
    }
}

awaitable<void> Fleet::maintenance_loop()
{
    asio::steady_timer timer(io_);
    auto next_partitions = Clock::now() + opts_.maintenance_interval;
    for (;;)
    {
        timer.expires_after(30s);
        co_await timer.async_wait(use_awaitable);
        if (shutting_down_) co_return;
        if (!db_.listener_alive())
        {
            log::error(kLog, "listener connection lost; exiting so a supervisor can restart us");
            exit_code_ = 1;
            co_await shutdown();
            co_return;
        }
        try
        {
            const auto expired = co_await on_db([this] { return db_.expire_enrollments(opts_.enrollment_timeout); });
            for (const auto id : expired) log::warn(kLog, std::format("enrollment {} expired", id));
            co_await sweep();
            if (Clock::now() >= next_partitions)
            {
                next_partitions = Clock::now() + opts_.maintenance_interval;
                const auto created = co_await on_db([this] {
                    std::vector<std::string> all;
                    for (const auto p : db::kAllPayloads)
                        for (auto& n : db_.ensure_history_partitions(p)) all.push_back(std::move(n));
                    return all;
                });
                for (const auto& n : created) log::info(kLog, "created partition " + n);
            }
        }
        catch (const std::exception& e)
        {
            log::error(kLog, std::string("maintenance: ") + e.what());
        }
    }
}

awaitable<void> Fleet::shutdown()
{
    if (shutting_down_) co_return;
    shutting_down_ = true;

    for (auto& [name, s] : slots_)
        if (s.pid != 0 || s.session) stop_process(s, "controller stopping");

    asio::steady_timer timer(io_);
    const auto deadline = Clock::now() + 5s;
    while (!pids_.empty() && Clock::now() < deadline)
    {
        timer.expires_after(100ms);
        co_await timer.async_wait(use_awaitable);
    }
    for (const auto& [pid, name] : pids_)
    {
        log::warn(kLog, std::format("{}: process {} ignored SIGTERM; killing it", name, pid));
        ::kill(pid, SIGKILL);
        ::waitpid(pid, nullptr, 0);
    }
    pids_.clear();
    for (auto& [name, s] : slots_)
    {
        s.pid = 0;
        if (s.session) s.session->close();
    }
    telemetry_timer_.cancel();
    co_await flush_telemetry();
    if (lock_fd_ >= 0)
    {
        ::close(lock_fd_);
        lock_fd_ = -1;
    }

    // On the write strand, so it lands after every queued state change.
    try
    {
        co_await asio::co_spawn(
            writes_, [this]() -> awaitable<void> { db_.reset_sessions(); co_return; }, use_awaitable);
    }
    catch (const std::exception& e)
    {
        log::warn(kLog, std::string("reset sessions: ") + e.what());
    }
    sigchld_.cancel();
    stop_signals_.cancel();
    log::info(kLog, "every device process has stopped");
    io_.stop();
}

// ---------------------------------------------------------------------------
// Device sessions
// ---------------------------------------------------------------------------

awaitable<Uplink> Fleet::receive(std::shared_ptr<Session> session, Clock::duration timeout, std::string_view what)
{
    co_return co_await with_deadline(session->inbox.async_receive(use_awaitable), timeout, what);
}

awaitable<void> Fleet::telemetry_loop()
{
    for (;;)
    {
        if (telemetry_.size() < kTelemetryBatch)
        {
            telemetry_timer_.expires_after(kTelemetryFlush);
            co_await telemetry_timer_.async_wait(asio::as_tuple(use_awaitable));
        }
        if (shutting_down_) co_return;
        co_await flush_telemetry();
    }
}

awaitable<void> Fleet::flush_telemetry()
{
    if (telemetry_.empty() && telemetry_dropped_ == 0) co_return;
    std::vector<db::DummySample> scalar;
    std::vector<db::TrackSample> gps;
    std::vector<db::XySample> xy;
    while (!telemetry_.empty())
    {
        auto item = std::move(telemetry_.front());
        telemetry_.pop_front();
        std::visit(
            [&](auto&& sample) {
                using T = std::decay_t<decltype(sample)>;
                if constexpr (std::is_same_v<T, db::DummySample>) scalar.push_back(sample);
                else if constexpr (std::is_same_v<T, db::TrackSample>) gps.push_back(sample);
                else xy.push_back(sample);
            },
            std::move(item));
    }
    const auto dropped = telemetry_dropped_;
    telemetry_dropped_ = 0;
    if (dropped != 0)
        log::warn(kLog, std::format("telemetry queue full; dropped {} sample(s)", dropped));
    if (scalar.empty() && gps.empty() && xy.empty()) co_return;
    try
    {
        co_await on_db([this, scalar = std::move(scalar), gps = std::move(gps), xy = std::move(xy)] {
            db_.write_telemetry_batch(scalar, gps, xy);
        });
    }
    catch (const std::exception& e)
    {
        log::warn(kLog, std::string("telemetry batch not stored: ") + e.what());
    }
}

awaitable<void> Fleet::device_session(std::shared_ptr<Session> session, std::string name,
                                       pgdemo_msgs::msg::Hello hello)
{
    std::optional<AuthFailure> auth_failure;
    std::optional<Refusal> refusal;
    std::string error;
    const pid_t peer = find(name) != nullptr ? find(name)->pid : 0;

    try
    {
        Slot* s = find(name);
        if (s == nullptr || s->session != session) throw std::runtime_error("process is no longer supervised");
        if (hello.device != name)
            throw AuthFailure("BAD_HELLO", "the node did not introduce itself as " + name);
        if (hello.mode != launch_mode(s->running))
            throw AuthFailure("BAD_HELLO", std::format("the node was admitted to {}", launch_name(s->running)));
        (void)hello;

        if (s->running != Launch::Run) co_await obtain_certificate(session, name);
        co_await authenticate(session, name);
        co_await serve(session, name);
    }
    catch (const AuthFailure& e)
    {
        auth_failure = e;
    }
    catch (const Refusal& e)
    {
        refusal = e;
    }
    catch (const boost::system::system_error& e)
    {
        if (e.code() != asio::error::eof && e.code() != asio::error::operation_aborted &&
            e.code() != asio::error::connection_reset && e.code() != asio::experimental::error::channel_closed)
            error = e.what();
    }
    catch (const std::exception& e)
    {
        error = e.what();
    }

    std::optional<std::string> bye;
    if (Slot* s = find(name); auth_failure)
    {
        log::warn(kLog, std::format("{}: authentication failed: {} ({})", name, auth_failure->code, auth_failure->what()));
        if (s != nullptr && s->session == session)
        {
            s->wanted = false;
            set_state(*s, "AUTH_FAILED", auth_failure->code + ": " + auth_failure->what());
        }
        bye = "authentication failed: " + auth_failure->code;
    }
    else if (refusal)
    {
        log::info(kLog, std::format("{}: {}", name, refusal->what()));
        if (s != nullptr && s->session == session && !refusal->state.empty())
            set_state(*s, refusal->state, refusal->what());
        bye = refusal->what();
    }
    else if (!error.empty())
    {
        log::warn(kLog, std::format("{}: session ended: {}", name, error));
    }
    if (bye) session->send_bye(*bye);

    if (Slot* s = find(name); s != nullptr && s->session == session)
    {
        s->session.reset();
        if (s->state == "ONLINE" || s->state == "STARTING") set_state(*s, "OFFLINE");
        // A child without its session has nothing to do. A remote node got bye above.
        if (s->pid != 0 && s->pid == peer) ::kill(peer, SIGTERM);
    }
    session->close();
}

awaitable<void> Fleet::obtain_certificate(std::shared_ptr<Session> session, std::string name)
{
    const auto msg = co_await receive(session, kCsrTimeout, "the CSR");
    Slot* s = find(name);
    if (s == nullptr) throw Refusal("", "device removed during enrollment");
    const std::int64_t request = s->running_request;
    if (const auto* err = std::get_if<pgdemo_msgs::msg::Error>(&msg))
        throw std::runtime_error(std::format("expected csr, got error {} {}", err->code, err->detail));
    const auto* csr_msg = std::get_if<pgdemo_msgs::msg::Csr>(&msg);
    if (csr_msg == nullptr) throw std::runtime_error("expected csr");
    const auto csr = csr_msg->csr;

    const bool submitted = co_await on_db([this, request, csr] { return db_.submit_csr(request, csr); });
    if (!submitted) throw Refusal("", std::format("request {} is no longer pending", request));
    log::info(kLog, std::format("{}: CSR of request {} submitted to the CA", name, request));

    const auto outcome = co_await with_deadline(session->decision.async_receive(use_awaitable),
                                                opts_.enrollment_timeout, "the certificate");
    s = find(name);
    if (s == nullptr) throw Refusal("", "device removed during enrollment");
    s->running_request = 0;
    if (s->next_request == request)
    {
        s->next = Launch::Run;
        s->next_request = 0;
    }

    if (outcome.status != "ISSUED" || !outcome.cert_pem)
    {
        session->send_rejected(outcome.error_code, outcome.error_detail);
        throw Refusal("", std::format("request {} {}: {} {}", request, outcome.status, outcome.error_code,
                                      outcome.error_detail));
    }
    if (outcome.device_id)
    {
        const int id = *outcome.device_id;
        const auto device = co_await on_db([this, id] { return db_.load_device(id); });
        if (device) apply_device(*device);
    }
    session->send_certificate(*outcome.cert_pem);
    // From here on it is an ordinary device process: a revocation stops it.
    if (s = find(name); s != nullptr) s->running = Launch::Run;
    log::info(kLog, std::format("{}: certificate of request {} delivered", name, request));
}

awaitable<void> Fleet::authenticate(std::shared_ptr<Session> session, std::string name)
{
    const auto raw_nonce = pki::random_bytes(32);
    if (raw_nonce.size() != 32) throw AuthFailure("BAD_PROOF", "nonce is not 32 bytes");
    std::array<unsigned char, 32> nonce{};
    std::copy(raw_nonce.begin(), raw_nonce.end(), nonce.begin());
    session->send_challenge(nonce);
    const auto msg = co_await receive(session, kProofTimeout, "the proof");

    if (const auto* err = std::get_if<pgdemo_msgs::msg::Error>(&msg))
        throw AuthFailure(err->code.empty() ? "DEVICE_ERROR" : err->code,
                          err->detail.empty() ? "device reported an error" : err->detail);
    const auto* proof = std::get_if<pgdemo_msgs::msg::Proof>(&msg);
    if (proof == nullptr) throw AuthFailure("BAD_PROOF", "expected proof");
    const auto& cert = proof->cert;
    const std::span<const std::uint8_t> signature(proof->signature.data(), proof->signature.size());

    Slot* s = find(name);
    if (s == nullptr || !s->device) throw Refusal("", "device is not registered");
    const int id = s->device->device_id;
    // Fresh from the database: a revocation committed a moment ago counts.
    const auto loaded = co_await on_db([this, id] {
        return std::tuple(db_.load_device(id), db_.load_root_pems(), db_.load_revoked_serials());
    });
    const auto& [device, roots, revoked] = loaded;
    s = find(name);
    if (s == nullptr || !device) throw Refusal("", "device deleted");
    s->device = *device;
    if (!device->active()) throw Refusal("DISABLED", "identity " + device->identity);

    pki::CertInfo info;
    try
    {
        info = pki::CertVerifier(roots).verify(cert, name, revoked);
    }
    catch (const pki::PkiError& e)
    {
        throw AuthFailure(e.code, e.what());
    }
    if (info.fingerprint != device->fingerprint)
        throw AuthFailure("CERT_NOT_CURRENT",
                          std::format("certificate {} is not the current one ({})", info.serial, device->serial));
    if (!pki::CertVerifier::verify_signature(cert, pki::proof_message(nonce, name, info.fingerprint), signature))
        throw AuthFailure("BAD_PROOF", "the signature does not verify with the certificate's key");
    log::info(kLog, std::format("{}: authenticated (serial {}, pid {})", name, info.serial, s->pid));
}

awaitable<void> Fleet::serve(std::shared_ptr<Session> session, std::string name)
{
    Slot* s = find(name);
    if (s == nullptr || !s->device) throw Refusal("", "device deleted");
    const db::Device device = *s->device;
    // Online first: a configuration committed from here on is published after this one.
    session->online = true;
    const int id = device.device_id;
    const auto config = co_await on_db([this, id] { return db_.init_device_config(id); });
    if (!config) throw Refusal("", "device deleted");
    session->send_config(*config);

    s = find(name);
    if (s == nullptr) throw Refusal("", "device deleted");
    session->last_frame = Clock::now();
    s->online_since = Clock::now();
    set_state(*s, "ONLINE");
    co_await read_samples(session, device);
}

awaitable<void> Fleet::read_samples(std::shared_ptr<Session> session, db::Device device)
{
    for (;;)
    {
        Uplink msg;
        try
        {
            msg = co_await session->inbox.async_receive(use_awaitable);
        }
        catch (const boost::system::system_error& e)
        {
            if (e.code() == asio::experimental::error::channel_closed) co_return;
            throw;
        }
        session->last_frame = Clock::now();
        std::visit(
            [&](const auto& m) {
                using T = std::decay_t<decltype(m)>;
                if constexpr (std::is_same_v<T, pgdemo_msgs::msg::Heartbeat>)
                    return;
                else if constexpr (std::is_same_v<T, pgdemo_msgs::msg::ScalarSample>)
                {
                    if (device.payload != db::Payload::Scalar)
                    {
                        log::warn(kLog, std::format("{}: ignoring scalar sample", device.name));
                        return;
                    }
                    enqueue_telemetry(db::DummySample{device.device_id, m.value, m.seq});
                }
                else if constexpr (std::is_same_v<T, pgdemo_msgs::msg::GpsSample>)
                {
                    if (device.payload != db::Payload::Gps)
                    {
                        log::warn(kLog, std::format("{}: ignoring GPS sample", device.name));
                        return;
                    }
                    enqueue_telemetry(db::TrackSample{device.device_id, m.origin_lon, m.origin_lat, m.radius_m,
                                                      m.bearing_rad, m.seq});
                }
                else if constexpr (std::is_same_v<T, pgdemo_msgs::msg::XySample>)
                {
                    if (device.payload != db::Payload::Xy)
                    {
                        log::warn(kLog, std::format("{}: ignoring xy sample", device.name));
                        return;
                    }
                    enqueue_telemetry(db::XySample{device.device_id, m.x, m.y, m.seq});
                }
                else
                    log::warn(kLog, std::format("{}: ignoring message while online", device.name));
            },
            msg);
    }
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

awaitable<void> Fleet::on_enrollment_requested(std::int64_t request_id)
{
    const auto e = co_await on_db([this, request_id] { return db_.load_enrollment(request_id); });
    if (!e || e->status != "PENDING") co_return;
    apply_enrollment(*e);
}

awaitable<void> Fleet::on_enrollment_decided(std::int64_t request_id)
{
    auto waiting = [&]() -> Slot* {
        for (auto& [_, s] : slots_)
            if (s.running_request == request_id || s.next_request == request_id) return &s;
        return nullptr;
    };
    if (waiting() == nullptr) co_return;
    const auto outcome = co_await on_db([this, request_id] { return db_.enrollment_outcome(request_id); });
    Slot* s = waiting();
    if (s == nullptr || !outcome || outcome->status == "PENDING" || outcome->status == "CSR_SUBMITTED") co_return;

    if (s->running_request == request_id && s->session)
    {
        s->session->decision.try_send(boost::system::error_code{}, *outcome);
        co_return;
    }
    // Decided before its process got that far: expired, or refused by the server.
    if (outcome->status == "ISSUED") co_return;
    log::info(kLog, std::format("{}: request {} {} ({})", s->name, request_id, outcome->status, outcome->error_code));
    if (s->next_request == request_id)
    {
        s->next = Launch::Run;
        s->next_request = 0;
    }
    if (s->running_request == request_id)
    {
        s->running_request = 0;
        stop_process(*s, "enrollment " + outcome->status);
    }
    else if (!s->device && s->pid == 0)
    {
        forget(*s);
    }
}

awaitable<void> Fleet::on_device_event(std::string action, std::string name, int device_id)
{
    if (action == "DELETE")
    {
        Slot* s = find(name);
        if (s == nullptr) co_return;
        log::info(kLog, std::format("{}: device deleted", name));
        s->removed = true;
        s->wanted = false;
        if (s->pid != 0 || s->session) stop_process(*s, "device deleted");
        else
        {
            forget(*s);
            reconcile_all();
        }
        co_return;
    }
    if (action != "INSERT" && action != "ROTATED" && action != "REVOKED" && action != "UNREVOKED") co_return;

    if (device_id == 0)
        if (const Slot* s = find(name); s != nullptr && s->device) device_id = s->device->device_id;
    if (device_id == 0) co_return;
    const auto device = co_await on_db([this, device_id] { return db_.load_device(device_id); });
    if (device) apply_device(*device);
}

awaitable<void> Fleet::drain()
{
    std::vector<db::AppliedConfig> applied;
    const auto stats = co_await on_db([this, &applied] {
        return db_.drain_pending(
            decide, [&applied](const db::AppliedConfig& c) { applied.push_back(c); }, opts_.pending_max_age);
    });
    for (const auto& c : applied)
    {
        Slot* s = find_by_id(c.device_id);
        if (s != nullptr && s->session && s->session->online) s->session->send_config(c);
    }
    if (stats.applied + stats.rejected + stats.expired > 0)
        log::info(kLog, std::format("queue drained: {} applied, {} rejected, {} expired", stats.applied,
                                    stats.rejected, stats.expired));
}

awaitable<void> Fleet::sweep()
{
    const auto devices = co_await on_db([this] { return db_.load_devices(); });
    std::set<std::string> present;
    for (const auto& d : devices)
    {
        present.insert(d.name);
        apply_device(d);
    }
    std::vector<std::string> gone;
    for (const auto& [name, s] : slots_)
        if (s.device && !present.contains(name)) gone.push_back(name);
    for (const auto& name : gone) co_await on_device_event("DELETE", name, 0);

    const auto pending = co_await on_db([this] { return db_.pending_enrollments(); });
    for (const auto& e : pending) apply_enrollment(e);
    reconcile_all();
}

void Fleet::apply_enrollment(const db::Enrollment& e)
{
    Slot* s = find(e.name);
    if (s != nullptr && (s->next_request == e.request_id || s->running_request == e.request_id)) return;

    if (e.kind == "ENROLL")
    {
        if (s != nullptr && s->device)
        {
            fail_enrollment(e.request_id, "EXISTS", "device " + e.name + " already exists");
            return;
        }
        if (s == nullptr)
        {
            s = &slots_[e.name];
            s->name = e.name;
        }
        s->type_id = e.type_id;
        s->payload = e.payload;
        s->next = Launch::Enroll;
    }
    else
    {
        if (s == nullptr || !s->device)
        {
            fail_enrollment(e.request_id, "NOT_MANAGED", "device " + e.name + " is not known to the controller");
            return;
        }
        s->next = Launch::Rotate;
    }
    s->next_request = e.request_id;
    s->wanted = true;
    s->failures = 0;
    log::info(kLog, std::format("{}: {} request {} from {}", e.name, e.kind, e.request_id, e.requested_by));

    if (s->pid != 0)
    {
        stop_process(*s, e.kind == "ENROLL" ? "re-enrolling" : "rotating key");
        return;
    }
    s->restart_scheduled = false;
    reconcile(*s);
}

void Fleet::apply_device(const db::Device& d)
{
    auto [it, inserted] = slots_.try_emplace(d.name);
    Slot& s = it->second;
    s.name = d.name;
    const bool changed = !s.device || s.device->fingerprint != d.fingerprint || s.device->identity != d.identity;
    s.device = d;
    s.type_id = d.type_id;
    s.payload = d.payload;
    if (!changed) return;

    if (!d.active())
    {
        if ((s.pid != 0 || s.session) && s.running == Launch::Run) stop_process(s, "identity " + d.identity);
        set_state(s, "DISABLED", "identity " + d.identity);
        return;
    }
    // ACTIVE again, or with a new certificate: give it another chance.
    if (s.state == "DISABLED" || s.state == "AUTH_FAILED" || !s.wanted)
    {
        s.wanted = true;
        s.failures = 0;
    }
    if (s.pid == 0) reconcile(s);
}

// ---------------------------------------------------------------------------
// Process table
// ---------------------------------------------------------------------------

void Fleet::admit(pgdemo_msgs::msg::Hello hello)
{
    const std::string name = hello.device;
    const auto mode = hello.mode;
    if (name.empty()) return;
    Slot* s = find(name);
    if (s == nullptr)
    {
        pending_hellos_[name] = std::move(hello);
        return;
    }
    if (s->session)
    {
        // The node repeats hello until the challenge. A new hello once it is online is another process.
        if (!s->session->online && mode == launch_mode(s->running)) return;
        s->session->send_bye("already connected");
        return;
    }
    if (shutting_down_ || s->removed || !s->wanted)
    {
        send_bye(name, "not managed");
        return;
    }
    const Launch expect = s->pid != 0 ? s->running : s->next;
    if (mode != launch_mode(expect))
    {
        send_bye(name, "unexpected mode");
        return;
    }
    if (s->pid == 0 && running() >= opts_.max_devices)
    {
        const std::string why = std::format("--max-devices {} reached", opts_.max_devices);
        if (s->next != Launch::Run)
        {
            fail_enrollment(s->next_request, "CAPACITY", why);
            s->next = Launch::Run;
            s->next_request = 0;
            if (!s->device)
            {
                send_bye(name, why);
                forget(*s);
                return;
            }
        }
        set_state(*s, "UNMANAGED", why);
        send_bye(name, why);
        return;
    }

    s->session = std::make_shared<Session>(io_.get_executor(), name, downlink_);
    if (s->pid == 0)
    {
        s->running = s->next;
        s->running_request = s->next_request;
    }
    if (auto it = early_.find(name); it != early_.end())
    {
        s->session->post(std::move(it->second));
        early_.erase(it);
    }
    log::info(kLog, std::format("{}: hello ({})", name, mode_name(mode)));
    spawn(device_session(s->session, name, std::move(hello)), "session " + name);
}

void Fleet::reconcile(Slot& slot)
{
    if (shutting_down_ || slot.removed || slot.restart_scheduled || !slot.wanted) return;
    const std::string name = slot.name;
    if (!slot.session)
    {
        if (auto it = pending_hellos_.find(name); it != pending_hellos_.end())
        {
            auto hello = std::move(it->second);
            pending_hellos_.erase(it);
            admit(std::move(hello)); // may erase the slot
        }
    }
    Slot* s = find(name);
    if (s == nullptr || s->pid != 0 || s->session) return;
    if (s->next == Launch::Run)
    {
        if (!s->device) return;
        if (!s->device->active())
        {
            set_state(*s, "DISABLED", "identity " + s->device->identity);
            return;
        }
    }
    if (running() >= opts_.max_devices)
    {
        const std::string why = std::format("--max-devices {} reached", opts_.max_devices);
        if (s->next != Launch::Run)
        {
            fail_enrollment(s->next_request, "CAPACITY", why);
            s->next = Launch::Run;
            s->next_request = 0;
            if (!s->device)
            {
                forget(*s);
                return;
            }
        }
        set_state(*s, "UNMANAGED", why);
        return;
    }
    start_process(*s);
}

void Fleet::reconcile_all()
{
    std::vector<std::pair<int, std::string>> order;
    for (const auto& [name, s] : slots_) order.emplace_back(s.device ? s.device->device_id : INT_MAX, name);
    std::ranges::sort(order);
    for (const auto& [_, name] : order)
        if (Slot* s = find(name)) reconcile(*s);
}

void Fleet::start_process(Slot& s)
{
    const fs::path dir = opts_.devices_dir / s.name;
    std::vector<std::string> argv{opts_.device_bin,
                                  "--name",
                                  s.name,
                                  "--type",
                                  s.type_id,
                                  "--payload",
                                  std::string(db::payload_name(s.payload)),
                                  "--dir",
                                  dir.string()};
    if (s.next != Launch::Run)
    {
        argv.push_back(s.next == Launch::Enroll ? "--enroll" : "--rotate");
        argv.push_back(std::to_string(s.next_request));
    }
    pid_t pid = 0;
    try
    {
        fs::create_directories(dir);
        fs::permissions(dir, fs::perms::owner_all, fs::perm_options::replace);
        pid = spawn_process(argv);
    }
    catch (const std::exception& e)
    {
        log::error(kLog, std::format("{}: cannot start {}: {}", s.name, opts_.device_bin, e.what()));
        set_state(s, "OFFLINE", e.what());
        schedule_restart(s, 30s);
        return;
    }
    pids_[pid] = s.name;
    s.pid = pid;
    s.running = s.next;
    s.running_request = s.next_request;
    s.stopping = false;
    log::info(kLog, std::format("{}: process {} started ({}{})", s.name, pid, launch_name(s.running),
                                s.running_request != 0 ? std::format(", request {}", s.running_request) : ""));
    set_state(s, "STARTING");
    const pid_t child = pid;
    const std::string name = s.name;
    spawn(
        [](Fleet* self, std::string device, pid_t spawned) -> awaitable<void> {
            asio::steady_timer timer(self->io_, kHelloTimeout);
            co_await timer.async_wait(use_awaitable);
            if (Slot* slot = self->find(device); slot != nullptr && slot->pid == spawned && !slot->session)
            {
                log::warn(kLog, device + ": no hello; killing process " + std::to_string(spawned));
                ::kill(spawned, SIGKILL);
            }
        }(this, name, child),
        "hello " + name);
}

void Fleet::stop_process(Slot& s, const std::string& reason)
{
    if (s.pid == 0 && !s.session) return;
    if (s.session) s.session->send_bye(reason);
    if (s.pid == 0)
    {
        log::info(kLog, std::format("{}: releasing node ({})", s.name, reason));
        s.session->close();
        s.session.reset();
        return;
    }
    s.stopping = true;
    ::kill(s.pid, SIGTERM);
    log::info(kLog, std::format("{}: stopping process {} ({})", s.name, s.pid, reason));
}

void Fleet::on_exit(pid_t pid, int status)
{
    const auto it = pids_.find(pid);
    if (it == pids_.end()) return;
    const std::string name = it->second;
    pids_.erase(it);
    Slot* s = find(name);
    if (s == nullptr) return;

    const std::string how = describe_status(status);
    const bool clean = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    (clean || s->stopping ? log::info : log::warn)(kLog, std::format("{}: process {} ended ({})", name, pid, how));
    s->pid = 0;
    if (s->session)
    {
        s->session->close();
        s->session.reset();
    }
    if (s->running != Launch::Run && s->running_request != 0)
    {
        fail_enrollment(s->running_request, "DEVICE_EXITED",
                        std::format("device process ended ({}) before its certificate was delivered", how));
        if (s->next_request == s->running_request)
        {
            s->next = Launch::Run;
            s->next_request = 0;
        }
        s->running_request = 0;
    }
    const bool stable = s->online_since != Clock::time_point{} && Clock::now() - s->online_since > kStableAfter;
    s->online_since = {};

    if (s->removed || (!s->device && s->next == Launch::Run))
    {
        forget(*s);
        reconcile_all();
        return;
    }
    if (shutting_down_) return;
    if (s->state != "AUTH_FAILED" && s->state != "DISABLED")
        set_state(*s, "OFFLINE", clean ? std::nullopt : std::optional<std::string>(how));
    else
        set_state(*s, s->state, s->last_error);

    std::chrono::seconds delay{0};
    if (!s->stopping && s->next == Launch::Run)
    {
        if (stable) s->failures = 0;
        delay = std::chrono::seconds(1 << std::min(s->failures, 5));
        delay = std::min(delay, std::chrono::seconds(30));
        ++s->failures;
    }
    s->stopping = false;
    schedule_restart(*s, delay);
    reconcile_all();
}

void Fleet::schedule_restart(Slot& s, std::chrono::seconds delay)
{
    if (delay == 0s)
    {
        reconcile(s);
        return;
    }
    if (s.restart_scheduled || !s.wanted) return;
    s.restart_scheduled = true;
    log::info(kLog, std::format("{}: restart in {} s", s.name, delay.count()));
    spawn(
        [](Fleet* self, std::string name, std::chrono::seconds wait) -> awaitable<void> {
            asio::steady_timer timer(self->io_, wait);
            co_await timer.async_wait(use_awaitable);
            if (Slot* slot = self->find(name))
            {
                slot->restart_scheduled = false;
                self->reconcile(*slot);
            }
        }(this, s.name, delay),
        "restart " + s.name);
}

void Fleet::forget(Slot& s)
{
    const std::string name = s.name;
    if ((s.removed || !s.device) && db::valid_device_name(name))
    {
        std::error_code ec;
        fs::remove_all(opts_.devices_dir / name, ec);
    }
    slots_.erase(name);
}

void Fleet::fail_enrollment(std::int64_t request_id, std::string code, std::string detail)
{
    if (request_id == 0) return;
    log::warn(kLog, std::format("request {} failed: {} ({})", request_id, code, detail));
    db_write("fail enrollment", [this, request_id, code = std::move(code), detail = std::move(detail)] {
        db_.fail_enrollment(request_id, code, detail);
    });
}

void Fleet::set_state(Slot& s, std::string state, std::optional<std::string> error)
{
    const int pid = s.pid;
    if (s.state == state && s.last_error == error && s.state_pid == pid) return;
    s.state = state;
    s.last_error = error;
    s.state_pid = pid;
    if (!s.device) return;
    const int id = s.device->device_id;
    db_write("session of " + s.name, [this, id, state = std::move(state), pid, error = std::move(error)] {
        db_.upsert_session(id, state, pid != 0 ? std::optional<int>(pid) : std::nullopt, error);
    });
}

Fleet::Slot* Fleet::find(const std::string& name)
{
    const auto it = slots_.find(name);
    return it == slots_.end() ? nullptr : &it->second;
}

Fleet::Slot* Fleet::find_by_id(int device_id)
{
    for (auto& [_, s] : slots_)
        if (s.device && s.device->device_id == device_id) return &s;
    return nullptr;
}

std::size_t Fleet::running() const
{
    return static_cast<std::size_t>(std::ranges::count_if(
        slots_, [](const auto& kv) { return kv.second.pid != 0 || kv.second.session != nullptr; }));
}

} // namespace pgdemo::controller
