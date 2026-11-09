#include <db/DatabaseManager.hpp>

#include <sys/socket.h>

#include <format>

#include <log/Log.hpp>

namespace pgdemo::db {

namespace {

constexpr std::string_view kLog = "db";

std::string build_conninfo(const std::string& conninfo, const std::string& application_name)
{
    // libpq reads PG* environment variables for every key that is not given.
    std::string out = conninfo;
    if (out.find("application_name") == std::string::npos)
    {
        if (!out.empty()) out += ' ';
        out += "application_name=" + application_name;
    }
    return out;
}

boost::json::value parse_json_field(const pqxx::field& f)
{
    if (f.is_null()) return nullptr;
    return boost::json::parse(f.view());
}

/// Row `(device_id, name, type_id, payload)`.
Device device_from_row(const pqxx::row& row)
{
    return Device{row[0].as<int>(), row[1].as<std::string>(), row[2].as<std::string>(),
                  parse_payload(row[3].view()).value_or(Payload::Scalar)};
}

AppliedConfig applied_from_row(const pqxx::row& row)
{
    AppliedConfig c;
    c.device_id = row[0].as<int>();
    c.device = row[1].as<std::string>();
    c.rate_ms = row[2].as<int>();
    c.amplitude = row[3].as<double>();
    c.is_active = row[4].as<bool>();
    if (!row[5].is_null()) c.request_id = row[5].as<std::int64_t>();
    return c;
}

} // namespace

DatabaseManager::DatabaseManager(std::string conninfo, std::string application_name, std::size_t pool_size)
    : conninfo_(std::move(conninfo))
    , application_name_(std::move(application_name))
    , pool_size_(pool_size)
{
    if (pool_size_ == 0) throw std::invalid_argument("DatabaseManager pool_size must be at least 1");
}

DatabaseManager::~DatabaseManager()
{
    // 1. disconnect() ends the listener loop (broken_connection), joins the
    //    thread and closes the listener connection and the query pool.
    disconnect();

    // 2. Nothing left to stop, but keep the jthread contract explicit.
    if (listener_thread_ && listener_thread_->joinable())
    {
        listener_thread_->request_stop();
        listener_thread_->join();
    }

    connection_listener_.reset();
}

void DatabaseManager::connect()
{
    // close() waits for outstanding leases and must not run under listener_mutex_.
    pool_.close();
    {
        std::lock_guard lock(listener_mutex_);
        connection_listener_.reset();
        listener_fd_ = -1;
    }

    try
    {
        auto listener = std::make_unique<pqxx::connection>(
            build_conninfo(conninfo_, application_name_ + "-listener"));
        pool_.open(build_conninfo(conninfo_, application_name_), pool_size_);
        const int listener_pid = listener->backendpid();

        {
            auto lease = pool_.acquire();
            std::string pid_list;
            for (const int pid : pool_.backend_pids())
            {
                if (!pid_list.empty()) pid_list += ", ";
                pid_list += std::to_string(pid);
            }
            log::info(kLog, std::format(
                                "connected to database '{}' as '{}' (pool {}, query pids [{}], listener pid {})",
                                lease.get().dbname(), lease.get().username(), pool_.size(), pid_list, listener_pid));
        }

        std::lock_guard lock(listener_mutex_);
        connection_listener_ = std::move(listener);
        listener_backend_pid_ = listener_pid;
    }
    catch (const std::exception& e)
    {
        pool_.close();
        std::lock_guard lock(listener_mutex_);
        connection_listener_.reset();
        log::error(kLog, std::string("connection failed: ") + e.what());
        throw;
    }
}

void DatabaseManager::disconnect()
{
    // Wake the listener. shutdown(2) on its socket makes poll() return and the
    // next read hit EOF, so wait_notification() throws broken_connection. Only
    // the socket is touched here; the pqxx::connection object stays owned by
    // the listener thread until it has exited.
    if (listener_thread_ && listener_thread_->joinable())
    {
        listener_thread_->request_stop();
        if (listener_fd_ >= 0)
        {
            ::shutdown(listener_fd_, SHUT_RDWR);
        }
        listener_thread_->join();
    }

    // Join finished, so no listener callback still holds a lease.
    pool_.close();

    std::lock_guard lock(listener_mutex_);
    try
    {
        if (connection_listener_ && connection_listener_->is_open()) connection_listener_->close();
    }
    catch (const std::exception& e)
    {
        log::warn(kLog, std::string("error during disconnect: ") + e.what());
    }
    listener_fd_ = -1;
}

// ---------------------------------------------------------------------------
// Listener
// ---------------------------------------------------------------------------

void DatabaseManager::register_listen_async(const std::string& channel, NotificationCallback callback)
{
    std::lock_guard lock(listener_mutex_);

    if (listening_.load())
    {
        throw std::logic_error("register_listen_async('" + channel +
                               "') after run_listener_loop(): register every channel first");
    }
    if (!connection_listener_ || !connection_listener_->is_open())
    {
        throw std::runtime_error("register_listen_async: database connection is not established");
    }

    log::debug(kLog, "register channel " + channel);

    pqxx::nontransaction nt(*connection_listener_);
    nt.exec("LISTEN " + nt.quote_name(channel)).no_rows();
    nt.commit();

    callbacks_[channel] = std::move(callback);

    connection_listener_->listen(channel, [channel, this](const pqxx::notification& n) {
        NotificationCallback handler;
        if (auto it = callbacks_.find(channel); it != callbacks_.end())
        {
            handler = it->second;
        }
        if (!handler) return;

        boost::json::object msg;
        parser_notify(n, msg);
        try
        {
            handler(std::move(msg));
        }
        catch (const std::exception& e)
        {
            log::error(kLog, "callback for '" + channel + "' threw: " + e.what());
        }
    });
}

void DatabaseManager::run_listener_loop()
{
    if (listener_thread_) return;

    {
        std::lock_guard lock(listener_mutex_);
        if (!connection_listener_ || !connection_listener_->is_open())
        {
            throw std::runtime_error("run_listener_loop: database connection is not established");
        }
        listener_fd_ = connection_listener_->sock();
        listening_.store(true);
    }

    log::info(kLog, std::format("listener running on {} channel(s)", callbacks_.size()));

    listener_alive_.store(true);
    listener_thread_ = std::make_unique<std::jthread>([this](std::stop_token st) {
        try
        {
            while (!st.stop_requested())
            {
                // Blocks in poll(-1) on the listener socket until PostgreSQL
                // sends something (patched libpqxx, see patches/README.md).
                // Every pending notification is dispatched to its channel
                // handler before the call returns.
                connection_listener_->wait_notification();
            }
        }
        catch (const pqxx::broken_connection& e)
        {
            // Expected on disconnect(), or when the server drops the session.
            log::info(kLog, std::string("listener exiting (connection closed: ") + e.what() + ")");
        }
        catch (const std::exception& e)
        {
            log::error(kLog, std::string("listener stopped by unexpected error: ") + e.what());
        }
        listener_alive_.store(false);
    });
}

void DatabaseManager::join()
{
    if (listener_thread_ && listener_thread_->joinable())
    {
        listener_thread_->join();
    }
}

void DatabaseManager::parser_notify(const pqxx::notification& n, boost::json::object& msg)
{
    msg["channel"] = std::string_view(n.channel);
    try
    {
        msg["payload"] = boost::json::parse(std::string_view(n.payload));
    }
    catch (const std::exception&)
    {
        msg["payload"] = std::string_view(n.payload);
    }
}

// ---------------------------------------------------------------------------
// Generic
// ---------------------------------------------------------------------------

boost::json::object DatabaseManager::get_sanity_info()
{
    auto lease = pool_.acquire();
    boost::json::object info;
    pqxx::nontransaction nt(lease.get());
    const auto row = nt.exec("SELECT version(), (now() - pg_postmaster_start_time())::text, "
                             "current_database(), current_user")
                         .one_row();
    info["postgres_version"] = row[0].view();
    info["uptime"] = row[1].view();
    info["database"] = row[2].view();
    info["user"] = row[3].view();
    info["queries_backend_pid"] = lease.backend_pid();
    info["pool_size"] = static_cast<std::int64_t>(pool_.size());
    info["pool_in_use"] = static_cast<std::int64_t>(pool_.in_use());
    boost::json::array pids;
    for (const int pid : pool_.backend_pids()) pids.emplace_back(static_cast<std::int64_t>(pid));
    info["query_backend_pids"] = std::move(pids);
    info["listener_backend_pid"] = listener_backend_pid_;
    info["pqxx_version"] = PQXX_VERSION;
    info["listener_alive"] = listener_alive_.load();
    info["listen_channels"] = static_cast<std::int64_t>(callbacks_.size());
    return info;
}

// ---------------------------------------------------------------------------
// Data plane
// ---------------------------------------------------------------------------

void DatabaseManager::write_scalar_sample(const DummySample& sample)
{
    // One statement: the CTE writes the snapshot, the outer INSERT appends the
    // same values to the partitioned telemetry. Both commit (and notify) together.
    auto lease = pool_.acquire();
    pqxx::work txn(lease.get());
    txn.exec("WITH up AS ("
             "  INSERT INTO device_state (device_id, value, seq, updated_at)"
             "  VALUES ($1, $2, $3, clock_timestamp())"
             "  ON CONFLICT (device_id) DO UPDATE SET value = EXCLUDED.value, seq = EXCLUDED.seq,"
             "    updated_at = EXCLUDED.updated_at"
             "  RETURNING device_id, value, seq, updated_at)"
             " INSERT INTO telemetry_scalar (device_id, value, seq, recorded_at)"
             " SELECT device_id, value, seq, updated_at FROM up",
             pqxx::params{sample.device_id, sample.value, sample.seq})
        .no_rows();
    txn.commit();
}

void DatabaseManager::write_gps_sample(const TrackSample& sample)
{
    // PostGIS builds the point. Distance and azimuth are cast: unbound parameters
    // are unknown, and PostGIS has both ST_Project(geography, float8, float8)
    // and ST_Project(geography, geography, float8).
    auto lease = pool_.acquire();
    pqxx::work txn(lease.get());
    txn.exec("WITH up AS ("
             "  INSERT INTO device_state (device_id, position, seq, updated_at) VALUES ("
             "    $1, ST_Project(ST_SetSRID(ST_MakePoint($2, $3), 4326)::geography,"
             "      $4::double precision, $5::double precision),"
             "    $6, clock_timestamp())"
             "  ON CONFLICT (device_id) DO UPDATE SET position = EXCLUDED.position, seq = EXCLUDED.seq,"
             "    updated_at = EXCLUDED.updated_at"
             "  RETURNING device_id, position, seq, updated_at)"
             " INSERT INTO telemetry_gps (device_id, position, seq, recorded_at)"
             " SELECT device_id, position, seq, updated_at FROM up",
             pqxx::params{sample.device_id, sample.origin_lon, sample.origin_lat, sample.radius_m,
                          sample.bearing_rad, sample.seq})
        .no_rows();
    txn.commit();
}

std::vector<Device> DatabaseManager::load_devices()
{
    auto lease = pool_.acquire();
    pqxx::work txn(lease.get());
    const auto res = txn.exec("SELECT d.device_id, d.name, d.type_id, t.payload"
                              " FROM device d JOIN device_type t USING (type_id)"
                              " ORDER BY d.device_id");
    txn.commit();
    std::vector<Device> out;
    for (const auto& row : res) out.push_back(device_from_row(row));
    return out;
}

std::optional<Device> DatabaseManager::load_device(int device_id)
{
    auto lease = pool_.acquire();
    pqxx::work txn(lease.get());
    const auto res = txn.exec("SELECT d.device_id, d.name, d.type_id, t.payload"
                              " FROM device d JOIN device_type t USING (type_id)"
                              " WHERE d.device_id = $1",
                              pqxx::params{device_id});
    txn.commit();
    if (res.empty()) return std::nullopt;
    return device_from_row(res[0]);
}

boost::json::value DatabaseManager::fetch_devices(DbRole role)
{
    return with_role(role, [](pqxx::work& txn) {
        return parse_json_field(
            txn.exec("SELECT COALESCE(json_agg(v ORDER BY v.device_id), '[]'::json)::text FROM v_devices v")
                .one_field());
    });
}

std::optional<boost::json::value> DatabaseManager::fetch_device(DbRole role, const std::string& name)
{
    return with_role(role, [&](pqxx::work& txn) -> std::optional<boost::json::value> {
        const auto res = txn.exec(
            "SELECT (to_jsonb(v) || jsonb_build_object('config', to_jsonb(s)"
            "        - 'device_id' - 'device' - 'type_id'))::text"
            " FROM v_devices v JOIN v_device_config_sync_status s USING (device_id)"
            " WHERE v.device = $1",
            pqxx::params{name});
        if (res.empty()) return std::nullopt;
        return parse_json_field(res[0][0]);
    });
}

std::optional<boost::json::value> DatabaseManager::fetch_telemetry(DbRole role, const std::string& name,
                                                                   const std::optional<std::string>& from,
                                                                   const std::optional<std::string>& to,
                                                                   int limit)
{
    return with_role(role, [&](pqxx::work& txn) -> std::optional<boost::json::value> {
        const auto dev = txn.exec("SELECT d.device_id, t.payload FROM device d JOIN device_type t USING (type_id)"
                                  " WHERE d.name = $1",
                                  pqxx::params{name});
        if (dev.empty()) return std::nullopt;
        const int device_id = dev[0][0].as<int>();
        const auto payload = parse_payload(dev[0][1].view()).value_or(Payload::Scalar);

        const std::string_view columns =
            payload == Payload::Gps
                ? "history_id, device_id, ST_X(position::geometry) AS lon, ST_Y(position::geometry) AS lat,"
                  " seq, recorded_at, tableoid::regclass::text AS partition"
                : "history_id, device_id, value, seq, recorded_at, tableoid::regclass::text AS partition";
        const std::string sql = std::format(
            "SELECT COALESCE(json_agg(r ORDER BY r.recorded_at DESC), '[]'::json)::text FROM ("
            "  SELECT {}"
            "  FROM {}"
            "  WHERE device_id = $1"
            "    AND recorded_at >= COALESCE($2::timestamptz, now() - interval '1 hour')"
            "    AND recorded_at <  COALESCE($3::timestamptz, now() + interval '1 second')"
            "  ORDER BY recorded_at DESC"
            "  LIMIT $4) r",
            columns, telemetry_table(payload));
        return parse_json_field(txn.exec(sql, pqxx::params{device_id, from, to, limit}).one_field());
    });
}

boost::json::value DatabaseManager::fetch_device_types(DbRole role)
{
    return with_role(role, [](pqxx::work& txn) {
        return parse_json_field(
            txn.exec("SELECT COALESCE(json_agg(t ORDER BY t.type_id), '[]'::json)::text FROM device_type t")
                .one_field());
    });
}

std::optional<boost::json::value> DatabaseManager::create_device(DbRole role, const std::string& name,
                                                                 const std::string& type_id)
{
    return with_role(role, [&](pqxx::work& txn) -> std::optional<boost::json::value> {
        // INSERT ... SELECT: an unknown type inserts nothing instead of raising 23503.
        const auto res = txn.exec(
            "WITH ins AS ("
            "  INSERT INTO device (name, type_id)"
            "  SELECT $1, t.type_id FROM device_type t WHERE t.type_id = $2"
            "  RETURNING device_id, name, type_id, created_at)"
            " SELECT json_build_object('device_id', i.device_id, 'device', i.name, 'type_id', i.type_id,"
            "   'payload', t.payload, 'created_at', i.created_at)::text"
            " FROM ins i JOIN device_type t USING (type_id)",
            pqxx::params{name, type_id});
        if (res.empty()) return std::nullopt;
        return parse_json_field(res[0][0]);
    });
}

bool DatabaseManager::delete_device(DbRole role, const std::string& name)
{
    return with_role(role, [&](pqxx::work& txn) {
        return !txn.exec("DELETE FROM device WHERE name = $1 RETURNING device_id", pqxx::params{name}).empty();
    });
}

std::vector<std::string> DatabaseManager::ensure_history_partitions(Payload payload, int months_ahead)
{
    auto lease = pool_.acquire();
    pqxx::work txn(lease.get());
    const auto res = txn.exec("SELECT ensure_monthly_partitions($1::regclass, NULL, $2)",
                              pqxx::params{std::string(telemetry_table(payload)), months_ahead});
    txn.commit();
    std::vector<std::string> created;
    for (const auto& row : res) created.emplace_back(row[0].view());
    return created;
}

// ---------------------------------------------------------------------------
// Control plane
// ---------------------------------------------------------------------------

std::int64_t DatabaseManager::generate_request_id(DbRole role)
{
    return with_role(role, [](pqxx::work& txn) {
        return txn.exec("SELECT nextval('request_id_seq')").one_field().as<std::int64_t>();
    });
}

void DatabaseManager::add_pending_config(DbRole role, const ConfigRequest& r)
{
    with_role(role, [&](pqxx::work& txn) {
        const pqxx::params p{r.request_id, r.device_id,   r.rate_ms,
                             r.amplitude,  r.is_active,   r.requested_by};
        txn.exec("INSERT INTO device_config_pending (request_id, device_id, requested_rate_ms,"
                 " requested_amplitude, requested_is_active, requested_by, status)"
                 " VALUES ($1, $2, $3, $4, $5, $6, 'PENDING')",
                 p)
            .no_rows();
        txn.exec("INSERT INTO device_config_pending_history (request_id, device_id, requested_rate_ms,"
                 " requested_amplitude, requested_is_active, requested_by)"
                 " VALUES ($1, $2, $3, $4, $5, $6)",
                 p)
            .no_rows();
    });
}

std::vector<AppliedConfig> DatabaseManager::fetch_applied_config()
{
    auto lease = pool_.acquire();
    pqxx::work txn(lease.get());
    const auto res = txn.exec("SELECT c.device_id, d.name, c.rate_ms, c.amplitude, c.is_active, c.request_id"
                              " FROM device_config c JOIN device d USING (device_id)"
                              " ORDER BY c.device_id");
    txn.commit();

    std::vector<AppliedConfig> out;
    for (const auto& row : res) out.push_back(applied_from_row(row));
    return out;
}

std::optional<AppliedConfig> DatabaseManager::init_device_config(int device_id)
{
    auto lease = pool_.acquire();
    pqxx::work txn(lease.get());
    txn.exec("WITH ins AS ("
             "  INSERT INTO device_config (device_id, rate_ms, amplitude, is_active)"
             "  SELECT d.device_id, t.default_rate_ms, t.default_amplitude, TRUE"
             "  FROM device d JOIN device_type t USING (type_id)"
             "  WHERE d.device_id = $1"
             "  ON CONFLICT (device_id) DO NOTHING"
             "  RETURNING device_id, rate_ms, amplitude, is_active)"
             " INSERT INTO device_config_history (device_id, rate_ms, amplitude, is_active)"
             " SELECT device_id, rate_ms, amplitude, is_active FROM ins",
             pqxx::params{device_id})
        .no_rows();
    const auto res = txn.exec("SELECT c.device_id, d.name, c.rate_ms, c.amplitude, c.is_active, c.request_id"
                              " FROM device_config c JOIN device d USING (device_id)"
                              " WHERE c.device_id = $1",
                              pqxx::params{device_id});
    txn.commit();
    if (res.empty()) return std::nullopt;
    return applied_from_row(res[0]);
}

DrainStats DatabaseManager::drain_pending(
    const std::function<ConfigOutcome(const ConfigRequest&)>& decide,
    const std::function<void(const AppliedConfig&)>& on_applied, std::chrono::seconds max_age)
{
    DrainStats stats;
    auto lease = pool_.acquire();

    for (;;)
    {
        pqxx::work txn(lease.get());
        // FOR UPDATE OF p: the controller may not lock rows of `device`. A device
        // removed meanwhile takes its PENDING rows with it (ON DELETE CASCADE).
        const auto res = txn.exec(
            "SELECT p.request_id, p.device_id, d.name, p.requested_rate_ms, p.requested_amplitude,"
            " p.requested_is_active, p.requested_by,"
            " extract(epoch FROM clock_timestamp() - p.requested_at)::float8"
            " FROM device_config_pending p JOIN device d USING (device_id)"
            " WHERE p.status = 'PENDING'"
            " ORDER BY p.requested_at, p.request_id"
            " LIMIT 1"
            " FOR UPDATE OF p SKIP LOCKED");
        if (res.empty())
        {
            txn.commit();
            break;
        }

        const auto row = res[0];
        ConfigRequest req;
        req.request_id = row[0].as<std::int64_t>();
        req.device_id = row[1].as<int>();
        req.device = row[2].as<std::string>();
        req.rate_ms = row[3].as<int>();
        req.amplitude = row[4].as<double>();
        req.is_active = row[5].as<bool>();
        req.requested_by = row[6].as<std::string>();
        const double age_s = row[7].as<double>();

        ConfigOutcome outcome;
        bool expired = false;
        if (age_s > static_cast<double>(max_age.count()))
        {
            expired = true;
            outcome = ConfigOutcome::failure(
                "EXPIRED", std::format("request waited {:.0f} s (limit {} s)", age_s, max_age.count()));
        }
        else
        {
            outcome = decide(req);
        }

        if (outcome.ok)
        {
            const pqxx::params p{req.device_id, req.rate_ms, req.amplitude, req.is_active, req.request_id};
            txn.exec("INSERT INTO device_config (device_id, rate_ms, amplitude, is_active, request_id, applied_at)"
                     " VALUES ($1, $2, $3, $4, $5, clock_timestamp())"
                     " ON CONFLICT (device_id) DO UPDATE SET rate_ms = EXCLUDED.rate_ms,"
                     " amplitude = EXCLUDED.amplitude, is_active = EXCLUDED.is_active,"
                     " request_id = EXCLUDED.request_id, applied_at = EXCLUDED.applied_at",
                     p)
                .no_rows();
            txn.exec("INSERT INTO device_config_history (device_id, rate_ms, amplitude, is_active, request_id)"
                     " VALUES ($1, $2, $3, $4, $5)",
                     p)
                .no_rows();
            txn.exec("UPDATE device_config_pending SET status = 'SUCCESS', completed_at = clock_timestamp()"
                     " WHERE request_id = $1",
                     pqxx::params{req.request_id})
                .no_rows();
            txn.commit();

            ++stats.applied;
            log::info(kLog, std::format("request {} applied to {} (rate_ms={} amplitude={} active={})",
                                        req.request_id, req.device, req.rate_ms, req.amplitude,
                                        req.is_active));
            if (on_applied)
            {
                on_applied(AppliedConfig{req.device_id, req.device, req.rate_ms, req.amplitude,
                                         req.is_active, req.request_id});
            }
        }
        else
        {
            txn.exec("INSERT INTO device_config_errors (request_id, device_id, error_code, error_detail)"
                     " VALUES ($1, $2, $3, $4)",
                     pqxx::params{req.request_id, req.device_id, outcome.error_code, outcome.error_detail})
                .no_rows();
            txn.exec(std::format("UPDATE device_config_pending SET status = '{}',"
                                 " completed_at = clock_timestamp() WHERE request_id = $1",
                                 expired ? "TIMEOUT" : "ERROR"),
                     pqxx::params{req.request_id})
                .no_rows();
            txn.commit();

            if (expired) ++stats.expired;
            else ++stats.rejected;
            log::warn(kLog, std::format("request {} rejected: {} ({})", req.request_id,
                                        outcome.error_code, outcome.error_detail));
        }
    }
    return stats;
}

boost::json::value DatabaseManager::fetch_sync_status(DbRole role)
{
    return with_role(role, [](pqxx::work& txn) {
        return parse_json_field(
            txn.exec("SELECT COALESCE(json_agg(v ORDER BY v.device_id), '[]'::json)::text"
                     " FROM v_device_config_sync_status v")
                .one_field());
    });
}

std::optional<boost::json::value> DatabaseManager::fetch_device_sync(DbRole role, const std::string& name)
{
    return with_role(role, [&](pqxx::work& txn) -> std::optional<boost::json::value> {
        const auto res = txn.exec("SELECT row_to_json(v)::text FROM v_device_config_sync_status v"
                                  " WHERE v.device = $1",
                                  pqxx::params{name});
        if (res.empty()) return std::nullopt;
        return parse_json_field(res[0][0]);
    });
}

std::optional<boost::json::value> DatabaseManager::fetch_request(DbRole role, std::int64_t request_id)
{
    return with_role(role, [&](pqxx::work& txn) -> std::optional<boost::json::value> {
        const auto res = txn.exec(
            "SELECT row_to_json(r)::text FROM ("
            "  SELECT p.request_id, p.device_id, d.name AS device, p.requested_rate_ms,"
            "         p.requested_amplitude, p.requested_is_active, p.status, p.requested_by,"
            "         p.requested_at, p.completed_at, e.error_code, e.error_detail"
            "  FROM device_config_pending p"
            "  JOIN device d USING (device_id)"
            "  LEFT JOIN device_config_errors e ON e.request_id = p.request_id"
            "  WHERE p.request_id = $1) r",
            pqxx::params{request_id});
        if (res.empty()) return std::nullopt;
        return parse_json_field(res[0][0]);
    });
}

boost::json::value DatabaseManager::fetch_config_history(DbRole role, const std::optional<std::string>& device,
                                                         int limit)
{
    return with_role(role, [&](pqxx::work& txn) {
        return parse_json_field(
            txn.exec("SELECT COALESCE(json_agg(h ORDER BY h.recorded_at DESC, h.history_id DESC),"
                     " '[]'::json)::text FROM ("
                     "  SELECT h.history_id, h.device_id, d.name AS device, h.rate_ms, h.amplitude,"
                     "         h.is_active, h.request_id, h.recorded_at"
                     "  FROM device_config_history h"
                     "  LEFT JOIN device d USING (device_id)"
                     "  WHERE ($1::text IS NULL OR d.name = $1::text)"
                     "  ORDER BY h.recorded_at DESC, h.history_id DESC"
                     "  LIMIT $2) h",
                     pqxx::params{device, limit})
                .one_field());
    });
}

// ---------------------------------------------------------------------------
// Users
// ---------------------------------------------------------------------------

std::optional<UserLoginRow> DatabaseManager::fetch_user_for_login(const std::string& username)
{
    auto lease = pool_.acquire();
    pqxx::work txn(lease.get());
    const auto res = txn.exec("SELECT password_hash, role FROM auth_fetch_user($1)",
                              pqxx::params{username});
    txn.commit();
    if (res.empty() || res[0][0].is_null()) return std::nullopt;
    return UserLoginRow{res[0][0].as<std::string>(), res[0][1].as<std::string>()};
}

boost::json::value DatabaseManager::list_users(DbRole role)
{
    return with_role(role, [](pqxx::work& txn) {
        return parse_json_field(
            txn.exec("SELECT COALESCE(json_agg(u ORDER BY u.user_id), '[]'::json)::text"
                     " FROM v_users_directory u")
                .one_field());
    });
}

int DatabaseManager::create_user(DbRole role, const std::string& username,
                                 const std::string& password_hash, DbRole user_role)
{
    return with_role(role, [&](pqxx::work& txn) {
        return txn
            .exec("INSERT INTO app_users (username, password_hash, role)"
                  " VALUES ($1, $2, $3::app_role) RETURNING user_id",
                  pqxx::params{username, password_hash, std::string(jwt_role_name(user_role))})
            .one_field()
            .as<int>();
    });
}

bool DatabaseManager::delete_user(DbRole role, const std::string& username)
{
    return with_role(role, [&](pqxx::work& txn) {
        return !txn.exec("DELETE FROM app_users WHERE username = $1 RETURNING user_id",
                         pqxx::params{username})
                    .empty();
    });
}

} // namespace pgdemo::db
