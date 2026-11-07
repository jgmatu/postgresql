#pragma once

#include <memory>
#include <string>
#include <string_view>

#include <auth/JwtManager.hpp>
#include <db/Tables.hpp>

#include "Json.hpp"
#include "oatpp/web/server/interceptor/RequestInterceptor.hpp"

namespace pgdemo::server::http {

/// Bundle keys set by `AuthInterceptor` for the controllers.
inline constexpr const char* kBundleSubject = "auth.sub";
inline constexpr const char* kBundleRole = "auth.role";

/**
 * @brief Authentication gate: every route needs `Authorization: Bearer <JWT>`
 * except the two public ones below.
 *
 * It is an allow-list of public paths, not a list of protected ones, so a route
 * added later is protected by default. On success the verified `sub` and `role`
 * claims go into the request bundle; authorization (which role may do what) is
 * the controllers' job and, underneath, PostgreSQL's.
 */
class AuthInterceptor : public oatpp::web::server::interceptor::RequestInterceptor
{
public:
    explicit AuthInterceptor(std::shared_ptr<auth::JwtManager> jwt)
        : jwt_(std::move(jwt))
    {
    }

    std::shared_ptr<OutgoingResponse> intercept(const std::shared_ptr<IncomingRequest>& request) override
    {
        std::string path = request->getStartingLine().path.std_str();
        if (const auto q = path.find('?'); q != std::string::npos) path.resize(q);
        if (path == "/api/auth/login" || path == "/api/health") return nullptr;

        const oatpp::String header = request->getHeader("Authorization");
        constexpr std::string_view kBearer = "Bearer ";
        if (!header || !std::string_view(*header).starts_with(kBearer))
        {
            return unauthorized("missing bearer token");
        }

        const auto claims = jwt_->verify(std::string(std::string_view(*header).substr(kBearer.size())));
        if (!claims || !db::parse_jwt_role(claims->role))
        {
            return unauthorized("invalid or expired token");
        }

        request->putBundleData(kBundleSubject, oatpp::String(claims->sub));
        request->putBundleData(kBundleRole, oatpp::String(claims->role));
        return nullptr;
    }

private:
    static std::shared_ptr<OutgoingResponse> unauthorized(std::string_view why)
    {
        auto r = error_response(Status::CODE_401, "unauthorized", why);
        r->putOrReplaceHeader("WWW-Authenticate", "Bearer realm=\"pgdemo\"");
        return r;
    }

    std::shared_ptr<auth::JwtManager> jwt_;
};

} // namespace pgdemo::server::http
