#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <variant>

#include <db/DatabaseManager.hpp>

namespace pgdemo::controller {

/**
 * @brief One FIFO of samples per device node, and the thread that inserts it.
 *
 * `push()` is what a ROS subscription calls. It does not touch PostgreSQL.
 * The first sample of a node starts a `std::jthread` for that queue. The
 * thread waits until 64 samples are queued or 100 ms have passed, then writes
 * the whole queue in one `write_telemetry_batch` transaction. Nodes flush
 * concurrently: each thread checks a connection out of the pool.
 *
 * A queue holds 256 samples. Past that, the sample that just arrived is
 * dropped and counted. `forget()` and `shutdown()` insert what remains and
 * join the thread. A failed insert is logged and not retried.
 *
 * `push()`, `forget()` and `shutdown()` run on the ROS spin thread. The
 * queue mutex is only there so the writer thread can take the samples.
 */
class TelemetryWriter
{
public:
    /// One sample, of whichever payload that node publishes.
    using Item = std::variant<db::DummySample, db::TrackSample, db::XySample, db::XyzSample>;

    /// `db` outlives this object. The pool must have a free connection per flushing node.
    explicit TelemetryWriter(db::DatabaseManager& db);
    ~TelemetryWriter();

    TelemetryWriter(const TelemetryWriter&) = delete;
    TelemetryWriter& operator=(const TelemetryWriter&) = delete;

    /// Append one sample to `name`'s queue. Drops it when that queue holds 256.
    void push(const std::string& name, Item sample);

    /// Insert what `name` still holds, then join its thread.
    void forget(const std::string& name);

    /// Insert every queue and join every thread. Idempotent.
    void shutdown();

private:
    struct Queue
    {
        std::mutex mu;
        std::condition_variable cv;
        std::deque<Item> samples;
        std::size_t dropped{0};
        bool stopping{false};
        std::jthread thread;
    };

    void run(Queue& queue, std::string name);
    void flush(const std::string& name, std::deque<Item>& taken);
    void stop_and_join(std::unique_ptr<Queue> queue);

    db::DatabaseManager& db_;
    std::mutex map_mu_;
    std::map<std::string, std::unique_ptr<Queue>> queues_;
    bool stopped_{false};
};

} // namespace pgdemo::controller
