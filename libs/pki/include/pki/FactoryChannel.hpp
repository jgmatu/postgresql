#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>

namespace pgdemo::pki {

class CertificateAuthority;

/**
 * @brief Ask the factory process to sign a self-signed certificate.
 *
 * The private key stays here. The factory checks the self-signature and
 * returns a new certificate for the same public key. `role` is `worker` or
 * `manager`.
 *
 * @throws std::runtime_error if the factory is unreachable or refuses
 */
[[nodiscard]] std::string request_signature(const std::string& host, std::uint16_t port, std::string_view role,
                                            std::string_view cn, std::string_view presented_pem);

/**
 * @brief Serve `request_signature` on 127.0.0.1:`port` until `stop` is set.
 *
 * The CA key is the one already loaded into `ca`. This function does not
 * create a key.
 */
/**
 * @param port  TCP port on 127.0.0.1; 0 binds an ephemeral port
 * @param bound set to the port actually bound, before the accept loop, when not null
 */
void serve_factory(std::uint16_t port, const CertificateAuthority& ca, const std::atomic<bool>& stop,
                   std::atomic<std::uint16_t>* bound = nullptr);

} // namespace pgdemo::pki
