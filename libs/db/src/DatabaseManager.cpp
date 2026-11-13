#include <db/DatabaseManager.hpp>

#include <sys/socket.h>

#include <format>
#include <map>

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

/// Columns read by `device_from_row`.
constexpr std::string_view kDeviceSelect =
    "SELECT d.device_id, d.name, d.type_id, t.payload, d.cert_id, c.serial, c.fingerprint_sha256,"
    " c.cert_pem, i.identity"
    " FROM device d"
    " JOIN device_type t USING (type_id)"
    " JOIN device_certificate c ON c.cert_id = d.cert_id"
    " JOIN v_device_identity i ON i.device_id = d.device_id";

Device device_from_row(const pqxx::row& row)
{
    Device d;
    d.device_id = row[0].as<int>();
    d.name = row[1].as<std::string>();
    d.type_id = row[2].as<std::string>();
    d.payload = parse_payload(row[3].view()).value_or(Payload::Scalar);
    d.cert_id = row[4].as<int>();
    d.serial = row[5].as<std::string>();
    d.fingerprint = row[6].as<std::string>();
    d.cert_pem = row[7].as<std::string>();
    d.identity = row[8].as<std::string>();
    return d;
}

/// Columns read by `enrollment_from_row`.
constexpr std::string_view kEnrollmentSelect =
    "SELECT e.request_id, e.kind, e.name, e.type_id, t.payload, e.device_id, e.status, e.csr_pem,"
    " e.requested_by"
    " FROM device_enrollment e JOIN device_type t USING (type_id)";

Enrollment enrollment_from_row(const pqxx::row& row)
{
    Enrollment e;
    e.request_id = row[0].as<std::int64_t>();
    e.kind = row[1].as<std::string>();
    e.name = row[2].as<std::string>();
    e.type_id = row[3].as<std::string>();
    e.payload = parse_payload(row[4].view()).value_or(Payload::Scalar);
    if (!row[5].is_null()) e.device_id = row[5].as<int>();
    e.status = row[6].as<std::string>();
    if (!row[7].is_null()) e.csr_pem = row[7].as<std::string>();
    e.requested_by = row[8].as<std::string>();
    return e;
}

/// JSON of a `device_enrollment` row, without the CSR.
constexpr std::string_view kEnrollmentJson =
    "SELECT e.request_id, e.kind, e.name AS device, e.type_id, e.device_id, e.status, e.cert_id,"
    " e.error_code, e.error_detail, e.requested_by, e.requested_at, e.csr_at, e.decided_at"
    " FROM device_enrollment e";

double epoch_seconds(std::chrono::system_clock::time_point t)
{
    return std::chrono::duration<double>(t.time_since_epoch()).count();
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
             "  ON CONFLICT (device_id) DO UPDATE SET value = EXCLUDED.value, xy = NULL, xyz = NULL, seq = EXCLUDED.seq,"
             "    updated_at = EXCLUDED.updated_at"
             "  RETURNING device_id, value, seq, updated_at)"
             " INSERT INTO telemetry_scalar (device_id, value, seq, recorded_at)"
             " SELECT device_id, value, seq, updated_at FROM up",
             pqxx::params{sample.device_id, sample.value, sample.seq})
        .no_rows();
    txn.commit();
}

void DatabaseManager::write_xy_sample(const XySample& sample)
{
    auto lease = pool_.acquire();
    pqxx::work txn(lease.get());
    txn.exec("WITH up AS ("
             "  INSERT INTO device_state (device_id, value, position, xy, seq, updated_at)"
             "  VALUES ($1, NULL, NULL, point($2::float8, $3::float8), $4, clock_timestamp())"
             "  ON CONFLICT (device_id) DO UPDATE SET value = NULL, position = NULL, xy = EXCLUDED.xy, xyz = NULL,"
             "    seq = EXCLUDED.seq, updated_at = EXCLUDED.updated_at"
             "  RETURNING device_id, xy, seq, updated_at)"
             " INSERT INTO telemetry_xy (device_id, xy, seq, recorded_at)"
             " SELECT device_id, xy, seq, updated_at FROM up",
             pqxx::params{sample.device_id, sample.x, sample.y, sample.seq})
        .no_rows();
    txn.commit();
}

void DatabaseManager::write_xyz_sample(const XyzSample& sample)
{
    auto lease = pool_.acquire();
    pqxx::work txn(lease.get());
    txn.exec("WITH up AS ("
             "  INSERT INTO device_state (device_id, value, position, xy, xyz, seq, updated_at)"
             "  VALUES ($1, NULL, NULL, NULL, ST_SetSRID(ST_MakePoint($2::float8, $3::float8, $4::float8), 0), $5,"
             "    clock_timestamp())"
             "  ON CONFLICT (device_id) DO UPDATE SET value = NULL, position = NULL, xy = NULL, xyz = EXCLUDED.xyz,"
             "    seq = EXCLUDED.seq, updated_at = EXCLUDED.updated_at"
             "  RETURNING device_id, xyz, seq, updated_at)"
             " INSERT INTO telemetry_xyz (device_id, xyz, seq, recorded_at)"
             " SELECT device_id, xyz, seq, updated_at FROM up",
             pqxx::params{sample.device_id, sample.x, sample.y, sample.z, sample.seq})
        .no_rows();
    txn.commit();
}

