#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include <boost/asio.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/json.hpp>

#include <db/DatabaseManager.hpp>

#include "Protocol.hpp"

namespace pgdemo::controller {

/// Settings of the device supervisor (command-line options of the controller).
struct FleetOptions
{
    std::string device_bin;                         ///< pgdemo-device executable
    std::filesystem::path devices_dir;              ///< one key directory per device, 0700
    std::size_t max_devices{16};                    ///< device processes and attached nodes at once
    std::chrono::seconds pending_max_age{600};      ///< config requests older than this become TIMEOUT
    std::chrono::seconds enrollment_timeout{120};   ///< enrollments open longer than this become EXPIRED
    std::chrono::seconds maintenance_interval{3600}; ///< partition maintenance period
};

/// How a device process is started.
enum class Launch {
    Run,    ///< prove the stored identity and produce
    Enroll, ///< first key and certificate, then run
    Rotate, ///< new key and certificate, then run
};

/// One worker that has said hello and is being supervised.
struct Session
{
    /// `downlink` publishes manager messages. `inbox` carries what the worker sends back.
    Session(boost::asio::any_io_executor ex, std::string name, Downlink downlink);

    /// Publish `bye` for this worker. Dropped once `close()`d.
    void send_bye(std::string reason);
    /// Publish the issued certificate.
    void send_certificate(std::string pem);
    /// Publish a refusal from the CA.
    void send_rejected(std::string code, std::string detail);
    /// Publish a 32-byte challenge nonce.
    void send_challenge(const std::array<unsigned char, 32>& nonce);
    /// Publish applied configuration.
    void send_config(const db::AppliedConfig& config);
    /// Queue an uplink message for the session coroutine. Dropped if the inbox is full or closed.
    void post(Uplink msg);
    /// Close both channels. Idempotent.
    void close();

    /// Messages from the worker, read by the session coroutine.
    boost::asio::experimental::channel<void(boost::system::error_code, Uplink)> inbox;
    /// The enrollment decision, delivered on `enrollment_events`.
    boost::asio::experimental::channel<void(boost::system::error_code, db::EnrollmentOutcome)> decision;
    /// When the worker last sent anything; a silent one is dropped (`heartbeat_loop()`).
    std::chrono::steady_clock::time_point last_frame;
    /// Authenticated and streaming: configuration changes go out through `send_config()`.
    bool online{false};

private:
    std::string name_;
    Downlink downlink_;
    bool closed_{false};
};

/**
 * @brief Supervisor of the device processes.
 *
 * Every member runs on the `io_context` thread: process table, slots and
 * sessions need no locks. Database calls are blocking (libpqxx), so they run
 * on the thread pool through `on_db()` and the coroutine resumes on the
 * io thread with the result. Listener callbacks arrive on the listener
 * thread and only post to the io thread (`notify()`).
 *
 * A ROS thread delivers worker topics through `on_hello()` and `on_uplink()`,
 * which only post to this thread, the same way the listener thread posts notifications.
 *
 * Coroutines:
 * - one `device_session()` per `hello`
 * - `reap_loop()`: SIGCHLD, `waitpid(WNOHANG)`, restart with backoff
 * - `heartbeat_loop()`: `last_seen`, stuck processes
 * - `maintenance_loop()`: partitions, expired enrollments, identity sweep, listener health
 * - `signal_loop()`: SIGINT/SIGTERM, orderly shutdown
 */
class Fleet
{
public:
    /**
     * @param io   the single thread that owns the fleet; signals are delivered here
     * @param pool threads for blocking database calls (`on_db()`)
     * @param db   connected, listener not yet running
     * @param opts     device binary, directories and limits
     * @param downlink publishes on `pgdemo/manager`; called on the io thread
     */
    Fleet(boost::asio::io_context& io, boost::asio::thread_pool& pool, db::DatabaseManager& db,
          FleetOptions opts, Downlink downlink);
    ~Fleet();

    Fleet(const Fleet&) = delete;
    Fleet& operator=(const Fleet&) = delete;

    /// Create the devices directory and take `devices_dir/.controller.lock`.
    /// Throws if another controller on this machine already holds it. Before `start()`.
    void acquire_lock();

    /// Spawn every coroutine. The io_context stops when shutdown has finished.
    void start();

    /// Listener callback, any thread: `{"channel", "payload"}`, posted to the io thread.
    void notify(boost::json::object msg);

    /// A `hello` from a worker, any thread: posted to the io thread.
    void on_hello(pgdemo_msgs::msg::Hello hello);
    /// One other worker message, any thread: posted to the io thread.
    void on_uplink(Uplink msg);

    /// 0 after a signal, 1 when the listener connection was lost.
    [[nodiscard]] int exit_code() const noexcept { return exit_code_; }

private:
    /// What the controller knows about one device, or one enrollment in progress.
    struct Slot
    {
        std::string name;
        std::string type_id;
        db::Payload payload{db::Payload::Scalar};
        std::optional<db::Device> device; ///< empty until the first certificate is issued

        Launch next{Launch::Run};      ///< how the next process starts
        std::int64_t next_request{0};  ///< enrollment for `next` != Run

