#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>
#include <boost/json.hpp>

/**
 * @brief Controller <-> device protocol over the Unix socket.
 *
 * Each frame is a 4-byte big-endian length followed by that many bytes of a
 * JSON object with a `type` member. Messages, in order:
 *
 * | direction | type        | members                                              |
 * |-----------|-------------|------------------------------------------------------|
 * | dev -> ctl | `hello`     | `device`, `mode` (run, enroll, rotate), `request_id` |
 * | dev -> ctl | `csr`       | `csr` (PEM); enroll and rotate only                  |
 * | ctl -> dev | `cert`      | `cert` (PEM), or                                     |
 * | ctl -> dev | `rejected`  | `code`, `detail`                                     |
 * | ctl -> dev | `challenge` | `nonce` (base64, 32 bytes)                           |
 * | dev -> ctl | `proof`     | `cert` (PEM), `signature` (base64, raw r‖s)          |
 * | dev -> ctl | `error`     | `code`, `detail`; the device cannot continue         |
 * | ctl -> dev | `config`    | `device_id`, `rate_ms`, `amplitude`, `is_active`     |
 * | dev -> ctl | `sample`    | `seq`, `value`, or `seq`, `origin_lon`, `origin_lat`, `radius_m`, `bearing_rad` |
 * | dev -> ctl | `heartbeat` | (none); sent while paused                            |
 * | ctl -> dev | `bye`       | `reason`; the device exits                           |
 */
namespace pgdemo::wire {

/// Largest frame body accepted in either direction.
inline constexpr std::size_t kMaxFrame = 64 * 1024;

/// A frame that is too large, empty, or not a JSON object.
struct FrameError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

/// Length prefix plus the serialized object. @throws FrameError if it exceeds `kMaxFrame`.
std::string encode(const boost::json::object& msg);

/// Parse a frame body. @throws FrameError unless it is a JSON object.
boost::json::object decode(std::string_view body);

/// Body length from a 4-byte header. @throws FrameError if 0 or above `kMaxFrame`.
std::size_t body_length(const std::array<unsigned char, 4>& header);

/// `msg[key]` as a string, or `std::nullopt` when absent or not a string.
std::optional<std::string> get_string(const boost::json::object& msg, std::string_view key);

/// `msg[key]` as a number, or `std::nullopt` when absent or not a number.
std::optional<double> get_number(const boost::json::object& msg, std::string_view key);

/// `msg["type"]`, or an empty string.
std::string type_of(const boost::json::object& msg);

/// Read one frame. @throws FrameError, boost::system::system_error (EOF included)
template <typename Stream>
boost::asio::awaitable<boost::json::object> read_frame(Stream& stream)
{
    std::array<unsigned char, 4> header{};
    co_await boost::asio::async_read(stream, boost::asio::buffer(header), boost::asio::use_awaitable);
    std::string body(body_length(header), '\0');
    co_await boost::asio::async_read(stream, boost::asio::buffer(body), boost::asio::use_awaitable);
    co_return decode(body);
}

/// Write one frame. Only one write may be in flight per stream.
template <typename Stream>
boost::asio::awaitable<void> write_frame(Stream& stream, const boost::json::object& msg)
{
    const std::string data = encode(msg);
    co_await boost::asio::async_write(stream, boost::asio::buffer(data), boost::asio::use_awaitable);
}

} // namespace pgdemo::wire
