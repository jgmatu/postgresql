#pragma once

#include <chrono>
#include <cstdlib>
#include <format>
#include <iostream>
#include <mutex>
#include <string_view>

namespace pgdemo::log {

enum class Level { Debug = 0, Info = 1, Warn = 2, Error = 3 };

inline Level threshold()
{
    static const Level level = [] {
        const char* env = std::getenv("PGDEMO_LOG_LEVEL");
        if (env == nullptr) return Level::Info;
        const std::string_view v{env};
        if (v == "debug") return Level::Debug;
        if (v == "warn") return Level::Warn;
        if (v == "error") return Level::Error;
        return Level::Info;
    }();
    return level;
}

inline void write(Level level, std::string_view component, std::string_view message)
{
    if (level < threshold()) return;

    static std::mutex mtx;
    static constexpr std::string_view names[] = {"DEBUG", "INFO", "WARN", "ERROR"};

    const auto now = std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now());
    const std::string line = std::format("{:%FT%T}Z {:<5} [{}] {}\n", now,
                                         names[static_cast<int>(level)], component, message);
    std::lock_guard lock(mtx);
    std::clog << line << std::flush;
}

inline void debug(std::string_view c, std::string_view m) { write(Level::Debug, c, m); }
inline void info(std::string_view c, std::string_view m) { write(Level::Info, c, m); }
inline void warn(std::string_view c, std::string_view m) { write(Level::Warn, c, m); }
inline void error(std::string_view c, std::string_view m) { write(Level::Error, c, m); }

} // namespace pgdemo::log
