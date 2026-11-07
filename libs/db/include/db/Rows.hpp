#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <db/Tables.hpp>

namespace pgdemo::db {

/// One sample written by a controller producer thread.
struct DummySample
{
    int item_id{1};
    double value{0.0};
    std::int64_t seq{0};
};

/// Applied configuration of one target (row of `dummy_config`).
struct AppliedConfig
{
    ConfigTarget target{ConfigTarget::Dummy1};
    int rate_ms{0};
    double amplitude{0.0};
    bool is_active{true};
    std::optional<std::int64_t> request_id;
};

/// Desired configuration (row of `dummy_config_pending`).
struct ConfigRequest
{
    std::int64_t request_id{0};
    ConfigTarget target{ConfigTarget::Dummy1};
    int rate_ms{0};
    double amplitude{0.0};
    bool is_active{true};
    std::string requested_by;
};

/// Decision taken by the controller for one queued request.
struct ConfigOutcome
{
    bool ok{false};
    std::string error_code;
    std::string error_detail;

    static ConfigOutcome success() { return {true, {}, {}}; }
    static ConfigOutcome failure(std::string code, std::string detail)
    {
        return {false, std::move(code), std::move(detail)};
    }
};

/// Result of a call to `drain_pending`.
struct DrainStats
{
    std::size_t applied{0};
    std::size_t rejected{0};
    std::size_t expired{0};
};

/// Row returned by `auth_fetch_user()`.
struct UserLoginRow
{
    std::string password_hash;
    std::string role;
};

} // namespace pgdemo::db
