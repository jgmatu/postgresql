#pragma once

#include <chrono>
#include <stdexcept>
#include <string>
#include <string_view>

namespace pgdemo::pki {

/**
 * @brief A refused CSR, certificate or proof, with a machine-readable code.
 *
 * Codes: `CSR_MALFORMED`, `CSR_KEY`, `CSR_SIGNATURE`, `CSR_SUBJECT`, `CSR_CA`,
 * `CERT_MALFORMED`, `CERT_SUBJECT`, `CERT_REVOKED`, `CERT_CHAIN`.
 */
struct PkiError : std::runtime_error
{
    /// @param c machine-readable code, stored in `code`
    /// @param detail human-readable reason, returned by `what()`
    PkiError(std::string c, const std::string& detail) : std::runtime_error(detail), code(std::move(c)) {}

    std::string code; ///< machine-readable reason
};

/// Fields of an X.509 certificate that the database stores beside its PEM.
struct CertInfo
{
    std::string serial;      ///< serial number, lower-case hex
    std::string fingerprint; ///< SHA-256 of the DER encoding, lower-case hex without colons
    std::string subject_cn;  ///< subject common name; the device name
    std::chrono::system_clock::time_point not_before; ///< start of validity
    std::chrono::system_clock::time_point not_after;  ///< end of validity
    std::string pem;         ///< the certificate, PEM
};

/**
 * @brief Parse a PEM certificate.
 * @throws PkiError `CERT_MALFORMED`
 */
CertInfo inspect_certificate(std::string_view pem);

/**
 * @brief Whether `pem` is signed with ECDSA-SHA256.
 *
 * DDS-Security's built-in authentication accepts that algorithm and rejects
 * ECDSA-SHA384. The key curve is a separate question. A certificate that does
 * not parse returns false.
 */
bool signed_with_sha256(std::string_view pem);

/**
 * @brief Whether `pem` is a self-signed ECDSA-SHA256 CA whose CN is `expected_cn`.
 *
 * The manager root is this certificate. A certificate that does not parse,
 * is not a CA, or is signed by someone else returns false.
 */
bool self_signed_root(std::string_view pem, std::string_view expected_cn);

/// Subject CN of the manager's own root.
inline constexpr std::string_view kManagerCommonName = "pgdemo-manager";

} // namespace pgdemo::pki