void DatabaseManager::write_telemetry_batch(const std::vector<DummySample>& scalar, const std::vector<TrackSample>& gps,
                                            const std::vector<XySample>& xy, const std::vector<XyzSample>& xyz)
{
    if (scalar.empty() && gps.empty() && xy.empty() && xyz.empty()) return;
    auto lease = pool_.acquire();
    pqxx::work txn(lease.get());

    if (!scalar.empty())
    {
        pqxx::params history;
        std::string values;
        for (std::size_t i = 0; i < scalar.size(); ++i)
        {
            if (i != 0) values += ',';
            const int b = static_cast<int>(i * 3 + 1);
            values += std::format("(${}, ${}, ${}, clock_timestamp())", b, b + 1, b + 2);
            history.append(scalar[i].device_id);
            history.append(scalar[i].value);
            history.append(scalar[i].seq);
        }
        txn.exec("INSERT INTO telemetry_scalar (device_id, value, seq, recorded_at) VALUES " + values, history).no_rows();

        std::map<int, const DummySample*> latest;
        for (const auto& sample : scalar)
        {
            const auto it = latest.find(sample.device_id);
            if (it == latest.end() || sample.seq >= it->second->seq) latest[sample.device_id] = &sample;
        }
        pqxx::params state;
        std::string rows;
        int n = 0;
        for (const auto& [_, sample] : latest)
        {
            if (n != 0) rows += ',';
            const int b = n * 3 + 1;
            rows += std::format("(${}, ${}, NULL, ${}, clock_timestamp())", b, b + 1, b + 2);
            state.append(sample->device_id);
            state.append(sample->value);
            state.append(sample->seq);
            ++n;
        }
        txn.exec("INSERT INTO device_state (device_id, value, position, seq, updated_at) VALUES " + rows +
                     " ON CONFLICT (device_id) DO UPDATE SET value = EXCLUDED.value, position = NULL, xy = NULL, xyz = NULL,"
                     " seq = EXCLUDED.seq, updated_at = EXCLUDED.updated_at",
                 state)
            .no_rows();
    }

    if (!gps.empty())
    {
        pqxx::params history;
        std::string values;
        for (std::size_t i = 0; i < gps.size(); ++i)
        {
            if (i != 0) values += ',';
            const int b = static_cast<int>(i * 6 + 1);
            values += std::format("(${}, ST_Project(ST_SetSRID(ST_MakePoint(${}, ${}), 4326)::geography,"
                                  " ${}::double precision, ${}::double precision), ${}, clock_timestamp())",
                                  b, b + 1, b + 2, b + 3, b + 4, b + 5);
            history.append(gps[i].device_id);
            history.append(gps[i].origin_lon);
            history.append(gps[i].origin_lat);
            history.append(gps[i].radius_m);
            history.append(gps[i].bearing_rad);
            history.append(gps[i].seq);
        }
        txn.exec("INSERT INTO telemetry_gps (device_id, position, seq, recorded_at) VALUES " + values, history).no_rows();

        std::map<int, const TrackSample*> latest;
        for (const auto& sample : gps)
        {
            const auto it = latest.find(sample.device_id);
            if (it == latest.end() || sample.seq >= it->second->seq) latest[sample.device_id] = &sample;
        }
        pqxx::params state;
        std::string rows;
        int n = 0;
        for (const auto& [_, sample] : latest)
        {
            if (n != 0) rows += ',';
            const int b = n * 6 + 1;
            rows += std::format("(${}, NULL, ST_Project(ST_SetSRID(ST_MakePoint(${}, ${}), 4326)::geography,"
                                " ${}::double precision, ${}::double precision), ${}, clock_timestamp())",
                                b, b + 1, b + 2, b + 3, b + 4, b + 5);
            state.append(sample->device_id);
            state.append(sample->origin_lon);
            state.append(sample->origin_lat);
            state.append(sample->radius_m);
            state.append(sample->bearing_rad);
            state.append(sample->seq);
            ++n;
        }
        txn.exec("INSERT INTO device_state (device_id, value, position, seq, updated_at) VALUES " + rows +
                     " ON CONFLICT (device_id) DO UPDATE SET value = NULL, position = EXCLUDED.position, xy = NULL, xyz = NULL,"
                     " seq = EXCLUDED.seq, updated_at = EXCLUDED.updated_at",
                 state)
            .no_rows();
    }

    if (!xy.empty())
    {
        pqxx::params history;
        std::string values;
        for (std::size_t i = 0; i < xy.size(); ++i)
        {
            if (i != 0) values += ',';
            const int b = static_cast<int>(i * 4 + 1);
            values += std::format("(${}, point(${}::float8, ${}::float8), ${}, clock_timestamp())", b, b + 1, b + 2, b + 3);
            history.append(xy[i].device_id);
            history.append(xy[i].x);
            history.append(xy[i].y);
            history.append(xy[i].seq);
        }
        txn.exec("INSERT INTO telemetry_xy (device_id, xy, seq, recorded_at) VALUES " + values, history).no_rows();

        std::map<int, const XySample*> latest;
        for (const auto& sample : xy)
        {
            const auto it = latest.find(sample.device_id);
            if (it == latest.end() || sample.seq >= it->second->seq) latest[sample.device_id] = &sample;
        }
        pqxx::params state;
        std::string rows;
        int n = 0;
        for (const auto& [_, sample] : latest)
        {
            if (n != 0) rows += ',';
            const int b = n * 4 + 1;
            rows += std::format("(${}, NULL, NULL, point(${}::float8, ${}::float8), ${}, clock_timestamp())", b, b + 1,
                                b + 2, b + 3);
            state.append(sample->device_id);
            state.append(sample->x);
            state.append(sample->y);
            state.append(sample->seq);
            ++n;
        }
        txn.exec("INSERT INTO device_state (device_id, value, position, xy, seq, updated_at) VALUES " + rows +
                     " ON CONFLICT (device_id) DO UPDATE SET value = NULL, position = NULL, xy = EXCLUDED.xy, xyz = NULL,"
                     " seq = EXCLUDED.seq, updated_at = EXCLUDED.updated_at",
                 state)
            .no_rows();
    }

    if (!xyz.empty())
    {
        pqxx::params history;
        std::string values;
        for (std::size_t i = 0; i < xyz.size(); ++i)
        {
            if (i != 0) values += ',';
            const int b = static_cast<int>(i * 5 + 1);
            values += std::format(
                "(${}, ST_SetSRID(ST_MakePoint(${}::float8, ${}::float8, ${}::float8), 0), ${}, clock_timestamp())", b,
                b + 1, b + 2, b + 3, b + 4);
            history.append(xyz[i].device_id);
            history.append(xyz[i].x);
            history.append(xyz[i].y);
            history.append(xyz[i].z);
            history.append(xyz[i].seq);
        }
        txn.exec("INSERT INTO telemetry_xyz (device_id, xyz, seq, recorded_at) VALUES " + values, history).no_rows();

        std::map<int, const XyzSample*> latest;
        for (const auto& sample : xyz)
        {
            const auto it = latest.find(sample.device_id);
            if (it == latest.end() || sample.seq >= it->second->seq) latest[sample.device_id] = &sample;
        }
        pqxx::params state;
        std::string rows;
        int n = 0;
        for (const auto& [_, sample] : latest)
        {
            if (n != 0) rows += ',';
            const int b = n * 5 + 1;
            rows += std::format(
                "(${}, NULL, NULL, NULL, ST_SetSRID(ST_MakePoint(${}::float8, ${}::float8, ${}::float8), 0), ${},"
                " clock_timestamp())",
                b, b + 1, b + 2, b + 3, b + 4);
            state.append(sample->device_id);
            state.append(sample->x);
            state.append(sample->y);
            state.append(sample->z);
            state.append(sample->seq);
            ++n;
        }
        txn.exec("INSERT INTO device_state (device_id, value, position, xy, xyz, seq, updated_at) VALUES " + rows +
                     " ON CONFLICT (device_id) DO UPDATE SET value = NULL, position = NULL, xy = NULL,"
                     " xyz = EXCLUDED.xyz, seq = EXCLUDED.seq, updated_at = EXCLUDED.updated_at",
                 state)
            .no_rows();
    }
    txn.commit();
}

