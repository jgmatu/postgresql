// Integration tests against a database prepared by scripts/db-init.sh.
//
//   PGDEMO_TEST_SERVER_CONNINFO      conninfo of the demo_server login role
//   PGDEMO_TEST_CONTROLLER_CONNINFO  conninfo of the demo_controller login role
//   PGDEMO_TEST_CA_DIR               device CA of that database (default <repo>/run/ca)
//   PGDEMO_TEST_OWNER_CONNINFO       optional, a role that may write `device`
//                                    directly (e.g. postgres), for the
//                                    foreign-key tests that no app role can reach
//
// Skipped when either app conninfo is unset. Devices are enrolled the way the
// server and controller do it: queue, CSR from a fresh P-384 key, signed by
// the CA, issue_device_certificate(). Do not run them while pgdemo-controller
// or pgdemo-server is running: they would drain the queues the tests fill.

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <db/DatabaseManager.hpp>
#include <db/Errors.hpp>
#include <dispatcher/Dispatcher.hpp>
#include <pki/CertificateAuthority.hpp>
#include <pki/DeviceKey.hpp>

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
        const char* ca_dir = std::getenv("PGDEMO_TEST_CA_DIR");
        const std::filesystem::path dir = ca_dir ? ca_dir : PGDEMO_SOURCE_DIR "/run/ca";
        if (!std::filesystem::exists(dir / "ca.key")) GTEST_SKIP() << (dir / "ca.key") << " not found";
        ca_ = std::make_unique<pki::CertificateAuthority>((dir / "ca.key").string(), (dir / "ca.crt").string());
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

    /// A device name unique to this process.
    static std::string unique_name()
    {
        static std::atomic<int> counter{0};
        return std::format("gtest-{}-{}", ::getpid(), counter.fetch_add(1));
    }

    /// The CSR step of the device process and the signing step of the server
    /// for an open request. @return the certificate it was issued
    pki::CertInfo issue(std::int64_t request_id, const std::string& name)
    {
        const auto key = pki::DeviceKey::generate();
        if (!controller_->submit_csr(request_id, key.csr_pem(name)))
            throw std::runtime_error("request is not PENDING");
        const auto csr = server_->fetch_csr(request_id);
        if (!csr || !csr->csr_pem) throw std::runtime_error("CSR not stored");
        auto cert = ca_->sign_csr(*csr->csr_pem, name);
        server_->issue_certificate(request_id, ca_->fingerprint(), to_issued(cert));
        return cert;
    }

    static db::IssuedCertificate to_issued(const pki::CertInfo& c)
    {
        return {c.serial, c.fingerprint, c.subject_cn, c.not_before, c.not_after, c.pem};
    }

    /// Enroll a device of `type_id` under a name unique to this process.
    db::Device make_device(const std::string& type_id)
    {
        const auto name = unique_name();
        const auto id = server_->generate_request_id(db::DbRole::Admin);
        if (!server_->enqueue_enrollment(db::DbRole::Admin, id, name, type_id, "gtest"))
            throw std::runtime_error("unknown device type " + type_id);
        created_.push_back(name);
        serials_[name] = issue(id, name).serial;
        const auto loaded = controller_->load_devices();
        for (const auto& d : loaded)
            if (d.name == name) return d;
        throw std::runtime_error("enrolled device " + name + " not loaded");
    }

    /// Re-key `dev` through a rotation. @return the serial of the new certificate
    std::string rotate(const db::Device& dev)
    {
        const auto id = server_->generate_request_id(db::DbRole::Admin);
        if (!server_->enqueue_rotation(db::DbRole::Admin, id, dev.name, "gtest"))
            throw std::runtime_error("no device " + dev.name);
        return serials_[dev.name] = issue(id, dev.name).serial;
    }

    /// `identity` of the device as the API shows it, or MISSING.
    std::string identity_of(const std::string& name)
    {
        const auto row = server_->fetch_device(db::DbRole::Normal, name);
        return row ? std::string(row->as_object().at("identity").as_string()) : "MISSING";
    }

    std::size_t certificate_count(const std::string& name)
    {
        const auto certs = server_->fetch_certificates(db::DbRole::Normal, name);
        return certs ? certs->as_array().size() : 0;
    }

    bool revoked(const std::string& serial) { return controller_->load_revoked_serials().contains(serial); }

    void forget(const std::string& name) { std::erase(created_, name); }

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
    std::map<std::string, std::string> serials_; ///< current serial of each enrolled device
    std::unique_ptr<pki::CertificateAuthority> ca_;
    std::unique_ptr<db::DatabaseManager> server_;
    std::unique_ptr<db::DatabaseManager> controller_;
};

} // namespace

