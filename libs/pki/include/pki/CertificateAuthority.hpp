#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <pki/Certificate.hpp>

namespace Botan {
class Private_Key;
class X509_Certificate;
} // namespace Botan

namespace pgdemo::pki {

/// One revoked certificate, as stored in the `crl` table.
struct RevokedCert
{
    std::string cert_pem; ///< the revoked certificate
    std::string reason;   ///< RFC 5280 reason name, e.g. `keyCompromise`
};

/**
 * @brief The device root CA, used by the server: signs device CSRs and the CRL.
 *
 * Every certificate it issues has the same profile, whatever the CSR asks for:
 * subject `CN=<device>` only, `basicConstraints CA:FALSE`, `keyUsage
 * digitalSignature` (critical), `extendedKeyUsage clientAuth`, subject and
 * authority key identifiers, a random 128-bit serial and ECDSA-SHA384.
 *
 * Thread-safe: each call builds its own signer and RNG.
 */
class CertificateAuthority
{
public:
    /**
     * @param key_path  PKCS#8 ECDSA P-384 private key of the CA
     * @param cert_path its self-signed CA certificate
     * @throws std::runtime_error if the files cannot be read, the key is not
     *         P-384, or the certificate is not a CA certificate
     */
    CertificateAuthority(const std::string& key_path, const std::string& cert_path);
    ~CertificateAuthority();

    CertificateAuthority(const CertificateAuthority&) = delete;
    CertificateAuthority& operator=(const CertificateAuthority&) = delete;

    /// SHA-256 fingerprint of the CA certificate (`root_ca.fingerprint_sha256`).
    [[nodiscard]] const std::string& fingerprint() const noexcept { return fingerprint_; }
    /// The CA certificate, PEM.
    [[nodiscard]] const std::string& certificate_pem() const noexcept { return pem_; }

    /**
     * @brief Check a device CSR and sign a certificate for it.
     *
     * The CSR must carry an ECDSA P-384 key, a valid self-signature (proof that
     * the requester holds the key), `CN=expected_cn`, and must not ask to be a CA.
     *
     * @param csr_pem     PKCS#10 request, PEM
     * @param expected_cn device name the certificate is for
     * @param validity    lifetime; `not_before` is one minute in the past
     * @return the new certificate
     * @throws PkiError `CSR_MALFORMED`, `CSR_KEY`, `CSR_SIGNATURE`, `CSR_SUBJECT`, `CSR_CA`
     */
    CertInfo sign_csr(std::string_view csr_pem, std::string_view expected_cn,
                      std::chrono::seconds validity = std::chrono::days(365)) const;

    /**
     * @brief Who the presented self-signed certificate is for.
     *
     * `Worker` is issued `clientAuth`. `Manager` is issued `serverAuth`.
     * Both are end-entity certificates (`CA:FALSE`, `digitalSignature`).
     */
    enum class PresentedUse { Worker, Manager };

    /**
     * @brief Check a self-signed certificate and issue one for the same public key.
     *
     * The presented certificate must be self-signed by an ECDSA P-384 key
     * (proof of possession), carry `CN=expected_cn`, and must not be a CA.
     * The issued certificate replaces it. Its key stays P-384 and the signature
     * algorithm is ECDSA-SHA256, which is what DDS-Security loads. `sign_csr`
     * stays ECDSA-SHA384.
     *
     * @param cert_pem    self-signed certificate, PEM
     * @param expected_cn name the certificate is for
     * @param use         worker (`clientAuth`) or manager (`serverAuth`)
     * @param validity    lifetime; `not_before` is one minute in the past
     * @return the certificate signed by this CA
     * @throws PkiError `CERT_MALFORMED`, `CERT_CHAIN`, `CERT_SUBJECT`
     */
    CertInfo sign_presented(std::string_view cert_pem, std::string_view expected_cn, PresentedUse use,
                            std::chrono::seconds validity = std::chrono::days(365)) const;

    /**
     * @brief PKCS#7 detached SMIME of `document`, the form SROS2 stores in `*.p7s`.
     *
     * The signature uses this CA key. The caller checks that `document` is the
     * governance or permissions template before asking.
     */
    [[nodiscard]] std::string sign_document(std::string_view document) const;

    /**
     * @brief A signed X.509 CRL listing `revoked`.
     * @param revoked     revoked certificates of this CA
     * @param next_update `nextUpdate` offset from now
     * @return the CRL, PEM
     */
    std::string crl_pem(const std::vector<RevokedCert>& revoked,
                        std::chrono::seconds next_update = std::chrono::days(1)) const;

private:
    std::unique_ptr<Botan::Private_Key> key_;
    std::unique_ptr<Botan::X509_Certificate> cert_;
    std::string fingerprint_;
    std::string pem_;
};

} // namespace pgdemo::pki
