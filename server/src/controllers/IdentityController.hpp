#pragma once

#include <algorithm>
#include <cctype>
#include <charconv>
#include <memory>
#include <set>
#include <string>

#include "Services.hpp"
#include "http/Common.hpp"
#include "http/Enrollment.hpp"
#include "oatpp/core/macro/codegen.hpp"
#include "oatpp/web/server/api/ApiController.hpp"

#include OATPP_CODEGEN_BEGIN(ApiController)

namespace pgdemo::server {

/**
 * @brief Device identity: certificates, revocation, key rotation, enrollments
 * and the CA. Reads run as demo_normal or demo_admin; changes need admin.
 *
 * GET    /api/devices/{name}/certificates          every certificate of the device
 * POST   /api/devices/{name}/revoke                admin; {"reason"}: revoke the current certificate
 * DELETE /api/devices/{name}/revoke                admin; lift a certificateHold
 * POST   /api/devices/{name}/rotate                admin; new key and certificate
 * DELETE /api/devices/{name}/certificates/{serial} admin; the current one takes the device with it
 * GET    /api/enrollments?device=&limit=           enrollment and rotation requests
 * GET    /api/enrollments/{id}                     one request
 * GET    /api/pki/ca                               trusted root CAs
 * GET    /api/pki/crl                              X.509 CRL signed by the CA (PEM)
 */
class IdentityController : public oatpp::web::server::api::ApiController
{
public:
    /**
     * @param services shared server state
     * @param mapper   Oat++ object mapper for the controller
     */
    IdentityController(std::shared_ptr<Services> services, const std::shared_ptr<ObjectMapper>& mapper)
        : oatpp::web::server::api::ApiController(mapper)
        , s_(std::move(services))
    {
    }

    /// `GET /api/devices/{name}/certificates` (any role): 200 newest first, 404 unknown device.
    ENDPOINT("GET", "/api/devices/{name}/certificates", certificates,
             REQUEST(std::shared_ptr<IncomingRequest>, request), PATH(String, name))
    {
        return http::guarded("certificates", [&] {
            const auto c = http::caller_of(request);
            const auto dev = http::device_param(*name);
            const auto rows = s_->db->fetch_certificates(c.role, dev);
            if (!rows) return http::no_such_device(dev);
            return http::json_response(Status::CODE_200, *rows);
        });
    }

    /// `POST /api/devices/{name}/revoke` (admin): 200 revoked, 400 bad reason, 404 unknown device,
    /// 409 already revoked permanently. The controller stops the device; the device row stays.
    ENDPOINT("POST", "/api/devices/{name}/revoke", revoke, REQUEST(std::shared_ptr<IncomingRequest>, request),
             PATH(String, name), BODY_STRING(String, body))
    {
        return http::guarded("revoke", [&] {
            const auto c = http::caller_of(request);
            if (!c.is_admin()) return http::forbidden();
            const auto dev = http::device_param(*name);
            const auto in = http::parse_object(body);
            const auto* reason_v = in ? in->if_contains("reason") : nullptr;
            const std::string reason = reason_v && reason_v->is_string() ? std::string(reason_v->as_string()) : "";
            if (!kReasons.contains(reason))
                throw http::BadRequest("reason must be one of unspecified, keyCompromise, affiliationChanged, "
                                       "cessationOfOperation, certificateHold");
            switch (s_->db->revoke_certificate(c.role, dev, reason, c.username))
            {
            case db::RevokeResult::NoSuchDevice: return http::no_such_device(dev);
            case db::RevokeResult::AlreadyRevoked:
                return http::error_response(Status::CODE_409, "revoked",
                                            "the current certificate of '" + dev + "' is already revoked");
            case db::RevokeResult::Revoked: break;
            }
            log::info("pki", "'" + c.username + "' revoked the certificate of '" + dev + "' (" + reason + ")");
            return device_reply(c, dev);
        });
    }

