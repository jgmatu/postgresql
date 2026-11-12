#include <pki/Handshake.hpp>

#include <pki/Certificate.hpp>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <csignal>
#include <climits>
#include <cstring>
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

[[noreturn]] void tls_fail(const std::string& what)
{
    const unsigned long err = ERR_get_error();
    if (err == 0) throw PkiError("CERT_CHAIN", what);
    char buf[256];
    ERR_error_string_n(err, buf, sizeof buf);
    throw PkiError("CERT_CHAIN", what + ": " + buf);
}

BIO* mem(const std::string& pem)
{
    if (pem.size() > static_cast<std::size_t>(INT_MAX)) throw std::runtime_error("TLS: PEM too large");
    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    if (bio == nullptr) throw std::runtime_error("TLS: out of memory");
    return bio;
}

SSL_CTX* make_ctx(const std::string& key_pem, const std::string& cert_pem, const std::string& root_pem)
{
    SSL_CTX* ctx = SSL_CTX_new(TLS_method());
    if (ctx == nullptr) tls_fail("cannot create a TLS context");
    if (SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION) != 1 ||
        SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION) != 1)
    {
        SSL_CTX_free(ctx);
        tls_fail("cannot require TLS 1.3");
    }
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, nullptr);

    BIO* root_bio = mem(root_pem);
    X509* root = PEM_read_bio_X509(root_bio, nullptr, nullptr, nullptr);
    BIO_free(root_bio);
    if (root == nullptr || X509_STORE_add_cert(SSL_CTX_get_cert_store(ctx), root) != 1)
    {
        X509_free(root);
        SSL_CTX_free(ctx);
        tls_fail("cannot load the factory root");
    }
    X509_free(root);

    BIO* cert_bio = mem(cert_pem);
    X509* cert = PEM_read_bio_X509(cert_bio, nullptr, nullptr, nullptr);
    BIO_free(cert_bio);
    if (cert == nullptr || SSL_CTX_use_certificate(ctx, cert) != 1)
    {
        X509_free(cert);
        SSL_CTX_free(ctx);
        tls_fail("cannot load the certificate");
    }
    X509_free(cert);

    BIO* key_bio = mem(key_pem);
    EVP_PKEY* key = PEM_read_bio_PrivateKey(key_bio, nullptr, nullptr, nullptr);
    BIO_free(key_bio);
    if (key == nullptr || SSL_CTX_use_PrivateKey(ctx, key) != 1 || SSL_CTX_check_private_key(ctx) != 1)
    {
        EVP_PKEY_free(key);
        SSL_CTX_free(ctx);
        tls_fail("cannot load the private key");
    }
    EVP_PKEY_free(key);
    return ctx;
}

void deadlines(int fd)
{
    const timeval tv{10, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}

HandshakePeer peer_of(SSL* ssl, std::string_view expected_cn)
{
    X509* cert = SSL_get1_peer_certificate(ssl);
    if (cert == nullptr) throw PkiError("CERT_CHAIN", "peer presented no certificate");
    char cn[256];
    const int n = X509_NAME_get_text_by_NID(X509_get_subject_name(cert), NID_commonName, cn, sizeof cn);
    BIO* out = BIO_new(BIO_s_mem());
    if (n < 0 || out == nullptr || PEM_write_bio_X509(out, cert) != 1)
    {
        BIO_free(out);
        X509_free(cert);
        throw PkiError("CERT_MALFORMED", "peer certificate cannot be read");
    }
    X509_free(cert);
    char* data = nullptr;
    const long len = BIO_get_mem_data(out, &data);
    HandshakePeer peer{n > 0 ? std::string(cn, static_cast<std::size_t>(n)) : std::string{},
                       std::string(data, static_cast<std::size_t>(len))};
    BIO_free(out);
    if (!expected_cn.empty() && peer.common_name != expected_cn)
        throw PkiError("CERT_SUBJECT", "peer CN '" + peer.common_name + "' is not '" + std::string(expected_cn) + "'");
    return peer;
}

struct Session
{
    SSL* ssl{nullptr};
    explicit Session(SSL_CTX* ctx) : ssl(SSL_new(ctx))
    {
        if (ssl == nullptr) tls_fail("cannot create a TLS session");
    }
    ~Session() { SSL_free(ssl); }
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
};

} // namespace

struct HandshakeListener::Ctx
{
    SSL_CTX* ssl;
};

HandshakeListener::HandshakeListener(const std::string& key_pem, const std::string& cert_pem, const std::string& root_pem)
    : ctx_((ignore_sigpipe(), new Ctx{make_ctx(key_pem, cert_pem, root_pem)}))
{
    fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd_ < 0)
    {
        SSL_CTX_free(ctx_->ssl);
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
        SSL_CTX_free(ctx_->ssl);
        delete ctx_;
        throw std::runtime_error("TLS: cannot listen on 127.0.0.1");
    }
    sockaddr_in bound{};
    socklen_t len = sizeof bound;
    if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&bound), &len) != 0)
    {
        ::close(fd_);
        SSL_CTX_free(ctx_->ssl);
        delete ctx_;
        throw std::runtime_error("TLS: cannot read the bound port");
    }
    port_ = ntohs(bound.sin_port);
}

HandshakeListener::~HandshakeListener()
{
    if (fd_ >= 0) ::close(fd_);
    if (ctx_ != nullptr)
    {
        SSL_CTX_free(ctx_->ssl);
        delete ctx_;
    }
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
    if (client < 0) tls_fail("accept failed");
    deadlines(client);
    Session session(ctx_->ssl);
    SSL_set_fd(session.ssl, client);
    if (SSL_accept(session.ssl) != 1)
    {
        ::close(client);
        tls_fail("handshake failed");
    }
    try
    {
        auto presented = peer_of(session.ssl, {});
        SSL_shutdown(session.ssl);
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
    SSL_CTX* ctx = make_ctx(key_pem, cert_pem, root_pem);
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
    {
        SSL_CTX_free(ctx);
        throw std::runtime_error("TLS: cannot open a socket");
    }
    deadlines(fd);
    try
    {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1)
            throw std::runtime_error("TLS: " + host + " is not an IPv4 address");
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) tls_fail("cannot connect");
        HandshakePeer presented;
        {
            Session session(ctx);
            SSL_set_fd(session.ssl, fd);
            if (SSL_connect(session.ssl) != 1) tls_fail("handshake failed");
            presented = peer_of(session.ssl, expected_cn);
            SSL_shutdown(session.ssl);
        }
        ::close(fd);
        SSL_CTX_free(ctx);
        return presented;
    }
    catch (...)
    {
        ::close(fd);
        SSL_CTX_free(ctx);
        throw;
    }
}

} // namespace pgdemo::pki