int count_channel(const std::vector<boost::json::object>& msgs, std::string_view channel)
{
    int n = 0;
    for (const auto& m : msgs)
        if (m.at("channel").as_string() == channel) ++n;
    return n;
}

// One channel per payload, one listener socket. A telemetry notification
// names the table, once per INSERT statement.
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
    const auto c = make_device("scatter");
    const auto cube = make_device("cloud");
    controller_->write_scalar_sample(db::DummySample{a.device_id, 1.5, 1});
    controller_->write_scalar_sample(db::DummySample{b.device_id, 2.5, 1});
    controller_->write_gps_sample(db::TrackSample{g.device_id, -3.7038, 40.4168, 250.0, 0.0, 1});
    controller_->write_xy_sample(db::XySample{c.device_id, -12.5, 40.0, 1});
    controller_->write_xyz_sample(db::XyzSample{cube.device_id, 1.0, -2.0, 3.5, 1});

    EXPECT_TRUE(inbox_.wait([&](const auto& m) {
        return count_channel(m, "telemetry_scalar") == 2 && count_channel(m, "telemetry_gps") == 1 &&
               count_channel(m, "telemetry_xy") == 1 && count_channel(m, "telemetry_xyz") == 1;
    }));
    EXPECT_TRUE(inbox_.wait(
        [&](const auto& m) {
            for (const auto& msg : m)
            {
                if (msg.at("payload").as_object().at("table").as_string() != msg.at("channel").as_string()) return false;
            }
            return true;
        },
        200ms));

    controller_->write_telemetry_batch(
        {db::DummySample{a.device_id, 3.0, 2}, db::DummySample{a.device_id, 4.0, 3}, db::DummySample{b.device_id, 5.0, 2}},
        {});
    EXPECT_TRUE(inbox_.wait([&](const auto& m) { return count_channel(m, "telemetry_scalar") == 3; }));
    EXPECT_TRUE(inbox_.wait([&](const auto& m) { return count_channel(m, "telemetry_gps") == 1; }, 200ms));
    EXPECT_TRUE(inbox_.wait([&](const auto& m) { return count_channel(m, "telemetry_xy") == 1; }, 200ms));
}

