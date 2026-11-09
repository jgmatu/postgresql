// Integration tests against a database prepared by scripts/db-init.sh.
//
//   PGDEMO_TEST_SERVER_CONNINFO      conninfo of the demo_server login role
//   PGDEMO_TEST_CONTROLLER_CONNINFO  conninfo of the demo_controller login role
//
// Skipped when either is unset. Do not run them while pgdemo-controller is
// running: it would drain the queue the tests are filling, and start producers
// for the devices the tests register.

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <ctime>
#include <format>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <db/DatabaseManager.hpp>
#include <db/Errors.hpp>
#include <dispatcher/Dispatcher.hpp>

using namespace std::chrono_literals;
using namespace pgdemo;

namespace {

/// Thread-safe collector for notifications delivered on the listener thread.
class Inbox
{
public:
    void push(boost::json::object msg)
    {
        {
            std::lock_guard lock(mtx_);
            msgs_.push_back(std::move(msg));
        }
        cv_.notify_all();
    }

    /// Waits until `pred` holds for the collected messages, or the timeout.
    template <typename Pred>
    bool wait(Pred pred, std::chrono::milliseconds timeout = 5000ms)
    {
        std::unique_lock lock(mtx_);
        return cv_.wait_for(lock, timeout, [&] { return pred(msgs_); });
    }

private:
    std::mutex mtx_;
    std::condition_variable cv_;
    std::vector<boost::json::object> msgs_;
};

/// True when a notification on `channel` names `device` (and, if given, `action`).
bool has(const std::vector<boost::json::object>& msgs, std::string_view channel, std::string_view device,
         std::string_view action = {})
{
    for (const auto& m : msgs)
    {
        if (m.at("channel").as_string() != channel) continue;
        const auto& p = m.at("payload").as_object();
        if (p.at("device").as_string() != device) continue;
        if (!action.empty() && p.at("action").as_string() != action) continue;
        return true;
    }
    return false;
}

std::string current_partition(std::string_view parent)
{
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&now, &tm);
    return std::format("{}_y{:04}m{:02}", parent, tm.tm_year + 1900, tm.tm_mon + 1);
}

class DbTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        const char* s = std::getenv("PGDEMO_TEST_SERVER_CONNINFO");
        const char* c = std::getenv("PGDEMO_TEST_CONTROLLER_CONNINFO");
        if (!s || !c) GTEST_SKIP() << "PGDEMO_TEST_SERVER_CONNINFO / PGDEMO_TEST_CONTROLLER_CONNINFO not set";
        server_ = std::make_unique<db::DatabaseManager>(s, "pgdemo-test-server");
        controller_ = std::make_unique<db::DatabaseManager>(c, "pgdemo-test-controller");
        server_->connect();
        controller_->connect();
    }

    void TearDown() override
    {
        if (server_)
        {
            for (const auto& name : created_) server_->delete_device(db::DbRole::Admin, name);
        }
        if (server_) server_->disconnect();
        if (controller_) controller_->disconnect();
    }

    /// Register a device of `type_id` under a name unique to this process.
    db::Device make_device(const std::string& type_id)
    {
        static std::atomic<int> counter{0};
        const auto name = std::format("gtest-{}-{}", ::getpid(), counter.fetch_add(1));
        const auto row = server_->create_device(db::DbRole::Admin, name, type_id);
        if (!row) throw std::runtime_error("unknown device type " + type_id);
        created_.push_back(name);
        const auto& o = row->as_object();
        return db::Device{static_cast<int>(o.at("device_id").as_int64()), name, type_id,
                          db::parse_payload(o.at("payload").as_string()).value()};
    }

    db::ConfigRequest request(std::int64_t id, const db::Device& dev, int rate_ms, double amplitude = 10.0)
    {
        return {id, dev.device_id, dev.name, rate_ms, amplitude, true, "gtest"};
    }

    std::string status_of(std::int64_t id)
    {
        const auto row = server_->fetch_request(db::DbRole::Normal, id);
        return row ? std::string(row->as_object().at("status").as_string()) : "MISSING";
    }

    std::int64_t telemetry_rows(db::Payload payload, int device_id)
    {
        return server_->with_role(db::DbRole::Normal, [&](pqxx::work& txn) {
            return txn
                .exec(std::format("SELECT count(*) FROM {} WHERE device_id = $1", db::telemetry_table(payload)),
                      pqxx::params{device_id})
                .one_field()
                .as<std::int64_t>();
        });
    }

    // Reached from listener callbacks: declared before the managers so they
    // outlive the listener threads.
    Inbox inbox_;
    server::Dispatcher dispatcher_;
    std::atomic<int> applied_{0};

    std::vector<std::string> created_;
    std::unique_ptr<db::DatabaseManager> server_;
    std::unique_ptr<db::DatabaseManager> controller_;
};

} // namespace

