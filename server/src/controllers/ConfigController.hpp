#pragma once

#include <charconv>
#include <exception>
#include <memory>
#include <optional>
#include <string>

#include "Services.hpp"
#include "http/Common.hpp"
#include "oatpp/core/macro/codegen.hpp"
#include "oatpp/web/server/api/ApiController.hpp"

#include OATPP_CODEGEN_BEGIN(ApiController)

namespace pgdemo::server {

/**
 * @brief Control plane: desired vs applied configuration.
 *
 * GET  /api/config                      v_dummy_config_sync_status
 * POST /api/config                      admin; {"target", "rate_ms"?, "amplitude"?, "is_active"?}
 * GET  /api/config/requests/{id}        one request and its error, if any
 * GET  /api/config/history?target=&limit=
 *
 * POST is synchronous for the client and asynchronous underneath:
 *
 *   1. request_id = nextval('request_id_seq')
 *   2. Dispatcher registers a waiter for request_id
 *   3. INSERT into dummy_config_pending (trigger: NOTIFY config_requested)
 *   4. the controller validates and commits: dummy_config (NOTIFY config_events)
 *      or dummy_config_errors (NOTIFY error_events)
 *   5. the listener thread calls Dispatcher::dispatch(request_id, ...) and this
 *      thread wakes up
 *
 * If no answer arrives in time the request stays queued; the reply is 202 and
 * the controller applies it when it next drains the queue.
 */
class ConfigController : public oatpp::web::server::api::ApiController
{
public:
    /**
     * @param services shared server state
     * @param mapper   Oat++ object mapper for the controller
     */
    ConfigController(std::shared_ptr<Services> services, const std::shared_ptr<ObjectMapper>& mapper)
        : oatpp::web::server::api::ApiController(mapper)
        , s_(std::move(services))
    {
    }

    /// `GET /api/config` (any role): 200 with `v_dummy_config_sync_status`.
    ENDPOINT("GET", "/api/config", status, REQUEST(std::shared_ptr<IncomingRequest>, request))
    {
        return http::guarded("config status", [&] {
            const auto c = http::caller_of(request);
            return http::json_response(Status::CODE_200, s_->db->fetch_sync_status(c.role));
        });
    }

    /// `POST /api/config` (admin): 200 applied, 202 still queued, 400 bad body, 403 not admin,
    /// 422 rejected or expired, 503 too many requests in flight.
    ENDPOINT("POST", "/api/config", submit, REQUEST(std::shared_ptr<IncomingRequest>, request),
             BODY_STRING(String, body))
    {
        return http::guarded("config submit", [&] {
            const auto c = http::caller_of(request);
            if (!c.is_admin()) return http::forbidden();

            const auto in = http::parse_object(body);
            if (!in) throw http::BadRequest("body must be a JSON object");
            const auto* target_v = in->if_contains("target");
            if (!target_v || !target_v->is_string()) throw http::BadRequest("target is required");
            const auto target = http::table_param(target_v->as_string());

            db::ConfigRequest req = current_as_request(c.role, target);
            req.requested_by = c.username;
            if (const auto* v = in->if_contains("rate_ms"))
            {
                if (!v->is_int64() || v->as_int64() < 1 || v->as_int64() > 1'000'000)
                    throw http::BadRequest("rate_ms must be a positive integer");
                req.rate_ms = static_cast<int>(v->as_int64());
            }
            if (const auto* v = in->if_contains("amplitude"))
            {
                if (!v->is_number()) throw http::BadRequest("amplitude must be a number");
                req.amplitude = v->to_number<double>();
            }
            if (const auto* v = in->if_contains("is_active"))
            {
                if (!v->is_bool()) throw http::BadRequest("is_active must be a boolean");
                req.is_active = v->as_bool();
            }

            // The server only checks types. Whether the values are acceptable is the
            // controller's decision, and a rejection comes back as error_events.
            req.request_id = s_->db->generate_request_id(c.role);

            std::exception_ptr submit_error;
            const auto outcome = s_->dispatcher->wait_for_response(
                static_cast<std::uint64_t>(req.request_id), s_->config.config_timeout_ms, [&] {
                    try
                    {
                        s_->db->add_pending_config(c.role, req);
                    }
                    catch (...)
                    {
                        submit_error = std::current_exception();
                        throw;
                    }
                });

            switch (outcome)
            {
            case ResponseStatus::SYSTEM_FULL:
                return http::error_response(Status::CODE_503, "busy", "too many configuration requests in flight");
            case ResponseStatus::DB_ERROR:
                if (submit_error) std::rethrow_exception(submit_error);
                throw std::runtime_error("dispatcher rejected request id " + std::to_string(req.request_id));
            case ResponseStatus::SUCCESS:
            case ResponseStatus::ERROR:
            case ResponseStatus::TIMEOUT:
            case ResponseStatus::PENDING:
                break;
            }
            // TIMEOUT: the notification may have been missed (listener reconnect)
            // or the controller is down. The row is the source of truth.
            return reply_from_row(c.role, req.request_id, outcome);
        });
    }

