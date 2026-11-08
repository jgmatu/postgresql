#include <db/ConnectionPool.hpp>

#include <format>
#include <stdexcept>
#include <string>
#include <utility>

#include <log/Log.hpp>

namespace pgdemo::db {

namespace {

constexpr std::string_view kLog = "db";

void log_pool_warning(std::string_view message)
{
    log::warn(kLog, message);
}

class PqxxSession final : public PoolSession
{
public:
    explicit PqxxSession(const std::string& conninfo)
        : connection_(conninfo)
    {
    }

    pqxx::connection& connection() override { return connection_; }
    bool is_open() const override { return connection_.is_open(); }
    void close() override { connection_.close(); }
    int backend_pid() const override { return connection_.backendpid(); }

private:
    pqxx::connection connection_;
};

class PqxxSessionFactory final : public PoolSessionFactory
{
public:
    std::unique_ptr<PoolSession> open(const std::string& conninfo) override
    {
        return std::make_unique<PqxxSession>(conninfo);
    }
};

} // namespace

PoolSession::PoolSession() = default;
PoolSession::~PoolSession() = default;
PoolSessionFactory::PoolSessionFactory() = default;
PoolSessionFactory::~PoolSessionFactory() = default;

ConnectionPool::Lease::Lease(ConnectionPool* pool, std::size_t index) noexcept
    : pool_(pool)
    , index_(index)
{
}

ConnectionPool::Lease::Lease(Lease&& other) noexcept
    : pool_(other.pool_)
    , index_(other.index_)
{
    other.pool_ = nullptr;
}

ConnectionPool::Lease::~Lease()
{
    if (pool_ != nullptr) pool_->release(index_);
}

pqxx::connection& ConnectionPool::Lease::get() const
{
    return pool_->slots_[index_].session->connection();
}

int ConnectionPool::Lease::backend_pid() const
{
    return pool_->slots_[index_].backend_pid;
}

ConnectionPool::ConnectionPool()
    : ConnectionPool(std::make_unique<PqxxSessionFactory>())
{
}

ConnectionPool::ConnectionPool(std::unique_ptr<PoolSessionFactory> factory)
    : factory_(std::move(factory))
{
    if (!factory_) throw std::invalid_argument("connection pool factory is null");
}

ConnectionPool::~ConnectionPool()
{
    close();
}

std::unique_ptr<PoolSession> ConnectionPool::open_session(const std::string& conninfo)
{
    auto session = factory_->open(conninfo);
    if (!session) throw pqxx::broken_connection("session factory returned null");
    return session;
}

void ConnectionPool::open(std::string conninfo, std::size_t size)
{
    if (size == 0) throw std::invalid_argument("connection pool size must be at least 1");

    // Connect outside the mutex: session startup is the slow part, and nothing
    // can observe these sessions until they are published below.
    std::vector<Slot> fresh;
    fresh.reserve(size);
    for (std::size_t i = 0; i < size; ++i)
    {
        Slot slot;
        slot.session = open_session(conninfo);
        slot.backend_pid = slot.session->backend_pid();
        fresh.push_back(std::move(slot));
    }

    std::lock_guard lock(mu_);
    if (state_ != State::closed) throw std::logic_error("connection pool is already open");

    slots_ = std::move(fresh);
    idle_.clear();
    idle_.reserve(slots_.size());
    for (std::size_t i = 0; i < slots_.size(); ++i) idle_.push_back(i);
    conninfo_ = std::move(conninfo);
    state_ = State::open;
}

void ConnectionPool::close() noexcept
{
    try
    {
        std::vector<Slot> dying;
        {
            std::unique_lock lock(mu_);
            if (state_ != State::open) return;
            state_ = State::closing;
            cv_.notify_all();
            cv_.wait(lock, [&] { return idle_.size() == slots_.size(); });
            dying.swap(slots_);
            idle_.clear();
            conninfo_.clear();
            state_ = State::closed;
        }

        for (auto& slot : dying)
        {
            if (!slot.session) continue;
            try
            {
                if (slot.session->is_open()) slot.session->close();
            }
            catch (const std::exception& e)
            {
                log_pool_warning(std::string("error closing pooled connection: ") + e.what());
            }
        }
    }
    catch (const std::exception& e)
    {
        log_pool_warning(std::string("error closing connection pool: ") + e.what());
    }
}

ConnectionPool::Lease ConnectionPool::acquire()
{
    std::size_t index = 0;
    {
        std::unique_lock lock(mu_);
        cv_.wait(lock, [&] { return state_ != State::open || !idle_.empty(); });
        if (state_ != State::open) throw pqxx::broken_connection("query pool is not open");
        index = idle_.back();
        idle_.pop_back();
    }

    try
    {
        ensure_open(index);
    }
    catch (...)
    {
        std::lock_guard lock(mu_);
        idle_.push_back(index);
        cv_.notify_all();
        throw;
    }
    return Lease(this, index);
}

std::size_t ConnectionPool::size() const
{
    std::lock_guard lock(mu_);
    return slots_.size();
}

std::size_t ConnectionPool::in_use() const
{
    std::lock_guard lock(mu_);
    return slots_.size() - idle_.size();
}

std::vector<int> ConnectionPool::backend_pids() const
{
    std::lock_guard lock(mu_);
    std::vector<int> pids;
    pids.reserve(slots_.size());
    for (const auto& slot : slots_) pids.push_back(slot.backend_pid);
    return pids;
}

void ConnectionPool::release(std::size_t index) noexcept
{
    std::lock_guard lock(mu_);
    // `idle_` is reserved to `slots_.size()` in open(), and a release only
    // happens while this slot is checked out, so push_back does not allocate.
    idle_.push_back(index);
    cv_.notify_all();
}

void ConnectionPool::ensure_open(std::size_t index)
{
    // This slot is checked out, so no other thread touches its session, and
    // `conninfo_` cannot change until it is returned.
    if (slots_[index].session && slots_[index].session->is_open()) return;

    auto fresh = open_session(conninfo_);
    const int pid = fresh->backend_pid();
    {
        std::lock_guard lock(mu_);
        if (state_ == State::closing) throw pqxx::broken_connection("query pool is closed");
        slots_[index].session = std::move(fresh);
        slots_[index].backend_pid = pid;
    }
    log_pool_warning(std::format("replaced closed query connection at slot {} (pid {})", index, pid));
}

} // namespace pgdemo::db
