#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <pki/Certificate.hpp>

namespace Botan {
class Certificate_Store_In_Memory;
} // namespace Botan

namespace pgdemo::pki {

/**
 * @brief Verification of device certificates and proofs, used by the controller.
 *
 * The trust anchors are the `root_ca` certificates read from the database.
 * Revocation comes from the `crl` table, passed in as a set of serials.
 */
class CertVerifier
{
public:
    /**
     * @param root_pems trusted CA certificates, PEM
     * @throws PkiError `CERT_MALFORMED` if one does not parse
     */
    explicit CertVerifier(const std::vector<std::string>& root_pems);
    ~CertVerifier();

    CertVerifier(const CertVerifier&) = delete;
    CertVerifier& operator=(const CertVerifier&) = delete;

    /**
     * @brief Check that `cert_pem` is a valid client certificate for `expected_cn`.
     *
     * Subject, revocation, then PKIX path validation at `now`: chain to one of
     * the roots, signatures, validity periods, CA constraints, and the
     * `clientAuth` usage.
     *
     * @param cert_pem        certificate presented by the device
     * @param expected_cn     device name
     * @param revoked_serials serials (lower-case hex) revoked by the issuing CA
     * @param now             validation time
     * @return the certificate's fields
     * @throws PkiError `CERT_MALFORMED`, `CERT_SUBJECT`, `CERT_REVOKED`, `CERT_CHAIN`
     */
    CertInfo verify(std::string_view cert_pem, std::string_view expected_cn,
                    const std::set<std::string>& revoked_serials,
                    std::chrono::system_clock::time_point now = std::chrono::system_clock::now()) const;

    /**
     * @brief Check an ECDSA-SHA384 signature (raw `r || s`) with the key of `cert_pem`.
     * @return false for a bad signature or an unusable certificate
     */
    static bool verify_signature(std::string_view cert_pem, std::span<const std::uint8_t> message,
                                 std::span<const std::uint8_t> signature) noexcept;

private:
    std::unique_ptr<Botan::Certificate_Store_In_Memory> store_;
};

} // namespace pgdemo::pki
