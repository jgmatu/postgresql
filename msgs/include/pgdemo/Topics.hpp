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
/// Worker to manager. Certificate and signature of the nonce.
inline constexpr const char* kProof = "pgdemo/worker/security/proof";

/// Manager to worker, after the proof and whenever configuration changes.
inline constexpr const char* kConfig = "pgdemo/manager/config";

/// Worker to manager. One scalar sample.
inline constexpr const char* kScalar = "pgdemo/worker/telemetry/scalar";
/// Worker to manager. One GPS fix.
inline constexpr const char* kGps = "pgdemo/worker/telemetry/gps";
/// Worker to manager. One sample of the 2D square.
inline constexpr const char* kXy = "pgdemo/worker/telemetry/xy";

} // namespace pgdemo::topics
