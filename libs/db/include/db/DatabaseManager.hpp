#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include <boost/json.hpp>
#include <pqxx/pqxx>

#include <db/Rows.hpp>
#include <db/Tables.hpp>

namespace pgdemo::db {

/**
 * @brief PostgreSQL access layer and reactive LISTEN/NOTIFY dispatcher.
 *
 * Two independent connections:
 * - `connection_queries_`: every query, transaction and DML statement. Guarded by
 *   `conn_mutex_`.
 * - `connection_listener_`: only `LISTEN` and the blocking wait for notifications.
 *
 * ## Listener lifecycle
 *
 * 1. **Registration** (sequential, before the thread starts): call
 *    `register_listen_async(channel, callback)` once per channel. Several channels
 *    share the single listener socket; libpqxx dispatches each notification to the
 *    handler of its channel.
 * 2. **Activation**: `run_listener_loop()` starts one `std::jthread` that blocks in the
 *    patched `pqxx::connection::wait_notification()` (`poll(-1)`, no periodic wakeups).
 * 3. **Shutdown**: `disconnect()` (or the destructor) makes the listener socket
 *    unusable, so `wait_notification()` throws `pqxx::broken_connection`; the thread
 *    exits and is joined before both connections are closed.
 *
 * libpqxx keeps its handler table in the connection object and is not thread-safe.
 * Registering a channel after `run_listener_loop()` throws `std::logic_error`.
 *
 * Callbacks run on the listener thread. They must be short and must not block; they
 * may call the query methods of this class (the listener never holds `conn_mutex_`).
 * An exception thrown by a callback is logged and does not stop the listener.
 */
class DatabaseManager
{
public:
    /// Called with `{"channel": "<name>", "payload": <parsed JSON or raw string>}`.
    using NotificationCallback = std::function<void(boost::json::object)>;

    /**
     * @param conninfo libpq connection string. Empty means "use PGHOST, PGPORT,
     *        PGDATABASE, PGUSER, PGPASSWORD from the environment".
     * @param application_name reported in `pg_stat_activity`.
     */
    explicit DatabaseManager(std::string conninfo, std::string application_name = "pgdemo");

    /// Stops the listener thread (if any), joins it, and closes both connections.
    ~DatabaseManager();

    DatabaseManager(const DatabaseManager&) = delete;
    DatabaseManager& operator=(const DatabaseManager&) = delete;

    /// Opens the query and listener connections. @throws pqxx::broken_connection
    void connect();

    /// Ends the listener loop, joins it, and closes both connections. Idempotent.
    void disconnect();

    // ---------------------------------------------------------------- listener

    /**
     * @brief Subscribe `callback` to `channel`. Registration phase only.
     *
     * Runs `LISTEN "<channel>"` on the listener connection, stores the callback in
     * `callbacks_` and installs the libpqxx handler for the channel.
     *
     * @throws std::logic_error if the listener loop is already running.
     * @throws std::runtime_error if `connect()` has not been called.
     */
    void register_listen_async(const std::string& channel, NotificationCallback callback);

    /// Starts the listener thread. A second call is a no-op.
    void run_listener_loop();

    /// Blocks until the listener thread has finished. Shutdown phase only.
    void join();

    /// True while the listener thread is inside its wait loop.
    [[nodiscard]] bool listener_alive() const noexcept { return listener_alive_.load(); }

    // ----------------------------------------------------------------- generic

    /// Server version, uptime, backend PIDs, libpqxx version, listener state.
    boost::json::object get_sanity_info();

    /**
     * @brief Run `fn(pqxx::work&)` in one transaction under `SET LOCAL ROLE`.
     *
     * The role reverts automatically when the transaction ends, so a request can
     * never leak its privileges into the next one.
     */
    template <typename Fn>
    auto with_role(DbRole role, Fn&& fn) -> std::invoke_result_t<Fn, pqxx::work&>
    {
        std::lock_guard lock(conn_mutex_);
        pqxx::work txn(queries());
        txn.exec(std::string("SET LOCAL ROLE ") + std::string(pg_role_name(role))).no_rows();
        if constexpr (std::is_void_v<std::invoke_result_t<Fn, pqxx::work&>>)
        {
            std::forward<Fn>(fn)(txn);
            txn.commit();
        }
        else
        {
            auto result = std::forward<Fn>(fn)(txn);
            txn.commit();
            return result;
        }
    }

