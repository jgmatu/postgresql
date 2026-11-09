#pragma once

#include <exception>
#include <memory>
#include <optional>
#include <string>

#include "Services.hpp"
#include "http/Common.hpp"

namespace pgdemo::server::http {

/**
 * @brief Queue an enrollment (`type` set) or a rotation (`type` empty) and
 * wait up to `--enroll-timeout-ms` for its certificate.
 *
 *   1. request_id = nextval('request_id_seq'); the Dispatcher registers a waiter
 *   2. INSERT device_enrollment (NOTIFY enrollment_requested)
 *   3. the controller starts the device process, which sends its CSR
 *      (status CSR_SUBMITTED, NOTIFY enrollment_csr)
 *   4. this server signs it: ISSUED, or REJECTED (NOTIFY enrollment_events)
 *   5. the listener wakes this thread
 *
 * Reply: 201 (enrollment) or 200 (rotation) once issued, 202 still in progress,
 * 422 rejected or expired, 400 unknown type, 404 unknown device, 409 the device
 * exists or a request for it is already open, 503 too many waiters.
 */
inline std::shared_ptr<Response> enroll_and_wait(Services& s, const Caller& c, const std::string& device,
                                                 const std::optional<std::string>& type)
{
    const auto id = s.db->generate_request_id(c.role);
    std::exception_ptr submit_error;
    bool queued = false;
    const auto outcome =
        s.dispatcher->wait_for_response(static_cast<std::uint64_t>(id), s.config.enroll_timeout_ms, [&] {
            try
            {
                queued = type ? s.db->enqueue_enrollment(c.role, id, device, *type, c.username)
                              : s.db->enqueue_rotation(c.role, id, device, c.username);
            }
            catch (...)
            {
                submit_error = std::current_exception();
                throw;
            }
            if (!queued) throw std::runtime_error("nothing queued");
        });

    switch (outcome)
    {
    case ResponseStatus::SYSTEM_FULL:
        return error_response(Status::CODE_503, "busy", "too many requests in flight");
    case ResponseStatus::DB_ERROR:
        if (submit_error) std::rethrow_exception(submit_error);
        if (!queued && type) throw BadRequest("unknown device type '" + *type + "' (see GET /api/device-types)");
        if (!queued) return no_such_device(device);
        throw std::runtime_error("dispatcher rejected request id " + std::to_string(id));
    case ResponseStatus::SUCCESS:
    case ResponseStatus::ERROR:
    case ResponseStatus::TIMEOUT:
    case ResponseStatus::PENDING:
        break;
    }
    log::info("devices", std::format("'{}' {} '{}' (request {}): {}", c.username,
                                     type ? "enrolled" : "rotated the key of", device, id,
                                     Dispatcher::status_to_string(outcome)));

    // The row is the source of truth: a TIMEOUT may hide a missed notification.
    const auto row = s.db->fetch_enrollment(c.role, id);
    if (!row) throw std::runtime_error("enrollment " + std::to_string(id) + " vanished");
    const std::string st(row->as_object().at("status").as_string());
    boost::json::object out{{"request_id", id},
                            {"status", st},
                            {"dispatcher", Dispatcher::status_to_string(outcome)},
                            {"enrollment", *row}};
    if (st == "ISSUED")
    {
        if (const auto dev = s.db->fetch_device(c.role, device)) out["device"] = *dev;
        auto r = json_response(type ? Status::CODE_201 : Status::CODE_200, out);
        r->putOrReplaceHeader("Location", oatpp::String("/api/devices/" + device));
        return r;
    }
    if (st == "PENDING" || st == "CSR_SUBMITTED")
    {
        out["message"] = "in progress; the controller starts the device process and the CA signs its CSR";
        auto r = json_response(Status::CODE_202, out);
        r->putOrReplaceHeader("Location", oatpp::String("/api/enrollments/" + std::to_string(id)));
        return r;
    }
    return json_response(Status::CODE_422, out);
}

} // namespace pgdemo::server::http
