#pragma once

#include <memory>
#include <string>

#include <auth/PasswordHasher.hpp>

#include "Services.hpp"
#include "http/Common.hpp"
#include "oatpp/core/macro/codegen.hpp"
#include "oatpp/web/server/api/ApiController.hpp"

#include OATPP_CODEGEN_BEGIN(ApiController)

namespace pgdemo::server {

/**
 * @brief Login and identity.
 *
 * POST /api/auth/login  {"username","password"} -> {"token","token_type","expires_in","role"}
 * GET  /api/auth/me     identity carried by the token
 */
class AuthController : public oatpp::web::server::api::ApiController
{
public:
    /**
     * @param services shared server state
     * @param mapper   Oat++ object mapper for the controller
     */
    AuthController(std::shared_ptr<Services> services, const std::shared_ptr<ObjectMapper>& mapper)
        : oatpp::web::server::api::ApiController(mapper)
        , s_(std::move(services))
    {
    }

    /// `POST /api/auth/login` (public): 200 with an ES384 token, 400 bad body, 401 bad credentials.
    ENDPOINT("POST", "/api/auth/login", login, BODY_STRING(String, body))
    {
        return http::guarded("login", [&] {
            const auto in = http::parse_object(body);
            const auto* user = in ? in->if_contains("username") : nullptr;
            const auto* pass = in ? in->if_contains("password") : nullptr;
            if (!user || !user->is_string() || !pass || !pass->is_string())
            {
                throw http::BadRequest(R"(body must be {"username": "...", "password": "..."})");
            }
            const std::string username(user->as_string());
            const std::string password(pass->as_string());
            if (username.empty() || username.size() > 64 || password.empty() || password.size() > 1024)
            {
                return invalid_credentials();
            }

            // Same Argon2 work whether or not the user exists.
            const auto row = s_->db->fetch_user_for_login(username);
            if (!row)
            {
                auth::PasswordHasher::verify_dummy(password);
                log::info("auth", "login failed (unknown user)");
                return invalid_credentials();
            }
            if (!auth::PasswordHasher::verify(password, row->password_hash))
            {
                log::info("auth", "login failed for '" + username + "'");
                return invalid_credentials();
            }

            const auto token = s_->jwt->generate(username, row->role, s_->config.token_ttl_s);
            log::info("auth", "login ok for '" + username + "' (" + row->role + ")");
            return http::json_response(Status::CODE_200,
                                       boost::json::object{{"token", token},
                                                           {"token_type", "Bearer"},
                                                           {"expires_in", s_->config.token_ttl_s},
                                                           {"role", row->role}});
        });
    }

    /// `GET /api/auth/me` (any role): 200 with username, role and PostgreSQL group.
    ENDPOINT("GET", "/api/auth/me", me, REQUEST(std::shared_ptr<IncomingRequest>, request))
    {
        return http::guarded("me", [&] {
            const auto c = http::caller_of(request);
            return http::json_response(
                Status::CODE_200,
                boost::json::object{{"username", c.username},
                                    {"role", db::jwt_role_name(c.role)},
                                    {"pg_role", db::pg_role_name(c.role)}});
        });
    }

private:
    static std::shared_ptr<OutgoingResponse> invalid_credentials()
    {
        return http::error_response(Status::CODE_401, "unauthorized", "invalid username or password");
    }

    std::shared_ptr<Services> s_;
};

} // namespace pgdemo::server

#include OATPP_CODEGEN_END(ApiController)
