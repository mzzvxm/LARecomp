#include "relay_info.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <random>

#include <rex/cvar.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using SocketHandle = SOCKET;
constexpr SocketHandle kNoSocket = INVALID_SOCKET;
static void CloseSocket(SocketHandle s) { closesocket(s); }
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketHandle = int;
constexpr SocketHandle kNoSocket = -1;
static void CloseSocket(SocketHandle s) { close(s); }
#endif

namespace larecomp::relay_info {
namespace {

// Must match server.cpp (the relay). The header is the relay protocol's v5
// MsgHeader; the bodies are this query's own, and the online client in the
// SDK neither sends nor expects them.
constexpr uint32_t kMagic = 0x3230434D;  // "MC02"
constexpr uint32_t kVersion = 5;
constexpr uint8_t kInfoQuery = 20;
constexpr uint8_t kInfo = 21;
constexpr uint8_t kIconQuery = 22;
constexpr uint8_t kIconChunk = 23;
constexpr uint32_t kIconChunkBytes = 1024;
constexpr uint32_t kIconMaxBytes = 64 * 1024;

#pragma pack(push, 1)
struct Header {
    uint32_t magic;
    uint32_t version;
    uint8_t type;
};
struct InfoMsg {
    Header hdr;
    uint32_t token;
    uint16_t players;
    uint16_t sessions;
    uint32_t icon_size;
    uint32_t icon_crc;
    char name[48];
};
struct IconChunkMsg {
    Header hdr;
    uint32_t crc;
    uint32_t offset;
    uint32_t total;
    uint16_t len;
    uint8_t data[kIconChunkBytes];
};
#pragma pack(pop)
// A query is padded to the size of its answer, so the relay never sends back
// more than it was sent.
using InfoQueryMsg = InfoMsg;
using IconQueryMsg = IconChunkMsg;

void EnsureWinsock() {
#ifdef _WIN32
    static bool started = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    (void)started;
#endif
}

// A connected UDP socket to the relay, or kNoSocket.
SocketHandle Open(const std::string& host, uint16_t port) {
    EnsureWinsock();
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    const std::string service = std::to_string(port);
    if (getaddrinfo(host.c_str(), service.c_str(), &hints, &res) != 0 || !res) return kNoSocket;
    SocketHandle s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s != kNoSocket && connect(s, res->ai_addr, int(res->ai_addrlen)) != 0) {
        CloseSocket(s);
        s = kNoSocket;
    }
    freeaddrinfo(res);
    return s;
}

// Waits for one datagram; returns its length, or -1 on timeout or error.
int Receive(SocketHandle s, void* buf, int size, int timeout_ms) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(s, &set);
    timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    if (select(int(s) + 1, &set, nullptr, nullptr, &tv) <= 0) return -1;
    return int(recv(s, static_cast<char*>(buf), size, 0));
}

void FillHeader(Header& h, uint8_t type) {
    h.magic = kMagic;
    h.version = kVersion;
    h.type = type;
}

bool HeaderIs(const Header& h, uint8_t type) {
    return h.magic == kMagic && h.version == kVersion && h.type == type;
}

}  // namespace

Info Query(const std::string& host, uint16_t port, int timeout_ms) {
    Info out;
    SocketHandle s = Open(host, port);
    if (s == kNoSocket) return out;

    static std::mt19937 rng{std::random_device{}()};
    InfoQueryMsg q{};
    FillHeader(q.hdr, kInfoQuery);
    q.token = rng();

    const auto sent = std::chrono::steady_clock::now();
    if (send(s, reinterpret_cast<const char*>(&q), int(sizeof(q)), 0) == int(sizeof(q))) {
        InfoMsg a{};
        int left = timeout_ms;
        while (left > 0) {
            const int n = Receive(s, &a, int(sizeof(a)), left);
            const auto now = std::chrono::steady_clock::now();
            const int elapsed = int(
                std::chrono::duration_cast<std::chrono::milliseconds>(now - sent).count());
            if (n == int(sizeof(a)) && HeaderIs(a.hdr, kInfo) && a.token == q.token) {
                out.ok = true;
                out.ping_ms = elapsed;
                out.players = a.players;
                out.sessions = a.sessions;
                out.icon_size = a.icon_size;
                out.icon_crc = a.icon_crc;
                a.name[sizeof(a.name) - 1] = 0;
                out.name = a.name;
                break;
            }
            if (n < 0) break;
            left = timeout_ms - elapsed;
        }
    }
    CloseSocket(s);
    return out;
}

std::vector<uint8_t> FetchIcon(const std::string& host, uint16_t port, uint32_t crc,
                               uint32_t size, int timeout_ms) {
    std::vector<uint8_t> icon;
    if (size == 0 || size > kIconMaxBytes) return icon;
    SocketHandle s = Open(host, port);
    if (s == kNoSocket) return icon;

    icon.resize(size);
    uint32_t offset = 0;
    while (offset < size) {
        IconQueryMsg q{};
        FillHeader(q.hdr, kIconQuery);
        q.crc = crc;
        q.offset = offset;
        if (send(s, reinterpret_cast<const char*>(&q), int(sizeof(q)), 0) != int(sizeof(q))) break;
        IconChunkMsg a{};
        const int n = Receive(s, &a, int(sizeof(a)), timeout_ms);
        if (n < int(sizeof(a) - kIconChunkBytes) || !HeaderIs(a.hdr, kIconChunk) ||
            a.crc != crc || a.offset != offset || a.total != size || a.len == 0 ||
            a.len > kIconChunkBytes || offset + a.len > size)
            break;
        std::memcpy(icon.data() + offset, a.data, a.len);
        offset += a.len;
    }
    CloseSocket(s);
    if (offset != size) icon.clear();
    return icon;
}

void ConfiguredServer(std::string& host, uint16_t& port) {
    host = rex::cvar::GetFlagByName("mcla_online_server");
    const int cfg = std::atoi(rex::cvar::GetFlagByName("mcla_online_port").c_str());
    port = (cfg > 0 && cfg < 65536) ? uint16_t(cfg) : uint16_t(31000);
    if (const size_t colon = host.rfind(':'); colon != std::string::npos) {
        const int p = std::atoi(host.c_str() + colon + 1);
        if (p > 0 && p < 65536) port = uint16_t(p);
        host.resize(colon);
    }
    if (host.empty()) host = "127.0.0.1";
}

}  // namespace larecomp::relay_info
