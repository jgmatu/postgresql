#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Botan {
class Private_Key;
class Public_Key;
} // namespace Botan

namespace pgdemo::auth {

/// Claims extracted from a verified token.
struct JwtClaims
{
    std::string sub;  ///< username
    std::string role; ///< "admin" | "normal"
    std::uint64_t iat{0}; ///< issued at, Unix seconds
    std::uint64_t exp{0}; ///< expiry, Unix seconds
};

/**
 * @brief ES384 (ECDSA P-384 + SHA-384) JSON Web Tokens, RFC 7515 / RFC 7519.
 *
 * Signs with a PKCS#8 private key and verifies with the public key of an X.509
 * certificate. Verification rejects: malformed tokens, any `alg` other than ES384
 * (including `none`), a bad signature, a wrong issuer, an expired token, and a
 * missing `sub` or `role`.
 */
class JwtManager
{
public:
    /// `iss` claim written by `generate()` and required by `verify()`.
    static constexpr const char* kIssuer = "pgdemo";

    /**
     * @brief Load the signing key and the verification certificate.
     * @param private_key_path PKCS#8 ECDSA P-384 private key
     * @param certificate_path X.509 certificate of that key
     * @throws std::runtime_error if a file cannot be read or holds no usable key.
     */
    JwtManager(const std::string& private_key_path, const std::string& certificate_path);
    ~JwtManager();

    JwtManager(const JwtManager&) = delete;
    JwtManager& operator=(const JwtManager&) = delete;

    /**
     * @brief Sign a token for `subject`.
     * @param subject     `sub` claim (username)
     * @param role        `role` claim, `admin` or `normal`
     * @param ttl_seconds lifetime; `exp = iat + ttl_seconds`
     * @return compact JWS `header.payload.signature`
     */
    std::string generate(const std::string& subject, const std::string& role,
                         std::uint64_t ttl_seconds = 900) const;

    /**
     * @brief Check signature, algorithm, issuer and expiry.
     * @param token compact JWS, without the `Bearer ` prefix
     * @return the claims, or `std::nullopt` for any invalid token
     */
    std::optional<JwtClaims> verify(const std::string& token) const;

    /// Base64url without padding (RFC 7515 section 2). Public for tests.
    static std::string base64url_encode(const std::vector<std::uint8_t>& data);
    /// Base64url without padding, of the bytes of `data`. Public for tests.
    static std::string base64url_encode(const std::string& data);
    /**
     * @brief Decode unpadded base64url. Public for tests.
     * @throws std::invalid_argument if `input` contains `+`, `/` or `=`
     */
    static std::vector<std::uint8_t> base64url_decode(const std::string& input);

private:
    std::unique_ptr<Botan::Private_Key> private_key_;
    std::unique_ptr<Botan::Public_Key> public_key_;
};

} // namespace pgdemo::auth
