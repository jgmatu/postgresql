#pragma once

#include "Options.hpp"

namespace pgdemo::device {

/**
 * @brief PUF, factory identity and SROS2, before the ROS node exists.
 *
 * Every telemetry worker shares this step. It does not open a node.
 */
class AppBootstrap
{
public:
    /// `opts` is read and updated in place, including `manager_cert_ok`.
    explicit AppBootstrap(Options& opts);

    /// Create the key directory, prepare the identity and arm SROS2.
    void run();

private:
    Options& opts_;

    void trace(const std::string& message) const;
    void prepare_identity();
    void arm_security();
};

} // namespace pgdemo::device
