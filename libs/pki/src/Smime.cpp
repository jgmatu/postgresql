#include <pki/CertificateAuthority.hpp>

#include <botan/asn1_obj.h>
#include <botan/auto_rng.h>
#include <botan/base64.h>
#include <botan/bigint.h>
#include <botan/der_enc.h>
#include <botan/hash.h>
#include <botan/hex.h>
#include <botan/pk_keys.h>
#include <botan/pubkey.h>
#include <botan/x509cert.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <format>
#include <stdexcept>
#include <string>
#include <vector>

namespace pgdemo::pki {

namespace {

/// OpenSSL `SMIME_crlf_copy`: each newline becomes CRLF, and a last line with
/// no newline is copied as it is.
std::string crlf_copy(std::string_view in)
{
    std::string out;
    out.reserve(in.size());
    std::size_t i = 0;
    while (i < in.size())
    {
        const auto cut = in.find_first_of("\r\n", i);
        if (cut == std::string_view::npos)
        {
            out.append(in.substr(i));
            break;
        }
        out.append(in.substr(i, cut - i));
        if (in[cut] == '\r' && cut + 1 < in.size() && in[cut + 1] == '\n')
            i = cut + 2;
        else
            i = cut + 1;
        out += "\r\n";
    }
    return out;
}

/// Bytes `PKCS7_TEXT` hashes: the text/plain header plus the CRLF document.
std::string text_canonical(std::string_view document)
{
    return "Content-Type: text/plain\r\n\r\n" + crlf_copy(document);
}

std::string utc_now()
{
    const auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
    if (gmtime_r(&t, &tm) == nullptr) throw std::runtime_error("factory: cannot read the signing time");
    return std::format("{:02}{:02}{:02}{:02}{:02}{:02}Z", tm.tm_year % 100, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                       tm.tm_min, tm.tm_sec);
}

std::vector<std::uint8_t> finish(Botan::DER_Encoder& enc)
{
    const auto bytes = enc.get_contents();
    return {bytes.begin(), bytes.end()};
}

Botan::OID oid(std::string_view dotted) { return Botan::OID::from_string(dotted); }

std::vector<std::uint8_t> attribute(const Botan::OID& type, const std::vector<std::uint8_t>& value)
{
    Botan::DER_Encoder enc;
    enc.start_sequence().encode(type).start_set().raw_bytes(value).end_cons().end_cons();
    return finish(enc);
}

std::vector<std::uint8_t> content_type_attr()
{
    Botan::DER_Encoder value;
    value.encode(oid("1.2.840.113549.1.7.1"));
    return attribute(oid("1.2.840.113549.1.9.3"), finish(value));
}

std::vector<std::uint8_t> signing_time_attr()
{
    Botan::DER_Encoder value;
    value.encode(Botan::ASN1_Time(utc_now(), Botan::ASN1_Type::UtcTime));
    return attribute(oid("1.2.840.113549.1.9.5"), finish(value));
}

std::vector<std::uint8_t> message_digest_attr(const std::vector<std::uint8_t>& digest)
{
    Botan::DER_Encoder value;
    value.encode(digest.data(), digest.size(), Botan::ASN1_Type::OctetString);
    return attribute(oid("1.2.840.113549.1.9.4"), finish(value));
}

/// DER SET of the authenticated attributes. The signature covers this encoding;
/// SignerInfo stores the same bytes with the SET tag replaced by `[0] IMPLICIT`.
std::vector<std::uint8_t> signed_attributes(const std::vector<std::uint8_t>& digest)
{
    Botan::DER_Encoder enc;
    enc.start_set()
        .raw_bytes(content_type_attr())
        .raw_bytes(signing_time_attr())
        .raw_bytes(message_digest_attr(digest))
        .end_cons();
    auto set = finish(enc);
    if (set.empty() || set[0] != 0x31) throw std::runtime_error("factory: signed attributes are not a SET");
    return set;
}

std::vector<std::uint8_t> sha256(std::string_view bytes)
{
    auto hash = Botan::HashFunction::create_or_throw("SHA-256");
    return hash->process<std::vector<std::uint8_t>>(bytes);
}

std::string b64_lines(const std::vector<std::uint8_t>& der)
{
    const auto raw = Botan::base64_encode(der.data(), der.size());
    std::string out;
    for (std::size_t i = 0; i < raw.size(); i += 64)
    {
        out.append(raw, i, std::min<std::size_t>(64, raw.size() - i));
        out.push_back('\n');
    }
    return out;
}

std::string boundary()
{
    Botan::AutoSeeded_RNG rng;
    std::vector<std::uint8_t> token(16);
    rng.randomize(token);
    return "----" + Botan::hex_encode(token.data(), token.size(), true);
}

} // namespace

std::string CertificateAuthority::sign_document(std::string_view document) const
{
    // Detached PKCS#7 SignedData, the shape `PKCS7_sign` + `SMIME_write_PKCS7`
    // emit with `PKCS7_DETACHED | PKCS7_TEXT`. SROS2 reads that with OpenSSL.
    // The digest is SHA-256 of the canonical text. The ECDSA signature is over
    // the DER SET of authenticated attributes, and the certificate rides inside
    // SignedData. Capabilities are omitted; a verifier does not require them.
    const auto canonical = text_canonical(document);
    const auto digest = sha256(canonical);
    const auto attrs = signed_attributes(digest);
    auto implicit_attrs = attrs;
    implicit_attrs[0] = 0xA0;

    Botan::AutoSeeded_RNG rng;
    Botan::PK_Signer signer(*key_, rng, "SHA-256", Botan::Signature_Format::DerSequence);
    const auto signature = signer.sign_message(attrs.data(), attrs.size(), rng);

    const auto sha256_id = Botan::AlgorithmIdentifier(oid("2.16.840.1.101.3.4.2.1"), Botan::AlgorithmIdentifier::USE_NULL_PARAM);
    const auto ecdsa_id =
        Botan::AlgorithmIdentifier(oid("1.2.840.10045.4.3.2"), Botan::AlgorithmIdentifier::USE_EMPTY_PARAM);
    const auto cert_der = cert_->BER_encode();

    Botan::DER_Encoder sid;
    sid.start_sequence()
        .raw_bytes(cert_->raw_issuer_dn())
        .encode(Botan::BigInt(cert_->serial_number().data(), cert_->serial_number().size()))
        .end_cons();

    Botan::DER_Encoder info;
    info.start_sequence()
        .encode(static_cast<std::size_t>(1))
        .raw_bytes(finish(sid))
        .encode(sha256_id)
        .raw_bytes(implicit_attrs)
        .encode(ecdsa_id)
        .encode(signature.data(), signature.size(), Botan::ASN1_Type::OctetString)
        .end_cons();

    Botan::DER_Encoder signed_data;
    signed_data.start_sequence()
        .encode(static_cast<std::size_t>(1))
        .start_set()
        .encode(sha256_id)
        .end_cons()
        .start_sequence()
        .encode(oid("1.2.840.113549.1.7.1"))
        .end_cons()
        .start_context_specific(0)
        .raw_bytes(cert_der)
        .end_cons()
        .start_set()
        .raw_bytes(finish(info))
        .end_cons()
        .end_cons();

    Botan::DER_Encoder content_info;
    content_info.start_sequence()
        .encode(oid("1.2.840.113549.1.7.2"))
        .start_explicit_context_specific(0)
        .raw_bytes(finish(signed_data))
        .end_cons()
        .end_cons();
    const auto pkcs7 = finish(content_info);

    const auto line = boundary();
    std::string out;
    out += "MIME-Version: 1.0\n";
    out += "Content-Type: multipart/signed; protocol=\"application/x-pkcs7-signature\"; micalg=\"sha-256\"; boundary=\"" +
           line + "\"\n\n";
    out += "This is an S/MIME signed message\n\n";
    out += "--" + line + "\n";
    out += canonical;
    out += "\n--" + line + "\n";
    out += "Content-Type: application/x-pkcs7-signature; name=\"smime.p7s\"\n";
    out += "Content-Transfer-Encoding: base64\n";
    out += "Content-Disposition: attachment; filename=\"smime.p7s\"\n\n";
    out += b64_lines(pkcs7);
    out += "\n--" + line + "--\n";
    return out;
}

} // namespace pgdemo::pki