// Two payload shapes, two channels, one listener socket, any number of
// devices: the device travels in the payload, not in the channel name.
TEST_F(DbTest, ListenerMultiplexesBothTelemetryChannels)
{
    for (const auto p : db::kAllPayloads)
    {
        server_->register_listen_async(std::string(db::telemetry_channel(p)),
                                       [this](boost::json::object m) { inbox_.push(std::move(m)); });
    }
    server_->run_listener_loop();

    const auto a = make_device("sine");
    const auto b = make_device("sawtooth");
    const auto g = make_device("gps_orbit");
    controller_->write_scalar_sample(db::DummySample{a.device_id, 1.5, 1});
    controller_->write_scalar_sample(db::DummySample{b.device_id, 2.5, 1});
    controller_->write_gps_sample(db::TrackSample{g.device_id, -3.7038, 40.4168, 250.0, 0.0, 1});

    EXPECT_TRUE(inbox_.wait([&](const auto& m) {
        return has(m, "telemetry_scalar", a.name) && has(m, "telemetry_scalar", b.name) &&
               has(m, "telemetry_gps", g.name);
    }));
    EXPECT_FALSE(inbox_.wait([&](const auto& m) { return has(m, "telemetry_gps", a.name); }, 200ms));
}

TEST_F(DbTest, RegisteringAfterTheLoopStartedThrows)
{
    server_->register_listen_async("telemetry_scalar", [](boost::json::object) {});
    server_->run_listener_loop();
    EXPECT_THROW(server_->register_listen_async("telemetry_gps", [](boost::json::object) {}), std::logic_error);
}

TEST_F(DbTest, DisconnectEndsTheBlockedListenerPromptly)
{
    server_->register_listen_async("telemetry_scalar", [](boost::json::object) {});
    server_->run_listener_loop();
    ASSERT_TRUE(server_->listener_alive());

    const auto t0 = std::chrono::steady_clock::now();
    server_->disconnect();
    EXPECT_FALSE(server_->listener_alive());
    EXPECT_LT(std::chrono::steady_clock::now() - t0, 1s);
}

TEST_F(DbTest, GpsTelemetryKeepsThePointAndThePartition)
{
    const auto g = make_device("gps_orbit");
    controller_->write_gps_sample(db::TrackSample{g.device_id, -3.7038, 40.4168, 250.0, 0.0, 7});
    const auto rows = server_->fetch_telemetry(db::DbRole::Normal, g.name, std::nullopt, std::nullopt, 1);
    ASSERT_TRUE(rows);
    ASSERT_EQ(rows->as_array().size(), 1u);
    const auto& row = rows->as_array()[0].as_object();
    EXPECT_EQ(row.at("device_id").as_int64(), g.device_id);
    EXPECT_EQ(row.at("seq").as_int64(), 7);
    EXPECT_EQ(row.at("partition").as_string(), current_partition("telemetry_gps"));
    // Bearing 0 is north, so PostGIS moves the point off the origin.
    EXPECT_TRUE(row.at("lon").is_number());
    EXPECT_GT(row.at("lat").to_number<double>(), 40.4168);
}