// The cursor moves to the last row of the select. A second read is empty,
// and a later insert returns only the new row.
TEST_F(DbTest, TelemetryCursorAdvancesPastABatch)
{
    for (const auto p : db::kAllPayloads)
    {
        for (;;)
        {
            if (server_->take_unread_telemetry(db::telemetry_channel(p)).size() < db::kTelemetryReadPage) break;
        }
    }

    const auto dev = make_device("sine");
    controller_->write_telemetry_batch(
        {db::DummySample{dev.device_id, 1.0, 10}, db::DummySample{dev.device_id, 2.0, 11},
         db::DummySample{dev.device_id, 3.0, 12}},
        {});

    const auto page = server_->take_unread_telemetry("telemetry_scalar");
    ASSERT_EQ(page.size(), 3u);
    for (std::size_t i = 0; i < page.size(); ++i)
    {
        EXPECT_EQ(page[i].at("device").as_string(), dev.name);
        EXPECT_EQ(page[i].at("action").as_string(), "INSERT");
        EXPECT_EQ(page[i].at("seq").as_int64(), static_cast<std::int64_t>(10 + i));
        EXPECT_EQ(page[i].at("value").to_number<double>(), static_cast<double>(i + 1));
    }
    EXPECT_TRUE(server_->take_unread_telemetry("telemetry_scalar").empty());

    controller_->write_scalar_sample(db::DummySample{dev.device_id, 9.0, 13});
    const auto next = server_->take_unread_telemetry("telemetry_scalar");
    ASSERT_EQ(next.size(), 1u);
    EXPECT_EQ(next[0].at("seq").as_int64(), 13);
    EXPECT_EQ(next[0].at("value").to_number<double>(), 9.0);
    EXPECT_TRUE(server_->take_unread_telemetry("telemetry_scalar").empty());

    const auto gps = make_device("gps_orbit");
    controller_->write_gps_sample(db::TrackSample{gps.device_id, -3.7038, 40.4168, 250.0, 0.0, 1});
    const auto fixes = server_->take_unread_telemetry("telemetry_gps");
    ASSERT_EQ(fixes.size(), 1u);
    EXPECT_EQ(fixes[0].at("device").as_string(), gps.name);
    EXPECT_EQ(fixes[0].at("seq").as_int64(), 1);
    EXPECT_GT(fixes[0].at("lat").to_number<double>(), 40.4168);
    EXPECT_TRUE(server_->take_unread_telemetry("telemetry_gps").empty());

    const auto cloud = make_device("scatter");
    controller_->write_telemetry_batch({}, {}, {db::XySample{cloud.device_id, -12.5, 40.0, 4}});
    const auto points = server_->take_unread_telemetry("telemetry_xy");
    ASSERT_EQ(points.size(), 1u);
    EXPECT_EQ(points[0].at("device").as_string(), cloud.name);
    EXPECT_EQ(points[0].at("seq").as_int64(), 4);
    EXPECT_DOUBLE_EQ(points[0].at("x").to_number<double>(), -12.5);
    EXPECT_DOUBLE_EQ(points[0].at("y").to_number<double>(), 40.0);
    EXPECT_TRUE(server_->take_unread_telemetry("telemetry_xy").empty());

    const auto cube = make_device("cloud");
    controller_->write_telemetry_batch({}, {}, {}, {db::XyzSample{cube.device_id, 4.0, -5.0, 6.5, 8}});
    const auto cubes = server_->take_unread_telemetry("telemetry_xyz");
    ASSERT_EQ(cubes.size(), 1u);
    EXPECT_EQ(cubes[0].at("device").as_string(), cube.name);
    EXPECT_EQ(cubes[0].at("seq").as_int64(), 8);
    EXPECT_DOUBLE_EQ(cubes[0].at("x").to_number<double>(), 4.0);
    EXPECT_DOUBLE_EQ(cubes[0].at("y").to_number<double>(), -5.0);
    EXPECT_DOUBLE_EQ(cubes[0].at("z").to_number<double>(), 6.5);
    EXPECT_TRUE(server_->take_unread_telemetry("telemetry_xyz").empty());
}

TEST_F(DbTest, XyzTelemetryKeepsThePointAndThePartition)
{
    for (;;)
    {
        if (server_->take_unread_telemetry("telemetry_xyz").size() < db::kTelemetryReadPage) break;
    }
    const auto d = make_device("cloud");
    controller_->write_xyz_sample(db::XyzSample{d.device_id, 3.5, -8.0, 12.25, 9});
    const auto rows = server_->fetch_telemetry(db::DbRole::Normal, d.name, std::nullopt, std::nullopt, 1);
    ASSERT_TRUE(rows);
    ASSERT_EQ(rows->as_array().size(), 1u);
    const auto& row = rows->as_array()[0].as_object();
    EXPECT_EQ(row.at("seq").as_int64(), 9);
    EXPECT_DOUBLE_EQ(row.at("x").to_number<double>(), 3.5);
    EXPECT_DOUBLE_EQ(row.at("y").to_number<double>(), -8.0);
    EXPECT_DOUBLE_EQ(row.at("z").to_number<double>(), 12.25);
    EXPECT_EQ(row.at("partition").as_string(), current_partition("telemetry_xyz"));

    const auto device = server_->fetch_device(db::DbRole::Normal, d.name);
    ASSERT_TRUE(device);
    EXPECT_EQ(device->as_object().at("payload").as_string(), "xyz");
    EXPECT_DOUBLE_EQ(device->as_object().at("x").to_number<double>(), 3.5);
    EXPECT_DOUBLE_EQ(device->as_object().at("y").to_number<double>(), -8.0);
    EXPECT_DOUBLE_EQ(device->as_object().at("z").to_number<double>(), 12.25);
    EXPECT_TRUE(device->as_object().at("value").is_null());
}

