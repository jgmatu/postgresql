#pragma once

#include <cmath>
#include <cstdint>
#include <numbers>
#include <string_view>
#include <variant>

#include <pgdemo_msgs/msg/gps_sample.hpp>
#include <pgdemo_msgs/msg/scalar_sample.hpp>
#include <pgdemo_msgs/msg/xy_sample.hpp>
#include <pgdemo_msgs/msg/xyz_sample.hpp>

/// Signal generators of the device process. Pure functions of the tick.
namespace pgdemo::device {

/// Generator selected by the device type.
enum class Generator {
    Sine,       ///< `a·sin(2π·seq/40 + phase)`
    Sawtooth,   ///< `a·(seq mod 50)/50` plus 5 % gaussian noise
    WhiteNoise, ///< normal draw with standard deviation `a`
    GpsOrbit,   ///< circle of radius `a` metres around Madrid, one turn per 40 ticks
    Scatter,    ///< uniform `x` and `y` in `[-a, a]`
    Cloud,      ///< uniform `x`, `y` and `z` in `[-a, a]`
};

/// One sample. The node stamps `device` before publishing.
using Sample = std::variant<pgdemo_msgs::msg::ScalarSample, pgdemo_msgs::msg::GpsSample, pgdemo_msgs::msg::XySample,
                            pgdemo_msgs::msg::XyzSample>;

/// `gps` orbits, `xy` scatters, `xyz` fills the cube, `white_noise` and `sawtooth` are named, every other scalar type is a sine.
constexpr Generator generator_for(std::string_view type_id, std::string_view payload) noexcept
{
    if (payload == "gps") return Generator::GpsOrbit;
    if (payload == "xy") return Generator::Scatter;
    if (payload == "xyz") return Generator::Cloud;
    if (type_id == "white_noise") return Generator::WhiteNoise;
    if (type_id == "sawtooth") return Generator::Sawtooth;
    return Generator::Sine;
}

/// Phase offset so that devices of one type do not overlap: (id mod 8)·π/4.
constexpr double phase_for(int device_id) noexcept
{
    return static_cast<double>(device_id % 8) * std::numbers::pi / 4.0;
}

/// Angle of tick `seq`: one turn every 40 ticks.
inline double angle_at(std::int64_t seq, double phase) noexcept
{
    return 2.0 * std::numbers::pi * static_cast<double>(seq) / 40.0 + phase;
}

/// Sine value of tick `seq`.
inline double sine(std::int64_t seq, double amplitude, double phase) noexcept
{
    return amplitude * std::sin(angle_at(seq, phase));
}

/// Sawtooth value of tick `seq`; `noise` is a standard normal draw.
inline double sawtooth(std::int64_t seq, double amplitude, double noise) noexcept
{
    return amplitude * static_cast<double>(seq % 50) / 50.0 + 0.05 * amplitude * noise;
}

/// Longitude of the origin of every GPS orbit (Madrid).
inline constexpr double kOriginLon = -3.7038;
/// Latitude of that origin.
inline constexpr double kOriginLat = 40.4168;

/**
 * @brief One tick of a signal.
 *
 * `noise` is a standard normal draw. `u`, `v` and `w` are uniform on `[0, 1]`
 * and place a scatter sample in the square or a cloud sample in the cube.
 */
class SampleGenerator
{
public:
    virtual ~SampleGenerator() = default;

    /**
     * @brief Build the sample for tick `seq`.
     * @param seq       tick number
     * @param amplitude peak, standard deviation, orbit radius or half-extent
     * @param phase     radians, from `phase_for`
     * @param noise     a standard normal draw
     * @param u         uniform draw on `[0, 1]`
     * @param v         uniform draw on `[0, 1]`
     * @param w         uniform draw on `[0, 1]`
     */
    [[nodiscard]] virtual Sample make(std::int64_t seq, double amplitude, double phase, double noise, double u, double v,
                                      double w) const = 0;
};

/// `a·sin(2π·seq/40 + phase)` on the scalar topic.
class Sine : public SampleGenerator
{
public:
    [[nodiscard]] Sample make(std::int64_t seq, double amplitude, double phase, double /*noise*/, double /*u*/, double /*v*/,
                              double /*w*/) const override
    {
        pgdemo_msgs::msg::ScalarSample m;
        m.seq = seq;
        m.value = sine(seq, amplitude, phase);
        return m;
    }
};

/// `a·(seq mod 50)/50` plus 5 % gaussian noise, on the scalar topic.
class Sawtooth : public SampleGenerator
{
public:
    [[nodiscard]] Sample make(std::int64_t seq, double amplitude, double /*phase*/, double noise, double /*u*/, double /*v*/,
                              double /*w*/) const override
    {
        pgdemo_msgs::msg::ScalarSample m;
        m.seq = seq;
        m.value = sawtooth(seq, amplitude, noise);
        return m;
    }
};

/// Normal draw with standard deviation `a`, on the scalar topic.
class WhiteNoise : public SampleGenerator
{
public:
    [[nodiscard]] Sample make(std::int64_t seq, double amplitude, double /*phase*/, double noise, double /*u*/, double /*v*/,
                              double /*w*/) const override
    {
        pgdemo_msgs::msg::ScalarSample m;
        m.seq = seq;
        m.value = amplitude * noise;
        return m;
    }
};

/// Circle of radius `a` metres around Madrid, one turn per 40 ticks.
class GpsOrbit : public SampleGenerator
{
public:
    [[nodiscard]] Sample make(std::int64_t seq, double amplitude, double phase, double /*noise*/, double /*u*/, double /*v*/,
                              double /*w*/) const override
    {
        pgdemo_msgs::msg::GpsSample m;
        m.seq = seq;
        m.origin_lon = kOriginLon;
        m.origin_lat = kOriginLat;
        m.radius_m = amplitude;
        m.bearing_rad = angle_at(seq, phase);
        return m;
    }
};

/// Uniform `x` and `y` in `[-a, a]`.
class Scatter : public SampleGenerator
{
public:
    [[nodiscard]] Sample make(std::int64_t seq, double amplitude, double /*phase*/, double /*noise*/, double u, double v,
                              double /*w*/) const override
    {
        pgdemo_msgs::msg::XySample m;
        m.seq = seq;
        m.x = (2.0 * u - 1.0) * amplitude;
        m.y = (2.0 * v - 1.0) * amplitude;
        return m;
    }
};

/// Uniform `x`, `y` and `z` in `[-a, a]`.
class Cloud : public SampleGenerator
{
public:
    [[nodiscard]] Sample make(std::int64_t seq, double amplitude, double /*phase*/, double /*noise*/, double u, double v,
                              double w) const override
    {
        pgdemo_msgs::msg::XyzSample m;
        m.seq = seq;
        m.x = (2.0 * u - 1.0) * amplitude;
        m.y = (2.0 * v - 1.0) * amplitude;
        m.z = (2.0 * w - 1.0) * amplitude;
        return m;
    }
};

} // namespace pgdemo::device
