#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <boost/json.hpp>

#include "oatpp/web/protocol/http/outgoing/ResponseFactory.hpp"

namespace pgdemo::server::http {

/// HTTP status code and reason phrase.
using Status = oatpp::web::protocol::http::Status;
/// Outgoing Oat++ response.
using Response = oatpp::web::protocol::http::outgoing::Response;
/// Oat++ factory for outgoing responses.
using ResponseFactory = oatpp::web::protocol::http::outgoing::ResponseFactory;

/// `body` serialized, with `Content-Type: application/json`.
inline std::shared_ptr<Response> json_response(const Status& status, const boost::json::value& body)
{
    auto r = ResponseFactory::createResponse(status, oatpp::String(boost::json::serialize(body)));
    r->putOrReplaceHeader("Content-Type", "application/json");
    return r;
}

/// `{"error": "<code>", "message": "<text>"}`
inline std::shared_ptr<Response> error_response(const Status& status, std::string_view code,
                                                std::string_view message)
{
    return json_response(status, boost::json::object{{"error", code}, {"message", message}});
}

/// Body parsed as a JSON object, or `std::nullopt`.
inline std::optional<boost::json::object> parse_object(const oatpp::String& body)
{
    if (!body) return std::nullopt;
    boost::system::error_code ec;
    auto v = boost::json::parse(std::string_view(*body), ec);
    if (ec || !v.is_object()) return std::nullopt;
    return std::move(v.as_object());
}

} // namespace pgdemo::server::http
