#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

#include <boost/asio.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/json.hpp>

#include <db/DatabaseManager.hpp>

namespace pgdemo::controller {

/// Settings of the device supervisor (command-line options of the controller).
struct FleetOptions
{
    std::string device_bin;                         ///< pgdemo-device executable
    std::filesystem::path devices_dir;              ///< one key directory per device, 0700
    std::filesystem::path socket_path;              ///< Unix socket the devices connect to
    std::size_t max_devices{16};                    ///< device processes run at once
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

/// One connection from a device process.
struct Session
{
    /// Take over an accepted connection.
    explicit Session(boost::asio::local::stream_protocol::socket s);

    /// Queue a frame for the writer. Dropped if the session is closing.
    void send(boost::json::object msg);
    /// Shut the socket and both channels down. Idempotent.
    void close();

    /// The connection to the device process.
    boost::asio::local::stream_protocol::socket socket;
    /// Frames to the device, written by one writer coroutine.
    boost::asio::experimental::channel<void(boost::system::error_code, boost::json::object)> outbox;
    /// The enrollment decision, delivered on `enrollment_events`.
    boost::asio::experimental::channel<void(boost::system::error_code, db::EnrollmentOutcome)> decision;
    /// When the device last sent anything; a silent one is killed (`heartbeat_loop()`).
    std::chrono::steady_clock::time_point last_frame;
    /// Authenticated and streaming: configuration changes go to `outbox`.
    bool online{false};
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
 * Coroutines:
 * - `accept_loop()`: one `device_session()` per connection
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
     * @param opts device binary, directories and limits
     */
    Fleet(boost::asio::io_context& io, boost::asio::thread_pool& pool, db::DatabaseManager& db,
          FleetOptions opts);
    ~Fleet();

    Fleet(const Fleet&) = delete;
    Fleet& operator=(const Fleet&) = delete;

    /// Create the devices directory and bind the socket. Before `start()`.
    /// Throws if another controller answers on the socket; a stale one is replaced.
    void open_socket();

    /// Spawn every coroutine. The io_context stops when shutdown has finished.
    void start();

    /// Listener callback, any thread: `{"channel", "payload"}`, posted to the io thread.
    void notify(boost::json::object msg);

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
    boost::asio::awaitable<void> accept_loop();
    boost::asio::awaitable<void> reap_loop();
    boost::asio::awaitable<void> heartbeat_loop();
    boost::asio::awaitable<void> maintenance_loop();
    boost::asio::awaitable<void> signal_loop();
    boost::asio::awaitable<void> shutdown();

    boost::asio::awaitable<void> device_session(boost::asio::local::stream_protocol::socket socket);
    boost::asio::awaitable<void> obtain_certificate(std::shared_ptr<Session> session, std::string name);
    boost::asio::awaitable<void> authenticate(std::shared_ptr<Session> session, std::string name);
    boost::asio::awaitable<void> serve(std::shared_ptr<Session> session, std::string name);
    boost::asio::awaitable<void> read_samples(std::shared_ptr<Session> session, db::Device device);
    boost::asio::awaitable<void> write_frames(std::shared_ptr<Session> session);

    // Events.
    boost::asio::awaitable<void> on_enrollment_requested(std::int64_t request_id);
    boost::asio::awaitable<void> on_enrollment_decided(std::int64_t request_id);
    boost::asio::awaitable<void> on_device_event(std::string action, std::string name, int device_id);
    boost::asio::awaitable<void> drain();
    boost::asio::awaitable<void> sweep();

    void apply_enrollment(const db::Enrollment& e);
    void apply_device(const db::Device& d);

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

    boost::asio::local::stream_protocol::acceptor acceptor_;
    /// Registered at construction, before the first fork, so no exit is missed.
    boost::asio::signal_set sigchld_;
    boost::asio::signal_set stop_signals_;
    std::map<std::string, Slot> slots_;
    std::map<pid_t, std::string> pids_;
    bool shutting_down_{false};
    int exit_code_{0};
};

} // namespace pgdemo::controller
