#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <algorithm>
#include <string>

#include <zlib.h>

#include "slic3r/Utils/HalotProtocol.hpp"

using namespace Slic3r;

namespace {

std::string from_hex(const std::string &hex)
{
    std::string out;
    for (size_t i = 0; i + 1 < hex.size(); i += 2)
        out.push_back(char(std::stoi(hex.substr(i, 2), nullptr, 16)));
    return out;
}

std::string to_hex(const std::string &bytes)
{
    static const char *digits = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : bytes) {
        out.push_back(digits[c >> 4]);
        out.push_back(digits[c & 15]);
    }
    return out;
}

} // namespace

TEST_CASE("DES matches the FIPS test vector", "[halot]") {
    CHECK(to_hex(halot::des_ecb_encrypt(from_hex("133457799BBCDFF1"), from_hex("0123456789ABCDEF"))) ==
          "85E813540F0AB405");
}

TEST_CASE("Token matches the one HALOT printers expect", "[halot]") {
    // Reference tokens from the open-source HALOT clients (same DES key as HalotBox).
    CHECK(halot::token("0") == "kpnyFvFnCSI=");
    CHECK(halot::token("123456") == "4ynhNQtA12U=");
    CHECK(halot::token("88888888") == "S04WtOGiRGCsR7ACiBVuXA==");
    // HalotBox uses 123456 when no password is set.
    CHECK(halot::token("") == "4ynhNQtA12U=");
}

TEST_CASE("File key is bytes 4-7 as a signed little-endian int in hex", "[halot]") {
    CHECK(halot::file_key(std::string("\x00\x00\x00\x00\x3c\xcf\xeb\x19", 8)) == "19ebcf3c");
    CHECK(halot::file_key(std::string("abcd\xff\xff\xff\xff", 8)) == "-1");
    CHECK(halot::file_key(std::string("abcd\x00\x00\x00\x80", 8)) == "-80000000");
    CHECK(halot::file_key("abc") == "0"); // too short to have a key
}

TEST_CASE("Chunks carry the length and a zlib stream, or the raw bytes", "[halot]") {
    std::string data;
    for (int i = 0; i < 100000; ++i) data.push_back(char(i % 7));

    const std::string framed = halot::chunk(data.data(), data.size(), true);
    REQUIRE(framed.size() > 4);
    const uint32_t n = (uint32_t(uint8_t(framed[0])) << 24) | (uint32_t(uint8_t(framed[1])) << 16) |
                       (uint32_t(uint8_t(framed[2])) << 8) | uint32_t(uint8_t(framed[3]));
    CHECK(n == data.size());

    std::string inflated(n, '\0');
    uLongf out_len = n;
    REQUIRE(uncompress(reinterpret_cast<Bytef *>(inflated.data()), &out_len,
                       reinterpret_cast<const Bytef *>(framed.data() + 4), uLong(framed.size() - 4)) == Z_OK);
    CHECK(out_len == n);
    CHECK(inflated == data);

    CHECK(halot::chunk(data.data(), data.size(), false) == data);
}

TEST_CASE("Commands are JSON with string values", "[halot]") {
    auto msg = halot::parse(halot::start_file("peça.cxdlpv4", 538539, "19ebcf3c"));
    REQUIRE(msg);
    CHECK(msg->cmd == "START_FILE");
    CHECK(msg->get("filename") == "peça.cxdlpv4");
    CHECK(msg->get("size") == "538539");
    CHECK(msg->get("offset") == "0");
    CHECK(msg->get("key") == "19ebcf3c");
    CHECK(msg->get("token").empty()); // HalotBox sends no token with START_FILE

    msg = halot::parse(halot::start_print("a.cxdlpv4", "tok"));
    REQUIRE(msg);
    CHECK(msg->cmd == "START_PRINT");
    CHECK(msg->get("filename") == "a.cxdlpv4");
    CHECK(msg->get("token") == "tok");

    CHECK(halot::parse(halot::version_check())->cmd == "VERSION_CHECK");
    CHECK(halot::parse(halot::get_print_status("tok"))->get("token") == "tok");
    CHECK(halot::parse(halot::cancel_file("a.cxdlpv4", "k"))->cmd == "CANCEL_FILE");
}

