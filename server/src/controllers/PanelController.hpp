#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "http/Json.hpp"
#include "http/Paths.hpp"
#include "http/StaticSite.hpp"
#include "oatpp/core/macro/codegen.hpp"
#include "oatpp/web/server/api/ApiController.hpp"

#include OATPP_CODEGEN_BEGIN(ApiController)

namespace pgdemo::server {

/**
 * @brief The control panel: every GET outside /api, from the webroot in memory.
 *
 * GET *    files of the static export; `/x/` is `/x/index.html`
 *
 * Registered after the API controllers, so it only gets the paths none of
 * them matched. Panel files are public (they hold no data; the panel reads
 * everything through the API with the user's token), so `AuthInterceptor`
 * lets them through; an unknown /api path that does get here, with a valid
 * token, is answered with the API's JSON 404 rather than the panel's page.
 */
class PanelController : public oatpp::web::server::api::ApiController
{
public:
    /// Content-Security-Policy of panel pages. Next's static export inlines its bootstrap
    /// scripts and Catalyst sets inline styles, hence `'unsafe-inline'` for both.
    static constexpr const char* kCsp = "default-src 'self'; script-src 'self' 'unsafe-inline'; "
                                        "style-src 'self' 'unsafe-inline'; img-src 'self' data:; "
                                        "connect-src 'self'; frame-ancestors 'none'; base-uri 'self'; "
                                        "form-action 'self'";

    /**
     * @param site   the loaded webroot
     * @param mapper Oat++ object mapper for the controller
     */
    PanelController(std::shared_ptr<const http::StaticSite> site, const std::shared_ptr<ObjectMapper>& mapper)
        : oatpp::web::server::api::ApiController(mapper)
        , site_(std::move(site))
    {
    }

    /// `GET *` (public): 200 with the file, 404 with the panel's 404 page, or JSON 404 under /api.
    ENDPOINT("GET", "*", serve, REQUEST(std::shared_ptr<IncomingRequest>, request))
    {
        std::string path = request->getStartingLine().path.std_str();
        if (const auto q = path.find_first_of("?#"); q != std::string::npos) path.resize(q);
        if (http::is_api_path(path)) return http::error_response(Status::CODE_404, "not_found", "no such endpoint");

        if (const auto* f = site_->lookup(path)) return file_response(Status::CODE_200, *f);
        if (const auto* page = site_->not_found_page()) return file_response(Status::CODE_404, *page);
        return http::error_response(Status::CODE_404, "not_found", "no such page");
    }

private:
    static std::shared_ptr<OutgoingResponse> file_response(const Status& status, const http::StaticFile& f)
    {
        auto r = http::ResponseFactory::createResponse(status, oatpp::String(f.body));
        r->putOrReplaceHeader("Content-Type", oatpp::String(std::string(f.mime)));
        // Hashed bundle names change with their content; pages must be revalidated
        // so a new build is picked up.
        r->putOrReplaceHeader("Cache-Control", f.immutable ? "public, max-age=31536000, immutable" : "no-cache");
        r->putOrReplaceHeader("X-Content-Type-Options", "nosniff");
        r->putOrReplaceHeader("X-Frame-Options", "DENY");
        r->putOrReplaceHeader("Referrer-Policy", "no-referrer");
        r->putOrReplaceHeader("Content-Security-Policy", kCsp);
        return r;
    }

    std::shared_ptr<const http::StaticSite> site_;
};

} // namespace pgdemo::server

#include OATPP_CODEGEN_END(ApiController)
