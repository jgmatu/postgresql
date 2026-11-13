#pragma once

#include <functional>
#include <string>
#include <variant>

#include <pgdemo_msgs/msg/bye.hpp>
#include <pgdemo_msgs/msg/certificate.hpp>
#include <pgdemo_msgs/msg/challenge.hpp>
#include <pgdemo_msgs/msg/config.hpp>
#include <pgdemo_msgs/msg/crl.hpp>
#include <pgdemo_msgs/msg/config_applied.hpp>
#include <pgdemo_msgs/msg/csr.hpp>
#include <pgdemo_msgs/msg/error.hpp>
#include <pgdemo_msgs/msg/gps_sample.hpp>
#include <pgdemo_msgs/msg/heartbeat.hpp>
#include <pgdemo_msgs/msg/hello.hpp>
#include <pgdemo_msgs/msg/proof.hpp>
#include <pgdemo_msgs/msg/rejected.hpp>
#include <pgdemo_msgs/msg/scalar_sample.hpp>
#include <pgdemo_msgs/msg/xy_sample.hpp>
#include <pgdemo_msgs/msg/xyz_sample.hpp>

namespace pgdemo::controller {

/// What a worker publishes after `hello`. `hello` itself opens the session.
using Uplink = std::variant<pgdemo_msgs::msg::Heartbeat, pgdemo_msgs::msg::Error, pgdemo_msgs::msg::Csr,
                            pgdemo_msgs::msg::Proof, pgdemo_msgs::msg::ConfigApplied, pgdemo_msgs::msg::ScalarSample,
                            pgdemo_msgs::msg::GpsSample, pgdemo_msgs::msg::XySample, pgdemo_msgs::msg::XyzSample>;

/// Publishers the manager owns. Fleet calls them; it does not include rclcpp.
struct Downlink
{
    /// End a worker process.
    std::function<void(pgdemo_msgs::msg::Bye)> bye;
    /// Deliver an issued certificate.
    std::function<void(pgdemo_msgs::msg::Certificate)> certificate;
    /// Tell a worker the CA refused its request.
    std::function<void(pgdemo_msgs::msg::Rejected)> rejected;
    /// Send a fresh authentication nonce.
    std::function<void(pgdemo_msgs::msg::Challenge)> challenge;
    /// Forward a desired configuration. The worker answers on `config/applied`.
    std::function<void(pgdemo_msgs::msg::Config)> config;
    /// Republish the manager revocation list. Empty when that origin is off.
    std::function<void(pgdemo_msgs::msg::Crl)> crl;
};

/// The `device` field of any uplink message.
inline std::string device_of(const Uplink& msg)
{
    return std::visit([](const auto& m) -> std::string { return m.device; }, msg);
}

} // namespace pgdemo::controller
