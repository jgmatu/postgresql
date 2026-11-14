#include <pki/Handshake.hpp>

#include <pki/Certificate.hpp>

#include <botan/auto_rng.h>
#include <botan/certstor.h>
#include <botan/credentials_manager.h>
#include <botan/data_src.h>
#include <botan/pkcs8.h>
#include <botan/tls_client.h>
#include <botan/tls_policy.h>
#include <botan/tls_server.h>
#include <botan/tls_session_manager_noop.h>
#include <botan/x509cert.h>
#include <botan/x509path.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <csignal>
#include <cstring>
#include <memory>
#include <span>
#include <stdexcept>

namespace pgdemo::pki {

namespace {

void ignore_sigpipe()
{
    static const int once = [] {
        std::signal(SIGPIPE, SIG_IGN);
        return 0;
    }();
    (void)once;
}

void write_all(int fd, std::span<const uint8_t> data)
{
    std::size_t done = 0;
    while (done < data.size())
    {
        const ssize_t n = ::write(fd, data.data() + done, data.size() - done);
        if (n < 0)
        {
            if (errno == EINTR) continue;
            throw PkiError("CERT_CHAIN", std::string("handshake failed: ") + std::strerror(errno));
        }
        done += static_cast<std::size_t>(n);
    }
}

void deadlines(int fd)
{
    const timeval tv{10, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}

class Tls13Policy final : public Botan::TLS::Policy
{
public:
    bool allow_tls12() const override { return false; }

    bool acceptable_protocol_version(Botan::TLS::Protocol_Version version) const override
    {
        return version == Botan::TLS::Protocol_Version::TLS_V13;
    }

    Botan::TLS::Protocol_Version latest_supported_version(bool datagram) const override
    {
        if (datagram) return Botan::TLS::Protocol_Version::latest_dtls_version();
        return Botan::TLS::Protocol_Version::TLS_V13;
    }

    bool require_client_certificate_authentication() const override { return true; }

    std::size_t new_session_tickets_upon_handshake_success() const override { return 0; }

    bool support_cert_status_message() const override { return false; }
};

Botan::X509_Certificate load_cert(const std::string& pem)
{
    Botan::DataSource_Memory src(pem);
    return Botan::X509_Certificate(src);
}

class Identity final : public Botan::Credentials_Manager
{
public:
    Identity(const std::string& key_pem, const std::string& cert_pem, const std::string& root_pem)
        : leaf_(load_cert(cert_pem))
    {
        Botan::DataSource_Memory key_src(key_pem);
        key_ = Botan::PKCS8::load_key(key_src);
        if (!key_) throw std::runtime_error("TLS: cannot load the private key");
        store_.add_certificate(load_cert(root_pem));
    }

    std::vector<Botan::Certificate_Store*> trusted_certificate_authorities(const std::string&,
                                                                           const std::string&) override
    {
        return {&store_};
    }

    std::vector<Botan::X509_Certificate> find_cert_chain(const std::vector<std::string>& cert_key_types,
                                                         const std::vector<Botan::AlgorithmIdentifier>&,
                                                         const std::vector<Botan::X509_DN>&, const std::string&,
                                                         const std::string&) override
    {
        if (!cert_key_types.empty() &&
            std::find(cert_key_types.begin(), cert_key_types.end(), std::string("ECDSA")) == cert_key_types.end())
            return {};
        return {leaf_};
    }

    std::shared_ptr<Botan::Private_Key> private_key_for(const Botan::X509_Certificate&, const std::string&,
                                                        const std::string&) override
    {
        return key_;
    }

private:
    std::shared_ptr<Botan::Private_Key> key_;
    Botan::X509_Certificate leaf_;
    Botan::Certificate_Store_In_Memory store_;
};

class SocketCallbacks final : public Botan::TLS::Callbacks
{
public:
    explicit SocketCallbacks(int fd) : fd_(fd) {}

    void tls_emit_data(std::span<const uint8_t> data) override { write_all(fd_, data); }

    void tls_record_received(uint64_t, std::span<const uint8_t>) override {}

    void tls_alert(Botan::TLS::Alert alert) override
    {
        if (alert.is_fatal()) fatal_ = alert.type_string();
    }

    void tls_verify_cert_chain(const std::vector<Botan::X509_Certificate>& cert_chain,
                               const std::vector<std::optional<Botan::OCSP::Response>>&,
                               const std::vector<Botan::Certificate_Store*>& trusted_roots, Botan::Usage_Type usage,
                               std::string_view, const Botan::TLS::Policy&) override
    {
        if (cert_chain.empty() || trusted_roots.empty()) throw std::runtime_error("peer presented no certificate");
        const Botan::Path_Validation_Restrictions restrictions(false, 110);
        const auto result = Botan::x509_path_validate(cert_chain, restrictions, trusted_roots, "", usage);
        if (!result.successful_validation()) throw std::runtime_error(result.result_string());
    }

    [[nodiscard]] const std::string& fatal() const noexcept { return fatal_; }

private:
    int fd_{-1};
    std::string fatal_;
};

struct Materials
{
    std::shared_ptr<Botan::AutoSeeded_RNG> rng = std::make_shared<Botan::AutoSeeded_RNG>();
    std::shared_ptr<Botan::TLS::Policy> policy = std::make_shared<Tls13Policy>();
    std::shared_ptr<Botan::TLS::Session_Manager_Noop> sessions = std::make_shared<Botan::TLS::Session_Manager_Noop>();
    std::shared_ptr<Identity> identity;

    Materials(const std::string& key_pem, const std::string& cert_pem, const std::string& root_pem)
        : identity(std::make_shared<Identity>(key_pem, cert_pem, root_pem))
    {
    }
};

void pump(int fd, Botan::TLS::Channel& channel, const SocketCallbacks& callbacks)
{
    std::array<uint8_t, 4096> buf{};
    while (!channel.is_handshake_complete())
    {
        if (!callbacks.fatal().empty()) throw PkiError("CERT_CHAIN", "handshake failed: " + callbacks.fatal());
        const ssize_t n = ::recv(fd, buf.data(), buf.size(), 0);
        if (n < 0)
        {
            if (errno == EINTR) continue;
            throw PkiError("CERT_CHAIN", std::string("handshake failed: ") + std::strerror(errno));
        }
        if (n == 0) throw PkiError("CERT_CHAIN", "handshake failed: connection closed");
        try
        {
            channel.received_data(buf.data(), static_cast<std::size_t>(n));
        }
        catch (const PkiError&)
        {
            throw;
        }
        catch (const std::exception& e)
        {
            throw PkiError("CERT_CHAIN", std::string("handshake failed: ") + e.what());
        }
    }
    if (!callbacks.fatal().empty()) throw PkiError("CERT_CHAIN", "handshake failed: " + callbacks.fatal());
}

HandshakePeer peer_of(Botan::TLS::Channel& channel, std::string_view expected_cn)
{
    const auto chain = channel.peer_cert_chain();
    if (chain.empty()) throw PkiError("CERT_CHAIN", "peer presented no certificate");
    const auto& cert = chain.front();
    HandshakePeer peer{cert.subject_dn().get_first_attribute("CN"), cert.PEM_encode()};
    if (!expected_cn.empty() && peer.common_name != expected_cn)
        throw PkiError("CERT_SUBJECT", "peer CN '" + peer.common_name + "' is not '" + std::string(expected_cn) + "'");
    return peer;
}

void shutdown_quietly(Botan::TLS::Channel& channel)
{
    try
    {
        channel.close();
    }
    catch (const std::exception&)
    {
    }
}

} // namespace

struct HandshakeListener::Ctx
{
    Materials materials;
    explicit Ctx(const std::string& key_pem, const std::string& cert_pem, const std::string& root_pem)
        : materials(key_pem, cert_pem, root_pem)
    {
    }
};

HandshakeListener::HandshakeListener(const std::string& key_pem, const std::string& cert_pem, const std::string& root_pem)
    : ctx_((ignore_sigpipe(), new Ctx(key_pem, cert_pem, root_pem)))
{
    fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd_ < 0)
    {
        delete ctx_;
        throw std::runtime_error("TLS: cannot open a socket");
    }
    const int yes = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);
    if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || ::listen(fd_, 16) != 0)
    {
        ::close(fd_);
        delete ctx_;
        throw std::runtime_error("TLS: cannot listen on 127.0.0.1");
    }
    sockaddr_in bound{};
    socklen_t len = sizeof bound;
    if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&bound), &len) != 0)
    {
        ::close(fd_);
        delete ctx_;
        throw std::runtime_error("TLS: cannot read the bound port");
    }
    port_ = ntohs(bound.sin_port);
}

