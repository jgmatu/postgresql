#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
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
 * `forwardable_pending` and `complete_pending` each take one lease and release it
 * before returning. The listener thread only queues the notification.
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
     * @param channel PostgreSQL notification channel, for example `telemetry_scalar`.
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
     * @brief UPSERT `device_state` and append to `telemetry_scalar`, in one statement.
     *
     * The insert into `telemetry_scalar` fires that channel once, on commit.
     *
     * @param sample device, value and tick
     */
    void write_scalar_sample(const DummySample& sample);

    /**
     * @brief UPSERT a GPS fix into `device_state` and append it to `telemetry_gps`.
     *
     * The point is `ST_Project` of the origin by `radius_m` along `bearing_rad`.
     * The insert into `telemetry_gps` fires that channel once, on commit.
     *
     * @param sample device, origin, radius, bearing and tick
     */
    void write_gps_sample(const TrackSample& sample);

    /**
     * @brief UPSERT a 2D sample into `device_state` and append it to `telemetry_xy`.
     *
     * The column is `point(x, y)`. The insert into `telemetry_xy` fires that
     * channel once, on commit. `value` and `position` are set NULL.
     *
     * @param sample device, coordinates and tick
     */
    void write_xy_sample(const XySample& sample);

    /**
     * @brief UPSERT a 3D sample into `device_state` and append it to `telemetry_xyz`.
     *
     * The column is `ST_MakePoint(x, y, z)` with SRID 0. The insert into
     * `telemetry_xyz` fires that channel once, on commit. `value`, `position`
     * and `xy` are set NULL.
     *
     * @param sample device, coordinates and tick
     */
    void write_xyz_sample(const XyzSample& sample);

    /**
     * @brief Append a batch of samples and refresh `device_state` once per device.
     *
     * One transaction, one pool lease. Every sample is inserted into its history
     * table in one statement per payload, which notifies that channel once.
     * `device_state` is updated with the highest `seq` of that device in the
     * batch and does not notify. Each row sets the other shapes NULL, so
     * exactly one of `value`, `position`, `xy` and `xyz` is present.
     *
     * @param scalar samples for `telemetry_scalar`
     * @param gps    samples for `telemetry_gps`
     * @param xy     samples for `telemetry_xy`
     * @param xyz    samples for `telemetry_xyz`
     */
    void write_telemetry_batch(const std::vector<DummySample>& scalar, const std::vector<TrackSample>& gps,
                               const std::vector<XySample>& xy = {}, const std::vector<XyzSample>& xyz = {});

    /**
     * @brief Read one page of telemetry past the cursor and advance it.
     *
     * One transaction on the pool, as `demo_server`. Locks the cursor row,
     * selects up to `kTelemetryReadPage` rows with a greater `history_id`,
     * and, when that select returns rows, sets the cursor to the last one
     * before committing. An empty page leaves the cursor where it is.
     * The caller publishes the rows after the commit.
     *
     * @param channel `telemetry_scalar`, `telemetry_gps`, `telemetry_xy` or `telemetry_xyz`
     * @return samples in `history_id` order, each with `action` `INSERT`
     * @throws std::invalid_argument when `channel` is not one of the four
     * @throws std::runtime_error when the cursor row is missing
     */
    std::vector<boost::json::object> take_unread_telemetry(std::string_view channel);

    /**
     * @brief Every device with its type and current certificate (controller, no SET ROLE).
     * @return devices ordered by `device_id`, whatever their identity
     */
    std::vector<Device> load_devices();

    /**
     * @brief One device by id (controller, on `device_events`; no SET ROLE).
     * @param device_id identity key
     * @return the device, or `std::nullopt` when it no longer exists
     */
    std::optional<Device> load_device(int device_id);

    /**
     * @brief `v_devices`: every device, its type and its last sample.
     * @param role group used for `SET LOCAL ROLE`
     * @return JSON array ordered by `device_id`; GPS devices carry `lon` and `lat`, scatter devices `x` and `y`, cloud devices `z` as well
     */
    boost::json::value fetch_devices(DbRole role);

    /**
     * @brief One `v_devices` row plus `config` (its `v_device_config_sync_status`
     * row), `certificate` and `process` (its `v_device_identity` row, split).
     * @param role group used for `SET LOCAL ROLE`
     * @param name device name
     * @return the device as a JSON object, or `std::nullopt` when it does not exist
     */
    std::optional<boost::json::value> fetch_device(DbRole role, const std::string& name);

    /**
     * @brief Telemetry of one device in `[from, to)`, newest first. Each row carries
     * the name of the partition it was read from. NULL bounds default to the last hour.
     *
     * The device's type selects `telemetry_scalar`, `telemetry_gps`, `telemetry_xy` or `telemetry_xyz`.
     *
     * @param role  group used for `SET LOCAL ROLE`
     * @param name  device name
     * @param from  inclusive lower bound (`timestamptz` text), or last hour
     * @param to    exclusive upper bound, or now
     * @param limit maximum number of rows
     * @return JSON array of rows, or `std::nullopt` when the device does not exist
     */
    std::optional<boost::json::value> fetch_telemetry(DbRole role, const std::string& name,
                                                      const std::optional<std::string>& from,
                                                      const std::optional<std::string>& to, int limit);

    /**
     * @brief `device_type` as a JSON array (payload and defaults of each type).
     * @param role group used for `SET LOCAL ROLE`
     * @return JSON array ordered by `type_id`
     */
    boost::json::value fetch_device_types(DbRole role);

    /**
     * @brief Remove a device. Certificates, state, configuration, queued requests
     * and session go with it (`ON DELETE CASCADE`); telemetry, audit and CRL rows
     * stay. Fires `device_events`.
     * @param role group used for `SET LOCAL ROLE`; must be admin
     * @param name device name
     * @return false when no such device
     */
    bool delete_device(DbRole role, const std::string& name);

    /**
     * @brief Create the current month's partition and the next `months_ahead`.
     * @param payload      whose telemetry table to maintain
     * @param months_ahead future months to create
     * @return names of the partitions created by this call; empty when all existed
     */
    std::vector<std::string> ensure_history_partitions(Payload payload, int months_ahead = 2);

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

    /// Applied configuration of every initialised device, ordered by `device_id`.
    std::vector<AppliedConfig> fetch_applied_config();

    /**
     * @brief Applied configuration of one device, creating it from the
     * `device_type` defaults (plus an audit row) when the device has none yet.
     *
     * The controller calls this for every device at start-up and for each
     * `device_events` INSERT. A new row fires `config_events`.
     *
     * @param device_id identity key
     * @return the configuration in force, or `std::nullopt` when the device does not exist
     */
    std::optional<AppliedConfig> init_device_config(int device_id);

    /**
     * @brief Expire stale `PENDING` requests and return the ones still waiting.
     *
     * A request older than `max_age` becomes `TIMEOUT` with an `EXPIRED` error
     * (`error_events`) and is not returned. The rows that stay `PENDING` are not
     * modified: the worker has not accepted them yet.
     *
     * @param max_age requests that waited longer become TIMEOUT
     * @return the live requests, oldest first, and how many were expired
     */
    ForwardBatch forwardable_pending(std::chrono::seconds max_age);

    /**
     * @brief Record the worker's answer for one request that is still `PENDING`.
     *
     * Success upserts `device_config` and its history (`config_events`). Failure
     * inserts `device_config_errors` (`error_events`). A request that is no longer
     * `PENDING`, or `request_id` 0, is left untouched.
     *
     * @param request_id the queued request
     * @param outcome    what the worker reported
     * @return true when a `PENDING` row was completed
     */
    bool complete_pending(std::int64_t request_id, const ConfigOutcome& outcome);

    /**
     * @brief `v_device_config_sync_status`: applied vs desired state per device.
     * @param role group used for `SET LOCAL ROLE`
     * @return JSON array, one object per device, ordered by `device_id`
     */
    boost::json::value fetch_sync_status(DbRole role);

    /**
     * @brief The `v_device_config_sync_status` row of one device.
     * @param role group used for `SET LOCAL ROLE`
     * @param name device name
     * @return the row as a JSON object, or `std::nullopt` when the device does not exist
     */
    std::optional<boost::json::value> fetch_device_sync(DbRole role, const std::string& name);

    /**
     * @brief One request with its error, if any.
     * @param role       group used for `SET LOCAL ROLE`
     * @param request_id id returned by `generate_request_id()`
     * @return the request as a JSON object, or `std::nullopt` when it does not exist
     */
    std::optional<boost::json::value> fetch_request(DbRole role, std::int64_t request_id);

    /**
     * @brief Applied-configuration audit, newest first.
     * Rows of removed devices stay; their `device` is null.
     *
     * @param role   group used for `SET LOCAL ROLE`
     * @param device only this device name, or every device
     * @param limit  maximum number of rows
     * @return JSON array of `device_config_history` rows
     */
    boost::json::value fetch_config_history(DbRole role, const std::optional<std::string>& device,
                                            int limit);

    // -------------------------------------------------------------- enrollment

    /**
     * @brief Queue the enrollment of a new device. Nothing is created yet: the
     * device appears when its certificate is issued. Fires `enrollment_requested`.
     * @param role         group used for `SET LOCAL ROLE`; must be admin
     * @param request_id   from `generate_request_id()`, so a waiter can be registered first
     * @param name         device name, and the CN of its certificate
     * @param type_id      existing `device_type.type_id`
     * @param requested_by username of the admin
     * @return false when the type does not exist (nothing queued)
     * @throws pqxx::sql_error SQLSTATE 23505 when the device exists or is already enrolling
     */
    bool enqueue_enrollment(DbRole role, std::int64_t request_id, const std::string& name,
                            const std::string& type_id, const std::string& requested_by);

    /**
     * @brief Queue a key rotation of an existing device. Fires `enrollment_requested`.
     * @param role         group used for `SET LOCAL ROLE`; must be admin
     * @param request_id   from `generate_request_id()`
     * @param name         device name
     * @param requested_by username of the admin
     * @return false when the device does not exist (nothing queued)
     * @throws pqxx::sql_error SQLSTATE 23505 when a rotation is already open
     */
    bool enqueue_rotation(DbRole role, std::int64_t request_id, const std::string& name,
                          const std::string& requested_by);

    /**
     * @brief One `device_enrollment` row (without the CSR) as JSON.
     * @param role       group used for `SET LOCAL ROLE`
     * @param request_id the request
     * @return the row, or `std::nullopt` when it does not exist
     */
    std::optional<boost::json::value> fetch_enrollment(DbRole role, std::int64_t request_id);

    /**
     * @brief Enrollment and rotation requests, newest first, without CSRs.
     * @param role   group used for `SET LOCAL ROLE`
     * @param device only this device name, or every request
     * @param limit  maximum number of rows
     * @return JSON array
     */
    boost::json::value fetch_enrollments(DbRole role, const std::optional<std::string>& device, int limit);

    /**
     * @brief Server session: the request whose CSR is to be signed.
     * @param request_id the request
     * @return the row, or `std::nullopt` when it does not exist
     */
    std::optional<Enrollment> fetch_csr(std::int64_t request_id);

    /**
     * @brief Server session: record a signed certificate through
     * `issue_device_certificate()`. Fires `enrollment_events` (and `device_events`).
     * @param request_id     enrollment in CSR_SUBMITTED
     * @param ca_fingerprint SHA-256 of the issuing CA, which must be an active `root_ca`
     * @param cert           the certificate
     * @return the device and the new certificate
     * @throws pqxx::sql_error when the request is not open, the CN differs or the CA is unknown
     */
    IssueResult issue_certificate(std::int64_t request_id, const std::string& ca_fingerprint,
                                  const IssuedCertificate& cert);

    /**
     * @brief Server session: refuse a CSR. Fires `enrollment_events`.
     * @return false when the request was no longer open
     */
    bool reject_enrollment(std::int64_t request_id, const std::string& code, const std::string& detail);

    /// Server session: SHA-256 fingerprints of the active root CAs.
    std::vector<std::string> active_root_fingerprints();

    /// Server session: requests whose CSR waits for a signature, oldest first.
    std::vector<std::int64_t> submitted_csr_ids();

    /// Controller: every PENDING request, oldest first.
    std::vector<Enrollment> pending_enrollments();

    /// Controller: one request, or `std::nullopt` when it does not exist.
    std::optional<Enrollment> load_enrollment(std::int64_t request_id);

    /**
     * @brief Controller: store the CSR a device process produced (PENDING to
     * CSR_SUBMITTED). Fires `enrollment_csr`.
     * @return false when the request was no longer PENDING
     */
    bool submit_csr(std::int64_t request_id, const std::string& csr_pem);

    /**
     * @brief Controller: give up on an open request (device process failed).
     * Fires `enrollment_events`.
     * @return false when the request was no longer open
     */
    bool fail_enrollment(std::int64_t request_id, const std::string& code, const std::string& detail);

    /**
     * @brief Controller: mark requests open for longer than `max_age` as EXPIRED.
     * @return the expired request ids
     */
    std::vector<std::int64_t> expire_enrollments(std::chrono::seconds max_age);

    /// Controller: status of a request and, once ISSUED, its certificate.
    std::optional<EnrollmentOutcome> enrollment_outcome(std::int64_t request_id);

    // ---------------------------------------------------------------- identity

    /// Controller: PEM of every active root CA, the trust anchors of `CertVerifier`.
    std::vector<std::string> load_root_pems();

    /// Controller: serial of every revoked certificate.
    std::set<std::string> load_revoked_serials();

    /**
     * @brief Every certificate a device has had, newest first, with `current`
     * and its revocation.
     * @param role group used for `SET LOCAL ROLE`
     * @param name device name
     * @return JSON array, or `std::nullopt` when the device does not exist
     */
    std::optional<boost::json::value> fetch_certificates(DbRole role, const std::string& name);

    /**
     * @brief Revoke the current certificate of a device. A certificateHold may
     * later be lifted or made permanent; any other reason is final. Fires `device_events`.
     * @param role       group used for `SET LOCAL ROLE`; must be admin
     * @param name       device name
     * @param reason     RFC 5280 reason name (see the `crl.reason` check)
     * @param revoked_by username of the admin
     */
    RevokeResult revoke_certificate(DbRole role, const std::string& name, const std::string& reason,
                                    const std::string& revoked_by);

    /**
     * @brief Lift the hold on the current certificate of a device. Fires `device_events`.
     * @param role group used for `SET LOCAL ROLE`; must be admin
     * @param name device name
     */
    UnrevokeResult unrevoke_certificate(DbRole role, const std::string& name);

    /**
     * @brief Delete one certificate of a device. Deleting the current one
     * deletes the device (`device_current_cert_fk ON DELETE CASCADE`).
     * @param role   group used for `SET LOCAL ROLE`; must be admin
     * @param name   device name
     * @param serial certificate serial, lowercase hex
     */
    DeleteCertResult delete_certificate(DbRole role, const std::string& name, const std::string& serial);

    /// `root_ca` rows (with PEM) as a JSON array.
    boost::json::value fetch_root_cas(DbRole role);

    /// `crl` rows (without PEM) as a JSON array, newest first.
    boost::json::value fetch_crl_entries(DbRole role);

    /// Every revoked certificate with its reason, for `CertificateAuthority::crl_pem()`.
    std::vector<RevokedPem> load_crl(DbRole role);

    /// The manager's signature over `manager_revocation`.
    struct ManagerCrlRecord
    {
        std::string payload;       ///< canonical serial list
        std::string signature_hex; ///< raw ECDSA-SHA384 signature, lower-case hex
    };

    /**
     * @brief Insert the factory root into `root_ca` when that name is absent.
     * The private key is not a parameter and is not stored.
     */
    void ensure_factory_root(const std::string& subject, const std::string& fingerprint, const std::string& pem,
                             const std::string& not_before, const std::string& not_after);

    /// True when `manager_certificate` already holds the factory-signed certificate.
    bool manager_certificate_present();

    /// PEM of the manager certificate, or empty when the row does not exist yet.
    std::optional<std::string> load_manager_certificate_pem();

    /// Insert the factory-signed manager certificate. A later call keeps the first row.
    void register_manager_certificate(const std::string& ca_fingerprint, const std::string& serial,
                                      const std::string& fingerprint, const std::string& subject_cn,
                                      const std::string& not_before, const std::string& not_after,
                                      const std::string& pem);

    /// Replace the manager's signature over its revocation list.
    void replace_manager_crl(const std::string& payload, const std::string& signature_hex);

    /// Record one worker serial on the manager's list. The signature is replaced separately.
    void add_manager_revocation(const std::string& serial, const std::string& fingerprint,
                                const std::string& device_name, const std::string& pem);

    /// Serials in `manager_revocation`.
    std::set<std::string> load_manager_revoked_serials();

    /// The stored signature, or empty when the manager has not signed the list yet.
    std::optional<ManagerCrlRecord> load_manager_crl();

    /**
     * @brief Store a factory-signed worker certificate beside the current one.
     * `device.cert_id` is left as it is.
     * @return false when no device has that name
     */
    bool register_factory_worker(const std::string& subject_cn, const std::string& ca_fingerprint,
                                 const std::string& serial, const std::string& fingerprint,
                                 const std::string& not_before, const std::string& not_after, const std::string& pem);

    /// Factory root, manager certificate and factory-issued worker certificates, for the panel.
    boost::json::value fetch_origin(DbRole role);

    // ----------------------------------------------------------------- session

    /**
     * @brief Controller: record the state of a device process. A change of state
     * fires `device_events`. A device deleted meanwhile is ignored.
     * @param device_id the device
     * @param state     STARTING, ONLINE, OFFLINE, UNMANAGED, AUTH_FAILED or DISABLED
     * @param pid       process id, when there is one
     * @param error     last error, if any
     */
    void upsert_session(int device_id, const std::string& state, std::optional<int> pid,
                        const std::optional<std::string>& error = std::nullopt);

    /// Controller: set `last_seen` of these devices to now.
    void touch_sessions(const std::vector<int>& device_ids);

    /// Controller: every STARTING or ONLINE session becomes OFFLINE (start-up and shutdown).
    void reset_sessions();

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
