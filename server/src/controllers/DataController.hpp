#pragma once

#include <memory>
#include <set>
#include <string>
#include <string_view>

#include "Services.hpp"
#include "http/Common.hpp"
#include "http/SseBody.hpp"
#include "oatpp/core/macro/codegen.hpp"
#include "oatpp/web/protocol/http/outgoing/StreamingBody.hpp"
#include "oatpp/web/server/api/ApiController.hpp"

#include OATPP_CODEGEN_BEGIN(ApiController)

namespace pgdemo::server {

/**
 * Data plane, any authenticated role (reads run as demo_normal or demo_admin).
 *
 * GET /api/health                     public liveness
 * GET /api/db/info                    PostgreSQL / libpqxx / listener details
 * GET /api/data/{table}               live rows of dummy1 | dummy2
 * GET /api/history/{table}?from=&to=&item_id=&limit=
 * GET /api/stream?channels=dummy1,dummy2,config,error   Server-Sent Events
 */
class DataController : public oatpp::web::server::api::ApiController
{
public:
    DataController(std::shared_ptr<Services> services, const std::shared_ptr<ObjectMapper>& mapper)
        : oatpp::web::server::api::ApiController(mapper)
        , s_(std::move(services))
    {
    }

    ENDPOINT("GET", "/api/health", health)
    {
        const bool alive = s_->db->listener_alive();
        return http::json_response(alive ? Status::CODE_200 : Status::CODE_503,
                                   boost::json::object{{"status", alive ? "ok" : "degraded"},
                                                       {"listener_alive", alive}});
    }

    ENDPOINT("GET", "/api/db/info", info)
    {
        return http::guarded("db info", [&] {
            auto o = s_->db->get_sanity_info();
            o["sse_subscribers"] = s_->hub->subscribers();
            o["events_published"] = s_->hub->published();
            o["dispatcher_waiters"] = s_->dispatcher->pending();
            return http::json_response(Status::CODE_200, o);
        });
    }

    ENDPOINT("GET", "/api/data/{table}", data, REQUEST(std::shared_ptr<IncomingRequest>, request),
             PATH(String, table))
    {
        return http::guarded("data", [&] {
            const auto c = http::caller_of(request);
            return http::json_response(Status::CODE_200, s_->db->fetch_dummy(c.role, http::table_param(*table)));
        });
    }

    ENDPOINT("GET", "/api/history/{table}", history, REQUEST(std::shared_ptr<IncomingRequest>, request),
             PATH(String, table))
    {
        return http::guarded("history", [&] {
            const auto c = http::caller_of(request);
            const auto t = http::table_param(*table);
            const auto item = http::int_param(request, "item_id", 1, 1'000'000);
            const int limit = http::int_param(request, "limit", 1, 5000).value_or(100);
            return http::json_response(
                Status::CODE_200,
                s_->db->fetch_history(c.role, t, http::str_param(request, "from"), http::str_param(request, "to"),
                                      item, limit));
        });
    }

    ENDPOINT("GET", "/api/stream", stream, REQUEST(std::shared_ptr<IncomingRequest>, request))
    {
        return http::guarded("stream", [&]() -> std::shared_ptr<OutgoingResponse> {
            const auto c = http::caller_of(request);
            std::set<std::string> channels;
            if (const auto raw = http::str_param(request, "channels"))
            {
                std::string_view rest(*raw);
                while (!rest.empty())
                {
                    const auto comma = rest.find(',');
                    const auto name = std::string(rest.substr(0, comma));
                    if (!EventHub::all_channels().contains(name))
                    {
                        throw http::BadRequest("unknown channel '" + name + "'");
                    }
                    channels.insert(name);
                    rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
                }
            }
            if (channels.empty()) channels = EventHub::all_channels();

            auto sub = s_->hub->subscribe(channels);
            log::info("sse", "'" + c.username + "' subscribed (" + std::to_string(s_->hub->subscribers()) +
                                 " stream(s))");
            auto body = std::make_shared<oatpp::web::protocol::http::outgoing::StreamingBody>(
                std::make_shared<http::SseReadCallback>(s_->hub, std::move(sub)));
            auto r = OutgoingResponse::createShared(Status::CODE_200, body);
            r->putOrReplaceHeader("Content-Type", "text/event-stream");
            r->putOrReplaceHeader("Cache-Control", "no-cache");
            r->putOrReplaceHeader("X-Accel-Buffering", "no");
            return r;
        });
    }

private:
    std::shared_ptr<Services> s_;
};

} // namespace pgdemo::server

#include OATPP_CODEGEN_END(ApiController)
