// pgdemo-server: zone A process.
//
// HTTP API (Oat++) in front of PostgreSQL, and the device CA. It never talks to
// the controller: enrollments and desired configuration are queued in tables,
// and results and live data come back as NOTIFY on the listener connection.
//
//   telemetry_scalar, telemetry_gps, device_events --> EventHub --> SSE /api/stream
//   config_events                --> Dispatcher::dispatch(SUCCESS) + SSE
//   error_events                 --> Dispatcher::dispatch(ERROR)   + SSE
//   enrollment_csr               --> CertificateSigner: sign, issue or reject
//   enrollment_events            --> Dispatcher::dispatch(ISSUED ? SUCCESS : ERROR) + SSE
//
// Subcommand: `pgdemo-server hash-password` reads a password on stdin and
// prints its Argon2id PHC string (used by scripts/db-init.sh).

#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#include <auth/JwtManager.hpp>
#include <auth/PasswordHasher.hpp>
#include <db/DatabaseManager.hpp>
#include <log/Log.hpp>

#include "Services.hpp"
#include "controllers/AuthController.hpp"
#include "controllers/ConfigController.hpp"
#include "controllers/DeviceController.hpp"
#include "controllers/IdentityController.hpp"
#include "controllers/UsersController.hpp"
#include "http/AuthInterceptor.hpp"
#include "http/ErrorHandler.hpp"

#include "oatpp-openssl/Config.hpp"
#include "oatpp-openssl/server/ConnectionProvider.hpp"
#include "oatpp/core/base/Environment.hpp"
#include "oatpp/network/Server.hpp"
#include "oatpp/network/tcp/server/ConnectionProvider.hpp"
#include "oatpp/parser/json/mapping/ObjectMapper.hpp"
#include "oatpp/web/server/HttpConnectionHandler.hpp"
#include "oatpp/web/server/HttpRouter.hpp"

namespace {

using namespace pgdemo;
using namespace pgdemo::server;

constexpr std::string_view kLog = "server";

void usage()
{
    std::cout << "Usage: pgdemo-server [options]\n"
                 "       pgdemo-server hash-password   < password\n"
                 "  --listen-addr ADDR        bind address (default 127.0.0.1)\n"
                 "  --port N                  TCP port (default 8080)\n"
                 "  --conninfo STR            libpq connection string (default: PG* environment)\n"
                 "  --pool-size N             query connections (default 8)\n"
                 "  --jwt-key PATH            ES384 PKCS#8 private key (default run/keys/jwt.key)\n"
                 "  --jwt-cert PATH           X.509 certificate of that key (default run/keys/jwt.crt)\n"
                 "  --tls-key PATH            TLS private key; with --tls-cert, listen on HTTPS\n"
                 "  --tls-cert PATH           TLS certificate; omit both to listen on HTTP\n"
                 "  --token-ttl-s N           token lifetime (default 900)\n"
                 "  --config-timeout-ms N     POST /api/config wait (default 2000)\n"
                 "  --ca-key PATH             device CA private key (default run/ca/ca.key)\n"
                 "  --ca-cert PATH            device CA certificate (default run/ca/ca.crt)\n"
                 "  --cert-validity-days N    lifetime of device certificates (default 365)\n"
                 "  --enroll-timeout-ms N     POST /api/devices and rotate wait (default 5000)\n";
}

ServerConfig parse(int argc, char** argv)
{
    ServerConfig c;
    for (int i = 1; i < argc; ++i)
    {
        const std::string_view a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) throw std::invalid_argument(std::string(a) + " needs a value");
            return argv[++i];
        };
        if (a == "--listen-addr") c.listen_addr = next();
        else if (a == "--port") c.port = static_cast<std::uint16_t>(std::stoul(next()));
        else if (a == "--conninfo") c.conninfo = next();
        else if (a == "--pool-size") c.pool_size = std::stoul(next());
        else if (a == "--jwt-key") c.jwt_key = next();
        else if (a == "--jwt-cert") c.jwt_cert = next();
        else if (a == "--tls-key") c.tls_key = next();
        else if (a == "--tls-cert") c.tls_cert = next();
        else if (a == "--token-ttl-s") c.token_ttl_s = std::stoull(next());
        else if (a == "--config-timeout-ms") c.config_timeout_ms = std::stoi(next());
        else if (a == "--ca-key") c.ca_key = next();
        else if (a == "--ca-cert") c.ca_cert = next();
        else if (a == "--cert-validity-days") c.cert_validity_days = std::stoi(next());
        else if (a == "--enroll-timeout-ms") c.enroll_timeout_ms = std::stoi(next());
        else if (a == "-h" || a == "--help") { usage(); std::exit(0); }
        else throw std::invalid_argument("unknown option " + std::string(a));
    }
    if (c.tls_key.empty() != c.tls_cert.empty())
        throw std::invalid_argument("--tls-key and --tls-cert must be set together");
    if (c.pool_size < 1) throw std::invalid_argument("--pool-size must be at least 1");
    if (c.token_ttl_s < 60 || c.token_ttl_s > 86400) throw std::invalid_argument("--token-ttl-s must be in [60, 86400]");
    if (c.config_timeout_ms < 100 || c.config_timeout_ms > 60000)
        throw std::invalid_argument("--config-timeout-ms must be in [100, 60000]");
    if (c.enroll_timeout_ms < 100 || c.enroll_timeout_ms > 60000)
        throw std::invalid_argument("--enroll-timeout-ms must be in [100, 60000]");
    if (c.cert_validity_days < 1 || c.cert_validity_days > 3650)
        throw std::invalid_argument("--cert-validity-days must be in [1, 3650]");
    return c;
}

