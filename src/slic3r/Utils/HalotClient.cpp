///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "HalotClient.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <optional>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/log/trivial.hpp>

#include "HalotProtocol.hpp"

namespace Slic3r { namespace halot {

namespace {

namespace asio      = boost::asio;
namespace beast     = boost::beast;
namespace websocket = boost::beast::websocket;
using tcp           = boost::asio::ip::tcp;
using Kind          = Error::Kind;

// "ip" or "ip:port", also accepting "http://" or "ws://" in front.
std::pair<std::string, std::string> split_host(std::string host)
{
    for (const char *scheme : {"http://", "https://", "ws://"})
        if (boost::istarts_with(host, scheme))
            host.erase(0, std::strlen(scheme));
    if (size_t slash = host.find('/'); slash != std::string::npos)
        host.erase(slash);
    if (size_t colon = host.rfind(':'); colon != std::string::npos)
        return {host.substr(0, colon), host.substr(colon + 1)};
    return {host, std::to_string(DefaultPort)};
}

// A WebSocket connection whose operations give up after a timeout.
class Connection
{
public:
    Connection(const std::string &host, std::chrono::milliseconds timeout) : m_timeout(timeout)
    {
        const auto [address, port] = split_host(host);

        tcp::resolver     resolver(m_ioc);
        beast::error_code ec;
        const auto        endpoints = resolver.resolve(address, port, ec);
        if (ec)
            throw Error(Kind::connection, host + ": " + ec.message());

        run([this, &endpoints](Done done) {
            asio::async_connect(m_ws.next_layer(), endpoints, [done](beast::error_code ec, const tcp::endpoint &) { done(ec); });
        }, m_timeout);
        run([this, &address, &port](Done done) { m_ws.async_handshake(address + ":" + port, "/", done); }, m_timeout);
    }

    ~Connection()
    {
        beast::error_code ec;
        m_ws.next_layer().close(ec);
    }

    void send_text(const std::string &text) { send(text, true); }
    void send_binary(const std::string &data) { send(data, false); }

    // Reads messages until one satisfies `wanted`. The printer also pushes
    // status reports on its own, which are skipped.
    Message wait_for(const std::function<bool(const Message &)> &wanted)
    {
        const auto deadline = std::chrono::steady_clock::now() + m_timeout;
        for (;;) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            if (left.count() <= 0)
                throw Error(Kind::timeout, "no answer");

            m_buffer.clear();
            run([this](Done done) { m_ws.async_read(m_buffer, [done](beast::error_code ec, std::size_t) { done(ec); }); }, left);

            const std::string text = beast::buffers_to_string(m_buffer.data());
            if (auto msg = parse(text); msg && m_ws.got_text() && wanted(*msg))
                return *msg;
            BOOST_LOG_TRIVIAL(debug) << "HALOT: skipped " << text.substr(0, 200);
        }
    }

    void close()
    {
        try {
            run([this](Done done) { m_ws.async_close(websocket::close_code::normal, done); }, m_timeout);
        } catch (const Error &) {
            // The work is done, a printer that doesn't close cleanly is fine.
        }
    }

private:
    using Done = std::function<void(beast::error_code)>;

    asio::io_context               m_ioc;
    websocket::stream<tcp::socket> m_ws{m_ioc};
    beast::flat_buffer             m_buffer;
    std::chrono::milliseconds      m_timeout;

    void send(const std::string &data, bool text)
    {
        m_ws.text(text);
        run([this, &data](Done done) {
            m_ws.async_write(asio::buffer(data), [done](beast::error_code ec, std::size_t) { done(ec); });
        }, m_timeout);
    }