TEST_F(DbTest, TelemetryRowsLandInTheCurrentMonthlyPartition)
{
    const auto s = make_device("sawtooth");
    controller_->write_scalar_sample(db::DummySample{s.device_id, 3.0, 42});
    const auto rows = server_->fetch_telemetry(db::DbRole::Normal, s.name, std::nullopt, std::nullopt, 1);
    ASSERT_TRUE(rows);
    ASSERT_EQ(rows->as_array().size(), 1u);
    const auto& row = rows->as_array()[0].as_object();
    EXPECT_EQ(row.at("seq").as_int64(), 42);
    EXPECT_EQ(row.at("value").to_number<double>(), 3.0);
    EXPECT_EQ(row.at("partition").as_string(), current_partition("telemetry_scalar"));

    const auto device = server_->fetch_device(db::DbRole::Normal, s.name);
    ASSERT_TRUE(device);
    EXPECT_EQ(device->as_object().at("seq").as_int64(), 42);
}

TEST_F(DbTest, EnsurePartitionsIsIdempotent)
{
    for (const auto p : db::kAllPayloads)
    {
        controller_->ensure_history_partitions(p);
        EXPECT_TRUE(controller_->ensure_history_partitions(p).empty());
    }
}

// The authorization lives in PostgreSQL too: demo_normal has no INSERT on the
// queue and no USAGE on the sequence.
TEST_F(DbTest, NormalRoleCannotEnqueue)
{
    try
    {
        server_->generate_request_id(db::DbRole::Normal);
        FAIL() << "nextval succeeded as demo_normal";
    }
    catch (const std::exception& e)
    {
        EXPECT_EQ(db::classify(e), db::DbErrorKind::Forbidden) << e.what();
    }

    const auto dev = make_device("sine");
    const auto id = server_->generate_request_id(db::DbRole::Admin);
    try
    {
        server_->add_pending_config(db::DbRole::Normal, request(id, dev, 500));
        FAIL() << "INSERT succeeded as demo_normal";
    }
    catch (const std::exception& e)
    {
        EXPECT_EQ(db::classify(e), db::DbErrorKind::Forbidden) << e.what();
    }
}

TEST_F(DbTest, NormalRoleCannotRegisterOrRemoveDevices)
{
    try
    {
        server_->create_device(db::DbRole::Normal, "gtest-normal", "sine");
        FAIL() << "INSERT INTO device succeeded as demo_normal";
    }
    catch (const std::exception& e)
    {
        EXPECT_EQ(db::classify(e), db::DbErrorKind::Forbidden) << e.what();
    }

    const auto dev = make_device("sine");
    try
    {
        server_->delete_device(db::DbRole::Normal, dev.name);
        FAIL() << "DELETE FROM device succeeded as demo_normal";
    }
    catch (const std::exception& e)
    {
        EXPECT_EQ(db::classify(e), db::DbErrorKind::Forbidden) << e.what();
    }
}

TEST_F(DbTest, DeviceNamesAreUniqueAndTypesMustExist)
{
    const auto dev = make_device("sine");
    try
    {
        server_->create_device(db::DbRole::Admin, dev.name, "sawtooth");
        FAIL() << "duplicate device name accepted";
    }
    catch (const std::exception& e)
    {
        EXPECT_EQ(db::classify(e), db::DbErrorKind::Conflict) << e.what();
    }
    EXPECT_FALSE(server_->create_device(db::DbRole::Admin, "gtest-no-type", "no_such_type"));
}

TEST_F(DbTest, NormalRoleCannotListUsers)
{
    EXPECT_THROW(server_->list_users(db::DbRole::Normal), pqxx::sql_error);
    EXPECT_NO_THROW(server_->list_users(db::DbRole::Admin));
}

