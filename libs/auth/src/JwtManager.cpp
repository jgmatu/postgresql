#include <auth/JwtManager.hpp>

#include <botan/auto_rng.h>
#include <botan/base64.h>
#include <botan/data_src.h>
#include <botan/pk_keys.h>
#include <botan/pkcs8.h>
#include <botan/pubkey.h>
#include <botan/x509cert.h>

#include <boost/json.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <stdexcept>

namespace pgdemo::auth {

namespace {

constexpr auto kAlg = "ES384";
constexpr auto kBotanHash = "SHA-384";
constexpr auto kHeader = R"({"alg":"ES384","typ":"JWT"})";

std::uint64_t now_epoch()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
}

std::uint64_t as_u64(const boost::json::value& v)
{
    if (v.is_uint64()) return v.as_uint64();
    if (v.is_int64() && v.as_int64() >= 0) return static_cast<std::uint64_t>(v.as_int64());
    throw std::invalid_argument("not a non-negative integer");
}

} // namespace

// Base64URL without padding (RFC 7515 section 2).
std::string JwtManager::base64url_encode(const std::vector<std::uint8_t>& data)
{
    std::string b64 = Botan::base64_encode(data);
    std::replace(b64.begin(), b64.end(), '+', '-');
    std::replace(b64.begin(), b64.end(), '/', '_');
    b64.erase(std::remove(b64.begin(), b64.end(), '='), b64.end());
    return b64;
}

std::string JwtManager::base64url_encode(const std::string& data)
{
    return base64url_encode(std::vector<std::uint8_t>(data.begin(), data.end()));
}

std::vector<std::uint8_t> JwtManager::base64url_decode(const std::string& input)
{
    if (input.find_first_of("+/=") != std::string::npos)
        throw std::invalid_argument("not base64url");
    std::string b64 = input;
    std::replace(b64.begin(), b64.end(), '-', '+');
    std::replace(b64.begin(), b64.end(), '_', '/');
    while (b64.size() % 4 != 0) b64.push_back('=');
    return Botan::unlock(Botan::base64_decode(b64));
}

JwtManager::JwtManager(const std::string& private_key_path, const std::string& certificate_path)
{
    std::ifstream key_stream(private_key_path);
    if (!key_stream) throw std::runtime_error("JwtManager: cannot open " + private_key_path);
    Botan::DataSource_Stream key_src(key_stream);
    private_key_ = Botan::PKCS8::load_key(key_src);
    if (!private_key_ || private_key_->algo_name() != "ECDSA")
        throw std::runtime_error("JwtManager: " + private_key_path + " is not an ECDSA PKCS#8 key");

    Botan::X509_Certificate cert(certificate_path);
    public_key_ = cert.subject_public_key();
    if (!public_key_)
        throw std::runtime_error("JwtManager: no public key in " + certificate_path);
    if (public_key_->key_length() != 384)
        throw std::runtime_error("JwtManager: ES384 needs a P-384 key");
}

JwtManager::~JwtManager() = default;

std::string JwtManager::generate(const std::string& subject, const std::string& role,
                                 std::uint64_t ttl_seconds) const
{
    const auto now = now_epoch();

    boost::json::object payload;
    payload["iss"] = kIssuer;
    payload["sub"] = subject;
    payload["role"] = role;
    payload["iat"] = now;
    payload["exp"] = now + ttl_seconds;

    const std::string signing_input =
        base64url_encode(std::string(kHeader)) + "." + base64url_encode(boost::json::serialize(payload));

    Botan::AutoSeeded_RNG rng;
    Botan::PK_Signer signer(*private_key_, rng, kBotanHash, Botan::Signature_Format::Standard);
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(signing_input.data());
    const auto signature = signer.sign_message(bytes, signing_input.size(), rng);

    return signing_input + "." + base64url_encode(signature);
}

std::optional<JwtClaims> JwtManager::verify(const std::string& token) const
{
    try
    {
        const auto dot1 = token.find('.');
        if (dot1 == std::string::npos) return std::nullopt;
        const auto dot2 = token.find('.', dot1 + 1);
        if (dot2 == std::string::npos || token.find('.', dot2 + 1) != std::string::npos)
            return std::nullopt;

        const std::string header_b64 = token.substr(0, dot1);
        const std::string payload_b64 = token.substr(dot1 + 1, dot2 - dot1 - 1);
        const std::string sig_b64 = token.substr(dot2 + 1);

        // 1. Algorithm pinned to ES384: no "none", no algorithm confusion.
        const auto header_raw = base64url_decode(header_b64);
        const auto header = boost::json::parse(std::string(header_raw.begin(), header_raw.end()));
        if (header.as_object().at("alg").as_string() != kAlg) return std::nullopt;

        // 2. Signature (raw r||s, 96 bytes for P-384).
        const std::string signing_input = header_b64 + "." + payload_b64;
        const auto sig = base64url_decode(sig_b64);
        if (sig.size() != 96) return std::nullopt;
        Botan::PK_Verifier verifier(*public_key_, kBotanHash, Botan::Signature_Format::Standard);
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(signing_input.data());
        if (!verifier.verify_message(bytes, signing_input.size(), sig.data(), sig.size()))
            return std::nullopt;

        // 3. Claims (RFC 7519 section 4.1).
        const auto payload_raw = base64url_decode(payload_b64);
        const auto payload = boost::json::parse(std::string(payload_raw.begin(), payload_raw.end()));
        const auto& obj = payload.as_object();

        if (obj.at("iss").as_string() != kIssuer) return std::nullopt;

        JwtClaims claims;
        claims.sub = boost::json::value_to<std::string>(obj.at("sub"));
        claims.role = boost::json::value_to<std::string>(obj.at("role"));
        claims.iat = as_u64(obj.at("iat"));
        claims.exp = as_u64(obj.at("exp"));

        if (claims.sub.empty() || claims.role.empty()) return std::nullopt;
        if (now_epoch() >= claims.exp) return std::nullopt;
        return claims;
    }
    catch (...)
    {
        return std::nullopt;
    }
}

} // namespace pgdemo::auth
