// Controller <-> device frames (libs/wire) and the device signal generators.

#include <array>
#include <chrono>
#include <cmath>
#include <exception>
#include <numbers>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/local/connect_pair.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/asio/write.hpp>
#include <boost/system/system_error.hpp>
#include <gtest/gtest.h>

#include "Generators.hpp"
#include "Stream.hpp"
#include "wire/Frame.hpp"

namespace asio = boost::asio;
namespace json = boost::json;
using namespace pgdemo;
using namespace std::chrono_literals;
using namespace asio::experimental::awaitable_operators;
using Clock = std::chrono::steady_clock;

namespace {

using Socket = asio::local::stream_protocol::socket;

/// Run `body` on a fresh io_context to completion; rethrow what it threw.
template <typename F>
void run_coroutine(asio::io_context& io, F body)
{
    std::exception_ptr failure;
    asio::co_spawn(io, std::move(body), [&](std::exception_ptr e) { failure = e; });
    io.run();
    if (failure) std::rethrow_exception(failure);
}

/// Next frame from `s`, or `std::nullopt` if none arrives within `wait`.
asio::awaitable<std::optional<json::object>> read_within(Socket& s, std::chrono::milliseconds wait)
{
    asio::steady_timer timer(co_await asio::this_coro::executor, wait);
    auto r = co_await (wire::read_frame(s) || timer.async_wait(asio::use_awaitable));
    if (r.index() == 0) co_return std::get<0>(std::move(r));
    co_return std::nullopt;
}

/// A device stream on one end of a socket pair, the test as its controller
/// on the other. `run()` reports whether the stream ended and how.
struct StreamHarness
{
    asio::io_context io;
    Socket controller{io};
    Socket device{io};
    device::SampleStream stream{device, device::Generator::Sine, "device test"};
    bool ended = false;
    std::exception_ptr ended_with;

    StreamHarness() { asio::local::connect_pair(controller, device); }

    /// Run the stream and `script` (the controller side) until both finish;
    /// a watchdog closes everything after 10 s so a hang fails instead.
    template <typename F>
    void run(F script)
    {
        asio::co_spawn(io, stream.run(), [this](std::exception_ptr e) {
            ended = true;
            ended_with = e;
        });
        std::exception_ptr failure;
        asio::co_spawn(io, std::move(script), [&](std::exception_ptr e) { failure = e; });
        asio::steady_timer watchdog(io, 10s);
        watchdog.async_wait([this](boost::system::error_code ec) {
            if (ec) return;
            ADD_FAILURE() << "watchdog fired";
            controller.close();
            device.close();
        });
        asio::co_spawn(io, [&]() -> asio::awaitable<void> {
            while (!ended) co_await asio::steady_timer(io, 5ms).async_wait(asio::use_awaitable);
            watchdog.cancel();
        }, [](std::exception_ptr) {});
        io.run();
        if (failure) std::rethrow_exception(failure);
    }
};

} // namespace

TEST(Frame, EncodesABigEndianLengthAndTheJsonBody)
{
    const std::string data = wire::encode({{"type", "hello"}, {"device", "d1"}});
    ASSERT_GE(data.size(), 4u);
    const std::array<unsigned char, 4> header{static_cast<unsigned char>(data[0]), static_cast<unsigned char>(data[1]),
                                              static_cast<unsigned char>(data[2]), static_cast<unsigned char>(data[3])};
    EXPECT_EQ(header[0], 0);
    EXPECT_EQ(header[1], 0);
    EXPECT_EQ(wire::body_length(header), data.size() - 4);
    const auto msg = wire::decode(std::string_view(data).substr(4));
    EXPECT_EQ(wire::type_of(msg), "hello");
    EXPECT_EQ(wire::get_string(msg, "device"), "d1");
}

TEST(Frame, RejectsEmptyAndOversizedBodies)
{
    EXPECT_THROW(wire::body_length({0, 0, 0, 0}), wire::FrameError);
    EXPECT_NO_THROW(wire::body_length({0, 1, 0, 0}));
    EXPECT_THROW(wire::body_length({0, 1, 0, 1}), wire::FrameError);
    EXPECT_THROW(wire::body_length({0xff, 0xff, 0xff, 0xff}), wire::FrameError);
    EXPECT_THROW(wire::encode({{"type", "csr"}, {"csr", std::string(wire::kMaxFrame, 'x')}}), wire::FrameError);
}

TEST(Frame, DecodesOnlyJsonObjects)
{
    EXPECT_THROW(wire::decode("not json"), wire::FrameError);
    EXPECT_THROW(wire::decode("[1,2]"), wire::FrameError);
    EXPECT_THROW(wire::decode("\"hello\""), wire::FrameError);
    EXPECT_THROW(wire::decode("{\"type\":1} trailing"), wire::FrameError);
    EXPECT_NO_THROW(wire::decode("{}"));
}

