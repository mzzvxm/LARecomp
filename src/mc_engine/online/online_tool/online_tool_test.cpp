// Check of larecomp_online_tool and of the browser code it shares with the game (server_browser/browser_query.cpp,
// relay_info.cpp). A program of its own: no game, no ReXGlue, no real server. The only network it touches is
// 127.0.0.1: fake relays (UDP) and a fake master (TCP) made here, plus answers recorded once from a real
// larecomp_relay_server on loopback (online_tool_recorded.h). Exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_online_tool_test     (CMakeLists.txt)
//   <build dir>/larecomp_online_tool_test
//
// or, with no build directory at all (Windows): the files of the tool (the clang++ line of README.md, "Building it
// by hand", without online_tool_main.cpp) plus this file:
//   clang++ -std=c++23 -D_CRT_SECURE_NO_WARNINGS online_tool_test.cpp online_tool.cpp online_tool_remote.cpp
//           identity_commands.cpp game_guard.cpp profile_ref.cpp ../server_browser/browser_query.cpp ../relay_info.cpp
//           ../rate_my_ride/rmr_net.cpp ../rate_my_ride/rmr_protocol.cpp ../leaderboards/lb_protocol.cpp
//           ../tournament_schedule.cpp ../identity/identity_core.cpp ../identity/identity_net.cpp
//           ../identity/identity_rename.cpp ../../../profile/larecomp_profile_store.cpp
//           -x c++ ../../../../thirdparty/monocypher/monocypher.c -lws2_32 -lcrypt32 -o online_tool_test.exe
//
// What it pins:
//   - the golden sizes and field offsets of every message (a byte of difference and an old relay is misread);
//   - the recorded answers decode into the fields the browser shows;
//   - hostile text (no NUL, control characters, bad UTF-8) is cleaned the same way for the game and the tool;
//   - the master's list parsing, the cap, the merged flags, every state of the list (not set, ok, unreachable, HTTP
//     error, a favourite that is a master), the legacy relay, a silent relay, a wrong token, rooms over pages;
//   - the icon rules (PNG, 64 KB, 512x512, CRC), the cache and its repair;
//   - the JSON documents: valid, strict UTF-8, the field names the launcher reads (a golden document pins the schema).

#ifndef NOMINMAX
#define NOMINMAX  // <windows.h> would otherwise define min and max
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "json_writer.h"
#include "online_tool.h"
#include "online_tool_recorded.h"
#include "../protocol/identity_protocol.h"
#include "../relay_info.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using Sock = SOCKET;
constexpr Sock kBadSock = INVALID_SOCKET;
static void CloseSock(Sock s) { closesocket(s); }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using Sock = int;
constexpr Sock kBadSock = -1;
static void CloseSock(Sock s) { close(s); }
#endif

namespace br = larecomp::browser;
namespace ot = larecomp::online_tool;
namespace fs = std::filesystem;

namespace {

// What the relay sends for the old info and icon queries (type 21: 73 bytes, type 23: 1047), written out here so the
// check does not take them from the code it checks.
constexpr size_t kLegacyInfoBytes = 73;
constexpr size_t kLegacyIconChunkBytes = 1047;

int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_failed;
        std::printf("FAILED: %s\n", what.c_str());
    }
}

void CheckEq(const std::string& got, const std::string& want, const std::string& what) {
    ++g_checks;
    if (got != want) {
        ++g_failed;
        std::printf("FAILED: %s: got '%s', want '%s'\n", what.c_str(), got.c_str(), want.c_str());
    }
}

void CheckInt(long long got, long long want, const std::string& what) {
    ++g_checks;
    if (got != want) {
        ++g_failed;
        std::printf("FAILED: %s: got %lld, want %lld\n", what.c_str(), got, want);
    }
}

std::vector<uint8_t> FromHex(const char* hex) {
    std::vector<uint8_t> out;
    auto nib = [](char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
    for (size_t i = 0; hex[i] && hex[i + 1]; i += 2) out.push_back(uint8_t(nib(hex[i]) * 16 + nib(hex[i + 1])));
    return out;
}

template <typename T>
T As(const std::vector<uint8_t>& bytes) {
    T v{};
    std::memcpy(&v, bytes.data(), std::min(sizeof(T), bytes.size()));
    return v;
}

// ---- a small JSON reader: the check reads the documents the way the launcher does -------------------------------

struct JV {
    enum Type { kNull, kBool, kNum, kStr, kArr, kObj } t = kNull;
    bool b = false;
    long long n = 0;
    std::string s;
    std::vector<JV> a;
    std::vector<std::string> keys;  // an object's members: keys[i] names o[i] (a vector of pairs would need JV complete)
    std::vector<JV> o;

    const JV* Get(const std::string& k) const {
        for (size_t i = 0; i < keys.size(); ++i)
            if (keys[i] == k) return &o[i];
        return nullptr;
    }
};

struct JsonParser {
    const std::string& in;
    size_t i = 0;
    std::string err;

    explicit JsonParser(const std::string& s) : in(s) {}
    bool Fail(const char* m) {
        if (err.empty()) err = std::string(m) + " at " + std::to_string(i);
        return false;
    }
    void Ws() {
        while (i < in.size() && (in[i] == ' ' || in[i] == '\n' || in[i] == '\r' || in[i] == '\t')) ++i;
    }
    bool Lit(const char* w) {
        const size_t n = std::strlen(w);
        if (in.compare(i, n, w) != 0) return Fail("bad literal");
        i += n;
        return true;
    }
    bool Str(std::string& out) {
        if (i >= in.size() || in[i] != '"') return Fail("string expected");
        ++i;
        while (i < in.size() && in[i] != '"') {
            const unsigned char c = (unsigned char)in[i];
            if (c < 0x20) return Fail("raw control character");
            if (c == '\\') {
                if (++i >= in.size()) return Fail("bad escape");
                const char e = in[i++];
                switch (e) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'u': {
                        if (i + 4 > in.size()) return Fail("short \\u");
                        unsigned cp = 0;
                        for (int k = 0; k < 4; ++k) {
                            const char h = in[i++];
                            if (!std::isxdigit((unsigned char)h)) return Fail("bad \\u");
                            cp = cp * 16 + unsigned(h <= '9' ? h - '0' : (h | 0x20) - 'a' + 10);
                        }
                        if (cp >= 0xD800 && cp <= 0xDFFF) return Fail("surrogate escape");
                        if (cp < 0x80) out += char(cp);
                        else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 63)); }
                        else { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 63)); out += char(0x80 | (cp & 63)); }
                        break;
                    }
                    default: return Fail("unknown escape");
                }
                continue;
            }
            if (c >= 0x80) {
                const size_t n = br::Utf8SequenceLength(reinterpret_cast<const unsigned char*>(in.data()) + i, in.size() - i);
                if (n == 0) return Fail("invalid UTF-8");
                out.append(in, i, n);
                i += n;
                continue;
            }
            out += char(c);
            ++i;
        }
        if (i >= in.size()) return Fail("unterminated string");
        ++i;
        return true;
    }
    bool Value(JV& v) {
        Ws();
        if (i >= in.size()) return Fail("value expected");
        const char c = in[i];
        if (c == '{') {
            v.t = JV::kObj;
            ++i;
            Ws();
            if (i < in.size() && in[i] == '}') { ++i; return true; }
            for (;;) {
                Ws();
                std::string k;
                if (!Str(k)) return false;
                Ws();
                if (i >= in.size() || in[i] != ':') return Fail("colon expected");
                ++i;
                JV child;
                if (!Value(child)) return false;
                v.keys.push_back(std::move(k));
                v.o.push_back(std::move(child));
                Ws();
                if (i < in.size() && in[i] == ',') { ++i; continue; }
                if (i < in.size() && in[i] == '}') { ++i; return true; }
                return Fail("comma or } expected");
            }
        }
        if (c == '[') {
            v.t = JV::kArr;
            ++i;
            Ws();
            if (i < in.size() && in[i] == ']') { ++i; return true; }
            for (;;) {
                JV child;
                if (!Value(child)) return false;
                v.a.push_back(std::move(child));
                Ws();
                if (i < in.size() && in[i] == ',') { ++i; continue; }
                if (i < in.size() && in[i] == ']') { ++i; return true; }
                return Fail("comma or ] expected");
            }
        }
        if (c == '"') { v.t = JV::kStr; return Str(v.s); }
        if (c == 't') { v.t = JV::kBool; v.b = true; return Lit("true"); }
        if (c == 'f') { v.t = JV::kBool; v.b = false; return Lit("false"); }
        if (c == 'n') { v.t = JV::kNull; return Lit("null"); }
        if (c == '-' || (c >= '0' && c <= '9')) {
            v.t = JV::kNum;
            const size_t start = i;
            if (c == '-') ++i;
            while (i < in.size() && in[i] >= '0' && in[i] <= '9') ++i;
            if (i == start || (in[start] == '-' && i == start + 1)) return Fail("bad number");
            v.n = std::atoll(in.c_str() + start);
            return true;
        }
        return Fail("unexpected character");
    }
};

bool ParseJson(const std::string& text, JV& out, std::string& err) {
    out = JV{};
    JsonParser p(text);
    if (!p.Value(out)) { err = p.err; return false; }
    p.Ws();
    if (p.i != text.size()) { err = "trailing characters"; return false; }
    return true;
}

