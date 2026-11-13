#include "ProcessTable.hpp"

#include "Spawn.hpp"

#include <log/Log.hpp>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <climits>
#include <csignal>
#include <format>
#include <thread>
#include <vector>

namespace pgdemo::controller {

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace {

constexpr std::string_view kLog = "fleet";
constexpr auto kHelloTimeout = 5s;
/// A process online for this long resets the restart backoff.
constexpr auto kStableAfter = 60s;

std::string describe_status(int status)
{
    if (WIFEXITED(status)) return std::format("exit {}", WEXITSTATUS(status));
    if (WIFSIGNALED(status)) return std::format("signal {}", WTERMSIG(status));
    return std::format("status {}", status);
}

} // namespace

ProcessTable::ProcessTable(db::DatabaseManager& db, TelemetryWriter& writer, FleetOptions opts, Downlink downlink)
    : db_(db), writer_(writer), opts_(std::move(opts)), downlink_(std::move(downlink))
{
}

ProcessTable::~ProcessTable()
{
    release_lock();
}

void ProcessTable::acquire_lock()
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

void ProcessTable::release_lock()
{
    if (lock_fd_ < 0) return;
    ::close(lock_fd_);
    lock_fd_ = -1;
}

void ProcessTable::set_admit(std::function<void(pgdemo_msgs::msg::Hello)> admit)
{
    admit_ = std::move(admit);
}

void ProcessTable::remember_hello(std::string name, pgdemo_msgs::msg::Hello hello)
{
    pending_hellos_[std::move(name)] = std::move(hello);
}

void ProcessTable::db_write(std::string what, std::function<void()> fn)
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

void ProcessTable::apply_enrollment(const db::Enrollment& e)
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
    s->restart_at.reset();
    reconcile(*s);
}

