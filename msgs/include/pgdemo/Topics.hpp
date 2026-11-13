#pragma once

namespace pgdemo::topics {

/// Worker to manager. Transient-local, so a manager that starts later still sees it.
inline constexpr const char* kHello = "pgdemo/worker/control/hello";
/// Worker to manager, while the device is paused.
inline constexpr const char* kHeartbeat = "pgdemo/worker/control/heartbeat";
/// Worker to manager. The worker cannot continue.
inline constexpr const char* kError = "pgdemo/worker/control/error";
/// Manager to worker. Ends the process.
inline constexpr const char* kBye = "pgdemo/manager/control/bye";

/// Worker to manager, during enrollment or rotation.
inline constexpr const char* kCsr = "pgdemo/worker/security/csr";
/// Manager to worker. The issued certificate.
inline constexpr const char* kCertificate = "pgdemo/manager/security/certificate";
/// Manager to worker. The CA refused the request.
inline constexpr const char* kRejected = "pgdemo/manager/security/rejected";
/// Manager to worker. Fresh nonce for the proof.
inline constexpr const char* kChallenge = "pgdemo/manager/security/challenge";
/// Manager to every worker. The manager-signed revocation list. No `device` field.
inline constexpr const char* kCrl = "pgdemo/manager/security/crl";
/// Worker to manager. Certificate and signature of the nonce.
inline constexpr const char* kProof = "pgdemo/worker/security/proof";

/// Manager to worker. A desired configuration, forwarded from `device_config_pending`.
/// `request_id` is 0 for the type defaults sent when the session goes online.
inline constexpr const char* kConfig = "pgdemo/manager/config";
/// Worker to manager. The worker accepted the configuration, or refused it and kept the previous one.
inline constexpr const char* kConfigApplied = "pgdemo/worker/config/applied";

/// Worker to manager. One scalar sample.
inline constexpr const char* kScalar = "pgdemo/worker/telemetry/scalar";
/// Worker to manager. One GPS fix.
inline constexpr const char* kGps = "pgdemo/worker/telemetry/gps";
/// Worker to manager. One sample of the 2D square.
inline constexpr const char* kXy = "pgdemo/worker/telemetry/xy";
/// Worker to manager. One sample of the 3D cube.
inline constexpr const char* kXyz = "pgdemo/worker/telemetry/xyz";

} // namespace pgdemo::topics