TEST(Frame, AccessorsIgnoreMissingAndMistypedMembers)
{
    const json::object msg{{"type", "config"}, {"rate_ms", 250}, {"amplitude", 1.5}, {"name", 7}};
    EXPECT_EQ(wire::get_number(msg, "rate_ms"), 250.0);
    EXPECT_EQ(wire::get_number(msg, "amplitude"), 1.5);
    EXPECT_EQ(wire::get_number(msg, "type"), std::nullopt);
    EXPECT_EQ(wire::get_number(msg, "missing"), std::nullopt);
    EXPECT_EQ(wire::get_string(msg, "name"), std::nullopt);
    EXPECT_EQ(wire::type_of(json::object{}), "");
    EXPECT_EQ(wire::type_of(json::object{{"type", 3}}), "");
}

TEST(FrameStream, ExchangesFramesOverASocketPair)
{
    asio::io_context io;
    Socket a(io), b(io);
    asio::local::connect_pair(a, b);
    std::vector<std::string> seen;
    run_coroutine(io, [&]() -> asio::awaitable<void> {
        co_await wire::write_frame(a, {{"type", "hello"}, {"device", "d1"}, {"mode", "run"}});
        co_await wire::write_frame(a, {{"type", "sample"}, {"seq", 1}, {"value", 2.5}});
        for (int i = 0; i < 2; ++i) seen.push_back(wire::type_of(co_await wire::read_frame(b)));
        co_await wire::write_frame(b, {{"type", "bye"}, {"reason", "test"}});
        const auto bye = co_await wire::read_frame(a);
        seen.push_back(*wire::get_string(bye, "reason"));
    });
    EXPECT_EQ(seen, (std::vector<std::string>{"hello", "sample", "test"}));
}

TEST(FrameStream, AnOversizedHeaderFailsBeforeAllocating)
{
    asio::io_context io;
    Socket a(io), b(io);
    asio::local::connect_pair(a, b);
    EXPECT_THROW(run_coroutine(io,
                               [&]() -> asio::awaitable<void> {
                                   const std::array<unsigned char, 4> header{0x7f, 0xff, 0xff, 0xff};
                                   co_await asio::async_write(a, asio::buffer(header), asio::use_awaitable);
                                   co_await wire::read_frame(b);
                               }),
                 wire::FrameError);
}

TEST(FrameStream, PeerCloseIsEndOfFile)
{
    asio::io_context io;
    Socket a(io), b(io);
    asio::local::connect_pair(a, b);
    a.close();
    try
    {
        run_coroutine(io, [&]() -> asio::awaitable<void> { co_await wire::read_frame(b); });
        FAIL() << "read_frame returned after the peer closed";
    }
    catch (const boost::system::system_error& e)
    {
        EXPECT_EQ(e.code(), asio::error::eof);
    }
}

// A config frame cancels the producer's wait: with a 60 s rate pending, the
// next sample arrives at the new 20 ms rate, not a minute later.
TEST(SampleStream, ConfigCancelsTheProducerWait)
{
    StreamHarness h;
    h.stream.apply({{"type", "config"}, {"device_id", 2}, {"rate_ms", 60000}, {"amplitude", 5.0}, {"is_active", true}});
    std::vector<std::int64_t> seqs;
    Clock::duration after_config{};
    h.run([&]() -> asio::awaitable<void> {
        const auto first = co_await read_within(h.controller, 2s);
        if (!first) throw std::runtime_error("no first sample");
        seqs.push_back(static_cast<std::int64_t>(*wire::get_number(*first, "seq")));

        co_await wire::write_frame(h.controller, {{"type", "config"}, {"rate_ms", 20}});
        const auto t0 = Clock::now();
        for (int i = 0; i < 5; ++i)
        {
            const auto m = co_await read_within(h.controller, 2s);
            if (!m) throw std::runtime_error("no sample after the config");
            seqs.push_back(static_cast<std::int64_t>(*wire::get_number(*m, "seq")));
        }
        after_config = Clock::now() - t0;
        co_await wire::write_frame(h.controller, {{"type", "bye"}, {"reason", "test"}});
    });
    EXPECT_EQ(seqs, (std::vector<std::int64_t>{1, 2, 3, 4, 5, 6}));
    EXPECT_LT(after_config, 2s);
    EXPECT_GE(after_config, 60ms); // four 20 ms waits after the immediate one
    EXPECT_EQ(h.stream.rate_ms(), 20);
    EXPECT_DOUBLE_EQ(h.stream.amplitude(), 5.0); // members absent from a config are kept
    EXPECT_TRUE(h.ended);
    EXPECT_FALSE(h.ended_with); // bye is a clean end
}

