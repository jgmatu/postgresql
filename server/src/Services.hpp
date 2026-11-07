#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <auth/JwtManager.hpp>
#include <db/DatabaseManager.hpp>
#include <dispatcher/Dispatcher.hpp>
#include <events/EventHub.hpp>

namespace pgdemo::server {

struct ServerConfig
{
    std::string listen_addr = "127.0.0.1";
    std::uint16_t port = 8080;
    std::string conninfo;
    std::string jwt_key = "run/keys/jwt.key";
    std::string jwt_cert = "run/keys/jwt.crt";
    std::uint64_t token_ttl_s = 900;
    int config_timeout_ms = 2000;
};

/// Shared by every controller. Each member is thread-safe.
struct Services
{
    ServerConfig config;
    std::shared_ptr<db::DatabaseManager> db;
    std::shared_ptr<auth::JwtManager> jwt;
    std::shared_ptr<Dispatcher> dispatcher;
    std::shared_ptr<EventHub> hub;
};

} // namespace pgdemo::server