// "servers.0.players" style lookup; null JV when absent.
const JV& Path(const JV& root, const std::string& path) {
    static const JV kNone;
    const JV* cur = &root;
    size_t pos = 0;
    while (pos <= path.size() && cur) {
        size_t dot = path.find('.', pos);
        if (dot == std::string::npos) dot = path.size();
        const std::string part = path.substr(pos, dot - pos);
        if (cur->t == JV::kArr) {
            const size_t idx = size_t(std::atoi(part.c_str()));
            cur = idx < cur->a.size() ? &cur->a[idx] : nullptr;
        } else {
            cur = cur->Get(part);
        }
        pos = dot + 1;
    }
    return cur ? *cur : kNone;
}

// ---- fakes on 127.0.0.1 ------------------------------------------------------------------------------------------

void StartSockets() {
#ifdef _WIN32
    WSADATA d;
    WSAStartup(MAKEWORD(2, 2), &d);
#endif
}

uint16_t BoundPort(Sock s) {
    sockaddr_in a{};
    socklen_t len = sizeof(a);
    getsockname(s, reinterpret_cast<sockaddr*>(&a), &len);
    return ntohs(a.sin_port);
}

sockaddr_in Loopback(uint16_t port) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    return a;
}

bool Readable(Sock s, int ms) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(s, &set);
    timeval tv{0, ms * 1000};
    return select(int(s) + 1, &set, nullptr, nullptr, &tv) > 0;
}

std::vector<uint8_t> MakePng(uint32_t w, uint32_t h, size_t total) {
    std::vector<uint8_t> d = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 13, 'I', 'H', 'D', 'R'};
    auto be32 = [&](uint32_t v) {
        for (int s = 24; s >= 0; s -= 8) d.push_back(uint8_t(v >> s));
    };
    be32(w);
    be32(h);
    for (uint8_t b : {8, 6, 0, 0, 0}) d.push_back(b);
    be32(0);  // the IHDR chunk's own CRC: not read by the relay or the tool
    uint32_t seed = w * 31 + h;
    while (d.size() < total) {
        seed = seed * 1664525u + 1013904223u;
        d.push_back(uint8_t(seed >> 24));
    }
    return d;
}

// A relay that answers like larecomp_relay_server, with the knobs a check needs.
struct FakeRelay {
    Sock sock = kBadSock;
    uint16_t port = 0;
    std::atomic<bool> stop{false};
    std::thread thread;

    // what it says
    bool legacy = false;           // answers only the old info/icon queries (types 20, 22)
    bool silent = false;           // answers nothing
    bool bad_token_first = false;  // sends every answer first with a wrong token
    bool stall_icon = false;       // never answers an icon query
    br::DetailsMsg details{};
    std::vector<br::Room> rooms;
    std::vector<uint8_t> icon;     // the bytes it serves (the announced size and crc are in `details`)
    std::string legacy_name;
    int legacy_players = 0, legacy_sessions = 0;

    // what it saw
    std::atomic<int> n_details{0}, n_rooms{0}, n_info{0}, n_icon{0}, n_bad_size{0};

    FakeRelay() {
        sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in a = Loopback(0);
        bind(sock, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        port = BoundPort(sock);
    }
    void Start() { thread = std::thread([this] { Loop(); }); }
    ~FakeRelay() {
        stop = true;
        if (thread.joinable()) thread.join();
        CloseSock(sock);
    }
    std::string Address() const { return "127.0.0.1:" + std::to_string(port); }

    void Send(const void* data, int n, const sockaddr_in& to, uint32_t token, bool wrong_token) {
        std::vector<uint8_t> copy(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + n);
        if (wrong_token) {
            const uint32_t bad = token ^ 0x5A5A5A5Au;
            std::memcpy(copy.data() + 9, &bad, 4);
            sendto(sock, reinterpret_cast<const char*>(copy.data()), n, 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to));
        }
        sendto(sock, reinterpret_cast<const char*>(data), n, 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to));
    }

    void Loop() {
        std::vector<uint8_t> buf(2048);
        while (!stop) {
            if (!Readable(sock, 20)) continue;
            sockaddr_in from{};
            socklen_t fl = sizeof(from);
            const int n = int(recvfrom(sock, reinterpret_cast<char*>(buf.data()), int(buf.size()), 0, reinterpret_cast<sockaddr*>(&from), &fl));
            if (n < 13) continue;
            uint32_t magic, version;
            std::memcpy(&magic, buf.data(), 4);
            std::memcpy(&version, buf.data() + 4, 4);
            if (magic != br::kMagic || version != br::kVersion) continue;
            const uint8_t type = buf[8];
            uint32_t token;
            std::memcpy(&token, buf.data() + 9, 4);
            if (silent) continue;

            if (type == br::kDetailsQuery && !legacy) {
                if (n != int(sizeof(br::DetailsMsg))) { ++n_bad_size; continue; }
                ++n_details;
                br::DetailsMsg a = details;
                a.hdr = {br::kMagic, br::kVersion, br::kDetails};
                a.token = token;
                Send(&a, int(sizeof(a)), from, token, bad_token_first);
            } else if (type == br::kRoomsQuery && !legacy) {
                if (n != int(sizeof(br::RoomsMsg))) { ++n_bad_size; continue; }
                ++n_rooms;
                uint16_t page;
                std::memcpy(&page, buf.data() + 13, 2);
                br::RoomsMsg a{};
                a.hdr = {br::kMagic, br::kVersion, br::kRooms};
                a.token = token;
                a.page = page;
                a.total = uint16_t(rooms.size());
                const size_t first = size_t(page) * br::kRoomsPerPage;
                for (size_t k = first; k < rooms.size() && a.count < br::kRoomsPerPage; ++k) a.rooms[a.count++] = rooms[k];
                Send(&a, int(sizeof(a)), from, token, false);
            } else if (type == 20) {
                if (n != int(kLegacyInfoBytes)) { ++n_bad_size; continue; }
                ++n_info;
                std::vector<uint8_t> a(kLegacyInfoBytes, 0);
                const br::Header h{br::kMagic, br::kVersion, 21};
                std::memcpy(a.data(), &h, 9);
                std::memcpy(a.data() + 9, &token, 4);
                const uint16_t players = uint16_t(legacy_players), sessions = uint16_t(legacy_sessions);
                std::memcpy(a.data() + 13, &players, 2);
                std::memcpy(a.data() + 15, &sessions, 2);
                const uint32_t isize = legacy ? details.icon_size : 0, icrc = legacy ? details.icon_crc : 0;
                std::memcpy(a.data() + 17, &isize, 4);
                std::memcpy(a.data() + 21, &icrc, 4);
                std::memcpy(a.data() + 25, legacy_name.c_str(), std::min<size_t>(legacy_name.size(), 47));
                Send(a.data(), int(a.size()), from, token, bad_token_first);
            } else if (type == 22) {
                if (n != int(kLegacyIconChunkBytes)) { ++n_bad_size; continue; }
                ++n_icon;
                if (stall_icon) continue;
                uint32_t crc, offset;
                std::memcpy(&crc, buf.data() + 9, 4);
                std::memcpy(&offset, buf.data() + 13, 4);
                if (icon.empty() || crc != details.icon_crc || offset >= icon.size()) continue;
                std::vector<uint8_t> a(kLegacyIconChunkBytes, 0);
                const br::Header h{br::kMagic, br::kVersion, 23};
                std::memcpy(a.data(), &h, 9);
                std::memcpy(a.data() + 9, &crc, 4);
                std::memcpy(a.data() + 13, &offset, 4);
                const uint32_t total = uint32_t(icon.size());
                std::memcpy(a.data() + 17, &total, 4);
                const uint16_t len = uint16_t(std::min<size_t>(1024, icon.size() - offset));
                std::memcpy(a.data() + 21, &len, 2);
                std::memcpy(a.data() + 23, icon.data() + offset, len);
                sendto(sock, reinterpret_cast<const char*>(a.data()), int(a.size()), 0, reinterpret_cast<const sockaddr*>(&from), sizeof(from));
            }
        }
    }
};

// A master over HTTP: POST/GET /servers gives `body` with `status`; `hang` accepts and never answers.
struct FakeMaster {
    Sock sock = kBadSock;
    uint16_t port = 0;
    std::atomic<bool> stop{false};
    std::thread thread;
    int status = 200;
    std::string body;
    bool hang = false;
    std::atomic<int> hits{0};

