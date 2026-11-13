#pragma once

#include <cstdint>
#include <string>

#include "ProcessTable.hpp"

namespace pgdemo::db {
class DatabaseManager;
}

namespace pgdemo::controller {

/// Files the factory model uses. Absent files leave the running enrollment untouched.
struct OriginFiles
{
    std::string manager_key = "run/controller/manager.key"; ///< created by controller/scripts
    std::string factory_cert = "run/factory/root.crt";      ///< public certificate of the factory root
    std::string factory_host = "127.0.0.1";                 ///< factory process address
    std::uint16_t factory_port{9443};                       ///< factory process port
};

/**
 * @brief Register the manager root, sign its CRL, and write the SROS2 enclave.
 *
 * `manager.crt` beside the key must already be that self-signed root. When the
 * key, that certificate or the factory certificate is missing, this returns
 * without changing `fleet` and without setting `ROS_SECURITY_*`. When the
 * factory process is down, the root rows may be registered and security stays off.
 * Call it before the manager node is created: DDS reads the enclave at init.
 */
void start_origin(db::DatabaseManager& db, FleetOptions& fleet, const OriginFiles& files);

} // namespace pgdemo::controller
