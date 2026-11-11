#pragma once

#include <memory>
#include <thread>
#include <vector>

#include <pgdemo_msgs/msg/bye.hpp>
#include <pgdemo_msgs/msg/certificate.hpp>
#include <pgdemo_msgs/msg/challenge.hpp>
#include <pgdemo_msgs/msg/config.hpp>
#include <pgdemo_msgs/msg/rejected.hpp>

#include <rclcpp/rclcpp.hpp>

#include "Protocol.hpp"

namespace pgdemo::controller {

struct SignalState;
class Fleet;

/**
 * @brief The `pgdemo_controller` node.
 *
 * Owns the `pgdemo/manager` publishers, the `pgdemo/worker` subscriptions,
 * the 100 ms wall timer and the `sigwait` thread. `hello` calls `Fleet::on_hello`;
 * every other worker topic calls `Fleet::on_uplink`. `main` keeps the options,
 * the database and `rclcpp::spin`.
 */
class Manager
{
public:
    /// `rclcpp::init` with signal shutdown disabled, then the node and its publishers.
    Manager(int argc, char** argv);
    ~Manager();

    Manager(const Manager&) = delete;
    Manager& operator=(const Manager&) = delete;

    /// Publishers captured by the downlink. The spin thread is the one that calls them.
    [[nodiscard]] Downlink downlink() const;

    /// One subscription per worker topic. Before `Fleet::boot()`.
    void subscribe(Fleet& fleet);

    /// Start the `sigwait` thread and the 100 ms timer. After `Fleet::boot()`.
    void arm(SignalState& signals);

    [[nodiscard]] rclcpp::Node::SharedPtr node() const { return node_; }

private:
    rclcpp::Node::SharedPtr node_;
    rclcpp::Publisher<pgdemo_msgs::msg::Bye>::SharedPtr bye_;
    rclcpp::Publisher<pgdemo_msgs::msg::Certificate>::SharedPtr certificate_;
    rclcpp::Publisher<pgdemo_msgs::msg::Rejected>::SharedPtr rejected_;
    rclcpp::Publisher<pgdemo_msgs::msg::Challenge>::SharedPtr challenge_;
    rclcpp::Publisher<pgdemo_msgs::msg::Config>::SharedPtr config_;
    std::vector<rclcpp::SubscriptionBase::SharedPtr> subscriptions_;
    rclcpp::TimerBase::SharedPtr tick_;
    SignalState* signals_{nullptr};
    Fleet* fleet_{nullptr};
    /// Last, so it is joined after `~Manager` has set `signals_->done`.
    std::jthread signal_thread_;
};

} // namespace pgdemo::controller