    FakeMaster() {
        sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a = Loopback(0);
        bind(sock, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        listen(sock, 8);
        port = BoundPort(sock);
    }
    void Start() { thread = std::thread([this] { Loop(); }); }
    ~FakeMaster() {
        stop = true;
        if (thread.joinable()) thread.join();
        CloseSock(sock);
    }
    std::string Address() const { return "127.0.0.1:" + std::to_string(port); }

    void Loop() {
        std::vector<Sock> held;
        while (!stop) {
            if (!Readable(sock, 20)) continue;
            Sock c = accept(sock, nullptr, nullptr);
            if (c == kBadSock) continue;
            ++hits;
            std::string req;
            char tmp[1024];
            while (req.find("\r\n\r\n") == std::string::npos && Readable(c, 500)) {
                const int n = int(recv(c, tmp, sizeof(tmp), 0));
                if (n <= 0) break;
                req.append(tmp, size_t(n));
            }
            if (hang) {
                held.push_back(c);
                continue;
            }
            const std::string reply = "HTTP/1.1 " + std::to_string(status) + (status == 200 ? " OK" : " Error") +
                                      "\r\nContent-Type: text/plain\r\nContent-Length: " + std::to_string(body.size()) +
                                      "\r\nConnection: close\r\n\r\n" + body;
            send(c, reply.data(), int(reply.size()), 0);
            CloseSock(c);
        }
        for (Sock c : held) CloseSock(c);
    }
};

uint16_t ClosedTcpPort() {
    Sock s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a = Loopback(0);
    bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    const uint16_t p = BoundPort(s);
    CloseSock(s);
    return p;
}

template <size_t N>
void SetText(char (&dst)[N], const std::string& text) {
    std::memset(dst, 0, N);
    std::memcpy(dst, text.data(), std::min(text.size(), N));  // may fill every byte: no NUL on purpose when asked
}

br::Room MakeRoom(uint32_t vip, const std::string& tag, const std::string& name, int players, int max, bool active, int props, int rtt = 12) {
    br::Room r{};
    r.host_vip = vip;
    r.host_rtt_ms = uint16_t(rtt);
    r.cur_players = uint8_t(players);
    r.max_players = uint8_t(max);
    r.flags = active ? br::kRoomActive : 0;
    r.prop_count = uint8_t(props);
    SetText(r.gamertag, tag);
    SetText(r.name, name);
    for (int i = 0; i < br::kRoomProps; ++i) r.props[i] = {uint32_t(100 + i), uint32_t(1000 + i)};
    return r;
}

br::Options QuickOptions() {
    br::Options o;
    o.udp_timeout_ms = 200;
    o.master_connect_timeout_ms = 500;
    o.master_io_timeout_ms = 400;
    o.fetch_icons = false;
    return o;
}

fs::path TempDir(const std::string& name) {
    const fs::path p = fs::temp_directory_path() / ("lar_online_tool_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" + name);
    fs::create_directories(p);
    return p;
}

std::vector<uint8_t> ReadFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// ---- the checks --------------------------------------------------------------------------------------------------

void CheckGoldenSizes() {
    CheckInt(sizeof(br::Header), 9, "Header size");
    CheckInt(sizeof(br::Prop), 8, "Prop size");
    CheckInt(sizeof(br::DetailsMsg), 469, "DetailsMsg size (type 24 query, type 25 answer)");
    CheckInt(sizeof(br::Room), 250, "Room size");
    CheckInt(sizeof(br::RoomsMsg), 1018, "RoomsMsg size (type 26 query, type 27 answer)");
    CheckInt(sizeof(br::TargetHintMsg), 17, "TargetHintMsg size (type 28)");
    CheckInt(sizeof(br::AnnounceMsg), 469, "AnnounceMsg size (type 40, as large as the probe it triggers)");
    CheckInt(larecomp::relay_info::kInfoMsgBytes, 73, "legacy info answer size (type 21)");
    CheckInt(larecomp::relay_info::kIconChunkMsgBytes, 1047, "legacy icon chunk size (type 23)");
    CheckInt(kLegacyInfoBytes, 73, "the check's own idea of the info size");
    CheckInt(kLegacyIconChunkBytes, 1047, "the check's own idea of the icon chunk size");
    CheckInt(br::kMagic, 0x3230434D, "magic MC02");
    CheckInt(br::kVersion, 5, "relay protocol version");
    CheckInt(br::kDetailsQuery, 24, "type 24");
    CheckInt(br::kDetails, 25, "type 25");
    CheckInt(br::kRoomsQuery, 26, "type 26");
    CheckInt(br::kRooms, 27, "type 27");
    CheckInt(br::kTargetHint, 28, "type 28");
    CheckInt(br::kAnnounce, 40, "type 40");
    CheckInt(br::kRoomsPerPage, 4, "rooms per page");
    CheckInt(br::kRoomProps, 24, "properties per room");

    CheckInt(offsetof(br::DetailsMsg, token), 9, "Details.token");
    CheckInt(offsetof(br::DetailsMsg, proof), 13, "Details.proof");
    CheckInt(offsetof(br::DetailsMsg, players), 21, "Details.players");
    CheckInt(offsetof(br::DetailsMsg, rooms), 23, "Details.rooms");
    CheckInt(offsetof(br::DetailsMsg, relay_port), 25, "Details.relay_port");
    CheckInt(offsetof(br::DetailsMsg, flags), 27, "Details.flags");
    CheckInt(offsetof(br::DetailsMsg, icon_size), 29, "Details.icon_size");
    CheckInt(offsetof(br::DetailsMsg, icon_crc), 33, "Details.icon_crc");
    CheckInt(offsetof(br::DetailsMsg, name), 37, "Details.name");
    CheckInt(offsetof(br::DetailsMsg, description), 85, "Details.description");
    CheckInt(offsetof(br::DetailsMsg, region), 341, "Details.region");
    CheckInt(offsetof(br::DetailsMsg, tags), 365, "Details.tags");
    CheckInt(offsetof(br::DetailsMsg, http_port), 461, "Details.http_port");
    CheckInt(offsetof(br::DetailsMsg, mods_required), 463, "Details.mods_required");
    CheckInt(offsetof(br::DetailsMsg, mods_optional), 464, "Details.mods_optional");
    CheckInt(offsetof(br::DetailsMsg, mods_crc), 465, "Details.mods_crc");
    CheckInt(offsetof(br::Room, host_vip), 0, "Room.host_vip");
    CheckInt(offsetof(br::Room, host_rtt_ms), 4, "Room.host_rtt_ms");
    CheckInt(offsetof(br::Room, cur_players), 6, "Room.cur_players");
    CheckInt(offsetof(br::Room, max_players), 7, "Room.max_players");
    CheckInt(offsetof(br::Room, flags), 8, "Room.flags");
    CheckInt(offsetof(br::Room, prop_count), 9, "Room.prop_count");
    CheckInt(offsetof(br::Room, gamertag), 10, "Room.gamertag");
    CheckInt(offsetof(br::Room, name), 26, "Room.name");
    CheckInt(offsetof(br::Room, props), 58, "Room.props");
    CheckInt(offsetof(br::RoomsMsg, token), 9, "Rooms.token");
    CheckInt(offsetof(br::RoomsMsg, page), 13, "Rooms.page");
    CheckInt(offsetof(br::RoomsMsg, total), 15, "Rooms.total");
    CheckInt(offsetof(br::RoomsMsg, count), 17, "Rooms.count");
    CheckInt(offsetof(br::RoomsMsg, rooms), 18, "Rooms.rooms");
    CheckInt(offsetof(br::TargetHintMsg, token), 9, "TargetHint.token");
    CheckInt(offsetof(br::TargetHintMsg, host_vip), 13, "TargetHint.host_vip");
    // the header on the wire is "MC02", version 5, then the type
    br::Header h{br::kMagic, br::kVersion, 24};
    uint8_t raw[9];
    std::memcpy(raw, &h, 9);
    CheckEq(std::string(reinterpret_cast<char*>(raw), 4), "MC02", "the first four bytes are MC02");
    CheckInt(raw[4], 5, "version byte");
    CheckInt(raw[8], 24, "type byte");
}

void CheckRecordedAnswers() {
    const auto d = FromHex(kRecordedDetails);
    CheckInt(d.size(), 469, "recorded details size");
    br::DetailsMsg a = As<br::DetailsMsg>(d);
    CheckInt(a.hdr.magic, br::kMagic, "recorded details magic");
    CheckInt(a.hdr.version, 5, "recorded details version");
    CheckInt(a.hdr.type, br::kDetails, "recorded details type");
    CheckInt(a.token, 0xA1B2C3D4u, "recorded details echo the token");
    br::ServerView v;
    br::DecodeDetails(a, v);
    CheckEq(v.name, "Test Relay", "recorded name");
    CheckEq(v.description, "Loopback test relay.\nSecond line.", "recorded description keeps its newline");
    CheckEq(v.region, "XX", "recorded region");
    CheckEq(v.tags, "drift, JDM,  night", "recorded tags (raw)");
    CheckInt(v.players, 1, "recorded players");
    CheckInt(v.rooms, 0, "recorded rooms");
    CheckInt(a.relay_port, 31910, "recorded relay port");
    CheckInt(v.flags, 0, "recorded flags");
    CheckInt(v.icon_size, 2135, "recorded icon size");
    CheckInt(v.icon_crc, 0xC1EDAB6Du, "recorded icon crc");
    CheckInt(v.http_port, 31911, "recorded http port");
    CheckInt(v.mods_required, 0, "recorded mods required");
    CheckInt(v.mods_optional, 0, "recorded mods optional");
    CheckInt(v.mods_crc, 0, "recorded mods crc");

    const auto r = FromHex(kRecordedRooms);
    CheckInt(r.size(), 1018, "recorded rooms size");
    br::RoomsMsg page = As<br::RoomsMsg>(r);
    CheckInt(page.hdr.type, br::kRooms, "recorded rooms type");
    CheckInt(page.token, 0xA1B2C3D4u, "recorded rooms echo the token");
    CheckInt(page.total, 1, "recorded rooms total");
    CheckInt(page.count, 1, "recorded rooms count");
    std::set<uint32_t> seen;
    std::vector<br::RoomView> rooms;
    CheckInt(br::AppendRooms(page, seen, rooms), 1, "recorded rooms: entries on the page");
    CheckInt(rooms.size(), 1, "recorded rooms: one room");
    if (!rooms.empty()) {
        const auto& room = rooms[0];
        CheckEq(ot::VipText(room.host_vip), "10.0.0.2", "recorded room vip");
        CheckEq(room.gamertag, "HostTag", "recorded room gamertag");
        CheckEq(room.name, "Night Race Room", "recorded room name");
        CheckInt(room.cur_players, 1, "recorded room players");
        CheckInt(room.max_players, 8, "recorded room max players");
        CheckInt(room.host_rtt_ms, 1, "recorded room rtt");
        Check(!room.active, "recorded room is not active (nobody has played in it)");
        CheckInt(room.props.size(), 3, "recorded room properties");
        if (room.props.size() == 3) {
            CheckInt(room.props[0].id, 0x1234, "recorded prop 0 id");
            CheckInt(room.props[0].value, 7, "recorded prop 0 value");
            CheckInt(room.props[1].id, 0x4242, "recorded prop 1 id");
            CheckInt(room.props[1].value, 0xDEADBEEFu, "recorded prop 1 value");
        }
    }

    const auto info = FromHex(kRecordedInfo);
    CheckInt(info.size(), larecomp::relay_info::kInfoMsgBytes, "recorded info size");
    CheckInt(info[8], 21, "recorded info type");
    uint32_t isize, icrc;
    std::memcpy(&isize, info.data() + 17, 4);
    std::memcpy(&icrc, info.data() + 21, 4);
    CheckInt(isize, 2135, "recorded info icon size");
    CheckInt(icrc, 0xC1EDAB6Du, "recorded info icon crc");
    CheckEq(std::string(reinterpret_cast<const char*>(info.data() + 25)), "Test Relay", "recorded info name");

    const auto chunk = FromHex(kRecordedIconChunk);
    CheckInt(chunk.size(), larecomp::relay_info::kIconChunkMsgBytes, "recorded icon chunk size");
    CheckInt(chunk[8], 23, "recorded icon chunk type");
    uint32_t total;
    uint16_t len;
    std::memcpy(&total, chunk.data() + 17, 4);
    std::memcpy(&len, chunk.data() + 21, 2);
    CheckInt(total, 2135, "recorded icon total");
    CheckInt(len, 1024, "recorded icon chunk length");
    static const uint8_t kSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    Check(std::memcmp(chunk.data() + 23, kSig, 8) == 0, "recorded icon starts with a PNG signature");
}

void CheckCleaning() {
    auto clean = [](const std::string& s, bool nl = false) { return br::CleanWire(s.data(), s.size(), nl); };
    CheckEq(clean("Plain"), "Plain", "clean: plain text");
    CheckEq(clean(std::string("Cut\0Here", 8)), "Cut", "clean: stops at the first NUL");
    CheckEq(clean("A\x01\x02" "B\x7f" "C"), "ABC", "clean: control characters go");
    CheckEq(clean("a\nb\tc\r"), "abc", "clean: newline and tab go when not asked");
    CheckEq(clean("a\nb\tc\r", true), "a\nbc", "clean: only the newline stays when asked");
    CheckEq(clean("Evil\x01\xC0\xAFName\x7f\xED\xA0\x80ok"), "EvilNameok", "clean: stray bytes, overlong and a surrogate go");
    CheckEq(clean("\xE0\x80\x80x"), "x", "clean: overlong three-byte form goes");
    CheckEq(clean("\xF4\x90\x80\x80x"), "x", "clean: above U+10FFFF goes");
    CheckEq(clean("\xF0\x80\x80\x80x"), "x", "clean: overlong four-byte form goes");
    CheckEq(clean("caf\xC3\xA9"), "caf\xC3\xA9", "clean: two-byte letters stay");
    CheckEq(clean("\xE2\x82\xAC"), "\xE2\x82\xAC", "clean: three-byte stays");
    CheckEq(clean("\xF0\x9F\x8E\x8F!"), "\xF0\x9F\x8E\x8F!", "clean: four-byte stays");
    CheckEq(clean("ok\xE2\x82"), "ok", "clean: a sequence cut by the end goes");
    char full[16];
    std::memset(full, 'A', sizeof(full));
    CheckEq(br::CleanWire(full, sizeof(full)), std::string(16, 'A'), "clean: a field with no NUL at all is read to its end");
    CheckInt(br::Utf8SequenceLength(reinterpret_cast<const unsigned char*>("\xED\x9F\xBF"), 3), 3, "U+D7FF is valid");
    CheckInt(br::Utf8SequenceLength(reinterpret_cast<const unsigned char*>("\xF4\x8F\xBF\xBF"), 4), 4, "U+10FFFF is valid");

    // A legacy relay's name is a fixed field off the wire: it is cleaned like the others (it was copied raw before).
    {
        larecomp::relay_info::Info legacy;
        legacy.ok = true;
        legacy.name = "Evil\x01\xC0\xAFName\n\x7f";
        legacy.players = 3;
        legacy.sessions = 1;
        legacy.ping_ms = 12;
        legacy.icon_size = 0;
        legacy.icon_crc = 0xDEADBEEFu;
        br::ServerView lv;
        br::DecodeLegacy(legacy, lv);
        CheckEq(lv.name, "EvilName", "legacy relay: the name is cleaned");
        Check(lv.answered && lv.legacy && lv.players == 3 && lv.rooms == 1 && lv.ping_ms == 12, "legacy relay: the counts are taken");
        CheckInt(lv.icon_crc, 0, "legacy relay: no icon size, no icon crc");
    }

    // The icon header is read before a decoder allocates what it says.
    {
        auto png = [](uint32_t w, uint32_t h) {
            std::vector<uint8_t> b = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 13, 'I', 'H', 'D', 'R'};
            for (uint32_t v : {w, h})
                for (int sh = 24; sh >= 0; sh -= 8) b.push_back(uint8_t(v >> sh));
            b.resize(33, 0);
            return b;
        };
        auto fits = [&](const std::vector<uint8_t>& b) { return br::PngHeaderFits(b.data(), b.size(), br::kIconMaxSide); };
        Check(fits(png(512, 512)) && fits(png(1, 1)), "icon header: 512x512 and 1x1 fit");
        Check(!fits(png(513, 512)) && !fits(png(512, 513)) && !fits(png(20000, 20000)) && !fits(png(0, 64)) && !fits(png(64, 0)),
              "icon header: larger than 512 a side or empty does not");
        std::vector<uint8_t> not_png = png(64, 64);
        not_png[1] = 'X';
        std::vector<uint8_t> no_ihdr = png(64, 64);
        no_ihdr[12] = 'X';
        Check(!fits(not_png) && !fits(no_ihdr) && !br::PngHeaderFits(nullptr, 0, 512) && !br::PngHeaderFits(png(8, 8).data(), 23, 512),
              "icon header: a wrong signature, no IHDR, nothing or a cut header does not");
    }
}

