#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <sys/types.h>

#include <db/DatabaseManager.hpp>
#include <pgdemo_msgs/msg/hello.hpp>

#include "Session.hpp"
#include "TelemetryWriter.hpp"

namespace pgdemo::controller {

/// Settings of the device supervisor (command-line options of the controller).
struct FleetOptions
{
    std::string device_bin;                          ///< pgdemo-device executable
    std::filesystem::path devices_dir;               ///< one key directory per device, 0700
    std::size_t max_devices{16};                     ///< device processes and attached nodes at once
    std::chrono::seconds pending_max_age{600};       ///< config requests older than this become TIMEOUT
    std::chrono::seconds enrollment_timeout{120};    ///< enrollments open longer than this become EXPIRED
    std::chrono::seconds maintenance_interval{3600}; ///< partition maintenance period
    std::filesystem::path puf_file;                  ///< devices.puf; empty leaves worker keys alone
    std::filesystem::path factory_root;              ///< factory root certificate passed to workers
    std::string factory_addr;                        ///< host:port of pgdemo-factory; empty skips signing
    std::string manager_tls;                         ///< host:port of the mutual handshake; empty skips it
};

/// How a device process is started.
enum class Launch {
    Run,    ///< prove the stored identity and produce
    Enroll, ///< first key and certificate, then run
    Rotate, ///< new key and certificate, then run
};

/// `hello.mode` for a launch.
inline std::uint8_t launch_mode(Launch launch) noexcept
{
    switch (launch)
    {
    case Launch::Enroll: return pgdemo_msgs::msg::Hello::ENROLL;
    case Launch::Rotate: return pgdemo_msgs::msg::Hello::ROTATE;
    case Launch::Run: break;
    }
    return pgdemo_msgs::msg::Hello::RUN;
}

inline std::string_view launch_name(Launch launch) noexcept
{
    switch (launch)
    {
    case Launch::Enroll: return "enroll";
    case Launch::Rotate: return "rotate";
    case Launch::Run: break;
    }
    return "run";
}

inline std::string_view mode_name(std::uint8_t mode) noexcept
{
    if (mode == pgdemo_msgs::msg::Hello::ENROLL) return "enroll";
    if (mode == pgdemo_msgs::msg::Hello::ROTATE) return "rotate";
    return "run";
}

/**
 * @brief One slot per device, and the processes the controller forked.
 *
 * `reconcile` starts a child through `spawn_process` when the slot is wanted
 * and under `--max-devices`. Exits come back through `reap`. A pending `hello`
 * is handed to `set_admit` before the child is started.
 */
class ProcessTable
{
public:
    /// What the controller knows about one device, or one enrollment in progress.
    struct Slot
    {
        std::string name;
        std::string type_id;
        db::Payload payload{db::Payload::Scalar};
        std::optional<db::Device> device; ///< empty until the first certificate is issued

        Launch next{Launch::Run};     ///< how the next process starts
        std::int64_t next_request{0}; ///< enrollment for `next` != Run

        pid_t pid{0};                     ///< running process, or 0
        Launch running{Launch::Run};      ///< how it was started
        std::int64_t running_request{0};  ///< its enrollment, until decided
        std::shared_ptr<Session> session; ///< its connection, once accepted

        bool wanted{true};    ///< false after AUTH_FAILED, until the identity changes
        bool stopping{false}; ///< we asked the process to exit
        bool removed{false};  ///< device deleted: forget the slot once the process is gone
        bool restart_scheduled{false};
        int failures{0}; ///< consecutive abnormal exits, for the backoff
        std::chrono::steady_clock::time_point online_since;
        /// Kill the child if it has not said hello by then.
        std::optional<std::chrono::steady_clock::time_point> hello_by;
        /// When a scheduled restart may run.
        std::optional<std::chrono::steady_clock::time_point> restart_at;

        std::string state;                     ///< last `device_session.state` written
        std::optional<std::string> last_error; ///< last error written with it
        int state_pid{0};                      ///< last pid written with it
    };

    ProcessTable(db::DatabaseManager& db, TelemetryWriter& writer, FleetOptions opts, Downlink downlink);
    ~ProcessTable();

    ProcessTable(const ProcessTable&) = delete;
    ProcessTable& operator=(const ProcessTable&) = delete;

    /// Create the devices directory and take `devices_dir/.controller.lock`.
    /// Throws if another controller on this machine already holds it.
    void acquire_lock();
    /// Close the lock file. Idempotent.
    void release_lock();

    [[nodiscard]] const FleetOptions& options() const noexcept { return opts_; }
    [[nodiscard]] const Downlink& downlink() const noexcept { return downlink_; }
    [[nodiscard]] bool shutting_down() const noexcept { return shutting_down_; }
    void shutting_down(bool stopping) noexcept { shutting_down_ = stopping; }

    /// Called with a `hello` that was waiting for its slot. Set before `boot`.
    void set_admit(std::function<void(pgdemo_msgs::msg::Hello)> admit);
    /// Keep a `hello` that arrived before its slot existed.
    void remember_hello(std::string name, pgdemo_msgs::msg::Hello hello);

    void apply_enrollment(const db::Enrollment& enrollment);
    void apply_device(const db::Device& device);
    /// `device_events` DELETE: stop the process and drop the slot when it is gone.
    void remove_device(const std::string& name);

    void reconcile(Slot& slot);
    void reconcile_all();
    void stop_process(Slot& slot, const std::string& reason);
    void reap();
    /// Hello deadlines, then `between` for each name, then restart deadlines, then `reap`.
    void poll(std::chrono::steady_clock::time_point now, const std::function<void(const std::string&)>& between);
    /// `bye`, SIGTERM, up to 5 s, then SIGKILL. Clears every session.
    void shutdown_children();

    void forget(Slot& slot);
    void fail_enrollment(std::int64_t request_id, std::string code, std::string detail);
    void set_state(Slot& slot, std::string state, std::optional<std::string> error = std::nullopt);
    /// Publish `bye` when there is no session yet.
    void send_bye(const std::string& device, const std::string& reason);

    Slot* find(const std::string& name);
    Slot* find_by_id(int device_id);
    [[nodiscard]] std::size_t running() const;
    [[nodiscard]] std::size_t size() const noexcept { return slots_.size(); }

    std::map<std::string, Slot>& slots() noexcept { return slots_; }
    std::map<pid_t, std::string>& pids() noexcept { return pids_; }

private:
    void start_process(Slot& slot);
    void on_exit(pid_t pid, int status);
    void schedule_restart(Slot& slot, std::chrono::seconds delay);
    void db_write(std::string what, std::function<void()> fn);

    db::DatabaseManager& db_;
    TelemetryWriter& writer_;
    FleetOptions opts_;
    Downlink downlink_;
    std::function<void(pgdemo_msgs::msg::Hello)> admit_;
    std::map<std::string, Slot> slots_;
    std::map<pid_t, std::string> pids_;
    /// `hello` that arrived before the slot existed.
    std::map<std::string, pgdemo_msgs::msg::Hello> pending_hellos_;
    int lock_fd_{-1};
    bool shutting_down_{false};
};

} // namespace pgdemo::controller