TEST_CASE("Printer replies are parsed, garbage is rejected", "[halot]") {
    auto msg = halot::parse(R"({"cmd":"START_FILE","compress":true,"filename":"a","key":"k","offset":"10","size":"20"})");
    REQUIRE(msg);
    CHECK(msg->get("compress") == "true");
    CHECK(msg->get("offset") == "10");

    msg = halot::parse(R"({"version":"1"})");
    REQUIRE(msg);
    CHECK(msg->cmd.empty());
    CHECK(msg->get("version") == "1");

    CHECK(!halot::parse("not json"));
}

// ---------------------------------------------------------------------------
// CrealityHalot host against a fake HALOT printer
// ---------------------------------------------------------------------------

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <random>
#include <thread>
#include <vector>

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/filesystem.hpp>

#include "slic3r/Utils/HalotClient.hpp"
#include "libslic3r/Utils.hpp"

namespace {

namespace asio      = boost::asio;
namespace beast     = boost::beast;
namespace websocket = boost::beast::websocket;
namespace fs        = boost::filesystem;
using tcp           = boost::asio::ip::tcp;

// Plays a HALOT printer on 127.0.0.1, following what HalotBox and the R6 do.
class FakeHalot
{
public:
    std::string password = "123456";
    bool        compress = true;
    bool        busy     = false; // printing
    bool        silent   = false; // never answers
    bool        chatty   = false; // pushes status messages before each answer
    std::string checkstate = "1"; // what the R6 answers after storing a file

    std::mutex               mutex;
    std::vector<std::string> commands; // cmd of every text message received
    std::string              file;     // reassembled upload
    std::string              print_filename, print_token;

    FakeHalot() : m_acceptor(m_ioc, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0))
    {
        m_thread = std::thread([this] { serve(); });
    }

    ~FakeHalot()
    {
        m_stop = true;
        // Wake up the blocking accept.
        try {
            asio::io_context ioc;
            tcp::socket s(ioc);
            s.connect(m_acceptor.local_endpoint());
        } catch (...) {}
        m_thread.join();
    }

    std::string host() const { return "127.0.0.1:" + std::to_string(m_acceptor.local_endpoint().port()); }

    bool saw(const std::string &cmd)
    {
        std::lock_guard<std::mutex> lk(mutex);
        return std::find(commands.begin(), commands.end(), cmd) != commands.end();
    }

private:
    asio::io_context  m_ioc;
    tcp::acceptor     m_acceptor;
    std::thread       m_thread;
    std::atomic<bool> m_stop{false};

    void serve()
    {
        while (!m_stop) {
            tcp::socket socket(m_ioc);
            boost::system::error_code ec;
            m_acceptor.accept(socket, ec);
            if (ec || m_stop)
                break;
            try {
                session(std::move(socket));
            } catch (...) {} // client closed or cancelled
        }
    }

