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

std::string_view target_sql(ConfigTarget t) { return table_name(t); }

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

void DatabaseManager::upsert_dummy_with_history(DummyTable table, const DummySample& sample)
{
    const auto live = table_name(table);
    const auto hist = history_table_name(table);
    // One statement: the CTE writes the snapshot, the outer INSERT appends the
    // same values to the partitioned history. Both commit (and notify) together.
    const std::string sql = std::format(
        "WITH up AS ("
        "  INSERT INTO {0} (item_id, value, seq, updated_at) VALUES ($1, $2, $3, clock_timestamp())"
        "  ON CONFLICT (item_id) DO UPDATE SET value = EXCLUDED.value, seq = EXCLUDED.seq,"
        "    updated_at = EXCLUDED.updated_at"
        "  RETURNING item_id, value, seq, updated_at)"
        " INSERT INTO {1} (item_id, value, seq, recorded_at)"
        " SELECT item_id, value, seq, updated_at FROM up",
        live, hist);

    auto lease = pool_.acquire();
    pqxx::work txn(lease.get());
    txn.exec(sql, pqxx::params{sample.item_id, sample.value, sample.seq}).no_rows();
    txn.commit();
}

boost::json::value DatabaseManager::fetch_dummy(DbRole role, DummyTable table)
{
    const std::string sql = std::format(
        "SELECT COALESCE(json_agg(json_build_object('item_id', item_id, 'value', value,"
        " 'seq', seq, 'updated_at', updated_at) ORDER BY item_id), '[]'::json)::text FROM {}",
        table_name(table));
    return with_role(role, [&](pqxx::work& txn) {
        return parse_json_field(txn.exec(sql).one_field());
    });
}

boost::json::value DatabaseManager::fetch_history(DbRole role, DummyTable table,
                                                  const std::optional<std::string>& from,
                                                  const std::optional<std::string>& to,
                                                  std::optional<int> item_id, int limit)
{
    const std::string sql = std::format(
        "SELECT COALESCE(json_agg(r ORDER BY r.recorded_at DESC), '[]'::json)::text FROM ("
        "  SELECT history_id, item_id, value, seq, recorded_at, tableoid::regclass::text AS partition"
        "  FROM {}"
        "  WHERE recorded_at >= COALESCE($1::timestamptz, now() - interval '1 hour')"
        "    AND recorded_at <  COALESCE($2::timestamptz, now() + interval '1 second')"
        "    AND ($3::int IS NULL OR item_id = $3::int)"
        "  ORDER BY recorded_at DESC"
        "  LIMIT $4) r",
        history_table_name(table));
    return with_role(role, [&](pqxx::work& txn) {
        return parse_json_field(txn.exec(sql, pqxx::params{from, to, item_id, limit}).one_field());
    });
}

