#include <pki/CertificateAuthority.hpp>

#include <botan/auto_rng.h>
#include <botan/bigint.h>
#include <botan/data_src.h>
#include <botan/pk_keys.h>
#include <botan/pkcs10.h>
#include <botan/pkcs8.h>
#include <botan/x509_ca.h>
#include <botan/x509_crl.h>
#include <botan/x509_ext.h>
#include <botan/x509cert.h>

#include <fstream>
#include <map>

namespace pgdemo::pki {

namespace {

Botan::CRL_Code crl_code(const std::string& reason)
{
    static const std::map<std::string, Botan::CRL_Code> codes{
        {"unspecified", Botan::CRL_Code::Unspecified},
        {"keyCompromise", Botan::CRL_Code::KeyCompromise},
        {"affiliationChanged", Botan::CRL_Code::AffiliationChanged},
        {"superseded", Botan::CRL_Code::Superseded},
        {"cessationOfOperation", Botan::CRL_Code::CessationOfOperation},
        {"certificateHold", Botan::CRL_Code::CertificateHold},
    };
    const auto it = codes.find(reason);
    return it == codes.end() ? Botan::CRL_Code::Unspecified : it->second;
}

} // namespace

CertificateAuthority::CertificateAuthority(const std::string& key_path, const std::string& cert_path)
{
    std::ifstream key_stream(key_path);
    if (!key_stream) throw std::runtime_error("CertificateAuthority: cannot open " + key_path);
    Botan::DataSource_Stream key_src(key_stream);
    key_ = Botan::PKCS8::load_key(key_src);
    if (!key_ || key_->algo_name() != "ECDSA" || key_->key_length() != 384)
        throw std::runtime_error("CertificateAuthority: " + key_path + " is not an ECDSA P-384 PKCS#8 key");

    cert_ = std::make_unique<Botan::X509_Certificate>(cert_path);
    if (!cert_->is_CA_cert())
        throw std::runtime_error("CertificateAuthority: " + cert_path + " is not a CA certificate");
    if (cert_->subject_public_key()->public_key_bits() != key_->public_key_bits())
        throw std::runtime_error("CertificateAuthority: " + key_path + " does not match " + cert_path);

    pem_ = cert_->PEM_encode();
    fingerprint_ = inspect_certificate(pem_).fingerprint;
}

CertificateAuthority::~CertificateAuthority() = default;

CertInfo CertificateAuthority::sign_csr(std::string_view csr_pem, std::string_view expected_cn,
                                        std::chrono::seconds validity) const
{
    std::unique_ptr<Botan::PKCS10_Request> req;
    try
    {
        Botan::DataSource_Memory src(csr_pem);
        req = std::make_unique<Botan::PKCS10_Request>(src);
    }
    catch (const std::exception& e)
    {
        throw PkiError("CSR_MALFORMED", std::string("CSR does not parse: ") + e.what());
    }

    const auto pub = req->subject_public_key();
    if (!pub || pub->algo_name() != "ECDSA" || pub->key_length() != 384)
        throw PkiError("CSR_KEY", "the CSR key must be ECDSA P-384");
    if (!req->check_signature(*pub))
        throw PkiError("CSR_SIGNATURE", "the CSR is not signed by its own key");
    const std::string cn = req->subject_dn().get_first_attribute("CN");
    if (cn != expected_cn)
        throw PkiError("CSR_SUBJECT", "CSR CN '" + cn + "' is not '" + std::string(expected_cn) + "'");
    if (req->is_CA()) throw PkiError("CSR_CA", "a device certificate cannot be a CA");

    // The profile is fixed here; nothing else the CSR asks for is copied.
    Botan::X509_DN subject;
    subject.add_attribute("X520.CommonName", cn);
    Botan::Extensions ext;
    ext.add(std::make_unique<Botan::Cert_Extension::Basic_Constraints>(false), true);
    ext.add(std::make_unique<Botan::Cert_Extension::Key_Usage>(Botan::Key_Constraints::DigitalSignature), true);
    ext.add(std::make_unique<Botan::Cert_Extension::Extended_Key_Usage>(
        std::vector<Botan::OID>{Botan::OID::from_string("PKIX.ClientAuth")}));
    ext.add(std::make_unique<Botan::Cert_Extension::Subject_Key_ID>(req->raw_public_key(), "SHA-256"));
    ext.add(std::make_unique<Botan::Cert_Extension::Authority_Key_ID>(cert_->subject_key_id()));

    Botan::AutoSeeded_RNG rng;
    Botan::X509_CA ca(*cert_, *key_, "SHA-384", rng);
    const Botan::BigInt serial(rng, 128);
    const auto now = std::chrono::system_clock::now();
    const auto cert = Botan::X509_CA::make_cert(
        ca.signature_op(), rng, serial, ca.algorithm_identifier(), req->raw_public_key(),
        Botan::X509_Time(now - std::chrono::minutes(1)), Botan::X509_Time(now + validity), cert_->subject_dn(),
        subject, ext);
    return inspect_certificate(cert.PEM_encode());
}

std::string CertificateAuthority::crl_pem(const std::vector<RevokedCert>& revoked,
                                          std::chrono::seconds next_update) const
{
    std::vector<Botan::CRL_Entry> entries;
    entries.reserve(revoked.size());
    for (const auto& r : revoked)
    {
        Botan::DataSource_Memory src(r.cert_pem);
        entries.emplace_back(Botan::X509_Certificate(src), crl_code(r.reason));
    }
    Botan::AutoSeeded_RNG rng;
    Botan::X509_CA ca(*cert_, *key_, "SHA-384", rng);
    const auto now = std::chrono::system_clock::now();
    const auto empty = ca.new_crl(rng, now, next_update);
    return ca.update_crl(empty, entries, rng, now, next_update).PEM_encode();
}

} // namespace pgdemo::pki
