#include <pki/Certificate.hpp>

#include <pki/Bytes.hpp>

#include <botan/data_src.h>
#include <botan/pk_keys.h>
#include <botan/pkix_types.h>
#include <botan/x509cert.h>

#include <algorithm>
#include <cctype>

namespace pgdemo::pki {

bool signed_with_sha256(std::string_view pem)
{
    try
    {
        Botan::DataSource_Memory src(pem);
        Botan::X509_Certificate cert(src);
        return cert.signature_algorithm().oid().to_string() == "1.2.840.10045.4.3.2";
    }
    catch (const std::exception&)
    {
        return false;
    }
}

bool self_signed_root(std::string_view pem, std::string_view expected_cn)
{
    try
    {
        Botan::DataSource_Memory src(pem);
        Botan::X509_Certificate cert(src);
        if (cert.subject_dn().get_first_attribute("CN") != expected_cn) return false;
        if (cert.issuer_dn() != cert.subject_dn() || !cert.is_CA_cert()) return false;
        const auto pub = cert.subject_public_key();
        if (!pub || !cert.check_signature(*pub)) return false;
        return cert.signature_algorithm().oid().to_string() == "1.2.840.10045.4.3.2";
    }
    catch (const std::exception&)
    {
        return false;
    }
}

CertInfo inspect_certificate(std::string_view pem)
{
    try
    {
        Botan::DataSource_Memory src(pem);
        const Botan::X509_Certificate cert(src);
        CertInfo info;
        info.serial = to_hex(cert.serial_number());
        std::string fp = cert.fingerprint("SHA-256");
        std::erase(fp, ':');
        std::ranges::transform(fp, fp.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        info.fingerprint = std::move(fp);
        info.subject_cn = cert.subject_dn().get_first_attribute("CN");
        info.not_before = cert.not_before().to_std_timepoint();
        info.not_after = cert.not_after().to_std_timepoint();
        info.pem = cert.PEM_encode();
        return info;
    }
    catch (const std::exception& e)
    {
        throw PkiError("CERT_MALFORMED", std::string("certificate does not parse: ") + e.what());
    }
}

} // namespace pgdemo::pki
