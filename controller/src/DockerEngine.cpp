#include "DockerEngine.hpp"

#include <boost/json.hpp>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string_view>

namespace pgdemo::controller {

namespace {

constexpr std::string_view kApi = "/v1.43";

void send_all(int fd, std::string_view data)
{
    while (!data.empty())
    {
        const ssize_t n = ::send(fd, data.data(), data.size(), MSG_NOSIGNAL);
        if (n < 0)
        {
            if (errno == EINTR) continue;
            throw std::runtime_error(std::string("docker: cannot write: ") + std::strerror(errno));
        }
        data.remove_prefix(static_cast<std::size_t>(n));
    }
}

std::string unchunk(std::string_view raw)
{
    std::string out;
    while (!raw.empty())
    {
        const auto line = raw.find("\r\n");
        if (line == std::string_view::npos) break;
        const auto size = std::stoul(std::string(raw.substr(0, line)), nullptr, 16);
        raw.remove_prefix(line + 2);
        if (size == 0) break;
        if (raw.size() < size) throw std::runtime_error("docker: short chunk");
        out.append(raw.substr(0, size));
        raw.remove_prefix(size);
        if (raw.starts_with("\r\n")) raw.remove_prefix(2);
    }
    return out;
}

int dial(const std::string& socket)
{
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) throw std::runtime_error(std::string("docker: cannot open a socket: ") + std::strerror(errno));
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (socket.size() >= sizeof addr.sun_path)
    {
        ::close(fd);
        throw std::runtime_error("docker: socket path is too long");
    }
    std::char_traits<char>::copy(addr.sun_path, socket.c_str(), socket.size() + 1);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0)
    {
        const int err = errno;
        ::close(fd);
        throw std::runtime_error(std::string("docker: cannot connect to ") + socket + ": " + std::strerror(err));
    }
    return fd;
}

std::string recv_all(int fd)
{
    std::string out;
    char buf[4096];
    while (true)
    {
        const ssize_t n = ::recv(fd, buf, sizeof buf, 0);
        if (n < 0)
        {
            if (errno == EINTR) continue;
            throw std::runtime_error(std::string("docker: cannot read: ") + std::strerror(errno));
        }
        if (n == 0) break;
        out.append(buf, static_cast<std::size_t>(n));
        if (out.size() > 1024 * 1024) throw std::runtime_error("docker: reply too large");
    }
    return out;
}

} // namespace

DockerEngine::DockerEngine(std::string socket) : socket_(std::move(socket)) {}

DockerEngine::Reply DockerEngine::request(const std::string& method, const std::string& path, const std::string& body) const
{
    const int raw_fd = dial(socket_);
    const struct Guard
    {
        int fd;
        ~Guard() { ::close(fd); }
    } guard{raw_fd};
    std::string head = method + " " + path + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n";
    if (!body.empty())
        head += "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
    head += "\r\n";
    send_all(guard.fd, head);
    send_all(guard.fd, body);
    const std::string raw = recv_all(guard.fd);
    const auto split = raw.find("\r\n\r\n");
    if (split == std::string::npos) throw std::runtime_error("docker: truncated reply");
    const std::string headers = raw.substr(0, split);
    const auto line_end = headers.find("\r\n");
    const std::string status_line = headers.substr(0, line_end);
    const auto code_at = status_line.find(' ');
    if (code_at == std::string::npos) throw std::runtime_error("docker: bad status line");
    Reply reply;
    reply.status = std::stoi(status_line.substr(code_at + 1));
    reply.body = raw.substr(split + 4);
    if (headers.find("chunked") != std::string::npos) reply.body = unchunk(reply.body);
    return reply;
}

void DockerEngine::ensure_volume(const std::string& name)
{
    const auto reply = request("POST", std::string(kApi) + "/volumes/create", "{\"Name\":\"" + name + "\"}");
    if (reply.status == 201 || reply.status == 409) return;
    throw std::runtime_error("docker: cannot create volume " + name + ": " + reply.body);
}

std::string DockerEngine::mountpoint(const std::string& volume) const
{
    const auto reply = request("GET", std::string(kApi) + "/volumes/" + volume, "");
    if (reply.status != 200) throw std::runtime_error("docker: cannot inspect volume " + volume);
    const auto json = boost::json::parse(reply.body).as_object();
    const auto it = json.find("Mountpoint");
    if (it == json.end() || !it->value().is_string() || it->value().as_string().empty())
        throw std::runtime_error("docker: volume " + volume + " has no mountpoint");
    return std::string(it->value().as_string());
}

