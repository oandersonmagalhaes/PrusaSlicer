///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "HalotProtocol.hpp"

#include <cstdint>
#include <cstdio>
#include <sstream>
#include <vector>

#include <zlib.h>

#include <boost/beast/core/detail/base64.hpp>
#include <boost/property_tree/json_parser.hpp>

namespace Slic3r { namespace halot {

namespace {

// DES tables (FIPS 46-3). Bit positions count from 1 at the most significant bit.
const uint8_t IP[64] = {58, 50, 42, 34, 26, 18, 10, 2, 60, 52, 44, 36, 28, 20, 12, 4,
                        62, 54, 46, 38, 30, 22, 14, 6, 64, 56, 48, 40, 32, 24, 16, 8,
                        57, 49, 41, 33, 25, 17, 9,  1, 59, 51, 43, 35, 27, 19, 11, 3,
                        61, 53, 45, 37, 29, 21, 13, 5, 63, 55, 47, 39, 31, 23, 15, 7};

const uint8_t FP[64] = {40, 8, 48, 16, 56, 24, 64, 32, 39, 7, 47, 15, 55, 23, 63, 31,
                        38, 6, 46, 14, 54, 22, 62, 30, 37, 5, 45, 13, 53, 21, 61, 29,
                        36, 4, 44, 12, 52, 20, 60, 28, 35, 3, 43, 11, 51, 19, 59, 27,
                        34, 2, 42, 10, 50, 18, 58, 26, 33, 1, 41, 9,  49, 17, 57, 25};

const uint8_t E[48] = {32, 1,  2,  3,  4,  5,  4,  5,  6,  7,  8,  9,  8,  9,  10, 11,
                       12, 13, 12, 13, 14, 15, 16, 17, 16, 17, 18, 19, 20, 21, 20, 21,
                       22, 23, 24, 25, 24, 25, 26, 27, 28, 29, 28, 29, 30, 31, 32, 1};

const uint8_t P[32] = {16, 7, 20, 21, 29, 12, 28, 17, 1,  15, 23, 26, 5,  18, 31, 10,
                       2,  8, 24, 14, 32, 27, 3,  9,  19, 13, 30, 6,  22, 11, 4,  25};

const uint8_t PC1[56] = {57, 49, 41, 33, 25, 17, 9,  1,  58, 50, 42, 34, 26, 18,
                         10, 2,  59, 51, 43, 35, 27, 19, 11, 3,  60, 52, 44, 36,
                         63, 55, 47, 39, 31, 23, 15, 7,  62, 54, 46, 38, 30, 22,
                         14, 6,  61, 53, 45, 37, 29, 21, 13, 5,  28, 20, 12, 4};

const uint8_t PC2[48] = {14, 17, 11, 24, 1,  5,  3,  28, 15, 6,  21, 10,
                         23, 19, 12, 4,  26, 8,  16, 7,  27, 20, 13, 2,
                         41, 52, 31, 37, 47, 55, 30, 40, 51, 45, 33, 48,
                         44, 49, 39, 56, 34, 53, 46, 42, 50, 36, 29, 32};

const uint8_t Shifts[16] = {1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1};

const uint8_t S[8][64] = {
    {14, 4,  13, 1,  2,  15, 11, 8,  3,  10, 6,  12, 5,  9,  0,  7,  0,  15, 7,  4,  14, 2,
     13, 1,  10, 6,  12, 11, 9,  5,  3,  8,  4,  1,  14, 8,  13, 6,  2,  11, 15, 12, 9,  7,
     3,  10, 5,  0,  15, 12, 8,  2,  4,  9,  1,  7,  5,  11, 3,  14, 10, 0,  6,  13},
    {15, 1,  8,  14, 6,  11, 3,  4,  9,  7,  2,  13, 12, 0,  5,  10, 3,  13, 4,  7,  15, 2,
     8,  14, 12, 0,  1,  10, 6,  9,  11, 5,  0,  14, 7,  11, 10, 4,  13, 1,  5,  8,  12, 6,
     9,  3,  2,  15, 13, 8,  10, 1,  3,  15, 4,  2,  11, 6,  7,  12, 0,  5,  14, 9},
    {10, 0,  9,  14, 6,  3,  15, 5,  1,  13, 12, 7,  11, 4,  2,  8,  13, 7,  0,  9,  3,  4,
     6,  10, 2,  8,  5,  14, 12, 11, 15, 1,  13, 6,  4,  9,  8,  15, 3,  0,  11, 1,  2,  12,
     5,  10, 14, 7,  1,  10, 13, 0,  6,  9,  8,  7,  4,  15, 14, 3,  11, 5,  2,  12},
    {7,  13, 14, 3,  0,  6,  9,  10, 1,  2,  8,  5,  11, 12, 4,  15, 13, 8,  11, 5,  6,  15,
     0,  3,  4,  7,  2,  12, 1,  10, 14, 9,  10, 6,  9,  0,  12, 11, 7,  13, 15, 1,  3,  14,
     5,  2,  8,  4,  3,  15, 0,  6,  10, 1,  13, 8,  9,  4,  5,  11, 12, 7,  2,  14},
    {2,  12, 4,  1,  7,  10, 11, 6,  8,  5,  3,  15, 13, 0,  14, 9,  14, 11, 2,  12, 4,  7,
     13, 1,  5,  0,  15, 10, 3,  9,  8,  6,  4,  2,  1,  11, 10, 13, 7,  8,  15, 9,  12, 5,
     6,  3,  0,  14, 11, 8,  12, 7,  1,  14, 2,  13, 6,  15, 0,  9,  10, 4,  5,  3},
    {12, 1,  10, 15, 9,  2,  6,  8,  0,  13, 3,  4,  14, 7,  5,  11, 10, 15, 4,  2,  7,  12,
     9,  5,  6,  1,  13, 14, 0,  11, 3,  8,  9,  14, 15, 5,  2,  8,  12, 3,  7,  0,  4,  10,
     1,  13, 11, 6,  4,  3,  2,  12, 9,  5,  15, 10, 11, 14, 1,  7,  6,  0,  8,  13},
    {4,  11, 2,  14, 15, 0,  8,  13, 3,  12, 9,  7,  5,  10, 6,  1,  13, 0,  11, 7,  4,  9,
     1,  10, 14, 3,  5,  12, 2,  15, 8,  6,  1,  4,  11, 13, 12, 3,  7,  14, 10, 15, 6,  8,
     0,  5,  9,  2,  6,  11, 13, 8,  1,  4,  10, 7,  9,  5,  0,  15, 14, 2,  3,  12},
    {13, 2,  8,  4,  6,  15, 11, 1,  10, 9,  3,  14, 5,  0,  12, 7,  1,  15, 13, 8,  10, 3,
     7,  4,  12, 5,  6,  11, 0,  14, 9,  2,  7,  11, 4,  1,  9,  12, 14, 2,  0,  6,  10, 13,
     15, 3,  5,  8,  2,  1,  14, 7,  4,  10, 8,  13, 15, 12, 9,  0,  3,  5,  6,  11}};

uint64_t permute(uint64_t in, int in_bits, const uint8_t *table, int out_bits)
{
    uint64_t out = 0;
    for (int i = 0; i < out_bits; ++i)
        out = (out << 1) | ((in >> (in_bits - table[i])) & 1);
    return out;
}

uint32_t rotl28(uint32_t v, int n) { return ((v << n) | (v >> (28 - n))) & 0x0FFFFFFF; }

uint32_t feistel(uint32_t r, uint64_t subkey)
{
    const uint64_t e = permute(r, 32, E, 48) ^ subkey;
    uint32_t s = 0;
    for (int j = 0; j < 8; ++j) {
        const unsigned six = unsigned(e >> (42 - 6 * j)) & 0x3F;
        const unsigned row = ((six & 0x20) >> 4) | (six & 1);
        const unsigned col = (six >> 1) & 0xF;
        s = (s << 4) | S[j][row * 16 + col];
    }
    return uint32_t(permute(s, 32, P, 32));
}

uint64_t load_be64(const char *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | uint8_t(p[i]);
    return v;
}

std::string json_escape(const std::string &s)
{
    std::string out;
    for (char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (uint8_t(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", unsigned(uint8_t(c)));
                out += buf;
            } else
                out += c;
        }
    }
    return out;
}

// Builds {"cmd":...,"k":"v",...} with every value as a string, like HalotBox.
std::string command(const std::string &cmd, std::initializer_list<std::pair<const char *, std::string>> fields)
{
    std::string out = "{\"cmd\":\"" + json_escape(cmd) + "\"";
    for (const auto &[k, v] : fields)
        out += ",\"" + std::string(k) + "\":\"" + json_escape(v) + "\"";
    return out + "}";
}

} // namespace

std::string des_ecb_encrypt(const std::string &key, const std::string &data)
{
    uint64_t subkeys[16];
    const uint64_t cd = permute(load_be64(key.data()), 64, PC1, 56);
    uint32_t c = uint32_t(cd >> 28), d = uint32_t(cd & 0x0FFFFFFF);
    for (int i = 0; i < 16; ++i) {
        c = rotl28(c, Shifts[i]);
        d = rotl28(d, Shifts[i]);
        subkeys[i] = permute((uint64_t(c) << 28) | d, 56, PC2, 48);
    }

    std::string out;
    out.reserve(data.size());
    for (size_t off = 0; off + 8 <= data.size(); off += 8) {
        const uint64_t ip = permute(load_be64(data.data() + off), 64, IP, 64);
        uint32_t l = uint32_t(ip >> 32), r = uint32_t(ip);
        for (int i = 0; i < 16; ++i) {
            const uint32_t next = l ^ feistel(r, subkeys[i]);
            l = r;
            r = next;
        }
        const uint64_t block = permute((uint64_t(r) << 32) | l, 64, FP, 64);
        for (int i = 7; i >= 0; --i)
            out.push_back(char(uint8_t(block >> (8 * i))));
    }
    return out;
}

std::string token(const std::string &password)
{
    std::string data = password.empty() ? "123456" : password;
    const size_t pad = 8 - data.size() % 8; // PKCS#7
    data.append(pad, char(pad));

    const std::string encrypted = des_ecb_encrypt("a85e9d68", data);

    namespace b64 = boost::beast::detail::base64;
    std::string out(b64::encoded_size(encrypted.size()), '\0');
    out.resize(b64::encode(out.data(), encrypted.data(), encrypted.size()));
    return out;
}

std::string file_key(const std::string &head)
{
    if (head.size() < 8)
        return "0";

    uint32_t u = 0;
    for (int i = 7; i >= 4; --i)
        u = (u << 8) | uint8_t(head[i]);
    const int32_t v = int32_t(u);

    char buf[16];
    if (v < 0)
        std::snprintf(buf, sizeof(buf), "-%llx", (unsigned long long)(-int64_t(v)));
    else
        std::snprintf(buf, sizeof(buf), "%x", unsigned(v));
    return buf;
}

std::string chunk(const char *data, std::size_t size, bool compress)
{
    if (!compress)
        return std::string(data, size);

    uLongf zsize = compressBound(uLong(size));
    std::string out(4 + zsize, '\0');
    out[0] = char(uint8_t(size >> 24));
    out[1] = char(uint8_t(size >> 16));
    out[2] = char(uint8_t(size >> 8));
    out[3] = char(uint8_t(size));
    if (compress2(reinterpret_cast<Bytef *>(&out[4]), &zsize, reinterpret_cast<const Bytef *>(data), uLong(size),
                  Z_DEFAULT_COMPRESSION) != Z_OK)
        return {};
    out.resize(4 + zsize);
    return out;
}

std::string version_check() { return command("VERSION_CHECK", {}); }

std::string get_print_status(const std::string &token) { return command("GET_PRINT_STATUS", {{"token", token}}); }

std::string start_file(const std::string &filename, std::size_t size, const std::string &key)
{
    return command("START_FILE", {{"filename", filename}, {"size", std::to_string(size)}, {"offset", "0"}, {"key", key}});
}

std::string cancel_file(const std::string &filename, const std::string &key)
{
    return command("CANCEL_FILE", {{"filename", filename}, {"key", key}});
}

std::string start_print(const std::string &filename, const std::string &token)
{
    return command("START_PRINT", {{"filename", filename}, {"token", token}});
}

std::string discovery_request(unsigned short reply_port) { return "10001\t" + std::to_string(reply_port); }

std::optional<Printer> parse_discovery_reply(const std::string &reply, const std::string &ip)
{
    std::vector<std::string> fields;
    size_t start = 0;
    for (size_t tab; (tab = reply.find('\t', start)) != std::string::npos; start = tab + 1)
        fields.push_back(reply.substr(start, tab - start));
    fields.push_back(reply.substr(start));

    if (fields.size() < 3 || fields[0] != "20001")
        return std::nullopt;
    return Printer{ip, fields[1], fields[2]};
}

std::string model_name(const std::string &code)
{
    // Only the codes seen on real printers.
    if (code == "R004")
        return "HALOT-R6";
    return code;
}

std::optional<Message> parse(const std::string &json)
{
    Message msg;
    try {
        std::istringstream in(json);
        boost::property_tree::read_json(in, msg.fields);
    } catch (const std::exception &) {
        return std::nullopt;
    }
    msg.cmd = msg.get("cmd");
    return msg;
}

}} // namespace Slic3r::halot
