#pragma once

#include <optional>
#include <string_view>

namespace pgdemo::db {

/**
 * @brief The two data-plane tables. Each has its own trigger, NOTIFY channel and
 * partitioned history table.
 *
 * SQL identifiers are never built from caller strings: every table and channel
 * name comes from this fixed whitelist.
 */
enum class DummyTable {
    Dummy1, ///< `dummy1`: sine wave, channel `dummy1_events`
    Dummy2, ///< `dummy2`: noisy sawtooth, channel `dummy2_events`
};

/// The control plane configures exactly the same two targets.
using ConfigTarget = DummyTable;

/// Live table name: `dummy1` or `dummy2`. Also the SSE channel and the `target` value.
constexpr std::string_view table_name(DummyTable t) noexcept
{
    return t == DummyTable::Dummy1 ? "dummy1" : "dummy2";
}

/// Partitioned history table: `dummy1_history` or `dummy2_history`.
constexpr std::string_view history_table_name(DummyTable t) noexcept
{
    return t == DummyTable::Dummy1 ? "dummy1_history" : "dummy2_history";
}

/// NOTIFY channel fired by the table's trigger: `dummy1_events` or `dummy2_events`.
constexpr std::string_view channel_name(DummyTable t) noexcept
{
    return t == DummyTable::Dummy1 ? "dummy1_events" : "dummy2_events";
}

/// Inverse of `table_name()`. `std::nullopt` for any other string.
constexpr std::optional<DummyTable> parse_dummy_table(std::string_view name) noexcept
{
    if (name == "dummy1") return DummyTable::Dummy1;
    if (name == "dummy2") return DummyTable::Dummy2;
    return std::nullopt;
}

/// Every data-plane table, for loops that must cover both.
constexpr DummyTable kAllTables[] = {DummyTable::Dummy1, DummyTable::Dummy2};

/// NOTIFY channels of the control plane (see sql/04_triggers.sql).
namespace channel {
/// Server inserted a PENDING request; wakes the controller's queue drain.
inline constexpr std::string_view kConfigRequested = "config_requested";
/// Controller applied a request (`dummy_config` changed); payload carries `request_id`.
inline constexpr std::string_view kConfigEvents = "config_events";
/// Controller rejected or expired a request; payload carries `request_id` and `error_code`.
inline constexpr std::string_view kErrorEvents = "error_events";
} // namespace channel

/**
 * @brief PostgreSQL group the server switches into for one request
 * (`SET LOCAL ROLE`), chosen from the verified JWT role claim.
 */
enum class DbRole {
    Admin,  ///< `demo_admin`: may enqueue configuration and manage users
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
