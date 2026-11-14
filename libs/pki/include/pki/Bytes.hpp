#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pgdemo::pki {

/// Lower-case hex of `bytes`.
std::string to_hex(std::span<const std::uint8_t> bytes);

/**
 * @brief Inverse of `to_hex()`.
 * @throws std::invalid_argument on malformed input
 */
std::vector<std::uint8_t> from_hex(std::string_view hex);

/// `n` bytes from the system RNG.
std::vector<std::uint8_t> random_bytes(std::size_t n);

/// Domain separation string of the device authentication proof.
inline constexpr std::string_view kProofContext = "pgdemo-device-auth-v1";

/**
 * @brief Bytes a device signs to prove it holds the key of its certificate:
 * `kProofContext || 0 || nonce || 0 || name || 0 || fingerprint`.
 *
 * Binding the name and the certificate fingerprint stops a proof from being
 * replayed for another device or another certificate; the nonce stops it from
 * being replayed at all.
 */
std::vector<std::uint8_t> proof_message(std::span<const std::uint8_t> nonce, std::string_view name,
                                        std::string_view fingerprint);

} // namespace pgdemo::pki
