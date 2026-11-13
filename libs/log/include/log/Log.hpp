#pragma once

#include <chrono>
#include <cstdlib>
#include <format>
#include <iostream>
#include <mutex>
#include <string_view>

namespace pgdemo::log {

/// Severity of a log line. Lines below `threshold()` are discarded.
enum class Level {
    Debug = 0, ///< diagnostic detail, off by default
    Info = 1,  ///< normal operation (default threshold)
    Warn = 2,  ///< recoverable problem
    Error = 3, ///< failure that needs attention
};

/**
 * @brief Minimum level written, read once from `PGDEMO_LOG_LEVEL`.
 *
 * Accepts `debug`, `warn` and `error`; anything else, or an unset variable, means `Info`.
 */
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

/// Mutex shared by `write`, so one line is not split by another.
inline std::mutex& sink_mutex()
{
    static std::mutex mtx;
    return mtx;
}

/**
 * @brief Write one UTC-timestamped line to `std::clog`. Thread-safe.
 *
 * Format: `2026-11-08T01:09:18.078Z INFO  [component] message`.
 *
 * @param level     severity; dropped when below `threshold()`
 * @param component short subsystem tag, e.g. `db` or `http`
 * @param message   line text, without a trailing newline
 */
inline void write(Level level, std::string_view component, std::string_view message)
{
    if (level < threshold()) return;

    static constexpr std::string_view names[] = {"DEBUG", "INFO", "WARN", "ERROR"};

    const auto now = std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now());
    const std::string line = std::format("{:%FT%T}Z {:<5} [{}] {}\n", now,
                                         names[static_cast<int>(level)], component, message);
    std::lock_guard lock(sink_mutex());
    std::clog << line << std::flush;
}

/// `write(Level::Debug, c, m)`.
inline void debug(std::string_view c, std::string_view m) { write(Level::Debug, c, m); }
/// `write(Level::Info, c, m)`.
inline void info(std::string_view c, std::string_view m) { write(Level::Info, c, m); }
/// `write(Level::Warn, c, m)`.
inline void warn(std::string_view c, std::string_view m) { write(Level::Warn, c, m); }
/// `write(Level::Error, c, m)`.
inline void error(std::string_view c, std::string_view m) { write(Level::Error, c, m); }

} // namespace pgdemo::log
