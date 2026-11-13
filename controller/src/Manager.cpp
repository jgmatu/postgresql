#include "Manager.hpp"

#include "Fleet.hpp"

#include <pgdemo/Topics.hpp>

#include <pgdemo_msgs/msg/config_applied.hpp>
#include <pgdemo_msgs/msg/csr.hpp>
#include <pgdemo_msgs/msg/error.hpp>
#include <pgdemo_msgs/msg/gps_sample.hpp>
#include <pgdemo_msgs/msg/heartbeat.hpp>
#include <pgdemo_msgs/msg/hello.hpp>
#include <pgdemo_msgs/msg/proof.hpp>
#include <pgdemo_msgs/msg/scalar_sample.hpp>
#include <pgdemo_msgs/msg/xy_sample.hpp>
#include <pgdemo_msgs/msg/xyz_sample.hpp>

#include <csignal>
#include <ctime>

namespace pgdemo::controller {

namespace topics = ::pgdemo::topics;

Manager::Manager(int argc, char** argv)
{
    // rclcpp must not take SIGINT: sigwait owns shutdown.
    rclcpp::InitOptions ros_init;
    ros_init.shutdown_on_signal = false;
    rclcpp::init(argc, argv, ros_init);
    node_ = std::make_shared<rclcpp::Node>("pgdemo_controller");
    const auto qos = rclcpp::QoS(rclcpp::KeepLast(16)).reliable();
    // One drain publishes every pending configuration. KeepLast(16) drops the
    // ones the worker has not taken, so a burst of 25 never all land.
    const auto config_qos = rclcpp::QoS(rclcpp::KeepLast(64)).reliable();
    namespace msg = pgdemo_msgs::msg;
    bye_ = node_->create_publisher<msg::Bye>(topics::kBye, qos);
    certificate_ = node_->create_publisher<msg::Certificate>(topics::kCertificate, qos);
    rejected_ = node_->create_publisher<msg::Rejected>(topics::kRejected, qos);
    challenge_ = node_->create_publisher<msg::Challenge>(topics::kChallenge, qos);
    config_ = node_->create_publisher<msg::Config>(topics::kConfig, config_qos);
    crl_ = node_->create_publisher<msg::Crl>(topics::kCrl, qos);
}

Manager::~Manager()
{
    if (signals_ != nullptr) signals_->done.store(true);
}

Downlink Manager::downlink() const
{
    Downlink out;
    out.bye = [pub = bye_](pgdemo_msgs::msg::Bye m) { pub->publish(m); };
    out.certificate = [pub = certificate_](pgdemo_msgs::msg::Certificate m) { pub->publish(m); };
    out.rejected = [pub = rejected_](pgdemo_msgs::msg::Rejected m) { pub->publish(m); };
    out.challenge = [pub = challenge_](pgdemo_msgs::msg::Challenge m) { pub->publish(m); };
    out.config = [pub = config_](pgdemo_msgs::msg::Config m) { pub->publish(m); };
    out.crl = [pub = crl_](pgdemo_msgs::msg::Crl m) { pub->publish(m); };
    return out;
}

void Manager::subscribe(Fleet& fleet)
{
    fleet_ = &fleet;
    const auto qos = rclcpp::QoS(rclcpp::KeepLast(16)).reliable();
    const auto config_qos = rclcpp::QoS(rclcpp::KeepLast(64)).reliable();
    const auto hello_qos = rclcpp::QoS(rclcpp::KeepLast(1)).reliable().transient_local();
    namespace msg = pgdemo_msgs::msg;
    using topics::kConfigApplied;
    using topics::kCsr;
    using topics::kError;
    using topics::kGps;
    using topics::kHeartbeat;
    using topics::kHello;
    using topics::kProof;
    using topics::kScalar;
    using topics::kXy;
    using topics::kXyz;
    subscriptions_.push_back(node_->create_subscription<msg::Hello>(
        kHello, hello_qos, [&fleet](msg::Hello::ConstSharedPtr m) { fleet.on_hello(*m); }));
    subscriptions_.push_back(node_->create_subscription<msg::Heartbeat>(
        kHeartbeat, qos, [&fleet](msg::Heartbeat::ConstSharedPtr m) { fleet.on_uplink(*m); }));
    subscriptions_.push_back(node_->create_subscription<msg::Error>(
        kError, qos, [&fleet](msg::Error::ConstSharedPtr m) { fleet.on_uplink(*m); }));
    subscriptions_.push_back(node_->create_subscription<msg::Csr>(
        kCsr, qos, [&fleet](msg::Csr::ConstSharedPtr m) { fleet.on_uplink(*m); }));
    subscriptions_.push_back(node_->create_subscription<msg::Proof>(
        kProof, qos, [&fleet](msg::Proof::ConstSharedPtr m) { fleet.on_uplink(*m); }));
    subscriptions_.push_back(node_->create_subscription<msg::ConfigApplied>(
        kConfigApplied, config_qos, [&fleet](msg::ConfigApplied::ConstSharedPtr m) { fleet.on_uplink(*m); }));
    subscriptions_.push_back(node_->create_subscription<msg::ScalarSample>(
        kScalar, qos, [&fleet](msg::ScalarSample::ConstSharedPtr m) { fleet.on_uplink(*m); }));
    subscriptions_.push_back(node_->create_subscription<msg::GpsSample>(
        kGps, qos, [&fleet](msg::GpsSample::ConstSharedPtr m) { fleet.on_uplink(*m); }));
    subscriptions_.push_back(node_->create_subscription<msg::XySample>(
        kXy, qos, [&fleet](msg::XySample::ConstSharedPtr m) { fleet.on_uplink(*m); }));
    subscriptions_.push_back(node_->create_subscription<msg::XyzSample>(
        kXyz, qos, [&fleet](msg::XyzSample::ConstSharedPtr m) { fleet.on_uplink(*m); }));
}

void Manager::arm(SignalState& signals)
{
    signals_ = &signals;
    // Signals stay blocked. This thread is the only one that receives them.
    signal_thread_ = std::jthread([this] {
        sigset_t set;
        sigemptyset(&set);
        sigaddset(&set, SIGINT);
        sigaddset(&set, SIGTERM);
        sigaddset(&set, SIGCHLD);
        while (!signals_->done.load())
        {
            timespec ts{};
            ts.tv_sec = 0;
            ts.tv_nsec = 200000000;
            const int sig = ::sigtimedwait(&set, nullptr, &ts);
            if (sig == SIGCHLD) signals_->child.store(true);
            else if (sig == SIGINT || sig == SIGTERM) signals_->stop.store(sig);
        }
    });
    tick_ = node_->create_wall_timer(std::chrono::milliseconds(100), [this] {
        if (fleet_ != nullptr) fleet_->tick();
    });
}

} // namespace pgdemo::controller
