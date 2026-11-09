#include "Fleet.hpp"

#include "Spawn.hpp"

#include <pki/CertVerifier.hpp>
#include <pki/Certificate.hpp>
#include <wire/Frame.hpp>
#include <wire/Settle.hpp>

#include <log/Log.hpp>

#include <boost/asio/experimental/awaitable_operators.hpp>

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <climits>
#include <csignal>
#include <format>
#include <set>
#include <tuple>

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
    auto result = co_await (wire::settle(std::move(op)) || timer.async_wait(use_awaitable));
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

boost::json::object config_frame(const db::AppliedConfig& c)
{
    return {{"type", "config"},
            {"device_id", c.device_id},
            {"rate_ms", c.rate_ms},
            {"amplitude", c.amplitude},
            {"is_active", c.is_active}};
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

Session::Session(asio::local::stream_protocol::socket s)
    : socket(std::move(s))
    , outbox(socket.get_executor(), 64)
    , decision(socket.get_executor(), 1)
    , last_frame(Clock::now())
{
}

void Session::send(boost::json::object msg)
{
    if (!outbox.try_send(boost::system::error_code{}, std::move(msg)))
        log::warn(kLog, "outbox full or closed; frame dropped");
}

void Session::close()
{
    boost::system::error_code ec;
    socket.shutdown(asio::socket_base::shutdown_both, ec);
    socket.close(ec);
    outbox.close();
    decision.close();
}

// ---------------------------------------------------------------------------
// Fleet: set-up
// ---------------------------------------------------------------------------

Fleet::Fleet(asio::io_context& io, asio::thread_pool& pool, db::DatabaseManager& db, FleetOptions opts)
    : io_(io)
    , pool_(pool)
    , writes_(asio::make_strand(pool.get_executor()))
    , db_(db)
    , opts_(std::move(opts))
    , acceptor_(io)
    , sigchld_(io, SIGCHLD)
    , stop_signals_(io, SIGINT, SIGTERM)
{
}

Fleet::~Fleet() = default;

void Fleet::open_socket()
{
    fs::create_directories(opts_.devices_dir);
    fs::permissions(opts_.devices_dir, fs::perms::owner_all, fs::perm_options::replace);
    const asio::local::stream_protocol::endpoint endpoint(opts_.socket_path.string());
    if (fs::is_socket(opts_.socket_path))
    {
        // Rebinding would orphan the running controller's devices and double every process.
        asio::local::stream_protocol::socket probe(io_);
        boost::system::error_code connect_error;
        probe.connect(endpoint, connect_error);
        if (!connect_error)
            throw std::runtime_error(std::format("another controller is listening on {}", opts_.socket_path.string()));
    }
    std::error_code ec;
    fs::remove(opts_.socket_path, ec);
    acceptor_.open();
    acceptor_.bind(endpoint);
    acceptor_.listen();
    ::chmod(opts_.socket_path.c_str(), 0600);
    log::info(kLog, std::format("device socket {} (max {} device process(es), binary {})", opts_.socket_path.string(),
                                opts_.max_devices, opts_.device_bin));
}

void Fleet::start()
{
    spawn(reap_loop(), "reaper");
    spawn(signal_loop(), "signals");
    spawn(accept_loop(), "acceptor");
    spawn(boot(), "boot");
    spawn(heartbeat_loop(), "heartbeat");
    spawn(maintenance_loop(), "maintenance");
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
        const auto channel = wire::get_string(msg, "channel").value_or("");
        const auto* p = msg.if_contains("payload");
        if (p == nullptr || !p->is_object()) return;
        const auto& o = p->as_object();
        const auto id = wire::get_number(o, "request_id");

        if (channel == db::channel::kEnrollmentRequested && id)
            spawn(on_enrollment_requested(static_cast<std::int64_t>(*id)), "enrollment_requested");
        else if (channel == db::channel::kEnrollmentEvents && id)
            spawn(on_enrollment_decided(static_cast<std::int64_t>(*id)), "enrollment_events");
        else if (channel == db::channel::kDeviceEvents)
            spawn(on_device_event(wire::get_string(o, "action").value_or(""), wire::get_string(o, "device").value_or(""),
                                  static_cast<int>(wire::get_number(o, "device_id").value_or(0))),
                  "device_events");
        else if (channel == db::channel::kConfigRequested)
            spawn(drain(), "config_requested");
    });
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