void CheckTargets() {
    std::vector<br::Target> list;
    std::set<std::string> seen;
    br::AddTarget(list, seen, "1.2.3.4", true, false, false);
    br::AddTarget(list, seen, "  example.org:4000 \r", false, true, false);
    br::AddTarget(list, seen, "1.2.3.4:31000", false, false, true);  // the same as the first: flags merge
    br::AddTarget(list, seen, "", true, false, false);
    br::AddTarget(list, seen, "bad host!", true, false, false);
    br::AddTarget(list, seen, "no/slash:1", true, false, false);
    br::AddTarget(list, seen, std::string(300, 'a'), true, false, false);
    br::AddTarget(list, seen, "weird:0", true, false, false);   // port 0 is not a port: the default stays
    br::AddTarget(list, seen, "high:70000", true, false, false);  // nor is 70000
    CheckInt(list.size(), 4, "targets: four good ones");
    if (list.size() == 4) {
        CheckEq(list[0].address, "1.2.3.4:31000", "targets: default port 31000");
        Check(list[0].listed && list[0].current && !list[0].favorite, "targets: flags merged on the first copy");
        CheckEq(list[1].address, "example.org:4000", "targets: spaces and CR dropped");
        Check(list[1].favorite && !list[1].listed, "targets: favourite flag");
        CheckEq(list[2].address, "weird:31000", "targets: port 0 keeps the default");
        CheckEq(list[3].address, "high:31000", "targets: port 70000 keeps the default");
    }
    // the cap
    std::vector<br::Target> many;
    std::set<std::string> many_seen;
    for (int i = 0; i < 300; ++i) br::AddTarget(many, many_seen, "10.0." + std::to_string(i / 250) + "." + std::to_string(i % 250), true, false, false);
    CheckInt(many.size(), br::kMaxServers, "targets: at most 128");

    const auto lines = br::SplitLines("a:1\nb:2\r\n\n\nc:3");
    CheckInt(lines.size(), 5, "split: five lines (blanks included)");
    CheckEq(lines.size() > 1 ? lines[1] : "", "b:2\r", "split: the CR stays for AddTarget to drop");
    CheckInt(br::SplitLines("").size(), 0, "split: nothing");

    std::string host;
    uint16_t port = 7;
    br::SplitHostPort("h", host, port);
    CheckEq(host, "h", "SplitHostPort: bare host");
    CheckInt(port, 7, "SplitHostPort: port kept");
    br::SplitHostPort("h:99", host, port);
    CheckInt(port, 99, "SplitHostPort: port taken");
    br::SplitHostPort("h:x", host, port);
    CheckInt(port, 99, "SplitHostPort: a bad port keeps the old one");
    CheckEq(host, "h", "SplitHostPort: host without the bad port");
}

