#pragma once

#include <chrono>
#include <format>
#include <string>

namespace pgdemo::db {

/// UTC `YYYY-MM-DDTHH:MM:SSZ`, the form stored in `timestamptz` parameters.
inline std::string format_time(std::chrono::system_clock::time_point tp)
{
    return std::format("{:%FT%TZ}", std::chrono::floor<std::chrono::seconds>(tp));
}

} // namespace pgdemo::db