int hash_password()
{
    std::string pw;
    std::getline(std::cin, pw);
    if (!pw.empty() && pw.back() == '\r') pw.pop_back();
    if (pw.empty())
    {
        std::cerr << "hash-password: empty password on stdin\n";
        return 2;
    }
    std::cout << auth::PasswordHasher::hash(pw) << "\n";
    return 0;
}

/// Request id carried by a config_events / error_events payload, if any.
std::optional<std::uint64_t> request_id_of(const boost::json::object& msg)
{
    const auto* payload = msg.if_contains("payload");
    if (!payload || !payload->is_object()) return std::nullopt;
    const auto* id = payload->as_object().if_contains("request_id");
    if (!id || !id->is_int64() || id->as_int64() < 1) return std::nullopt;
    return static_cast<std::uint64_t>(id->as_int64());
}

std::string payload_text(const boost::json::object& msg)
{
    const auto* payload = msg.if_contains("payload");
    return payload ? boost::json::serialize(*payload) : std::string("null");
}

/// Device name carried by every payload of sql/04_triggers.sql; empty if absent.
std::string device_of(const boost::json::object& msg)
{
    const auto* payload = msg.if_contains("payload");
    if (!payload || !payload->is_object()) return {};
    const auto* name = payload->as_object().if_contains("device");
    return name && name->is_string() ? std::string(name->as_string()) : std::string{};
}

void register_channels(Services& s)
{
    auto& db = *s.db;
    auto hub = s.hub;
    auto dispatcher = s.dispatcher;

    // Data plane: one channel per payload shape, whatever the number of devices.
    for (const auto p : db::kAllPayloads)
    {
        const std::string channel(db::telemetry_channel(p));
        db.register_listen_async(channel, [hub, channel](const boost::json::object& msg) {
            hub->publish(channel, device_of(msg), payload_text(msg));
        });
    }
    db.register_listen_async(std::string(db::channel::kDeviceEvents), [hub](const boost::json::object& msg) {
        hub->publish("device", device_of(msg), payload_text(msg));
    });

    // Control plane: completions wake the HTTP thread waiting on that request_id.
    db.register_listen_async(std::string(db::channel::kConfigEvents), [hub, dispatcher](const boost::json::object& msg) {
        if (const auto id = request_id_of(msg)) dispatcher->dispatch(*id, ResponseStatus::SUCCESS);
        hub->publish("config", device_of(msg), payload_text(msg));
    });
    db.register_listen_async(std::string(db::channel::kErrorEvents), [hub, dispatcher](const boost::json::object& msg) {
        if (const auto id = request_id_of(msg)) dispatcher->dispatch(*id, ResponseStatus::ERROR);
        hub->publish("error", device_of(msg), payload_text(msg));
    });

    // Identity: the controller submits CSRs, this process signs them.
    // Weak: the signer holds the DatabaseManager that holds this callback.
    std::weak_ptr<CertificateSigner> signer = s.signer;
    db.register_listen_async(std::string(db::channel::kEnrollmentRequested), [hub](const boost::json::object& msg) {
        hub->publish("enrollment", device_of(msg), payload_text(msg));
    });
    db.register_listen_async(std::string(db::channel::kEnrollmentCsr), [hub, signer](const boost::json::object& msg) {
        const auto id = request_id_of(msg);
        if (const auto sp = signer.lock(); sp && id) sp->submit(static_cast<std::int64_t>(*id));
        hub->publish("enrollment", device_of(msg), payload_text(msg));
    });
    db.register_listen_async(std::string(db::channel::kEnrollmentEvents), [hub, dispatcher](const boost::json::object& msg) {
        if (const auto id = request_id_of(msg))
        {
            const auto& payload = msg.at("payload").as_object();
            const auto* status = payload.if_contains("status");
            const bool issued = status && status->is_string() && status->as_string() == "ISSUED";
            dispatcher->dispatch(*id, issued ? ResponseStatus::SUCCESS : ResponseStatus::ERROR);
        }
        hub->publish("enrollment", device_of(msg), payload_text(msg));
    });
}

/// Load the device CA and check that the database trusts it.
std::shared_ptr<CertificateSigner> make_signer(const ServerConfig& cfg, std::shared_ptr<db::DatabaseManager> db)
{
    auto ca = std::make_shared<pki::CertificateAuthority>(cfg.ca_key, cfg.ca_cert);
    const auto roots = db->active_root_fingerprints();
    if (std::ranges::find(roots, ca->fingerprint()) == roots.end())
    {
        log::warn(kLog, "device CA " + ca->fingerprint() +
                            " is not an active root_ca row; issuing will fail (run scripts/db-init.sh)");
    }
    log::info(kLog, "device CA " + ca->fingerprint() + " (" + cfg.ca_cert + ")");
    return std::make_shared<CertificateSigner>(std::move(db), std::move(ca),
                                               std::chrono::days(cfg.cert_validity_days));
}

