#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include <auth/JwtManager.hpp>
#include <db/DatabaseManager.hpp>
#include <dispatcher/Dispatcher.hpp>
#include <events/EventHub.hpp>

#include "pki/CertificateSigner.hpp"

namespace pgdemo::server {

/// Command-line options of `pgdemo-server`.
struct ServerConfig
{
    std::string listen_addr = "127.0.0.1"; ///< `--listen-addr`: IPv4 bind address
    std::uint16_t port = 8080;              ///< `--port`: TCP port
    std::string conninfo;                   ///< `--conninfo`: libpq string; empty uses PG* variables
    std::size_t pool_size = db::DatabaseManager::kDefaultPoolSize; ///< `--pool-size`: query connections
    std::string jwt_key = "run/keys/jwt.key";   ///< `--jwt-key`: PKCS#8 signing key
    std::string jwt_cert = "run/keys/jwt.crt";  ///< `--jwt-cert`: certificate of that key
    std::string tls_key;                        ///< `--tls-key`: TLS private key, not the JWT key
    std::string tls_cert;                       ///< `--tls-cert`: TLS certificate presented to clients
    std::uint64_t token_ttl_s = 900;            ///< `--token-ttl-s`: token lifetime, 60..86400
    int config_timeout_ms = 2000;               ///< `--config-timeout-ms`: POST /api/config wait
    std::string ca_key = "run/ca/ca.key";       ///< `--ca-key`: device CA private key (signs CSRs)
    std::string ca_cert = "run/ca/ca.crt";      ///< `--ca-cert`: device CA certificate
    int cert_validity_days = 365;               ///< `--cert-validity-days`: lifetime of device certificates
    int enroll_timeout_ms = 5000;               ///< `--enroll-timeout-ms`: POST /api/devices and rotate wait
};

/// Shared by every controller. Each member is thread-safe.
struct Services
{
    ServerConfig config;                         ///< parsed command line
    std::shared_ptr<db::DatabaseManager> db;     ///< query pool and NOTIFY listener
    std::shared_ptr<auth::JwtManager> jwt;       ///< token signing and verification
    std::shared_ptr<Dispatcher> dispatcher;      ///< waits of POST /api/config
    std::shared_ptr<EventHub> hub;               ///< SSE fan-out
    std::shared_ptr<CertificateSigner> signer;   ///< device CA: signs CSRs, builds the CRL
};

} // namespace pgdemo::server
