#pragma once

#include <cstddef>
#include <optional>
#include <string_view>

namespace pgdemo::db {

/**
 * @brief Shape of the data a device produces. Selects the telemetry table and
 * the NOTIFY channel. The device travels in the row; the notification names the table.
 *
 * SQL identifiers are never built from caller strings: every table and channel
 * name comes from this fixed whitelist. Devices are rows of `device`, so adding
 * one changes neither the schema nor this list.
 */
enum class Payload {
    Scalar, ///< one `double` per sample: `telemetry_scalar`, channel `telemetry_scalar`
    Gps,    ///< one WGS84 point per sample: `telemetry_gps`, channel `telemetry_gps`
    Xy,     ///< one native `point` per sample: `telemetry_xy`, channel `telemetry_xy`
};

/// Value of `device_type.payload`: `scalar`, `gps` or `xy`.
constexpr std::string_view payload_name(Payload p) noexcept
{
    if (p == Payload::Gps) return "gps";
    if (p == Payload::Xy) return "xy";
    return "scalar";
}

/// Inverse of `payload_name()`. `std::nullopt` for any other string.
constexpr std::optional<Payload> parse_payload(std::string_view name) noexcept
{
    if (name == "scalar") return Payload::Scalar;
    if (name == "gps") return Payload::Gps;
    if (name == "xy") return Payload::Xy;
    return std::nullopt;
}

/// Partitioned, append-only telemetry table of the payload.
constexpr std::string_view telemetry_table(Payload p) noexcept
{
    if (p == Payload::Gps) return "telemetry_gps";
    if (p == Payload::Xy) return "telemetry_xy";
    return "telemetry_scalar";
}

/// NOTIFY channel fired by an INSERT into the telemetry table of this payload.
constexpr std::string_view telemetry_channel(Payload p) noexcept
{
    if (p == Payload::Gps) return "telemetry_gps";
    if (p == Payload::Xy) return "telemetry_xy";
    return "telemetry_scalar";
}

/// Every payload, for loops that must cover all telemetry tables.
constexpr Payload kAllPayloads[] = {Payload::Scalar, Payload::Gps, Payload::Xy};

/// Rows `take_unread_telemetry` reads and consumes in one transaction.
constexpr std::size_t kTelemetryReadPage = 1024;

/**
 * @brief Same rule as the `CHECK` on `device.name`: `^[a-z][a-z0-9_.-]{1,63}$`.
 *
 * Lets the HTTP layer answer 400 before touching the database.
 */
constexpr bool valid_device_name(std::string_view name) noexcept
{
    if (name.size() < 2 || name.size() > 64) return false;
    if (name[0] < 'a' || name[0] > 'z') return false;
    for (const char c : name.substr(1))
    {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' ||
                        c == '-';
        if (!ok) return false;
    }
    return true;
}

/// NOTIFY channels besides telemetry (see sql/04_triggers.sql).
namespace channel {
/// A device was registered, removed or re-keyed, a certificate revoked or
/// restored, or a device process changed state. `action` tells which.
inline constexpr std::string_view kDeviceEvents = "device_events";
/// Admin queued an enrollment or rotation; wakes the controller.
inline constexpr std::string_view kEnrollmentRequested = "enrollment_requested";
/// A device process submitted its CSR; wakes the server, which signs it.
inline constexpr std::string_view kEnrollmentCsr = "enrollment_csr";
/// An enrollment was ISSUED, REJECTED or EXPIRED; payload carries `request_id` and `status`.
inline constexpr std::string_view kEnrollmentEvents = "enrollment_events";
/// Server inserted a PENDING request; wakes the controller's queue drain.
inline constexpr std::string_view kConfigRequested = "config_requested";
/// Controller applied a configuration (`device_config` changed); payload carries `request_id`.
inline constexpr std::string_view kConfigEvents = "config_events";
/// Controller rejected or expired a request; payload carries `request_id` and `error_code`.
inline constexpr std::string_view kErrorEvents = "error_events";
} // namespace channel

/**
 * @brief PostgreSQL group the server switches into for one request
 * (`SET LOCAL ROLE`), chosen from the verified JWT role claim.
 */
enum class DbRole {
    Admin,  ///< `demo_admin`: may enroll and revoke devices, enqueue configuration and manage users
    Normal, ///< `demo_normal`: read-only
};

/// PostgreSQL group used in `SET LOCAL ROLE`: `demo_admin` or `demo_normal`.
constexpr std::string_view pg_role_name(DbRole r) noexcept
{
    return r == DbRole::Admin ? "demo_admin" : "demo_normal";
}

/// Value of the JWT `role` claim and of `app_users.role`: `admin` or `normal`.
constexpr std::string_view jwt_role_name(DbRole r) noexcept
{
    return r == DbRole::Admin ? "admin" : "normal";
}

/// Inverse of `jwt_role_name()`. `std::nullopt` for any other string.
constexpr std::optional<DbRole> parse_jwt_role(std::string_view name) noexcept
{
    if (name == "admin") return DbRole::Admin;
    if (name == "normal") return DbRole::Normal;
    return std::nullopt;
}

} // namespace pgdemo::db
