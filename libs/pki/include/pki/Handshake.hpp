#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace pgdemo::pki {

/// The certificate the peer presented on a completed TLS 1.3 handshake.
struct HandshakePeer
{
    std::string common_name; ///< subject CN
    std::string pem;         ///< peer certificate, PEM
};

/**
 * @brief One mutual TLS 1.3 listener. Both sides must present a certificate.
 *
 * The trust anchor is the factory root. Revocation is applied by the caller
 * after `accept_one`, with the manager CRL.
 */
class HandshakeListener
{
public:
    /**
     * @param key_pem  PKCS#8 private key, PEM
     * @param cert_pem certificate of that key, PEM
     * @param root_pem factory root certificate, PEM
     * @throws std::runtime_error if the material or the socket cannot be set up
     */
    HandshakeListener(const std::string& key_pem, const std::string& cert_pem, const std::string& root_pem);
    ~HandshakeListener();

    HandshakeListener(const HandshakeListener&) = delete;
    HandshakeListener& operator=(const HandshakeListener&) = delete;

    /// Bound port on 127.0.0.1.
    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

    /// True when a client is waiting, or `timeout` elapses with none.
    [[nodiscard]] bool wait(std::chrono::milliseconds timeout) const;

    /**
     * @brief Accept one client and finish the handshake.
     * @throws PkiError `CERT_CHAIN` when the peer certificate is missing or rejected
     */
    [[nodiscard]] HandshakePeer accept_one();

private:
    struct Ctx;
    Ctx* ctx_{nullptr};
    int fd_{-1};
    std::uint16_t port_{0};
};

/**
 * @brief Connect to a `HandshakeListener` and require `expected_cn`.
 * @return the manager certificate
 * @throws PkiError `CERT_CHAIN` or `CERT_SUBJECT`
 */
[[nodiscard]] HandshakePeer handshake_connect(const std::string& host, std::uint16_t port, const std::string& key_pem,
                                              const std::string& cert_pem, const std::string& root_pem,
                                              std::string_view expected_cn);

} // namespace pgdemo::pki