void ProcessTable::apply_device(const db::Device& d)
{
    auto [it, inserted] = slots_.try_emplace(d.name);
    (void)inserted;
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

void ProcessTable::remove_device(const std::string& name)
{
    Slot* s = find(name);
    if (s == nullptr) return;
    log::info(kLog, std::format("{}: device deleted", name));
    s->removed = true;
    s->wanted = false;
    if (s->pid != 0 || s->session) stop_process(*s, "device deleted");
    else
    {
        forget(*s);
        reconcile_all();
    }
}

void ProcessTable::reconcile(Slot& slot)
{
    if (shutting_down_ || slot.removed || slot.restart_scheduled || !slot.wanted) return;
    const std::string name = slot.name;
    if (!slot.session)
    {
        if (auto it = pending_hellos_.find(name); it != pending_hellos_.end())
        {
            auto hello = std::move(it->second);
            pending_hellos_.erase(it);
            if (admit_) admit_(std::move(hello)); // may erase the slot
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

void ProcessTable::reconcile_all()
{
    std::vector<std::pair<int, std::string>> order;
    for (const auto& [name, s] : slots_) order.emplace_back(s.device ? s.device->device_id : INT_MAX, name);
    std::ranges::sort(order);
    for (const auto& [_, name] : order)
        if (Slot* s = find(name)) reconcile(*s);
}

void ProcessTable::start_process(Slot& s)
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
    else if (s.device && !opts_.puf_file.empty())
    {
        argv.push_back("--device-id");
        argv.push_back(std::to_string(s.device->device_id));
        argv.push_back("--puf");
        argv.push_back(opts_.puf_file.string());
    }
    if (!opts_.factory_addr.empty())
    {
        argv.push_back("--factory");
        argv.push_back(opts_.factory_addr);
    }
    if (!opts_.manager_cert.empty() && !opts_.factory_root.empty())
    {
        argv.push_back("--manager-cert");
        argv.push_back(opts_.manager_cert.string());
        argv.push_back("--factory-root");
        argv.push_back(opts_.factory_root.string());
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
    s.hello_by = Clock::now() + kHelloTimeout;
    log::info(kLog, std::format("{}: process {} started ({}{})", s.name, pid, launch_name(s.running),
                                s.running_request != 0 ? std::format(", request {}", s.running_request) : ""));
    set_state(s, "STARTING");
}

void ProcessTable::stop_process(Slot& s, const std::string& reason)
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

void ProcessTable::reap()
{
    for (;;)
    {
        int status = 0;
        const pid_t pid = ::waitpid(-1, &status, WNOHANG);
        if (pid <= 0) break;
        on_exit(pid, status);
    }
}

void ProcessTable::poll(Clock::time_point now, const std::function<void(const std::string&)>& between)
{
    std::vector<std::string> names;
    names.reserve(slots_.size());
    for (const auto& [name, _] : slots_) names.push_back(name);
    for (const auto& name : names)
    {
        if (shutting_down_) return;
        Slot* s = find(name);
        if (s == nullptr) continue;
        if (s->pid != 0 && !s->session && s->hello_by && now >= *s->hello_by)
        {
            log::warn(kLog, name + ": no hello; killing process " + std::to_string(s->pid));
            ::kill(s->pid, SIGKILL);
            s->hello_by.reset();
        }
        if (between) between(name);
        s = find(name);
        if (s != nullptr && s->restart_scheduled && s->restart_at && now >= *s->restart_at)
        {
            s->restart_scheduled = false;
            reconcile(*s);
        }
    }
    reap();
}

void ProcessTable::shutdown_children()
{
    for (auto& [name, s] : slots_)
        if (s.pid != 0 || s.session) stop_process(s, "controller stopping");

    const auto deadline = Clock::now() + 5s;
    while (!pids_.empty() && Clock::now() < deadline)
    {
        reap();
        if (!pids_.empty()) std::this_thread::sleep_for(100ms);
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
        s.session.reset();
    }
}

void ProcessTable::on_exit(pid_t pid, int status)
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
    s->hello_by.reset();
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

void ProcessTable::schedule_restart(Slot& s, std::chrono::seconds delay)
{
    if (delay == 0s)
    {
        reconcile(s);
        return;
    }
    if (s.restart_scheduled || !s.wanted) return;
    s.restart_scheduled = true;
    s.restart_at = Clock::now() + delay;
    log::info(kLog, std::format("{}: restart in {} s", s.name, delay.count()));
}

void ProcessTable::forget(Slot& s)
{
    const std::string name = s.name;
    writer_.forget(name);
    if ((s.removed || !s.device) && db::valid_device_name(name))
    {
        std::error_code ec;
        fs::remove_all(opts_.devices_dir / name, ec);
    }
    slots_.erase(name);
}

void ProcessTable::fail_enrollment(std::int64_t request_id, std::string code, std::string detail)
{
    if (request_id == 0) return;
    log::warn(kLog, std::format("request {} failed: {} ({})", request_id, code, detail));
    db_write("fail enrollment", [this, request_id, code = std::move(code), detail = std::move(detail)] {
        db_.fail_enrollment(request_id, code, detail);
    });
}

void ProcessTable::set_state(Slot& s, std::string state, std::optional<std::string> error)
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

void ProcessTable::send_bye(const std::string& device, const std::string& reason)
{
    if (!downlink_.bye) return;
    pgdemo_msgs::msg::Bye msg;
    msg.device = device;
    msg.reason = reason;
    downlink_.bye(std::move(msg));
}

ProcessTable::Slot* ProcessTable::find(const std::string& name)
{
    const auto it = slots_.find(name);
    return it == slots_.end() ? nullptr : &it->second;
}

ProcessTable::Slot* ProcessTable::find_by_id(int device_id)
{
    for (auto& [_, s] : slots_)
        if (s.device && s.device->device_id == device_id) return &s;
    return nullptr;
}

std::size_t ProcessTable::running() const
{
    return static_cast<std::size_t>(
        std::ranges::count_if(slots_, [](const auto& kv) { return kv.second.pid != 0 || kv.second.session != nullptr; }));
}

} // namespace pgdemo::controller