// The whole control-plane loop with the production pieces: Dispatcher on the
// server side, queue drain on config_requested on the controller side, and the
// completion coming back as config_events / error_events.
TEST_F(DbTest, ConfigLoopAppliesAndRejectsThroughNotify)
{
    server_->register_listen_async("config_events", [this](boost::json::object m) {
        const auto& id = m.at("payload").as_object().at("request_id");
        if (id.is_int64())
            dispatcher_.dispatch(static_cast<std::uint64_t>(id.as_int64()), server::ResponseStatus::SUCCESS);
    });
    server_->register_listen_async("error_events", [this](boost::json::object m) {
        const auto id = m.at("payload").as_object().at("request_id").as_int64();
        dispatcher_.dispatch(static_cast<std::uint64_t>(id), server::ResponseStatus::ERROR);
    });
    server_->run_listener_loop();

    controller_->register_listen_async("config_requested", [this](boost::json::object) {
        controller_->drain_pending(
            [](const db::ConfigRequest& r) {
                return r.rate_ms >= 50 ? db::ConfigOutcome::success()
                                       : db::ConfigOutcome::failure("INVALID_RATE", "too fast");
            },
            [this](const db::AppliedConfig&) { applied_.fetch_add(1); }, 600s);
    });
    controller_->run_listener_loop();

    const auto dev = make_device("sine");
    const auto initial = controller_->init_device_config(dev.device_id);
    ASSERT_TRUE(initial);
    EXPECT_EQ(initial->rate_ms, 500);
    EXPECT_FALSE(initial->request_id);

    auto submit = [&](int rate_ms) {
        const auto id = server_->generate_request_id(db::DbRole::Admin);
        const auto st = dispatcher_.wait_for_response(static_cast<std::uint64_t>(id), 5000, [&] {
            server_->add_pending_config(db::DbRole::Admin, request(id, dev, rate_ms));
        });
        return std::pair{id, st};
    };

    const auto [ok_id, ok] = submit(700);
    EXPECT_EQ(ok, server::ResponseStatus::SUCCESS);
    EXPECT_EQ(status_of(ok_id), "SUCCESS");
    EXPECT_GE(applied_.load(), 1);
    const auto sync = server_->fetch_device_sync(db::DbRole::Normal, dev.name);
    ASSERT_TRUE(sync);
    EXPECT_EQ(sync->as_object().at("applied_rate_ms").as_int64(), 700);
    EXPECT_EQ(sync->as_object().at("sync").as_string(), "SYNC");

    const auto [bad_id, bad] = submit(10);
    EXPECT_EQ(bad, server::ResponseStatus::ERROR);
    EXPECT_EQ(status_of(bad_id), "ERROR");
    const auto row = server_->fetch_request(db::DbRole::Normal, bad_id);
    EXPECT_EQ(row->as_object().at("error_code").as_string(), "INVALID_RATE");
    EXPECT_EQ(row->as_object().at("device").as_string(), dev.name);

    const auto history = server_->fetch_config_history(db::DbRole::Normal, dev.name, 10);
    EXPECT_EQ(history.as_array().size(), 2u); // defaults, then request ok_id
}

TEST_F(DbTest, StaleRequestsExpireInsteadOfApplying)
{
    const auto dev = make_device("sine");
    const auto id = server_->generate_request_id(db::DbRole::Admin);
    server_->add_pending_config(db::DbRole::Admin, request(id, dev, 500));
    std::this_thread::sleep_for(1100ms);

    bool decided = false;
    const auto stats = controller_->drain_pending(
        [&](const db::ConfigRequest&) {
            decided = true;
            return db::ConfigOutcome::success();
        },
        [](const db::AppliedConfig&) {}, 1s);
    EXPECT_GE(stats.expired, 1u);
    EXPECT_EQ(status_of(id), "TIMEOUT");
    EXPECT_FALSE(decided);
}

