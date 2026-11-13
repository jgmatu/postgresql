#pragma once

#include <atomic>
#include <cstdint>
#include <string_view>

namespace pgdemo::pki {
class CertificateAuthority;
}

namespace pgdemo::factory {

/// Accept each SIGN on its own coroutine. The protocol is the one `answer_factory` implements.
void serve_factory_asio(std::uint16_t port, const pki::CertificateAuthority& ca, const std::atomic<bool>& stop,
                        std::string_view listen_addr);

} // namespace pgdemo::factory
