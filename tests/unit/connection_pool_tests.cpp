#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <db/ConnectionPool.hpp>

using namespace std::chrono_literals;
using pgdemo::db::ConnectionPool;
using pgdemo::db::PoolSession;
using pgdemo::db::PoolSessionFactory;
using testing::Invoke;
using testing::NiceMock;
using testing::Return;
using testing::StrictMock;
using testing::Throw;

namespace {

class MockSession : public PoolSession
{
public:
    MOCK_METHOD(pqxx::connection&, connection, (), (override));
    MOCK_METHOD(bool, is_open, (), (const, override));
    MOCK_METHOD(void, close, (), (override));
    MOCK_METHOD(int, backend_pid, (), (const, override));
};

class MockFactory : public PoolSessionFactory
{
public:
    MOCK_METHOD(std::unique_ptr<PoolSession>, open, (const std::string& conninfo), (override));
};

template <typename Pred>
bool wait_until(Pred pred, std::chrono::milliseconds timeout = 1000ms)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!pred())
    {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

enum class StepKind { Session, Throw, Null };

struct Step
{
    StepKind kind{StepKind::Session};
    std::unique_ptr<PoolSession> session;
};

/// Sessions are armed before the pool calls the factory, so expectations are
/// not installed from inside a mock action.
class Harness
{
public:
    Harness()
        : factory_owner_(std::make_unique<StrictMock<MockFactory>>())
        , factory_(factory_owner_.get())
    {
    }

    std::shared_ptr<std::atomic<bool>> prepare(int pid, bool expect_close)
    {
        auto session = std::make_unique<NiceMock<MockSession>>();
        auto open = std::make_shared<std::atomic<bool>>(true);
        EXPECT_CALL(*session, backend_pid()).WillOnce(Return(pid));
        EXPECT_CALL(*session, is_open()).WillRepeatedly(Invoke([open] { return open->load(); }));
        EXPECT_CALL(*session, close()).Times(expect_close ? 1 : 0);
        steps_.push_back(Step{StepKind::Session, std::move(session)});
        return open;
    }

    void prepare_throw() { steps_.push_back(Step{StepKind::Throw, nullptr}); }

    void prepare_null() { steps_.push_back(Step{StepKind::Null, nullptr}); }

    void prepare_is_open_throws(int pid)
    {
        auto session = std::make_unique<NiceMock<MockSession>>();
        EXPECT_CALL(*session, backend_pid()).WillOnce(Return(pid));
        EXPECT_CALL(*session, is_open()).WillRepeatedly(Throw(std::runtime_error("boom")));
        EXPECT_CALL(*session, close()).Times(0);
        steps_.push_back(Step{StepKind::Session, std::move(session)});
    }

    void expect_opens(const std::string& conninfo)
    {
        EXPECT_CALL(*factory_, open(conninfo))
            .Times(static_cast<int>(steps_.size()))
            .WillRepeatedly(Invoke([this](const std::string& info) {
                seen_.push_back(info);
                auto step = std::move(steps_.at(next_++));
                if (step.kind == StepKind::Throw) throw pqxx::broken_connection("down");
                if (step.kind == StepKind::Null) return std::unique_ptr<PoolSession>{};
                return std::move(step.session);
            }));
    }

    ConnectionPool make_pool() { return ConnectionPool(std::move(factory_owner_)); }

    [[nodiscard]] const std::vector<std::string>& seen() const { return seen_; }

private:
    std::unique_ptr<StrictMock<MockFactory>> factory_owner_;
    StrictMock<MockFactory>* factory_;
    std::vector<Step> steps_;
    std::size_t next_{0};
    std::vector<std::string> seen_;
};

} // namespace

TEST(ConnectionPool, RejectsANullFactory)
{
    EXPECT_THROW(ConnectionPool(std::unique_ptr<PoolSessionFactory>{}), std::invalid_argument);
}

TEST(ConnectionPool, RejectsAZeroSize)
{
    Harness harness;
    auto pool = harness.make_pool();
    EXPECT_THROW(pool.open("dbname=test", 0), std::invalid_argument);
    EXPECT_EQ(pool.size(), 0u);
    EXPECT_EQ(pool.in_use(), 0u);
}

TEST(ConnectionPool, AcquireBeforeOpenThrows)
{
    Harness harness;
    auto pool = harness.make_pool();
    const auto started = std::chrono::steady_clock::now();
    EXPECT_THROW(static_cast<void>(pool.acquire()), pqxx::broken_connection);
    EXPECT_LT(std::chrono::steady_clock::now() - started, 200ms);
}