    void session(tcp::socket socket)
    {
        websocket::stream<tcp::socket> ws(std::move(socket));
        ws.accept();

        size_t expected = 0;
        for (;;) {
            beast::flat_buffer buffer;
            ws.read(buffer);
            const std::string data = beast::buffers_to_string(buffer.data());

            if (silent)
                continue;

            auto reply = [&ws, this](const std::string &json) {
                if (chatty) {
                    ws.text(true);
                    ws.write(asio::buffer(std::string(R"({"cmd":"REPORT_PRINT_STATUS","printStatus":"PRINT_GENERAL"})")));
                }
                ws.text(true);
                ws.write(asio::buffer(json));
            };

            if (!ws.got_text()) {
                std::string payload = data;
                if (compress) {
                    if (payload.size() <= 4)
                        throw std::runtime_error("short chunk");
                    const uLongf n = (uLongf(uint8_t(payload[0])) << 24) | (uLongf(uint8_t(payload[1])) << 16) |
                                     (uLongf(uint8_t(payload[2])) << 8) | uLongf(uint8_t(payload[3]));
                    std::string inflated(n, '\0');
                    uLongf out = n;
                    if (uncompress(reinterpret_cast<Bytef *>(inflated.data()), &out,
                                   reinterpret_cast<const Bytef *>(payload.data() + 4), uLong(payload.size() - 4)) != Z_OK)
                        throw std::runtime_error("bad zlib chunk");
                    payload = inflated;
                }
                size_t received;
                {
                    std::lock_guard<std::mutex> lk(mutex);
                    file += payload;
                    received = file.size();
                }
                reply(R"({"cmd":"START_DATA","errorcode":0,"key":"k","received":")" + std::to_string(received) +
                      R"(","size":")" + std::to_string(expected) + "\"}");
                if (received == expected)
                    reply(R"({"checkstate":)" + checkstate + R"(,"cmd":"CHECK_DATA","key":"k"})");
                continue;
            }

            auto msg = halot::parse(data);
            if (!msg)
                throw std::runtime_error("bad json");
            {
                std::lock_guard<std::mutex> lk(mutex);
                commands.push_back(msg->cmd.empty() ? "?" : msg->cmd);
            }

            const bool token_ok = msg->get("token") == halot::token(password);
            if (msg->cmd == "VERSION_CHECK")
                reply(R"({"version":"1"})");
            else if (msg->cmd == "GET_PRINT_STATUS")
                reply(std::string(R"({"cmd":"GET_PRINT_STATUS","printStatus":")") +
                      (!token_ok ? "TOKEN_ERROR" : busy ? "PRINT_PROCESSING" : "PRINT_GENERAL") + "\"}");
            else if (msg->cmd == "START_FILE") {
                expected = std::stoul(msg->get("size"));
                reply(std::string(R"({"cmd":"START_FILE","compress":)") + (compress ? "true" : "false") +
                      R"(,"filename":")" + msg->get("filename") + R"(","key":"k","offset":"0","size":")" +
                      msg->get("size") + "\"}");
            } else if (msg->cmd == "START_PRINT") {
                {
                    std::lock_guard<std::mutex> lk(mutex);
                    print_filename = msg->get("filename");
                    print_token    = msg->get("token");
                }
                reply(std::string(R"({"cmd":"START_PRINT","filename":")") + msg->get("filename") +
                      R"(","status":")" + (token_ok ? "STARTED" : "CHECK_FALSE") + "\"}");
            }
            // CANCEL_FILE: just recorded.
        }
    }
};

// A file of `size` pseudo random bytes in a temporary folder.
fs::path random_file(size_t size)
{
    fs::path p = fs::temp_directory_path() / fs::unique_path("halot-%%%%-%%%%.cxdlpv4");
    std::mt19937 rng(42);
    std::string data(size, '\0');
    for (char &c : data) c = char(rng() % 8); // compressible, like layer images
    std::ofstream(p.string(), std::ios::binary) << data;
    return p;
}

