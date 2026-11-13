#pragma once

#include <chrono>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

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

/// Lower-case hex of `bytes`.
std::string to_hex(std::span<const std::uint8_t> bytes);

/**
 * @brief Inverse of `to_hex()`.
 * @throws std::invalid_argument on malformed input
 */
std::vector<std::uint8_t> from_hex(std::string_view hex);

/// UTC `YYYY-MM-DDTHH:MM:SSZ`, the form stored in `timestamptz` parameters.
std::string format_time(std::chrono::system_clock::time_point tp);

/// Subject CN of the manager's own root.
inline constexpr std::string_view kManagerCommonName = "pgdemo-manager";

/// Standard base64 (with padding) of `bytes`.
std::string base64_encode(std::span<const std::uint8_t> bytes);

/**
 * @brief Inverse of `base64_encode()`.
 * @throws std::invalid_argument on malformed input
 */
std::vector<std::uint8_t> base64_decode(std::string_view text);

/// `n` bytes from the system RNG.
std::vector<std::uint8_t> random_bytes(std::size_t n);

/// Domain separation string of the device authentication proof.
inline constexpr std::string_view kProofContext = "pgdemo-device-auth-v1";

/**
 * @brief Bytes a device signs to prove it holds the key of its certificate:
 * `kProofContext || 0 || nonce || 0 || name || 0 || fingerprint`.
 *
 * Binding the name and the certificate fingerprint stops a proof from being
 * replayed for another device or another certificate; the nonce stops it from
 * being replayed at all.
 */
std::vector<std::uint8_t> proof_message(std::span<const std::uint8_t> nonce, std::string_view name,
                                        std::string_view fingerprint);

} // namespace pgdemo::pki