    /// `DELETE /api/devices/{name}/revoke` (admin): 200 hold lifted, 404 unknown device, 409 not on hold.
    ENDPOINT("DELETE", "/api/devices/{name}/revoke", unrevoke, REQUEST(std::shared_ptr<IncomingRequest>, request),
             PATH(String, name))
    {
        return http::guarded("unrevoke", [&] {
            const auto c = http::caller_of(request);
            if (!c.is_admin()) return http::forbidden();
            const auto dev = http::device_param(*name);
            switch (s_->db->unrevoke_certificate(c.role, dev))
            {
            case db::UnrevokeResult::NoSuchDevice: return http::no_such_device(dev);
            case db::UnrevokeResult::NotOnHold:
                return http::error_response(Status::CODE_409, "not_on_hold",
                                            "the current certificate of '" + dev +
                                                "' is not on hold; other revocations are permanent");
            case db::UnrevokeResult::Lifted: break;
            }
            log::info("pki", "'" + c.username + "' lifted the hold on '" + dev + "'");
            return device_reply(c, dev);
        });
    }

    /// `POST /api/devices/{name}/rotate` (admin): 200 issued, 202 in progress, 404 unknown device,
    /// 409 revoked or rotation already open, 422 refused.
    ENDPOINT("POST", "/api/devices/{name}/rotate", rotate, REQUEST(std::shared_ptr<IncomingRequest>, request),
             PATH(String, name))
    {
        return http::guarded("rotate", [&]() -> std::shared_ptr<OutgoingResponse> {
            const auto c = http::caller_of(request);
            if (!c.is_admin()) return http::forbidden();
            const auto dev = http::device_param(*name);
            const auto row = s_->db->fetch_device(c.role, dev);
            if (!row) return http::no_such_device(dev);
            // A new certificate would bring a revoked device back: lift the hold, or enroll anew.
            const auto* identity = row->as_object().if_contains("identity");
            const std::string id = identity && identity->is_string() ? std::string(identity->as_string()) : "";
            if (id == "REVOKED" || id == "SUSPENDED")
                return http::error_response(Status::CODE_409, "revoked",
                                            "device '" + dev + "' is " + id + "; rotation would restore it");
            return http::enroll_and_wait(*s_, c, dev, std::nullopt);
        });
    }

    /// `DELETE /api/devices/{name}/certificates/{serial}` (admin): 200 deleted (with `device_deleted`
    /// when it was the current certificate), 400 bad serial, 404 unknown device or serial.
    ENDPOINT("DELETE", "/api/devices/{name}/certificates/{serial}", delete_certificate,
             REQUEST(std::shared_ptr<IncomingRequest>, request), PATH(String, name), PATH(String, serial))
    {
        return http::guarded("delete certificate", [&] {
            const auto c = http::caller_of(request);
            if (!c.is_admin()) return http::forbidden();
            const auto dev = http::device_param(*name);
            std::string sn(*serial);
            std::ranges::transform(sn, sn.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            if (sn.empty() || sn.size() > 40 || !std::ranges::all_of(sn, [](unsigned char ch) { return std::isxdigit(ch); }))
                throw http::BadRequest("serial must be 1 to 40 hexadecimal digits");
            const auto result = s_->db->delete_certificate(c.role, dev, sn);
            switch (result)
            {
            case db::DeleteCertResult::NoSuchDevice: return http::no_such_device(dev);
            case db::DeleteCertResult::NoSuchCertificate:
                return http::error_response(Status::CODE_404, "not_found",
                                            "device '" + dev + "' has no certificate " + sn);
            case db::DeleteCertResult::Deleted:
            case db::DeleteCertResult::DeviceDeleted: break;
            }
            const bool gone = result == db::DeleteCertResult::DeviceDeleted;
            log::info("pki", "'" + c.username + "' deleted certificate " + sn + " of '" + dev + "'" +
                                 (gone ? ", and with it the device" : ""));
            return http::json_response(Status::CODE_200, boost::json::object{
                                                             {"device", dev}, {"serial", sn}, {"device_deleted", gone}});
        });
    }

    /// `GET /api/enrollments?device=&limit=` (any role): 200 newest first.
    ENDPOINT("GET", "/api/enrollments", enrollments, REQUEST(std::shared_ptr<IncomingRequest>, request))
    {
        return http::guarded("enrollments", [&] {
            const auto c = http::caller_of(request);
            std::optional<std::string> device;
            if (const auto d = http::str_param(request, "device")) device = http::device_param(*d);
            const int limit = http::int_param(request, "limit", 1, 1000).value_or(50);
            return http::json_response(Status::CODE_200, s_->db->fetch_enrollments(c.role, device, limit));
        });
    }

    /// `GET /api/enrollments/{id}` (any role): 200, 400 bad id, 404 unknown.
    ENDPOINT("GET", "/api/enrollments/{id}", enrollment, REQUEST(std::shared_ptr<IncomingRequest>, request),
             PATH(String, id))
    {
        return http::guarded("enrollment", [&] {
            const auto c = http::caller_of(request);
            std::int64_t rid = 0;
            const std::string_view sv(*id);
            const auto [end, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), rid);
            if (ec != std::errc{} || end != sv.data() + sv.size() || rid < 1)
                throw http::BadRequest("request id must be a positive integer");
            const auto row = s_->db->fetch_enrollment(c.role, rid);
            if (!row) return http::error_response(Status::CODE_404, "not_found", "no such enrollment");
            return http::json_response(Status::CODE_200, *row);
        });
    }