/// Point OpenSSL at the provider and the config copied beside this executable.
/// oatpp-openssl creates its SSL_CTX afterwards and inherits both.
void use_openssl_beside_executable()
{
    const auto dir = std::filesystem::canonical("/proc/self/exe").parent_path();
    const auto conf = dir / "openssl-oqs.cnf";
    const auto module = dir / "oqsprovider.so";
    if (!std::filesystem::is_regular_file(conf) || !std::filesystem::is_regular_file(module))
    {
        throw std::runtime_error("TLS files missing beside the executable (" + dir.string() + ")");
    }
    if (setenv("OPENSSL_CONF", conf.c_str(), 1) != 0 || setenv("OPENSSL_MODULES", dir.c_str(), 1) != 0)
    {
        throw std::runtime_error("cannot set OPENSSL_CONF / OPENSSL_MODULES");
    }
}

int run(const ServerConfig& cfg)
{
    const bool tls = !cfg.tls_key.empty();
    if (tls) use_openssl_beside_executable();

    sigset_t sigs;
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGINT);
    sigaddset(&sigs, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &sigs, nullptr);
    // A client that disconnects mid-stream must not kill the process.
    std::signal(SIGPIPE, SIG_IGN);

    auto services = std::make_shared<Services>();
    services->config = cfg;
    services->jwt = std::make_shared<auth::JwtManager>(cfg.jwt_key, cfg.jwt_cert);
    services->dispatcher = std::make_shared<Dispatcher>();
    services->hub = std::make_shared<EventHub>();
    services->db = std::make_shared<db::DatabaseManager>(cfg.conninfo, "pgdemo-server", cfg.pool_size);
    services->db->connect();
    services->signer = make_signer(cfg, services->db);

    // Registration phase, then activation (see DatabaseManager).
    register_channels(*services);
    services->db->run_listener_loop();
    // CSRs submitted while this server was down.
    for (const auto id : services->db->submitted_csr_ids()) services->signer->submit(id);

    auto mapper = oatpp::parser::json::mapping::ObjectMapper::createShared();
    auto router = oatpp::web::server::HttpRouter::createShared();
    router->addController(std::make_shared<AuthController>(services, mapper));
    router->addController(std::make_shared<UsersController>(services, mapper));
    router->addController(std::make_shared<DeviceController>(services, mapper));
    router->addController(std::make_shared<IdentityController>(services, mapper));
    router->addController(std::make_shared<ConfigController>(services, mapper));

    auto handler = oatpp::web::server::HttpConnectionHandler::createShared(router);
    handler->setErrorHandler(std::make_shared<http::JsonErrorHandler>());
    handler->addRequestInterceptor(std::make_shared<http::AuthInterceptor>(services->jwt));

    auto tcp = oatpp::network::tcp::server::ConnectionProvider::createShared(
        {cfg.listen_addr, cfg.port, oatpp::network::Address::IP_4});
    std::shared_ptr<oatpp::network::ServerConnectionProvider> provider = tcp;
    if (tls)
    {
        auto tls_cfg = oatpp::openssl::Config::createDefaultServerConfigShared(cfg.tls_cert, cfg.tls_key);
        provider = oatpp::openssl::server::ConnectionProvider::createShared(tls_cfg, tcp);
    }
    oatpp::network::Server server(provider, handler);

    log::info(kLog, std::format("listening on {}://{}:{}", tls ? "https" : "http", cfg.listen_addr, cfg.port));
    std::thread http_thread([&] { server.run(); });

    int exit_code = 0;
    for (;;)
    {
        const timespec tick{5, 0};
        const int sig = sigtimedwait(&sigs, nullptr, &tick);
        if (sig == SIGINT || sig == SIGTERM)
        {
            log::info(kLog, std::format("signal {} received, shutting down", sig));
            break;
        }
        if (!services->db->listener_alive())
        {
            log::error(kLog, "listener connection lost; exiting so a supervisor can restart us");
            exit_code = 1;
            break;
        }
    }

    services->hub->close_all();
    server.stop();
    provider->stop();
    handler->stop();
    http_thread.join();
    services->signer.reset();
    services->db->disconnect();
    return exit_code;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc >= 2 && std::string_view(argv[1]) == "hash-password") return hash_password();

    pgdemo::server::ServerConfig cfg;
    try
    {
        cfg = parse(argc, argv);
    }
    catch (const std::exception& e)
    {
        std::cerr << "pgdemo-server: " << e.what() << "\n";
        usage();
        return 2;
    }

    oatpp::base::Environment::init();
    int rc = 1;
    try
    {
        rc = run(cfg);
    }
    catch (const std::exception& e)
    {
        pgdemo::log::error(kLog, e.what());
    }
    oatpp::base::Environment::destroy();
    return rc;
}
