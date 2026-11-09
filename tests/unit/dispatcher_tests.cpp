#include <atomic>
#include <chrono>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <dispatcher/Dispatcher.hpp>
#include <events/EventHub.hpp>

using namespace std::chrono_literals;
using pgdemo::server::Dispatcher;
using pgdemo::server::EventHub;
using pgdemo::server::ResponseStatus;

// The completion can arrive before the waiting thread reaches the condition
// variable (here: synchronously, inside submit). It must not be lost.
TEST(Dispatcher, CompletionDuringSubmitIsNotLost)
{
    Dispatcher d;
    const auto t0 = std::chrono::steady_clock::now();
    const auto st = d.wait_for_response(1, 5000, [&] { d.dispatch(1, ResponseStatus::SUCCESS); });
    EXPECT_EQ(st, ResponseStatus::SUCCESS);
    EXPECT_LT(std::chrono::steady_clock::now() - t0, 1s);
    EXPECT_EQ(d.pending(), 0u);
}

TEST(Dispatcher, ErrorIsPropagated)
{
    Dispatcher d;
    EXPECT_EQ(d.wait_for_response(7, 5000, [&] { d.dispatch(7, ResponseStatus::ERROR); }), ResponseStatus::ERROR);
}

TEST(Dispatcher, TimeoutUnregisters)
{
    Dispatcher d;
    EXPECT_EQ(d.wait_for_response(2, 50, [] {}), ResponseStatus::TIMEOUT);
    EXPECT_EQ(d.pending(), 0u);
    d.dispatch(2, ResponseStatus::SUCCESS); // late completion: no-op
}

TEST(Dispatcher, ThrowingSubmitIsDbError)
{
    Dispatcher d;
    EXPECT_EQ(d.wait_for_response(3, 5000, [] { throw std::runtime_error("insert failed"); }),
              ResponseStatus::DB_ERROR);
    EXPECT_EQ(d.pending(), 0u);
}

TEST(Dispatcher, DuplicateIdIsRejected)
{
    Dispatcher d;
    std::thread first([&] { EXPECT_EQ(d.wait_for_response(9, 5000), ResponseStatus::SUCCESS); });
    while (d.pending() == 0) std::this_thread::sleep_for(1ms);
    EXPECT_EQ(d.wait_for_response(9, 100, [] {}), ResponseStatus::DB_ERROR);
    d.dispatch(9, ResponseStatus::SUCCESS);
    first.join();
}

TEST(Dispatcher, FullSystemRejectsWithoutSubmitting)
{
    Dispatcher d(1);
    std::thread holder([&] { EXPECT_EQ(d.wait_for_response(1, 5000), ResponseStatus::SUCCESS); });
    while (d.pending() == 0) std::this_thread::sleep_for(1ms);

    bool submitted = false;
    EXPECT_EQ(d.wait_for_response(2, 100, [&] { submitted = true; }), ResponseStatus::SYSTEM_FULL);
    EXPECT_FALSE(submitted);

    d.dispatch(1, ResponseStatus::SUCCESS);
    holder.join();
}

// Many concurrent waiters, completed from one thread in random order (the
// listener thread in production). Each waiter gets exactly its own status.
TEST(Dispatcher, ConcurrentWaitersGetTheirOwnCompletion)
{
    constexpr int kN = 200;
    Dispatcher d(kN);
    std::vector<ResponseStatus> got(kN, ResponseStatus::PENDING);
    std::atomic<int> registered{0};

    std::vector<std::jthread> waiters;
    for (int i = 0; i < kN; ++i)
    {
        waiters.emplace_back([&, i] {
            got[static_cast<std::size_t>(i)] =
                d.wait_for_response(static_cast<std::uint64_t>(i + 1), 10000, [&] { registered.fetch_add(1); });
        });
    }
    while (registered.load() < kN) std::this_thread::sleep_for(1ms);

    std::vector<int> order(kN);
    for (int i = 0; i < kN; ++i) order[static_cast<std::size_t>(i)] = i;
    std::shuffle(order.begin(), order.end(), std::mt19937{42});
    for (const int i : order)
    {
        d.dispatch(static_cast<std::uint64_t>(i + 1), i % 2 ? ResponseStatus::ERROR : ResponseStatus::SUCCESS);
    }
    waiters.clear();

    for (int i = 0; i < kN; ++i)
    {
        EXPECT_EQ(got[static_cast<std::size_t>(i)], i % 2 ? ResponseStatus::ERROR : ResponseStatus::SUCCESS) << i;
    }
    EXPECT_EQ(d.pending(), 0u);
}