    // -------------------------------------------------------------- data plane

    /// UPSERT the live row and append it to the history, in one transaction.
    void upsert_dummy_with_history(DummyTable table, const DummySample& sample);

    /// `[{"item_id","value","seq","updated_at"}, ...]`
    boost::json::value fetch_dummy(DbRole role, DummyTable table);

    /**
     * @brief History rows in `[from, to)`, newest first. Each row carries the name
     * of the partition it was read from. NULL bounds default to the last hour.
     */
    boost::json::value fetch_history(DbRole role, DummyTable table,
                                     const std::optional<std::string>& from,
                                     const std::optional<std::string>& to,
                                     std::optional<int> item_id, int limit);

    /// Creates missing monthly partitions; returns the names it created.
    std::vector<std::string> ensure_history_partitions(DummyTable table, int months_ahead = 2);

    // ----------------------------------------------------------- control plane

    /// `nextval('request_id_seq')`: the single, cross-process ID authority.
    std::int64_t generate_request_id(DbRole role);

    /// INSERT the desired configuration and its audit row, in one transaction.
    /// The trigger fires `config_requested` on commit.
    void add_pending_config(DbRole role, const ConfigRequest& request);

    /// Applied configuration of every target (controller start-up).
    std::vector<AppliedConfig> fetch_applied_config();

    /**
     * @brief Process every PENDING request, oldest first.
     *
     * Each request is locked with `FOR UPDATE SKIP LOCKED`, so several controllers
     * may drain the same queue. Requests older than `max_age` are marked TIMEOUT.
     * Otherwise `decide` validates the request:
     * - success: UPSERT `dummy_config` + history + status SUCCESS (fires `config_events`)
     * - failure: INSERT `dummy_config_errors` + status ERROR (fires `error_events`)
     *
     * `on_applied` runs after the commit, with the configuration now in force.
     */
    DrainStats drain_pending(const std::function<ConfigOutcome(const ConfigRequest&)>& decide,
                             const std::function<void(const AppliedConfig&)>& on_applied,
                             std::chrono::seconds max_age);

    /// `v_dummy_config_sync_status` as a JSON array.
    boost::json::value fetch_sync_status(DbRole role);

    /// One request with its error (if any), or `std::nullopt`.
    std::optional<boost::json::value> fetch_request(DbRole role, std::int64_t request_id);

    /// Applied-configuration audit, newest first.
    boost::json::value fetch_config_history(DbRole role, std::optional<ConfigTarget> target,
                                            int limit);

    // ------------------------------------------------------------------- users

    /// Login lookup through `auth_fetch_user()` (no SET ROLE).
    std::optional<UserLoginRow> fetch_user_for_login(const std::string& username);

    /// `v_users_directory` as a JSON array. Admin only.
    boost::json::value list_users(DbRole role);

    /// @return the new user_id. @throws pqxx::sql_error (23505 when the name exists)
    int create_user(DbRole role, const std::string& username, const std::string& password_hash,
                    DbRole user_role);

    /// @return false when no such user. @throws pqxx::sql_error (last admin: 23514)
    bool delete_user(DbRole role, const std::string& username);

private:
    pqxx::connection& queries();
    static void parser_notify(const pqxx::notification& n, boost::json::object& msg);

    std::string conninfo_;
    std::string application_name_;

    std::unique_ptr<pqxx::connection> connection_queries_;
    std::unique_ptr<pqxx::connection> connection_listener_;

    /// Per-channel callbacks. Written only in the registration phase.
    std::unordered_map<std::string, NotificationCallback> callbacks_;

    std::unique_ptr<std::jthread> listener_thread_;
    std::atomic<bool> listening_{false};
    std::atomic<bool> listener_alive_{false};
    int listener_fd_{-1};
    /// Read once at connect(): the listener connection belongs to its thread afterwards.
    int listener_backend_pid_{0};

    /// Guards both connection pointers and every use of `connection_queries_`.
    mutable std::mutex conn_mutex_;
};

} // namespace pgdemo::db
