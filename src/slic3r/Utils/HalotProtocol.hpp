///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_HalotProtocol_hpp_
#define slic3r_HalotProtocol_hpp_

// Messages of the WiFi protocol of Creality HALOT resin printers (HALOT BOX):
// JSON commands and binary file chunks over a WebSocket on port 18188.

#include <cstddef>
#include <optional>
#include <string>

#include <boost/property_tree/ptree.hpp>

namespace Slic3r { namespace halot {

constexpr int DefaultPort = 18188;

// HalotBox finds printers with a UDP broadcast to DiscoveryPort, announcing
// the port it listens on for the answers (ReplyPort).
constexpr unsigned short DiscoveryPort = 45454;
constexpr unsigned short ReplyPort     = 44454;

// HalotBox sends the file in chunks of this size.
constexpr std::size_t ChunkSize = std::size_t(1) << 20;

// DES in ECB mode. The key has 8 bytes and the data a multiple of 8 bytes.
std::string des_ecb_encrypt(const std::string &key, const std::string &data);

// The token the printer checks in commands: the HALOT BOX password
// (123456 when empty), DES encrypted with HalotBox's key and Base64 encoded.
std::string token(const std::string &password);

// The key HalotBox sends with a file: bytes 4-7 of the file as a signed
// little-endian integer, in lowercase hex ("0" for shorter files).
std::string file_key(const std::string &head);

// One binary frame of the file: the uncompressed length (4 bytes, big endian)
// and a zlib stream when the printer asks for compression, else the raw bytes.
std::string chunk(const char *data, std::size_t size, bool compress);

std::string version_check();
std::string get_print_status(const std::string &token);
std::string start_file(const std::string &filename, std::size_t size, const std::string &key);
std::string cancel_file(const std::string &filename, const std::string &key);
std::string start_print(const std::string &filename, const std::string &token);

// A message from the printer. Every value is a string ("true" for booleans).
struct Message
{
    std::string                 cmd;
    boost::property_tree::ptree fields;

    std::string get(const std::string &key) const { return fields.get<std::string>(key, ""); }
};

std::optional<Message> parse(const std::string &json);

// A printer that answered the discovery broadcast.
struct Printer
{
    std::string ip;
    std::string serial;
    std::string model; // e.g. R004
};

// The broadcast: "10001" and the port the answers should go to.
std::string discovery_request(unsigned short reply_port);

// A printer's answer, "20001\t<serial>\t<model>\t...", from `ip`.
std::optional<Printer> parse_discovery_reply(const std::string &reply, const std::string &ip);

// The printer name for a model code, e.g. HALOT-R6 for R004.
std::string model_name(const std::string &code);

}} // namespace Slic3r::halot

#endif