TEST(ConnectionPool, RejectsANullSession)
{
    Harness harness;
    harness.prepare_null();
    harness.expect_opens("dbname=test");
    auto pool = harness.make_pool();
    EXPECT_THROW(pool.open("dbname=test", 1), pqxx::broken_connection);
    EXPECT_EQ(pool.size(), 0u);
    EXPECT_EQ(harness.seen(), std::vector<std::string>({"dbname=test"}));
}

TEST(ConnectionPool, OpenFailureLeavesThePoolClosed)
{
    Harness harness;
    harness.prepare(10, false);
    harness.prepare_throw();
    harness.expect_opens("dbname=test");
    auto pool = harness.make_pool();
    EXPECT_THROW(pool.open("dbname=test", 2), pqxx::broken_connection);
    EXPECT_EQ(pool.size(), 0u);
    EXPECT_TRUE(pool.backend_pids().empty());
}

TEST(ConnectionPool, OpenPublishesPidsInCreationOrder)
{
    Harness harness;
    harness.prepare(10, true);
    harness.prepare(20, true);
    harness.prepare(30, true);
    harness.expect_opens("dbname=test");
    auto pool = harness.make_pool();

    pool.open("dbname=test", 3);

    EXPECT_EQ(pool.size(), 3u);
    EXPECT_EQ(pool.in_use(), 0u);
    EXPECT_EQ(pool.backend_pids(), (std::vector<int>{10, 20, 30}));
    EXPECT_EQ(harness.seen(), std::vector<std::string>({"dbname=test", "dbname=test", "dbname=test"}));
}

TEST(ConnectionPool, SecondOpenIsRejectedAndKeepsTheFirstSessions)
{
    Harness harness;
    harness.prepare(10, true);
    harness.prepare(20, true);
    harness.prepare(30, false);
    harness.prepare(40, false);
    harness.expect_opens("dbname=test");
    auto pool = harness.make_pool();

    pool.open("dbname=test", 2);
    EXPECT_THROW(pool.open("dbname=test", 2), std::logic_error);

    EXPECT_EQ(pool.size(), 2u);
    EXPECT_EQ(pool.backend_pids(), (std::vector<int>{10, 20}));
}

TEST(ConnectionPool, CheckoutIsLifoAndReleasedWhenTheLeaseDies)
{
    Harness harness;
    harness.prepare(10, true);
    harness.prepare(20, true);
    harness.expect_opens("dbname=test");
    auto pool = harness.make_pool();
    pool.open("dbname=test", 2);

    {
        auto first = pool.acquire();
        auto second = pool.acquire();
        EXPECT_EQ(first.backend_pid(), 20);
        EXPECT_EQ(second.backend_pid(), 10);
        EXPECT_EQ(pool.in_use(), 2u);
    }
    EXPECT_EQ(pool.in_use(), 0u);

    // `second` dies before `first`, and the free list hands out the session returned last.
    auto again = pool.acquire();
    EXPECT_EQ(again.backend_pid(), 20);
    EXPECT_EQ(pool.in_use(), 1u);
}

TEST(ConnectionPool, LeaseIsReturnedWhenTheCallerThrows)
{
    Harness harness;
    harness.prepare(10, true);
    harness.expect_opens("dbname=test");
    auto pool = harness.make_pool();
    pool.open("dbname=test", 1);

    EXPECT_THROW(
        [&] {
            auto lease = pool.acquire();
            EXPECT_EQ(pool.in_use(), 1u);
            throw std::runtime_error("rollback");
        }(),
        std::runtime_error);
    EXPECT_EQ(pool.in_use(), 0u);
}

TEST(ConnectionPool, MovedFromLeaseDoesNotRelease)
{
    Harness harness;
    harness.prepare(10, true);
    harness.expect_opens("dbname=test");
    auto pool = harness.make_pool();
    pool.open("dbname=test", 1);

    std::optional<ConnectionPool::Lease> kept;
    {
        auto lease = pool.acquire();
        kept.emplace(std::move(lease));
    }
    EXPECT_EQ(pool.in_use(), 1u);
    kept.reset();
    EXPECT_EQ(pool.in_use(), 0u);
}

TEST(ConnectionPool, AcquireBlocksUntilASessionIsReturned)
{
    Harness harness;
    harness.prepare(10, true);
    harness.expect_opens("dbname=test");
    auto pool = harness.make_pool();
    pool.open("dbname=test", 1);

    std::optional<ConnectionPool::Lease> held;
    held.emplace(pool.acquire());
    std::atomic<bool> entered{false};
    std::atomic<bool> got{false};
    std::thread worker([&] {
        entered = true;
        auto lease = pool.acquire();
        got = true;
    });

    ASSERT_TRUE(wait_until([&] { return entered.load(); }));
    std::this_thread::sleep_for(30ms);
    EXPECT_FALSE(got.load());

    held.reset();
    EXPECT_TRUE(wait_until([&] { return got.load(); }));
    worker.join();
    EXPECT_EQ(pool.in_use(), 0u);
}

