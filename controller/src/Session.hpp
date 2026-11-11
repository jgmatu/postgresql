#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <sys/types.h>

#include <db/DatabaseManager.hpp>

#include "Protocol.hpp"

namespace pgdemo::controller {

class ProcessTable;
class TelemetryWriter;

/// Where a session is in its handshake. The spin thread advances it.
enum class Phase {
    WaitCsr,   ///< enroll or rotate: the next uplink must be the CSR
    WaitCert,  ///< CSR is stored; waiting for the CA
    WaitProof, ///< challenge is out; the next uplink must be the proof
    Online,    ///< authenticated and streaming
};

/// What one worker session needs from the rest of the controller.
struct SessionDeps
{
    ProcessTable& table;
    db::DatabaseManager& db;
    TelemetryWriter& writer;
    /// The one uplink that arrived before this session existed.
    std::map<std::string, Uplink>& early;
};

/**
 * @brief Handshake and online samples of one worker.
 *
 * The phase decides which uplink is valid. A CSR, a proof and the move to
 * online write the database at once. Samples go to `TelemetryWriter`.
 */
class Session
{
public:
    /// `downlink` publishes manager messages for this worker.
    Session(std::string name, Downlink downlink);

    /// Publish `bye` for this worker. Dropped once `close()`d.
    void send_bye(std::string reason);
    /// Publish the issued certificate.
    void send_certificate(std::string pem);
    /// Publish a refusal from the CA.
    void send_rejected(std::string code, std::string detail);
    /// Publish a 32-byte challenge nonce.
    void send_challenge(const std::array<unsigned char, 32>& bytes);
    /// Publish the configuration in force. `request_id` is 0 for the type defaults.
    void send_config(const db::AppliedConfig& config);
    /// Forward one desired request. The worker validates it and answers on `config/applied`.
    void send_desired(const db::ConfigRequest& request);
    /// Further publishes are dropped.
    void close();
    /// True after `close()`.
    [[nodiscard]] bool closed() const noexcept { return closed_; }

    /// Open a session for `hello`, or remember it until the slot exists.
    static void admit(SessionDeps& deps, pgdemo_msgs::msg::Hello hello);

    /// First step after `admit` has attached this session to its slot.
    void begin(SessionDeps& deps, const pgdemo_msgs::msg::Hello& hello);

    /// One worker message for the current phase.
    void handle(SessionDeps& deps, Uplink msg);

    /// The CA decided the enrollment this session is waiting on.
    void accept_certificate(SessionDeps& deps, std::int64_t request, db::EnrollmentOutcome outcome);

    /// The phase deadline has passed.
    void expire(SessionDeps& deps, const std::string& name);

    /// When the worker last sent anything; a silent one is dropped by `tick()`.
    std::chrono::steady_clock::time_point last_frame;
    /// Authenticated and streaming: configuration changes go out through `send_config()`.
    bool online{false};
    /// Handshake step the next uplink belongs to.
    Phase phase{Phase::WaitProof};
    /// When `phase` gives up. Empty while `Online`.
    std::optional<std::chrono::steady_clock::time_point> deadline;
    /// Challenge nonce, valid for this session only.
    std::array<unsigned char, 32> nonce{};
    /// Uplink that arrived while the session was waiting for the CA. At most 64.
    std::deque<Uplink> inbox;
    /// A decision that arrived before the CSR was stored.
    std::optional<db::EnrollmentOutcome> pending_outcome;
    /// Device the samples belong to, fixed when the session goes online.
    int device_id{0};
    db::Payload payload{db::Payload::Scalar};
    /// Pid at admission. A later `SIGTERM` is sent only when the slot still has this pid.
    pid_t opened_pid{0};

private:
    std::string name_;
    Downlink downlink_;
    bool closed_{false};
};

/// Expire stale requests and publish the rest to each online session.
void forward_configs(SessionDeps& deps);

} // namespace pgdemo::controller