std::vector<boost::json::object> DatabaseManager::take_unread_telemetry(std::string_view channel)
{
    const auto payload = [&]() -> std::optional<Payload> {
        for (const auto p : kAllPayloads)
            if (channel == telemetry_channel(p)) return p;
        return std::nullopt;
    }();
    if (!payload)
    {
        throw std::invalid_argument("take_unread_telemetry: unknown channel " + std::string(channel));
    }

    auto lease = pool_.acquire();
    pqxx::work txn(lease.get());
    const auto cur = txn.exec("SELECT history_id FROM telemetry_read_cursor WHERE channel = $1 FOR UPDATE",
                              pqxx::params{std::string(channel)});
    if (cur.empty())
    {
        throw std::runtime_error("telemetry_read_cursor has no row for " + std::string(channel));
    }
    const auto cursor = cur[0][0].as<std::int64_t>();

    // '{{}}' is the format escape for the JSON operator '{}'.
    const char* columns = "t.value";
    if (*payload == Payload::Gps)
        columns = "extensions.ST_X(t.position::extensions.geometry) AS lon,"
                  " extensions.ST_Y(t.position::extensions.geometry) AS lat";
    else if (*payload == Payload::Xy)
        columns = "(t.xy)[0] AS x, (t.xy)[1] AS y";
    else if (*payload == Payload::Xyz)
        columns = "ST_X(t.xyz) AS x, ST_Y(t.xyz) AS y, ST_Z(t.xyz) AS z";
    const auto sql = std::format("SELECT t.history_id, t.device_id, d.name AS device, t.seq,"
                                 " to_json(t.recorded_at)#>>'{{}}' AS ts, {}"
                                 " FROM {} t"
                                 " JOIN device d ON d.device_id = t.device_id"
                                 " WHERE t.history_id > $1"
                                 " ORDER BY t.history_id"
                                 " LIMIT {}",
                                 columns, telemetry_table(*payload), kTelemetryReadPage);
    const auto rows = txn.exec(sql, pqxx::params{cursor});

    std::vector<boost::json::object> out;
    out.reserve(rows.size());
    if (!rows.empty())
    {
        txn.exec("UPDATE telemetry_read_cursor SET history_id = $2 WHERE channel = $1",
                 pqxx::params{std::string(channel), rows.back()["history_id"].as<std::int64_t>()})
            .no_rows();
        for (const auto& row : rows)
        {
            boost::json::object sample;
            sample["action"] = "INSERT";
            sample["device_id"] = row["device_id"].as<std::int64_t>();
            sample["device"] = row["device"].view();
            sample["seq"] = row["seq"].as<std::int64_t>();
            sample["ts"] = row["ts"].view();
            if (*payload == Payload::Gps)
            {
                sample["lon"] = row["lon"].as<double>();
                sample["lat"] = row["lat"].as<double>();
            }
            else if (*payload == Payload::Xy)
            {
                sample["x"] = row["x"].as<double>();
                sample["y"] = row["y"].as<double>();
            }
            else if (*payload == Payload::Xyz)
            {
                sample["x"] = row["x"].as<double>();
                sample["y"] = row["y"].as<double>();
                sample["z"] = row["z"].as<double>();
            }
            else
            {
                sample["value"] = row["value"].as<double>();
            }
            out.push_back(std::move(sample));
        }
    }
    txn.commit();
    return out;
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
             "  ON CONFLICT (device_id) DO UPDATE SET position = EXCLUDED.position, xy = NULL, xyz = NULL, seq = EXCLUDED.seq,"
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
    const auto res = txn.exec(std::string(kDeviceSelect) + " ORDER BY d.device_id");
    txn.commit();
    std::vector<Device> out;
    for (const auto& row : res) out.push_back(device_from_row(row));
    return out;
}