// ------------------------------------------------------------------- EventHub

TEST(EventHub, ChannelsArePerTopicNotPerDevice)
{
    const std::set<std::string> expected{"telemetry_scalar", "telemetry_gps", "device", "config", "error"};
    EXPECT_EQ(EventHub::all_channels(), expected);
}

TEST(EventHub, DeliversOnlySubscribedChannels)
{
    EventHub hub;
    auto s = hub.subscribe({"telemetry_scalar"});
    hub.publish("telemetry_gps", "dummy3", "{}");
    hub.publish("telemetry_scalar", "dummy1", R"({"v":1})");
    const auto e = s->pop(100ms);
    ASSERT_TRUE(e);
    EXPECT_EQ(e->channel, "telemetry_scalar");
    EXPECT_EQ(e->device, "dummy1");
    EXPECT_EQ(e->data, R"({"v":1})");
    EXPECT_FALSE(s->pop(10ms));
}

// One channel carries every device; the device filter narrows it per stream.
TEST(EventHub, DeviceFilterSelectsDevices)
{
    EventHub hub;
    auto only_a = hub.subscribe({"telemetry_scalar", "config"}, {"sensor-a"});
    auto every = hub.subscribe({"telemetry_scalar"});
    hub.publish("telemetry_scalar", "dummy1", "1");
    hub.publish("telemetry_scalar", "sensor-a", "2");
    hub.publish("config", "sensor-a", "3");
    hub.publish("config", "dummy1", "4");

    EXPECT_EQ(only_a->pop(10ms)->data, "2");
    EXPECT_EQ(only_a->pop(10ms)->data, "3");
    EXPECT_FALSE(only_a->pop(10ms));

    EXPECT_EQ(every->pop(10ms)->data, "1");
    EXPECT_EQ(every->pop(10ms)->data, "2");
    EXPECT_FALSE(every->pop(10ms));
}

TEST(EventHub, SlowSubscriberDropsOldest)
{
    EventHub hub;
    auto s = hub.subscribe({"telemetry_scalar"}, {}, 2);
    for (int i = 1; i <= 5; ++i) hub.publish("telemetry_scalar", "dummy1", std::to_string(i));
    EXPECT_EQ(s->dropped(), 3u);
    EXPECT_EQ(s->pop(10ms)->data, "4");
    EXPECT_EQ(s->pop(10ms)->data, "5");
}

TEST(EventHub, CloseAllWakesWaiters)
{
    EventHub hub;
    auto s = hub.subscribe(EventHub::all_channels());
    std::jthread t([&] { hub.close_all(); });
    const auto t0 = std::chrono::steady_clock::now();
    EXPECT_FALSE(s->pop(5s));
    EXPECT_TRUE(s->closed());
    EXPECT_LT(std::chrono::steady_clock::now() - t0, 2s);
}

TEST(EventHub, UnsubscribeStopsDelivery)
{
    EventHub hub;
    auto s = hub.subscribe({"config"});
    hub.unsubscribe(s);
    EXPECT_EQ(hub.subscribers(), 0u);
    hub.publish("config", "dummy1", "{}");
    EXPECT_FALSE(s->pop(10ms));
}