std::string read_all(const fs::path &p)
{
    std::ifstream in(p.string(), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

struct UploadResult
{
    bool                              ok = false;
    std::optional<halot::Error::Kind> error;
};

UploadResult upload(const halot::Client &client, const fs::path &file, bool print,
                    const std::function<bool(size_t, size_t)> &progress = [](size_t, size_t) { return true; })
{
    UploadResult r;
    try {
        r.ok = client.upload(file.string(), "cubo teste.cxdlpv4", print, progress);
    } catch (const halot::Error &e) {
        r.error = e.kind;
    }
    return r;
}

std::optional<halot::Error::Kind> test_error(const halot::Client &client)
{
    try {
        client.test();
    } catch (const halot::Error &e) {
        return e.kind;
    }
    return std::nullopt;
}

const auto ShortTimeout = std::chrono::milliseconds(500);

using Kind = halot::Error::Kind;

} // namespace

TEST_CASE("Test connection checks version and password", "[halot]") {
    FakeHalot printer;
    CHECK(halot::Client(printer.host(), "123456", ShortTimeout).test() == "PRINT_GENERAL");
    CHECK(printer.saw("VERSION_CHECK"));
    CHECK(printer.saw("GET_PRINT_STATUS"));

    CHECK(test_error(halot::Client(printer.host(), "000000", ShortTimeout)) == Kind::password);
    // Empty means HalotBox's default password.
    CHECK(!test_error(halot::Client(printer.host(), "", ShortTimeout)));
}

TEST_CASE("Upload sends the whole file and does not print", "[halot]") {
    FakeHalot printer;
    printer.chatty = true; // status pushes are skipped
    const fs::path file = random_file(3 * halot::ChunkSize + 12345);

    size_t last_sent = 0, total = 0;
    UploadResult r = upload(halot::Client(printer.host(), "123456", ShortTimeout), file, false,
                            [&](size_t sent, size_t t) { last_sent = sent; total = t; return true; });
    CHECK(!r.error);
    CHECK(r.ok);
    CHECK(printer.file == read_all(file));
    CHECK(total == fs::file_size(file));
    CHECK(last_sent == total);
    CHECK(!printer.saw("START_PRINT"));
    fs::remove(file);
}

TEST_CASE("Upload and print starts the file with the token", "[halot]") {
    FakeHalot printer;
    const fs::path file = random_file(50000);

    UploadResult r = upload(halot::Client(printer.host(), "123456", ShortTimeout), file, true);
    CHECK(!r.error);
    CHECK(r.ok);
    CHECK(printer.print_filename == "cubo teste.cxdlpv4");
    CHECK(printer.print_token == halot::token("123456"));
    fs::remove(file);
}

TEST_CASE("Upload sends raw chunks when the printer does not want compression", "[halot]") {
    FakeHalot printer;
    printer.compress = false;
    const fs::path file = random_file(halot::ChunkSize + 7);

    UploadResult r = upload(halot::Client(printer.host(), "123456", ShortTimeout), file, false);
    CHECK(r.ok);
    CHECK(printer.file == read_all(file));
    fs::remove(file);
}

TEST_CASE("Cancelling an upload tells the printer and returns false", "[halot]") {
    FakeHalot printer;
    const fs::path file = random_file(3 * halot::ChunkSize);

    UploadResult r = upload(halot::Client(printer.host(), "123456", ShortTimeout), file, false,
                            [](size_t sent, size_t) { return sent < halot::ChunkSize; }); // cancel after one chunk
    CHECK(!r.error);
    CHECK(!r.ok);
    // The printer gets CANCEL_FILE (the server records it asynchronously).
    for (int i = 0; i < 100 && !printer.saw("CANCEL_FILE"); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(printer.saw("CANCEL_FILE"));
    CHECK(printer.file.size() == halot::ChunkSize);
    fs::remove(file);
}

TEST_CASE("Cancelling at the last chunk does not print", "[halot]") {
    FakeHalot printer;
    const fs::path file = random_file(1000); // one chunk: the whole upload is the last chunk

    UploadResult r = upload(halot::Client(printer.host(), "123456", ShortTimeout), file, true,
                            [](size_t sent, size_t total) { return sent < total; }); // cancel when done
    CHECK(!r.error);
    CHECK(!r.ok);
    for (int i = 0; i < 100 && !printer.saw("CANCEL_FILE"); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(printer.saw("CANCEL_FILE"));
    CHECK(!printer.saw("START_PRINT"));
    fs::remove(file);
}

TEST_CASE("A printer that stops answering times out", "[halot]") {
    FakeHalot printer;
    printer.silent = true;
    const fs::path file = random_file(1000);

    const auto start = std::chrono::steady_clock::now();
    UploadResult r = upload(halot::Client(printer.host(), "123456", ShortTimeout), file, false);
    const auto took = std::chrono::steady_clock::now() - start;

    CHECK(r.error == Kind::timeout);
    CHECK(took < 4 * ShortTimeout);
    fs::remove(file);
}

TEST_CASE("Nothing listening is a connection error", "[halot]") {
    std::string host;
    {
        FakeHalot printer; // take a free port, then close it
        host = printer.host();
    }
    CHECK(test_error(halot::Client(host, "123456", ShortTimeout)) == Kind::connection);
}

TEST_CASE("Upload and print refuses a busy printer before sending", "[halot]") {
    FakeHalot printer;
    printer.busy = true;
    const fs::path file = random_file(1000);

    UploadResult r = upload(halot::Client(printer.host(), "123456", ShortTimeout), file, true);
    CHECK(r.error == Kind::busy);
    CHECK(!printer.saw("START_FILE"));

    // Uploading without printing is fine while it prints.
    r = upload(halot::Client(printer.host(), "123456", ShortTimeout), file, false);
    CHECK(r.ok);
    fs::remove(file);
}

TEST_CASE("Upload and print with a wrong password fails before sending", "[halot]") {
    FakeHalot printer;
    const fs::path file = random_file(1000);

    UploadResult r = upload(halot::Client(printer.host(), "999999", ShortTimeout), file, true);
    CHECK(r.error == Kind::password);
    CHECK(!printer.saw("START_FILE"));
    fs::remove(file);
}

TEST_CASE("A file the printer fails to check is an error and is not printed", "[halot]") {
    FakeHalot printer;
    printer.checkstate = "0";
    const fs::path file = random_file(1000);

    UploadResult r = upload(halot::Client(printer.host(), "123456", ShortTimeout), file, true);
    CHECK(r.error == Kind::refused);
    CHECK(!printer.saw("START_PRINT"));
    fs::remove(file);
}

TEST_CASE("A missing file is reported before connecting", "[halot]") {
    FakeHalot printer;
    UploadResult r = upload(halot::Client(printer.host(), "123456", ShortTimeout), "/nonexistent/x.cxdlpv4", false);
    CHECK(r.error == Kind::file);
    CHECK(!printer.saw("VERSION_CHECK"));
}

TEST_CASE("A folder instead of a file is a file error", "[halot]") {
    FakeHalot printer;
    const fs::path dir = fs::temp_directory_path() / fs::unique_path("halot-dir-%%%%");
    fs::create_directories(dir);

    UploadResult r = upload(halot::Client(printer.host(), "123456", ShortTimeout), dir, false);
    CHECK(r.error == Kind::file);
    fs::remove(dir);
}

// ---------------------------------------------------------------------------
// Finding HALOT printers on the network
// ---------------------------------------------------------------------------

namespace {

using udp = boost::asio::ip::udp;

// Answers HalotBox's discovery broadcast like the R6 does: to the port the
// broadcast announces, with "20001", serial, model and three more fields.
class FakeHalotResponder
{
public:
    explicit FakeHalotResponder(std::string serial)
        : m_serial(std::move(serial)), m_socket(m_ioc, udp::endpoint(asio::ip::make_address("127.0.0.1"), 0))
    {
        m_thread = std::thread([this] { serve(); });
    }

    ~FakeHalotResponder()
    {
        m_stop = true;
        boost::system::error_code ec;
        udp::socket s(m_ioc);
        s.open(udp::v4(), ec);
        s.send_to(asio::buffer(std::string("stop")), m_socket.local_endpoint(), 0, ec);
        m_thread.join();
    }

    unsigned short port() const { return m_socket.local_endpoint().port(); }
    int            requests() const { return m_requests; }

private:
    std::string       m_serial;
    asio::io_context  m_ioc;
    udp::socket       m_socket;
    std::thread       m_thread;
    std::atomic<bool> m_stop{false};
    std::atomic<int>  m_requests{0};

    void serve()
    {
        char buf[256];
        while (!m_stop) {
            udp::endpoint             from;
            boost::system::error_code ec;
            const size_t n = m_socket.receive_from(asio::buffer(buf), from, 0, ec);
            if (ec || m_stop)
                break;
            const std::string msg(buf, n);
            if (msg.rfind("10001\t", 0) != 0)
                continue;
            ++m_requests;
            const unsigned short reply_port = (unsigned short) std::stoi(msg.substr(6));
            const std::string reply = "20001\t" + m_serial + "\tR004\t00\t01\t00";
            m_socket.send_to(asio::buffer(reply), udp::endpoint(from.address(), reply_port), 0, ec);
        }
    }
};

} // namespace

TEST_CASE("Discovery replies are parsed", "[halot]") {
    auto p = halot::parse_discovery_reply("20001\t10000795020G124JAIY\tR004\t00\t01\t00", "192.168.68.68");
    REQUIRE(p);
    CHECK(p->ip == "192.168.68.68");
    CHECK(p->serial == "10000795020G124JAIY");
    CHECK(p->model == "R004");
    CHECK(halot::model_name("R004") == "HALOT-R6");
    CHECK(halot::model_name("X999") == "X999");

    CHECK(!halot::parse_discovery_reply("10001\t44454", "192.168.68.10")); // our own broadcast
    CHECK(!halot::parse_discovery_reply("garbage", "192.168.68.10"));
}

TEST_CASE("Discovery finds a HALOT printer", "[halot]") {
    FakeHalotResponder printer("SERIAL-1");
    const std::vector<halot::Printer> found = halot::discover(std::chrono::milliseconds(300), "127.0.0.1", printer.port());
    REQUIRE(found.size() == 1);
    CHECK(found[0].ip == "127.0.0.1");
    CHECK(found[0].serial == "SERIAL-1");
    CHECK(found[0].model == "R004");
    CHECK(printer.requests() >= 1);
}

TEST_CASE("Discovery without printers ends in time with nothing", "[halot]") {
    unsigned short free_port;
    {
        FakeHalotResponder nobody("x"); // take a free port, then close it
        free_port = nobody.port();
    }
    const auto start = std::chrono::steady_clock::now();
    CHECK(halot::discover(std::chrono::milliseconds(300), "127.0.0.1", free_port).empty());
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(1));
}

// Against a real printer: HALOT_IP=192.168.x.x HALOT_FILE=cube.cxdlpv4 [HALOT_PASSWORD=...]
// Uploads without printing.
TEST_CASE("Live HALOT printer: test and upload without printing", "[.halot_live]") {
    const char *ip   = std::getenv("HALOT_IP");
    const char *file = std::getenv("HALOT_FILE");
    const char *pw   = std::getenv("HALOT_PASSWORD");
    REQUIRE(ip);
    REQUIRE(file);

    Slic3r::set_logging_level(4); // show the printer's answers
    halot::Client client(ip, pw ? pw : "", std::chrono::seconds(20));
    const std::string status = client.test();
    INFO("status " << status);

    size_t last = 0;
    const bool ok = client.upload(file, fs::path(file).filename().string(), false,
                                  [&last](size_t sent, size_t) { last = sent; return true; });
    CHECK(ok);
    CHECK(last == fs::file_size(file));
}

// Finds the HALOT printers on this network (needs one switched on).
TEST_CASE("Live HALOT printer: discovery", "[.halot_live]") {
    const std::vector<halot::Printer> found = halot::discover(std::chrono::seconds(2));
    for (const halot::Printer &p : found)
        UNSCOPED_INFO(p.ip << " " << halot::model_name(p.model) << " " << p.serial);
    CHECK(!found.empty());
}