        pid_t pid{0};                       ///< running process, or 0
        Launch running{Launch::Run};        ///< how it was started
        std::int64_t running_request{0};    ///< its enrollment, until decided
        std::shared_ptr<Session> session;   ///< its connection, once accepted

        bool wanted{true};    ///< false after AUTH_FAILED, until the identity changes
        bool stopping{false}; ///< we asked the process to exit
        bool removed{false};  ///< device deleted: forget the slot once the process is gone
        bool restart_scheduled{false};
        int failures{0};      ///< consecutive abnormal exits, for the backoff
        std::chrono::steady_clock::time_point online_since;

        std::string state;                     ///< last `device_session.state` written
        std::optional<std::string> last_error; ///< last error written with it
        int state_pid{0};                      ///< last pid written with it
    };

    template <typename F>
    auto on_db(F fn) -> boost::asio::awaitable<std::invoke_result_t<F&>>
    {
        using R = std::invoke_result_t<F&>;
        co_return co_await boost::asio::co_spawn(
            pool_, [fn = std::move(fn)]() mutable -> boost::asio::awaitable<R> { co_return fn(); },
            boost::asio::use_awaitable);
    }

    void spawn(boost::asio::awaitable<void> task, std::string what);
    void db_write(std::string what, std::function<void()> fn);

    // Coroutines.
    boost::asio::awaitable<void> boot();
    boost::asio::awaitable<void> reap_loop();
    boost::asio::awaitable<void> heartbeat_loop();
    boost::asio::awaitable<void> maintenance_loop();
    boost::asio::awaitable<void> signal_loop();
    boost::asio::awaitable<void> shutdown();

    boost::asio::awaitable<void> device_session(std::shared_ptr<Session> session, std::string name,
                                               pgdemo_msgs::msg::Hello hello);
    boost::asio::awaitable<void> obtain_certificate(std::shared_ptr<Session> session, std::string name);
    boost::asio::awaitable<void> authenticate(std::shared_ptr<Session> session, std::string name);
    boost::asio::awaitable<void> serve(std::shared_ptr<Session> session, std::string name);
    boost::asio::awaitable<void> read_samples(std::shared_ptr<Session> session, db::Device device);
    boost::asio::awaitable<Uplink> receive(std::shared_ptr<Session> session,
                                           std::chrono::steady_clock::duration timeout, std::string_view what);
    boost::asio::awaitable<void> telemetry_loop();
    boost::asio::awaitable<void> flush_telemetry();

    // Events.
    boost::asio::awaitable<void> on_enrollment_requested(std::int64_t request_id);
    boost::asio::awaitable<void> on_enrollment_decided(std::int64_t request_id);
    boost::asio::awaitable<void> on_device_event(std::string action, std::string name, int device_id);
    boost::asio::awaitable<void> drain();
    boost::asio::awaitable<void> sweep();

    void apply_enrollment(const db::Enrollment& e);
    void apply_device(const db::Device& d);
    void admit(pgdemo_msgs::msg::Hello hello);
    void enqueue_telemetry(std::variant<db::DummySample, db::TrackSample> sample);
    void send_bye(const std::string& device, const std::string& reason);

    // Process table.
    void reconcile(Slot& s);
    void reconcile_all();
    void start_process(Slot& s);
    void stop_process(Slot& s, const std::string& reason);
    void on_exit(pid_t pid, int status);
    void schedule_restart(Slot& s, std::chrono::seconds delay);
    void forget(Slot& s);
    void fail_enrollment(std::int64_t request_id, std::string code, std::string detail);
    void set_state(Slot& s, std::string state, std::optional<std::string> error = std::nullopt);

    Slot* find(const std::string& name);
    Slot* find_by_id(int device_id);
    [[nodiscard]] std::size_t running() const;

    boost::asio::io_context& io_;
    boost::asio::thread_pool& pool_;
    /// Session-state writes keep their order.
    boost::asio::strand<boost::asio::thread_pool::executor_type> writes_;
    db::DatabaseManager& db_;
    FleetOptions opts_;
    /// Publishes on `pgdemo/manager`. Empty callbacks drop the message.
    Downlink downlink_;
    /// Samples from every worker, flushed by `telemetry_loop()` on this thread.
    std::deque<std::variant<db::DummySample, db::TrackSample>> telemetry_;
    /// Samples dropped because `telemetry_` was at `kTelemetryCapacity`.
    std::size_t telemetry_dropped_{0};
    /// Wakes `telemetry_loop()` early once a batch is full.
    boost::asio::steady_timer telemetry_timer_;

    /// Registered at construction, before the first fork, so no exit is missed.
    boost::asio::signal_set sigchld_;
    boost::asio::signal_set stop_signals_;
    std::map<std::string, Slot> slots_;
    std::map<pid_t, std::string> pids_;
    /// `hello` that arrived before the slot existed.
    std::map<std::string, pgdemo_msgs::msg::Hello> pending_hellos_;
    /// The one uplink message that arrived before the session existed.
    std::map<std::string, Uplink> early_;
    int lock_fd_{-1};
    bool shutting_down_{false};
    int exit_code_{0};
};

} // namespace pgdemo::controller
