#pragma once

#include "Options.hpp"
#include "Stream.hpp"

#include <pki/DeviceKey.hpp>

#include <pgdemo_msgs/msg/bye.hpp>
#include <pgdemo_msgs/msg/certificate.hpp>
#include <pgdemo_msgs/msg/challenge.hpp>
#include <pgdemo_msgs/msg/config.hpp>
#include <pgdemo_msgs/msg/config_applied.hpp>
#include <pgdemo_msgs/msg/crl.hpp>
#include <pgdemo_msgs/msg/csr.hpp>
#include <pgdemo_msgs/msg/error.hpp>
#include <pgdemo_msgs/msg/hello.hpp>
#include <pgdemo_msgs/msg/proof.hpp>
#include <pgdemo_msgs/msg/rejected.hpp>

#include <rclcpp/rclcpp.hpp>

#include <filesystem>
#include <memory>
#include <string>

namespace pgdemo::device {

/**
 * @brief Common ROS node of a worker.
 *
 * The handshake, the shared publishers and the sample clock live here. The
 * telemetry class owns its publisher and implements `publish_sample`. `hello`
 * is transient-local and repeated once a second until the challenge arrives,
 * so a manager that starts later still sees the node. A manager message whose
 * device name is not this worker's is ignored.
 */
class Worker : public rclcpp::Node
{
public:
    /// `generator` produces each sample. The derived class creates its publisher.
    Worker(Options opts, int& exit_code, std::unique_ptr<SampleGenerator> generator);
    /// Releases the node and the enrollment key.
    ~Worker() override;

    /// A node is not copied.
    Worker(const Worker&) = delete;
    /// A node is not copied.
    Worker& operator=(const Worker&) = delete;

protected:
    /// Publish one sample on this worker's telemetry topic.
    virtual void publish_sample(Sample sample) = 0;

    /**
     * @brief Log `line` about every 2 s.
     *
     * The counter counts samples, and `rate_ms` says how much time each one represents.
     */
    void trace_sample(const std::string& line);

    /// The arguments this worker was started with.
    [[nodiscard]] const Options& options() const noexcept { return opts_; }

    /// Reliable volatile queue of depth 16.
    [[nodiscard]] static rclcpp::QoS reliable_qos()
    {
        return rclcpp::QoS(rclcpp::KeepLast(16)).reliable();
    }

private:
    /// Where the handshake is. `Announce` repeats `hello` until `challenge`.
    enum class Phase { Announce, Cert, Challenge, Config, Run, Done };

    [[nodiscard]] std::filesystem::path key_path() const { return opts_.dir / "key.pem"; }
    [[nodiscard]] std::filesystem::path cert_path() const { return opts_.dir / "cert.pem"; }
    [[nodiscard]] std::filesystem::path factory_cert_path() const { return opts_.dir / "factory.crt"; }
    [[nodiscard]] std::string tag() const { return "device " + opts_.name; }
    [[nodiscard]] bool mine(const std::string& device) const { return device == opts_.name; }

    void publish_emission(SampleStream::Emission msg);
    void publish_error(const std::string& code, const std::string& detail);
    void fail(int code, const std::string& what);
    void announce();
    void send_csr();
    void store_certificate(const std::string& cert);
    void prove(const pgdemo_msgs::msg::Challenge& challenge);
    void arm_samples();
    void on_crl(const pgdemo_msgs::msg::Crl& msg);
    void on_bye(const pgdemo_msgs::msg::Bye& msg);
    void on_rejected(const pgdemo_msgs::msg::Rejected& msg);
    void on_certificate(const pgdemo_msgs::msg::Certificate& msg);
    void on_challenge(const pgdemo_msgs::msg::Challenge& msg);
    void publish_applied(const pgdemo_msgs::msg::Config& msg, bool ok, std::string code, std::string detail);
    void on_config(const pgdemo_msgs::msg::Config& msg);

    Options opts_;
    int& exit_code_;
    std::unique_ptr<SampleGenerator> generator_;
    SampleStream stream_;
    std::string own_serial_;
    Phase phase_{Phase::Announce};
    bool csr_sent_{false};
    bool hello_traced_{false};
    int samples_since_trace_{0};
    std::unique_ptr<pki::DeviceKey> next_key_;
    rclcpp::Publisher<pgdemo_msgs::msg::Hello>::SharedPtr hello_;
    rclcpp::Publisher<pgdemo_msgs::msg::Heartbeat>::SharedPtr heartbeat_;
    rclcpp::Publisher<pgdemo_msgs::msg::Error>::SharedPtr error_;
    rclcpp::Publisher<pgdemo_msgs::msg::Csr>::SharedPtr csr_;
    rclcpp::Publisher<pgdemo_msgs::msg::Proof>::SharedPtr proof_;
    rclcpp::Publisher<pgdemo_msgs::msg::ConfigApplied>::SharedPtr applied_;
    rclcpp::Subscription<pgdemo_msgs::msg::Bye>::SharedPtr bye_;
    rclcpp::Subscription<pgdemo_msgs::msg::Rejected>::SharedPtr rejected_;
    rclcpp::Subscription<pgdemo_msgs::msg::Certificate>::SharedPtr certificate_;
    rclcpp::Subscription<pgdemo_msgs::msg::Challenge>::SharedPtr challenge_;
    rclcpp::Subscription<pgdemo_msgs::msg::Config>::SharedPtr config_;
    rclcpp::Subscription<pgdemo_msgs::msg::Crl>::SharedPtr crl_;
    rclcpp::TimerBase::SharedPtr hello_timer_;
    rclcpp::TimerBase::SharedPtr sample_timer_;
};

} // namespace pgdemo::device
