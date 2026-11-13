#pragma once

#include <string>
#include <vector>

namespace pgdemo::controller {

/// Docker Engine API over `/var/run/docker.sock`. Creates one worker container
/// and the volume that holds its PUF seed. The controller does not mount that volume.
class DockerEngine
{
public:
    explicit DockerEngine(std::string socket = "/var/run/docker.sock");

    void ensure_volume(const std::string& name);
    /// Host path of a volume's data directory, from the engine.
    [[nodiscard]] std::string mountpoint(const std::string& volume) const;

    /// Remove a previous container with this name, create it and start it.
    void run(const std::string& name, const std::string& image, const std::vector<std::string>& cmd,
             const std::vector<std::string>& env, const std::vector<std::string>& binds, const std::string& network,
             const std::string& extra_network);

    void stop(const std::string& name);
    /// False when the container is missing or has exited.
    [[nodiscard]] bool running(const std::string& name) const;
    /// Exit code of a stopped container, or 1 when the engine does not say.
    [[nodiscard]] int exit_code(const std::string& name) const;

private:
    struct Reply
    {
        int status{0};
        std::string body;
    };
    [[nodiscard]] Reply request(const std::string& method, const std::string& path, const std::string& body) const;

    std::string socket_;
};

} // namespace pgdemo::controller