void CheckHelpers() {
    CheckInt(ot::Crc32(reinterpret_cast<const uint8_t*>("123456789"), 9), 0xCBF43926u, "CRC-32 of 123456789");
    CheckEq(ot::VipText(0x0A000002), "10.0.0.2", "vip text");
    CheckEq(ot::VipText(0xFFFFFFFF), "255.255.255.255", "vip text, all ones");
    CheckEq(ot::IconFileName(0x00ABCDEF), "00abcdef.png", "icon file name");
    const auto tags = ot::SplitTags(" drift,JDM , ,, night ");
    CheckInt(tags.size(), 3, "tags: three words");
    if (tags.size() == 3) CheckEq(tags[0] + "|" + tags[1] + "|" + tags[2], "drift|JDM|night", "tags: trimmed");
    CheckInt(ot::SplitTags("").size(), 0, "tags: none");

    // icon rules
    auto good = MakePng(64, 64, 500);
    const uint32_t crc = ot::Crc32(good.data(), good.size());
    CheckEq(ot::CheckIcon(good, crc), "", "icon: a good PNG");
    CheckEq(ot::CheckIcon(good, crc ^ 1).empty() ? "accepted" : "rejected", "rejected", "icon: wrong checksum");
    auto big = MakePng(64, 64, br::kIconMaxBytes + 1);
    Check(!ot::CheckIcon(big, ot::Crc32(big.data(), big.size())).empty(), "icon: over 64 KB");
    auto limit = MakePng(64, 64, br::kIconMaxBytes);
    CheckEq(ot::CheckIcon(limit, ot::Crc32(limit.data(), limit.size())), "", "icon: exactly 64 KB is fine");
    auto wide = MakePng(513, 64, 500);
    Check(!ot::CheckIcon(wide, ot::Crc32(wide.data(), wide.size())).empty(), "icon: 513 wide");
    auto tall = MakePng(64, 513, 500);
    Check(!ot::CheckIcon(tall, ot::Crc32(tall.data(), tall.size())).empty(), "icon: 513 tall");
    auto edge = MakePng(512, 512, 500);
    CheckEq(ot::CheckIcon(edge, ot::Crc32(edge.data(), edge.size())), "", "icon: 512x512 is fine");
    auto zero = MakePng(0, 64, 500);
    Check(!ot::CheckIcon(zero, ot::Crc32(zero.data(), zero.size())).empty(), "icon: zero width");
    std::vector<uint8_t> text(100, 'x');
    Check(!ot::CheckIcon(text, ot::Crc32(text.data(), text.size())).empty(), "icon: not a PNG");
    std::vector<uint8_t> tiny(10, 0);
    Check(!ot::CheckIcon(tiny, 0).empty(), "icon: too short");
}

void CheckJsonWriter() {
    ot::JsonWriter j;
    j.BeginObject();
    j.Field("a", "x\"y\\z\n\t\r\x01\x7f");
    j.Field("u", std::string("caf\xC3\xA9 \xF0\x9F\x8E\x8F"));
    j.Field("bad", std::string("a\xC0\xAF" "b\xED\xA0\x80" "c\xE2\x82"));
    j.Key("arr").BeginArray().Int(-5).Uint(7).Bool(true).Null().String("s").BeginArray().EndArray().BeginObject().EndObject().EndArray();
    j.Key("h").Hex32(0xABCDu);
    j.EndObject();
    CheckEq(j.str(),
            "{\"a\":\"x\\\"y\\\\z\\n\\t\\r\\u0001\\u007f\",\"u\":\"caf\xC3\xA9 \xF0\x9F\x8E\x8F\","
            "\"bad\":\"a\xEF\xBF\xBD\xEF\xBF\xBD" "b\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD" "c\xEF\xBF\xBD\xEF\xBF\xBD\","
            "\"arr\":[-5,7,true,null,\"s\",[],{}],\"h\":\"0000abcd\"}",
            "json: escapes, strict UTF-8, nesting, empty containers");
    JV v;
    std::string err;
    Check(ParseJson(j.str(), v, err), "json: the writer's output reads back (" + err + ")");

    ot::JsonWriter p(true);
    p.BeginObject().Field("k", 1).Key("l").BeginArray().Int(1).Int(2).EndArray().Key("e").BeginObject().EndObject().EndObject();
    CheckEq(p.str(), "{\n  \"k\": 1,\n  \"l\": [\n    1,\n    2\n  ],\n  \"e\": {}\n}", "json: pretty output");
    Check(ParseJson(p.str(), v, err), "json: pretty output reads back");
}

void CheckArguments() {
    auto parse = [](std::vector<std::string> v) { return ot::ParseArgs(v); };
    auto r = parse({"--version"});
    Check(r.ok && r.args.command == ot::Command::kVersion, "args: --version");
    r = parse({"--protocol", "--pretty"});
    Check(r.ok && r.args.command == ot::Command::kProtocol && r.args.pretty, "args: --protocol --pretty");
    r = parse({"--help"});
    Check(r.ok && r.args.command == ot::Command::kHelp, "args: --help");
    r = parse({"servers", "--master", "h:1", "--servers=a,b", "--current", "c:2", "--icons", "C:\\icons dir", "--timeout-ms", "500",
               "--master-timeout-ms=8000", "--pretty"});
    Check(r.ok && r.args.command == ot::Command::kServers, "args: servers with every option");
    if (r.ok) {
        CheckEq(r.args.master, "h:1", "args: master");
        CheckEq(r.args.servers, "a,b", "args: servers (with =)");
        CheckEq(r.args.current, "c:2", "args: current");
        CheckEq(r.args.icons_dir, "C:\\icons dir", "args: icons dir with a space");
        CheckInt(r.args.timeout_ms, 500, "args: timeout");
        CheckInt(r.args.master_timeout_ms, 8000, "args: master timeout");
        Check(r.args.pretty, "args: pretty");
    }
    r = parse({"servers"});
    Check(r.ok && r.args.master.empty() && r.args.timeout_ms == 800 && r.args.master_timeout_ms == 3000, "args: defaults are the game's (800 / 3000 ms), and there is no master");
    r = parse({"servers", "--master", ""});
    Check(r.ok && r.args.master.empty(), "args: an empty value is allowed (no list)");
    Check(!parse({}).ok, "args: nothing");
    Check(!parse({"bogus"}).ok, "args: unknown command");
    Check(!parse({"servers", "--bogus", "1"}).ok, "args: unknown option");
    Check(!parse({"servers", "--master"}).ok, "args: missing value at the end");
    Check(!parse({"servers", "--master", "--servers", "a"}).ok, "args: an option where a value should be");
    Check(!parse({"servers", "--master", "a", "--master", "b"}).ok, "args: given twice");
    Check(!parse({"servers", "--timeout-ms", "10"}).ok, "args: timeout too small");
    Check(!parse({"servers", "--timeout-ms", "20000"}).ok, "args: timeout too large");
    Check(!parse({"servers", "--timeout-ms", "abc"}).ok, "args: timeout not a number");
    Check(!parse({"servers", "--timeout-ms", "-5"}).ok, "args: timeout negative");
    Check(!parse({"--version", "--protocol"}).ok, "args: two commands");
    Check(!parse({"servers", "--version"}).ok, "args: servers and --version");
    Check(!parse({"--version", "--master", "x"}).ok, "args: a servers option with --version");
    Check(!parse({"--pretty"}).ok, "args: --pretty alone is no command");
}

void CheckVersionAndProtocol() {
    JV v;
    std::string err;
    Check(ParseJson(ot::RenderVersion(false), v, err), "version: valid JSON " + err);
    Check(Path(v, "ok").b, "version: ok");
    CheckEq(Path(v, "command").s, "version", "version: command");
    CheckInt(Path(v, "schema").n, ot::kSchema, "version: schema");
    CheckEq(Path(v, "tool").s, "larecomp_online_tool", "version: tool");
    CheckEq(Path(v, "version").s, ot::kToolVersion, "version: version");
    Check(!Path(v, "game_label").s.empty(), "version: game label");

    Check(ParseJson(ot::RenderProtocol(false), v, err), "protocol: valid JSON " + err);
    CheckEq(Path(v, "command").s, "protocol", "protocol: command");
    CheckInt(Path(v, "schema").n, ot::kSchema, "protocol: schema");
    CheckEq(Path(v, "commands.0").s, "servers", "protocol: the commands it has");
    CheckEq(Path(v, "relay.magic").s, "MC02", "protocol: magic");
    CheckInt(Path(v, "relay.version").n, 5, "protocol: relay version");
    CheckInt(Path(v, "relay.bytes.details").n, 469, "protocol: details bytes");
    CheckInt(Path(v, "relay.bytes.rooms").n, 1018, "protocol: rooms bytes");
    CheckInt(Path(v, "relay.bytes.info").n, 73, "protocol: info bytes");
    CheckInt(Path(v, "relay.bytes.icon_chunk").n, 1047, "protocol: icon chunk bytes");
    CheckInt(Path(v, "relay.messages.details_query").n, 24, "protocol: details query type");
    CheckInt(Path(v, "limits.udp_timeout_ms").n, 800, "protocol: the game's UDP timeout");
    CheckInt(Path(v, "limits.master_connect_timeout_ms").n, 1500, "protocol: the game's master connect timeout");
    CheckInt(Path(v, "limits.master_io_timeout_ms").n, 3000, "protocol: the game's master I/O timeout");
    CheckInt(Path(v, "limits.max_servers").n, 128, "protocol: server cap");
    CheckInt(Path(v, "limits.icon_max_bytes").n, 65536, "protocol: icon cap");
    CheckInt(Path(v, "limits.default_master_port").n, 31003, "protocol: default master port");
    // the master's address is never in the tool
    Check(ot::RenderProtocol(false).find("26.") == std::string::npos, "protocol: no address of any server");

    Check(ParseJson(ot::RenderError("servers", "usage", "bad \"thing\"", false), v, err), "error: valid JSON");
    Check(!Path(v, "ok").b && Path(v, "ok").t == JV::kBool, "error: ok is false");
    CheckEq(Path(v, "error.code").s, "usage", "error: code");
    CheckEq(Path(v, "error.message").s, "bad \"thing\"", "error: message");
}

