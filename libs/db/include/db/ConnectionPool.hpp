#pragma once

#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <pqxx/pqxx>

namespace pgdemo::db {

/// One backend session. Production uses libpq; tests supply a mock.
class PoolSession
{
public:
    virtual ~PoolSession();

    /// The libpqxx connection to run transactions on. Only the lease holder may use it.
    virtual pqxx::connection& connection() = 0;
    /// False once the server side has gone away; the pool then replaces the session.
    virtual bool is_open() const = 0;
    /// End the session. Called by `ConnectionPool::close()` only.
    virtual void close() = 0;
    /// PID of the PostgreSQL backend serving this session (`pg_stat_activity.pid`).
    virtual int backend_pid() const = 0;

    PoolSession(const PoolSession&) = delete;
    PoolSession& operator=(const PoolSession&) = delete;

protected:
    PoolSession();
};

/// Opens the sessions a pool keeps checked out.
class PoolSessionFactory
{
public:
    virtual ~PoolSessionFactory();

    /**
     * @brief Establish one session.
     * @param conninfo libpq connection string
     * @return the new session; never null on success
     * @throws pqxx::broken_connection when the server cannot be reached
     */
    virtual std::unique_ptr<PoolSession> open(const std::string& conninfo) = 0;

    PoolSessionFactory(const PoolSessionFactory&) = delete;
    PoolSessionFactory& operator=(const PoolSessionFactory&) = delete;

protected:
    PoolSessionFactory();
};

/**
 * @brief Fixed set of open sessions, checked out one thread at a time.
 *
 * A connection is not thread-safe. `acquire()` checks one out to a single
 * thread; the mutex guards only the free list, never the work done with the
 * session. Callers block while every session is checked out.
 */
class ConnectionPool
{
public:
    /// Returns its session in the destructor, including when the caller throws.
    class Lease
    {
    public:
        /// Puts the session back on the free list and wakes one waiter.
        ~Lease();
        /// Takes over the checkout; `other` no longer returns anything.
        Lease(Lease&& other) noexcept;
        Lease& operator=(Lease&&) = delete;
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;

        /// Connection checked out to this lease, valid until the lease is destroyed.
        [[nodiscard]] pqxx::connection& get() const;
        /// Backend PID of the checked-out session.
        [[nodiscard]] int backend_pid() const;

    private:
        friend class ConnectionPool;
        Lease(ConnectionPool* pool, std::size_t index) noexcept;

        ConnectionPool* pool_;
        std::size_t index_;
    };

    /// libpq sessions (`pqxx::connection`).
    ConnectionPool();

    /// `factory` opens every session. Tests pass a mock.
    explicit ConnectionPool(std::unique_ptr<PoolSessionFactory> factory);

    ~ConnectionPool();

    ConnectionPool(const ConnectionPool&) = delete;
    ConnectionPool& operator=(const ConnectionPool&) = delete;

    /**
     * @brief Open `size` sessions.
     *
     * @throws std::invalid_argument if `size` is 0.
     * @throws std::logic_error if the pool is already open.
     * @throws pqxx::broken_connection if a session cannot be established.
     */
    void open(std::string conninfo, std::size_t size);

    /**
     * @brief Wake waiters, wait until every lease is back, then close the sessions.
     *
     * Idempotent. The caller must not hold a lease: this waits until every slot is idle.
     */
    void close() noexcept;

    /**
     * @brief Block until a session is free.
     *
     * A slot whose session has dropped is replaced before the lease is returned.
     * If that fails, the slot goes back to the free list and the error propagates.
     *
     * @throws pqxx::broken_connection if the pool is closed, or a dead slot cannot be replaced.
     */
    [[nodiscard]] Lease acquire();

    /// Number of sessions opened by `open()`; 0 when closed.
    [[nodiscard]] std::size_t size() const;
    /// Number of sessions currently checked out.
    [[nodiscard]] std::size_t in_use() const;

    /// Backend PIDs captured when each slot was opened. Safe to call while other threads hold leases.
    [[nodiscard]] std::vector<int> backend_pids() const;

private:
    enum class State { closed, open, closing };

    struct Slot
    {
        std::unique_ptr<PoolSession> session;
        /// Cached at open: read under `mu_` while another thread holds the session.
        int backend_pid{0};
    };

    void release(std::size_t index) noexcept;
    void ensure_open(std::size_t index);
    [[nodiscard]] std::unique_ptr<PoolSession> open_session(const std::string& conninfo);

    std::unique_ptr<PoolSessionFactory> factory_;

    /// Guards every member below. Never held while connecting or running SQL.
    mutable std::mutex mu_;
    /// Signalled when a slot returns to `idle_` or `state_` changes.
    std::condition_variable cv_;
    State state_{State::closed};
    /// Written only while no slot is checked out.
    std::string conninfo_;
    std::vector<Slot> slots_;
    /// Slots not checked out; in use = `slots_.size() - idle_.size()`.
    std::vector<std::size_t> idle_;
};

} // namespace pgdemo::db