// Registering and removing a device is a row change announced on
// device_events. Removal takes state, configuration and queue with it and
// keeps the telemetry.
TEST_F(DbTest, DeviceLifecycleEmitsDeviceEventsAndKeepsTelemetry)
{
    server_->register_listen_async("device_events", [this](boost::json::object m) { inbox_.push(std::move(m)); });
    server_->run_listener_loop();

    const auto dev = make_device("sine");
    EXPECT_TRUE(inbox_.wait([&](const auto& m) { return has(m, "device_events", dev.name, "INSERT"); }));

    const auto loaded = controller_->load_device(dev.device_id);
    ASSERT_TRUE(loaded);
    EXPECT_EQ(loaded->name, dev.name);
    EXPECT_EQ(loaded->payload, db::Payload::Scalar);
    ASSERT_TRUE(controller_->init_device_config(dev.device_id));
    controller_->write_scalar_sample(db::DummySample{dev.device_id, 1.0, 1});
    controller_->write_scalar_sample(db::DummySample{dev.device_id, 2.0, 2});
    const auto queued = server_->generate_request_id(db::DbRole::Admin);
    server_->add_pending_config(db::DbRole::Admin, request(queued, dev, 900));

    ASSERT_TRUE(server_->delete_device(db::DbRole::Admin, dev.name));
    std::erase(created_, dev.name);
    EXPECT_TRUE(inbox_.wait([&](const auto& m) { return has(m, "device_events", dev.name, "DELETE"); }));

    EXPECT_FALSE(server_->fetch_device(db::DbRole::Normal, dev.name));
    EXPECT_FALSE(controller_->load_device(dev.device_id));
    EXPECT_FALSE(controller_->init_device_config(dev.device_id));
    EXPECT_EQ(status_of(queued), "MISSING");
    EXPECT_FALSE(server_->delete_device(db::DbRole::Admin, dev.name));
    EXPECT_EQ(telemetry_rows(db::Payload::Scalar, dev.device_id), 2);
}

TEST(DatabaseManagerPool, ZeroSizeIsRejected)
{
    EXPECT_THROW(db::DatabaseManager("dbname=postgres", "pgdemo-test-pool", 0), std::invalid_argument);
}

// More callers than connections: checkout blocks instead of sharing a session.
TEST_F(DbTest, QueryPoolServesConcurrentCheckouts)
{
    const char* conninfo = std::getenv("PGDEMO_TEST_CONTROLLER_CONNINFO");
    ASSERT_NE(conninfo, nullptr);

    db::DatabaseManager db(conninfo, "pgdemo-test-pool", 4);
    db.connect();

    constexpr int kThreads = 8;
    std::atomic<int> ok{0};
    std::mutex errors_mtx;
    std::vector<std::string> errors;
    std::vector<std::jthread> threads;
    threads.reserve(static_cast<std::size_t>(kThreads));
    for (int i = 0; i < kThreads; ++i)
    {
        threads.emplace_back([&] {
            try
            {
                for (int n = 0; n < 20; ++n)
                {
                    const auto info = db.get_sanity_info();
                    if (info.at("pool_size").as_int64() != 4) throw std::runtime_error("pool_size");
                    if (info.at("query_backend_pids").as_array().size() != 4)
                        throw std::runtime_error("query_backend_pids");
                    const auto in_use = info.at("pool_in_use").as_int64();
                    if (in_use < 1 || in_use > 4) throw std::runtime_error("pool_in_use");
                    db.fetch_applied_config();
                }
                ok.fetch_add(1);
            }
            catch (const std::exception& e)
            {
                std::lock_guard lock(errors_mtx);
                errors.emplace_back(e.what());
            }
        });
    }
    threads.clear();

    for (const auto& error : errors) ADD_FAILURE() << error;
    EXPECT_EQ(ok.load(), kThreads);
    db.disconnect();
}
