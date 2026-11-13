#pragma once

#include "Worker.hpp"

#include <pgdemo/Topics.hpp>
#include <pgdemo_msgs/msg/xyz_sample.hpp>

#include <format>
#include <memory>
#include <utility>

namespace pgdemo::device {

/// XYZ telemetry. A cloud in the cube on `pgdemo/worker/telemetry/xyz`.
class XyzWorker : public Worker
{
public:
    /// Cloud in the cube of half-extent `amplitude`.
    XyzWorker(Options opts, int& exit_code)
        : Worker(std::move(opts), exit_code, std::make_unique<Cloud>())
    {
        xyz_ = create_publisher<pgdemo_msgs::msg::XyzSample>(topics::kXyz, reliable_qos());
    }

protected:
    void publish_sample(Sample sample) override
    {
        auto msg = std::get<pgdemo_msgs::msg::XyzSample>(std::move(sample));
        msg.device = options().name;
        xyz_->publish(msg);
        trace_sample(std::format("sample seq {} x {:.6g} y {:.6g} z {:.6g}", msg.seq, msg.x, msg.y, msg.z));
    }

private:
    rclcpp::Publisher<pgdemo_msgs::msg::XyzSample>::SharedPtr xyz_;
};

} // namespace pgdemo::device
