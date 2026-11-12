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
    return verify_as(cert_pem, expected_cn, revoked_serials, CertUse::Client, now);
}

CertInfo CertVerifier::verify_as(std::string_view cert_pem, std::string_view expected_cn,
                                 const std::set<std::string>& revoked_serials, CertUse use,
                                 std::chrono::system_clock::time_point now) const
{
    const auto cert = parse(cert_pem);
    auto info = inspect_certificate(cert_pem);
    if (info.subject_cn != expected_cn)
        throw PkiError("CERT_SUBJECT", "certificate CN '" + info.subject_cn + "' is not '" +
                                           std::string(expected_cn) + "'");
    if (revoked_serials.contains(info.serial))
        throw PkiError("CERT_REVOKED", "certificate serial " + info.serial + " is revoked");

    const auto usage = use == CertUse::Server ? Botan::Usage_Type::TLS_SERVER_AUTH : Botan::Usage_Type::TLS_CLIENT_AUTH;
    const Botan::Path_Validation_Restrictions restrictions(false, 110);
    const auto result = Botan::x509_path_validate(cert, restrictions, *store_, "", usage, now);
    if (!result.successful_validation()) throw PkiError("CERT_CHAIN", result.result_string());
    return info;
}

std::string manager_crl_payload(const std::set<std::string>& serials)
{
    std::string out = "pgdemo-manager-crl-v1\n";
    for (const auto& serial : serials) out += serial + '\n';
    return out;
}

void verify_factory_pair(std::string_view worker_pem, std::string_view manager_pem, std::string_view root_pem,
                         std::string_view worker_cn, std::string_view crl_payload,
                         std::span<const std::uint8_t> crl_signature)
{
    constexpr std::string_view kHeader = "pgdemo-manager-crl-v1\n";
    if (!crl_payload.starts_with(kHeader))
        throw PkiError("CERT_CHAIN", "the manager CRL payload is not the expected form");
    const auto payload = std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(crl_payload.data()),
                                                       crl_payload.size());
    if (!CertVerifier::verify_signature(manager_pem, payload, crl_signature))
        throw PkiError("CERT_CHAIN", "the manager CRL signature does not match the manager certificate");

    std::set<std::string> revoked;
    std::string_view rest = crl_payload.substr(kHeader.size());
    while (!rest.empty())
    {
        const auto cut = rest.find('\n');
        const auto line = rest.substr(0, cut);
        if (!line.empty()) revoked.emplace(line);
        if (cut == std::string_view::npos) break;
        rest.remove_prefix(cut + 1);
    }
    const std::string canonical = manager_crl_payload(revoked);
    if (canonical != crl_payload)
        throw PkiError("CERT_CHAIN", "the manager CRL payload is not the canonical serial list");

    CertVerifier anchors({std::string(root_pem)});
    anchors.verify_as(worker_pem, worker_cn, revoked, CertVerifier::CertUse::Client);
    const auto manager = inspect_certificate(manager_pem);
    anchors.verify_as(manager_pem, manager.subject_cn, {}, CertVerifier::CertUse::Server);
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
