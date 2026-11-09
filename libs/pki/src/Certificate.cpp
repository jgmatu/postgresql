#include <pki/Certificate.hpp>

#include <botan/auto_rng.h>
#include <botan/base64.h>
#include <botan/data_src.h>
#include <botan/hex.h>
#include <botan/pkix_types.h>
#include <botan/x509cert.h>

#include <algorithm>
#include <cctype>

namespace pgdemo::pki {

std::string to_hex(std::span<const std::uint8_t> bytes)
{
    return Botan::hex_encode(bytes.data(), bytes.size(), false);
}

std::string base64_encode(std::span<const std::uint8_t> bytes)
{
    return Botan::base64_encode(bytes.data(), bytes.size());
}

std::vector<std::uint8_t> base64_decode(std::string_view text)
{
    try
    {
        return Botan::unlock(Botan::base64_decode(text));
    }
    catch (const std::exception& e)
    {
        throw std::invalid_argument(std::string("invalid base64: ") + e.what());
    }
}

std::vector<std::uint8_t> random_bytes(std::size_t n)
{
    Botan::AutoSeeded_RNG rng;
    std::vector<std::uint8_t> out(n);
    rng.randomize(out);
    return out;
}

std::vector<std::uint8_t> proof_message(std::span<const std::uint8_t> nonce, std::string_view name,
                                        std::string_view fingerprint)
{
    std::vector<std::uint8_t> m;
    m.reserve(kProofContext.size() + nonce.size() + name.size() + fingerprint.size() + 3);
    m.insert(m.end(), kProofContext.begin(), kProofContext.end());
    m.push_back(0);
    m.insert(m.end(), nonce.begin(), nonce.end());
    m.push_back(0);
    m.insert(m.end(), name.begin(), name.end());
    m.push_back(0);
    m.insert(m.end(), fingerprint.begin(), fingerprint.end());
    return m;
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
