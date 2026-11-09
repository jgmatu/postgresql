#include <wire/Frame.hpp>

namespace pgdemo::wire {

std::string encode(const boost::json::object& msg)
{
    const std::string body = boost::json::serialize(msg);
    if (body.size() > kMaxFrame) throw FrameError("frame of " + std::to_string(body.size()) + " bytes is too large");
    const auto n = static_cast<std::uint32_t>(body.size());
    std::string out;
    out.reserve(4 + body.size());
    out.push_back(static_cast<char>((n >> 24) & 0xff));
    out.push_back(static_cast<char>((n >> 16) & 0xff));
    out.push_back(static_cast<char>((n >> 8) & 0xff));
    out.push_back(static_cast<char>(n & 0xff));
    out += body;
    return out;
}

std::size_t body_length(const std::array<unsigned char, 4>& h)
{
    const std::uint32_t n = (std::uint32_t{h[0]} << 24) | (std::uint32_t{h[1]} << 16) |
                            (std::uint32_t{h[2]} << 8) | std::uint32_t{h[3]};
    if (n == 0 || n > kMaxFrame) throw FrameError("invalid frame length " + std::to_string(n));
    return n;
}

boost::json::object decode(std::string_view body)
{
    boost::system::error_code ec;
    auto value = boost::json::parse(body, ec);
    if (ec) throw FrameError("frame is not JSON: " + ec.message());
    if (!value.is_object()) throw FrameError("frame is not a JSON object");
    return std::move(value.as_object());
}

std::optional<std::string> get_string(const boost::json::object& msg, std::string_view key)
{
    const auto* v = msg.if_contains(key);
    if (v == nullptr || !v->is_string()) return std::nullopt;
    return std::string(v->as_string());
}

std::optional<double> get_number(const boost::json::object& msg, std::string_view key)
{
    const auto* v = msg.if_contains(key);
    if (v == nullptr || !v->is_number()) return std::nullopt;
    return v->to_number<double>();
}

std::string type_of(const boost::json::object& msg) { return get_string(msg, "type").value_or(""); }

} // namespace pgdemo::wire
