#include <pki/CertVerifier.hpp>

#include <botan/certstor.h>
#include <botan/data_src.h>
#include <botan/pubkey.h>
#include <botan/x509cert.h>
#include <botan/x509path.h>

namespace pgdemo::pki {

namespace {

Botan::X509_Certificate parse(std::string_view pem)
{
    try
    {
        Botan::DataSource_Memory src(pem);
        return Botan::X509_Certificate(src);
    }
    catch (const std::exception& e)
    {
        throw PkiError("CERT_MALFORMED", std::string("certificate does not parse: ") + e.what());
    }
}

} // namespace

CertVerifier::CertVerifier(const std::vector<std::string>& root_pems)
    : store_(std::make_unique<Botan::Certificate_Store_In_Memory>())
{
    for (const auto& pem : root_pems) store_->add_certificate(parse(pem));
}

CertVerifier::~CertVerifier() = default;

CertInfo CertVerifier::verify(std::string_view cert_pem, std::string_view expected_cn,
                              const std::set<std::string>& revoked_serials,
                              std::chrono::system_clock::time_point now) const
{
    const auto cert = parse(cert_pem);
    auto info = inspect_certificate(cert_pem);
    if (info.subject_cn != expected_cn)
        throw PkiError("CERT_SUBJECT", "certificate CN '" + info.subject_cn + "' is not '" +
                                           std::string(expected_cn) + "'");
    if (revoked_serials.contains(info.serial))
        throw PkiError("CERT_REVOKED", "certificate serial " + info.serial + " is revoked");

    const Botan::Path_Validation_Restrictions restrictions(false, 110);
    const auto result = Botan::x509_path_validate(cert, restrictions, *store_, "",
                                                  Botan::Usage_Type::TLS_CLIENT_AUTH, now);
    if (!result.successful_validation()) throw PkiError("CERT_CHAIN", result.result_string());
    return info;
}

bool CertVerifier::verify_signature(std::string_view cert_pem, std::span<const std::uint8_t> message,
                                    std::span<const std::uint8_t> signature) noexcept
{
    try
    {
        const auto cert = parse(cert_pem);
        const auto pub = cert.subject_public_key();
        if (!pub || pub->algo_name() != "ECDSA") return false;
        Botan::PK_Verifier verifier(*pub, "SHA-384", Botan::Signature_Format::Standard);
        return verifier.verify_message(message.data(), message.size(), signature.data(), signature.size());
    }
    catch (...)
    {
        return false;
    }
}

} // namespace pgdemo::pki
