// Integration tests against a database prepared by scripts/db-init.sh.
//
//   PGDEMO_TEST_SERVER_CONNINFO      conninfo of the demo_server login role
//   PGDEMO_TEST_CONTROLLER_CONNINFO  conninfo of the demo_controller login role
//
// Skipped when either is unset. Do not run them while pgdemo-controller is
// running: it would drain the queue the tests are filling.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <ctime>
#include <format>
#include <mutex>
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

bool has(const std::vector<boost::json::object>& msgs, std::string_view channel, int item_id)
{
    for (const auto& m : msgs)
    {
        if (m.at("channel").as_string() != channel) continue;
        const auto& p = m.at("payload").as_object();
        if (p.at("item_id").as_int64() == item_id) return true;
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
        if (server_) server_->disconnect();
        if (controller_) controller_->disconnect();
    }

    db::ConfigRequest request(std::int64_t id, int rate_ms, double amplitude = 10.0)
    {
        return {id, db::DummyTable::Dummy1, rate_ms, amplitude, true, "gtest"};
    }

    std::string status_of(std::int64_t id)
    {
        const auto row = server_->fetch_request(db::DbRole::Normal, id);
        return row ? std::string(row->as_object().at("status").as_string()) : "MISSING";
    }

    // Reached from listener callbacks: declared before the managers so they
    // outlive the listener threads.
    Inbox inbox_;
    server::Dispatcher dispatcher_;
    std::atomic<int> applied_{0};

    std::unique_ptr<db::DatabaseManager> server_;
    std::unique_ptr<db::DatabaseManager> controller_;
};

} // namespace

// Two tables, two channels, one listener socket: libpqxx routes each
// notification to the handler registered for its channel.
TEST_F(DbTest, ListenerMultiplexesBothDataChannels)
{
    for (const auto t : db::kAllTables)
    {
        server_->register_listen_async(std::string(db::channel_name(t)),
                                       [this](boost::json::object m) { inbox_.push(std::move(m)); });
    }
    server_->run_listener_loop();

    controller_->upsert_dummy_with_history(db::DummyTable::Dummy1, {901, 1.5, 1});
    controller_->upsert_dummy_with_history(db::DummyTable::Dummy2, {902, 2.5, 1});

    EXPECT_TRUE(inbox_.wait([](const auto& m) {
        return has(m, "dummy1_events", 901) && has(m, "dummy2_events", 902);
    }));
    EXPECT_FALSE(inbox_.wait([](const auto& m) { return has(m, "dummy1_events", 902); }, 200ms));
}

TEST_F(DbTest, RegisteringAfterTheLoopStartedThrows)
{
    server_->register_listen_async("dummy1_events", [](boost::json::object) {});
    server_->run_listener_loop();
    EXPECT_THROW(server_->register_listen_async("dummy2_events", [](boost::json::object) {}), std::logic_error);
}

TEST_F(DbTest, DisconnectEndsTheBlockedListenerPromptly)
{
    server_->register_listen_async("dummy1_events", [](boost::json::object) {});
    server_->run_listener_loop();
    ASSERT_TRUE(server_->listener_alive());

    const auto t0 = std::chrono::steady_clock::now();
    server_->disconnect();
    EXPECT_FALSE(server_->listener_alive());
    EXPECT_LT(std::chrono::steady_clock::now() - t0, 1s);
}

TEST_F(DbTest, HistoryRowsLandInTheCurrentMonthlyPartition)
{
    controller_->upsert_dummy_with_history(db::DummyTable::Dummy2, {903, 3.0, 42});
    const auto rows = server_->fetch_history(db::DbRole::Normal, db::DummyTable::Dummy2, std::nullopt,
                                             std::nullopt, 903, 1);
    ASSERT_EQ(rows.as_array().size(), 1u);
    const auto& row = rows.as_array()[0].as_object();
    EXPECT_EQ(row.at("seq").as_int64(), 42);
    EXPECT_EQ(row.at("partition").as_string(), current_partition("dummy2_history"));
}

TEST_F(DbTest, EnsurePartitionsIsIdempotent)
{
    controller_->ensure_history_partitions(db::DummyTable::Dummy1);
    EXPECT_TRUE(controller_->ensure_history_partitions(db::DummyTable::Dummy1).empty());
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

    const auto id = server_->generate_request_id(db::DbRole::Admin);
    try
    {
        server_->add_pending_config(db::DbRole::Normal, request(id, 500));
        FAIL() << "INSERT succeeded as demo_normal";
    }
    catch (const std::exception& e)
    {
        EXPECT_EQ(db::classify(e), db::DbErrorKind::Forbidden) << e.what();
    }
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

    auto submit = [&](int rate_ms) {
        const auto id = server_->generate_request_id(db::DbRole::Admin);
        const auto st = dispatcher_.wait_for_response(static_cast<std::uint64_t>(id), 5000, [&] {
            server_->add_pending_config(db::DbRole::Admin, request(id, rate_ms));
        });
        return std::pair{id, st};
    };

    const auto [ok_id, ok] = submit(500);
    EXPECT_EQ(ok, server::ResponseStatus::SUCCESS);
    EXPECT_EQ(status_of(ok_id), "SUCCESS");
    EXPECT_GE(applied_.load(), 1);

    const auto [bad_id, bad] = submit(10);
    EXPECT_EQ(bad, server::ResponseStatus::ERROR);
    EXPECT_EQ(status_of(bad_id), "ERROR");
    const auto row = server_->fetch_request(db::DbRole::Normal, bad_id);
    EXPECT_EQ(row->as_object().at("error_code").as_string(), "INVALID_RATE");
}

TEST_F(DbTest, StaleRequestsExpireInsteadOfApplying)
{
    const auto id = server_->generate_request_id(db::DbRole::Admin);
    server_->add_pending_config(db::DbRole::Admin, request(id, 500));
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
