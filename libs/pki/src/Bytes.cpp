#include <pki/Bytes.hpp>

#include <botan/auto_rng.h>
#include <botan/hex.h>

namespace pgdemo::pki {

std::string to_hex(std::span<const std::uint8_t> bytes)
{
    return Botan::hex_encode(bytes.data(), bytes.size(), false);
}

std::vector<std::uint8_t> from_hex(std::string_view hex)
{
    try
    {
        return Botan::hex_decode(hex);
    }
    catch (const std::exception& e)
    {
        throw std::invalid_argument(std::string("invalid hex: ") + e.what());
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

} // namespace pgdemo::pki
