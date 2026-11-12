#include <pki/DeviceKey.hpp>

#include <botan/auto_rng.h>
#include <botan/bigint.h>
#include <botan/data_src.h>
#include <botan/ec_group.h>
#include <botan/ec_scalar.h>
#include <botan/ecdsa.h>
#include <botan/kdf.h>
#include <botan/pkcs10.h>
#include <botan/pkcs8.h>
#include <botan/pubkey.h>
#include <botan/x509self.h>

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace pgdemo::pki {

DeviceKey::DeviceKey(std::unique_ptr<Botan::Private_Key> key) : key_(std::move(key)) {}
DeviceKey::DeviceKey(DeviceKey&&) noexcept = default;
DeviceKey& DeviceKey::operator=(DeviceKey&&) noexcept = default;
DeviceKey::~DeviceKey() = default;

DeviceKey DeviceKey::generate()
{
    Botan::AutoSeeded_RNG rng;
    return DeviceKey(std::make_unique<Botan::ECDSA_PrivateKey>(rng, Botan::EC_Group::from_name("secp384r1")));
}

DeviceKey DeviceKey::from_seed(std::span<const std::uint8_t> seed)
{
    if (seed.size() != 32) throw std::invalid_argument("DeviceKey: seed must be 32 bytes");
    auto kdf = Botan::KDF::create_or_throw("HKDF(SHA-384)");
    constexpr std::string_view kInfo = "pgdemo/worker-key/v1";
    const auto info = std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(kInfo.data()), kInfo.size());
    const auto okm = kdf->derive_key<Botan::secure_vector<std::uint8_t>>(64, seed, {}, info);
    Botan::BigInt x(okm.data(), okm.size());
    const auto group = Botan::EC_Group::from_name("secp384r1");
    Botan::BigInt modulus = group.get_order();
    modulus -= 1;
    x %= modulus;
    x += 1;
    return DeviceKey(std::make_unique<Botan::ECDSA_PrivateKey>(group, Botan::EC_Scalar::from_bigint(group, x)));
}

DeviceKey DeviceKey::load(const std::string& path)
{
    std::ifstream in(path);
    if (!in) throw std::runtime_error("DeviceKey: cannot open " + path);
    Botan::DataSource_Stream src(in);
    auto key = Botan::PKCS8::load_key(src);
    if (!key || key->algo_name() != "ECDSA" || key->key_length() != 384)
        throw std::runtime_error("DeviceKey: " + path + " is not an ECDSA P-384 key");
    return DeviceKey(std::move(key));
}

void DeviceKey::save(const std::string& path) const
{
    const std::string pem = Botan::PKCS8::PEM_encode(*key_);
    const std::string tmp = path + ".tmp";
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) throw std::runtime_error("DeviceKey: cannot create " + tmp + ": " + std::strerror(errno));
    std::size_t done = 0;
    while (done < pem.size())
    {
        const ssize_t n = ::write(fd, pem.data() + done, pem.size() - done);
        if (n < 0)
        {
            if (errno == EINTR) continue;
            const int err = errno;
            ::close(fd);
            throw std::runtime_error("DeviceKey: cannot write " + tmp + ": " + std::strerror(err));
        }
        done += static_cast<std::size_t>(n);
    }
    ::fsync(fd);
    ::close(fd);
    if (std::rename(tmp.c_str(), path.c_str()) != 0)
        throw std::runtime_error("DeviceKey: cannot rename to " + path + ": " + std::strerror(errno));
}

std::string DeviceKey::csr_pem(std::string_view cn) const
{
    Botan::X509_Cert_Options opts;
    opts.common_name = std::string(cn);
    opts.constraints = Botan::Key_Constraints::DigitalSignature;
    opts.add_ex_constraint("PKIX.ClientAuth");
    Botan::AutoSeeded_RNG rng;
    return Botan::X509::create_cert_req(opts, *key_, "SHA-384", rng).PEM_encode();
}

std::string DeviceKey::self_signed_pem(std::string_view cn, bool server_auth) const
{
    Botan::X509_Cert_Options opts;
    opts.common_name = std::string(cn);
    opts.constraints = Botan::Key_Constraints::DigitalSignature;
    opts.add_ex_constraint(server_auth ? "PKIX.ServerAuth" : "PKIX.ClientAuth");
    Botan::AutoSeeded_RNG rng;
    return Botan::X509::create_self_signed_cert(opts, *key_, "SHA-384", rng).PEM_encode();
}

std::vector<std::uint8_t> DeviceKey::sign(std::span<const std::uint8_t> message) const
{
    Botan::AutoSeeded_RNG rng;
    Botan::PK_Signer signer(*key_, rng, "SHA-384", Botan::Signature_Format::Standard);
    return signer.sign_message(message.data(), message.size(), rng);
}

} // namespace pgdemo::pki