TEST(ConnectionPool, CloseWakesAThreadBlockedInAcquire)
{
    Harness harness;
    harness.prepare(10, true);
    harness.expect_opens("dbname=test");
    auto pool = harness.make_pool();
    pool.open("dbname=test", 1);

    std::optional<ConnectionPool::Lease> held;
    held.emplace(pool.acquire());
    std::atomic<bool> entered{false};
    std::atomic<bool> threw{false};
    std::thread worker([&] {
        entered = true;
        try
        {
            auto lease = pool.acquire();
            (void)lease;
        }
        catch (const pqxx::broken_connection&)
        {
            threw = true;
        }
    });
    ASSERT_TRUE(wait_until([&] { return entered.load(); }));

    std::thread closer([&] { pool.close(); });
    EXPECT_TRUE(wait_until([&] { return threw.load(); }));
    held.reset();
    closer.join();
    worker.join();

    EXPECT_TRUE(threw.load());
    EXPECT_EQ(pool.size(), 0u);
    EXPECT_EQ(pool.in_use(), 0u);
    EXPECT_THROW(static_cast<void>(pool.acquire()), pqxx::broken_connection);
}

TEST(ConnectionPool, CloseIsIdempotent)
{
    Harness harness;
    harness.prepare(10, true);
    harness.prepare(20, true);
    harness.expect_opens("dbname=test");
    auto pool = harness.make_pool();
    pool.open("dbname=test", 2);

    pool.close();
    pool.close();

    EXPECT_EQ(pool.size(), 0u);
    EXPECT_THROW(static_cast<void>(pool.acquire()), pqxx::broken_connection);
}

TEST(ConnectionPool, CloseContinuesWhenASessionThrows)
{
    Harness harness;
    harness.prepare_is_open_throws(7);
    harness.prepare(8, true);
    harness.expect_opens("dbname=test");
    auto pool = harness.make_pool();
    pool.open("dbname=test", 2);

    EXPECT_NO_THROW(pool.close());
    EXPECT_EQ(pool.size(), 0u);
}

TEST(ConnectionPool, DeadSessionIsReplacedBeforeUse)
{
    Harness harness;
    const auto open = harness.prepare(10, false);
    harness.prepare(30, true);
    harness.expect_opens("dbname=test");
    auto pool = harness.make_pool();
    pool.open("dbname=test", 1);

    open->store(false);
    auto lease = pool.acquire();

    EXPECT_EQ(lease.backend_pid(), 30);
    EXPECT_EQ(pool.backend_pids(), (std::vector<int>{30}));
    EXPECT_EQ(pool.in_use(), 1u);
    EXPECT_EQ(harness.seen(), std::vector<std::string>({"dbname=test", "dbname=test"}));
}

TEST(ConnectionPool, FailedReplaceReturnsTheSlot)
{
    Harness harness;
    const auto open = harness.prepare(10, false);
    harness.prepare_throw();
    harness.prepare(40, true);
    harness.expect_opens("dbname=test");
    auto pool = harness.make_pool();
    pool.open("dbname=test", 1);
    open->store(false);

    EXPECT_THROW(static_cast<void>(pool.acquire()), pqxx::broken_connection);
    EXPECT_EQ(pool.in_use(), 0u);
    EXPECT_EQ(pool.size(), 1u);
    EXPECT_EQ(pool.backend_pids(), (std::vector<int>{10}));

    auto lease = pool.acquire();
    EXPECT_EQ(lease.backend_pid(), 40);
    EXPECT_EQ(pool.backend_pids(), (std::vector<int>{40}));
    EXPECT_EQ(pool.in_use(), 1u);
}

TEST(ConnectionPool, ConcurrentCheckoutsStayInsideThePool)
{
    Harness harness;
    harness.prepare(10, true);
    harness.prepare(20, true);
    harness.expect_opens("dbname=test");
    auto pool = harness.make_pool();
    pool.open("dbname=test", 2);

    constexpr int kThreads = 8;
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(kThreads));
    for (int i = 0; i < kThreads; ++i)
    {
        threads.emplace_back([&] {
            try
            {
                for (int n = 0; n < 40; ++n)
                {
                    auto lease = pool.acquire();
                    const auto in_use = pool.in_use();
                    const int pid = lease.backend_pid();
                    if (in_use < 1 || in_use > 2 || (pid != 10 && pid != 20)) failures.fetch_add(1);
                }
            }
            catch (const std::exception&)
            {
                failures.fetch_add(1);
            }
        });
    }
    for (auto& thread : threads) thread.join();

    EXPECT_EQ(failures.load(), 0);
    EXPECT_EQ(pool.in_use(), 0u);
    EXPECT_EQ(pool.size(), 2u);
}
