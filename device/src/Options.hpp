#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace pgdemo::device {

inline constexpr int kOk = 0;         ///< bye from the manager, or SIGTERM/SIGINT
inline constexpr int kFailed = 1;     ///< protocol or I/O error
inline constexpr int kUsage = 2;      ///< bad arguments
inline constexpr int kRejected = 3;   ///< enrollment refused by the CA
inline constexpr int kNoIdentity = 4; ///< no key or certificate on disk

/// Arguments of `pgdemo-device`. `AppBootstrap` may set `manager_cert_ok`.
struct Options
{
    std::string name;                  ///< device name and certificate CN
    std::string type;                  ///< device type; selects the scalar generator
    std::string payload;               ///< `scalar`, `gps`, `xy` or `xyz`
    std::filesystem::path dir;         ///< key directory, mode 0700
    std::string mode = "run";          ///< `run`, `enroll` or `rotate`
    std::int64_t request_id = 0;       ///< enrollment request, 0 on a plain run
    std::optional<std::int64_t> device_id; ///< row of `devices.puf` on the fork path
    std::filesystem::path puf;         ///< path of `devices.puf`
    std::filesystem::path puf_dir;     ///< directory of this worker's seed file
    std::string factory;               ///< `HOST:PORT` of the factory
    std::filesystem::path factory_root; ///< factory root certificate
    std::filesystem::path manager_cert; ///< manager certificate, used to verify its CRL
    bool manager_cert_ok = false;      ///< the manager certificate is its own root
};

/// ROS node name. `.` and `-` in the device name become `_`.
inline std::string node_name_for(std::string name)
{
    for (char& c : name)
        if (c == '.' || c == '-') c = '_';
    return "pgdemo_device_" + name;
}

} // namespace pgdemo::device