TEST_F(DbTest, XyTelemetryKeepsThePointAndThePartition)
{
    for (;;)
    {
        if (server_->take_unread_telemetry("telemetry_xy").size() < db::kTelemetryReadPage) break;
    }
    const auto d = make_device("scatter");
    controller_->write_xy_sample(db::XySample{d.device_id, 3.5, -8.0, 9});
    const auto rows = server_->fetch_telemetry(db::DbRole::Normal, d.name, std::nullopt, std::nullopt, 1);
    ASSERT_TRUE(rows);
    ASSERT_EQ(rows->as_array().size(), 1u);
    const auto& row = rows->as_array()[0].as_object();
    EXPECT_EQ(row.at("seq").as_int64(), 9);
    EXPECT_DOUBLE_EQ(row.at("x").to_number<double>(), 3.5);
    EXPECT_DOUBLE_EQ(row.at("y").to_number<double>(), -8.0);
    EXPECT_EQ(row.at("partition").as_string(), current_partition("telemetry_xy"));

    const auto device = server_->fetch_device(db::DbRole::Normal, d.name);
    ASSERT_TRUE(device);
    EXPECT_EQ(device->as_object().at("payload").as_string(), "xy");
    EXPECT_DOUBLE_EQ(device->as_object().at("x").to_number<double>(), 3.5);
    EXPECT_DOUBLE_EQ(device->as_object().at("y").to_number<double>(), -8.0);
    EXPECT_TRUE(device->as_object().at("value").is_null());
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

/// Runs `fn`; expects it to throw an error that classifies as `kind`.
template <typename Fn>
void expect_db_error(db::DbErrorKind kind, Fn fn, std::string_view what)
{
    try
    {
        fn();
        ADD_FAILURE() << what << " succeeded";
    }
    catch (const std::exception& e)
    {
        EXPECT_EQ(db::classify(e), kind) << what << ": " << e.what();
    }
}

TEST_F(DbTest, NormalRoleCannotEnrollRevokeOrRemoveDevices)
{
    const auto dev = make_device("sine");
    const auto id = server_->generate_request_id(db::DbRole::Admin);
    const auto F = db::DbErrorKind::Forbidden;
    expect_db_error(F, [&] { server_->enqueue_enrollment(db::DbRole::Normal, id, "gtest-normal", "sine", "x"); },
                    "enrollment as demo_normal");
    expect_db_error(F, [&] { server_->enqueue_rotation(db::DbRole::Normal, id, dev.name, "x"); },
                    "rotation as demo_normal");
    expect_db_error(F, [&] { server_->revoke_certificate(db::DbRole::Normal, dev.name, "keyCompromise", "x"); },
                    "revocation as demo_normal");
    expect_db_error(F, [&] { server_->delete_certificate(db::DbRole::Normal, dev.name, serials_[dev.name]); },
                    "certificate deletion as demo_normal");
    expect_db_error(F, [&] { server_->delete_device(db::DbRole::Normal, dev.name); }, "device deletion as demo_normal");
    EXPECT_EQ(identity_of(dev.name), "ACTIVE");
}

// Not even the admin writes `device` or `device_certificate`: a device comes
// into existence only through issue_device_certificate().
TEST_F(DbTest, AdminCannotCreateDevicesOrCertificatesDirectly)
{
    const auto dev = make_device("sine");
    const auto F = db::DbErrorKind::Forbidden;
    expect_db_error(F,
                    [&] {
                        server_->with_role(db::DbRole::Admin, [&](pqxx::work& txn) {
                            txn.exec("INSERT INTO device (name, type_id, cert_id) VALUES ('gtest-direct', 'sine', 1)");
                        });
                    },
                    "INSERT INTO device as demo_admin");
    expect_db_error(F,
                    [&] {
                        server_->with_role(db::DbRole::Admin, [&](pqxx::work& txn) {
                            txn.exec("UPDATE device SET cert_id = cert_id WHERE device_id = $1",
                                     pqxx::params{dev.device_id});
                        });
                    },
                    "UPDATE device as demo_admin");
}

TEST_F(DbTest, DeviceNamesAreUniqueAndTypesMustExist)
{
    const auto dev = make_device("sine");
    const auto id = server_->generate_request_id(db::DbRole::Admin);
    expect_db_error(db::DbErrorKind::Conflict,
                    [&] { server_->enqueue_enrollment(db::DbRole::Admin, id, dev.name, "sawtooth", "gtest"); },
                    "enrollment of an existing name");
    EXPECT_FALSE(server_->enqueue_enrollment(db::DbRole::Admin, id, unique_name(), "no_such_type", "gtest"));

    // One open request per name.
    const auto name = unique_name();
    ASSERT_TRUE(server_->enqueue_enrollment(db::DbRole::Admin, id, name, "sine", "gtest"));
    const auto again = server_->generate_request_id(db::DbRole::Admin);
    expect_db_error(db::DbErrorKind::Conflict,
                    [&] { server_->enqueue_enrollment(db::DbRole::Admin, again, name, "sine", "gtest"); },
                    "second open enrollment");
    EXPECT_TRUE(controller_->fail_enrollment(id, "GTEST", "cleanup"));
    EXPECT_FALSE(controller_->fail_enrollment(id, "GTEST", "twice"));
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

// ---------------------------------------------------------------- identity

// The device does not exist until its certificate does; then both appear in
// one transaction, with the type defaults, and the request becomes ISSUED.
TEST_F(DbTest, EnrollmentCreatesTheDeviceWithItsFirstCertificate)
{
    server_->register_listen_async("enrollment_events", [this](boost::json::object m) { inbox_.push(std::move(m)); });
    server_->run_listener_loop();

    const auto name = unique_name();
    const auto id = server_->generate_request_id(db::DbRole::Admin);
    ASSERT_TRUE(server_->enqueue_enrollment(db::DbRole::Admin, id, name, "gps_orbit", "gtest"));
    created_.push_back(name);
    EXPECT_EQ(identity_of(name), "MISSING");
    const auto pending = controller_->load_enrollment(id);
    ASSERT_TRUE(pending);
    EXPECT_EQ(pending->status, "PENDING");
    EXPECT_EQ(pending->payload, db::Payload::Gps);

    const auto cert = issue(id, name);
    EXPECT_TRUE(inbox_.wait([&](const auto& m) { return has(m, "enrollment_events", name); }));
    const auto outcome = controller_->enrollment_outcome(id);
    ASSERT_TRUE(outcome);
    EXPECT_EQ(outcome->status, "ISSUED");
    EXPECT_EQ(outcome->cert_pem, cert.pem);

    EXPECT_EQ(identity_of(name), "ACTIVE");
    const auto certs = server_->fetch_certificates(db::DbRole::Normal, name);
    ASSERT_TRUE(certs);
    ASSERT_EQ(certs->as_array().size(), 1u);
    const auto& c = certs->as_array()[0].as_object();
    EXPECT_EQ(c.at("serial").as_string(), cert.serial);
    EXPECT_TRUE(c.at("current").as_bool());
    const auto row = server_->fetch_device(db::DbRole::Normal, name);
    ASSERT_TRUE(row);
    EXPECT_TRUE(controller_->init_device_config(static_cast<int>(row->as_object().at("device_id").as_int64())));
}

// issue_device_certificate() checks again what the server already checked.
TEST_F(DbTest, IssuingChecksTheNameTheCaAndTheState)
{
    const auto name = unique_name();
    const auto id = server_->generate_request_id(db::DbRole::Admin);
    ASSERT_TRUE(server_->enqueue_enrollment(db::DbRole::Admin, id, name, "sine", "gtest"));
    const auto key = pki::DeviceKey::generate();
    ASSERT_TRUE(controller_->submit_csr(id, key.csr_pem(name)));
    EXPECT_FALSE(controller_->submit_csr(id, key.csr_pem(name))); // no longer PENDING

    const auto other = ca_->sign_csr(key.csr_pem("gtest-someone-else"), "gtest-someone-else");
    EXPECT_THROW(server_->issue_certificate(id, ca_->fingerprint(), to_issued(other)), pqxx::sql_error);
    const auto mine = ca_->sign_csr(key.csr_pem(name), name);
    EXPECT_THROW(server_->issue_certificate(id, std::string(64, '0'), to_issued(mine)), pqxx::sql_error);

    EXPECT_TRUE(server_->reject_enrollment(id, "GTEST", "refused by the test"));
    EXPECT_THROW(server_->issue_certificate(id, ca_->fingerprint(), to_issued(mine)), pqxx::sql_error);
    EXPECT_EQ(identity_of(name), "MISSING");
    const auto row = server_->fetch_enrollment(db::DbRole::Normal, id);
    ASSERT_TRUE(row);
    EXPECT_EQ(row->as_object().at("status").as_string(), "REJECTED");
    EXPECT_EQ(row->as_object().at("error_code").as_string(), "GTEST");
}

// A device may have had several certificates; a rotation adds one, makes it
// current and revokes the previous one as superseded.
TEST_F(DbTest, RotationSupersedesThePreviousCertificate)
{
    const auto dev = make_device("sawtooth");
    const auto first = serials_[dev.name];
    const auto second = rotate(dev);
    EXPECT_NE(first, second);
    EXPECT_EQ(certificate_count(dev.name), 2u);
    EXPECT_TRUE(revoked(first));
    EXPECT_FALSE(revoked(second));
    EXPECT_EQ(identity_of(dev.name), "ACTIVE");
    const auto loaded = controller_->load_device(dev.device_id);
    ASSERT_TRUE(loaded);
    EXPECT_EQ(loaded->serial, second);
    EXPECT_TRUE(loaded->active());
}

// Deleting a previous certificate touches nothing else, and its revocation
// stays in the CRL: the crl table has no foreign key to device_certificate.
TEST_F(DbTest, DeletingAPreviousCertificateKeepsTheDeviceAndTheCrl)
{
    const auto dev = make_device("sine");
    const auto first = serials_[dev.name];
    rotate(dev);
    EXPECT_EQ(server_->delete_certificate(db::DbRole::Admin, dev.name, first), db::DeleteCertResult::Deleted);
    EXPECT_EQ(certificate_count(dev.name), 1u);
    EXPECT_EQ(identity_of(dev.name), "ACTIVE");
    EXPECT_TRUE(revoked(first));
    EXPECT_EQ(server_->delete_certificate(db::DbRole::Admin, dev.name, first),
              db::DeleteCertResult::NoSuchCertificate);
}

// device.(cert_id, device_id) -> device_certificate ON DELETE CASCADE: the
// current certificate takes the device with it; the telemetry stays.
TEST_F(DbTest, DeletingTheCurrentCertificateDeletesTheDevice)
{
    server_->register_listen_async("device_events", [this](boost::json::object m) { inbox_.push(std::move(m)); });
    server_->run_listener_loop();

    const auto dev = make_device("sine");
    rotate(dev);
    controller_->write_scalar_sample(db::DummySample{dev.device_id, 1.0, 1});
    EXPECT_EQ(server_->delete_certificate(db::DbRole::Admin, dev.name, serials_[dev.name]),
              db::DeleteCertResult::DeviceDeleted);
    forget(dev.name);
    EXPECT_TRUE(inbox_.wait([&](const auto& m) { return has(m, "device_events", dev.name, "DELETE"); }));
    EXPECT_EQ(identity_of(dev.name), "MISSING");
    EXPECT_FALSE(controller_->load_device(dev.device_id));
    EXPECT_EQ(certificate_count(dev.name), 0u);
    EXPECT_EQ(telemetry_rows(db::Payload::Scalar, dev.device_id), 1);
    EXPECT_EQ(server_->delete_certificate(db::DbRole::Admin, dev.name, serials_[dev.name]),
              db::DeleteCertResult::NoSuchDevice);
}

// Revoked means out of service, not deleted. Only a hold can be undone.
TEST_F(DbTest, RevocationKeepsTheDeviceAndIsPermanent)
{
    const auto dev = make_device("sine");
    const auto serial = serials_[dev.name];
    EXPECT_EQ(server_->revoke_certificate(db::DbRole::Admin, dev.name, "keyCompromise", "gtest"),
              db::RevokeResult::Revoked);
    EXPECT_EQ(identity_of(dev.name), "REVOKED");
    const auto loaded = controller_->load_device(dev.device_id);
    ASSERT_TRUE(loaded);
    EXPECT_FALSE(loaded->active());
    EXPECT_TRUE(revoked(serial));

    EXPECT_EQ(server_->revoke_certificate(db::DbRole::Admin, dev.name, "certificateHold", "gtest"),
              db::RevokeResult::AlreadyRevoked);
    EXPECT_EQ(server_->unrevoke_certificate(db::DbRole::Admin, dev.name), db::UnrevokeResult::NotOnHold);
    expect_db_error(db::DbErrorKind::Constraint,
                    [&] {
                        server_->with_role(db::DbRole::Admin, [&](pqxx::work& txn) {
                            txn.exec("DELETE FROM crl WHERE serial = $1", pqxx::params{serial});
                        });
                    },
                    "DELETE of a permanent CRL row");
    EXPECT_TRUE(revoked(serial));

    // The revocation outlives the device.
    ASSERT_TRUE(server_->delete_device(db::DbRole::Admin, dev.name));
    forget(dev.name);
    EXPECT_TRUE(revoked(serial));
    EXPECT_EQ(server_->revoke_certificate(db::DbRole::Admin, dev.name, "keyCompromise", "gtest"),
              db::RevokeResult::NoSuchDevice);
}

TEST_F(DbTest, CertificateHoldSuspendsAndCanBeLiftedOrMadePermanent)
{
    const auto dev = make_device("sine");
    EXPECT_EQ(server_->revoke_certificate(db::DbRole::Admin, dev.name, "certificateHold", "gtest"),
              db::RevokeResult::Revoked);
    EXPECT_EQ(identity_of(dev.name), "SUSPENDED");
    EXPECT_EQ(server_->unrevoke_certificate(db::DbRole::Admin, dev.name), db::UnrevokeResult::Lifted);
    EXPECT_EQ(identity_of(dev.name), "ACTIVE");
    EXPECT_FALSE(revoked(serials_[dev.name]));

    EXPECT_EQ(server_->revoke_certificate(db::DbRole::Admin, dev.name, "certificateHold", "gtest"),
              db::RevokeResult::Revoked);
    EXPECT_EQ(server_->revoke_certificate(db::DbRole::Admin, dev.name, "cessationOfOperation", "gtest"),
              db::RevokeResult::Revoked);
    EXPECT_EQ(identity_of(dev.name), "REVOKED");
    EXPECT_EQ(server_->unrevoke_certificate(db::DbRole::Admin, dev.name), db::UnrevokeResult::NotOnHold);
    EXPECT_EQ(server_->unrevoke_certificate(db::DbRole::Admin, "gtest-nobody"), db::UnrevokeResult::NoSuchDevice);
}

// What no application role can even attempt, checked as the schema owner:
// a device without a certificate, or with somebody else's, never commits.
TEST_F(DbTest, ADeviceCannotBeLeftWithoutItsOwnCertificate)
{
    const char* owner = std::getenv("PGDEMO_TEST_OWNER_CONNINFO");
    if (!owner) GTEST_SKIP() << "PGDEMO_TEST_OWNER_CONNINFO not set";
    const auto a = make_device("sine");
    const auto b = make_device("sine");
    rotate(b); // b.cert_id is now a previous certificate of b, current of nobody
    pqxx::connection conn(owner);
    {
        pqxx::work txn(conn);
        EXPECT_THROW(txn.exec("UPDATE device SET cert_id = NULL WHERE device_id = $1", pqxx::params{a.device_id}),
                     pqxx::not_null_violation);
    }
    {
        pqxx::work txn(conn);
        txn.exec("UPDATE device SET cert_id = $1 WHERE device_id = $2", pqxx::params{b.cert_id, a.device_id});
        EXPECT_THROW(txn.commit(), pqxx::foreign_key_violation);
    }
    {
        pqxx::work txn(conn);
        txn.exec("INSERT INTO device (name, type_id, cert_id) VALUES ($1, 'sine', 2147483647)",
                 pqxx::params{unique_name()});
        EXPECT_THROW(txn.commit(), pqxx::foreign_key_violation);
    }
    EXPECT_EQ(identity_of(a.name), "ACTIVE");
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