awaitable<void> Fleet::accept_loop()
{
    for (;;)
    {
        auto accepted = co_await acceptor_.async_accept(asio::as_tuple(use_awaitable));
        auto& [ec, socket] = accepted;
        if (ec)
        {
            if (shutting_down_ || ec == asio::error::operation_aborted) co_return;
            log::warn(kLog, "accept: " + ec.message());
            continue;
        }
        spawn(device_session(std::move(socket)), "session");
    }
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
                log::warn(kLog, std::format("{}: no frames for {} s; killing process {}", name,
                                            std::chrono::duration_cast<std::chrono::seconds>(kStuckAfter).count(),
                                            s.pid));
                if (s.pid != 0) ::kill(s.pid, SIGKILL);
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

    boost::system::error_code ec;
    acceptor_.close(ec);
    for (auto& [name, s] : slots_)
        if (s.pid != 0) stop_process(s, "controller stopping");

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
    std::error_code fec;
    fs::remove(opts_.socket_path, fec);

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

awaitable<void> Fleet::device_session(asio::local::stream_protocol::socket socket)
{
    auto session = std::make_shared<Session>(std::move(socket));
    std::string name;
    pid_t peer = 0;
    std::optional<AuthFailure> auth_failure;
    std::optional<Refusal> refusal;
    std::string error;

    try
    {
        // The kernel vouches for the pid: only processes this controller
        // forked are accepted, and each under the name it was started with.
        const auto cred = peer_credentials(session->socket.native_handle());
        if (!cred || cred->uid != ::getuid()) throw std::runtime_error("connection from another user");
        const auto it = pids_.find(cred->pid);
        if (it == pids_.end())
            throw std::runtime_error(std::format("pid {} was not started by this controller", cred->pid));
        peer = cred->pid;
        name = it->second;

        const auto hello = co_await with_deadline(wire::read_frame(session->socket), kHelloTimeout, "hello");
        Slot* s = find(name);
        if (s == nullptr || s->pid != peer) throw std::runtime_error("process is no longer supervised");
        if (wire::type_of(hello) != "hello" || wire::get_string(hello, "device") != name)
            throw AuthFailure("BAD_HELLO", std::format("process {} did not introduce itself as {}", peer, name));
        if (wire::get_string(hello, "mode") != launch_name(s->running))
            throw AuthFailure("BAD_HELLO", std::format("process {} was started to {}", peer, launch_name(s->running)));
        if (s->session) throw std::runtime_error("process is already connected");
        s->session = session;

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
            e.code() != asio::error::connection_reset)
            error = e.what();
    }
    catch (const std::exception& e)
    {
        error = e.what();
    }

    const std::string tag = name.empty() ? "unknown peer" : name;
    std::optional<boost::json::object> bye;
    if (Slot* s = find(name); auth_failure)
    {
        log::warn(kLog, std::format("{}: authentication failed: {} ({})", tag, auth_failure->code, auth_failure->what()));
        if (s != nullptr && s->session == session)
        {
            s->wanted = false;
            set_state(*s, "AUTH_FAILED", auth_failure->code + ": " + auth_failure->what());
        }
        bye = boost::json::object{{"type", "bye"}, {"reason", "authentication failed: " + auth_failure->code}};
    }
    else if (refusal)
    {
        log::info(kLog, std::format("{}: {}", tag, refusal->what()));
        if (s != nullptr && s->session == session && !refusal->state.empty())
            set_state(*s, refusal->state, refusal->what());
        bye = boost::json::object{{"type", "bye"}, {"reason", refusal->what()}};
    }
    else if (!error.empty())
    {
        log::warn(kLog, std::format("{}: session ended: {}", tag, error));
    }
    if (bye)
    {
        try
        {
            co_await wire::write_frame(session->socket, *bye);
        }
        catch (const std::exception&)
        {
        }
    }

    if (Slot* s = find(name); s != nullptr && s->session == session)
    {
        s->session.reset();
        if (s->state == "ONLINE" || s->state == "STARTING") set_state(*s, "OFFLINE");
        // Without its session the process can do nothing: make it exit.
        if (s->pid != 0 && s->pid == peer) ::kill(peer, SIGTERM);
    }
    session->close();
}

