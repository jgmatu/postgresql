#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>

#include <boost/json.hpp>

#include "ProcessTable.hpp"

namespace pgdemo::controller {

/**
 * @brief Notices from the signal thread.
 *
 * `SIGINT`, `SIGTERM` and `SIGCHLD` stay blocked in every thread. One thread
 * `sigwait`s them and stores the notice here. `Fleet::tick()` reads it.
 */
struct SignalState
{
    std::atomic<int> stop{0};       ///< `SIGINT` or `SIGTERM`, else 0
    std::atomic<bool> child{false}; ///< a `SIGCHLD` arrived
    std::atomic<bool> done{false};  ///< the signal thread should return
};

/**
 * @brief Tick and the `LISTEN` queue.
 *
 * Every method except `notify()` runs on the ROS spin thread. `notify()` is
 * the listener thread: it only queues the JSON object. `tick()` drains that
 * queue and hands each notice on: an enrollment or a `device_events` row to
 * the `ProcessTable`, an uplink to that name's `Session`.
 *
 * Samples do not stay here. An online session calls `TelemetryWriter::push`.
 * CSR, proof and session state still write immediately, on the spin thread.
 */
class Fleet
{
public:
    /**
     * @param db       connected; the listener is not running yet
     * @param writer   sample queues; outlives the fleet
     * @param opts     device binary, directories and limits
     * @param downlink publishes on `pgdemo/manager`; called on the spin thread
     * @param signals  written by the signal thread, read from `tick()`
     * @param stop     called once shutdown has finished; ends `rclcpp::spin`
     */
    Fleet(db::DatabaseManager& db, TelemetryWriter& writer, FleetOptions opts, Downlink downlink, SignalState& signals,
          std::function<void()> stop);

    Fleet(const Fleet&) = delete;
    Fleet& operator=(const Fleet&) = delete;

    /// Create the devices directory and take `devices_dir/.controller.lock`.
    /// Throws if another controller on this machine already holds it. Before `boot()`.
    void acquire_lock();

    /// Read devices and open enrollments, drain pending configuration.
    /// Call it after `LISTEN` is active and before `spin`.
    void boot();

    /// One beat of the spin timer: notifications, signals, reaping, deadlines,
    /// heartbeat and maintenance.
    void tick();

    /// Listener callback, any thread: `{"channel", "payload"}`, queued for `tick()`.
    void notify(boost::json::object msg);

    /// A `hello` from a worker. Runs on the spin thread.
    void on_hello(pgdemo_msgs::msg::Hello hello);

    /// One other worker message. Runs on the spin thread.
    void on_uplink(Uplink msg);

    /// 0 after a signal, 1 when the listener connection was lost.
    [[nodiscard]] int exit_code() const noexcept { return exit_code_; }

private:
    [[nodiscard]] SessionDeps deps();

    void db_write(std::string what, std::function<void()> fn);
    void drain_events();
    void handle_notify(const boost::json::object& msg);
    void poll_slots();
    void heartbeat();
    void maintain();
    void shutdown();

    void on_enrollment_requested(std::int64_t request_id);
    void on_enrollment_decided(std::int64_t request_id);
    void on_device_event(std::string action, std::string name, int device_id);
    void drain();
    void sweep();

    db::DatabaseManager& db_;
    TelemetryWriter& writer_;
    ProcessTable table_;
    SignalState& signals_;
    std::function<void()> stop_;

    std::mutex events_mu_;
    std::deque<boost::json::object> events_;
    /// The one uplink message that arrived before the session existed.
    std::map<std::string, Uplink> early_;
    int exit_code_{0};
    std::chrono::steady_clock::time_point next_heartbeat_{};
    std::chrono::steady_clock::time_point next_maintenance_{};
    std::chrono::steady_clock::time_point next_partitions_{};
};

} // namespace pgdemo::controller