    /// `GET /api/config/requests/{id}` (any role): 200 with the request and its error, 400 bad id, 404 unknown.
    ENDPOINT("GET", "/api/config/requests/{id}", request_by_id,
             REQUEST(std::shared_ptr<IncomingRequest>, request), PATH(String, id))
    {
        return http::guarded("config request", [&] {
            const auto c = http::caller_of(request);
            std::int64_t rid = 0;
            const std::string_view s(*id);
            const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), rid);
            if (ec != std::errc{} || end != s.data() + s.size() || rid < 1)
                throw http::BadRequest("request id must be a positive integer");
            const auto row = s_->db->fetch_request(c.role, rid);
            if (!row) return http::error_response(Status::CODE_404, "not_found", "no such request");
            return http::json_response(Status::CODE_200, *row);
        });
    }

    /// `GET /api/config/history?target=&limit=` (any role): 200 with the applied-configuration audit.
    ENDPOINT("GET", "/api/config/history", history, REQUEST(std::shared_ptr<IncomingRequest>, request))
    {
        return http::guarded("config history", [&] {
            const auto c = http::caller_of(request);
            std::optional<db::ConfigTarget> target;
            if (const auto t = http::str_param(request, "target")) target = http::table_param(*t);
            const int limit = http::int_param(request, "limit", 1, 1000).value_or(50);
            return http::json_response(Status::CODE_200, s_->db->fetch_config_history(c.role, target, limit));
        });
    }

private:
    /// The applied configuration of `target`, as the base of a partial update.
    db::ConfigRequest current_as_request(db::DbRole role, db::ConfigTarget target)
    {
        const auto rows = s_->db->fetch_sync_status(role);
        for (const auto& v : rows.as_array())
        {
            const auto& o = v.as_object();
            if (o.at("target").as_string() != db::table_name(target)) continue;
            db::ConfigRequest r;
            r.target = target;
            r.rate_ms = static_cast<int>(o.at("applied_rate_ms").to_number<std::int64_t>());
            r.amplitude = o.at("applied_amplitude").to_number<double>();
            r.is_active = o.at("applied_is_active").as_bool();
            return r;
        }
        throw std::runtime_error("no applied configuration for " + std::string(db::table_name(target)));
    }

    std::shared_ptr<OutgoingResponse> reply_from_row(db::DbRole role, std::int64_t id, ResponseStatus outcome)
    {
        const auto row = s_->db->fetch_request(role, id);
        if (!row) throw std::runtime_error("request " + std::to_string(id) + " vanished");
        const std::string st(row->as_object().at("status").as_string());

        boost::json::object out{{"request_id", id},
                                {"status", st},
                                {"dispatcher", Dispatcher::status_to_string(outcome)},
                                {"request", *row}};
        if (st == "SUCCESS") return http::json_response(Status::CODE_200, out);
        if (st == "PENDING")
        {
            out["message"] = "queued; the controller will apply it when it drains the queue";
            auto r = http::json_response(Status::CODE_202, out);
            r->putOrReplaceHeader("Location", oatpp::String("/api/config/requests/" + std::to_string(id)));
            return r;
        }
        return http::json_response(Status::CODE_422, out);
    }

    std::shared_ptr<Services> s_;
};

} // namespace pgdemo::server

#include OATPP_CODEGEN_END(ApiController)
