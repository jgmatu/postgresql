#include <factory/FactoryChannel.hpp>

#include <factory/SecurityPolicy.hpp>
#include <pki/Certificate.hpp>
#include <pki/CertificateAuthority.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>

namespace pgdemo::factory {

namespace {

constexpr std::size_t kMaxMessage = 64 * 1024;

void deadlines(int fd)
{
    const timeval tv{5, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}

void write_all(int fd, std::string_view data)
{
    std::size_t done = 0;
    while (done < data.size())
    {
        const ssize_t n = ::write(fd, data.data() + done, data.size() - done);
        if (n < 0)
        {
            if (errno == EINTR) continue;
            throw std::runtime_error(std::string("factory: cannot write: ") + std::strerror(errno));
        }
        done += static_cast<std::size_t>(n);
    }
}

std::string read_some(int fd, std::string& pending)
{
    char buf[4096];
    const ssize_t n = ::recv(fd, buf, sizeof buf, 0);
    if (n < 0)
    {
        if (errno == EINTR) return {};
        throw std::runtime_error(std::string("factory: cannot read: ") + std::strerror(errno));
    }
    if (n == 0) throw std::runtime_error("factory: connection closed");
    if (pending.size() + static_cast<std::size_t>(n) > kMaxMessage)
        throw std::runtime_error("factory: message too large");
    pending.append(buf, static_cast<std::size_t>(n));
    return {};
}

std::string next_line(int fd, std::string& pending)
{
    while (true)
    {
        const auto cut = pending.find('\n');
        if (cut != std::string::npos)
        {
            std::string line = pending.substr(0, cut);
            pending.erase(0, cut + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return line;
        }
        read_some(fd, pending);
    }
}

std::string read_pem(int fd, std::string& pending)
{
    std::string pem;
    while (true)
    {
        const std::string line = next_line(fd, pending);
        if (line == ".") return pem;
        pem += line;
        pem += '\n';
    }
}

int connect_to(const std::string& host, std::uint16_t port)
{
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) throw std::runtime_error("factory: cannot open a socket");
    deadlines(fd);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1)
    {
        ::close(fd);
        throw std::runtime_error("factory: " + host + " is not an IPv4 address");
    }
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0)
    {
        const int err = errno;
        ::close(fd);
        throw std::runtime_error(std::string("factory: cannot connect: ") + std::strerror(err));
    }
    return fd;
}

void serve_one(int client, const pki::CertificateAuthority& ca)
{
    deadlines(client);
    std::string pending;
    const std::string head = next_line(client, pending);
    const bool has_body = head == "SIGN governance" || head.starts_with("SIGN ");
    const std::string body = has_body ? read_pem(client, pending) : std::string{};
    write_all(client, answer_factory(ca, head, body));
}

} // namespace

std::string request_signed_document(const std::string& host, std::uint16_t port, std::string_view kind,
                                      std::string_view cn, std::string_view document)
{
    const int fd = connect_to(host, port);
    try
    {
        std::string body(document);
        if (body.empty() || body.back() != '\n') body.push_back('\n');
        std::string head = "SIGN " + std::string(kind);
        if (kind == "permissions") head += " " + std::string(cn);
        write_all(fd, head + "\n" + body + ".\n");
        std::string pending;
        const std::string status = next_line(fd, pending);
        if (status.starts_with("ERR")) throw std::runtime_error("factory: " + status);
        if (status != "OK") throw std::runtime_error("factory: unexpected reply");
        const std::string smime = read_pem(fd, pending);
        ::close(fd);
        return smime;
    }
    catch (...)
    {
        ::close(fd);
        throw;
    }
}

