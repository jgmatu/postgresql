#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

#include <db/Tables.hpp>

namespace pgdemo::db {

/// One registered device: `device`, its `device_type` and its current certificate.
struct Device
{
    int device_id{0};                ///< identity key, used by every other table
    std::string name;                ///< unique, human-facing name; the API addresses devices by it
    std::string type_id;             ///< `device_type.type_id`; selects the device's generator
    Payload payload{Payload::Scalar}; ///< telemetry table and channel of its samples
    int cert_id{0};                  ///< current `device_certificate`
    std::string serial;              ///< its serial, lowercase hex
    std::string fingerprint;         ///< its SHA-256 fingerprint, lowercase hex
    std::string cert_pem;            ///< the certificate itself
    /// `v_device_identity.identity`: ACTIVE, SUSPENDED, REVOKED, CA_INACTIVE or EXPIRED.
    std::string identity;

    /// Only ACTIVE devices get a process.
    [[nodiscard]] bool active() const noexcept { return identity == "ACTIVE"; }
};

/// One row of `device_enrollment`, as the controller and the server read it.
struct Enrollment
{
    std::int64_t request_id{0};       ///< from `request_id_seq`
    std::string kind;                 ///< `ENROLL` or `ROTATE`
    std::string name;                 ///< device name, and the CN of the certificate
    std::string type_id;              ///< device type
    Payload payload{Payload::Scalar}; ///< payload of that type
    std::optional<int> device_id;     ///< ROTATE: the device being re-keyed
    std::string status;               ///< PENDING, CSR_SUBMITTED, ISSUED, REJECTED or EXPIRED
    std::optional<std::string> csr_pem; ///< set once the device process submitted it
    std::string requested_by;         ///< admin who asked for it
};

/// What became of an enrollment once it was decided.
struct EnrollmentOutcome
{
    std::int64_t request_id{0};         ///< the request
    std::string status;                 ///< ISSUED, REJECTED or EXPIRED (or still open)
    std::optional<int> device_id;       ///< the device, once it exists
    std::optional<std::string> cert_pem; ///< ISSUED: the new certificate
    std::string error_code;             ///< REJECTED or EXPIRED: why
    std::string error_detail;           ///< human-readable reason
};

/// A certificate the server has just signed, as `issue_device_certificate()` records it.
struct IssuedCertificate
{
    std::string serial;                              ///< lowercase hex
    std::string fingerprint;                         ///< SHA-256, lowercase hex
    std::string subject_cn;                          ///< must equal the enrollment name
    std::chrono::system_clock::time_point not_before; ///< start of validity
    std::chrono::system_clock::time_point not_after;  ///< end of validity
    std::string pem;                                 ///< the certificate
};

/// Result of `issue_device_certificate()`.
struct IssueResult
{
    int device_id{0}; ///< device created (ENROLL) or re-keyed (ROTATE)
    int cert_id{0};   ///< the new current certificate
};

/// One revoked certificate, for building the X.509 CRL.
struct RevokedPem
{
    std::string cert_pem; ///< the revoked certificate
    std::string reason;   ///< RFC 5280 reason name
};

/// Outcome of revoking the current certificate of a device.
enum class RevokeResult {
    Revoked,        ///< a CRL row was added, or a hold was made permanent
    NoSuchDevice,   ///< no device of that name
    AlreadyRevoked, ///< already revoked permanently; nothing changed
};

/// Outcome of lifting a hold.
enum class UnrevokeResult {
    Lifted,       ///< the certificateHold row was deleted
    NoSuchDevice, ///< no device of that name
    NotOnHold,    ///< the current certificate is not on hold
};

/// Outcome of deleting one certificate of a device.
enum class DeleteCertResult {
    Deleted,           ///< a previous certificate was deleted; the device stays
    DeviceDeleted,     ///< it was the current one, so the device went with it
    NoSuchDevice,      ///< no device of that name
    NoSuchCertificate, ///< the device has no certificate with that serial
};

/// One scalar sample written by a controller producer thread.
struct DummySample
{
    int device_id{0};     ///< device that produced the sample
    double value{0.0};    ///< generated value
    std::int64_t seq{0};  ///< producer tick counter, increases by one per tick
};

/// One sample of the 2D square. Stored as a native `point`.
struct XySample
{
    int device_id{0};     ///< device that produced the sample
    double x{0.0};        ///< first coordinate, `point` subscript 0
    double y{0.0};        ///< second coordinate, `point` subscript 1
    std::int64_t seq{0};  ///< producer tick counter, increases by one per tick
};

/// One GPS fix. PostGIS projects the point from the origin.
struct TrackSample
{
    int device_id{0};          ///< device that produced the fix
    double origin_lon{0.0};    ///< origin longitude, degrees
    double origin_lat{0.0};    ///< origin latitude, degrees
    double radius_m{0.0};      ///< orbit radius in metres (`amplitude`)
    double bearing_rad{0.0};   ///< azimuth passed to `ST_Project`, radians clockwise from north
    std::int64_t seq{0};       ///< producer tick counter, increases by one per tick
};

/// Applied configuration of one device (row of `device_config`).
struct AppliedConfig
{
    int device_id{0};                        ///< device the producer writes for
    std::string device;                      ///< its name, for logs
    int rate_ms{0};                          ///< period between ticks
    double amplitude{0.0};                   ///< scale of the generated values
    bool is_active{true};                    ///< false pauses the producer
    std::optional<std::int64_t> request_id;  ///< request that produced this state; empty for defaults
};

/// Desired configuration (row of `device_config_pending`).
struct ConfigRequest
{
    std::int64_t request_id{0};  ///< from `request_id_seq`, unique across processes
    int device_id{0};            ///< device to reconfigure
    std::string device;          ///< its name, for logs and responses
    int rate_ms{0};              ///< requested period between ticks
    double amplitude{0.0};       ///< requested amplitude
    bool is_active{true};        ///< requested on/off state
    std::string requested_by;    ///< username of the admin who submitted it
};

/// Decision taken by the controller for one queued request.
struct ConfigOutcome
{
    bool ok{false};            ///< true: apply; false: record an error
    std::string error_code;    ///< machine-readable reason, e.g. `INVALID_RATE`; empty on success
    std::string error_detail;  ///< human-readable reason; empty on success

    /// Accept the request.
    static ConfigOutcome success() { return {true, {}, {}}; }

    /// Reject the request with `code` and `detail` (stored in `device_config_errors`).
    static ConfigOutcome failure(std::string code, std::string detail)
    {
        return {false, std::move(code), std::move(detail)};
    }
};

/// Result of a call to `drain_pending`.
struct DrainStats
{
    std::size_t applied{0};   ///< requests committed to `device_config`
    std::size_t rejected{0};  ///< requests refused by `decide` (status ERROR)
    std::size_t expired{0};   ///< requests older than `max_age` (status TIMEOUT)
};

/// Row returned by `auth_fetch_user()`.
struct UserLoginRow
{
    std::string password_hash;  ///< Argon2id PHC string
    std::string role;           ///< `admin` or `normal`
};

} // namespace pgdemo::db