HandshakeListener::~HandshakeListener()
{
    if (fd_ >= 0) ::close(fd_);
    delete ctx_;
}

bool HandshakeListener::wait(std::chrono::milliseconds timeout) const
{
    pollfd pfd{fd_, POLLIN, 0};
    const int rc = ::poll(&pfd, 1, static_cast<int>(timeout.count()));
    return rc > 0 && (pfd.revents & POLLIN) != 0;
}

HandshakePeer HandshakeListener::accept_one()
{
    sockaddr_in peer{};
    socklen_t len = sizeof peer;
    const int client = ::accept(fd_, reinterpret_cast<sockaddr*>(&peer), &len);
    if (client < 0) throw PkiError("CERT_CHAIN", std::string("accept failed: ") + std::strerror(errno));
    deadlines(client);
    try
    {
        auto callbacks = std::make_shared<SocketCallbacks>(client);
        const auto& m = ctx_->materials;
        Botan::TLS::Server server(callbacks, m.sessions, m.identity, m.policy, m.rng);
        pump(client, server, *callbacks);
        auto presented = peer_of(server, {});
        shutdown_quietly(server);
        ::close(client);
        return presented;
    }
    catch (...)
    {
        ::close(client);
        throw;
    }
}

HandshakePeer handshake_connect(const std::string& host, std::uint16_t port, const std::string& key_pem,
                                const std::string& cert_pem, const std::string& root_pem, std::string_view expected_cn)
{
    ignore_sigpipe();
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) throw std::runtime_error("TLS: cannot open a socket");
    deadlines(fd);
    try
    {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1)
            throw std::runtime_error("TLS: " + host + " is not an IPv4 address");
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0)
            throw PkiError("CERT_CHAIN", std::string("cannot connect: ") + std::strerror(errno));

        Materials materials(key_pem, cert_pem, root_pem);
        auto callbacks = std::make_shared<SocketCallbacks>(fd);
        Botan::TLS::Client client(callbacks, materials.sessions, materials.identity, materials.policy, materials.rng,
                                  Botan::TLS::Server_Information(), Botan::TLS::Protocol_Version::TLS_V13);
        pump(fd, client, *callbacks);
        auto presented = peer_of(client, expected_cn);
        shutdown_quietly(client);
        ::close(fd);
        return presented;
    }
    catch (...)
    {
        ::close(fd);
        throw;
    }
}

} // namespace pgdemo::pki
