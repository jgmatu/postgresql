#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace pgdemo::pki {

/**
 * @brief The governance document every enclave shares.
 *
 * Authentication is required and RTPS traffic is signed. The domain range is
 * the whole DDS space, not only domain 0. The factory signs this exact text.
 */
[[nodiscard]] std::string governance_document();

/**
 * @brief The permissions document for one subject `CN=<cn>`.
 *
 * The allow rules are the SROS2 wildcard profile, so `rosout` and the node's
 * services keep working. `cn` is the certificate common name.
 *
 * @throws std::invalid_argument when `cn` is not a device name
 */
[[nodiscard]] std::string permissions_document(std::string_view cn);

/**
 * @brief Directory `rcl_get_secure_root` resolves for `enclave`.
 *
 * `enclave` is a ROS name such as `/pgdemo_controller`. The files live in
 * `<keystore>/enclaves/<name>`.
 */
[[nodiscard]] std::filesystem::path enclave_directory(const std::filesystem::path& keystore,
                                                      std::string_view enclave);

/**
 * @brief The identity CA file both enclaves share.
 *
 * The factory root is first, then the manager root. Fast-DDS checks the
 * signature algorithm of the first certificate and trusts every certificate
 * in the file.
 */
[[nodiscard]] std::string identity_ca_bundle(std::string_view factory_pem, std::string_view manager_pem);

/**
 * @brief Write the six files an SROS2 enclave needs.
 *
 * `identity_ca_pem` is the bundle of both roots. `permissions_ca_pem` is the
 * factory certificate, which signs the policy. The factory private key is not
 * written. `key.pem` is mode 0600.
 */
void write_enclave(const std::filesystem::path& directory, std::string_view key_pem, std::string_view cert_pem,
                   std::string_view identity_ca_pem, std::string_view permissions_ca_pem,
                   std::string_view governance_p7s, std::string_view permissions_p7s);

/// Export `ROS_SECURITY_*` so the next `rclcpp::init` uses `enclave`.
void enable_ros_security(const std::filesystem::path& keystore, std::string_view enclave);

/// Remove `ROS_SECURITY_ENABLE`, the strategy, the keystore and the enclave override.
void clear_ros_security();

} // namespace pgdemo::pki
