#pragma once

#include "Worker.hpp"

#include <pgdemo/Topics.hpp>
#include <pgdemo_msgs/msg/xy_sample.hpp>

#include <format>
#include <memory>
#include <utility>

namespace pgdemo::device {

/// XY telemetry. A scatter in the square on `pgdemo/worker/telemetry/xy`.
class XyWorker : public Worker
{
public:
    /// Scatter in the square of half-extent `amplitude`.
    XyWorker(Options opts, int& exit_code)
        : Worker(std::move(opts), exit_code, std::make_unique<Scatter>())
    {
        xy_ = create_publisher<pgdemo_msgs::msg::XySample>(topics::kXy, reliable_qos());
    }

protected:
    void publish_sample(Sample sample) override
    {
        auto msg = std::get<pgdemo_msgs::msg::XySample>(std::move(sample));
        msg.device = options().name;
        xy_->publish(msg);
        trace_sample(std::format("sample seq {} x {:.6g} y {:.6g}", msg.seq, msg.x, msg.y));
    }

private:
    rclcpp::Publisher<pgdemo_msgs::msg::XySample>::SharedPtr xy_;
};

} // namespace pgdemo::device
