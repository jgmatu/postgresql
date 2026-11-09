#pragma once

#include <cctype>
#include <string>
#include <string_view>

namespace pgdemo::server::http {

/**
 * @brief Decode `%XX` escapes in a query value.
 *
 * Oat++ hands query values over as they arrived, so `channels=a%2Cb` (what
 * URLSearchParams and most proxies send) would otherwise reach the endpoint
 * with `%2C` in place of the comma. `+` is left alone: timestamps such as
 * `from=2026-11-09T10:00:00+01:00` are commonly typed with a literal plus.
 * A malformed escape is kept as it is.
 */
inline std::string percent_decode(std::string_view in)
{
    const auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i)
    {
        if (in[i] == '%' && i + 2 < in.size())
        {
            const int hi = hex(in[i + 1]);
            const int lo = hex(in[i + 2]);
            if (hi >= 0 && lo >= 0)
            {
                out.push_back(static_cast<char>(hi * 16 + lo));
                i += 2;
                continue;
            }
        }
        out.push_back(in[i]);
    }
    return out;
}

/**
 * @brief Whether a request path (without query string) belongs to the API,
 * where `AuthInterceptor` requires a token; the rest is the control panel.
 *
 * Case, escapes and leading slashes are folded first, so `/API/devices`,
 * `/%61pi/devices` or `//api/devices` count as API paths too. Routing is
 * literal, so none of them reaches an endpoint, but neither is served as a
 * public panel path.
 */
inline bool is_api_path(std::string_view path)
{
    std::string p = percent_decode(path);
    for (auto& c : p) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const auto start = p.find_first_not_of('/');
    const std::string_view rest = start == std::string::npos ? std::string_view{} : std::string_view(p).substr(start);
    return !p.empty() && p.front() == '/' && (rest == "api" || rest.starts_with("api/"));
}

} // namespace pgdemo::server::http
