#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <db/Tables.hpp>

namespace pgdemo::db {

/// One sample written by a controller producer thread.
struct DummySample
{
    int item_id{1};       ///< primary key of the live row (1..items)
    double value{0.0};    ///< generated value
    std::int64_t seq{0};  ///< producer tick counter, increases by one per tick
};

/// One GPS fix for `dummy3`. PostGIS projects the point from the origin.
struct TrackSample
{
    int item_id{1};            ///< primary key of the live row (1..items)
    double origin_lon{0.0};    ///< origin longitude, degrees
    double origin_lat{0.0};    ///< origin latitude, degrees
    double radius_m{0.0};      ///< orbit radius in metres (`amplitude`)
    double bearing_rad{0.0};   ///< azimuth passed to `ST_Project`, radians clockwise from north
    std::int64_t seq{0};       ///< producer tick counter, increases by one per tick
};

/// Applied configuration of one target (row of `dummy_config`).
struct AppliedConfig
{
    ConfigTarget target{ConfigTarget::Dummy1};  ///< table the producer writes
    int rate_ms{0};                             ///< period between ticks
    double amplitude{0.0};                      ///< scale of the generated values
    bool is_active{true};                       ///< false pauses the producer
    std::optional<std::int64_t> request_id;     ///< request that produced this state; empty for the seed row
};

/// Desired configuration (row of `dummy_config_pending`).
struct ConfigRequest
{
    std::int64_t request_id{0};                 ///< from `request_id_seq`, unique across processes
    ConfigTarget target{ConfigTarget::Dummy1};  ///< table to reconfigure
    int rate_ms{0};                             ///< requested period between ticks
    double amplitude{0.0};                      ///< requested amplitude
    bool is_active{true};                       ///< requested on/off state
    std::string requested_by;                   ///< username of the admin who submitted it
};

/// Decision taken by the controller for one queued request.
struct ConfigOutcome
{
    bool ok{false};            ///< true: apply; false: record an error
    std::string error_code;    ///< machine-readable reason, e.g. `INVALID_RATE`; empty on success
    std::string error_detail;  ///< human-readable reason; empty on success

    /// Accept the request.
    static ConfigOutcome success() { return {true, {}, {}}; }

    /// Reject the request with `code` and `detail` (stored in `dummy_config_errors`).
    static ConfigOutcome failure(std::string code, std::string detail)
    {
        return {false, std::move(code), std::move(detail)};
    }
};

/// Result of a call to `drain_pending`.
struct DrainStats
{
    std::size_t applied{0};   ///< requests committed to `dummy_config`
    std::size_t rejected{0};  ///< requests refused by `decide` (status ERROR)
    std::size_t expired{0};   ///< requests older than `max_age` (status TIMEOUT)
};

/// Row returned by `auth_fetch_user()`.
struct UserLoginRow
{
    std::string password_hash;  ///< Argon2id PHC string
    std::string role;           ///< `admin` or `normal`
};

} // namespace pgdemo::db
