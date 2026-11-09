#pragma once

#include <cmath>
#include <cstdint>
#include <numbers>
#include <string_view>

#include <boost/json.hpp>

/// Signal generators of the device process. Pure functions of the tick.
namespace pgdemo::device {

/// Generator selected by the device type.
enum class Generator {
    Sine,     ///< `a·sin(2π·seq/40 + phase)`
    Sawtooth, ///< `a·(seq mod 50)/50` plus 5 % gaussian noise
    GpsOrbit, ///< circle of radius `a` metres around Madrid, one turn per 40 ticks
};

/// GPS payloads orbit, `sawtooth` ramps, every other scalar type is a sine.
constexpr Generator generator_for(std::string_view type_id, std::string_view payload) noexcept
{
    if (payload == "gps") return Generator::GpsOrbit;
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

/// `sample` frame for tick `seq`. PostGIS projects GPS fixes from the origin.
inline boost::json::object make_sample(Generator g, std::int64_t seq, double amplitude, double phase,
                                       double noise)
{
    boost::json::object m{{"type", "sample"}, {"seq", seq}};
    switch (g)
    {
    case Generator::GpsOrbit:
        m["origin_lon"] = kOriginLon;
        m["origin_lat"] = kOriginLat;
        m["radius_m"] = amplitude;
        m["bearing_rad"] = angle_at(seq, phase);
        break;
    case Generator::Sawtooth:
        m["value"] = sawtooth(seq, amplitude, noise);
        break;
    case Generator::Sine:
        m["value"] = sine(seq, amplitude, phase);
        break;
    }
    return m;
}

} // namespace pgdemo::device
