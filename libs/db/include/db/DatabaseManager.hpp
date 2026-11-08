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

#include <db/ConnectionPool.hpp>
#include <db/Rows.hpp>
#include <db/Tables.hpp>

namespace pgdemo::db {

/**
 * @brief PostgreSQL access layer and reactive LISTEN/NOTIFY dispatcher.
 *
 * Query work goes through `pool_`, a fixed set of connections opened by
 * `connect()`. `pqxx::connection` is not thread-safe, so each checkout is
 * exclusive to one thread. The pool mutex guards only the free list; callers
 * block in `acquire()` when every connection is in use.
 *
 * `connection_listener_` stays outside the pool. `LISTEN` state and the
 * blocking wait belong to that single session.
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
 *    exits and is joined before the listener connection and the query pool are closed.
 *
 * libpqxx keeps its handler table in the connection object and is not thread-safe.
 * Registering a channel after `run_listener_loop()` throws `std::logic_error`.
 *
 * Callbacks run on the listener thread. They must be short. They may call the
 * query methods of this class: the listener thread does not already hold a pool
 * lease, so the call checks one out. An exception thrown by a callback is logged
 * and does not stop the listener.
 *
 * `drain_pending` keeps its lease across `decide` and `on_applied`. Those
 * callbacks must not call back into this object on the same thread.
 */
class DatabaseManager
{
public:
    /// Query connections opened by `connect()` when the caller does not pass a size.
    static constexpr std::size_t kDefaultPoolSize = 8;

    /// Called with `{"channel": "<name>", "payload": <parsed JSON or raw string>}`.
    using NotificationCallback = std::function<void(boost::json::object)>;

    /**
     * @param conninfo libpq connection string. Empty means "use PGHOST, PGPORT,
     *        PGDATABASE, PGUSER, PGPASSWORD from the environment".
     * @param application_name reported in `pg_stat_activity`.
     * @param pool_size query connections opened by `connect()`. At least 1.
     *
     * @throws std::invalid_argument if `pool_size` is 0.
     */
    explicit DatabaseManager(std::string conninfo, std::string application_name = "pgdemo",
                             std::size_t pool_size = kDefaultPoolSize);

    /// Stops the listener thread (if any), joins it, and closes the listener and the pool.
    ~DatabaseManager();

    DatabaseManager(const DatabaseManager&) = delete;
    DatabaseManager& operator=(const DatabaseManager&) = delete;

    /// Opens the query and listener connections. @throws pqxx::broken_connection
    void connect();

    /// Ends the listener loop, joins it, and closes the listener and the pool. Idempotent.
    void disconnect();

    // ---------------------------------------------------------------- listener

    /**
     * @brief Subscribe `callback` to `channel`. Registration phase only.
     *
     * Runs `LISTEN "<channel>"` on the listener connection, stores the callback in
     * `callbacks_` and installs the libpqxx handler for the channel.
     *
     * Every channel is registered here, one call after another, and only then may
     * `run_listener_loop()` start. libpqxx keeps its handler table inside the
     * connection object, and that table is not thread-safe. Registering another
     * channel while the listener thread is blocked in `wait_notification()` races
     * with that wait and can deadlock or corrupt the connection. This method
     * therefore refuses the call once the loop is running.
     *
     * @param channel PostgreSQL notification channel, for example `dummy1_events`.
     * @param callback invoked on the listener thread with the decoded payload.
     * @throws std::logic_error if the listener loop is already running.
     * @throws std::runtime_error if `connect()` has not been called.
     */
    void register_listen_async(const std::string& channel, NotificationCallback callback);

    /**
     * @brief Start the listener thread. Activation phase; a second call is a no-op.
     *
     * The thread blocks in `wait_notification()` and dispatches each notification
     * to the handler installed for its channel.
     *
     * Call this only after every `register_listen_async()`. Those calls run on the
     * caller thread and mutate the same libpqxx connection this thread is about to
     * own. libpqxx is not thread-safe, so the two phases stay strictly ordered:
     * register every channel first, then start the thread. A registration attempted
     * afterwards is rejected; it is not applied concurrently with the wait.
     *
     * @throws std::runtime_error if `connect()` has not been called.
     */
    void run_listener_loop();

    /// Blocks until the listener thread has finished. Shutdown phase only.
    void join();

    /// True while the listener thread is inside its wait loop.
    [[nodiscard]] bool listener_alive() const noexcept { return listener_alive_.load(); }

    // ----------------------------------------------------------------- generic

    /// Server version, uptime, backend PIDs, pool occupancy, libpqxx version, listener state.
    boost::json::object get_sanity_info();

