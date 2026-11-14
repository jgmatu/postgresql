#include "Session.hpp"

#include "ProcessTable.hpp"
#include "TelemetryWriter.hpp"

#include <pki/Bytes.hpp>
#include <pki/CertVerifier.hpp>
#include <pki/Certificate.hpp>

#include <log/Log.hpp>

#include <csignal>
#include <unistd.h>

#include <algorithm>
#include <format>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>

namespace pgdemo::controller {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace {

constexpr std::string_view kLog = "fleet";
constexpr std::size_t kInboxCapacity = 64;
constexpr auto kCsrTimeout = 30s;
constexpr auto kProofTimeout = 5s;

std::string_view phase_wait(Phase phase) noexcept
{
    switch (phase)
    {
    case Phase::WaitCsr: return "the CSR";
    case Phase::WaitCert: return "the certificate";
    case Phase::WaitProof: return "the proof";
    case Phase::Online: break;
    }
    return "the proof";
}

/// A repeated CSR, a heartbeat or a sample can arrive while the certificate is
/// issued. They are not the proof.
bool awaits_proof(const Uplink& msg)
{
    return std::holds_alternative<pgdemo_msgs::msg::Proof>(msg) || std::holds_alternative<pgdemo_msgs::msg::Error>(msg);
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

void finish(ProcessTable& table, const std::string& name, const std::shared_ptr<Session>& session,
            std::optional<std::string> auth_code, const std::string& auth_detail, std::optional<std::string> refusal_state,
            const std::string& refusal_detail, const std::string& error)
{
    std::optional<std::string> bye;
    ProcessTable::Slot* s = table.find(name);
    if (auth_code)
    {
        log::warn(kLog, std::format("{}: authentication failed: {} ({})", name, *auth_code, auth_detail));
        if (s != nullptr && s->session == session)
        {
            s->wanted = false;
            table.set_state(*s, "AUTH_FAILED", *auth_code + ": " + auth_detail);
        }
        bye = "authentication failed: " + *auth_code;
    }
    else if (refusal_state)
    {
        log::info(kLog, std::format("{}: {}", name, refusal_detail));
        if (s != nullptr && s->session == session && !refusal_state->empty())
            table.set_state(*s, *refusal_state, refusal_detail);
        bye = refusal_detail;
    }
    else if (!error.empty())
    {
        log::warn(kLog, std::format("{}: session ended: {}", name, error));
    }
    if (bye) session->send_bye(*bye);

    s = table.find(name);
    if (s != nullptr && s->session == session)
    {
        s->session.reset();
        if (s->state == "ONLINE" || s->state == "STARTING") table.set_state(*s, "OFFLINE");
        // A child without its session has nothing to do. A remote node got bye above.
        if (s->pid != 0 && s->pid == session->opened_pid) ::kill(session->opened_pid, SIGTERM);
        else if (!s->container.empty()) table.stop_process(*s, "session ended");
    }
    session->close();
}

template <typename F>
void guard(ProcessTable& table, const std::string& name, const std::shared_ptr<Session>& session, F fn)
{
    try
    {
        fn();
    }
    catch (const AuthFailure& e)
    {
        finish(table, name, session, e.code, e.what(), std::nullopt, {}, {});
    }
    catch (const Refusal& e)
    {
        finish(table, name, session, std::nullopt, {}, e.state, e.what(), {});
    }
    catch (const std::exception& e)
    {
        finish(table, name, session, std::nullopt, {}, std::nullopt, {}, e.what());
    }
}

void queue_inbox(Session& session, Uplink msg)
{
    if (session.closed() || session.inbox.size() >= kInboxCapacity)
    {
        log::warn(kLog, "inbox full or closed; message dropped");
        return;
    }
    session.inbox.push_back(std::move(msg));
}

} // namespace

void on_wait_csr(SessionDeps& deps, ProcessTable::Slot& slot, const Uplink& msg);
void apply_cert_outcome(SessionDeps& deps, const std::string& name, std::int64_t request, db::EnrollmentOutcome outcome);
void begin_auth(SessionDeps& deps, ProcessTable::Slot& slot);
void on_proof(SessionDeps& deps, ProcessTable::Slot& slot, const Uplink& msg);
void enter_online(SessionDeps& deps, ProcessTable::Slot& slot);
void on_online(SessionDeps& deps, ProcessTable::Slot& slot, const Uplink& msg);

Session::Session(std::string name, Downlink downlink)
    : last_frame(Clock::now()), name_(std::move(name)), downlink_(std::move(downlink))
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

void Session::send_challenge(const std::array<unsigned char, 32>& bytes)
{
    if (closed_ || !downlink_.challenge) return;
    pgdemo_msgs::msg::Challenge msg;
    msg.device = name_;
    msg.nonce = bytes;
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
    msg.request_id = config.request_id.value_or(0);
    downlink_.config(std::move(msg));
}

void Session::send_desired(const db::ConfigRequest& request)
{
    if (closed_ || !downlink_.config) return;
    pgdemo_msgs::msg::Config msg;
    msg.device = name_;
    msg.device_id = request.device_id;
    msg.rate_ms = request.rate_ms;
    msg.amplitude = request.amplitude;
    msg.is_active = request.is_active;
    msg.request_id = request.request_id;
    downlink_.config(std::move(msg));
}

void Session::close()
{
    closed_ = true;
}

void forward_configs(SessionDeps& deps)
{
    const auto batch = deps.db.forwardable_pending(deps.table.options().pending_max_age);
    std::size_t sent = 0;
    for (const auto& req : batch.pending)
    {
        auto* slot = deps.table.find(req.device);
        if (slot == nullptr || !slot->session || !slot->session->online) continue;
        slot->session->send_desired(req);
        ++sent;
    }
    if (sent != 0 || batch.expired != 0)
        log::info(kLog, std::format("config forwarded: {} sent, {} expired", sent, batch.expired));
}

void Session::admit(SessionDeps& deps, pgdemo_msgs::msg::Hello hello)
{
    auto& table = deps.table;
    const std::string name = hello.device;
    const auto mode = hello.mode;
    if (name.empty()) return;
    ProcessTable::Slot* s = table.find(name);
    if (s == nullptr)
    {
        table.remember_hello(name, std::move(hello));
        return;
    }
    if (s->session)
    {
        // The node repeats hello until the challenge. A new hello once it is online is another process.
        if (!s->session->online && mode == launch_mode(s->running)) return;
        s->session->send_bye("already connected");
        return;
    }
    if (table.shutting_down() || s->removed || !s->wanted)
    {
        table.send_bye(name, "not managed");
        return;
    }
    const bool ours = s->pid != 0 || !s->container.empty();
    const Launch expect = ours ? s->running : s->next;
    if (mode != launch_mode(expect))
    {
        table.send_bye(name, "unexpected mode");
        return;
    }
    if (!ours && table.running() >= table.options().max_devices)
    {
        const std::string why = std::format("--max-devices {} reached", table.options().max_devices);
        if (s->next != Launch::Run)
        {
            table.fail_enrollment(s->next_request, "CAPACITY", why);
            s->next = Launch::Run;
            s->next_request = 0;
            if (!s->device)
            {
                table.send_bye(name, why);
                table.forget(*s);
                return;
            }
        }
        table.set_state(*s, "UNMANAGED", why);
        table.send_bye(name, why);
        return;
    }

    s->session = std::make_shared<Session>(name, table.downlink());
    s->session->opened_pid = s->pid;
    s->hello_by.reset();
    if (s->pid == 0)
    {
        s->running = s->next;
        s->running_request = s->next_request;
    }
    log::info(kLog, std::format("{}: hello ({})", name, mode_name(mode)));
    s->session->begin(deps, hello);
}

void Session::begin(SessionDeps& deps, const pgdemo_msgs::msg::Hello& hello)
{
    auto session = deps.table.find(name_) != nullptr ? deps.table.find(name_)->session : nullptr;
    // `admit` just attached `this`. Use that object if the lookup raced with forget.
    if (session == nullptr) return;
    const std::string name = name_;
    std::optional<Uplink> early;
    if (auto it = deps.early.find(name); it != deps.early.end())
    {
        early = std::move(it->second);
        deps.early.erase(it);
    }
    guard(deps.table, name, session, [&] {
        ProcessTable::Slot* s = deps.table.find(name);
        if (s == nullptr || s->session != session) throw std::runtime_error("process is no longer supervised");
        if (hello.device != name)
            throw AuthFailure("BAD_HELLO", "the node did not introduce itself as " + name);
        if (hello.mode != launch_mode(s->running))
            throw AuthFailure("BAD_HELLO", std::format("the node was admitted to {}", launch_name(s->running)));

        if (s->running != Launch::Run)
        {
            session->phase = Phase::WaitCsr;
            session->deadline = Clock::now() + kCsrTimeout;
            if (early) on_wait_csr(deps, *s, *early);
            return;
        }
        if (early) session->inbox.push_back(std::move(*early));
        begin_auth(deps, *s);
    });
}

void Session::handle(SessionDeps& deps, Uplink msg)
{
    const std::string name = name_;
    ProcessTable::Slot* slot = deps.table.find(name);
    if (slot == nullptr || slot->session.get() != this) return;
    auto session = slot->session;
    guard(deps.table, name, session, [&] {
        ProcessTable::Slot* live = deps.table.find(name);
        if (live == nullptr || live->session != session) throw std::runtime_error("process is no longer supervised");
        switch (session->phase)
        {
        case Phase::WaitCsr: on_wait_csr(deps, *live, msg); break;
        case Phase::WaitCert: queue_inbox(*session, std::move(msg)); break;
        case Phase::WaitProof:
            if (!awaits_proof(msg))
            {
                log::info(kLog, name + ": still waiting for the proof");
                break;
            }
            on_proof(deps, *live, msg);
            break;
        case Phase::Online:
            session->last_frame = Clock::now();
            on_online(deps, *live, msg);
            break;
        }
    });
}

void Session::accept_certificate(SessionDeps& deps, std::int64_t request, db::EnrollmentOutcome outcome)
{
    const std::string name = name_;
    auto session = deps.table.find(name) != nullptr ? deps.table.find(name)->session : nullptr;
    if (session == nullptr) return;
    guard(deps.table, name, session, [&] { apply_cert_outcome(deps, name, request, std::move(outcome)); });
}

void Session::expire(SessionDeps& deps, const std::string& name)
{
    ProcessTable::Slot* s = deps.table.find(name);
    if (s == nullptr || s->session.get() != this) return;
    finish(deps.table, name, s->session, std::nullopt, {}, std::nullopt, {},
           std::format("timed out waiting for {}", phase_wait(phase)));
}

void on_wait_csr(SessionDeps& deps, ProcessTable::Slot& slot, const Uplink& msg)
{
    auto session = slot.session;
    if (session == nullptr) throw Refusal("", "device removed during enrollment");
    const std::int64_t request = slot.running_request;
    if (const auto* err = std::get_if<pgdemo_msgs::msg::Error>(&msg))
        throw std::runtime_error(std::format("expected csr, got error {} {}", err->code, err->detail));
    const auto* csr_msg = std::get_if<pgdemo_msgs::msg::Csr>(&msg);
    if (csr_msg == nullptr) throw std::runtime_error("expected csr");
    const auto csr = csr_msg->csr;

    const bool submitted = deps.db.submit_csr(request, csr);
    if (!submitted) throw Refusal("", std::format("request {} is no longer pending", request));
    log::info(kLog, std::format("{}: CSR of request {} submitted to the CA", slot.name, request));

    if (session->pending_outcome)
    {
        auto outcome = std::move(*session->pending_outcome);
        session->pending_outcome.reset();
        apply_cert_outcome(deps, slot.name, request, std::move(outcome));
        return;
    }
    session->phase = Phase::WaitCert;
    session->deadline = Clock::now() + deps.table.options().enrollment_timeout;
}

void apply_cert_outcome(SessionDeps& deps, const std::string& name, std::int64_t request, db::EnrollmentOutcome outcome)
{
    ProcessTable::Slot* s = deps.table.find(name);
    if (s == nullptr || !s->session) throw Refusal("", "device removed during enrollment");
    auto session = s->session;
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
        const auto device = deps.db.load_device(id);
        if (device) deps.table.apply_device(*device);
    }
    s = deps.table.find(name);
    if (s == nullptr || s->session != session) throw Refusal("", "device removed during enrollment");
    session->send_certificate(*outcome.cert_pem);
    // From here on it is an ordinary device process: a revocation stops it.
    s->running = Launch::Run;
    log::info(kLog, std::format("{}: certificate of request {} delivered", name, request));
    begin_auth(deps, *s);
}

void begin_auth(SessionDeps& deps, ProcessTable::Slot& slot)
{
    auto session = slot.session;
    if (session == nullptr) throw std::runtime_error("process is no longer supervised");
    const auto raw_nonce = pki::random_bytes(32);
    if (raw_nonce.size() != 32) throw AuthFailure("BAD_PROOF", "nonce is not 32 bytes");
    std::copy(raw_nonce.begin(), raw_nonce.end(), session->nonce.begin());
    session->send_challenge(session->nonce);
    while (!session->inbox.empty() && !awaits_proof(session->inbox.front()))
    {
        log::info(kLog, slot.name + ": ignoring a message queued before the challenge");
        session->inbox.pop_front();
    }
    if (!session->inbox.empty())
    {
        auto msg = std::move(session->inbox.front());
        session->inbox.pop_front();
        on_proof(deps, slot, msg);
        return;
    }
    session->phase = Phase::WaitProof;
    session->deadline = Clock::now() + kProofTimeout;
}

void on_proof(SessionDeps& deps, ProcessTable::Slot& slot, const Uplink& msg)
{
    auto session = slot.session;
    if (session == nullptr) throw std::runtime_error("process is no longer supervised");
    const std::string name = slot.name;
    if (const auto* err = std::get_if<pgdemo_msgs::msg::Error>(&msg))
        throw AuthFailure(err->code.empty() ? "DEVICE_ERROR" : err->code,
                          err->detail.empty() ? "device reported an error" : err->detail);
    const auto* proof = std::get_if<pgdemo_msgs::msg::Proof>(&msg);
    if (proof == nullptr) throw AuthFailure("BAD_PROOF", "expected proof");
    const auto& cert = proof->cert;
    const std::span<const std::uint8_t> signature(proof->signature.data(), proof->signature.size());

    ProcessTable::Slot* s = deps.table.find(name);
    if (s == nullptr || !s->device) throw Refusal("", "device is not registered");
    const int id = s->device->device_id;
    // Fresh from the database: a revocation committed a moment ago counts.
    const auto loaded = std::tuple(deps.db.load_device(id), deps.db.load_root_pems(), deps.db.load_revoked_serials());
    const auto& [device, roots, revoked] = loaded;
    s = deps.table.find(name);
    if (s == nullptr || s->session != session || !device) throw Refusal("", "device deleted");
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
    if (!pki::CertVerifier::verify_signature(cert, pki::proof_message(session->nonce, name, info.fingerprint), signature))
        throw AuthFailure("BAD_PROOF", "the signature does not verify with the certificate's key");
    log::info(kLog, std::format("{}: authenticated (serial {}, pid {})", name, info.serial, s->pid));
    enter_online(deps, *s);
}

void enter_online(SessionDeps& deps, ProcessTable::Slot& slot)
{
    auto session = slot.session;
    if (session == nullptr || !slot.device) throw Refusal("", "device deleted");
    const std::string name = slot.name;
    const db::Device device = *slot.device;
    // Online first: a configuration committed from here on is published after this one.
    session->online = true;
    session->device_id = device.device_id;
    session->payload = device.payload;
    const auto config = deps.db.init_device_config(device.device_id);
    if (!config) throw Refusal("", "device deleted");
    session->send_config(*config);
    forward_configs(deps);

    ProcessTable::Slot* s = deps.table.find(name);
    if (s == nullptr || s->session != session) throw Refusal("", "device deleted");
    session->last_frame = Clock::now();
    s->online_since = Clock::now();
    session->phase = Phase::Online;
    session->deadline.reset();
    deps.table.set_state(*s, "ONLINE");
    while (!session->inbox.empty())
    {
        auto queued = std::move(session->inbox.front());
        session->inbox.pop_front();
        session->last_frame = Clock::now();
        on_online(deps, *s, queued);
    }
}

void on_online(SessionDeps& deps, ProcessTable::Slot& slot, const Uplink& msg)
{
    auto session = slot.session;
    if (session == nullptr) return;
    const std::string name = slot.name;
    std::visit(
        [&](const auto& m) {
            using T = std::decay_t<decltype(m)>;
            if constexpr (std::is_same_v<T, pgdemo_msgs::msg::Heartbeat>)
                return;
            else if constexpr (std::is_same_v<T, pgdemo_msgs::msg::ScalarSample>)
            {
                if (session->payload != db::Payload::Scalar)
                {
                    log::warn(kLog, std::format("{}: ignoring scalar sample", name));
                    return;
                }
                deps.writer.push(name, db::DummySample{session->device_id, m.value, m.seq});
            }
            else if constexpr (std::is_same_v<T, pgdemo_msgs::msg::GpsSample>)
            {
                if (session->payload != db::Payload::Gps)
                {
                    log::warn(kLog, std::format("{}: ignoring GPS sample", name));
                    return;
                }
                deps.writer.push(name, db::TrackSample{session->device_id, m.origin_lon, m.origin_lat, m.radius_m,
                                                       m.bearing_rad, m.seq});
            }
            else if constexpr (std::is_same_v<T, pgdemo_msgs::msg::XySample>)
            {
                if (session->payload != db::Payload::Xy)
                {
                    log::warn(kLog, std::format("{}: ignoring xy sample", name));
                    return;
                }
                deps.writer.push(name, db::XySample{session->device_id, m.x, m.y, m.seq});
            }
            else if constexpr (std::is_same_v<T, pgdemo_msgs::msg::XyzSample>)
            {
                if (session->payload != db::Payload::Xyz)
                {
                    log::warn(kLog, std::format("{}: ignoring xyz sample", name));
                    return;
                }
                deps.writer.push(name, db::XyzSample{session->device_id, m.x, m.y, m.z, m.seq});
            }
            else if constexpr (std::is_same_v<T, pgdemo_msgs::msg::ConfigApplied>)
            {
                if (m.request_id == 0) return;
                const auto outcome = m.ok ? db::ConfigOutcome::success()
                                          : db::ConfigOutcome::failure(m.code.empty() ? "REJECTED" : m.code,
                                                                       m.detail.empty() ? "worker rejected the configuration"
                                                                                        : m.detail);
                try
                {
                    deps.db.complete_pending(m.request_id, outcome);
                }
                catch (const std::exception& e)
                {
                    log::warn(kLog, std::format("{}: configuration {} not recorded: {}", name, m.request_id, e.what()));
                }
            }
            else
                log::warn(kLog, std::format("{}: ignoring message while online", name));
        },
        msg);
}

} // namespace pgdemo::controller
