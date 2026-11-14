#include "Fleet.hpp"

#include <pki/Bytes.hpp>
#include <pki/CertVerifier.hpp>
#include <pki/Certificate.hpp>
#include <pki/DeviceKey.hpp>

#include <db/Time.hpp>

#include <log/Log.hpp>

#include <csignal>
#include <unistd.h>

#include <filesystem>
#include <format>
#include <string_view>
#include <fstream>
#include <set>
#include <span>
#include <sstream>
#include <utility>
#include <vector>

namespace pgdemo::controller {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace {

constexpr std::string_view kLog = "fleet";
/// A device that sends nothing for this long (it heartbeats every 5 s) is killed.
constexpr auto kStuckAfter = 30s;
constexpr auto kHeartbeatEvery = 10s;
constexpr auto kMaintenanceEvery = 30s;
constexpr auto kCrlEvery = 1s;

std::string read_file(const std::filesystem::path& path)
{
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot read " + path.string());
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::span<const std::uint8_t> bytes_of(std::string_view text)
{
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
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

} // namespace

Fleet::Fleet(db::DatabaseManager& db, TelemetryWriter& writer, FleetOptions opts, Downlink downlink, SignalState& signals,
             std::function<void()> stop)
    : db_(db), writer_(writer), table_(db, writer, std::move(opts), std::move(downlink)), signals_(signals),
      stop_(std::move(stop))
{
    // Installed before boot(), which is the first reconcile. The callback is
    // not invoked from this constructor.
    table_.set_admit([this](pgdemo_msgs::msg::Hello hello) {
        auto deps = this->deps();
        Session::admit(deps, std::move(hello));
    });
}

SessionDeps Fleet::deps()
{
    return SessionDeps{table_, db_, writer_, early_};
}

void Fleet::acquire_lock()
{
    table_.acquire_lock();
}

void Fleet::boot()
{
    db_.reset_sessions();
    sweep();
    drain();
    const auto now = Clock::now();
    next_heartbeat_ = now + kHeartbeatEvery;
    next_maintenance_ = now + kMaintenanceEvery;
    next_partitions_ = now + table_.options().maintenance_interval;
    log::info(kLog, std::format("supervising {} device(s), {} process(es) started (max {}); Ctrl-C to stop", table_.size(),
                                table_.running(), table_.options().max_devices));
}

void Fleet::db_write(std::string what, std::function<void()> fn)
{
    try
    {
        fn();
    }
    catch (const std::exception& e)
    {
        log::warn(kLog, what + ": " + e.what());
    }
}

void Fleet::notify(boost::json::object msg)
{
    std::lock_guard lock(events_mu_);
    events_.push_back(std::move(msg));
}

void Fleet::tick()
{
    if (table_.shutting_down()) return;
    drain_events();
    if (table_.shutting_down()) return;
    if (const int sig = signals_.stop.exchange(0))
    {
        log::info(kLog, std::format("signal {} received, shutting down", sig));
        shutdown();
        return;
    }
    signals_.child.store(false);
    table_.reap();
    poll_slots();
    if (table_.shutting_down()) return;

    const auto now = Clock::now();
    if (now >= next_heartbeat_)
    {
        next_heartbeat_ = now + kHeartbeatEvery;
        heartbeat();
    }
    if (table_.shutting_down()) return;
    if (now >= next_maintenance_)
    {
        next_maintenance_ = now + kMaintenanceEvery;
        maintain();
    }
    if (now >= next_crl_)
    {
        next_crl_ = now + kCrlEvery;
        publish_crl();
    }
}

void Fleet::drain_events()
{
    std::deque<boost::json::object> batch;
    {
        std::lock_guard lock(events_mu_);
        batch.swap(events_);
    }
    for (const auto& msg : batch)
    {
        if (table_.shutting_down()) return;
        try
        {
            handle_notify(msg);
        }
        catch (const std::exception& e)
        {
            log::error(kLog, std::string("notify: ") + e.what());
        }
    }
}

void Fleet::handle_notify(const boost::json::object& msg)
{
    const auto channel = field_string(msg, "channel").value_or("");
    const auto* p = msg.if_contains("payload");
    if (p == nullptr || !p->is_object()) return;
    const auto& o = p->as_object();
    const auto id = field_number(o, "request_id");

    if (channel == db::channel::kEnrollmentRequested && id)
        on_enrollment_requested(static_cast<std::int64_t>(*id));
    else if (channel == db::channel::kEnrollmentEvents && id)
        on_enrollment_decided(static_cast<std::int64_t>(*id));
    else if (channel == db::channel::kDeviceEvents)
        on_device_event(field_string(o, "action").value_or(""), field_string(o, "device").value_or(""),
                        static_cast<int>(field_number(o, "device_id").value_or(0)));
    else if (channel == db::channel::kConfigRequested)
        drain();
}

void Fleet::poll_slots()
{
    const auto now = Clock::now();
    table_.poll(now, [this, now](const std::string& name) {
        auto* s = table_.find(name);
        if (s != nullptr && s->session && s->session->deadline && now >= *s->session->deadline)
        {
            auto deps = this->deps();
            s->session->expire(deps, name);
        }
    });
}

void Fleet::heartbeat()
{
    std::vector<int> seen;
    const auto now = Clock::now();
    std::vector<std::string> names;
    for (const auto& [name, _] : table_.slots()) names.push_back(name);
    for (const auto& name : names)
    {
        auto* s = table_.find(name);
        if (s == nullptr || !s->session || !s->session->online || !s->device) continue;
        if (now - s->session->last_frame > kStuckAfter)
        {
            log::warn(kLog, std::format("{}: no frames for {} s", name,
                                        std::chrono::duration_cast<std::chrono::seconds>(kStuckAfter).count()));
            // A child we forked is killed and reaped. A node started elsewhere is only released.
            if (s->pid != 0) ::kill(s->pid, SIGKILL);
            else table_.stop_process(*s, "no frames");
            continue;
        }
        seen.push_back(s->device->device_id);
    }
    if (!seen.empty()) db_write("last_seen", [this, seen] { db_.touch_sessions(seen); });
}

void Fleet::maintain()
{
    if (!db_.listener_alive())
    {
        log::error(kLog, "listener connection lost; exiting so a supervisor can restart us");
        exit_code_ = 1;
        shutdown();
        return;
    }
    try
    {
        const auto expired = db_.expire_enrollments(table_.options().enrollment_timeout);
        for (const auto id : expired) log::warn(kLog, std::format("enrollment {} expired", id));
        sweep();
        if (Clock::now() >= next_partitions_)
        {
            next_partitions_ = Clock::now() + table_.options().maintenance_interval;
            std::vector<std::string> created;
            for (const auto p : db::kAllPayloads)
                for (auto& n : db_.ensure_history_partitions(p)) created.push_back(std::move(n));
            for (const auto& n : created) log::info(kLog, "created partition " + n);
        }
    }
    catch (const std::exception& e)
    {
        log::error(kLog, std::string("maintenance: ") + e.what());
    }
}

void Fleet::shutdown()
{
    if (table_.shutting_down()) return;
    table_.shutting_down(true);
    table_.shutdown_children();
    writer_.shutdown();
    table_.release_lock();
    db_write("reset sessions", [this] { db_.reset_sessions(); });
    log::info(kLog, "every device process has stopped");
    if (stop_) stop_();
}

bool Fleet::factory_listed(const std::string& name, const std::string& pem)
{
    const auto& opts = table_.options();
    if (pem.empty() || opts.factory_root.empty() || opts.factory_fingerprint.empty()) return false;
    try
    {
        const auto root = read_file(opts.factory_root);
        const auto info = pki::CertVerifier({root}).verify(pem, name, {});
        if (!db_.register_factory_worker(info.subject_cn, opts.factory_fingerprint, info.serial, info.fingerprint,
                                         db::format_time(info.not_before), db::format_time(info.not_after), info.pem))
            log::info(kLog, name + " authenticated; no device row to attach the certificate");
        return db_.load_manager_revoked_serials().contains(info.serial);
    }
    catch (const pki::PkiError& e)
    {
        if (e.code == "CERT_CHAIN" || e.code == "CERT_SUBJECT") return false;
        log::warn(kLog, name + ": " + e.what());
        return false;
    }
}

void Fleet::publish_crl()
{
    const auto& opts = table_.options();
    if (opts.manager_key.empty() || !table_.downlink().crl) return;
    try
    {
        const auto payload = pki::manager_crl_payload(db_.load_manager_revoked_serials());
        auto stored = db_.load_manager_crl();
        if (!stored || stored->payload != payload)
        {
            const auto signature = pki::to_hex(pki::DeviceKey::load(opts.manager_key).sign(bytes_of(payload)));
            db_.replace_manager_crl(payload, signature);
            stored = db::DatabaseManager::ManagerCrlRecord{payload, signature};
        }
        pgdemo_msgs::msg::Crl message;
        message.payload = stored->payload;
        message.signature = stored->signature_hex;
        table_.downlink().crl(std::move(message));
    }
    catch (const std::exception& e)
    {
        log::warn(kLog, std::string("manager CRL: ") + e.what());
    }
}

void Fleet::on_hello(pgdemo_msgs::msg::Hello hello)
{
    if (table_.shutting_down()) return;
    if (factory_listed(hello.device, hello.certificate))
    {
        table_.send_bye(hello.device, "revoked");
        return;
    }
    auto deps = this->deps();
    Session::admit(deps, std::move(hello));
}

void Fleet::on_uplink(Uplink msg)
{
    if (table_.shutting_down()) return;
    const auto name = device_of(msg);
    if (name.empty()) return;
    auto* s = table_.find(name);
    if (s == nullptr || !s->session)
    {
        early_[name] = std::move(msg);
        return;
    }
    auto deps = this->deps();
    s->session->handle(deps, std::move(msg));
}

void Fleet::on_enrollment_requested(std::int64_t request_id)
{
    const auto e = db_.load_enrollment(request_id);
    if (!e || e->status != "PENDING") return;
    table_.apply_enrollment(*e);
}

void Fleet::on_enrollment_decided(std::int64_t request_id)
{
    auto waiting = [&]() -> ProcessTable::Slot* {
        for (auto& [_, s] : table_.slots())
            if (s.running_request == request_id || s.next_request == request_id) return &s;
        return nullptr;
    };
    if (waiting() == nullptr) return;
    const auto outcome = db_.enrollment_outcome(request_id);
    auto* s = waiting();
    if (s == nullptr || !outcome || outcome->status == "PENDING" || outcome->status == "CSR_SUBMITTED") return;

    if (s->running_request == request_id && s->session)
    {
        if (s->session->phase == Phase::WaitCert)
        {
            auto deps = this->deps();
            s->session->accept_certificate(deps, request_id, *outcome);
        }
        else if (!s->session->pending_outcome)
            s->session->pending_outcome = *outcome;
        return;
    }
    // Decided before its process got that far: expired, or refused by the server.
    if (outcome->status == "ISSUED") return;
    log::info(kLog, std::format("{}: request {} {} ({})", s->name, request_id, outcome->status, outcome->error_code));
    if (s->next_request == request_id)
    {
        s->next = Launch::Run;
        s->next_request = 0;
    }
    if (s->running_request == request_id)
    {
        s->running_request = 0;
        table_.stop_process(*s, "enrollment " + outcome->status);
    }
    else if (!s->device && s->pid == 0)
    {
        table_.forget(*s);
    }
}

void Fleet::on_device_event(std::string action, std::string name, int device_id)
{
    if (action == "DELETE")
    {
        table_.remove_device(name);
        return;
    }
    if (action != "INSERT" && action != "ROTATED" && action != "REVOKED" && action != "UNREVOKED") return;

    if (device_id == 0)
        if (const auto* s = table_.find(name); s != nullptr && s->device) device_id = s->device->device_id;
    if (device_id == 0) return;
    const auto device = db_.load_device(device_id);
    if (device) table_.apply_device(*device);
}

void Fleet::drain()
{
    auto deps = this->deps();
    forward_configs(deps);
}

void Fleet::sweep()
{
    const auto devices = db_.load_devices();
    std::set<std::string> present;
    for (const auto& d : devices)
    {
        present.insert(d.name);
        table_.apply_device(d);
    }
    std::vector<std::string> gone;
    for (const auto& [name, s] : table_.slots())
        if (s.device && !present.contains(name)) gone.push_back(name);
    for (const auto& name : gone) on_device_event("DELETE", name, 0);

    const auto pending = db_.pending_enrollments();
    for (const auto& e : pending) table_.apply_enrollment(e);
    table_.reconcile_all();
}

} // namespace pgdemo::controller