std::string request_signature(const std::string& host, std::uint16_t port, std::string_view role, std::string_view cn,
                              std::string_view presented_pem)
{
    const int fd = connect_to(host, port);
    try
    {
        std::string body(presented_pem);
        if (body.empty() || body.back() != '\n') body.push_back('\n');
        write_all(fd, "SIGN " + std::string(role) + " " + std::string(cn) + "\n" + body + ".\n");
        std::string pending;
        const std::string status = next_line(fd, pending);
        if (status.starts_with("ERR")) throw std::runtime_error("factory: " + status);
        if (status != "OK") throw std::runtime_error("factory: unexpected reply");
        const std::string pem = read_pem(fd, pending);
        ::close(fd);
        return pem;
    }
    catch (...)
    {
        ::close(fd);
        throw;
    }
}

std::string answer_factory(const pki::CertificateAuthority& ca, std::string_view head, std::string_view body)
{
    auto signed_reply = [&](std::string_view document) {
        std::string smime = ca.sign_document(document);
        if (smime.empty() || smime.back() != '\n') smime.push_back('\n');
        return "OK\n" + smime + ".\n";
    };
    if (head == "SIGN governance")
    {
        if (body != governance_document())
            throw pki::PkiError("CERT_SUBJECT", "governance document is not the template");
        return signed_reply(body);
    }
    constexpr std::string_view kPermissions = "SIGN permissions ";
    if (head.starts_with(kPermissions))
    {
        const std::string cn(head.substr(kPermissions.size()));
        if (body != permissions_document(cn))
            throw pki::PkiError("CERT_SUBJECT", "permissions document is not the template");
        return signed_reply(body);
    }
    const auto role_at = head.find(' ');
    const auto cn_at = role_at == std::string_view::npos ? std::string_view::npos : head.find(' ', role_at + 1);
    if (head.starts_with("SIGN ") && role_at != std::string_view::npos && cn_at != std::string_view::npos)
    {
        const auto role = head.substr(role_at + 1, cn_at - role_at - 1);
        const auto cn = head.substr(cn_at + 1);
        if (role != "worker" && role != "manager") return "ERR role must be worker or manager\n";
        const auto use = role == "manager" ? pki::CertificateAuthority::PresentedUse::Manager
                                            : pki::CertificateAuthority::PresentedUse::Worker;
        const auto issued = ca.sign_presented(std::string(body), std::string(cn), use);
        return "OK\n" + issued.pem + ".\n";
    }
    return "ERR expected SIGN worker|manager name, SIGN governance, or SIGN permissions name\n";
}

void serve_factory(std::uint16_t port, const pki::CertificateAuthority& ca, const std::atomic<bool>& stop,
                   std::atomic<std::uint16_t>* bound, std::string_view listen_addr)
{
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) throw std::runtime_error("factory: cannot open a socket");
    const int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    const std::string host(listen_addr);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1)
    {
        ::close(fd);
        throw std::runtime_error("factory: bad listen address " + host);
    }
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || ::listen(fd, 16) != 0)
    {
        ::close(fd);
        throw std::runtime_error(std::string("factory: cannot listen: ") + std::strerror(errno));
    }
    if (bound != nullptr)
    {
        sockaddr_in got{};
        socklen_t got_len = sizeof got;
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&got), &got_len) != 0)
        {
            ::close(fd);
            throw std::runtime_error("factory: cannot read the bound port");
        }
        bound->store(ntohs(got.sin_port));
    }
    while (!stop.load())
    {
        pollfd pfd{fd, POLLIN, 0};
        const int rc = ::poll(&pfd, 1, 200);
        if (rc <= 0) continue;
        const int client = ::accept(fd, nullptr, nullptr);
        if (client < 0) continue;
        try
        {
            serve_one(client, ca);
        }
        catch (const pki::PkiError& e)
        {
            try
            {
                write_all(client, "ERR " + std::string(e.what()) + "\n");
            }
            catch (...)
            {
            }
        }
        catch (...)
        {
            try
            {
                write_all(client, "ERR refused\n");
            }
            catch (...)
            {
            }
        }
        ::close(client);
    }
    ::close(fd);
}

} // namespace pgdemo::factory
