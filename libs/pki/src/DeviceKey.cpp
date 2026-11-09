#include <pki/DeviceKey.hpp>

#include <botan/auto_rng.h>
#include <botan/data_src.h>
#include <botan/ec_group.h>
#include <botan/ecdsa.h>
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

std::vector<std::uint8_t> DeviceKey::sign(std::span<const std::uint8_t> message) const
{
    Botan::AutoSeeded_RNG rng;
    Botan::PK_Signer signer(*key_, rng, "SHA-384", Botan::Signature_Format::Standard);
    return signer.sign_message(message.data(), message.size(), rng);
}

} // namespace pgdemo::pki