    /// `GET /api/pki/ca` (any role): 200 with every root CA, and which one this server signs with.
    ENDPOINT("GET", "/api/pki/ca", ca, REQUEST(std::shared_ptr<IncomingRequest>, request))
    {
        return http::guarded("pki ca", [&] {
            const auto c = http::caller_of(request);
            return http::json_response(Status::CODE_200,
                                       boost::json::object{{"signing_fingerprint", s_->signer->ca().fingerprint()},
                                                           {"roots", s_->db->fetch_root_cas(c.role)},
                                                           {"revoked", s_->db->fetch_crl_entries(c.role)}});
        });
    }

    /// `GET /api/pki/origin` (any role): the factory root, the manager certificate and
    /// the worker certificates issued by that root. No private key is returned.
    ENDPOINT("GET", "/api/pki/origin", origin, REQUEST(std::shared_ptr<IncomingRequest>, request))
    {
        return http::guarded("pki origin", [&] {
            const auto c = http::caller_of(request);
            return http::json_response(Status::CODE_200, s_->db->fetch_origin(c.role));
        });
    }

    /// `GET /api/pki/crl` (any role): 200 `application/x-pem-file`, a CRL signed now by the CA.
    ENDPOINT("GET", "/api/pki/crl", crl, REQUEST(std::shared_ptr<IncomingRequest>, request))
    {
        return http::guarded("pki crl", [&] {
            const auto c = http::caller_of(request);
            std::vector<pki::RevokedCert> revoked;
            for (auto& r : s_->db->load_crl(c.role)) revoked.push_back({std::move(r.cert_pem), std::move(r.reason)});
            auto r = http::ResponseFactory::createResponse(Status::CODE_200,
                                                           oatpp::String(s_->signer->ca().crl_pem(revoked)));
            r->putOrReplaceHeader("Content-Type", "application/x-pem-file");
            return r;
        });
    }

private:
    std::shared_ptr<OutgoingResponse> device_reply(const http::Caller& c, const std::string& dev)
    {
        const auto row = s_->db->fetch_device(c.role, dev);
        if (!row) return http::no_such_device(dev);
        return http::json_response(Status::CODE_200, *row);
    }

    static inline const std::set<std::string> kReasons{"unspecified", "keyCompromise", "affiliationChanged",
                                                       "cessationOfOperation", "certificateHold"};

    std::shared_ptr<Services> s_;
};

} // namespace pgdemo::server

#include OATPP_CODEGEN_END(ApiController)