std::vector<std::string> DatabaseManager::ensure_history_partitions(DummyTable table, int months_ahead)
{
    auto lease = pool_.acquire();
    pqxx::work txn(lease.get());
    const auto res = txn.exec("SELECT ensure_monthly_partitions($1::regclass, NULL, $2)",
                              pqxx::params{std::string(history_table_name(table)), months_ahead});
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
        const pqxx::params p{r.request_id, std::string(target_sql(r.target)), r.rate_ms,
                             r.amplitude,  r.is_active,                        r.requested_by};
        txn.exec("INSERT INTO dummy_config_pending (request_id, target, requested_rate_ms,"
                 " requested_amplitude, requested_is_active, requested_by, status)"
                 " VALUES ($1, $2, $3, $4, $5, $6, 'PENDING')",
                 p)
            .no_rows();
        txn.exec("INSERT INTO dummy_config_pending_history (request_id, target, requested_rate_ms,"
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
    const auto res = txn.exec(
        "SELECT target, rate_ms, amplitude, is_active, request_id FROM dummy_config ORDER BY target");
    txn.commit();

    std::vector<AppliedConfig> out;
    for (const auto& row : res)
    {
        const auto target = parse_dummy_table(row[0].view());
        if (!target) continue;
        AppliedConfig c;
        c.target = *target;
        c.rate_ms = row[1].as<int>();
        c.amplitude = row[2].as<double>();
        c.is_active = row[3].as<bool>();
        if (!row[4].is_null()) c.request_id = row[4].as<std::int64_t>();
        out.push_back(c);
    }
    return out;
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
        const auto res = txn.exec(
            "SELECT request_id, target, requested_rate_ms, requested_amplitude, requested_is_active,"
            " requested_by, extract(epoch FROM clock_timestamp() - requested_at)::float8"
            " FROM dummy_config_pending"
            " WHERE status = 'PENDING'"
            " ORDER BY requested_at, request_id"
            " LIMIT 1"
            " FOR UPDATE SKIP LOCKED");
        if (res.empty())
        {
            txn.commit();
            break;
        }

        const auto row = res[0];
        ConfigRequest req;
        req.request_id = row[0].as<std::int64_t>();
        const auto target = parse_dummy_table(row[1].view());
        req.target = target.value_or(ConfigTarget::Dummy1);
        req.rate_ms = row[2].as<int>();
        req.amplitude = row[3].as<double>();
        req.is_active = row[4].as<bool>();
        req.requested_by = row[5].as<std::string>();
        const double age_s = row[6].as<double>();

        ConfigOutcome outcome;
        bool expired = false;
        if (!target)
        {
            outcome = ConfigOutcome::failure("UNKNOWN_TARGET", "target is not dummy1 or dummy2");
        }
        else if (age_s > static_cast<double>(max_age.count()))
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
            const pqxx::params p{std::string(target_sql(req.target)), req.rate_ms, req.amplitude,
                                 req.is_active, req.request_id};
            txn.exec("INSERT INTO dummy_config (target, rate_ms, amplitude, is_active, request_id, applied_at)"
                     " VALUES ($1, $2, $3, $4, $5, clock_timestamp())"
                     " ON CONFLICT (target) DO UPDATE SET rate_ms = EXCLUDED.rate_ms,"
                     " amplitude = EXCLUDED.amplitude, is_active = EXCLUDED.is_active,"
                     " request_id = EXCLUDED.request_id, applied_at = EXCLUDED.applied_at",
                     p)
                .no_rows();
            txn.exec("INSERT INTO dummy_config_history (target, rate_ms, amplitude, is_active, request_id)"
                     " VALUES ($1, $2, $3, $4, $5)",
                     p)
                .no_rows();
            txn.exec("UPDATE dummy_config_pending SET status = 'SUCCESS', completed_at = clock_timestamp()"
                     " WHERE request_id = $1",
                     pqxx::params{req.request_id})
                .no_rows();
            txn.commit();

            ++stats.applied;
            log::info(kLog, std::format("request {} applied to {} (rate_ms={} amplitude={} active={})",
                                        req.request_id, target_sql(req.target), req.rate_ms,
                                        req.amplitude, req.is_active));
            if (on_applied)
            {
                on_applied(AppliedConfig{req.target, req.rate_ms, req.amplitude, req.is_active,
                                         req.request_id});
            }
        }
        else
        {
            txn.exec("INSERT INTO dummy_config_errors (request_id, target, error_code, error_detail)"
                     " VALUES ($1, $2, $3, $4)",
                     pqxx::params{req.request_id, row[1].as<std::string>(), outcome.error_code,
                                  outcome.error_detail})
                .no_rows();
            txn.exec(std::format("UPDATE dummy_config_pending SET status = '{}',"
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
            txn.exec("SELECT COALESCE(json_agg(v ORDER BY v.target), '[]'::json)::text"
                     " FROM v_dummy_config_sync_status v")
                .one_field());
    });
}

std::optional<boost::json::value> DatabaseManager::fetch_request(DbRole role, std::int64_t request_id)
{
    return with_role(role, [&](pqxx::work& txn) -> std::optional<boost::json::value> {
        const auto res = txn.exec(
            "SELECT row_to_json(r)::text FROM ("
            "  SELECT p.request_id, p.target, p.requested_rate_ms, p.requested_amplitude,"
            "         p.requested_is_active, p.status, p.requested_by, p.requested_at, p.completed_at,"
            "         e.error_code, e.error_detail"
            "  FROM dummy_config_pending p"
            "  LEFT JOIN dummy_config_errors e ON e.request_id = p.request_id"
            "  WHERE p.request_id = $1) r",
            pqxx::params{request_id});
        if (res.empty()) return std::nullopt;
        return parse_json_field(res[0][0]);
    });
}

boost::json::value DatabaseManager::fetch_config_history(DbRole role, std::optional<ConfigTarget> target,
                                                         int limit)
{
    std::optional<std::string> t;
    if (target) t = std::string(target_sql(*target));
    return with_role(role, [&](pqxx::work& txn) {
        return parse_json_field(
            txn.exec("SELECT COALESCE(json_agg(h ORDER BY h.recorded_at DESC, h.history_id DESC),"
                     " '[]'::json)::text FROM ("
                     "  SELECT history_id, target, rate_ms, amplitude, is_active, request_id, recorded_at"
                     "  FROM dummy_config_history"
                     "  WHERE ($1::text IS NULL OR target = $1::text)"
                     "  ORDER BY recorded_at DESC, history_id DESC"
                     "  LIMIT $2) h",
                     pqxx::params{t, limit})
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