TEST(SampleStream, PausedDeviceSendsNoSamplesUntilResumed)
{
    StreamHarness h;
    h.stream.apply({{"rate_ms", 10}, {"is_active", false}});
    std::optional<json::object> while_paused, resumed;
    h.run([&]() -> asio::awaitable<void> {
        while_paused = co_await read_within(h.controller, 300ms);
        co_await wire::write_frame(h.controller, {{"type", "config"}, {"is_active", true}});
        resumed = co_await read_within(h.controller, 2s);
        co_await wire::write_frame(h.controller, {{"type", "bye"}, {"reason", "test"}});
    });
    EXPECT_FALSE(while_paused);
    ASSERT_TRUE(resumed);
    EXPECT_EQ(wire::type_of(*resumed), "sample");
    EXPECT_EQ(wire::get_number(*resumed, "seq"), 1.0);
}

// The controller going away ends all three coroutines: io.run() returns only
// once the producer, the listener and the signal wait have all finished.
TEST(SampleStream, ControllerEofEndsTheStream)
{
    StreamHarness h;
    h.stream.apply({{"rate_ms", 10}});
    h.run([&]() -> asio::awaitable<void> {
        for (int i = 0; i < 3; ++i)
            if (!co_await read_within(h.controller, 2s)) throw std::runtime_error("no sample");
        h.controller.close();
    });
    ASSERT_TRUE(h.ended);
    ASSERT_TRUE(h.ended_with);
    try
    {
        std::rethrow_exception(h.ended_with);
    }
    catch (const boost::system::system_error& e)
    {
        EXPECT_EQ(e.code(), asio::error::eof);
    }
    EXPECT_GE(h.stream.samples(), 3);
}

TEST(Generators, TypeAndPayloadSelectTheGenerator)
{
    EXPECT_EQ(device::generator_for("sine", "scalar"), device::Generator::Sine);
    EXPECT_EQ(device::generator_for("sawtooth", "scalar"), device::Generator::Sawtooth);
    EXPECT_EQ(device::generator_for("gps_orbit", "gps"), device::Generator::GpsOrbit);
    EXPECT_EQ(device::generator_for("thermometer", "scalar"), device::Generator::Sine);
}

TEST(Generators, PhaseSpreadsDevicesOverAnOctant)
{
    EXPECT_DOUBLE_EQ(device::phase_for(0), 0.0);
    EXPECT_DOUBLE_EQ(device::phase_for(2), std::numbers::pi / 2.0);
    EXPECT_DOUBLE_EQ(device::phase_for(8), 0.0);
    EXPECT_DOUBLE_EQ(device::phase_for(9), device::phase_for(1));
}

TEST(Generators, SineTurnsOnceEveryFortyTicks)
{
    EXPECT_NEAR(device::sine(0, 100.0, 0.0), 0.0, 1e-9);
    EXPECT_NEAR(device::sine(10, 100.0, 0.0), 100.0, 1e-9);
    EXPECT_NEAR(device::sine(30, 100.0, 0.0), -100.0, 1e-9);
    EXPECT_NEAR(device::sine(40, 100.0, 0.0), device::sine(0, 100.0, 0.0), 1e-9);
    EXPECT_NEAR(device::sine(0, 100.0, std::numbers::pi / 2.0), 100.0, 1e-9);
}

TEST(Generators, SawtoothRampsAndWrapsEveryFiftyTicks)
{
    EXPECT_DOUBLE_EQ(device::sawtooth(0, 10.0, 0.0), 0.0);
    EXPECT_DOUBLE_EQ(device::sawtooth(25, 10.0, 0.0), 5.0);
    EXPECT_DOUBLE_EQ(device::sawtooth(50, 10.0, 0.0), 0.0);
    EXPECT_DOUBLE_EQ(device::sawtooth(25, 10.0, 1.0), 5.5);
}

TEST(Generators, SamplesCarryThePayloadOfTheirType)
{
    const auto scalar = device::make_sample(device::Generator::Sine, 10, 2.0, 0.0, 0.0);
    EXPECT_EQ(wire::type_of(scalar), "sample");
    EXPECT_EQ(wire::get_number(scalar, "seq"), 10.0);
    EXPECT_NEAR(*wire::get_number(scalar, "value"), 2.0, 1e-9);
    EXPECT_FALSE(scalar.contains("radius_m"));

    const auto gps = device::make_sample(device::Generator::GpsOrbit, 10, 500.0, 0.0, 0.0);
    EXPECT_FALSE(gps.contains("value"));
    EXPECT_EQ(wire::get_number(gps, "origin_lon"), device::kOriginLon);
    EXPECT_EQ(wire::get_number(gps, "origin_lat"), device::kOriginLat);
    EXPECT_EQ(wire::get_number(gps, "radius_m"), 500.0);
    EXPECT_NEAR(*wire::get_number(gps, "bearing_rad"), std::numbers::pi / 2.0, 1e-9);
}
