#pragma once

#include <charconv>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include <db/Errors.hpp>
#include <db/Tables.hpp>
#include <log/Log.hpp>

#include "AuthInterceptor.hpp"
#include "Json.hpp"
#include "oatpp/web/protocol/http/incoming/Request.hpp"

namespace pgdemo::server::http {

/// Incoming Oat++ request.
using IncomingRequest = oatpp::web::protocol::http::incoming::Request;

/// Invalid client input; becomes `400 bad_request` with the message.
struct BadRequest : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

/// Authenticated identity of the request, from the bundle set by `AuthInterceptor`.
struct Caller
{
    std::string username; ///< verified `sub` claim
    db::DbRole role;      ///< verified `role` claim; also the PostgreSQL group for the request

    /// True for the admin role.
    [[nodiscard]] bool is_admin() const noexcept { return role == db::DbRole::Admin; }
};

/**
 * @brief Identity stored by `AuthInterceptor` in the request bundle.
 * @throws std::logic_error on a public route, where no identity was stored
 */
inline Caller caller_of(const std::shared_ptr<IncomingRequest>& request)
{
    const auto sub = request->getBundleData<oatpp::String>(kBundleSubject);
    const auto role = request->getBundleData<oatpp::String>(kBundleRole);
    const auto parsed = role ? db::parse_jwt_role(*role) : std::nullopt;
    if (!sub || !parsed) throw std::logic_error("protected endpoint reached without authentication");
    return {*sub, *parsed};
}

/// `403 forbidden` with `message`.
inline std::shared_ptr<Response> forbidden(std::string_view message = "admin role required")
{
    return error_response(Status::CODE_403, "forbidden", message);
}

/**
 * @brief Optional integer query parameter in `[lo, hi]`.
 * @throws BadRequest when present but not an integer in range
 */
inline std::optional<int> int_param(const std::shared_ptr<IncomingRequest>& request, const char* name,
                                    int lo, int hi)
{
    const oatpp::String raw = request->getQueryParameter(name);
    if (!raw) return std::nullopt;
    const std::string_view s(*raw);
    int v = 0;
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || end != s.data() + s.size() || v < lo || v > hi)
    {
        throw BadRequest(std::string(name) + " must be an integer in [" + std::to_string(lo) + ", " +
                         std::to_string(hi) + "]");
    }
    return v;
}

/// Optional string query parameter, unvalidated.
inline std::optional<std::string> str_param(const std::shared_ptr<IncomingRequest>& request,
                                            const char* name)
{
    const oatpp::String raw = request->getQueryParameter(name);
    if (!raw) return std::nullopt;
    return std::string(*raw);
}

/**
 * @brief `dummy1` or `dummy2` from a path or query value.
 * @throws BadRequest for any other name
 */
inline db::DummyTable table_param(std::string_view name)
{
    const auto t = db::parse_dummy_table(name);
    if (!t) throw BadRequest("unknown table '" + std::string(name) + "' (expected dummy1 or dummy2)");
    return *t;
}

/**
 * @brief Run an endpoint body and map failures to HTTP.
 *
 * Database failures are mapped by SQLSTATE class. `403` from here means the
 * PostgreSQL role itself refused the statement, which is the second, independent
 * authorization layer behind the HTTP role check. Internal details are logged,
 * not returned.
 */
template <typename Fn>
std::shared_ptr<Response> guarded(std::string_view op, Fn&& fn)
{
    try
    {
        return std::forward<Fn>(fn)();
    }
    catch (const BadRequest& e)
    {
        return error_response(Status::CODE_400, "bad_request", e.what());
    }
    catch (const std::exception& e)
    {
        switch (db::classify(e))
        {
        case db::DbErrorKind::Conflict:
            return error_response(Status::CODE_409, "conflict", "resource already exists");
        case db::DbErrorKind::Constraint:
            log::warn("http", std::string(op) + ": constraint: " + e.what());
            return error_response(Status::CODE_422, "constraint", "rejected by a database constraint");
        case db::DbErrorKind::Forbidden:
            log::warn("http", std::string(op) + ": denied by PostgreSQL role: " + e.what());
            return forbidden("denied by database role");
        case db::DbErrorKind::Unavailable:
            log::error("http", std::string(op) + ": database unavailable: " + e.what());
            return error_response(Status::CODE_503, "unavailable", "database unavailable");
        case db::DbErrorKind::Other:
            break;
        }
        log::error("http", std::string(op) + ": " + e.what());
        return error_response(Status::CODE_500, "internal", "internal server error");
    }
}

} // namespace pgdemo::server::http
