#include "AsioServer.hpp"

#include <factory/FactoryChannel.hpp>
#include <pki/Certificate.hpp>

#include <log/Log.hpp>

#include <boost/asio.hpp>

#include <chrono>
#include <functional>
#include <stdexcept>
#include <string>

namespace pgdemo::factory {

namespace net = boost::asio;
using tcp = net::ip::tcp;

namespace {

constexpr std::size_t kMax = 64 * 1024;
constexpr std::string_view kLog = "factory";

std::string peer_of(const tcp::socket& socket)
{
    boost::system::error_code ec;
    const auto endpoint = socket.remote_endpoint(ec);
    if (ec) return "unknown";
    return endpoint.address().to_string() + ":" + std::to_string(endpoint.port());
}

/// The first line of a reply, plus the issued serial when the body is a certificate.
std::string outcome_of(std::string_view head, std::string_view reply)
{
    const auto nl = reply.find('\n');
    const std::string status(reply.substr(0, nl == std::string_view::npos ? reply.size() : nl));
    if (!status.starts_with("OK")) return status;
    if (head == "SIGN governance") return "OK governance";
    constexpr std::string_view kPermissions = "SIGN permissions ";
    if (head.starts_with(kPermissions)) return "OK permissions " + std::string(head.substr(kPermissions.size()));
    const auto begin = reply.find("-----BEGIN");
    if (begin == std::string_view::npos) return "OK " + std::string(head);
    const auto end = reply.find("-----END", begin);
    if (end == std::string_view::npos) return "OK " + std::string(head);
    const auto tail = reply.find('\n', end);
    const auto pem = reply.substr(begin, (tail == std::string_view::npos ? reply.size() : tail + 1) - begin);
    try
    {
        const auto info = pki::inspect_certificate(pem);
        return "OK " + std::string(head) + " serial " + info.serial;
    }
    catch (const std::exception&)
    {
        return "OK " + std::string(head);
    }
}

net::awaitable<std::string> read_request(tcp::socket& socket)
{
    net::streambuf buf;
    std::string text;
    while (text.find("\n.\n") == std::string::npos)
    {
        if (text.size() > kMax) throw std::runtime_error("factory: request too large");
        net::steady_timer timer(co_await net::this_coro::executor);
        timer.expires_after(std::chrono::seconds(5));
        timer.async_wait([&socket](const boost::system::error_code& ec) {
            if (!ec) socket.cancel();
        });
        co_await net::async_read_until(socket, buf, '\n', net::use_awaitable);
        timer.cancel();
        std::istream in(&buf);
        std::string line;
        std::getline(in, line);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        text += line;
        text += '\n';
        if (line == ".") break;
    }
    co_return text;
}

net::awaitable<void> session(tcp::socket socket, const pki::CertificateAuthority& ca)
{
    const std::string peer = peer_of(socket);
    std::string head;
    std::string reply;
    bool probe = false;
    try
    {
        const std::string text = co_await read_request(socket);
        const auto nl = text.find('\n');
        head = text.substr(0, nl);
        log::info(kLog, peer + ": " + head);
        std::string body = nl == std::string::npos ? std::string{} : text.substr(nl + 1);
        if (body.ends_with(".\n")) body.resize(body.size() - 2);
        reply = answer_factory(ca, head, body);
        log::info(kLog, peer + ": " + outcome_of(head, reply));
    }
    catch (const pki::PkiError& e)
    {
        reply = "ERR " + std::string(e.what()) + "\n";
        log::warn(kLog, peer + ": " + reply.substr(0, reply.size() - 1));
    }
    catch (const boost::system::system_error& e)
    {
        // The healthcheck opens the port and closes it without a request.
        probe = head.empty() &&
            (e.code() == net::error::eof || e.code() == net::error::operation_aborted);
        if (!probe)
        {
            reply = "ERR refused\n";
            const std::string why = e.code() == net::error::operation_aborted ? "timed out" : e.code().message();
            const std::string where = head.empty() ? std::string{} : head + ": ";
            log::warn(kLog, peer + ": " + where + why);
        }
    }
    catch (const std::exception& e)
    {
        reply = "ERR refused\n";
        log::warn(kLog, peer + ": " + e.what());
    }
    if (!reply.empty())
    {
        boost::system::error_code ec;
        co_await net::async_write(socket, net::buffer(reply),
            net::redirect_error(net::use_awaitable, ec));
        if (ec && ec != net::error::eof)
            log::warn(kLog, peer + ": reply not sent: " + ec.message());
    }
    if (!probe) log::info(kLog, peer + ": closed");
}

} // namespace

void serve_factory_asio(std::uint16_t port, const pki::CertificateAuthority& ca, const std::atomic<bool>& stop,
                        std::string_view listen_addr)
{
    net::io_context io;
    const tcp::endpoint endpoint(net::ip::make_address(std::string(listen_addr)), port);
    tcp::acceptor acceptor(io, endpoint);
    log::info(kLog, "listening on " + endpoint.address().to_string() + ":" + std::to_string(endpoint.port()));
    net::co_spawn(
        io,
        [&]() -> net::awaitable<void> {
            while (!stop.load())
            {
                boost::system::error_code ec;
                tcp::socket socket = co_await acceptor.async_accept(net::redirect_error(net::use_awaitable, ec));
                if (ec)
                {
                    if (!stop.load()) log::warn(kLog, std::string("accept: ") + ec.message());
                    co_return;
                }
                net::co_spawn(io, session(std::move(socket), ca), net::detached);
            }
        },
        net::detached);
    net::steady_timer timer(io);
    std::function<void(const boost::system::error_code&)> arm;
    arm = [&](const boost::system::error_code&) {
        if (stop.load())
        {
            boost::system::error_code ec;
            acceptor.cancel(ec);
            return;
        }
        timer.expires_after(std::chrono::milliseconds(200));
        timer.async_wait(arm);
    };
    arm({});
    io.run();
    log::info(kLog, "stopped");
}

} // namespace pgdemo::factory