// The whole tool against fake relays and a fake master on loopback.
void CheckLoopbackFlows() {
    const auto icon_a = MakePng(32, 32, 2500);  // three chunks
    const uint32_t icon_a_crc = ot::Crc32(icon_a.data(), icon_a.size());

    FakeRelay a, b, c, d;
    // A: a modern relay, verified nicks, mods, an icon, seven room entries (one a repeat), one with 30 properties
    a.icon = icon_a;
    SetText(a.details.name, "Alpha");
    SetText(a.details.description, "Line one\nLine two\r\nLine\tthree");
    SetText(a.details.region, "EU");
    SetText(a.details.tags, "drift, JDM");
    a.details.players = 3;
    a.details.rooms = 2;
    a.details.flags = larecomp::identity::kRelayVerifiedNicks;
    a.details.icon_size = uint32_t(icon_a.size());
    a.details.icon_crc = icon_a_crc;
    a.details.http_port = 40001;
    a.details.mods_required = 2;
    a.details.mods_optional = 1;
    a.details.mods_crc = 0x1234ABCD;
    a.rooms = {MakeRoom(0x0A000002, "One", "Room one", 2, 8, true, 3), MakeRoom(0x0A000003, "Two", "Room two", 1, 4, false, 30, 0xFFFF),
               MakeRoom(0x0A000004, "Three", "Room three", 4, 8, true, 0), MakeRoom(0x0A000005, "Four", "Room four", 8, 8, true, 24),
               MakeRoom(0x0A000006, "Five", "Room five", 1, 2, false, 1), MakeRoom(0x0A000003, "Two again", "moved between pages", 1, 4, false, 1),
               MakeRoom(0x0A000007, "Seven", "Room seven", 3, 6, true, 2)};
    // B: a relay from before the browser
    b.legacy = true;
    b.legacy_name = "Bravo Old";
    b.legacy_players = 5;
    b.legacy_sessions = 1;
    // C: nobody home
    c.silent = true;
    // D: answers every query twice, the first time with a wrong token; hostile text
    SetText(d.details.name, std::string("Del\x01ta\xC0\xAF", 8));
    SetText(d.details.description, "");
    d.details.players = 9;
    d.bad_token_first = true;
    for (FakeRelay* r : {&a, &b, &c, &d}) r->Start();

    FakeMaster master;
    master.body = a.Address() + "\n" + b.Address() + "\r\n\n  \nnot a host!\n" + a.Address() + "\n";
    master.Start();

    br::Sources src;
    src.master = master.Address();
    src.favorites = c.Address() + ", " + b.Address() + "," + d.Address();
    src.current = a.Address();
    br::Options opts = QuickOptions();
    const br::Collected got = br::Collect(src, opts, {});

    CheckInt(got.servers.size(), 4, "flow: four servers (A, B, C, D in that order)");
    CheckInt(got.icons.size(), got.servers.size(), "flow: icons parallel to servers");
    if (got.servers.size() == 4) {
        const auto& sa = got.servers[0];
        const auto& sb = got.servers[1];
        const auto& sc = got.servers[2];
        const auto& sd = got.servers[3];
        CheckEq(sa.address, a.Address(), "flow: first is A");
        Check(sa.listed && sa.current && !sa.favorite, "flow: A is listed and current");
        Check(sa.answered && !sa.legacy && sa.queried, "flow: A answered with details");
        Check(sa.ping_ms >= 0, "flow: A has a ping");
        CheckEq(sa.name, "Alpha", "flow: A name");
        CheckEq(sa.description, "Line one\nLine two\nLinethree", "flow: A description keeps newlines only");
        CheckEq(sa.region, "EU", "flow: A region");
        CheckEq(sa.tags, "drift, JDM", "flow: A tags");
        CheckInt(sa.players, 3, "flow: A players");
        CheckInt(sa.rooms, 2, "flow: A rooms");
        CheckInt(sa.flags, larecomp::identity::kRelayVerifiedNicks, "flow: A flags");
        CheckInt(sa.http_port, 40001, "flow: A http port");
        CheckInt(sa.mods_required, 2, "flow: A mods required");
        CheckInt(sa.mods_optional, 1, "flow: A mods optional");
        CheckInt(sa.mods_crc, 0x1234ABCD, "flow: A mods crc");
        CheckInt(sa.icon_crc, icon_a_crc, "flow: A icon crc");
        CheckInt(sa.room_list.size(), 6, "flow: A six distinct rooms over two pages (the repeat is dropped)");
        if (sa.room_list.size() == 6) {
            CheckEq(sa.room_list[0].gamertag, "One", "flow: room 0 gamertag");
            CheckInt(sa.room_list[0].host_rtt_ms, 12, "flow: room 0 rtt");
            CheckInt(sa.room_list[0].props.size(), 3, "flow: room 0 has three properties");
            Check(sa.room_list[0].active, "flow: room 0 active");
            CheckInt(sa.room_list[1].host_rtt_ms, -1, "flow: room 1 rtt unknown (0xFFFF)");
            CheckInt(sa.room_list[1].props.size(), 24, "flow: room 1 properties clamped to 24");
            Check(!sa.room_list[1].active, "flow: room 1 idle");
            CheckInt(sa.room_list[4].host_vip, 0x0A000006, "flow: room 4 vip");
            CheckInt(sa.room_list[5].host_vip, 0x0A000007, "flow: room 5 is the last one, from page 2");
        }
        CheckInt(got.icons[0].size(), 0, "flow: no icon fetched when not wanted");

        CheckEq(sb.address, b.Address(), "flow: second is B");
        Check(sb.listed && sb.favorite && !sb.current, "flow: B listed and a favourite (flags merged)");
        Check(sb.answered && sb.legacy, "flow: B is a legacy relay");
        CheckEq(sb.name, "Bravo Old", "flow: B name");
        CheckInt(sb.players, 5, "flow: B players");
        CheckInt(sb.rooms, 1, "flow: B sessions");
        CheckInt(sb.room_list.size(), 0, "flow: B has no rooms");
        CheckInt(sb.flags, 0, "flow: B has no flags");

        CheckEq(sc.address, c.Address(), "flow: third is C");
        Check(sc.favorite && !sc.listed && !sc.answered && sc.queried, "flow: C is a silent favourite");
        CheckInt(sc.ping_ms, -1, "flow: C has no ping");

        CheckEq(sd.address, d.Address(), "flow: fourth is D");
        Check(sd.answered && !sd.legacy, "flow: D answered after its wrong-token copy");
        CheckEq(sd.name, "Delta", "flow: D hostile name cleaned");
        CheckInt(sd.players, 9, "flow: D players come from the right-token answer");
    }
    CheckInt(int(got.master.state), int(br::MasterStatus::State::kOk), "flow: master ok");
    CheckEq(got.master.text, "", "flow: master text empty");
    CheckInt(got.master.lines, 6, "flow: the master listed six lines");
    for (FakeRelay* r : {&a, &b, &c, &d}) CheckInt(r->n_bad_size, 0, "flow: no query was a different size from its answer");
    CheckInt(a.n_details, 1, "flow: A asked for details once");
    CheckInt(a.n_rooms, 2, "flow: A asked for two room pages");
    CheckInt(b.n_info, 1, "flow: B asked the old way once");

    // ---- the whole tool: JSON + icons ----
    const fs::path dir = TempDir("icons");
    ot::Args args;
    args.command = ot::Command::kServers;
    args.master = master.Address();
    args.servers = c.Address() + "," + b.Address() + "," + d.Address();
    args.current = a.Address();
    args.icons_dir = (dir / "sub dir").string();
    args.timeout_ms = 200;
    args.master_timeout_ms = 400;
    ot::ServersReport rep = ot::GatherServers(args);
    const std::string json = ot::RenderServers(rep, false);
    JV doc;
    std::string err;
    Check(ParseJson(json, doc, err), "tool: the servers document is valid JSON (" + err + ")");
    Check(Path(doc, "ok").b, "tool: ok");
    CheckEq(Path(doc, "command").s, "servers", "tool: command");
    CheckInt(Path(doc, "schema").n, ot::kSchema, "tool: schema");
    CheckEq(Path(doc, "master.address").s, master.Address(), "tool: master address as given");
    CheckEq(Path(doc, "master.state").s, "ok", "tool: master state");
    CheckInt(Path(doc, "master.listed").n, 2, "tool: two servers are on the list");
    CheckInt(Path(doc, "summary.servers").n, 4, "tool: four servers");
    CheckInt(Path(doc, "summary.answered").n, 3, "tool: three answered");
    CheckInt(Path(doc, "summary.players").n, 3 + 5 + 9, "tool: players summed over the relays that answered");
    CheckEq(Path(doc, "servers.0.name").s, "Alpha", "tool: A name");
    Check(Path(doc, "servers.0.verified_nicks").b, "tool: A verifies nicks");
    Check(Path(doc, "servers.0.verified_nicks").t == JV::kBool, "tool: verified_nicks is a boolean for A");
    Check(Path(doc, "servers.0.password").t == JV::kBool && !Path(doc, "servers.0.password").b, "tool: A password flag false");
    CheckEq(Path(doc, "servers.0.tags.1").s, "JDM", "tool: A tags as a list");
    CheckInt(Path(doc, "servers.0.mods.required").n, 2, "tool: A mods");
    CheckEq(Path(doc, "servers.0.mods.crc").s, "1234abcd", "tool: A mods crc as hex");
    CheckInt(Path(doc, "servers.0.mods.http_port").n, 40001, "tool: A http port");
    CheckInt(int(Path(doc, "servers.0.room_list").a.size()), 6, "tool: A rooms");
    CheckEq(Path(doc, "servers.0.room_list.0.host_vip").s, "10.0.0.2", "tool: room vip as text");
    CheckEq(Path(doc, "servers.0.room_list.0.host_gamertag").s, "One", "tool: room gamertag");
    Check(Path(doc, "servers.0.room_list.1.host_rtt_ms").t == JV::kNull, "tool: unknown rtt is null");
    CheckInt(int(Path(doc, "servers.0.room_list.1.props").a.size()), 24, "tool: room props clamped");
    CheckEq(Path(doc, "servers.0.icon.state").s, "saved", "tool: A icon saved");
    CheckEq(Path(doc, "servers.0.icon.file").s, ot::IconFileName(icon_a_crc), "tool: A icon file name");
    CheckEq(Path(doc, "servers.0.icon.crc").s, ot::IconFileName(icon_a_crc).substr(0, 8), "tool: A icon crc");
    CheckInt(Path(doc, "servers.0.icon.size").n, (long long)icon_a.size(), "tool: A icon size");
    Check(Path(doc, "servers.1.legacy").b, "tool: B legacy");
    Check(Path(doc, "servers.1.verified_nicks").t == JV::kNull, "tool: B verified_nicks unknown (null)");
    Check(Path(doc, "servers.2.ping_ms").t == JV::kNull, "tool: C ping null");
    Check(!Path(doc, "servers.2.answered").b && Path(doc, "servers.2.answered").t == JV::kBool, "tool: C did not answer");
    CheckEq(Path(doc, "servers.2.icon.state").s, "none", "tool: C has no icon");
    CheckEq(Path(doc, "servers.3.name").s, "Delta", "tool: D name");
    const fs::path icon_file = fs::path(args.icons_dir) / ot::IconFileName(icon_a_crc);
    CheckInt((long long)(ReadFile(icon_file) == icon_a), 1, "tool: the icon file holds the relay's bytes");
    int tmp_left = 0;
    for (const auto& e : fs::directory_iterator(fs::path(args.icons_dir))) tmp_left += e.path().extension() == ".tmp" ? 1 : 0;
    CheckInt(tmp_left, 0, "tool: no temporary file left in the icons folder");

    // The later runs leave the silent relay out: a favourite that does not answer is also tried as a master over HTTP,
    // and a refused connection takes a Windows PC about two seconds.
    args.servers = b.Address() + "," + d.Address();

    // second run: from the cache, no icon query reaches the relay
    const int icon_queries = a.n_icon;
    CheckInt(icon_queries, 3, "tool: the first run fetched the icon in three chunks");
    rep = ot::GatherServers(args);
    CheckEq(ot::IconStateName(rep.icons.empty() ? ot::IconResult::State::kNone : rep.icons[0].state), "cached", "tool: second run takes the icon from the cache");
    CheckInt(a.n_icon, icon_queries, "tool: the second run asked for no icon chunk");

    // a damaged cache file is not trusted and is replaced
    {
        std::ofstream f(icon_file, std::ios::binary | std::ios::trunc);
        f << "damaged";
    }
    rep = ot::GatherServers(args);
    CheckEq(ot::IconStateName(rep.icons.empty() ? ot::IconResult::State::kNone : rep.icons[0].state), "saved", "tool: a damaged cached icon is fetched again");
    CheckInt((long long)(ReadFile(icon_file) == icon_a), 1, "tool: and the file is repaired");

    // without --icons nothing is fetched or written
    args.icons_dir.clear();
    const int before = a.n_icon;
    rep = ot::GatherServers(args);
    CheckEq(ot::IconStateName(rep.icons.empty() ? ot::IconResult::State::kNone : rep.icons[0].state), "not_requested", "tool: no folder, no icon");
    CheckInt(a.n_icon, before, "tool: no folder, no icon queries");
    CheckInt(int(rep.got.servers.empty() ? -1 : rep.got.servers[0].icon_crc != 0), 1, "tool: the icon's checksum is still reported");
    std::error_code ec;
    fs::remove_all(dir, ec);

    // ---- icon failures, one relay each ----
    const fs::path dir2 = TempDir("icons2");
    auto one_relay_icon = [&](const std::vector<uint8_t>& served, uint32_t announced_crc, uint32_t announced_size,
                              bool stall) {
        FakeRelay r;
        SetText(r.details.name, "Icon test");
        r.icon = served;
        r.details.icon_size = announced_size;
        r.details.icon_crc = announced_crc;
        r.stall_icon = stall;
        r.Start();
        ot::Args one;
        one.command = ot::Command::kServers;
        one.servers = r.Address();
        one.icons_dir = dir2.string();
        one.timeout_ms = 150;
        const auto report = ot::GatherServers(one);
        const std::string state = report.icons.empty() ? "?" : ot::IconStateName(report.icons[0].state);
        const std::string file = report.icons.empty() ? "?" : report.icons[0].file;
        return std::make_pair(state, file);
    };
    {
        auto bad_crc = MakePng(16, 16, 300);
        auto r1 = one_relay_icon(bad_crc, 0x11111111, uint32_t(bad_crc.size()), false);
        CheckEq(r1.first, "invalid", "icon failure: a checksum that does not match is invalid");
        CheckEq(r1.second, "", "icon failure: no file name for it");
        Check(!fs::exists(dir2 / ot::IconFileName(0x11111111)), "icon failure: and nothing was written");
        std::vector<uint8_t> not_png(300, 'x');
        auto r2 = one_relay_icon(not_png, ot::Crc32(not_png.data(), not_png.size()), 300, false);
        CheckEq(r2.first, "invalid", "icon failure: not a PNG is invalid");
        auto huge = MakePng(600, 600, 400);
        auto r3 = one_relay_icon(huge, ot::Crc32(huge.data(), huge.size()), uint32_t(huge.size()), false);
        CheckEq(r3.first, "invalid", "icon failure: 600x600 is invalid");
        auto r4 = one_relay_icon({}, 0x22222222, 70000, false);
        CheckEq(r4.first, "too_large", "icon failure: announced over 64 KB is too large, never fetched");
        auto fine = MakePng(16, 16, 300);
        auto r5 = one_relay_icon(fine, ot::Crc32(fine.data(), fine.size()), uint32_t(fine.size()), true);
        CheckEq(r5.first, "unavailable", "icon failure: a relay that stalls the icon leaves it unavailable");
        auto r6 = one_relay_icon(fine, ot::Crc32(fine.data(), fine.size()), uint32_t(fine.size()), false);
        CheckEq(r6.first, "saved", "icon failure: control run saves it");
    }
    fs::remove_all(dir2, ec);

    // ---- the states of the list ----
    {
        br::Sources s;
        const br::Collected none = br::Collect(s, opts, {});
        CheckInt(int(none.master.state), int(br::MasterStatus::State::kNotSet), "list: no list set");
        CheckInt(none.servers.size(), 0, "list: nothing to ask");
    }
    {
        br::Sources s;
        s.master = "127.0.0.1:" + std::to_string(ClosedTcpPort());
        const br::Collected res = br::Collect(s, opts, {});
        CheckInt(int(res.master.state), int(br::MasterStatus::State::kUnreachable), "list: a closed port is unreachable");
        CheckEq(res.master.text, "master unreachable (" + s.master + ")", "list: the game's words for it");
        Check(!res.master.detail.empty(), "list: and the transport's reason");
        CheckInt(res.servers.size(), 0, "list: no servers");
    }
    {
        FakeMaster bad;
        bad.status = 500;
        bad.Start();
        br::Sources s;
        s.master = bad.Address();
        const br::Collected res = br::Collect(s, opts, {});
        CheckInt(int(res.master.state), int(br::MasterStatus::State::kHttpError), "list: HTTP 500");
        CheckEq(res.master.text, "master answered HTTP 500", "list: the game's words for it");
        CheckInt(res.master.http_status, 500, "list: status kept");
    }
    {
        FakeMaster hang;
        hang.hang = true;
        hang.Start();
        br::Sources s;
        s.master = hang.Address();
        const auto t0 = std::chrono::steady_clock::now();
        const br::Collected res = br::Collect(s, opts, {});
        const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        CheckInt(int(res.master.state), int(br::MasterStatus::State::kUnreachable), "list: a master that never answers is unreachable");
        Check(ms < 3000, "list: and costs the I/O timeout, not more (" + std::to_string(ms) + " ms)");
    }
    {
        // a favourite that is no relay but a master: its relays are listed, it is not
        br::Sources s;
        s.favorites = master.Address();
        const br::Collected res = br::Collect(s, opts, {});
        CheckInt(res.master.via_favorites, 1, "list: a favourite that is a master");
        CheckEq(res.master.text, "", "list: its text is clear");
        CheckInt(int(res.master.state), int(br::MasterStatus::State::kNotSet), "list: no list was set");
        CheckInt(res.servers.size(), 2, "list: its two relays, not itself");
        if (res.servers.size() == 2) Check(res.servers[0].listed && !res.servers[0].favorite, "list: they are listed");
    }
    {
        // the tool's JSON for a failing list is still an answer
        ot::Args one;
        one.command = ot::Command::kServers;
        one.master = "127.0.0.1:" + std::to_string(ClosedTcpPort());
        one.timeout_ms = 150;
        one.master_timeout_ms = 300;
        const std::string text = ot::RenderServers(ot::GatherServers(one), false);
        JV doc2;
        Check(ParseJson(text, doc2, err), "list: unreachable master document is valid JSON");
        Check(Path(doc2, "ok").b, "list: unreachable master is still ok:true");
        CheckEq(Path(doc2, "master.state").s, "unreachable", "list: state unreachable");
        CheckInt(int(Path(doc2, "servers").a.size()), 0, "list: no servers");
    }

    {
        // several favourites that are off are tried as lists together, not one after the other (a refused connection
        // costs a Windows PC the whole connect timeout each)
        FakeRelay off1, off2, off3, off4, off5, off6;
        for (FakeRelay* r : {&off1, &off2, &off3, &off4, &off5, &off6}) {
            r->silent = true;
            r->Start();
        }
        br::Sources s;
        s.favorites = off1.Address() + "," + off2.Address() + "," + off3.Address() + "," + off4.Address() + "," + off5.Address() + "," +
                      off6.Address();
        const auto t0 = std::chrono::steady_clock::now();
        const br::Collected res = br::Collect(s, opts, {});
        const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        CheckInt(res.servers.size(), 6, "off favourites: all six are shown");
        for (const auto& sv : res.servers) Check(!sv.answered, "off favourites: none answered");
        CheckInt(res.master.via_favorites, 0, "off favourites: none is a list");
        // one after the other they would take 6 x 0.5 s of connect timeout on top of the 0.4 s of UDP queries
        Check(ms < 2200, "off favourites: probed together (" + std::to_string(ms) + " ms)");
    }

    // ---- a relay that sends junk instead of an answer ----
    {
        // an answer of the wrong size, wrong type or wrong version is ignored: a relay that answers with an
        // eight-hundred-byte datagram never makes a server
        Sock junk = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in addr = Loopback(0);
        bind(junk, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        const uint16_t port = BoundPort(junk);
        std::atomic<bool> stop{false};
        std::thread t([&] {
            std::vector<uint8_t> buf(2048);
            while (!stop) {
                if (!Readable(junk, 20)) continue;
                sockaddr_in from{};
                socklen_t fl = sizeof(from);
                const int n = int(recvfrom(junk, reinterpret_cast<char*>(buf.data()), int(buf.size()), 0, reinterpret_cast<sockaddr*>(&from), &fl));
                if (n < 13) continue;
                std::vector<uint8_t> a(buf.begin(), buf.begin() + n);
                a.resize(size_t(n) + 3, 0xFF);  // longer than the answer should be
                sendto(junk, reinterpret_cast<const char*>(a.data()), int(a.size()), 0, reinterpret_cast<const sockaddr*>(&from), sizeof(from));
                a.resize(size_t(n));
                a[8] = 99;  // the right size, a type nobody asked for
                sendto(junk, reinterpret_cast<const char*>(a.data()), int(a.size()), 0, reinterpret_cast<const sockaddr*>(&from), sizeof(from));
                a[8] = buf[8] + 1;
                a[4] = 4;  // the right type, an older protocol version
                sendto(junk, reinterpret_cast<const char*>(a.data()), int(a.size()), 0, reinterpret_cast<const sockaddr*>(&from), sizeof(from));
            }
        });
        br::Sources s;
        s.favorites = "127.0.0.1:" + std::to_string(port);
        const br::Collected res = br::Collect(s, opts, {});
        stop = true;
        t.join();
        CloseSock(junk);
        CheckInt(res.servers.size(), 1, "junk: the address is still listed");
        if (res.servers.size() == 1) Check(!res.servers[0].answered, "junk: but nothing it sent counts as an answer");
    }
}

// A golden document: the layout the launcher reads, byte for byte.
void CheckGoldenDocument() {
    ot::ServersReport rep;
    rep.master_address = "master.example:31003";
    rep.elapsed_ms = 42;
    rep.got.master.state = br::MasterStatus::State::kOk;
    rep.got.master.lines = 2;

    br::ServerView a;
    a.address = "relay.example:31000";
    a.host = "relay.example";
    a.port = 31000;
    a.listed = true;
    a.current = true;
    a.queried = true;
    a.answered = true;
    a.ping_ms = 23;
    a.name = "Alpha \"One\"";
    a.description = "Line one\nLine two";
    a.region = "EU";
    a.tags = "drift, JDM";
    a.players = 3;
    a.rooms = 1;
    a.flags = larecomp::identity::kRelayVerifiedNicks;
    a.icon_size = 2135;
    a.icon_crc = 0xC1EDAB6D;
    a.http_port = 31001;
    a.mods_required = 2;
    a.mods_optional = 1;
    a.mods_crc = 0x00ABCDEF;
    br::RoomView room;
    room.host_vip = 0x0A000002;
    room.host_rtt_ms = 5;
    room.cur_players = 2;
    room.max_players = 8;
    room.active = true;
    room.gamertag = "Host";
    room.name = "Night Race";
    room.props = {{0x1234, 7}, {5, 0xDEADBEEF}};
    a.room_list.push_back(room);

    br::ServerView b;
    b.address = "silent.example:31000";
    b.host = "silent.example";
    b.port = 31000;
    b.favorite = true;
    b.queried = true;

    rep.got.servers = {a, b};
    rep.got.icons.resize(2);
    ot::IconResult icon;
    icon.state = ot::IconResult::State::kSaved;
    icon.file = "c1edab6d.png";
    rep.icons = {icon, ot::IconResult{}};
    rep.icons[1].state = ot::IconResult::State::kNone;

    const std::string want =
        "{\"ok\":true,\"command\":\"servers\",\"schema\":1,"
        "\"master\":{\"address\":\"master.example:31003\",\"state\":\"ok\",\"message\":\"\",\"detail\":\"\",\"http_status\":0,\"listed\":1,\"via_favorites\":0},"
        "\"summary\":{\"servers\":2,\"answered\":1,\"players\":3,\"rooms\":1,\"elapsed_ms\":42},"
        "\"servers\":["
        "{\"address\":\"relay.example:31000\",\"host\":\"relay.example\",\"port\":31000,\"listed\":true,\"favorite\":false,\"current\":true,"
        "\"answered\":true,\"legacy\":false,\"ping_ms\":23,\"name\":\"Alpha \\\"One\\\"\",\"description\":\"Line one\\nLine two\",\"region\":\"EU\","
        "\"tags\":[\"drift\",\"JDM\"],\"players\":3,\"rooms\":1,\"verified_nicks\":true,\"password\":false,"
        "\"icon\":{\"crc\":\"c1edab6d\",\"size\":2135,\"state\":\"saved\",\"file\":\"c1edab6d.png\",\"error\":\"\"},"
        "\"mods\":{\"required\":2,\"optional\":1,\"http_port\":31001,\"crc\":\"00abcdef\"},"
        "\"room_list\":[{\"host_vip\":\"10.0.0.2\",\"host_gamertag\":\"Host\",\"name\":\"Night Race\",\"players\":2,\"max_players\":8,"
        "\"host_rtt_ms\":5,\"active\":true,\"props\":[{\"id\":4660,\"value\":7},{\"id\":5,\"value\":3735928559}]}]},"
        "{\"address\":\"silent.example:31000\",\"host\":\"silent.example\",\"port\":31000,\"listed\":false,\"favorite\":true,\"current\":false,"
        "\"answered\":false,\"legacy\":false,\"ping_ms\":null,\"name\":\"\",\"description\":\"\",\"region\":\"\","
        "\"tags\":[],\"players\":0,\"rooms\":0,\"verified_nicks\":null,\"password\":null,"
        "\"icon\":{\"crc\":null,\"size\":0,\"state\":\"none\",\"file\":null,\"error\":\"\"},"
        "\"mods\":{\"required\":0,\"optional\":0,\"http_port\":0,\"crc\":null},"
        "\"room_list\":[]}]}";
    CheckEq(ot::RenderServers(rep, false), want, "golden: the servers document");
}

}  // namespace

int main() {
    StartSockets();
    CheckGoldenSizes();
    CheckRecordedAnswers();
    CheckCleaning();
    CheckTargets();
    CheckHelpers();
    CheckJsonWriter();
    CheckArguments();
    CheckVersionAndProtocol();
    CheckGoldenDocument();
    CheckLoopbackFlows();
    std::printf("online_tool_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
