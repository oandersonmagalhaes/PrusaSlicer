///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_HalotClient_hpp_
#define slic3r_HalotClient_hpp_

#include <chrono>
#include <cstddef>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include "HalotProtocol.hpp"

namespace Slic3r { namespace halot {

// Why a request to a HALOT printer failed. The message is a detail for logs.
struct Error : std::runtime_error
{
    enum class Kind { connection, timeout, password, busy, refused, file };

    Kind kind;

    Error(Kind kind, const std::string &detail) : std::runtime_error(detail), kind(kind) {}
};

// Sends files to one HALOT printer the way HalotBox does. Every network
// operation gives up after `timeout`, so a printer that is off never hangs.
class Client
{
public:
    // host is "ip" or "ip:port"; an empty password means HalotBox's 123456.
    Client(std::string host, std::string password, std::chrono::milliseconds timeout);

    // Checks the connection and the password and returns the printer status,
    // e.g. PRINT_GENERAL when idle. Throws Error.
    std::string test() const;

    // Sends the file at `source` as `filename` and starts printing it if
    // `print`. `progress` gets the bytes sent and the total and returns false
    // to cancel. Returns false when cancelled. Throws Error.
    bool upload(const std::string &source, const std::string &filename, bool print,
                const std::function<bool(std::size_t sent, std::size_t total)> &progress) const;

private:
    bool upload_file(const std::string &source, const std::string &filename, bool print,
                     const std::function<bool(std::size_t sent, std::size_t total)> &progress) const;

    std::string               m_host;
    std::string               m_password;
    std::chrono::milliseconds m_timeout;
};

// Finds HALOT printers on the local network the way HalotBox does, waiting
// `wait` for answers. `address` and `port` are where the broadcast goes.
std::vector<Printer> discover(std::chrono::milliseconds wait,
                              const std::string &address = "255.255.255.255",
                              unsigned short port = DiscoveryPort);

}} // namespace Slic3r::halot

#endif