    // Runs one asynchronous operation until it completes or `limit` passes.
    void run(const std::function<void(Done)> &start, std::chrono::milliseconds limit)
    {
        std::optional<beast::error_code> result;
        start([&result](beast::error_code ec) { result = ec; });

        m_ioc.restart();
        m_ioc.run_for(limit);
        if (!result) {
            // Closing the socket aborts the operation; let its handler run.
            beast::error_code ec;
            m_ws.next_layer().close(ec);
            m_ioc.restart();
            m_ioc.run();
            throw Error(Kind::timeout, "no answer");
        }
        if (*result)
            throw Error(Kind::connection, result->message());
    }
};

bool is_idle(const std::string &status)
{
    return status.empty() || status == "PRINT_GENERAL" || status == "PRINT_END" || status == "PRINT_COMPLETE";
}

// VERSION_CHECK, which HalotBox also sends first.
void check_version(Connection &c)
{
    c.send_text(version_check());
    c.wait_for([](const Message &m) { return !m.get("version").empty(); });
}

std::string print_status(Connection &c, const std::string &token)
{
    c.send_text(get_print_status(token));
    const std::string status = c.wait_for([](const Message &m) { return m.cmd == "GET_PRINT_STATUS"; }).get("printStatus");
    if (status == "TOKEN_ERROR")
        throw Error(Kind::password, "TOKEN_ERROR");
    return status;
}

size_t to_size(const std::string &s)
{
    try {
        return size_t(std::stoull(s));
    } catch (const std::exception &) {
        return 0;
    }
}

} // namespace

Client::Client(std::string host, std::string password, std::chrono::milliseconds timeout)
    : m_host(std::move(host)), m_password(std::move(password)), m_timeout(timeout)
{}

std::string Client::test() const
{
    try {
        Connection c(m_host, m_timeout);
        check_version(c);
        const std::string status = print_status(c, token(m_password));
        c.close();
        return status;
    } catch (const Error &) {
        throw;
    } catch (const std::exception &e) {
        // Anything unexpected still reaches the caller as an Error.
        throw Error(Kind::connection, e.what());
    }
}

bool Client::upload(const std::string &source, const std::string &filename, bool print,
                    const std::function<bool(std::size_t, std::size_t)> &progress) const
{
    try {
        return upload_file(source, filename, print, progress);
    } catch (const Error &) {
        throw;
    } catch (const std::exception &e) {
        // Anything unexpected still reaches the caller as an Error, so the
        // upload queue keeps running.
        throw Error(Kind::connection, e.what());
    }
}

bool Client::upload_file(const std::string &source, const std::string &filename, bool print,
                         const std::function<bool(std::size_t, std::size_t)> &progress) const
{
    std::ifstream in(source, std::ios::binary | std::ios::ate);
    const std::streamoff end = in ? std::streamoff(in.tellg()) : -1;
    if (end < 0)
        throw Error(Kind::file, source);
    const size_t size = size_t(end);

    std::string head(8, '\0');
    in.seekg(0);
    in.read(head.data(), std::streamsize(head.size()));
    head.resize(size_t(in.gcount()));
    in.clear();

    BOOST_LOG_TRIVIAL(info) << "HALOT: uploading " << filename << " (" << size << " bytes) to " << m_host
                            << (print ? " and printing" : "");

    const std::string tok = token(m_password);
    Connection        c(m_host, m_timeout);
    check_version(c);

    // Checked before sending, so a wrong password or a running print doesn't
    // cost a long upload.
    if (print && !is_idle(print_status(c, tok)))
        throw Error(Kind::busy, "printing");

    const std::string key = file_key(head);
    c.send_text(start_file(filename, size, key));
    const Message started = c.wait_for([](const Message &m) { return m.cmd == "START_FILE"; });

    // The printer may ask to resume an interrupted upload at `offset`.
    size_t     sent     = std::min(size, to_size(started.get("offset")));
    const bool compress = started.get("compress") == "true";
    BOOST_LOG_TRIVIAL(debug) << "HALOT: START_FILE offset " << sent << ", compress " << started.get("compress");

    std::string buffer;
    while (sent < size) {
        if (!progress(sent, size)) {
            c.send_text(cancel_file(filename, key));
            c.close();
            return false;
        }

        const size_t n = std::min(ChunkSize, size - sent);
        buffer.resize(n);
        in.seekg(std::streamoff(sent));
        in.read(buffer.data(), std::streamsize(n));
        if (size_t(in.gcount()) != n)
            throw Error(Kind::file, source);

        c.send_binary(chunk(buffer.data(), n, compress));

        const Message     ack   = c.wait_for([](const Message &m) { return m.cmd == "START_DATA"; });
        const std::string error = ack.get("errorcode");
        if (!error.empty() && error != "0")
            throw Error(Kind::refused, "START_DATA errorcode " + error);

        const size_t received = to_size(ack.get("received"));
        if (received <= sent)
            throw Error(Kind::refused, "START_DATA received " + ack.get("received") + " after " + std::to_string(sent));
        sent = std::min(received, size);
    }
    // The last chunk is in: a cancel now still keeps the print from starting.
    if (!progress(size, size)) {
        c.send_text(cancel_file(filename, key));
        c.close();
        return false;
    }

    // The R6 answers checkstate 1 once the file is stored.
    const std::string checkstate = c.wait_for([](const Message &m) { return m.cmd == "CHECK_DATA"; }).get("checkstate");
    BOOST_LOG_TRIVIAL(info) << "HALOT: " << filename << " stored, checkstate " << checkstate;
    if (checkstate != "1")
        throw Error(Kind::refused, "CHECK_DATA checkstate " + checkstate);

    if (print) {
        c.send_text(start_print(filename, tok));
        const std::string status = c.wait_for([](const Message &m) { return m.cmd == "START_PRINT"; }).get("status");
        if (status == "CHECK_FALSE")
            throw Error(Kind::refused, "START_PRINT CHECK_FALSE");
        BOOST_LOG_TRIVIAL(info) << "HALOT: print of " << filename << ": " << status;
    }

    c.close();
    return true;
}

std::vector<Printer> discover(std::chrono::milliseconds wait, const std::string &address, unsigned short port)
{
    using udp = asio::ip::udp;

    std::vector<Printer> found;
    asio::io_context     ioc;
    udp::socket          socket(ioc);
    beast::error_code    ec;

    socket.open(udp::v4(), ec);
    if (ec)
        return found;
    socket.set_option(udp::socket::reuse_address(true), ec);
    socket.set_option(asio::socket_base::broadcast(true), ec);
    // Listen where HalotBox does; if that port is taken, any port will do,
    // as the broadcast announces it.
    socket.bind(udp::endpoint(udp::v4(), ReplyPort), ec);
    if (ec) {
        ec.clear();
        socket.bind(udp::endpoint(udp::v4(), 0), ec);
        if (ec)
            return found;
    }

    const std::string   request = discovery_request(socket.local_endpoint().port());
    const udp::endpoint target(asio::ip::make_address(address, ec), port);
    if (ec)
        return found;

    std::array<char, 1024> buffer;
    udp::endpoint          from;
    std::function<void()>  receive = [&]() {
        socket.async_receive_from(asio::buffer(buffer), from, [&](beast::error_code ec, std::size_t n) {
            if (ec)
                return;
            auto printer = parse_discovery_reply(std::string(buffer.data(), n), from.address().to_string());
            if (printer && std::none_of(found.begin(), found.end(), [&](const Printer &p) { return p.ip == printer->ip; }))
                found.push_back(*printer);
            receive();
        });
    };
    receive();

    // UDP may drop a packet, so ask a few times while waiting.
    constexpr int rounds = 3;
    for (int i = 0; i < rounds; ++i) {
        socket.send_to(asio::buffer(request), target, 0, ec);
        ioc.restart();
        ioc.run_for(wait / rounds);
    }

    socket.close(ec);
    ioc.restart();
    ioc.run();

    BOOST_LOG_TRIVIAL(info) << "HALOT: discovery found " << found.size() << " printer(s)";
    return found;
}

}} // namespace Slic3r::halot
