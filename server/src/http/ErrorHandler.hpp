#pragma once

#include <format>

#include <log/Log.hpp>

#include "Json.hpp"
#include "oatpp/web/server/handler/ErrorHandler.hpp"

namespace pgdemo::server::http {

/// Every framework-level error (404, malformed request, uncaught exception) as JSON.
/// Exception text is logged, never returned to the client.
class JsonErrorHandler : public oatpp::web::server::handler::ErrorHandler
{
public:
    /// Uncaught exception from a handler: log it, reply `500 internal`.
    std::shared_ptr<Response> handleError(const std::exception_ptr& error) override
    {
        try
        {
            if (error) std::rethrow_exception(error);
        }
        catch (const std::exception& e)
        {
            log::error("http", std::string("unhandled exception: ") + e.what());
        }
        catch (...)
        {
            log::error("http", "unhandled non-standard exception");
        }
        return error_response(Status::CODE_500, "internal", "internal server error");
    }

    /// Framework error such as 404: `{"error": "http_<code>", "message"}`, keeping its headers.
    std::shared_ptr<Response> handleError(const Status& status, const oatpp::String& message,
                                          const Headers& headers) override
    {
        const std::string text = message ? std::string(*message) : std::string(status.description);
        auto r = error_response(status, std::format("http_{}", status.code), text);
        for (const auto& [k, v] : headers.getAll())
        {
            r->putOrReplaceHeader(k.toString(), v.toString());
        }
        return r;
    }
};

} // namespace pgdemo::server::http
