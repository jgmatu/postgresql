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
enum class DummyTable { Dummy1, Dummy2 };

/// The control plane configures exactly the same two targets.
using ConfigTarget = DummyTable;

constexpr std::string_view table_name(DummyTable t) noexcept
{
    return t == DummyTable::Dummy1 ? "dummy1" : "dummy2";
}

constexpr std::string_view history_table_name(DummyTable t) noexcept
{
    return t == DummyTable::Dummy1 ? "dummy1_history" : "dummy2_history";
}

constexpr std::string_view channel_name(DummyTable t) noexcept
{
    return t == DummyTable::Dummy1 ? "dummy1_events" : "dummy2_events";
}

constexpr std::optional<DummyTable> parse_dummy_table(std::string_view name) noexcept
{
    if (name == "dummy1") return DummyTable::Dummy1;
    if (name == "dummy2") return DummyTable::Dummy2;
    return std::nullopt;
}

constexpr DummyTable kAllTables[] = {DummyTable::Dummy1, DummyTable::Dummy2};

/// NOTIFY channels of the control plane (see sql/04_triggers.sql).
namespace channel {
inline constexpr std::string_view kConfigRequested = "config_requested";
inline constexpr std::string_view kConfigEvents = "config_events";
inline constexpr std::string_view kErrorEvents = "error_events";
} // namespace channel

/**
 * @brief PostgreSQL group the server switches into for one request
 * (`SET LOCAL ROLE`), chosen from the verified JWT role claim.
 */
enum class DbRole { Admin, Normal };

constexpr std::string_view pg_role_name(DbRole r) noexcept
{
    return r == DbRole::Admin ? "demo_admin" : "demo_normal";
}

constexpr std::string_view jwt_role_name(DbRole r) noexcept
{
    return r == DbRole::Admin ? "admin" : "normal";
}

constexpr std::optional<DbRole> parse_jwt_role(std::string_view name) noexcept
{
    if (name == "admin") return DbRole::Admin;
    if (name == "normal") return DbRole::Normal;
    return std::nullopt;
}

} // namespace pgdemo::db