awaitable<void> Fleet::obtain_certificate(std::shared_ptr<Session> session, std::string name)
{
    const auto msg = co_await with_deadline(wire::read_frame(session->socket), kCsrTimeout, "the CSR");
    Slot* s = find(name);
    if (s == nullptr) throw Refusal("", "device removed during enrollment");
    const std::int64_t request = s->running_request;
    if (wire::type_of(msg) != "csr")
        throw std::runtime_error(std::format("expected csr, got {} {}", wire::type_of(msg),
                                             wire::get_string(msg, "detail").value_or("")));
    const auto csr = wire::get_string(msg, "csr").value_or("");

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
        co_await wire::write_frame(session->socket, {{"type", "rejected"},
                                                     {"code", outcome.error_code},
                                                     {"detail", outcome.error_detail}});
        throw Refusal("", std::format("request {} {}: {} {}", request, outcome.status, outcome.error_code,
                                      outcome.error_detail));
    }
    if (outcome.device_id)
    {
        const int id = *outcome.device_id;
        const auto device = co_await on_db([this, id] { return db_.load_device(id); });
        if (device) apply_device(*device);
    }
    co_await wire::write_frame(session->socket, {{"type", "cert"}, {"cert", *outcome.cert_pem}});
    // From here on it is an ordinary device process: a revocation stops it.
    if (s = find(name); s != nullptr) s->running = Launch::Run;
    log::info(kLog, std::format("{}: certificate of request {} delivered", name, request));
}

awaitable<void> Fleet::authenticate(std::shared_ptr<Session> session, std::string name)
{
    const auto nonce = pki::random_bytes(32);
    co_await wire::write_frame(session->socket, {{"type", "challenge"}, {"nonce", pki::base64_encode(nonce)}});
    const auto msg = co_await with_deadline(wire::read_frame(session->socket), kProofTimeout, "the proof");

    const auto type = wire::type_of(msg);
    if (type == "error")
        throw AuthFailure(wire::get_string(msg, "code").value_or("DEVICE_ERROR"),
                          wire::get_string(msg, "detail").value_or("device reported an error"));
    if (type != "proof") throw AuthFailure("BAD_PROOF", "expected proof, got " + type);
    const auto cert = wire::get_string(msg, "cert").value_or("");
    std::vector<std::uint8_t> signature;
    try
    {
        signature = pki::base64_decode(wire::get_string(msg, "signature").value_or(""));
    }
    catch (const std::exception&)
    {
        throw AuthFailure("BAD_PROOF", "signature is not base64");
    }

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
    // Online first: a configuration committed from here on is queued in the
    // outbox and follows the initial one.
    session->online = true;
    const int id = device.device_id;
    const auto config = co_await on_db([this, id] { return db_.init_device_config(id); });
    if (!config) throw Refusal("", "device deleted");
    co_await wire::write_frame(session->socket, config_frame(*config));

    s = find(name);
    if (s == nullptr) throw Refusal("", "device deleted");
    session->last_frame = Clock::now();
    s->online_since = Clock::now();
    set_state(*s, "ONLINE");
    // settle(): when the reader hits EOF the writer must stop too, and vice versa.
    wire::rethrow_winner(co_await (wire::settle(read_samples(session, device)) || wire::settle(write_frames(session))));
}