std::optional<Device> DatabaseManager::load_device(int device_id)
{
    auto lease = pool_.acquire();
    pqxx::work txn(lease.get());
    const auto res = txn.exec(std::string(kDeviceSelect) + " WHERE d.device_id = $1", pqxx::params{device_id});
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
            "SELECT (to_jsonb(v) || jsonb_build_object("
            "   'config', to_jsonb(s) - 'device_id' - 'device' - 'type_id',"
            "   'certificate', to_jsonb(i) - 'device_id' - 'device' - 'identity' - 'session' - 'pid'"
            "                  - 'connected_at' - 'last_seen' - 'last_error',"
            "   'process', jsonb_build_object('state', i.session, 'pid', i.pid,"
            "              'connected_at', i.connected_at, 'last_seen', i.last_seen,"
            "              'last_error', i.last_error)))::text"
            " FROM v_devices v"
            " JOIN v_device_config_sync_status s USING (device_id)"
            " JOIN v_device_identity i USING (device_id)"
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

        const char* columns =
            "history_id, device_id, value, seq, recorded_at, tableoid::regclass::text AS partition";
        if (payload == Payload::Gps)
            columns = "history_id, device_id, ST_X(position::geometry) AS lon, ST_Y(position::geometry) AS lat,"
                      " seq, recorded_at, tableoid::regclass::text AS partition";
        else if (payload == Payload::Xy)
            columns = "history_id, device_id, (xy)[0] AS x, (xy)[1] AS y,"
                      " seq, recorded_at, tableoid::regclass::text AS partition";
        else if (payload == Payload::Xyz)
            columns = "history_id, device_id, ST_X(xyz) AS x, ST_Y(xyz) AS y, ST_Z(xyz) AS z,"
                      " seq, recorded_at, tableoid::regclass::text AS partition";
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

ForwardBatch DatabaseManager::forwardable_pending(std::chrono::seconds max_age)
{
    ForwardBatch batch;
    auto lease = pool_.acquire();
    {
        pqxx::work txn(lease.get());
        const auto stale = txn.exec(
            "UPDATE device_config_pending SET status = 'TIMEOUT', completed_at = clock_timestamp()"
            " WHERE status = 'PENDING'"
            " AND extract(epoch FROM clock_timestamp() - requested_at) > $1"
            " RETURNING request_id, device_id, extract(epoch FROM clock_timestamp() - requested_at)::float8",
            pqxx::params{static_cast<double>(max_age.count())});
        for (const auto& row : stale)
        {
            const auto id = row[0].as<std::int64_t>();
            const int device_id = row[1].as<int>();
            const double age_s = row[2].as<double>();
            const auto detail = std::format("request waited {:.0f} s (limit {} s)", age_s, max_age.count());
            txn.exec("INSERT INTO device_config_errors (request_id, device_id, error_code, error_detail)"
                     " VALUES ($1, $2, 'EXPIRED', $3)",
                     pqxx::params{id, device_id, detail})
                .no_rows();
            log::warn(kLog, std::format("request {} expired: {}", id, detail));
            ++batch.expired;
        }
        txn.commit();
    }
    {
        pqxx::work txn(lease.get());
        const auto res = txn.exec(
            "SELECT p.request_id, p.device_id, d.name, p.requested_rate_ms, p.requested_amplitude,"
            " p.requested_is_active, p.requested_by"
            " FROM device_config_pending p JOIN device d USING (device_id)"
            " WHERE p.status = 'PENDING'"
            " ORDER BY p.requested_at, p.request_id");
        for (const auto& row : res)
        {
            ConfigRequest req;
            req.request_id = row[0].as<std::int64_t>();
            req.device_id = row[1].as<int>();
            req.device = row[2].as<std::string>();
            req.rate_ms = row[3].as<int>();
            req.amplitude = row[4].as<double>();
            req.is_active = row[5].as<bool>();
            req.requested_by = row[6].as<std::string>();
            batch.pending.push_back(std::move(req));
        }
        txn.commit();
    }
    return batch;
}

bool DatabaseManager::complete_pending(std::int64_t request_id, const ConfigOutcome& outcome)
{
    if (request_id == 0) return false;
    auto lease = pool_.acquire();
    pqxx::work txn(lease.get());
    const auto res = txn.exec(
        "SELECT p.device_id, d.name, p.requested_rate_ms, p.requested_amplitude, p.requested_is_active"
        " FROM device_config_pending p JOIN device d USING (device_id)"
        " WHERE p.request_id = $1 AND p.status = 'PENDING'"
        " FOR UPDATE OF p",
        pqxx::params{request_id});
    if (res.empty())
    {
        txn.commit();
        return false;
    }
    const auto row = res[0];
    const int device_id = row[0].as<int>();
    const auto device = row[1].as<std::string>();
    const int rate_ms = row[2].as<int>();
    const double amplitude = row[3].as<double>();
    const bool active = row[4].as<bool>();

    if (outcome.ok)
    {
        const pqxx::params p{device_id, rate_ms, amplitude, active, request_id};
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
                 pqxx::params{request_id})
            .no_rows();
        txn.commit();
        log::info(kLog, std::format("request {} applied to {} (rate_ms={} amplitude={} active={})", request_id, device,
                                    rate_ms, amplitude, active));
        return true;
    }

    txn.exec("INSERT INTO device_config_errors (request_id, device_id, error_code, error_detail)"
             " VALUES ($1, $2, $3, $4)",
             pqxx::params{request_id, device_id, outcome.error_code, outcome.error_detail})
        .no_rows();
    txn.exec("UPDATE device_config_pending SET status = 'ERROR', completed_at = clock_timestamp()"
             " WHERE request_id = $1",
             pqxx::params{request_id})
        .no_rows();
    txn.commit();
    log::warn(kLog, std::format("request {} rejected: {} ({})", request_id, outcome.error_code, outcome.error_detail));
    return true;
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
// Enrollment
// ---------------------------------------------------------------------------

bool DatabaseManager::enqueue_enrollment(DbRole role, std::int64_t request_id, const std::string& name,
                                         const std::string& type_id, const std::string& requested_by)
{
    return with_role(role, [&](pqxx::work& txn) {
        // INSERT ... SELECT: an unknown type inserts nothing instead of raising 23503.
        return !txn.exec("INSERT INTO device_enrollment (request_id, kind, name, type_id, requested_by)"
                         " SELECT $1, 'ENROLL', $2, t.type_id, $4 FROM device_type t WHERE t.type_id = $3"
                         " RETURNING request_id",
                         pqxx::params{request_id, name, type_id, requested_by})
                    .empty();
    });
}

bool DatabaseManager::enqueue_rotation(DbRole role, std::int64_t request_id, const std::string& name,
                                       const std::string& requested_by)
{
    return with_role(role, [&](pqxx::work& txn) {
        return !txn.exec("INSERT INTO device_enrollment (request_id, kind, name, type_id, device_id, requested_by)"
                         " SELECT $1, 'ROTATE', d.name, d.type_id, d.device_id, $3 FROM device d"
                         " WHERE d.name = $2"
                         " RETURNING request_id",
                         pqxx::params{request_id, name, requested_by})
                    .empty();
    });
}

std::optional<boost::json::value> DatabaseManager::fetch_enrollment(DbRole role, std::int64_t request_id)
{
    return with_role(role, [&](pqxx::work& txn) -> std::optional<boost::json::value> {
        const auto res = txn.exec("SELECT row_to_json(r)::text FROM (" + std::string(kEnrollmentJson) +
                                      " WHERE e.request_id = $1) r",
                                  pqxx::params{request_id});
        if (res.empty()) return std::nullopt;
        return parse_json_field(res[0][0]);
    });
}

boost::json::value DatabaseManager::fetch_enrollments(DbRole role, const std::optional<std::string>& device,
                                                      int limit)
{
    return with_role(role, [&](pqxx::work& txn) {
        return parse_json_field(
            txn.exec("SELECT COALESCE(json_agg(r ORDER BY r.requested_at DESC, r.request_id DESC),"
                     " '[]'::json)::text FROM (" +
                         std::string(kEnrollmentJson) +
                         "  WHERE ($1::text IS NULL OR e.name = $1::text)"
                         "  ORDER BY e.requested_at DESC, e.request_id DESC"
                         "  LIMIT $2) r",
                     pqxx::params{device, limit})
                .one_field());
    });
}

std::optional<Enrollment> DatabaseManager::fetch_csr(std::int64_t request_id)
{
    // demo_server may read only these columns, and not device_type.
    auto lease = pool_.acquire();
    pqxx::work txn(lease.get());
    const auto res = txn.exec("SELECT request_id, kind, name, type_id, status, csr_pem, requested_by"
                              " FROM device_enrollment WHERE request_id = $1",
                              pqxx::params{request_id});
    txn.commit();
    if (res.empty()) return std::nullopt;
    const auto row = res[0];
    Enrollment e;
    e.request_id = row[0].as<std::int64_t>();
    e.kind = row[1].as<std::string>();
    e.name = row[2].as<std::string>();
    e.type_id = row[3].as<std::string>();
    e.status = row[4].as<std::string>();
    if (!row[5].is_null()) e.csr_pem = row[5].as<std::string>();
    e.requested_by = row[6].as<std::string>();
    return e;
}

IssueResult DatabaseManager::issue_certificate(std::int64_t request_id, const std::string& ca_fingerprint,
                                               const IssuedCertificate& cert)
{
    auto lease = pool_.acquire();
    pqxx::work txn(lease.get());
    const auto row = txn.exec("SELECT issued_device_id, issued_cert_id FROM issue_device_certificate("
                              "$1, $2, $3, $4, $5, to_timestamp($6::float8), to_timestamp($7::float8), $8)",
                              pqxx::params{request_id, ca_fingerprint, cert.serial, cert.fingerprint,
                                           cert.subject_cn, epoch_seconds(cert.not_before),
                                           epoch_seconds(cert.not_after), cert.pem})
                         .one_row();
    txn.commit();
    return IssueResult{row[0].as<int>(), row[1].as<int>()};
}

bool DatabaseManager::reject_enrollment(std::int64_t request_id, const std::string& code,
                                        const std::string& detail)
{
    auto lease = pool_.acquire();
    pqxx::work txn(lease.get());
    const auto res = txn.exec("SELECT reject_enrollment($1, $2, $3)", pqxx::params{request_id, code, detail});
    txn.commit();
    return !res.empty() && !res[0][0].is_null() && res[0][0].as<bool>();
}

std::vector<std::string> DatabaseManager::active_root_fingerprints()
{
    auto lease = pool_.acquire();
    pqxx::work txn(lease.get());
    const auto res = txn.exec("SELECT fingerprint_sha256 FROM root_ca WHERE is_active AND not_after > now()"
                              " ORDER BY ca_id");
    txn.commit();
    std::vector<std::string> out;
    for (const auto& row : res) out.emplace_back(row[0].view());
    return out;
}

std::vector<std::int64_t> DatabaseManager::submitted_csr_ids()
{
    auto lease = pool_.acquire();
    pqxx::work txn(lease.get());
    const auto res = txn.exec("SELECT request_id FROM device_enrollment WHERE status = 'CSR_SUBMITTED'"
                              " ORDER BY request_id");
    txn.commit();
    std::vector<std::int64_t> out;
    for (const auto& row : res) out.push_back(row[0].as<std::int64_t>());
    return out;
}

std::vector<Enrollment> DatabaseManager::pending_enrollments()
{
    auto lease = pool_.acquire();
    pqxx::work txn(lease.get());
    const auto res = txn.exec(std::string(kEnrollmentSelect) +
                              " WHERE e.status = 'PENDING' ORDER BY e.requested_at, e.request_id");
    txn.commit();
    std::vector<Enrollment> out;
    for (const auto& row : res) out.push_back(enrollment_from_row(row));
    return out;
}

std::optional<Enrollment> DatabaseManager::load_enrollment(std::int64_t request_id)
{
    auto lease = pool_.acquire();
    pqxx::work txn(lease.get());
    const auto res = txn.exec(std::string(kEnrollmentSelect) + " WHERE e.request_id = $1",
                              pqxx::params{request_id});
    txn.commit();
    if (res.empty()) return std::nullopt;
    return enrollment_from_row(res[0]);
}

bool DatabaseManager::submit_csr(std::int64_t request_id, const std::string& csr_pem)
{
    auto lease = pool_.acquire();
    pqxx::work txn(lease.get());
    const auto res = txn.exec("UPDATE device_enrollment"
                              " SET status = 'CSR_SUBMITTED', csr_pem = $2, csr_at = clock_timestamp()"
                              " WHERE request_id = $1 AND status = 'PENDING' RETURNING request_id",
                              pqxx::params{request_id, csr_pem});
    txn.commit();
    return !res.empty();
}

bool DatabaseManager::fail_enrollment(std::int64_t request_id, const std::string& code,
                                      const std::string& detail)
{
    auto lease = pool_.acquire();
    pqxx::work txn(lease.get());
    const auto res = txn.exec("UPDATE device_enrollment"
                              " SET status = 'REJECTED', error_code = $2, error_detail = $3,"
                              "     decided_at = clock_timestamp()"
                              " WHERE request_id = $1 AND status IN ('PENDING', 'CSR_SUBMITTED')"
                              " RETURNING request_id",
                              pqxx::params{request_id, code, detail});
    txn.commit();
    return !res.empty();
}

std::vector<std::int64_t> DatabaseManager::expire_enrollments(std::chrono::seconds max_age)
{
    auto lease = pool_.acquire();
    pqxx::work txn(lease.get());
    const auto res = txn.exec(
        "UPDATE device_enrollment"
        " SET status = 'EXPIRED', error_code = 'EXPIRED',"
        "     error_detail = format('open for more than %s s', $1::int), decided_at = clock_timestamp()"
        " WHERE status IN ('PENDING', 'CSR_SUBMITTED')"
        "   AND requested_at < clock_timestamp() - make_interval(secs => $1::int)"
        " RETURNING request_id",
        pqxx::params{static_cast<int>(max_age.count())});
    txn.commit();
    std::vector<std::int64_t> out;
    for (const auto& row : res) out.push_back(row[0].as<std::int64_t>());
    return out;
}

std::optional<EnrollmentOutcome> DatabaseManager::enrollment_outcome(std::int64_t request_id)
{
    auto lease = pool_.acquire();
    pqxx::work txn(lease.get());
    const auto res = txn.exec("SELECT e.request_id, e.status, e.device_id, c.cert_pem, e.error_code, e.error_detail"
                              " FROM device_enrollment e"
                              " LEFT JOIN device_certificate c ON c.cert_id = e.cert_id"
                              " WHERE e.request_id = $1",
                              pqxx::params{request_id});
    txn.commit();
    if (res.empty()) return std::nullopt;
    const auto row = res[0];
    EnrollmentOutcome o;
    o.request_id = row[0].as<std::int64_t>();
    o.status = row[1].as<std::string>();
    if (!row[2].is_null()) o.device_id = row[2].as<int>();
    if (!row[3].is_null()) o.cert_pem = row[3].as<std::string>();
    if (!row[4].is_null()) o.error_code = row[4].as<std::string>();
    if (!row[5].is_null()) o.error_detail = row[5].as<std::string>();
    return o;
}

// ---------------------------------------------------------------------------
// Identity
// ---------------------------------------------------------------------------

std::vector<std::string> DatabaseManager::load_root_pems()
{
    auto lease = pool_.acquire();
    pqxx::work txn(lease.get());
    const auto res = txn.exec("SELECT cert_pem FROM root_ca WHERE is_active ORDER BY ca_id");
    txn.commit();
    std::vector<std::string> out;
    for (const auto& row : res) out.emplace_back(row[0].view());
    return out;
}

std::set<std::string> DatabaseManager::load_revoked_serials()
{
    auto lease = pool_.acquire();
    pqxx::work txn(lease.get());
    const auto res = txn.exec("SELECT serial FROM crl");
    txn.commit();
    std::set<std::string> out;
    for (const auto& row : res) out.emplace(row[0].view());
    return out;
}

std::optional<boost::json::value> DatabaseManager::fetch_certificates(DbRole role, const std::string& name)
{
    return with_role(role, [&](pqxx::work& txn) -> std::optional<boost::json::value> {
        if (txn.exec("SELECT 1 FROM device WHERE name = $1", pqxx::params{name}).empty()) return std::nullopt;
        return parse_json_field(
            txn.exec("SELECT COALESCE(json_agg(r ORDER BY r.issued_at DESC, r.cert_id DESC), '[]'::json)::text"
                     " FROM ("
                     "  SELECT c.cert_id, c.serial, c.fingerprint_sha256 AS fingerprint, c.subject_cn,"
                     "         r.name AS ca, c.not_before, c.not_after, c.issued_at,"
                     "         (c.cert_id = d.cert_id) AS current,"
                     "         x.reason AS revocation_reason, x.revoked_at, x.revoked_by"
                     "  FROM device d"
                     "  JOIN device_certificate c ON c.device_id = d.device_id"
                     "  JOIN root_ca r ON r.ca_id = c.ca_id"
                     "  LEFT JOIN crl x ON x.ca_id = c.ca_id AND x.serial = c.serial"
                     "  WHERE d.name = $1) r",
                     pqxx::params{name})
                .one_field());
    });
}

RevokeResult DatabaseManager::revoke_certificate(DbRole role, const std::string& name, const std::string& reason,
                                                 const std::string& revoked_by)
{
    return with_role(role, [&](pqxx::work& txn) {
        // A hold may be replaced by any reason (protect_crl allows it); any
        // other existing row stays as it is.
        const auto res = txn.exec(
            "INSERT INTO crl (ca_id, serial, fingerprint_sha256, device_name, cert_pem, reason, revoked_by)"
            " SELECT c.ca_id, c.serial, c.fingerprint_sha256, d.name, c.cert_pem, $2, $3"
            " FROM device d JOIN device_certificate c ON c.cert_id = d.cert_id"
            " WHERE d.name = $1"
            " ON CONFLICT (ca_id, serial) DO UPDATE"
            "   SET reason = EXCLUDED.reason, revoked_at = clock_timestamp(), revoked_by = EXCLUDED.revoked_by"
            "   WHERE crl.reason = 'certificateHold'"
            " RETURNING serial",
            pqxx::params{name, reason, revoked_by});
        if (!res.empty()) return RevokeResult::Revoked;
        if (txn.exec("SELECT 1 FROM device WHERE name = $1", pqxx::params{name}).empty())
            return RevokeResult::NoSuchDevice;
        return RevokeResult::AlreadyRevoked;
    });
}

UnrevokeResult DatabaseManager::unrevoke_certificate(DbRole role, const std::string& name)
{
    return with_role(role, [&](pqxx::work& txn) {
        const auto res = txn.exec("DELETE FROM crl x"
                                  " USING device d JOIN device_certificate c ON c.cert_id = d.cert_id"
                                  " WHERE d.name = $1 AND x.ca_id = c.ca_id AND x.serial = c.serial"
                                  "   AND x.reason = 'certificateHold'"
                                  " RETURNING x.serial",
                                  pqxx::params{name});
        if (!res.empty()) return UnrevokeResult::Lifted;
        if (txn.exec("SELECT 1 FROM device WHERE name = $1", pqxx::params{name}).empty())
            return UnrevokeResult::NoSuchDevice;
        return UnrevokeResult::NotOnHold;
    });
}

DeleteCertResult DatabaseManager::delete_certificate(DbRole role, const std::string& name,
                                                     const std::string& serial)
{
    return with_role(role, [&](pqxx::work& txn) {
        if (txn.exec("SELECT 1 FROM device WHERE name = $1", pqxx::params{name}).empty())
            return DeleteCertResult::NoSuchDevice;
        // Referential actions are never deferred: deleting the current
        // certificate deletes the device within this statement.
        const auto res = txn.exec("DELETE FROM device_certificate c USING device d"
                                  " WHERE d.name = $1 AND c.device_id = d.device_id AND c.serial = $2"
                                  " RETURNING c.cert_id = d.cert_id",
                                  pqxx::params{name, serial});
        if (res.empty()) return DeleteCertResult::NoSuchCertificate;
        return res[0][0].as<bool>() ? DeleteCertResult::DeviceDeleted : DeleteCertResult::Deleted;
    });
}

boost::json::value DatabaseManager::fetch_root_cas(DbRole role)
{
    return with_role(role, [](pqxx::work& txn) {
        return parse_json_field(
            txn.exec("SELECT COALESCE(json_agg(r ORDER BY r.ca_id), '[]'::json)::text FROM ("
                     "  SELECT ca_id, name, subject, fingerprint_sha256 AS fingerprint, not_before, not_after,"
                     "         is_active, created_at, cert_pem FROM root_ca) r")
                .one_field());
    });
}

boost::json::value DatabaseManager::fetch_crl_entries(DbRole role)
{
    return with_role(role, [](pqxx::work& txn) {
        return parse_json_field(
            txn.exec("SELECT COALESCE(json_agg(r ORDER BY r.revoked_at DESC), '[]'::json)::text FROM ("
                     "  SELECT x.serial, x.fingerprint_sha256 AS fingerprint, x.device_name AS device,"
                     "         r.name AS ca, x.reason, x.revoked_at, x.revoked_by"
                     "  FROM crl x JOIN root_ca r USING (ca_id)) r")
                .one_field());
    });
}

std::vector<RevokedPem> DatabaseManager::load_crl(DbRole role)
{
    return with_role(role, [](pqxx::work& txn) {
        std::vector<RevokedPem> out;
        for (const auto& row : txn.exec("SELECT cert_pem, reason FROM crl ORDER BY revoked_at, serial"))
            out.push_back(RevokedPem{row[0].as<std::string>(), row[1].as<std::string>()});
        return out;
    });
}

void DatabaseManager::ensure_factory_root(const std::string& subject, const std::string& fingerprint,
                                          const std::string& pem, const std::string& not_before,
                                          const std::string& not_after)
{
    auto lease = pool_.acquire();
    pqxx::work txn(lease.get());
    txn.exec("SELECT register_factory_root($1, $2, $3, $4::timestamptz, $5::timestamptz)",
             pqxx::params{subject, fingerprint, pem, not_before, not_after});
    txn.commit();
}

bool DatabaseManager::manager_certificate_present()
{
    auto lease = pool_.acquire();
    pqxx::work txn(lease.get());
    const auto res = txn.exec("SELECT 1 FROM manager_certificate");
    txn.commit();
    return !res.empty();
}

std::optional<std::string> DatabaseManager::load_manager_certificate_pem()
{
    auto lease = pool_.acquire();
    pqxx::work txn(lease.get());
    const auto res = txn.exec("SELECT cert_pem FROM manager_certificate");
    txn.commit();
    if (res.empty()) return std::nullopt;
    return res[0][0].as<std::string>();
}

void DatabaseManager::register_manager_certificate(const std::string& factory_fingerprint, const std::string& serial,
                                                   const std::string& fingerprint, const std::string& subject_cn,
                                                   const std::string& not_before, const std::string& not_after,
                                                   const std::string& pem)
{
    auto lease = pool_.acquire();
    pqxx::work txn(lease.get());
    txn.exec("SELECT register_manager_certificate($1, $2, $3, $4, $5::timestamptz, $6::timestamptz, $7)",
             pqxx::params{factory_fingerprint, serial, fingerprint, subject_cn, not_before, not_after, pem});
    txn.commit();
}

void DatabaseManager::replace_manager_crl(const std::string& payload, const std::string& signature_hex)
{
    auto lease = pool_.acquire();
    pqxx::work txn(lease.get());
    txn.exec("SELECT replace_manager_crl($1, $2)", pqxx::params{payload, signature_hex});
    txn.commit();
}

void DatabaseManager::add_manager_revocation(const std::string& serial, const std::string& fingerprint,
                                             const std::string& device_name, const std::string& pem)
{
    auto lease = pool_.acquire();
    pqxx::work txn(lease.get());
    txn.exec("SELECT add_manager_revocation($1, $2, $3, $4)", pqxx::params{serial, fingerprint, device_name, pem});
    txn.commit();
}

std::set<std::string> DatabaseManager::load_manager_revoked_serials()
{
    auto lease = pool_.acquire();
    pqxx::work txn(lease.get());
    const auto res = txn.exec("SELECT serial FROM manager_revocation ORDER BY serial");
    txn.commit();
    std::set<std::string> out;
    for (const auto& row : res) out.emplace(row[0].view());
    return out;
}

std::optional<DatabaseManager::ManagerCrlRecord> DatabaseManager::load_manager_crl()
{
    auto lease = pool_.acquire();
    pqxx::work txn(lease.get());
    const auto res = txn.exec("SELECT payload, signature FROM manager_crl_signature");
    txn.commit();
    if (res.empty()) return std::nullopt;
    return ManagerCrlRecord{res[0][0].as<std::string>(), res[0][1].as<std::string>()};
}

bool DatabaseManager::register_factory_worker(const std::string& subject_cn, const std::string& ca_fingerprint,
                                              const std::string& serial, const std::string& fingerprint,
                                              const std::string& not_before, const std::string& not_after,
                                              const std::string& pem)
{
    auto lease = pool_.acquire();
    pqxx::work txn(lease.get());
    const auto res = txn.exec(
        "SELECT register_factory_worker($1, $2, $3, $4, $5::timestamptz, $6::timestamptz, $7)",
        pqxx::params{subject_cn, ca_fingerprint, serial, fingerprint, not_before, not_after, pem});
    txn.commit();
    return res[0][0].as<bool>();
}

boost::json::value DatabaseManager::fetch_origin(DbRole role)
{
    return with_role(role, [](pqxx::work& txn) {
        return parse_json_field(
            txn.exec("SELECT json_build_object("
                     "  'factory', (SELECT json_build_object("
                     "      'name', name, 'subject', subject, 'fingerprint', fingerprint_sha256,"
                     "      'not_before', not_before, 'not_after', not_after, 'cert_pem', cert_pem)"
                     "    FROM root_ca WHERE name = 'factory'),"
                     "  'manager', (SELECT json_build_object("
                     "      'subject_cn', subject_cn, 'serial', serial, 'fingerprint', fingerprint_sha256,"
                     "      'not_before', not_before, 'not_after', not_after, 'cert_pem', cert_pem)"
                     "    FROM manager_certificate),"
                     "  'workers', (SELECT COALESCE(json_agg(w ORDER BY w.device, w.serial), '[]'::json) FROM ("
                     "      SELECT d.name AS device, c.serial, c.fingerprint_sha256 AS fingerprint, c.subject_cn,"
                     "             c.not_before, c.not_after, c.cert_pem, (c.cert_id = d.cert_id) AS current"
                     "      FROM device_certificate c"
                     "      JOIN root_ca r ON r.ca_id = c.ca_id AND r.name = 'factory'"
                     "      JOIN device d ON d.device_id = c.device_id) w),"
                     "  'crl', (SELECT json_build_object("
                     "      'payload', s.payload, 'signature', s.signature, 'signed_at', s.signed_at,"
                     "      'serials', COALESCE((SELECT json_agg(r.serial ORDER BY r.serial)"
                     "                           FROM manager_revocation r), '[]'::json))"
                     "    FROM manager_crl_signature s)"
                     ")::text")
                .one_field());
    });
}

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------

void DatabaseManager::upsert_session(int device_id, const std::string& state, std::optional<int> pid,
                                     const std::optional<std::string>& error)
{
    // INSERT ... SELECT: a device removed meanwhile inserts nothing instead of raising 23503.
    auto lease = pool_.acquire();
    pqxx::work txn(lease.get());
    txn.exec("INSERT INTO device_session (device_id, state, pid, connected_at, last_seen, last_error, updated_at)"
             " SELECT d.device_id, $2::text, $3,"
             "        CASE WHEN $2::text = 'ONLINE' THEN clock_timestamp() END,"
             "        CASE WHEN $2::text = 'ONLINE' THEN clock_timestamp() END,"
             "        $4, clock_timestamp()"
             " FROM device d WHERE d.device_id = $1"
             " ON CONFLICT (device_id) DO UPDATE SET"
             "   state = EXCLUDED.state, pid = EXCLUDED.pid,"
             "   connected_at = COALESCE(EXCLUDED.connected_at, device_session.connected_at),"
             "   last_seen = COALESCE(EXCLUDED.last_seen, device_session.last_seen),"
             "   last_error = EXCLUDED.last_error, updated_at = EXCLUDED.updated_at"
             " WHERE (device_session.state, device_session.pid, device_session.last_error)"
             "       IS DISTINCT FROM (EXCLUDED.state, EXCLUDED.pid, EXCLUDED.last_error)",
             pqxx::params{device_id, state, pid, error})
        .no_rows();
    txn.commit();
}

void DatabaseManager::touch_sessions(const std::vector<int>& device_ids)
{
    if (device_ids.empty()) return;
    std::string ids = "{";
    for (const int id : device_ids)
    {
        if (ids.size() > 1) ids += ',';
        ids += std::to_string(id);
    }
    ids += '}';
    auto lease = pool_.acquire();
    pqxx::work txn(lease.get());
    txn.exec("UPDATE device_session SET last_seen = clock_timestamp() WHERE device_id = ANY($1::int[])",
             pqxx::params{ids})
        .no_rows();
    txn.commit();
}

void DatabaseManager::reset_sessions()
{
    auto lease = pool_.acquire();
    pqxx::work txn(lease.get());
    txn.exec("UPDATE device_session SET state = 'OFFLINE', pid = NULL, updated_at = clock_timestamp()"
             " WHERE state IN ('STARTING', 'ONLINE')")
        .no_rows();
    txn.commit();
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
