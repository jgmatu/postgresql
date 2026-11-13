#pragma once

#include "Worker.hpp"

#include <pgdemo/Topics.hpp>
#include <pgdemo_msgs/msg/scalar_sample.hpp>

#include <format>
#include <memory>
#include <utility>

namespace pgdemo::device {

/// Scalar telemetry. The generator is `Sine`, `Sawtooth` or `WhiteNoise`.
class ScalarWorker : public Worker
{
public:
    /// `generator` is the scalar signal this device type uses.
    ScalarWorker(Options opts, int& exit_code, std::unique_ptr<SampleGenerator> generator)
        : Worker(std::move(opts), exit_code, std::move(generator))
    {
        scalar_ = create_publisher<pgdemo_msgs::msg::ScalarSample>(topics::kScalar, reliable_qos());
    }

protected:
    void publish_sample(Sample sample) override
    {
        auto msg = std::get<pgdemo_msgs::msg::ScalarSample>(std::move(sample));
        msg.device = options().name;
        scalar_->publish(msg);
        trace_sample(std::format("sample seq {} value {:.6g}", msg.seq, msg.value));
    }

private:
    rclcpp::Publisher<pgdemo_msgs::msg::ScalarSample>::SharedPtr scalar_;
};

} // namespace pgdemo::device
