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
 * @brief Devices and their telemetry. Reads run as demo_normal or demo_admin;
 * registering and removing devices needs admin.
 *
 * GET    /api/health                         public liveness
 * GET    /api/db/info                        PostgreSQL / libpqxx / listener details
 * GET    /api/device-types                   payload and default configuration of each type
 * GET    /api/devices                        every device with its last sample (v_devices)
 * GET    /api/devices/{name}                 one device, its last sample and its config sync state
 * GET    /api/devices/{name}/history?from=&to=&limit=
 * POST   /api/devices                        admin; {"name", "type"}
 * DELETE /api/devices/{name}                 admin; telemetry is kept
 * GET    /api/stream?channels=&devices=      Server-Sent Events
 *
 * A device is a row, so registering one changes neither the schema nor the
 * routes: the controller sees `device_events` and starts writing for it.
 */
class DeviceController : public oatpp::web::server::api::ApiController
{
public:
    /**
     * @param services shared server state
     * @param mapper   Oat++ object mapper for the controller
     */
    DeviceController(std::shared_ptr<Services> services, const std::shared_ptr<ObjectMapper>& mapper)
        : oatpp::web::server::api::ApiController(mapper)
        , s_(std::move(services))
    {
    }

    /// `GET /api/health` (public): 200 while the listener is alive, 503 otherwise.
    ENDPOINT("GET", "/api/health", health)
    {
        const bool alive = s_->db->listener_alive();
        return http::json_response(alive ? Status::CODE_200 : Status::CODE_503,
                                   boost::json::object{{"status", alive ? "ok" : "degraded"},
                                                       {"listener_alive", alive}});
    }

    /// `GET /api/db/info` (any role): 200 with database, pool, listener, SSE and Dispatcher state.
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

    /// `GET /api/device-types` (any role): 200 with `device_type`.
    ENDPOINT("GET", "/api/device-types", types, REQUEST(std::shared_ptr<IncomingRequest>, request))
    {
        return http::guarded("device types", [&] {
            const auto c = http::caller_of(request);
            return http::json_response(Status::CODE_200, s_->db->fetch_device_types(c.role));
        });
    }

    /// `GET /api/devices` (any role): 200 with every device and its last sample.
    ENDPOINT("GET", "/api/devices", list, REQUEST(std::shared_ptr<IncomingRequest>, request))
    {
        return http::guarded("devices", [&] {
            const auto c = http::caller_of(request);
            return http::json_response(Status::CODE_200, s_->db->fetch_devices(c.role));
        });
    }

    /// `GET /api/devices/{name}` (any role): 200 with the device, 400 bad name, 404 unknown.
    ENDPOINT("GET", "/api/devices/{name}", one, REQUEST(std::shared_ptr<IncomingRequest>, request),
             PATH(String, name))
    {
        return http::guarded("device", [&] {
            const auto c = http::caller_of(request);
            const auto dev = http::device_param(*name);
            const auto row = s_->db->fetch_device(c.role, dev);
            if (!row) return http::no_such_device(dev);
            return http::json_response(Status::CODE_200, *row);
        });
    }

    /// `GET /api/devices/{name}/history?from=&to=&limit=` (any role): 200 with telemetry rows,
    /// 400 bad parameter, 404 unknown device.
    ENDPOINT("GET", "/api/devices/{name}/history", history, REQUEST(std::shared_ptr<IncomingRequest>, request),
             PATH(String, name))
    {
        return http::guarded("device history", [&] {
            const auto c = http::caller_of(request);
            const auto dev = http::device_param(*name);
            const int limit = http::int_param(request, "limit", 1, 5000).value_or(100);
            const auto rows = s_->db->fetch_telemetry(c.role, dev, http::str_param(request, "from"),
                                                      http::str_param(request, "to"), limit);
            if (!rows) return http::no_such_device(dev);
            return http::json_response(Status::CODE_200, *rows);
        });
    }

    /// `POST /api/devices` (admin): 201 created, 400 bad body or unknown type, 403 not admin,
    /// 409 name taken.
    ENDPOINT("POST", "/api/devices", create, REQUEST(std::shared_ptr<IncomingRequest>, request),
             BODY_STRING(String, body))
    {
        return http::guarded("create device", [&] {
            const auto c = http::caller_of(request);
            if (!c.is_admin()) return http::forbidden();

            const auto in = http::parse_object(body);
            const auto* name_v = in ? in->if_contains("name") : nullptr;
            const auto* type_v = in ? in->if_contains("type") : nullptr;
            if (!name_v || !name_v->is_string() || !type_v || !type_v->is_string())
            {
                throw http::BadRequest(R"(body must be {"name": "...", "type": "sine|sawtooth|gps_orbit|..."})");
            }
            const auto dev = http::device_param(name_v->as_string());
            const std::string type(type_v->as_string());

            const auto row = s_->db->create_device(c.role, dev, type);
            if (!row) throw http::BadRequest("unknown device type '" + type + "' (see GET /api/device-types)");
            log::info("devices", "'" + c.username + "' registered device '" + dev + "' (" + type + ")");
            auto r = http::json_response(Status::CODE_201, *row);
            r->putOrReplaceHeader("Location", oatpp::String("/api/devices/" + dev));
            return r;
        });
    }

    /// `DELETE /api/devices/{name}` (admin): 200 removed, 400 bad name, 403 not admin, 404 unknown.
    ENDPOINT("DELETE", "/api/devices/{name}", remove, REQUEST(std::shared_ptr<IncomingRequest>, request),
             PATH(String, name))
    {
        return http::guarded("delete device", [&] {
            const auto c = http::caller_of(request);
            if (!c.is_admin()) return http::forbidden();
            const auto dev = http::device_param(*name);
            if (!s_->db->delete_device(c.role, dev)) return http::no_such_device(dev);
            log::info("devices", "'" + c.username + "' removed device '" + dev + "'");
            return http::json_response(Status::CODE_200, boost::json::object{{"deleted", dev}});
        });
    }

    /// `GET /api/stream?channels=&devices=` (any role): 200 `text/event-stream`,
    /// 400 unknown channel or malformed device name.
    ///
    /// Device names are not checked for existence, so a client may subscribe to a
    /// device before it is registered.
    ENDPOINT("GET", "/api/stream", stream, REQUEST(std::shared_ptr<IncomingRequest>, request))
    {
        return http::guarded("stream", [&]() -> std::shared_ptr<OutgoingResponse> {
            const auto c = http::caller_of(request);
            std::set<std::string> channels;
            if (const auto raw = http::str_param(request, "channels"))
            {
                http::for_each_listed(*raw, [&](std::string_view name) {
                    if (!EventHub::all_channels().contains(std::string(name)))
                        throw http::BadRequest("unknown channel '" + std::string(name) + "'");
                    channels.emplace(name);
                });
            }
            if (channels.empty()) channels = EventHub::all_channels();
            std::set<std::string> devices;
            if (const auto raw = http::str_param(request, "devices"))
            {
                http::for_each_listed(*raw, [&](std::string_view name) { devices.insert(http::device_param(name)); });
            }

            auto sub = s_->hub->subscribe(channels, devices);
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