void DockerEngine::run(const std::string& name, const std::string& image, const std::vector<std::string>& cmd,
                       const std::vector<std::string>& env, const std::vector<std::string>& binds,
                       const std::string& network, const std::string& extra_network)
{
    const auto removed = request("DELETE", std::string(kApi) + "/containers/" + name + "?force=1", "");
    if (removed.status != 204 && removed.status != 404 && removed.status != 200)
        throw std::runtime_error("docker: cannot remove " + name + ": " + removed.body);
    boost::json::array cmd_json;
    for (const auto& arg : cmd) cmd_json.emplace_back(arg);
    boost::json::array env_json;
    for (const auto& item : env) env_json.emplace_back(item);
    boost::json::array binds_json;
    for (const auto& item : binds) binds_json.emplace_back(item);
    boost::json::object spec;
    spec["Image"] = image;
    spec["User"] = "0:0";
    spec["WorkingDir"] = "/app";
    spec["Entrypoint"] = boost::json::array{"/app/entrypoint.sh"};
    spec["Cmd"] = std::move(cmd_json);
    spec["Env"] = std::move(env_json);
    // Service `worker` is declared at scale 0. These containers carry no
    // config-hash, so Compose follows a start without creating or removing
    // them. container-number is the device name: `pgdemo-worker-<name>` is the
    // default Compose name, and the log prefix is `worker-<name>`.
    boost::json::object labels;
    labels["com.docker.compose.project"] = "pgdemo";
    labels["com.docker.compose.service"] = "worker";
    labels["com.docker.compose.oneoff"] = "False";
    constexpr std::string_view kWorkerPrefix = "pgdemo-worker-";
    if (name.starts_with(kWorkerPrefix))
        labels["com.docker.compose.container-number"] = name.substr(kWorkerPrefix.size());
    spec["Labels"] = std::move(labels);
    boost::json::object host;
    host["Init"] = true;
    host["Binds"] = std::move(binds_json);
    host["NetworkMode"] = network;
    spec["HostConfig"] = std::move(host);
    const auto created = request("POST", std::string(kApi) + "/containers/create?name=" + name, boost::json::serialize(spec));
    if (created.status != 201) throw std::runtime_error("docker: cannot create " + name + ": " + created.body);
    const auto id = std::string(boost::json::parse(created.body).as_object().at("Id").as_string());
    if (!extra_network.empty())
    {
        const auto joined = request("POST", std::string(kApi) + "/networks/" + extra_network + "/connect",
                                    "{\"Container\":\"" + id + "\"}");
        if (joined.status != 200 && joined.status != 201)
            throw std::runtime_error("docker: cannot attach " + name + " to " + extra_network + ": " + joined.body);
    }
    const auto started = request("POST", std::string(kApi) + "/containers/" + id + "/start", "");
    if (started.status != 204 && started.status != 304)
        throw std::runtime_error("docker: cannot start " + name + ": " + started.body);
}

void DockerEngine::stop(const std::string& name)
{
    const auto reply = request("POST", std::string(kApi) + "/containers/" + name + "/stop?t=5", "");
    if (reply.status == 204 || reply.status == 304 || reply.status == 404) return;
    throw std::runtime_error("docker: cannot stop " + name + ": " + reply.body);
}

bool DockerEngine::running(const std::string& name) const
{
    const auto reply = request("GET", std::string(kApi) + "/containers/" + name + "/json", "");
    if (reply.status == 404) return false;
    if (reply.status != 200) throw std::runtime_error("docker: cannot inspect " + name);
    const auto state = boost::json::parse(reply.body).as_object().at("State").as_object();
    return state.at("Running").as_bool();
}

int DockerEngine::exit_code(const std::string& name) const
{
    const auto reply = request("GET", std::string(kApi) + "/containers/" + name + "/json", "");
    if (reply.status != 200) return 1;
    const auto state = boost::json::parse(reply.body).as_object().at("State").as_object();
    if (state.at("Running").as_bool()) return 0;
    return static_cast<int>(state.at("ExitCode").as_int64());
}

} // namespace pgdemo::controller
