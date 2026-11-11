// Device signal generators and the online sample stream.

#include <chrono>
#include <cmath>
#include <numbers>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include <pgdemo_msgs/msg/bye.hpp>
#include <pgdemo_msgs/msg/config.hpp>
#include <pgdemo_msgs/msg/gps_sample.hpp>
#include <pgdemo_msgs/msg/heartbeat.hpp>
#include <pgdemo_msgs/msg/scalar_sample.hpp>

#include "ConfigCheck.hpp"
#include "Generators.hpp"
#include "Stream.hpp"

using namespace pgdemo;
using Clock = std::chrono::steady_clock;
namespace msg = pgdemo_msgs::msg;

namespace {

struct StreamHarness
{
    std::vector<device::SampleStream::Emission> sent;
    device::SampleStream stream{[&](device::SampleStream::Emission msg) { sent.push_back(std::move(msg)); },
                                 device::Generator::Sine, "device test"};
};

msg::Config config(int32_t device_id, int32_t rate_ms, double amplitude, bool active)
{
    msg::Config c;
    c.device_id = device_id;
    c.rate_ms = rate_ms;
    c.amplitude = amplitude;
    c.is_active = active;
    return c;
}

} // namespace

TEST(ConfigCheck, AcceptsOnlyValuesTheTableCanStore)
{
    EXPECT_FALSE(device::check_config(50, 0.1).has_value());
    EXPECT_FALSE(device::check_config(10000, 1000.0).has_value());

    const auto rate = device::check_config(49, 1.0);
    ASSERT_TRUE(rate.has_value());
    EXPECT_EQ(rate->code, "INVALID_RATE");

    const auto fast = device::check_config(10001, 1.0);
    ASSERT_TRUE(fast.has_value());
    EXPECT_EQ(fast->code, "INVALID_RATE");

    const auto amp = device::check_config(100, 1000.1);
    ASSERT_TRUE(amp.has_value());
    EXPECT_EQ(amp->code, "INVALID_AMPLITUDE");
    EXPECT_EQ(device::check_config(100, 0.0)->code, "INVALID_AMPLITUDE");
}

// The node re-arms its timer when on_config reports Config. tick() itself never
// waits, so a config that shortens the rate takes effect on the next tick.
TEST(SampleStream, ConfigAppliesBeforeTheNextTick)
{
    StreamHarness h;
    h.stream.apply(config(2, 60000, 5.0, true));
    const auto t0 = Clock::now();
    h.stream.tick(t0);
    EXPECT_EQ(h.stream.on_config(config(2, 20, 5.0, true)), device::SampleStream::Effect::Config);
    for (int i = 0; i < 5; ++i) h.stream.tick(t0);
    std::vector<std::int64_t> seqs;
    for (const auto& msg : h.sent)
        seqs.push_back(std::get<msg::ScalarSample>(msg).seq);
    EXPECT_EQ(seqs, (std::vector<std::int64_t>{1, 2, 3, 4, 5, 6}));
    EXPECT_EQ(h.stream.rate_ms(), 20);
    EXPECT_EQ(h.stream.period(), std::chrono::milliseconds(20));
    EXPECT_DOUBLE_EQ(h.stream.amplitude(), 5.0);
}

TEST(SampleStream, PausedDeviceSendsHeartbeatsInsteadOfSamples)
{
    StreamHarness h;
    h.stream.apply(config(1, 10, 1.0, false));
    const auto t0 = Clock::now();
    h.stream.tick(t0);
    h.stream.tick(t0 + std::chrono::milliseconds(300));
    EXPECT_TRUE(h.sent.empty());
    h.stream.tick(t0 + std::chrono::seconds(5));
    ASSERT_EQ(h.sent.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<msg::Heartbeat>(h.sent[0]));

    EXPECT_EQ(h.stream.on_config(config(1, 10, 1.0, true)), device::SampleStream::Effect::Config);
    h.stream.tick(t0 + std::chrono::seconds(5));
    ASSERT_EQ(h.sent.size(), 2u);
    EXPECT_EQ(std::get<msg::ScalarSample>(h.sent[1]).seq, 1);
}

TEST(SampleStream, ByeStopsTheStream)
{
    StreamHarness h;
    h.stream.apply(config(1, 10, 1.0, true));
    const auto t0 = Clock::now();
    h.stream.tick(t0);
    h.stream.tick(t0);
    h.stream.tick(t0);
    msg::Bye bye;
    bye.reason = "test";
    EXPECT_EQ(h.stream.on_bye(bye), device::SampleStream::Effect::Bye);
    EXPECT_TRUE(h.stream.stopped());
    h.stream.tick(t0);
    EXPECT_EQ(h.sent.size(), 3u);
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
    const auto scalar = std::get<msg::ScalarSample>(device::make_sample(device::Generator::Sine, 10, 2.0, 0.0, 0.0));
    EXPECT_EQ(scalar.seq, 10);
    EXPECT_NEAR(scalar.value, 2.0, 1e-9);

    const auto gps = std::get<msg::GpsSample>(device::make_sample(device::Generator::GpsOrbit, 10, 500.0, 0.0, 0.0));
    EXPECT_EQ(gps.origin_lon, device::kOriginLon);
    EXPECT_EQ(gps.origin_lat, device::kOriginLat);
    EXPECT_EQ(gps.radius_m, 500.0);
    EXPECT_NEAR(gps.bearing_rad, std::numbers::pi / 2.0, 1e-9);

    const auto noise = std::get<msg::ScalarSample>(device::make_sample(device::Generator::WhiteNoise, 3, 2.0, 0.0, 1.5));
    EXPECT_EQ(noise.seq, 3);
    EXPECT_DOUBLE_EQ(noise.value, 3.0);

    const auto cloud = std::get<msg::XySample>(device::make_sample(device::Generator::Scatter, 1, 100.0, 0.0, 0.0, 0.0, 1.0));
    EXPECT_EQ(cloud.seq, 1);
    EXPECT_DOUBLE_EQ(cloud.x, -100.0);
    EXPECT_DOUBLE_EQ(cloud.y, 100.0);

    const auto cube = std::get<msg::XyzSample>(
        device::make_sample(device::Generator::Cloud, 2, 100.0, 0.0, 0.0, 0.0, 1.0, 0.0));
    EXPECT_EQ(cube.seq, 2);
    EXPECT_DOUBLE_EQ(cube.x, -100.0);
    EXPECT_DOUBLE_EQ(cube.y, 100.0);
    EXPECT_DOUBLE_EQ(cube.z, -100.0);
}

TEST(Generators, TypeSelectsWhiteNoiseAndScatter)
{
    EXPECT_EQ(device::generator_for("white_noise", "scalar"), device::Generator::WhiteNoise);
    EXPECT_EQ(device::generator_for("scatter", "xy"), device::Generator::Scatter);
    EXPECT_EQ(device::generator_for("sine", "scalar"), device::Generator::Sine);
    EXPECT_EQ(device::generator_for("anything", "xy"), device::Generator::Scatter);
    EXPECT_EQ(device::generator_for("cloud", "xyz"), device::Generator::Cloud);
    EXPECT_EQ(device::generator_for("anything", "xyz"), device::Generator::Cloud);
}