awaitable<void> Fleet::read_samples(std::shared_ptr<Session> session, db::Device device)
{
    for (;;)
    {
        const auto msg = co_await wire::read_frame(session->socket);
        session->last_frame = Clock::now();
        const auto type = wire::type_of(msg);
        if (type == "heartbeat") continue;
        if (type != "sample")
        {
            log::warn(kLog, std::format("{}: ignoring {}", device.name, type));
            continue;
        }
        const auto seq = static_cast<std::int64_t>(wire::get_number(msg, "seq").value_or(0));
        if (device.payload == db::Payload::Gps)
        {
            const auto lon = wire::get_number(msg, "origin_lon");
            const auto lat = wire::get_number(msg, "origin_lat");
            const auto radius = wire::get_number(msg, "radius_m");
            const auto bearing = wire::get_number(msg, "bearing_rad");
            if (!lon || !lat || !radius || !bearing)
            {
                log::warn(kLog, std::format("{}: incomplete GPS sample {}", device.name, seq));
                continue;
            }
            const db::TrackSample t{device.device_id, *lon, *lat, *radius, *bearing, seq};
            const bool ok = co_await on_db([this, t] {
                try
                {
                    db_.write_gps_sample(t);
                    return true;
                }
                catch (const std::exception& e)
                {
                    log::warn(kLog, std::format("device {}: sample {} not stored: {}", t.device_id, t.seq, e.what()));
                    return false;
                }
            });
            (void)ok;
        }
        else
        {
            const auto value = wire::get_number(msg, "value");
            if (!value)
            {
                log::warn(kLog, std::format("{}: sample {} without value", device.name, seq));
                continue;
            }
            const db::DummySample d{device.device_id, *value, seq};
            const bool ok = co_await on_db([this, d] {
                try
                {
                    db_.write_scalar_sample(d);
                    return true;
                }
                catch (const std::exception& e)
                {
                    log::warn(kLog, std::format("device {}: sample {} not stored: {}", d.device_id, d.seq, e.what()));
                    return false;
                }
            });
            (void)ok;
        }
    }
}

awaitable<void> Fleet::write_frames(std::shared_ptr<Session> session)
{
    for (;;)
    {
        const auto msg = co_await session->outbox.async_receive(use_awaitable);
        co_await wire::write_frame(session->socket, msg);
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
        if (s->pid != 0) stop_process(*s, "device deleted");
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
        if (s != nullptr && s->session && s->session->online) s->session->send(config_frame(c));
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
        if (s.pid != 0 && s.running == Launch::Run) stop_process(s, "identity " + d.identity);
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

void Fleet::reconcile(Slot& s)
{
    if (shutting_down_ || s.removed || s.pid != 0 || s.restart_scheduled || !s.wanted) return;
    if (s.next == Launch::Run)
    {
        if (!s.device) return;
        if (!s.device->active())
        {
            set_state(s, "DISABLED", "identity " + s.device->identity);
            return;
        }
    }
    if (running() >= opts_.max_devices)
    {
        const std::string why = std::format("--max-devices {} reached", opts_.max_devices);
        if (s.next != Launch::Run)
        {
            fail_enrollment(s.next_request, "CAPACITY", why);
            s.next = Launch::Run;
            s.next_request = 0;
            if (!s.device)
            {
                forget(s);
                return;
            }
        }
        set_state(s, "UNMANAGED", why);
        return;
    }
    start_process(s);
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
    std::vector<std::string> argv{opts_.device_bin, "--name",   s.name,
                                  "--type",         s.type_id,  "--payload",
                                  std::string(db::payload_name(s.payload)), "--socket", opts_.socket_path.string(),
                                  "--dir",          dir.string()};
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
}

void Fleet::stop_process(Slot& s, const std::string& reason)
{
    if (s.pid == 0) return;
    s.stopping = true;
    if (s.session) s.session->send({{"type", "bye"}, {"reason", reason}});
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
    return static_cast<std::size_t>(std::ranges::count_if(slots_, [](const auto& kv) { return kv.second.pid != 0; }));
}

} // namespace pgdemo::controller