    /**
     * @brief Run `fn(pqxx::work&)` in one transaction under `SET LOCAL ROLE`.
     *
     * The role reverts automatically when the transaction ends, so a request can
     * never leak its privileges into the next one.
     *
     * @param role PostgreSQL group to switch into for this transaction
     * @param fn   work to run; an exception rolls the transaction back
     * @return whatever `fn` returns
     */
    template <typename Fn>
    auto with_role(DbRole role, Fn&& fn) -> std::invoke_result_t<Fn, pqxx::work&>
    {
        auto lease = pool_.acquire();
        pqxx::work txn(lease.get());
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

    /**
     * @brief UPSERT the live row and append it to the history, in one transaction.
     * @param table  `dummy1` or `dummy2`; the trigger fires its channel on commit
     * @param sample row to write
     */
    void upsert_dummy_with_history(DummyTable table, const DummySample& sample);

    /**
     * @brief Live rows of `table`, ordered by `item_id`.
     * @param role  group used for `SET LOCAL ROLE`
     * @param table table to read
     * @return `[{"item_id","value","seq","updated_at"}, ...]`
     */
    boost::json::value fetch_dummy(DbRole role, DummyTable table);

    /**
     * @brief History rows in `[from, to)`, newest first. Each row carries the name
     * of the partition it was read from. NULL bounds default to the last hour.
     *
     * @param role    group used for `SET LOCAL ROLE`
     * @param table   whose history table to read
     * @param from    inclusive lower bound (`timestamptz` text), or last hour
     * @param to      exclusive upper bound, or now
     * @param item_id only this item, or every item
     * @param limit   maximum number of rows
     * @return JSON array of history rows
     */
    boost::json::value fetch_history(DbRole role, DummyTable table,
                                     const std::optional<std::string>& from,
                                     const std::optional<std::string>& to,
                                     std::optional<int> item_id, int limit);

    /**
     * @brief Create the current month's partition and the next `months_ahead`.
     * @param table        whose history table to maintain
     * @param months_ahead future months to create
     * @return names of the partitions created by this call; empty when all existed
     */
    std::vector<std::string> ensure_history_partitions(DummyTable table, int months_ahead = 2);

    // ----------------------------------------------------------- control plane

    /**
     * @brief `nextval('request_id_seq')`: the single, cross-process ID authority.
     * @param role group used for `SET LOCAL ROLE`
     * @return a fresh request id
     */
    std::int64_t generate_request_id(DbRole role);

    /**
     * @brief INSERT the desired configuration and its audit row, in one transaction.
     *
     * The trigger fires `config_requested` on commit.
     *
     * @param role    group used for `SET LOCAL ROLE`; only admin may insert
     * @param request desired configuration, with an id from `generate_request_id()`
     */
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
     * One pooled connection is held for the whole drain. `decide` and `on_applied`
     * must not call back into this `DatabaseManager` on the same thread.
     *
     * @param decide     validation of one request
     * @param on_applied called after each successful commit
     * @param max_age    requests that waited longer become TIMEOUT without `decide`
     * @return how many requests were applied, rejected and expired
     */
    DrainStats drain_pending(const std::function<ConfigOutcome(const ConfigRequest&)>& decide,
                             const std::function<void(const AppliedConfig&)>& on_applied,
                             std::chrono::seconds max_age);

    /**
     * @brief `v_dummy_config_sync_status`: applied vs desired state per target.
     * @param role group used for `SET LOCAL ROLE`
     * @return JSON array, one object per target
     */
    boost::json::value fetch_sync_status(DbRole role);

    /**
     * @brief One request with its error, if any.
     * @param role       group used for `SET LOCAL ROLE`
     * @param request_id id returned by `generate_request_id()`
     * @return the request as a JSON object, or `std::nullopt` when it does not exist
     */
    std::optional<boost::json::value> fetch_request(DbRole role, std::int64_t request_id);

    /**
     * @brief Applied-configuration audit, newest first.
     * @param role   group used for `SET LOCAL ROLE`
     * @param target only this target, or both
     * @param limit  maximum number of rows
     * @return JSON array of `dummy_config_history` rows
     */
    boost::json::value fetch_config_history(DbRole role, std::optional<ConfigTarget> target,
                                            int limit);

    // ------------------------------------------------------------------- users

    /**
     * @brief Login lookup through `auth_fetch_user()` (no SET ROLE).
     * @param username login name
     * @return hash and role, or `std::nullopt` when the user does not exist
     */
    std::optional<UserLoginRow> fetch_user_for_login(const std::string& username);

    /**
     * @brief `v_users_directory` as a JSON array (no hashes). Admin only.
     * @param role group used for `SET LOCAL ROLE`
     * @return JSON array of users
     */
    boost::json::value list_users(DbRole role);

    /**
     * @brief Insert a user.
     * @param role          group used for `SET LOCAL ROLE`; must be admin
     * @param username      login name
     * @param password_hash Argon2id PHC string
     * @param user_role     role of the new user
     * @return the new user_id
     * @throws pqxx::sql_error SQLSTATE 23505 when the name exists
     */
    int create_user(DbRole role, const std::string& username, const std::string& password_hash,
                    DbRole user_role);

    /**
     * @brief Delete a user.
     * @param role     group used for `SET LOCAL ROLE`; must be admin
     * @param username login name
     * @return false when no such user
     * @throws pqxx::sql_error SQLSTATE 23514 when it is the last admin
     */
    bool delete_user(DbRole role, const std::string& username);

private:
    static void parser_notify(const pqxx::notification& n, boost::json::object& msg);

    std::string conninfo_;
    std::string application_name_;
    std::size_t pool_size_;

    ConnectionPool pool_;
    std::unique_ptr<pqxx::connection> connection_listener_;

    /// Per-channel callbacks. Written only in the registration phase.
    std::unordered_map<std::string, NotificationCallback> callbacks_;

    std::unique_ptr<std::jthread> listener_thread_;
    std::atomic<bool> listening_{false};
    std::atomic<bool> listener_alive_{false};
    int listener_fd_{-1};
    /// Read once at connect(): the listener connection belongs to its thread afterwards.
    int listener_backend_pid_{0};

    /// Covers listener setup and teardown. The listener thread does not hold it
    /// across `wait_notification()`.
    std::mutex listener_mutex_;
};

} // namespace pgdemo::db
