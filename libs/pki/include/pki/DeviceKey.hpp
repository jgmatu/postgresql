#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Botan {
class Private_Key;
} // namespace Botan

namespace pgdemo::pki {

/**
 * @brief The private key of one device (ECDSA P-384), used by `pgdemo-device`.
 *
 * Generated inside the device process and written to its own directory with
 * mode 0600; it never travels.
 */
class DeviceKey
{
public:
    /// A fresh P-384 key from the system RNG.
    static DeviceKey generate();

    /**
     * @brief The same P-384 key every time for the same 32-byte seed.
     *
     * HKDF-SHA384 with info `pgdemo/worker-key/v1` expands the seed to 64 bytes.
     * The integer is reduced modulo `n - 1` and then increased by one, so the
     * secp384r1 scalar is in `1 .. n - 1`. The ECDSA nonce stays random.
     *
     * @throws std::invalid_argument if `seed` is not 32 bytes
     */
    static DeviceKey from_seed(std::span<const std::uint8_t> seed);

    /**
     * @brief Load a PKCS#8 PEM key written by `save()`.
     * @throws std::runtime_error if the file cannot be read or is not P-384
     */
    static DeviceKey load(const std::string& path);

    /// Movable, not copyable: the key exists once in memory.
    DeviceKey(DeviceKey&&) noexcept;
    /// Move assignment.
    DeviceKey& operator=(DeviceKey&&) noexcept;
    ~DeviceKey();

    /**
     * @brief Write the key as unencrypted PKCS#8 PEM, mode 0600, atomically
     * (temporary file, then rename).
     * @throws std::runtime_error on I/O failure
     */
    void save(const std::string& path) const;

    /**
     * @brief PKCS#10 request for `CN=cn`, asking for `digitalSignature` and
     * `clientAuth`, signed with this key (ECDSA-SHA384).
     * @return the CSR, PEM
     */
    [[nodiscard]] std::string csr_pem(std::string_view cn) const;

    /**
     * @brief Self-signed certificate for `CN=cn`, proof that this process holds the key.
     *
     * The factory checks this signature and returns a new certificate for the
     * same public key. `server_auth` selects `serverAuth`; otherwise `clientAuth`.
     * @return the certificate, PEM
     */
    [[nodiscard]] std::string self_signed_pem(std::string_view cn, bool server_auth = false) const;

    /// ECDSA-SHA384 signature of `message`, raw `r || s` (96 bytes).
    [[nodiscard]] std::vector<std::uint8_t> sign(std::span<const std::uint8_t> message) const;

private:
    explicit DeviceKey(std::unique_ptr<Botan::Private_Key> key);
    std::unique_ptr<Botan::Private_Key> key_;
};

} // namespace pgdemo::pki
