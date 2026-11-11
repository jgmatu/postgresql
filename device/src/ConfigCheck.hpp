#pragma once

#include <format>
#include <optional>
#include <string>

namespace pgdemo::device {

/// Why a desired configuration was not applied. Empty means the values fit `device_config`.
struct ConfigRejection
{
    std::string code;
    std::string detail;
};

/**
 * @brief The bounds of `device_config`: `rate_ms` in `[50, 10000]`, `amplitude` in `(0, 1000]`.
 *
 * A value that passes here can be stored. One that does not is refused and the previous
 * configuration stays in force.
 */
inline std::optional<ConfigRejection> check_config(int rate_ms, double amplitude)
{
    if (rate_ms < 50 || rate_ms > 10000)
        return ConfigRejection{"INVALID_RATE", std::format("rate_ms {} outside [50, 10000]", rate_ms)};
    if (!(amplitude > 0.0) || amplitude > 1000.0)
        return ConfigRejection{"INVALID_AMPLITUDE", std::format("amplitude {} outside (0, 1000]", amplitude)};
    return std::nullopt;
}

} // namespace pgdemo::device
