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
 * User management, admin only.
 *
 * Two independent checks: the HTTP layer rejects a non-admin token with 403, and
 * the statement runs under `SET LOCAL ROLE demo_admin`, the only group with
 * privileges on `app_users`. A request that slipped past the first check would
 * still fail in PostgreSQL with SQLSTATE 42501.
 *
 * GET    /api/users
 * POST   /api/users            {"username","password","role"}
 * DELETE /api/users/{username}
 */
class UsersController : public oatpp::web::server::api::ApiController
{
public:
    UsersController(std::shared_ptr<Services> services, const std::shared_ptr<ObjectMapper>& mapper)
        : oatpp::web::server::api::ApiController(mapper)
        , s_(std::move(services))
    {
    }

    ENDPOINT("GET", "/api/users", list, REQUEST(std::shared_ptr<IncomingRequest>, request))
    {
        return http::guarded("list users", [&] {
            const auto c = http::caller_of(request);
            if (!c.is_admin()) return http::forbidden();
            return http::json_response(Status::CODE_200, s_->db->list_users(c.role));
        });
    }

    ENDPOINT("POST", "/api/users", create, REQUEST(std::shared_ptr<IncomingRequest>, request),
             BODY_STRING(String, body))
    {
        return http::guarded("create user", [&] {
            const auto c = http::caller_of(request);
            if (!c.is_admin()) return http::forbidden();

            const auto in = http::parse_object(body);
            const auto* user = in ? in->if_contains("username") : nullptr;
            const auto* pass = in ? in->if_contains("password") : nullptr;
            const auto* role = in ? in->if_contains("role") : nullptr;
            if (!user || !user->is_string() || !pass || !pass->is_string())
            {
                throw http::BadRequest(
                    R"(body must be {"username": "...", "password": "...", "role": "admin|normal"})");
            }
            const std::string username(user->as_string());
            const std::string password(pass->as_string());
            const std::string role_name = role && role->is_string() ? std::string(role->as_string()) : "normal";
            const auto new_role = db::parse_jwt_role(role_name);
            if (!new_role) throw http::BadRequest("role must be admin or normal");
            if (password.size() < 12 || password.size() > 1024)
            {
                throw http::BadRequest("password must be 12 to 1024 characters");
            }

            const int id = s_->db->create_user(c.role, username, auth::PasswordHasher::hash(password), *new_role);
            log::info("users", "'" + c.username + "' created user '" + username + "' (" + role_name + ")");
            return http::json_response(
                Status::CODE_201,
                boost::json::object{{"user_id", id}, {"username", username}, {"role", role_name}});
        });
    }

    ENDPOINT("DELETE", "/api/users/{username}", remove, REQUEST(std::shared_ptr<IncomingRequest>, request),
             PATH(String, username))
    {
        return http::guarded("delete user", [&] {
            const auto c = http::caller_of(request);
            if (!c.is_admin()) return http::forbidden();
            if (!s_->db->delete_user(c.role, *username))
            {
                return http::error_response(Status::CODE_404, "not_found", "no such user");
            }
            log::info("users", "'" + c.username + "' deleted user '" + std::string(*username) + "'");
            return http::json_response(Status::CODE_200, boost::json::object{{"deleted", *username}});
        });
    }

private:
    std::shared_ptr<Services> s_;
};

} // namespace pgdemo::server

#include OATPP_CODEGEN_END(ApiController)
