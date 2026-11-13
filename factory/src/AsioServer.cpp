#include "AsioServer.hpp"

#include <pki/Certificate.hpp>
#include <pki/FactoryChannel.hpp>

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
    std::string reply;
    try
    {
        const std::string text = co_await read_request(socket);
        const auto nl = text.find('\n');
        const std::string head = text.substr(0, nl);
        std::string body = nl == std::string::npos ? std::string{} : text.substr(nl + 1);
        if (body.ends_with(".\n")) body.resize(body.size() - 2);
        reply = pki::answer_factory(ca, head, body);
    }
    catch (const pki::PkiError& e)
    {
        reply = "ERR " + std::string(e.what()) + "\n";
    }
    catch (...)
    {
        reply = "ERR refused\n";
    }
    if (!reply.empty())
    {
        boost::system::error_code ec;
        co_await net::async_write(socket, net::buffer(reply), net::redirect_error(net::use_awaitable, ec));
    }
}

} // namespace

void serve_factory_asio(std::uint16_t port, const pki::CertificateAuthority& ca, const std::atomic<bool>& stop,
                        std::string_view listen_addr)
{
    net::io_context io;
    const tcp::endpoint endpoint(net::ip::make_address(std::string(listen_addr)), port);
    tcp::acceptor acceptor(io, endpoint);
    net::co_spawn(
        io,
        [&]() -> net::awaitable<void> {
            while (!stop.load())
            {
                tcp::socket socket = co_await acceptor.async_accept(net::use_awaitable);
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
}

} // namespace pgdemo::factory
