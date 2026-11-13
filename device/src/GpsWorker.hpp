#pragma once

#include "Worker.hpp"

#include <pgdemo/Topics.hpp>
#include <pgdemo_msgs/msg/gps_sample.hpp>

#include <format>
#include <memory>
#include <utility>

namespace pgdemo::device {

/// GPS telemetry. One orbit around Madrid on `pgdemo/worker/telemetry/gps`.
class GpsWorker : public Worker
{
public:
    /// Orbit of radius `amplitude` metres around Madrid.
    GpsWorker(Options opts, int& exit_code)
        : Worker(std::move(opts), exit_code, std::make_unique<GpsOrbit>())
    {
        gps_ = create_publisher<pgdemo_msgs::msg::GpsSample>(topics::kGps, reliable_qos());
    }

protected:
    void publish_sample(Sample sample) override
    {
        auto msg = std::get<pgdemo_msgs::msg::GpsSample>(std::move(sample));
        msg.device = options().name;
        gps_->publish(msg);
        trace_sample(std::format("sample seq {} radius_m {:.6g} bearing_rad {:.6g}", msg.seq, msg.radius_m, msg.bearing_rad));
    }

private:
    rclcpp::Publisher<pgdemo_msgs::msg::GpsSample>::SharedPtr gps_;
};

} // namespace pgdemo::device
