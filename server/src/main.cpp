// pgdemo-server: zone A process.
//
// HTTP API (Oat++) in front of PostgreSQL. It never talks to the controller:
// desired configuration goes into the dummy_config_pending queue, and results
// and live data come back as NOTIFY on the listener connection.
//
//   dummy1_events, dummy2_events --> EventHub --> SSE /api/stream
//   config_events                --> Dispatcher::dispatch(SUCCESS) + SSE
//   error_events                 --> Dispatcher::dispatch(ERROR)   + SSE
//
// Subcommand: `pgdemo-server hash-password` reads a password on stdin and
// prints its Argon2id PHC string (used by scripts/db-init.sh).

#include <csignal>
#include <cstdlib>
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
#include "controllers/DataController.hpp"
#include "controllers/UsersController.hpp"
#include "http/AuthInterceptor.hpp"
#include "http/ErrorHandler.hpp"

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
                 "  --token-ttl-s N           token lifetime (default 900)\n"
                 "  --config-timeout-ms N     POST /api/config wait (default 2000)\n";
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
        else if (a == "--token-ttl-s") c.token_ttl_s = std::stoull(next());
        else if (a == "--config-timeout-ms") c.config_timeout_ms = std::stoi(next());
        else if (a == "-h" || a == "--help") { usage(); std::exit(0); }
        else throw std::invalid_argument("unknown option " + std::string(a));
    }
    if (c.pool_size < 1) throw std::invalid_argument("--pool-size must be at least 1");
    if (c.token_ttl_s < 60 || c.token_ttl_s > 86400) throw std::invalid_argument("--token-ttl-s must be in [60, 86400]");
    if (c.config_timeout_ms < 100 || c.config_timeout_ms > 60000)
        throw std::invalid_argument("--config-timeout-ms must be in [100, 60000]");
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

void register_channels(Services& s)
{
    auto& db = *s.db;
    auto hub = s.hub;
    auto dispatcher = s.dispatcher;

    // Data plane: two tables, two channels, one listener socket.
    for (const auto t : db::kAllTables)
    {
        const std::string sse(db::table_name(t));
        db.register_listen_async(std::string(db::channel_name(t)),
                                 [hub, sse](const boost::json::object& msg) { hub->publish(sse, payload_text(msg)); });
    }

    // Control plane: completions wake the HTTP thread waiting on that request_id.
    db.register_listen_async(std::string(db::channel::kConfigEvents), [hub, dispatcher](const boost::json::object& msg) {
        if (const auto id = request_id_of(msg)) dispatcher->dispatch(*id, ResponseStatus::SUCCESS);
        hub->publish("config", payload_text(msg));
    });
    db.register_listen_async(std::string(db::channel::kErrorEvents), [hub, dispatcher](const boost::json::object& msg) {
        if (const auto id = request_id_of(msg)) dispatcher->dispatch(*id, ResponseStatus::ERROR);
        hub->publish("error", payload_text(msg));
    });
}

int run(const ServerConfig& cfg)
{
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

    // Registration phase, then activation (see DatabaseManager).
    register_channels(*services);
    services->db->run_listener_loop();

    auto mapper = oatpp::parser::json::mapping::ObjectMapper::createShared();
    auto router = oatpp::web::server::HttpRouter::createShared();
    router->addController(std::make_shared<AuthController>(services, mapper));
    router->addController(std::make_shared<UsersController>(services, mapper));
    router->addController(std::make_shared<DataController>(services, mapper));
    router->addController(std::make_shared<ConfigController>(services, mapper));

    auto handler = oatpp::web::server::HttpConnectionHandler::createShared(router);
    handler->setErrorHandler(std::make_shared<http::JsonErrorHandler>());
    handler->addRequestInterceptor(std::make_shared<http::AuthInterceptor>(services->jwt));

    auto provider = oatpp::network::tcp::server::ConnectionProvider::createShared(
        {cfg.listen_addr, cfg.port, oatpp::network::Address::IP_4});
    oatpp::network::Server server(provider, handler);

    log::info(kLog, std::format("listening on http://{}:{}", cfg.listen_addr, cfg.port));
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
