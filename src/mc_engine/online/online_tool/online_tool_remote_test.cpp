// Check of the rmr, lb and tournament commands of larecomp_online_tool (online_tool_remote.cpp). A program of its own: no
// game, no ReXGlue, no real server. The only network it touches is 127.0.0.1: servers made here, and one of them is made of
// the real larecomp_rmr_server's own store code (rate_my_ride/rmr_store.cpp, leaderboards/lb_store.cpp,
// tournament_awards.cpp) behind a listener bound to loopback only (larecomp_rmr_server itself binds every interface, so
// running it would make Windows Firewall ask). Exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_online_tool_remote_test     (CMakeLists.txt)
//   <build dir>/larecomp_online_tool_remote_test
//
// or, with no build directory at all (Windows, llvm-mingw or clang; from this folder):
//   clang++ -std=c++20 -I. -I.. online_tool_remote_test.cpp online_tool.cpp online_tool_remote.cpp
//           ../server_browser/browser_query.cpp ../relay_info.cpp ../rate_my_ride/rmr_net.cpp ../rate_my_ride/rmr_protocol.cpp
//           ../rate_my_ride/rmr_store.cpp ../leaderboards/lb_protocol.cpp ../leaderboards/lb_store.cpp
//           ../tournament_schedule.cpp ../tournament_awards.cpp -lws2_32 -o online_tool_remote_test.exe
//
// What it pins:
//   - the command line: every command, every option, every way to get it wrong;
//   - answers recorded from the real stores (online_tool_remote_recorded.h) read into the documents the launcher reads;
//   - hostile answers: garbage, truncation, wrong magic, a server that hangs, HTTP errors, hostile text, wrong sizes;
//   - the game's own tables file (names and columns of stats views) in every state;
//   - a live server of the real store code: rides, photos kept in a folder, rating, boards in every shape, the weekly
//     tournament (the slot's view, the prizes of a week that ended), the request budget of one call;
//   - the secrets: the player's token is in no document, whatever happens.
//
// With LARECOMP_ONLINE_TOOL_TEST_DUMP set in the environment it also prints every document it made (lines starting DOC).

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "json_reader.h"
#include "json_writer.h"
#include "online_tool.h"
#include "online_tool_remote.h"
#include "online_tool_remote_recorded.h"
#include "../leaderboards/lb_protocol.h"
#include "../leaderboards/lb_store.h"
#include "../rate_my_ride/rmr_protocol.h"
#include "../rate_my_ride/rmr_store.h"
#include "../tournament_schedule.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using Sock = SOCKET;
constexpr Sock kBadSock = INVALID_SOCKET;
static void CloseSock(Sock s) { closesocket(s); }
constexpr int kSendFlags = 0;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using Sock = int;
constexpr Sock kBadSock = -1;
static void CloseSock(Sock s) { close(s); }
constexpr int kSendFlags = MSG_NOSIGNAL;  // a client that gave up must not kill the check
#endif

namespace ot = larecomp::online_tool;
namespace json = larecomp::online_tool::json;
namespace rec = larecomp::online_tool::recorded;
namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

namespace {

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
void CheckNear(double got, double want, double eps, const std::string& what) {
    ++g_checks;
    if (!(got > want - eps && got < want + eps)) {
        ++g_failed;
        std::printf("FAILED: %s: got %.9g, want %.9g\n", what.c_str(), got, want);
    }
}

Bytes FromHex(const std::string& hex) {
    Bytes out;
    auto nib = [](char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
    for (size_t i = 0; i + 1 < hex.size(); i += 2) out.push_back(uint8_t(nib(hex[i]) * 16 + nib(hex[i + 1])));
    return out;
}

std::string HexOf(const Bytes& b) {
    static const char kD[] = "0123456789abcdef";
    std::string s;
    for (uint8_t c : b) {
        s += kD[c >> 4];
        s += kD[c & 15];
    }
    return s;
}

rmr::Token Tok(uint8_t b) {
    rmr::Token t;
    t.fill(b);
    return t;
}

std::string TokHex(uint8_t b) { return HexOf(Bytes(16, b)); }  // the token of 16 bytes b, as the tool reads it on stdin

// A path as the UTF-8 text the tool's command line carries.
std::string Utf8(const fs::path& p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

// ---- documents -------------------------------------------------------------------------------------------------

std::vector<std::string> g_all_docs;  // every document this check made: scanned for secrets at the end

struct Doc {
    json::Value v;
    bool parsed = false;
    std::string raw;
    int exit_code = -1;

    const json::Value* P(const std::string& path) const { return v.Path(path); }
    bool Has(const std::string& path) const { return P(path) != nullptr; }
    bool IsNull(const std::string& path) const {
        const json::Value* x = P(path);
        return x && x->IsNull();
    }
    std::string S(const std::string& path) const {
        const json::Value* x = P(path);
        return x && x->IsString() ? x->Text() : std::string("<missing ") + path + ">";
    }
    long long N(const std::string& path) const {
        const json::Value* x = P(path);
        return x && x->IsNumber() ? (x->is_integer ? x->integer : (long long)x->number) : -999999999;
    }
    double D(const std::string& path) const {
        const json::Value* x = P(path);
        return x && x->IsNumber() ? x->number : -1e18;
    }
    bool B(const std::string& path) const {
        const json::Value* x = P(path);
        return x && x->IsBool() && x->boolean;
    }
    size_t Len(const std::string& path) const {
        const json::Value* x = P(path);
        return x && x->IsArray() ? x->items.size() : size_t(-1);
    }
};

// Parses a command line and runs it, like the tool's main does.
Doc Run(const std::vector<std::string>& argv, const std::string* token = nullptr) {
    Doc d;
    const ot::ParseResult p = ot::ParseArgs(argv);
    if (!p.ok) {
        d.raw = ot::RenderError("usage", "usage", p.error, false);
        d.exit_code = 2;
    } else {
        d.raw = ot::RunRemote(p.args.remote, &d.exit_code, token);
    }
    std::string err;
    d.parsed = json::Reader::Parse(d.raw, d.v, err);
    if (!d.parsed) std::printf("FAILED: not JSON (%s): %s\n", err.c_str(), d.raw.substr(0, 200).c_str());
    ++g_checks;
    if (!d.parsed) ++g_failed;
    g_all_docs.push_back(d.raw);
    if (std::getenv("LARECOMP_ONLINE_TOOL_TEST_DUMP")) std::printf("DOC %s\n", d.raw.c_str());  // to look at the documents
    return d;
}

std::vector<std::string> Cmd(std::initializer_list<const char*> words) {
    std::vector<std::string> v;
    for (const char* w : words) v.push_back(w);
    return v;
}

// ---- sockets and a loopback HTTP server ------------------------------------------------------------------------

void StartSockets() {
#ifdef _WIN32
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
#endif
}

sockaddr_in Loopback(uint16_t port) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    return a;
}

uint16_t BoundPort(Sock s) {
    sockaddr_in a{};
    socklen_t len = sizeof(a);
    getsockname(s, reinterpret_cast<sockaddr*>(&a), &len);
    return ntohs(a.sin_port);
}

bool Readable(Sock s, int ms) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(s, &set);
    timeval tv{0, ms * 1000};
    return select(int(s) + 1, &set, nullptr, nullptr, &tv) > 0;
}

uint16_t ClosedTcpPort() {
    Sock s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a = Loopback(0);
    bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    const uint16_t p = BoundPort(s);
    CloseSock(s);
    return p;
}

struct HttpRequest {
    std::string method, path;
    Bytes body;
};

struct HttpReply {
    int status = 200;
    Bytes body;
    bool hang = false;               // accept and never answer
    std::string raw;                 // when set: sent as it is (the status line, headers and body are the check's)
    int64_t announce_length = -1;    // when >= 0: the Content-Length to announce instead of the body's size
};

// A server on 127.0.0.1 only: one request at a time, like larecomp_rmr_server's.
struct HttpServer {
    Sock sock = kBadSock;
    uint16_t port = 0;
    std::atomic<bool> stop{false};
    std::thread thread;
    std::function<void(const HttpRequest&, HttpReply&)> handler;
    std::atomic<int> hits{0};

    HttpServer() {
        sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a = Loopback(0);
        bind(sock, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        listen(sock, 16);
        port = BoundPort(sock);
    }
    void Start() { thread = std::thread([this] { Loop(); }); }
    ~HttpServer() {
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
            std::string head;
            char tmp[4096];
            while (head.find("\r\n\r\n") == std::string::npos && Readable(c, 500)) {
                const int n = int(recv(c, tmp, sizeof(tmp), 0));
                if (n <= 0) break;
                head.append(tmp, size_t(n));
            }
            const size_t end = head.find("\r\n\r\n");
            HttpRequest req;
            if (end != std::string::npos) {
                const size_t sp1 = head.find(' ');
                const size_t sp2 = head.find(' ', sp1 + 1);
                req.method = head.substr(0, sp1);
                req.path = head.substr(sp1 + 1, sp2 - sp1 - 1);
                std::string lower = head;
                for (char& ch : lower) ch = char(std::tolower(static_cast<unsigned char>(ch)));
                size_t want = 0;
                if (const size_t cl = lower.find("content-length:"); cl != std::string::npos) want = size_t(std::atoll(lower.c_str() + cl + 15));
                req.body.assign(head.begin() + long(end + 4), head.end());
                while (req.body.size() < want && Readable(c, 500)) {
                    const int n = int(recv(c, tmp, sizeof(tmp), 0));
                    if (n <= 0) break;
                    req.body.insert(req.body.end(), tmp, tmp + n);
                }
            }
            HttpReply reply;
            handler(req, reply);
            if (reply.hang) {
                held.push_back(c);
                continue;
            }
            std::string out;
            if (!reply.raw.empty()) {
                out = reply.raw;
            } else {
                const size_t announced = reply.announce_length >= 0 ? size_t(reply.announce_length) : reply.body.size();
                out = "HTTP/1.1 " + std::to_string(reply.status) + (reply.status == 200 ? " OK" : " Error") +
                      "\r\nContent-Type: application/octet-stream\r\nContent-Length: " + std::to_string(announced) +
                      "\r\nConnection: close\r\n\r\n";
                out.append(reinterpret_cast<const char*>(reply.body.data()), reply.body.size());
            }
            size_t sent = 0;
            while (sent < out.size()) {
                const int n = int(send(c, out.data() + sent, size_t(std::min<size_t>(out.size() - sent, 32768)), kSendFlags));
                if (n <= 0) break;
                sent += size_t(n);
            }
            CloseSock(c);
        }
        for (Sock c : held) CloseSock(c);
    }
};

// What the real larecomp_rmr_server does with a request (rmr_server_main.cpp's handler), over its own store code.
struct StoreServer {
    fs::path dir;
    rmr::Store store;
    lb::Store boards;
    HttpServer http;
    std::atomic<int> downloads{0};
    std::atomic<int> requests{0};

    explicit StoreServer(const fs::path& d) : dir(d), store(d / "rmr"), boards(d / "lb") {
        std::error_code ec;
        fs::remove_all(dir, ec);
        std::string err;
        store.Open(&err);
        boards.Open(&err);
        store.AttachBoards(&boards);
        http.handler = [this](const HttpRequest& req, HttpReply& reply) {
            ++requests;
            if (req.path == lb::kHttpPath) {
                if (req.method != "POST") {
                    reply.status = 405;
                    return;
                }
                reply.body = boards.HandleWire(req.body);
                return;
            }
            if (req.path != rmr::kHttpPath) {
                reply.status = 404;
                return;
            }
            if (req.method != "POST") {
                reply.status = 405;
                return;
            }
            rmr::Request q;
            if (rmr::DecodeRequest(req.body, q) && q.op == rmr::Op::kDownload) ++downloads;
            reply.body = store.HandleWire(req.body);
        };
        http.Start();
    }
    std::string Address() const { return http.Address(); }

    // ---- seeding the data, in process (what the game's players did) ----
    rmr::Response Upload(uint8_t tok, const std::string& name, std::vector<Bytes> files, Bytes userdata = {}) {
        rmr::Request r;
        r.op = rmr::Op::kUpload;
        r.token = Tok(tok);
        r.gamertag = name;
        r.files = std::move(files);
        r.userdata = std::move(userdata);
        r.content = 3;
        return store.Handle(r);
    }
    rmr::Response Rate(uint8_t tok, uint32_t id, int v) {
        rmr::Request r;
        r.op = rmr::Op::kRate;
        r.token = Tok(tok);
        r.record_id = id;
        r.rating = uint8_t(v);
        return store.Handle(r);
    }
    void LbWrite(uint8_t tok, const std::string& name, uint32_t view, int32_t rating, int32_t wins) {
        lb::Request q;
        q.op = lb::Op::kWrite;
        q.token = Tok(tok);
        q.gamertag = name;
        q.view = view;
        lb::Value a;
        a.attr = lb::kAttrRating;
        a.type = lb::kTypeInt32;
        a.agg = lb::kAggMax;
        a.bits = rating;
        lb::Value b;
        b.attr = 1;
        b.type = lb::kTypeInt32;
        b.agg = lb::kAggSum;
        b.bits = wins;
        q.columns = {a, b};
        boards.Handle(q);
    }
};

Bytes Jpeg(uint8_t seed, size_t n) {
    Bytes b = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0x00};
    while (b.size() + 2 < n) b.push_back(uint8_t(seed + b.size()));
    b.push_back(0xFF);
    b.push_back(0xD9);
    return b;
}

Bytes ReadFileBytes(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

void WriteFileBytes(const fs::path& p, const std::string& text) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(text.data(), std::streamsize(text.size()));
}

fs::path ScratchDir() {
    static const fs::path dir = fs::temp_directory_path() / ("larecomp_online_tool_remote_test_" + std::to_string(std::time(nullptr)) + "_" + std::to_string(std::rand()));
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

// ---- the command line ------------------------------------------------------------------------------------------------

void CheckArguments() {
    auto parse = [](std::initializer_list<const char*> words) { return ot::ParseRemoteArgs(Cmd(words)); };
    auto bad = [&](std::initializer_list<const char*> words, const std::string& what) {
        const auto r = parse(words);
        Check(!r.ok, "args: " + what + " is refused");
        Check(!r.ok && !r.error.empty(), "args: " + what + " says why");
    };

    {
        const auto r = parse({"rmr", "top", "--server", "127.0.0.1:4000", "--count", "5", "--start=3", "--owner", "0c50476a8dc2d715", "--pretty",
                              "--photos", "C:\\dir with space", "--photo-slots", "2", "--min-ratings", "7", "--connect-timeout-ms", "500",
                              "--io-timeout-ms=2000"});
        Check(r.ok, "args: rmr top with every option: " + r.error);
        Check(r.args.group == ot::RemoteGroup::kRmr && r.args.sub == ot::RemoteSub::kRmrTop, "args: rmr top");
        CheckEq(r.args.server, "127.0.0.1:4000", "args: server");
        CheckInt(r.args.count, 5, "args: count");
        CheckInt(r.args.start, 3, "args: start with =");
        CheckInt(r.args.owners.size(), 1, "args: one owner");
        Check(r.args.owners.size() == 1 && r.args.owners[0] == rec::kOwnerT1, "args: the owner's value");
        Check(r.args.pretty, "args: pretty");
        CheckEq(r.args.photos_dir, "C:\\dir with space", "args: a folder with a space");
        CheckInt(r.args.photo_slots, 2, "args: photo slots");
        CheckInt(r.args.min_ratings, 7, "args: min ratings");
        CheckInt(r.args.connect_timeout_ms, 500, "args: connect timeout");
        CheckInt(r.args.io_timeout_ms, 2000, "args: io timeout");
    }
    {
        const auto r = parse({"rmr", "table"});
        Check(r.ok, "args: rmr table");
        CheckInt(r.args.port, 31002, "args: the default port");
        CheckInt(r.args.min_ratings, 3, "args: the game's minimum of ratings");
        CheckInt(r.args.connect_timeout_ms, 3000, "args: the game's connect timeout");
        CheckInt(r.args.io_timeout_ms, 15000, "args: the game's I/O timeout");
        CheckInt(r.args.deadline_ms, 60000, "args: a minute for the whole call");
        const auto d = parse({"rmr", "table", "--deadline-ms", "5000"});
        Check(d.ok && d.args.deadline_ms == 5000, "args: a deadline given");
    }
    {
        const auto r = parse({"rmr", "photo", "--file", "52000002,0x52000003", "--out", "x"});
        Check(r.ok && r.args.files.size() == 2 && r.args.files[1] == 0x52000003, "args: rmr photo with two files");
    }
    {
        const auto r = parse({"rmr", "rate", "--record", "52000001", "--rating", "8", "--confirm", "--token-stdin", "--relay", "10.0.0.5:31000", "--port", "4000"});
        Check(r.ok && r.args.confirm && r.args.token_stdin && r.args.rating == 8 && r.args.record == 0x52000001, "args: rmr rate");
        CheckEq(r.args.relay, "10.0.0.5:31000", "args: relay");
        CheckInt(r.args.port, 4000, "args: port");
    }
    {
        const auto r = parse({"lb", "owners", "--view", "382", "--owner", "0c50476a8dc2d715,cba577d9c14a1b25"});
        Check(r.ok && r.args.owners.size() == 2 && r.args.views.size() == 1 && r.args.views[0] == 382, "args: lb owners with two owners");
    }
    {
        const auto r = parse({"lb", "size", "--view", "1,2,3"});
        Check(r.ok && r.args.views.size() == 3 && r.args.view_given, "args: lb size with three views");
    }
    {
        const auto r = parse({"tournament", "standings", "--now", "1795089600", "--view", "400", "--tables", "t.json", "--token-stdin"});
        Check(r.ok && r.args.now == 1795089600 && r.args.view_given && r.args.tables == "t.json", "args: tournament standings");
    }
    {
        const auto r = parse({"tournament", "next", "--weeks", "6"});
        Check(r.ok && r.args.weeks == 6, "args: tournament next");
    }

    bad({}, "no words");
    bad({"rmr"}, "rmr alone");
    bad({"rmr", "--pretty"}, "rmr with only an option");
    bad({"rmr", "fly"}, "an unknown rmr command");
    bad({"lb", "table"}, "a command of another group");
    bad({"rmr", "top", "--bogus", "1"}, "an unknown option");
    bad({"rmr", "top", "extra"}, "a stray word");
    bad({"rmr", "top", "--count"}, "an option without its value");
    bad({"rmr", "top", "--count", "--start"}, "an option followed by an option");
    bad({"rmr", "top", "--count", "5", "--count", "6"}, "a repeated option");
    bad({"rmr", "top", "--count", "0"}, "count 0");
    bad({"rmr", "top", "--count", "51"}, "count over the page");
    bad({"rmr", "top", "--count", "x"}, "a count that is no number");
    bad({"lb", "top", "--view", "1", "--count", "101"}, "an lb count over 100");
    bad({"rmr", "top", "--start", "0"}, "start 0");
    bad({"rmr", "top", "--min-ratings", "0"}, "min ratings 0");
    bad({"rmr", "top", "--photo-slots", "2"}, "photo slots without a folder");
    bad({"rmr", "top", "--photos", "d", "--photo-slots", "3"}, "three photo slots");
    bad({"rmr", "top", "--confirm"}, "--confirm where nothing is written");
    bad({"rmr", "top", "--token-stdin"}, "a token where none is used");
    bad({"rmr", "top", "--table", "x"}, "a near miss");
    bad({"rmr", "top", "--server", "bad host"}, "a server with a space");
    bad({"rmr", "top", "--server", "host:0"}, "port 0");
    bad({"rmr", "top", "--server", "host:99999"}, "port 99999");
    bad({"rmr", "top", "--server", "host:abc"}, "a port that is no number");
    bad({"rmr", "top", "--server", ":31002"}, "a server with no host");
    bad({"rmr", "top", "--server", "http://host"}, "a server with a scheme");
    bad({"rmr", "top", "--port", "70000"}, "port 70000");
    bad({"rmr", "top", "--connect-timeout-ms", "10"}, "a connect timeout of 10 ms");
    bad({"rmr", "top", "--io-timeout-ms", "999999"}, "an I/O timeout of 999999 ms");
    bad({"rmr", "top", "--deadline-ms", "999"}, "a deadline of 999 ms");
    bad({"rmr", "top", "--deadline-ms", "600001"}, "a deadline over ten minutes");
    bad({"lb", "views", "--deadline-ms", "5000"}, "a deadline for a command that asks no server");
    bad({"rmr", "top", "--owner", "0"}, "owner 0");
    bad({"rmr", "top", "--owner", "zzzz"}, "an owner that is not hex");
    bad({"rmr", "top", "--owner", "0c50476a8dc2d7150"}, "an owner of 17 digits");
    bad({"rmr", "mine"}, "rmr mine without an owner or token");
    bad({"rmr", "mine", "--owner", "0c50476a8dc2d715,cba577d9c14a1b25"}, "rmr mine with two owners");
    bad({"rmr", "photo", "--file", "52000002"}, "rmr photo without --out");
    bad({"rmr", "photo", "--out", "d"}, "rmr photo without --file");
    bad({"rmr", "photo", "--file", "12000002", "--out", "d"}, "a file id that is not ours");
    bad({"rmr", "photo", "--file", "zz", "--out", "d"}, "a file id that is not hex");
    bad({"rmr", "rate", "--record", "52000001", "--rating", "5", "--token-stdin"}, "rmr rate without --confirm");
    bad({"rmr", "rate", "--record", "52000001", "--rating", "5", "--confirm"}, "rmr rate without a token");
    bad({"rmr", "rate", "--rating", "5", "--confirm", "--token-stdin"}, "rmr rate without a ride");
    bad({"rmr", "rate", "--record", "52000001", "--confirm", "--token-stdin"}, "rmr rate without a rating");
    bad({"rmr", "rate", "--record", "52000001", "--rating", "11", "--confirm", "--token-stdin"}, "a rating of 11");
    bad({"rmr", "rate", "--record", "52000001", "--rating", "0", "--confirm", "--token-stdin"}, "a rating of 0");
    bad({"rmr", "rate", "--record", "0", "--rating", "5", "--confirm", "--token-stdin"}, "ride 0");
    bad({"rmr", "rate", "--record", "52000001", "--rating", "5", "--confirm=1", "--token-stdin"}, "a flag with a value");
    bad({"lb", "size"}, "lb size without a view");
    bad({"lb", "size", "--view", "0"}, "view 0");
    bad({"lb", "size", "--view", "65536"}, "view 65536");
    bad({"lb", "size", "--view", "1,,2"}, "an empty view in a list");
    bad({"lb", "top"}, "lb top without a view");
    bad({"lb", "top", "--view", "1,2"}, "lb top with two views");
    bad({"lb", "around", "--view", "1"}, "lb around without an owner");
    bad({"lb", "around", "--view", "1", "--owner", "0c50476a8dc2d715", "--span", "50"}, "a span of 50");
    bad({"lb", "owners", "--view", "1"}, "lb owners without owners");
    bad({"lb", "views", "--server", "x"}, "a server for a command that asks none");
    bad({"lb", "views", "--view", "1"}, "a view for lb views");
    bad({"tournament", "next", "--server", "x"}, "a server for tournament next");
    bad({"tournament", "next", "--weeks", "0"}, "0 weeks");
    bad({"tournament", "next", "--weeks", "53"}, "53 weeks");
    bad({"tournament", "rewards"}, "tournament rewards without a token");
    bad({"tournament", "current", "--count", "5"}, "a count for tournament current");
    bad({"tournament", "standings", "--view", "1,2"}, "standings with two views");
    bad({"tournament", "standings", "--now", "0"}, "now 0");
    {
        std::string many = "1";
        for (int i = 2; i <= 33; ++i) many += "," + std::to_string(i);
        const auto r = ot::ParseRemoteArgs({"lb", "size", "--view", many});
        Check(!r.ok, "args: 33 views are refused");
        std::string owners = "0c50476a8dc2d715";
        for (int i = 0; i < 100; ++i) owners += ",0c50476a8dc2d715";
        const auto r2 = ot::ParseRemoteArgs({"lb", "owners", "--view", "1", "--owner", owners});
        Check(!r2.ok, "args: 101 owners are refused");
    }

    // The tool's own parser hands the groups over, and only when the group comes first.
    {
        const auto p = ot::ParseArgs(Cmd({"rmr", "top", "--server", "h"}));
        Check(p.ok && p.args.command == ot::Command::kRmr && p.args.remote.sub == ot::RemoteSub::kRmrTop, "args: ParseArgs hands rmr over");
        const auto q = ot::ParseArgs(Cmd({"lb", "views"}));
        Check(q.ok && q.args.command == ot::Command::kLb, "args: ParseArgs hands lb over");
        const auto t = ot::ParseArgs(Cmd({"tournament", "next", "--pretty"}));
        Check(t.ok && t.args.command == ot::Command::kTournament && t.args.pretty, "args: ParseArgs hands tournament over");
        Check(!ot::ParseArgs(Cmd({"--pretty", "rmr", "top"})).ok, "args: the group comes first");
        Check(!ot::ParseArgs(Cmd({"rmr", "nothing"})).ok, "args: ParseArgs passes the refusal on");
        CheckEq(ot::CommandName(ot::Command::kRmr), "rmr", "args: command name rmr");
        CheckEq(ot::CommandName(ot::Command::kLb), "lb", "args: command name lb");
        CheckEq(ot::CommandName(ot::Command::kTournament), "tournament", "args: command name tournament");
        // the older commands are untouched
        Check(ot::ParseArgs(Cmd({"servers", "--master", "h"})).ok, "args: servers still parses");
        Check(!ot::ParseArgs(Cmd({"servers", "--count", "1"})).ok, "args: servers still refuses the new options");
    }
    // usage text mentions everything
    {
        const std::string u = ot::RemoteUsageText();
        for (const char* w : {"rmr table", "rmr top", "rmr mine", "rmr photo", "rmr rate", "lb views", "lb size", "lb top", "lb around", "lb owners",
                              "tournament current", "tournament standings", "tournament next", "tournament rewards", "--token-stdin", "--confirm"})
            Check(u.find(w) != std::string::npos, std::string("usage: mentions ") + w);
    }
}

// ---- helpers ---------------------------------------------------------------------------------------------------------

void CheckHelpers() {
    uint32_t v32 = 0;
    uint64_t v64 = 0;
    Check(ot::ParseHex32("52000001", v32) && v32 == 0x52000001, "hex32: eight digits");
    Check(ot::ParseHex32("0x52000001", v32) && v32 == 0x52000001, "hex32: 0x prefix");
    Check(ot::ParseHex32("5200000A", v32) && v32 == 0x5200000A, "hex32: capitals");
    Check(ot::ParseHex32("1", v32) && v32 == 1, "hex32: one digit");
    Check(!ot::ParseHex32("", v32), "hex32: empty");
    Check(!ot::ParseHex32("0x", v32), "hex32: only the prefix");
    Check(!ot::ParseHex32("520000011", v32), "hex32: nine digits");
    Check(!ot::ParseHex32("5200000g", v32), "hex32: not hex");
    Check(!ot::ParseHex32(" 1", v32), "hex32: a space");
    Check(ot::ParseHex64("0c50476a8dc2d715", v64) && v64 == rec::kOwnerT1, "hex64: sixteen digits");
    Check(!ot::ParseHex64("0c50476a8dc2d7150", v64), "hex64: seventeen digits");
    CheckEq(ot::Hex32Text(0x52000a01), "52000a01", "hex text 32");
    CheckEq(ot::Hex32Text(1), "00000001", "hex text 32 pads");
    CheckEq(ot::Hex64Text(rec::kOwnerT2), "cba577d9c14a1b25", "hex text 64");
    CheckEq(ot::Hex64Text(1), "0000000000000001", "hex text 64 pads");
    CheckEq(ot::PhotoFileName(0x52000002, 120), "52000002-120.jpg", "photo file name");

    Check(ot::LooksLikeJpeg({0xFF, 0xD8, 0xFF, 0xE0}), "jpeg: a header");
    Check(!ot::LooksLikeJpeg({0xFF, 0xD8, 0xFF}), "jpeg: too short");
    Check(!ot::LooksLikeJpeg({0x89, 'P', 'N', 'G', 1, 2}), "jpeg: a PNG is not");
    Check(!ot::LooksLikeJpeg({}), "jpeg: nothing");
    {
        Bytes big(rmr::kMaxPhotoBytes, 0);
        big[0] = 0xFF;
        big[1] = 0xD8;
        big[2] = 0xFF;
        Check(ot::LooksLikeJpeg(big), "jpeg: at the cap");
        big.push_back(0);
        Check(!ot::LooksLikeJpeg(big), "jpeg: over the cap");
    }

    {
        const auto p = ot::PrizesFromDescription({"Hollywood Sprint 5", "Any car. Ends Monday 00:00 UTC.", "1st: Audi R8, TIS rims, hydraulics.",
                                                  "2nd-3rd: TIS rims, hydraulics."});
        CheckInt(p.size(), 2, "prizes: two lines");
        if (p.size() == 2) {
            CheckEq(p[0].place, "1st", "prizes: first place");
            CheckInt(p[0].rewards.size(), 3, "prizes: three rewards");
            CheckEq(p[1].place, "2nd-3rd", "prizes: second to third");
            CheckInt(p[1].rewards.size(), 2, "prizes: two rewards");
            CheckEq(p[1].rewards[1], "hydraulics", "prizes: a name");
        }
        CheckInt(ot::PrizesFromDescription({"Note: bring friends", "1st: a car I made up", "x: Audi R8", "1st : TIS rims . "}).size(), 1,
                 "prizes: only lines with a place and a known reward");
        const auto twice = ot::PrizesFromDescription({"1st: Audi R8, Audi R8"});
        CheckInt(twice.size() == 1 ? twice[0].rewards.size() : 0, 2, "prizes: a repeated name is kept as written");
        CheckInt(ot::PrizesFromDescription({}).size(), 0, "prizes: nothing");
    }
}

void CheckJsonPieces() {
    // the reader
    json::Value v;
    std::string err;
    Check(json::Reader::Parse("{\"a\":[1,2,{\"b\":\"c\\u00e9\\n\"}],\"d\":true,\"e\":null,\"f\":-12,\"g\":1.5e2}", v, err), "json: a document " + err);
    CheckEq(v.Path("a.2.b") ? v.Path("a.2.b")->Text() : "", "c\xC3\xA9\n", "json: a path and an escape");
    CheckInt(v.Path("a.1") ? v.Path("a.1")->Int() : -1, 2, "json: an integer");
    CheckInt(v.Path("f") ? v.Path("f")->Int() : 0, -12, "json: a negative integer");
    Check(v.Path("g") && !v.Path("g")->is_integer && v.Path("g")->number == 150.0, "json: a real number");
    Check(v.Path("d") && v.Path("d")->boolean, "json: true");
    Check(v.Path("e") && v.Path("e")->IsNull(), "json: null");
    Check(!v.Path("a.9") && !v.Path("x") && !v.Path("a.b"), "json: missing paths");
    for (const char* bad : {"", "{", "[1,]", "{\"a\":}", "{\"a\" 1}", "[1 2]", "01", "1.", "-", "\"abc", "\"\\x\"", "\"\\ud800\"", "tru", "{\"a\":1} x",
                            "\"a\tb\"", "[1,2", "{1:2}", "nul"}) {
        json::Value x;
        std::string e;
        Check(!json::Reader::Parse(bad, x, e) && !e.empty(), std::string("json: refuses '") + bad + "'");
    }
    {
        std::string deep(100, '[');
        deep += std::string(100, ']');
        json::Value x;
        std::string e;
        Check(!json::Reader::Parse(deep, x, e), "json: refuses 100 levels");
        std::string ok(30, '[');
        ok += std::string(30, ']');
        Check(json::Reader::Parse(ok, x, e), "json: takes 30 levels");
    }
    Check(json::Reader::Parse("{\"a\":9223372036854775807,\"b\":9999999999999999999}", v, err), "json: big numbers");
    Check(v.Path("a") && v.Path("a")->is_integer && v.Path("a")->integer == 9223372036854775807ll, "json: the largest int64");
    Check(v.Path("b") && !v.Path("b")->is_integer, "json: one over the largest int64 is not an integer");

    // the writer's Double
    auto dbl = [](double x, int digits = 9) {
        ot::JsonWriter j;
        j.BeginArray().Double(x, digits).EndArray();
        return j.str();
    };
    CheckEq(dbl(7.5), "[7.5]", "double: 7.5");
    CheckEq(dbl(9.0), "[9]", "double: 9");
    CheckEq(dbl(0.1, 6), "[0.1]", "double: 0.1 with six digits");
    CheckEq(dbl(-55.125), "[-55.125]", "double: negative");
    CheckEq(dbl(std::numeric_limits<double>::infinity()), "[null]", "double: infinity is null");
    CheckEq(dbl(-std::numeric_limits<double>::infinity()), "[null]", "double: minus infinity is null");
    CheckEq(dbl(std::numeric_limits<double>::quiet_NaN()), "[null]", "double: NaN is null");
    CheckEq(dbl(1e30, 6), "[1e+30]", "double: exponent form is JSON too");
    CheckEq(dbl(12345.678, 3), "[1.23e+04]", "double: digits are significant digits");

    // the documents the tool says about itself
    Doc p;
    p.raw = ot::RenderProtocol(false);
    std::string e2;
    Check(json::Reader::Parse(p.raw, p.v, e2), "protocol: valid JSON " + e2);
    CheckEq(p.S("commands.0"), "servers", "protocol: servers is still first");
    CheckEq(p.S("commands.1"), "rmr", "protocol: rmr");
    CheckEq(p.S("commands.2"), "lb", "protocol: lb");
    CheckEq(p.S("commands.3"), "tournament", "protocol: tournament");
    CheckEq(p.S("subcommands.rmr.4"), "rate", "protocol: rmr rate is listed");
    CheckInt(p.Len("subcommands.lb"), 5, "protocol: five lb commands");
    CheckInt(p.Len("subcommands.tournament"), 4, "protocol: four tournament commands");
    CheckEq(p.S("writes.0"), "rmr rate", "protocol: the one command that writes");
    CheckInt(p.Len("writes"), 1, "protocol: only one writes");
    CheckInt(p.N("remote.default_port"), 31002, "protocol: the server's port");
    CheckInt(p.N("remote.connect_timeout_ms"), 3000, "protocol: the game's connect timeout for the server");
    CheckInt(p.N("remote.io_timeout_ms"), 15000, "protocol: the game's I/O timeout for the server");
    CheckInt(p.N("remote.max_requests_per_call"), 40, "protocol: the request budget");
    CheckEq(p.S("remote.rmr.magic"), "RMR1", "protocol: rmr magic");
    CheckEq(p.S("remote.rmr.path"), "/rmr/v1", "protocol: rmr path");
    CheckInt(p.N("remote.rmr.max_page"), 50, "protocol: rmr page");
    CheckInt(p.N("remote.rmr.min_ranked"), 3, "protocol: the game ranks from 3 ratings");
    CheckInt(p.N("remote.rmr.max_photo_bytes"), 204799, "protocol: photo cap");
    CheckEq(p.S("remote.lb.magic"), "LBD1", "protocol: lb magic");
    CheckEq(p.S("remote.lb.path"), "/lb/v1", "protocol: lb path");
    CheckInt(p.N("remote.lb.max_rows"), 100, "protocol: lb rows");
    CheckInt(p.N("remote.lb.max_owners"), 100, "protocol: lb owners");
    CheckInt(p.N("remote.tournament.week_seconds"), 604800, "protocol: a week");
    CheckInt(p.N("remote.tournament.first_monday"), 345600, "protocol: the first Monday");
    CheckInt(p.N("remote.tournament.definition_bytes"), 1556, "protocol: the definition file");
    CheckEq(p.S("remote.tournament.rewards.0.name"), "Audi R8", "protocol: reward 0");
    CheckEq(p.S("remote.tournament.rewards.2.name"), "hydraulics", "protocol: reward 2");
    CheckInt(p.N("remote.tournament.slots.2"), 351, "protocol: the second slot range");
    Check(p.raw.find("26.") == std::string::npos, "protocol: no address of any server");
    Check(ot::RenderVersion(false).find("\"version\":\"0.2.0\"") != std::string::npos, "version: 0.2.0 (the commands were added)");
}

// ---- recorded answers ------------------------------------------------------------------------------------------------

struct Recorded {
    std::map<int, Bytes> rmr;  // op -> the answer
    std::map<int, Bytes> lb;
    std::map<uint32_t, Bytes> files;  // file id -> the answer to a download
    Bytes missing;
};

void RecordedHandler(const Recorded& r, const HttpRequest& req, HttpReply& reply) {
    if (req.path == lb::kHttpPath) {
        lb::Request q;
        if (!lb::DecodeRequest(req.body, q)) {
            reply.status = 400;
            return;
        }
        auto it = r.lb.find(int(q.op));
        if (it == r.lb.end()) {
            reply.status = 500;
            return;
        }
        reply.body = it->second;
        return;
    }
    rmr::Request q;
    if (req.path != rmr::kHttpPath || !rmr::DecodeRequest(req.body, q)) {
        reply.status = 400;
        return;
    }
    if (q.op == rmr::Op::kDownload) {
        auto f = r.files.find(q.file_id);
        reply.body = f == r.files.end() ? r.missing : f->second;
        return;
    }
    auto it = r.rmr.find(int(q.op));
    if (it == r.rmr.end()) {
        reply.status = 500;
        return;
    }
    reply.body = it->second;
}

void CheckRecordedAnswers() {
    Recorded r;
    r.rmr[int(rmr::Op::kTableSize)] = FromHex(rec::kRmrTableSize);
    r.rmr[int(rmr::Op::kByRank)] = FromHex(rec::kRmrByRank);
    r.rmr[int(rmr::Op::kByOwner)] = FromHex(rec::kRmrByOwner);
    r.rmr[int(rmr::Op::kTournament)] = FromHex(rec::PadHexTo(rec::kRmrTournament, rec::kRmrTournamentBytes));
    r.rmr[int(rmr::Op::kRewards)] = FromHex(rec::kRmrRewardsAll);
    r.rmr[int(rmr::Op::kRate)] = FromHex(rec::kRmrRated);
    r.lb[int(lb::Op::kSize)] = FromHex(rec::kLbSize);
    r.lb[int(lb::Op::kByRank)] = FromHex(rec::kLbByRank);
    r.lb[int(lb::Op::kByPivot)] = FromHex(rec::kLbByPivot);
    r.lb[int(lb::Op::kByOwners)] = FromHex(rec::kLbByOwners);
    r.files[rec::kAlphaPhoto1] = FromHex(rec::kRmrDownload);
    r.missing = FromHex(rec::kRmrDownloadMissing);

    Check(FromHex(rec::PadHexTo(rec::kRmrTournament, rec::kRmrTournamentBytes)).size() == rec::kRmrTournamentBytes, "recorded: the tournament answer is padded to its size");

    // The recorded bytes are what the codecs of the game read.
    {
        rmr::Response x;
        Check(rmr::DecodeResponse(FromHex(rec::kRmrByRank), x) && x.records.size() == 2, "recorded: the codec reads the ride page");
        lb::Response y;
        Check(lb::DecodeResponse(FromHex(rec::kLbByRank), y) && y.rows.size() == 3, "recorded: the codec reads the board page");
    }

    HttpServer srv;
    srv.handler = [&](const HttpRequest& q, HttpReply& a) { RecordedHandler(r, q, a); };
    srv.Start();
    const std::string at = srv.Address();
    const std::string t1 = ot::Hex64Text(rec::kOwnerT1), t2 = ot::Hex64Text(rec::kOwnerT2), t3 = ot::Hex64Text(rec::kOwnerT3);
    const fs::path photos = ScratchDir() / fs::path(std::u8string(u8"recorded-photos \u00e9"));

    // ---- Rate My Ride
    {
        Doc d = Run({"rmr", "table", "--server", at});
        CheckInt(d.exit_code, 0, "recorded table: exit code");
        Check(d.B("ok"), "recorded table: ok");
        CheckEq(d.S("command"), "rmr", "recorded table: command");
        CheckEq(d.S("sub"), "table", "recorded table: sub");
        CheckInt(d.N("schema"), ot::kSchema, "recorded table: schema");
        CheckEq(d.S("server.state"), "ok", "recorded table: the server answered");
        CheckEq(d.S("server.address"), at, "recorded table: the address");
        CheckInt(d.N("server.http_status"), 200, "recorded table: HTTP 200");
        CheckInt(d.N("server.err"), 0, "recorded table: no error code");
        CheckInt(d.N("server.requests"), 1, "recorded table: one request");
        CheckInt(d.N("ranked"), 2, "recorded table: two ranked rides");
        CheckInt(d.N("min_ratings"), 3, "recorded table: the game's minimum");
    }
    {
        Doc d = Run({"rmr", "top", "--server", at});
        CheckEq(d.S("server.state"), "ok", "recorded top: state");
        CheckInt(d.N("server.requests"), 2, "recorded top: size and page");
        CheckInt(d.N("total_ranked"), 2, "recorded top: total");
        CheckInt(d.N("start"), 1, "recorded top: start");
        CheckInt(d.N("count"), 10, "recorded top: the default page");
        CheckInt(d.Len("rides"), 2, "recorded top: two rides");
        CheckEq(d.S("rides.0.id"), "52000001", "recorded top: Alpha's id");
        CheckEq(d.S("rides.0.owner"), t1, "recorded top: Alpha's owner key");
        Check(!d.B("rides.0.mine"), "recorded top: not mine without an owner");
        CheckEq(d.S("rides.0.gamertag"), "Alpha", "recorded top: gamertag");
        CheckInt(d.N("rides.0.ratings"), 3, "recorded top: ratings");
        CheckNear(d.D("rides.0.average"), 9.0, 0.001, "recorded top: average");
        CheckInt(d.N("rides.0.rank"), 1, "recorded top: rank");
        Check(d.B("rides.0.ranked"), "recorded top: ranked");
        CheckInt(d.N("rides.0.content"), 3, "recorded top: content bit");
        Check(d.N("rides.0.created") > 1700000000 && d.N("rides.0.updated") >= d.N("rides.0.created"), "recorded top: times");
        CheckInt(d.N("rides.0.car_config_bytes"), 64, "recorded top: the car's configuration size");
        CheckInt(d.N("rides.0.sale.vehicle"), 300, "recorded top: sale vehicle");
        CheckInt(d.N("rides.0.sale.price"), 5000, "recorded top: sale price");
        CheckInt(d.N("rides.0.sale.purchases"), 2, "recorded top: sale purchases");
        CheckInt(d.N("rides.0.sale.pending_reward"), 1, "recorded top: pending reward");
        Check(d.B("rides.0.sale.for_sale"), "recorded top: for sale");
        CheckInt(d.Len("rides.0.photos"), 2, "recorded top: two photo slots");
        CheckInt(d.N("rides.0.photos.0.slot"), 1, "recorded top: photo slot 1");
        CheckEq(d.S("rides.0.photos.0.file_id"), "52000002", "recorded top: photo 1's id");
        CheckInt(d.N("rides.0.photos.0.size"), 120, "recorded top: photo 1's size");
        CheckEq(d.S("rides.0.photos.0.state"), "not_requested", "recorded top: no folder, no photo");
        Check(d.IsNull("rides.0.photos.0.file"), "recorded top: no file");
        CheckEq(d.S("rides.0.photos.1.file_id"), "52000003", "recorded top: photo 2's id");
        CheckInt(d.N("rides.0.photos.1.size"), 90, "recorded top: photo 2's size");
        CheckEq(d.S("rides.1.gamertag"), "Bravo", "recorded top: Bravo");
        CheckNear(d.D("rides.1.average"), 6.0, 0.001, "recorded top: Bravo's average");
        CheckInt(d.N("rides.1.rank"), 2, "recorded top: Bravo's rank");
        Check(d.IsNull("rides.1.sale"), "recorded top: no sale data");
        CheckEq(d.S("rides.1.photos.0.file_id"), "52000005", "recorded top: Bravo's photo");
        CheckEq(d.S("rides.1.photos.1.state"), "none", "recorded top: Bravo has no second photo");
        CheckInt(d.N("rides.1.photos.1.size"), 0, "recorded top: no second photo size");
    }
    {
        Doc d = Run({"rmr", "top", "--server", at, "--owner", t2});
        Check(!d.B("rides.0.mine") && d.B("rides.1.mine"), "recorded top: Bravo is mine with the owner key");
    }
    {
        Doc d = Run({"rmr", "top", "--server", at, "--photos", Utf8(photos), "--photo-slots", "2"});
        CheckInt(d.N("server.requests"), 5, "recorded photos: two reads and three downloads");
        CheckEq(d.S("rides.0.photos.0.state"), "saved", "recorded photos: the first is kept");
        CheckEq(d.S("rides.0.photos.0.file"), "52000002-120.jpg", "recorded photos: its name");
        CheckEq(d.S("rides.0.photos.1.state"), "unavailable", "recorded photos: the server has no second one");
        CheckEq(d.S("rides.0.photos.1.error"), "the server no longer has the photo", "recorded photos: and says so");
        CheckEq(d.S("rides.1.photos.0.state"), "unavailable", "recorded photos: nor Bravo's");
        CheckEq(HexOf(ReadFileBytes(photos / "52000002-120.jpg")), std::string(rec::kRmrDownload).substr(56), "recorded photos: the file is the JPEG the server sent");
        Doc again = Run({"rmr", "top", "--server", at, "--photos", Utf8(photos)});
        CheckInt(again.N("server.requests"), 3, "recorded photos: a photo already kept is not fetched again (one slot: 2 + 1)");
        CheckEq(again.S("rides.0.photos.0.state"), "cached", "recorded photos: cached");
        CheckEq(again.S("rides.0.photos.1.state"), "not_requested", "recorded photos: the second slot was not asked for");
    }
    {
        Doc d = Run({"rmr", "mine", "--server", at, "--owner", t2});
        Check(d.B("found"), "recorded mine: found");
        CheckEq(d.S("owner"), t2, "recorded mine: the owner");
        CheckEq(d.S("ride.gamertag"), "Bravo", "recorded mine: Bravo's ride");
        Check(d.B("ride.mine"), "recorded mine: marked mine");
        Doc none = Run({"rmr", "mine", "--server", at, "--owner", "1234567800000001"});
        Check(none.Has("found") && !none.B("found"), "recorded mine: nobody's ride is not found");
        Check(none.IsNull("ride"), "recorded mine: ride is null");
        const std::string token = TokHex(0x22);
        Doc by_token = Run({"rmr", "mine", "--server", at, "--token-stdin"}, &token);
        CheckEq(by_token.S("owner"), t2, "recorded mine: the owner key comes from the token");
        Check(by_token.B("found"), "recorded mine: found by token");
    }
    {
        Doc d = Run({"rmr", "photo", "--server", at, "--file", "52000002,52000009", "--out", Utf8(photos)});
        CheckInt(d.Len("photos"), 2, "recorded photo: two results");
        CheckEq(d.S("photos.0.state"), "saved", "recorded photo: saved");
        CheckEq(d.S("photos.0.file"), "52000002-120.jpg", "recorded photo: name");
        CheckInt(d.N("photos.0.size"), 120, "recorded photo: size");
        CheckEq(d.S("photos.1.state"), "unavailable", "recorded photo: a file that is not there");
        CheckInt(d.N("server.requests"), 2, "recorded photo: two requests");
    }
    {
        const std::string token = TokHex(0x11);
        r.rmr[int(rmr::Op::kRate)] = FromHex(rec::kRmrRated);
        Doc d = Run({"rmr", "rate", "--server", at, "--record", "52000002", "--rating", "10", "--confirm", "--token-stdin"}, &token);
        CheckEq(d.S("result"), "rated", "recorded rate: rated");
        CheckEq(d.S("record"), "52000002", "recorded rate: the ride");
        CheckInt(d.N("rating"), 10, "recorded rate: the rating");
        CheckInt(d.N("ratings"), 4, "recorded rate: ratings now");
        CheckNear(d.D("average"), 7.0, 0.001, "recorded rate: average now");
        r.rmr[int(rmr::Op::kRate)] = FromHex(rec::kRmrRateOwn);
        Doc own = Run({"rmr", "rate", "--server", at, "--record", "52000001", "--rating", "5", "--confirm", "--token-stdin"}, &token);
        CheckEq(own.S("result"), "refused", "recorded rate: the player's own ride is refused");
        Check(own.IsNull("ratings") && own.IsNull("average"), "recorded rate: no new numbers when refused");
        CheckEq(own.S("server.state"), "ok", "recorded rate: a refusal of the ride is not a failure of the server");
        r.rmr[int(rmr::Op::kRate)] = FromHex(rec::kRmrRateMissing);
        Doc gone = Run({"rmr", "rate", "--server", at, "--record", "52000fff", "--rating", "5", "--confirm", "--token-stdin"}, &token);
        CheckEq(gone.S("result"), "no_such_ride", "recorded rate: no such ride");
    }

    // ---- leaderboards
    {
        Doc d = Run({"lb", "size", "--server", at, "--view", "382,383"});
        CheckEq(d.S("command"), "lb", "recorded lb size: command");
        CheckEq(d.S("server.state"), "ok", "recorded lb size: state");
        CheckInt(d.Len("boards"), 2, "recorded lb size: two boards");
        CheckInt(d.N("boards.0.view"), 382, "recorded lb size: the first view");
        CheckInt(d.N("boards.0.total"), 3, "recorded lb size: its rows");
        CheckInt(d.N("boards.1.view"), 383, "recorded lb size: the second view");
        Check(d.IsNull("boards.0.name") && d.IsNull("boards.0.known"), "recorded lb size: no names without the tables file");
        CheckEq(d.S("tables.state"), "not_given", "recorded lb size: tables not given");
        CheckInt(d.N("server.requests"), 2, "recorded lb size: one request per view");
    }
    {
        Doc d = Run({"lb", "top", "--server", at, "--view", "382", "--owner", t1});
        CheckEq(d.S("server.state"), "ok", "recorded lb top: state");
        CheckInt(d.N("view.id"), 382, "recorded lb top: the view");
        CheckInt(d.N("total"), 3, "recorded lb top: total");
        CheckInt(d.Len("rows"), 3, "recorded lb top: three rows");
        CheckEq(d.S("rows.0.gamertag"), "Charlie", "recorded lb top: the best first");
        CheckEq(d.S("rows.0.owner"), t3, "recorded lb top: owner key");
        CheckInt(d.N("rows.0.rank"), 1, "recorded lb top: rank");
        CheckInt(d.N("rows.0.rating"), 150, "recorded lb top: rating");
        Check(d.N("rows.0.updated") > 1700000000, "recorded lb top: updated");
        CheckInt(d.Len("rows.0.columns"), 4, "recorded lb top: four columns");
        // the server sends a row's columns in attribute order: 1, 2, 3 and the rating column 0xFFFE last
        CheckInt(d.N("rows.0.columns.0.attr"), 1, "recorded lb top: attr 1");
        CheckEq(d.S("rows.0.columns.0.type"), "int32", "recorded lb top: its type");
        CheckInt(d.N("rows.0.columns.0.value"), -2, "recorded lb top: a negative int32");
        CheckInt(d.N("rows.0.columns.1.attr"), 2, "recorded lb top: attr 2");
        CheckEq(d.S("rows.0.columns.1.type"), "float", "recorded lb top: a float column");
        CheckNear(d.D("rows.0.columns.1.value"), 55.125, 0.0001, "recorded lb top: the float value");
        CheckEq(d.S("rows.0.columns.2.type"), "int64", "recorded lb top: an int64 column");
        CheckInt(d.N("rows.0.columns.2.value"), -7, "recorded lb top: a negative int64");
        CheckInt(d.N("rows.0.columns.3.attr"), 0xFFFE, "recorded lb top: the rating column");
        CheckEq(d.S("rows.0.columns.3.type"), "int32", "recorded lb top: its type");
        CheckInt(d.N("rows.0.columns.3.value"), 150, "recorded lb top: its value");
        Check(!d.B("rows.0.mine"), "recorded lb top: Charlie is not the player");
        CheckEq(d.S("rows.1.gamertag"), "Alpha", "recorded lb top: second");
        Check(d.B("rows.1.mine"), "recorded lb top: Alpha is the player");
        CheckNear(d.D("rows.1.columns.1.value"), 61.25, 0.0001, "recorded lb top: Alpha's float");
        CheckInt(d.N("rows.1.columns.2.value"), 5000000000ll, "recorded lb top: an int64 above 32 bits");
        CheckInt(d.N("rows.2.rank"), 3, "recorded lb top: third");
        CheckNear(d.D("rows.2.columns.1.value"), 70.5, 0.0001, "recorded lb top: Bravo's float");
        Check(!d.Has("rows.0.in_week"), "recorded lb top: no week on a plain board");
    }
    {
        Doc d = Run({"lb", "around", "--server", at, "--view", "382", "--owner", t1, "--span", "1"});
        Check(d.B("found"), "recorded lb around: the player is in the window");
        CheckInt(d.Len("rows"), 3, "recorded lb around: the window");
        const std::string tok1 = TokHex(0x11);
        Doc token_run = Run({"lb", "around", "--server", at, "--view", "382", "--token-stdin"}, &tok1);
        CheckEq(token_run.S("server.state"), "ok", "recorded lb around: by token");
    }
    {
        Doc d = Run({"lb", "owners", "--server", at, "--view", "382", "--owner", t2 + ",1234567800000002"});
        CheckInt(d.Len("rows"), 1, "recorded lb owners: the player with a row only");
        CheckEq(d.S("rows.0.gamertag"), "Bravo", "recorded lb owners: Bravo");
    }

    // ---- the tournament
    {
        const int64_t starts = tournament::kFirstMondayUnix + 2967 * tournament::kWeekSeconds;
        Doc d = Run({"tournament", "current", "--server", at, "--now", std::to_string(rec::kTournamentMoment)});
        CheckEq(d.S("server.state"), "ok", "recorded tournament: state");
        CheckInt(d.N("tournament.week"), 2967, "recorded tournament: week");
        CheckInt(d.N("tournament.version"), 1, "recorded tournament: file version");
        CheckInt(d.N("tournament.slot"), 22, "recorded tournament: slot");
        CheckEq(d.S("tournament.race_key"), "e68bb43f", "recorded tournament: race key");
        CheckEq(d.S("tournament.race"), "Hollywood Sprint 5", "recorded tournament: the rotation's race name");
        CheckEq(d.S("tournament.restriction.type"), "car_class", "recorded tournament: restriction");
        CheckInt(d.N("tournament.restriction.type_id"), 2, "recorded tournament: restriction id");
        Check(d.B("tournament.restriction.any_car"), "recorded tournament: any car");
        Check(d.IsNull("tournament.restriction.data"), "recorded tournament: no car data");
        CheckEq(d.S("tournament.description"), "Hollywood Sprint 5\nAny car. Ends Monday 00:00 UTC.\n1st: Audi R8, TIS rims, hydraulics.\n2nd-3rd: TIS rims, hydraulics.",
                "recorded tournament: description");
        CheckInt(d.Len("tournament.lines"), 4, "recorded tournament: four lines");
        CheckEq(d.S("tournament.lines.1"), "Any car. Ends Monday 00:00 UTC.", "recorded tournament: a line");
        CheckInt(d.Len("tournament.prizes"), 2, "recorded tournament: two prize lines");
        CheckEq(d.S("tournament.prizes.0.place"), "1st", "recorded tournament: first place");
        CheckInt(d.Len("tournament.prizes.0.rewards"), 3, "recorded tournament: three prizes for first");
        CheckEq(d.S("tournament.prizes.1.place"), "2nd-3rd", "recorded tournament: podium");
        CheckEq(d.S("tournament.prizes.1.rewards.0"), "TIS rims", "recorded tournament: a podium prize");
        CheckInt(d.N("tournament.starts"), starts, "recorded tournament: the week starts on Monday 00:00 UTC");
        CheckInt(d.N("tournament.ends"), starts + 604800, "recorded tournament: and ends a week later");
        CheckInt(d.N("tournament.now"), rec::kTournamentMoment, "recorded tournament: the clock given");
        Check(d.B("tournament.active"), "recorded tournament: active");
        CheckInt(d.N("tournament.seconds_left"), starts + 604800 - rec::kTournamentMoment, "recorded tournament: time left");
        Doc before = Run({"tournament", "current", "--server", at, "--now", std::to_string(starts - 10)});
        Check(!before.B("tournament.active"), "recorded tournament: not active before the week");
        CheckInt(before.N("tournament.seconds_left"), 604810, "recorded tournament: time left counts from the clock");
        Doc after = Run({"tournament", "current", "--server", at, "--now", std::to_string(starts + 604800 + 5)});
        Check(!after.B("tournament.active"), "recorded tournament: not active after the week");
        CheckInt(after.N("tournament.seconds_left"), 0, "recorded tournament: no time left");
    }
    {
        Doc d = Run({"tournament", "standings", "--server", at, "--now", std::to_string(rec::kTournamentMoment), "--owner", t2});
        CheckEq(d.S("server.state"), "ok", "recorded standings: state");
        CheckInt(d.N("server.requests"), 3, "recorded standings: the week, the page, the player's row");
        CheckInt(d.N("tournament.slot"), 22, "recorded standings: the week's slot");
        CheckInt(d.N("board.view.id"), 22, "recorded standings: the board is the slot's view");
        CheckEq(d.S("board.view_basis"), "slot", "recorded standings: by the slot");
        CheckInt(d.Len("board.candidates"), 1, "recorded standings: one candidate for a slot below 351");
        CheckInt(d.N("board.total"), 3, "recorded standings: total");
        CheckInt(d.Len("board.rows"), 3, "recorded standings: rows");
        Check(d.B("board.rows.0.in_week"), "recorded standings: written in the week");
        Check(d.B("board.rows.1.in_week") && d.B("board.rows.2.in_week"), "recorded standings: all written in the week");
        CheckEq(d.S("board.mine.gamertag"), "Bravo", "recorded standings: the player's row");
        Check(d.B("board.mine.mine"), "recorded standings: marked");
        Doc forced = Run({"tournament", "standings", "--server", at, "--now", std::to_string(rec::kTournamentMoment), "--view", "400"});
        CheckEq(forced.S("board.view_basis"), "override", "recorded standings: a view given");
        CheckInt(forced.N("board.view.id"), 400, "recorded standings: the view given");
        Check(forced.IsNull("board.mine"), "recorded standings: no player, no row of theirs");
    }
    {
        const std::string token = TokHex(0x11);
        Doc d = Run({"tournament", "rewards", "--server", at, "--token-stdin"}, &token);
        CheckEq(d.S("server.state"), "ok", "recorded rewards: state");
        CheckInt(d.N("rewards.mask"), 7, "recorded rewards: all three");
        CheckEq(d.S("rewards.items.0.name"), "Audi R8", "recorded rewards: item 0");
        Check(d.B("rewards.items.0.won") && d.B("rewards.items.1.won") && d.B("rewards.items.2.won"), "recorded rewards: all won");
        CheckInt(d.N("rewards.other_bits"), 0, "recorded rewards: no other bit");
        r.rmr[int(rmr::Op::kRewards)] = FromHex(rec::kRmrRewardsRims);
        Doc rims = Run({"tournament", "rewards", "--server", at, "--token-stdin"}, &token);
        CheckInt(rims.N("rewards.mask"), 2, "recorded rewards: the rims only");
        Check(!rims.B("rewards.items.0.won") && rims.B("rewards.items.1.won") && !rims.B("rewards.items.2.won"), "recorded rewards: only the rims");
        r.rmr[int(rmr::Op::kRewards)] = FromHex(rec::kRmrRewardsNone);
        Doc none = Run({"tournament", "rewards", "--server", at, "--token-stdin"}, &token);
        CheckInt(none.N("rewards.mask"), 0, "recorded rewards: none");
        Check(!none.B("rewards.items.0.won") && !none.B("rewards.items.1.won") && !none.B("rewards.items.2.won"), "recorded rewards: nothing won");
    }
    {
        Doc d = Run({"tournament", "next", "--now", std::to_string(rec::kTournamentMoment), "--weeks", "3"});
        CheckEq(d.S("command"), "tournament", "next: command");
        Check(d.IsNull("server"), "next: no server is asked");
        CheckEq(d.S("source"), "default_rotation", "next: the source");
        Check(d.B("server_can_change"), "next: the server can change it");
        CheckInt(d.Len("weeks"), 3, "next: three weeks");
        const tournament::Definition cur = tournament::Current(rec::kTournamentMoment, {});
        CheckInt(cur.table_id, 22, "next: the rotation gives the recorded slot for the recorded week");
        CheckEq(cur.name, "Hollywood Sprint 5", "next: and its race");
        CheckInt(d.N("weeks.0.week"), 2968, "next: the week after");
        CheckInt(d.N("weeks.0.starts"), cur.ends, "next: starts where this one ends");
        const tournament::Definition n1 = tournament::Current(cur.ends, {});
        CheckInt(d.N("weeks.0.slot"), n1.table_id, "next: slot");
        CheckEq(d.S("weeks.0.race"), n1.name, "next: race");
        CheckEq(d.S("weeks.0.race_key"), ot::Hex32Text(n1.race_key), "next: race key");
        CheckInt(d.N("weeks.2.week"), 2970, "next: the third week");
        CheckInt(d.N("weeks.1.starts"), d.N("weeks.0.ends"), "next: weeks follow each other");
        CheckInt(d.Len("weeks.0.prizes.first"), 3, "next: three prizes for first");
        CheckInt(d.Len("weeks.0.prizes.podium"), 2, "next: two prizes for the podium");
        CheckInt(d.N("weeks.0.prizes.podium_places"), 3, "next: three places");
        // every slot the rotation hands out is one the game accepts: 13..39 or 351..381
        bool slots_ok = true;
        std::set<std::string> races;
        for (int w = 0; w < 200; ++w) {
            const tournament::Definition x = tournament::Current(tournament::kFirstMondayUnix + int64_t(2900 + w) * tournament::kWeekSeconds, {});
            slots_ok = slots_ok && ((x.table_id >= 13 && x.table_id < 40) || (x.table_id >= 351 && x.table_id < 382));
            races.insert(x.name);
            slots_ok = slots_ok && !tournament::RaceName(x.race_key).empty();
        }
        Check(slots_ok, "next: the rotation only uses the game's slots and known races");
        CheckInt(races.size(), 76, "next: every race of the rotation comes up");
    }
}

// ---- hostile answers -------------------------------------------------------------------------------------------------

rmr::Record MakeRecord(uint32_t id, uint64_t owner, const std::string& tag) {
    rmr::Record r;
    r.id = id;
    r.owner = owner;
    r.gamertag = tag;
    r.file_count = 2;
    r.file_ids = {id, id + 1, 0};
    r.file_sizes = {64, 40, 0};
    r.num_ratings = 5;
    r.average = 7.5f;
    r.rank = 1;
    r.created = 1700000000;
    r.updated = 1700000100;
    return r;
}

void CheckHostile() {
    // ---- no server set: every command says so and asks nobody
    {
        const std::string token = TokHex(0x11);
        struct Case {
            std::vector<std::string> words;
            bool token;
        };
        const std::vector<Case> cases = {
            {{"rmr", "table"}, false},
            {{"rmr", "top"}, false},
            {{"rmr", "mine", "--owner", "0c50476a8dc2d715"}, false},
            {{"rmr", "photo", "--file", "52000002", "--out", "x"}, false},
            {{"rmr", "rate", "--record", "52000001", "--rating", "5", "--confirm", "--token-stdin"}, true},
            {{"lb", "size", "--view", "1,2"}, false},
            {{"lb", "top", "--view", "1"}, false},
            {{"lb", "around", "--view", "1", "--owner", "0c50476a8dc2d715"}, false},
            {{"lb", "owners", "--view", "1", "--owner", "0c50476a8dc2d715"}, false},
            {{"tournament", "current"}, false},
            {{"tournament", "standings"}, false},
            {{"tournament", "rewards", "--token-stdin"}, true},
        };
        for (const Case& c : cases) {
            Doc d = Run(c.words, c.token ? &token : nullptr);
            const std::string what = "no server: " + c.words[0] + " " + c.words[1];
            CheckInt(d.exit_code, 0, what + ": exit code 0 (not a usage error)");
            Check(d.B("ok"), what + ": ok");
            CheckEq(d.S("server.state"), "not_set", what + ": not set");
            Check(d.IsNull("server.address"), what + ": no address");
            CheckInt(d.N("server.requests"), 0, what + ": nobody asked");
            Check(!d.S("server.message").empty(), what + ": says why");
        }
        Doc relay = Run({"rmr", "table", "--relay", "127.0.0.1:31000", "--connect-timeout-ms", "300", "--port", "1"});
        CheckEq(relay.S("server.address"), "127.0.0.1:1", "relay: the relay's host with the server's port");
        Doc both = Run({"rmr", "table", "--server", "127.0.0.1:2", "--relay", "127.0.0.1:31000", "--connect-timeout-ms", "300"});
        CheckEq(both.S("server.address"), "127.0.0.1:2", "relay: the server given wins");
        Doc view = Run({"lb", "views"});
        Check(view.IsNull("server"), "lb views asks no server");
        CheckEq(view.S("tables.state"), "not_given", "lb views: no tables file");
        CheckInt(view.Len("views"), 0, "lb views: no views without the file");
    }

    // ---- the server cannot be reached
    {
        const uint16_t port = ClosedTcpPort();
        Doc d = Run({"rmr", "top", "--server", "127.0.0.1:" + std::to_string(port), "--connect-timeout-ms", "500"});
        Check(d.S("server.state") == "unreachable" || d.S("server.state") == "timed_out", "closed port: not reachable (" + d.S("server.state") + ")");
        CheckInt(d.N("server.requests"), 1, "closed port: one try, no retry");
        Check(d.IsNull("total_ranked"), "closed port: no total");
        CheckInt(d.Len("rides"), 0, "closed port: no rides");
        Check(!d.S("server.detail").empty(), "closed port: the transport's reason");
        Check(d.N("server.elapsed_ms") < 3000, "closed port: it did not wait long");
    }
    {
        HttpServer hang;
        hang.handler = [](const HttpRequest&, HttpReply& r) { r.hang = true; };
        hang.Start();
        const auto t0 = std::chrono::steady_clock::now();
        Doc d = Run({"rmr", "table", "--server", hang.Address(), "--io-timeout-ms", "300"});
        const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        CheckEq(d.S("server.state"), "timed_out", "hang: timed out");
        Check(ms < 2500, "hang: the I/O timeout given is the one kept (" + std::to_string(ms) + " ms)");
        Doc more = Run({"lb", "size", "--server", hang.Address(), "--view", "1,2,3", "--io-timeout-ms", "300"});
        CheckInt(more.N("server.requests"), 1, "hang: after a failure nothing more is asked");
        CheckInt(more.Len("boards"), 3, "hang: every board is still listed");
        Check(more.IsNull("boards.0.total") && more.IsNull("boards.2.total"), "hang: with no totals");
    }

    // ---- HTTP errors
    {
        HttpServer srv;
        std::atomic<int> status{500};
        srv.handler = [&](const HttpRequest&, HttpReply& r) { r.status = status; };
        srv.Start();
        Doc d = Run({"rmr", "table", "--server", srv.Address()});
        CheckEq(d.S("server.state"), "http_error", "HTTP 500: http_error");
        CheckInt(d.N("server.http_status"), 500, "HTTP 500: the status");
        status = 404;
        Doc nf = Run({"rmr", "top", "--server", srv.Address()});
        CheckEq(nf.S("server.state"), "http_error", "HTTP 404 on rmr: http_error");
        CheckInt(nf.N("server.http_status"), 404, "HTTP 404 on rmr: the status");
        Doc lbd = Run({"lb", "size", "--server", srv.Address(), "--view", "5"});
        CheckEq(lbd.S("server.state"), "no_service", "HTTP 404 on lb: an older server without leaderboards");
        CheckInt(lbd.N("server.http_status"), 404, "HTTP 404 on lb: the status");
        Check(lbd.S("server.message").find("older") != std::string::npos, "HTTP 404 on lb: the message names the cause");
        status = 500;
        Doc lb500 = Run({"lb", "top", "--server", srv.Address(), "--view", "5"});
        CheckEq(lb500.S("server.state"), "http_error", "HTTP 500 on lb: http_error");
    }

    // ---- answers that are not answers
    {
        HttpServer srv;
        Bytes body;
        srv.handler = [&](const HttpRequest&, HttpReply& r) { r.body = body; };
        srv.Start();
        auto state = [&](const Bytes& b, const std::vector<std::string>& cmd) {
            body = b;
            std::vector<std::string> full = cmd;
            full.push_back("--server");
            full.push_back(srv.Address());
            return Run(full).S("server.state");
        };
        const Bytes good = FromHex(rec::kRmrTableSize);
        const std::vector<std::string> tbl = {"rmr", "table"};
        CheckEq(state(good, tbl), "ok", "bad answers: the good one is good");
        CheckEq(state({}, tbl), "bad_answer", "bad answers: empty");
        CheckEq(state({'h', 'e', 'l', 'l', 'o'}, tbl), "bad_answer", "bad answers: text");
        Bytes truncated = good;
        truncated.pop_back();
        CheckEq(state(truncated, tbl), "bad_answer", "bad answers: one byte short");
        Bytes extra = good;
        extra.push_back(0);
        CheckEq(state(extra, tbl), "bad_answer", "bad answers: one byte over");
        Bytes magic = good;
        magic[0] ^= 1;
        CheckEq(state(magic, tbl), "bad_answer", "bad answers: wrong magic");
        Bytes lbmagic = FromHex(rec::kLbSize);
        CheckEq(state(lbmagic, tbl), "bad_answer", "bad answers: a leaderboards answer to a Rate My Ride question");
        CheckEq(state(good, {"lb", "size", "--view", "3"}), "bad_answer", "bad answers: a Rate My Ride answer to a leaderboards question");
        // a page that claims 300 records
        Bytes many = good;
        many[20] = 0x2C;
        many[21] = 0x01;
        CheckEq(state(many, tbl), "bad_answer", "bad answers: 300 records claimed");
        // a page of 255 records claimed with none there
        Bytes claim = good;
        claim[20] = 0xFF;
        CheckEq(state(claim, tbl), "bad_answer", "bad answers: records claimed and missing");
        Bytes huge(2 * 1024 * 1024, 0x41);
        CheckEq(state(huge, tbl), "unreachable", "bad answers: a body over the game's 1 MB cap is not read");
        body = good;
    }
    {
        HttpServer srv;
        srv.handler = [&](const HttpRequest&, HttpReply& r) {
            r.body = FromHex(rec::kRmrTableSize);
            r.announce_length = int64_t(r.body.size()) + 50;  // the server closes early
        };
        srv.Start();
        Doc d = Run({"rmr", "table", "--server", srv.Address()});
        CheckEq(d.S("server.state"), "unreachable", "truncated HTTP body: unreachable");
        HttpServer junk;
        junk.handler = [&](const HttpRequest&, HttpReply& r) { r.raw = "this is not HTTP at all\r\n\r\n"; };
        junk.Start();
        Doc j = Run({"rmr", "table", "--server", junk.Address()});
        Check(j.S("server.state") != "ok", "not HTTP: not ok (" + j.S("server.state") + ")");
    }

    // ---- the server refuses
    {
        HttpServer srv;
        uint32_t err = 12;
        srv.handler = [&](const HttpRequest& q, HttpReply& r) {
            if (q.path == lb::kHttpPath) {
                lb::Response x;
                x.err = err;
                r.body = lb::EncodeResponse(x);
            } else {
                rmr::Response x;
                x.err = err;
                r.body = rmr::EncodeResponse(x);
            }
        };
        srv.Start();
        Doc d = Run({"rmr", "table", "--server", srv.Address()});
        CheckEq(d.S("server.state"), "refused", "refused: state");
        CheckInt(d.N("server.err"), 12, "refused: the code");
        Check(!d.S("server.message").empty(), "refused: the message");
        Check(d.IsNull("ranked"), "refused: no number");
        Doc top = Run({"rmr", "top", "--server", srv.Address()});
        CheckEq(top.S("server.state"), "refused", "refused: top");
        CheckInt(top.N("server.requests"), 1, "refused: the page is not asked after the size is refused");
        err = 1;
        Doc lbd = Run({"lb", "top", "--server", srv.Address(), "--view", "5"});
        CheckEq(lbd.S("server.state"), "refused", "refused: lb");
        CheckInt(lbd.N("server.err"), 1, "refused: lb code");
        Check(lbd.IsNull("total"), "refused: lb no total");
        CheckInt(lbd.Len("rows"), 0, "refused: lb no rows");
        err = 13;
        const std::string tok1 = TokHex(0x11);
        Doc rate = Run({"rmr", "rate", "--server", srv.Address(), "--record", "52000001", "--rating", "5", "--confirm", "--token-stdin"}, &tok1);
        CheckEq(rate.S("result"), "error", "refused: a rate that fails");
        CheckEq(rate.S("server.state"), "refused", "refused: and the server says so");
    }

    // ---- hostile text and shapes in rides
    {
        HttpServer srv;
        std::atomic<int> mode{0};
        const uint64_t ow = 0x00ABCDEF12345678ull;
        srv.handler = [&](const HttpRequest& q, HttpReply& r) {
            rmr::Request req;
            rmr::DecodeRequest(q.body, req);
            rmr::Response x;
            if (req.op == rmr::Op::kTableSize) {
                x.count = 9;
            } else if (req.op == rmr::Op::kByRank) {
                if (mode == 0) {
                    x.records.push_back(MakeRecord(0x52000001, ow, std::string("A\x01\x1b[31mB\xff\xfe\"q\\t\x7f", 15)));
                } else if (mode == 1) {  // more than asked
                    for (uint32_t i = 0; i < 20; ++i) x.records.push_back(MakeRecord(0x52000100 + i * 4, ow + i, "R" + std::to_string(i)));
                } else if (mode == 2) {  // hostile photo records
                    rmr::Record a = MakeRecord(0x52000010, ow, "Big");
                    a.file_sizes[1] = rmr::kMaxPhotoBytes + 1;
                    rmr::Record b = MakeRecord(0x52000020, ow + 1, "Wrong");
                    b.file_sizes[1] = 50;  // the data will be 40 bytes
                    rmr::Record c = MakeRecord(0x52000030, ow + 2, "Png");
                    rmr::Record d = MakeRecord(0x52000040, ow + 3, "NoPhotos");
                    d.file_count = 1;
                    d.file_sizes[1] = 0;
                    d.file_ids[1] = 0;
                    rmr::Record e = MakeRecord(0x52000050, ow + 4, "ZeroId");
                    e.file_ids[1] = 0;
                    x.records = {a, b, c, d, e};
                } else if (mode == 3) {  // userdata short and long; weird floats
                    rmr::Record a = MakeRecord(0x52000060, ow, "Short");
                    a.userdata = {1, 2, 3};
                    rmr::Record b = MakeRecord(0x52000061, ow + 1, "Exact");
                    b.userdata.assign(20, 0);
                    b.userdata[19] = 1;
                    b.average = 0.0f / 0.0f;
                    b.rank = 0;
                    rmr::Record c = MakeRecord(0x52000062, ow + 2, "");
                    c.average = 1.0f / 0.0f;
                    x.records = {a, b, c};
                }
            } else if (req.op == rmr::Op::kDownload) {
                if (req.file_id == 0x52000021) {
                    x.data = Jpeg(1, 40);
                } else if (req.file_id == 0x52000031) {
                    x.data = {0x89, 'P', 'N', 'G', 0, 0, 0, 0, 0, 0};
                } else {
                    x.data = Jpeg(2, 40);
                }
            }
            r.body = rmr::EncodeResponse(x);
        };
        srv.Start();
        const std::string at = srv.Address();
        {
            Doc d = Run({"rmr", "top", "--server", at});
            const std::string tag = d.S("rides.0.gamertag");
            CheckEq(tag, "A[31mB\"q\\t", "hostile ride: controls and invalid bytes are cleaned");
            CheckEq(d.S("rides.0.owner"), "00abcdef12345678", "hostile ride: the owner key is 16 digits");
            Check(d.raw.find('\x1b') == std::string::npos && d.raw.find('\x01') == std::string::npos, "hostile ride: no control character in the document");
        }
        mode = 1;
        {
            Doc d = Run({"rmr", "top", "--server", at, "--count", "3"});
            CheckInt(d.Len("rides"), 3, "too many rides: cut to the page asked");
            Doc all = Run({"rmr", "top", "--server", at, "--count", "50"});
            CheckInt(all.Len("rides"), 20, "too many rides: what came is kept up to the page");
        }
        mode = 2;
        {
            const fs::path dir = ScratchDir() / "hostile-photos";
            Doc d = Run({"rmr", "top", "--server", at, "--photos", Utf8(dir), "--photo-slots", "2"});
            CheckEq(d.S("rides.0.photos.0.state"), "too_large", "hostile photos: an announced size over the cap is never asked for");
            CheckEq(d.S("rides.1.photos.0.state"), "invalid", "hostile photos: a size that is not the announced one");
            Check(d.S("rides.1.photos.0.error").find("size") != std::string::npos, "hostile photos: and says so");
            CheckEq(d.S("rides.2.photos.0.state"), "invalid", "hostile photos: a PNG is not a JPEG");
            CheckEq(d.S("rides.3.photos.0.state"), "none", "hostile photos: a ride without photos");
            CheckEq(d.S("rides.4.photos.0.state"), "none", "hostile photos: a photo of id 0 is none");
            std::error_code ec;
            size_t files = 0;
            for (const auto& e : fs::directory_iterator(dir, ec)) {
                (void)e;
                ++files;
            }
            CheckInt(files, 0, "hostile photos: nothing is kept (the one valid photo slot has no match)");
        }
        mode = 3;
        {
            Doc d = Run({"rmr", "top", "--server", at});
            Check(d.IsNull("rides.0.sale"), "hostile userdata: 3 bytes are no sale data");
            Check(d.Has("rides.1.sale") && !d.IsNull("rides.1.sale") && d.B("rides.1.sale.for_sale"), "hostile userdata: exactly 20 bytes are sale data");
            Check(d.IsNull("rides.1.average"), "hostile average: NaN is null");
            Check(d.IsNull("rides.2.average"), "hostile average: infinity is null");
            CheckEq(d.S("rides.2.gamertag"), "", "hostile ride: an empty gamertag stays empty");
            Check(!d.B("rides.1.ranked"), "hostile ride: rank 0 is not ranked");
        }
    }

    // ---- hostile boards
    {
        HttpServer srv;
        std::atomic<int> mode{0};
        srv.handler = [&](const HttpRequest& q, HttpReply& r) {
            lb::Request req;
            lb::DecodeRequest(q.body, req);
            lb::Response x;
            x.total = 1000;
            auto val = [](uint16_t attr, uint8_t type, int64_t bits) {
                lb::Value v;
                v.attr = attr;
                v.type = type;
                v.bits = bits;
                return v;
            };
            if (mode == 0) {
                for (uint32_t i = 0; i < 150; ++i) {
                    lb::Row row;
                    row.owner = 0x1000 + i;
                    row.gamertag = "P" + std::to_string(i);
                    row.rank = i + 1;
                    x.rows.push_back(row);
                }
            } else {
                lb::Row row;
                row.owner = 0x2000;
                row.gamertag = std::string("Hos\x01tile\xC3", 9);
                row.rank = 1;
                row.rating = 5;
                uint32_t nan_bits = 0x7FC00000, inf_bits = 0x7F800000;
                const double inf = 1.0 / 0.0;
                int64_t dbits;
                std::memcpy(&dbits, &inf, 8);
                row.columns = {val(lb::kAttrRating, lb::kTypeInt32, 5), val(1, lb::kTypeFloat, nan_bits), val(2, lb::kTypeFloat, inf_bits),
                               val(3, lb::kTypeDouble, dbits), val(4, 9, 77), val(5, lb::kTypeContext, 0x1FFFFFFFFll), val(6, lb::kTypeInt32, 0x80000000ll),
                               val(7, lb::kTypeDouble, 0x3FF8000000000000ll)};
                x.rows.push_back(row);
            }
            r.body = lb::EncodeResponse(x);
        };
        srv.Start();
        const std::string at = srv.Address();
        Doc d = Run({"lb", "top", "--server", at, "--view", "9", "--count", "100"});
        CheckInt(d.Len("rows"), 100, "hostile board: 150 rows are cut to 100");
        CheckInt(d.N("total"), 1000, "hostile board: the total is the server's");
        mode = 1;
        Doc h = Run({"lb", "top", "--server", at, "--view", "9"});
        CheckEq(h.S("rows.0.gamertag"), "Hostile", "hostile board: the gamertag is cleaned");
        Check(h.IsNull("rows.0.columns.1.value"), "hostile board: a float NaN is null");
        Check(h.IsNull("rows.0.columns.2.value"), "hostile board: a float infinity is null");
        Check(h.IsNull("rows.0.columns.3.value"), "hostile board: a double infinity is null");
        CheckEq(h.S("rows.0.columns.4.type"), "unknown", "hostile board: an unknown type is named unknown");
        CheckInt(h.N("rows.0.columns.4.value"), 77, "hostile board: and read as a number");
        CheckInt(h.N("rows.0.columns.5.value"), -1, "hostile board: a context is its low 32 bits");
        CheckInt(h.N("rows.0.columns.6.value"), -2147483648ll, "hostile board: an int32 is signed");
        CheckNear(h.D("rows.0.columns.7.value"), 1.5, 1e-9, "hostile board: a double");
        CheckEq(h.S("rows.0.columns.7.type"), "double", "hostile board: its type");
    }

    // ---- hostile tournament files
    {
        HttpServer srv;
        std::atomic<int> mode{0};
        srv.handler = [&](const HttpRequest& q, HttpReply& r) {
            rmr::Request req;
            rmr::DecodeRequest(q.body, req);
            rmr::Response x;
            if (req.op == rmr::Op::kTournament) {
                tournament::Definition d = tournament::Current(rec::kTournamentMoment, {});
                x.count = d.week;
                switch (int(mode)) {
                    case 0: x.data.assign(100, 0); break;  // shorter than the game's file
                    case 1:
                        x.data = tournament::EncodeGuest(d);
                        x.count = 5000000;  // not a week
                        break;
                    case 2:
                        d.description = std::string("x\x01y\xff\r2nd\nthing\x1b[0m", 18);
                        x.data = tournament::EncodeGuest(d);
                        break;
                    case 3: {  // the first language entry is empty, the second has text
                        x.data = tournament::EncodeGuest(d);
                        std::memset(x.data.data() + 20, 0, 256);
                        const char* t = "de:Nur Deutsch\rZeile zwei";
                        std::memcpy(x.data.data() + 20 + 256, t, std::strlen(t));
                        break;
                    }
                    case 4:  // nothing in any entry
                        x.data = tournament::EncodeGuest(d);
                        std::memset(x.data.data() + 20, 0, 1536);
                        break;
                    case 5:  // restriction values nobody knows, a race that is not in the rotation, a car
                        x.data = tournament::EncodeGuest(d);
                        x.data[8] = x.data[9] = x.data[10] = 0;
                        x.data[11] = 9;
                        x.data[12] = 0;
                        x.data[13] = 0;
                        x.data[14] = 0x12;
                        x.data[15] = 0x34;
                        x.data[16] = 0xDE;
                        x.data[17] = 0xAD;
                        x.data[18] = 0xBE;
                        x.data[19] = 0xEF;
                        break;
                    case 6:  // a description of 255 characters with no terminator
                        x.data = tournament::EncodeGuest(d);
                        std::memset(x.data.data() + 20, 'z', 256);
                        break;
                }
            } else if (req.op == rmr::Op::kRewards) {
                switch (int(mode)) {
                    case 10: x.count = 1; x.data = tournament::EncodeRewardsFile(1); x.data[0] = 'X'; break;  // not "001"
                    case 11: x.count = 1; x.data = tournament::EncodeRewardsFile(1); x.data.push_back(0); break;  // 39 bytes
                    case 12: x.count = 5; break;  // prizes named, no file
                    case 13: x.count = 0; x.data = tournament::EncodeRewardsFile(0x21); break;  // the file wins over the count; bit 5 is no known prize
                }
            }
            r.body = rmr::EncodeResponse(x);
        };
        srv.Start();
        const std::string at = srv.Address();
        auto cur = [&](int m) {
            mode = m;
            return Run({"tournament", "current", "--server", at, "--now", std::to_string(rec::kTournamentMoment)});
        };
        Doc short_file = cur(0);
        CheckEq(short_file.S("server.state"), "bad_answer", "hostile tournament: a short file");
        Check(short_file.IsNull("tournament"), "hostile tournament: no tournament from it");
        Doc not_week = cur(1);
        CheckEq(not_week.S("server.state"), "bad_answer", "hostile tournament: a week that is not a week");
        Doc text = cur(2);
        CheckEq(text.S("server.state"), "ok", "hostile tournament: text with controls is still an answer");
        CheckEq(text.S("tournament.description"), "xy\n2nd thing[0m", "hostile tournament: controls and bad bytes go, '\\r' breaks a line, '\\n' is a space");
        CheckInt(text.Len("tournament.lines"), 2, "hostile tournament: two lines");
        Doc german = cur(3);
        CheckEq(german.S("tournament.description"), "Nur Deutsch\nZeile zwei", "hostile tournament: the first entry that has text");
        Doc empty = cur(4);
        CheckEq(empty.S("tournament.description"), "", "hostile tournament: no text");
        CheckInt(empty.Len("tournament.lines"), 0, "hostile tournament: no lines");
        CheckInt(empty.Len("tournament.prizes"), 0, "hostile tournament: no prizes");
        Doc odd = cur(5);
        CheckEq(odd.S("tournament.restriction.type"), "unknown", "hostile tournament: an unknown restriction");
        CheckInt(odd.N("tournament.restriction.type_id"), 9, "hostile tournament: its number");
        Check(!odd.B("tournament.restriction.any_car"), "hostile tournament: a car is asked for");
        CheckEq(odd.S("tournament.restriction.data"), "00001234", "hostile tournament: its data");
        CheckEq(odd.S("tournament.race_key"), "deadbeef", "hostile tournament: a race key");
        Check(odd.IsNull("tournament.race"), "hostile tournament: a race that is not in the rotation has no name");
        Doc full = cur(6);
        CheckInt(full.S("tournament.description").size(), 256, "hostile tournament: a whole entry with no terminator is read, and no further");

        const std::string token = TokHex(0x11);
        auto rew = [&](int m) {
            mode = m;
            return Run({"tournament", "rewards", "--server", at, "--token-stdin"}, &token);
        };
        CheckEq(rew(10).S("server.state"), "bad_answer", "hostile rewards: not the game's header");
        CheckEq(rew(11).S("server.state"), "bad_answer", "hostile rewards: not the game's size");
        CheckEq(rew(12).S("server.state"), "bad_answer", "hostile rewards: prizes named without a file");
        Doc bits = rew(13);
        CheckEq(bits.S("server.state"), "ok", "hostile rewards: a file with another bit");
        CheckInt(bits.N("rewards.mask"), 0x21, "hostile rewards: the file's bits");
        CheckInt(bits.N("rewards.other_bits"), 0x20, "hostile rewards: the bit that is no known prize");
        Check(bits.B("rewards.items.0.won"), "hostile rewards: the known one is won");
    }

    // ---- the token
    {
        for (const std::string& bad_text : std::vector<std::string>{std::string(""), std::string("zz"), std::string(31, 'a'), std::string(33, 'a'), std::string(32, 'g'), std::string("my secret token\n")}) {
            Doc d = Run({"tournament", "rewards", "--server", "127.0.0.1:1", "--token-stdin"}, &bad_text);
            CheckInt(d.exit_code, 2, "bad token: exit code 2 for '" + bad_text.substr(0, 10) + "'");
            Check(!d.B("ok"), "bad token: not ok");
            CheckEq(d.S("error.code"), "token", "bad token: its code");
            Check(bad_text.empty() || d.raw.find(bad_text.substr(0, std::min<size_t>(bad_text.size(), 8))) == std::string::npos, "bad token: the text is not echoed");
        }
        const std::string padded = "  " + TokHex(0x7A) + " \r\n";
        Doc ok = Run({"tournament", "rewards", "--server", "127.0.0.1:1", "--connect-timeout-ms", "300", "--token-stdin"}, &padded);
        CheckInt(ok.exit_code, 0, "token: spaces and a newline around it are fine");
        CheckEq(ok.S("server.address"), "127.0.0.1:1", "token: and the command went on");
    }
}

// ---- the clock and the token on the wire ----------------------------------------------------------------------------

void CheckDeadlineAndToken() {
    // A server that answers, slowly: a call stops starting requests once its deadline has passed.
    {
        HttpServer srv;
        srv.handler = [&](const HttpRequest& q, HttpReply& r) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            rmr::Request req;
            rmr::DecodeRequest(q.body, req);
            rmr::Response x;
            if (req.op == rmr::Op::kTableSize) {
                x.count = 10;
            } else if (req.op == rmr::Op::kByRank) {
                for (uint32_t i = 0; i < 10; ++i) {
                    rmr::Record rec = MakeRecord(0x52000100 + i * 4, 0x1000 + i, "R" + std::to_string(i));
                    rec.file_sizes[1] = 40;
                    x.records.push_back(rec);
                }
            } else if (req.op == rmr::Op::kDownload) {
                x.data = Jpeg(1, 40);
            }
            r.body = rmr::EncodeResponse(x);
        };
        srv.Start();
        const fs::path dir = ScratchDir() / "slow-photos";
        Doc d = Run({"rmr", "top", "--server", srv.Address(), "--photos", Utf8(dir), "--deadline-ms", "1000"});
        CheckEq(d.S("server.state"), "limit", "deadline: the call stopped starting requests");
        Check(d.S("server.message").find("time") != std::string::npos, "deadline: and says it ran out of time");
        Check(d.N("server.requests") >= 4 && d.N("server.requests") <= 7, "deadline: after a few requests (" + std::to_string(d.N("server.requests")) + ")");
        CheckInt(d.Len("rides"), 10, "deadline: the rides it had are listed");
        Check(d.N("server.elapsed_ms") < 2500, "deadline: and it did not run on");
        size_t saved = 0, unavailable = 0;
        for (size_t k = 0; k < d.Len("rides"); ++k) {
            const std::string st = d.S("rides." + std::to_string(k) + ".photos.0.state");
            saved += st == "saved";
            unavailable += st == "unavailable";
        }
        Check(saved >= 2 && unavailable >= 3 && saved + unavailable == 10, "deadline: some photos kept, the rest unavailable (" + std::to_string(saved) + "/" + std::to_string(unavailable) + ")");
        Doc again = Run({"rmr", "top", "--server", srv.Address(), "--photos", Utf8(dir), "--deadline-ms", "60000"});
        CheckEq(again.S("server.state"), "ok", "deadline: with time to spare the same call finishes");
    }

    // Which commands put the player's token on the wire: only the two that need it. The others use it, when it is given,
    // to know the player's public owner key, and send zeros.
    {
        HttpServer srv;
        std::mutex lock;
        std::vector<std::pair<std::string, std::string>> seen;  // what was asked, the token that came with it
        srv.handler = [&](const HttpRequest& q, HttpReply& r) {
            std::string what, token;
            if (q.path == lb::kHttpPath) {
                lb::Request req;
                lb::DecodeRequest(q.body, req);
                what = std::string("lb ") + lb::OpName(req.op);
                token = HexOf(Bytes(req.token.begin(), req.token.end()));
                lb::Response x;
                x.total = 0;
                r.body = lb::EncodeResponse(x);
            } else {
                rmr::Request req;
                rmr::DecodeRequest(q.body, req);
                what = std::string("rmr ") + rmr::OpName(req.op);
                token = HexOf(Bytes(req.token.begin(), req.token.end()));
                rmr::Response x;
                if (req.op == rmr::Op::kTournament) {
                    x.count = 2967;
                    x.data = tournament::EncodeGuest(tournament::Current(rec::kTournamentMoment, {}));
                }
                r.body = rmr::EncodeResponse(x);
            }
            std::lock_guard<std::mutex> g(lock);
            seen.push_back({what, token});
        };
        srv.Start();
        const std::string at = srv.Address();
        const std::string secret = TokHex(0x5C), zeros = HexOf(Bytes(16, 0));
        auto token_of = [&](const std::vector<std::string>& cmd, const std::string& want_op) {
            {
                std::lock_guard<std::mutex> g(lock);
                seen.clear();
            }
            std::vector<std::string> full = cmd;
            full.push_back("--server");
            full.push_back(at);
            Run(full, &secret);
            std::lock_guard<std::mutex> g(lock);
            for (const auto& s : seen)
                if (s.first == want_op) return s.second;
            return std::string("<not asked>");
        };
        CheckEq(token_of({"rmr", "rate", "--record", "52000001", "--rating", "5", "--confirm", "--token-stdin"}, "rmr rate"), secret, "token on the wire: rate sends it");
        CheckEq(token_of({"tournament", "rewards", "--token-stdin"}, "rmr rewards"), secret, "token on the wire: rewards sends it");
        CheckEq(token_of({"rmr", "mine", "--token-stdin"}, "rmr by_owner"), zeros, "token on the wire: mine does not");
        CheckEq(token_of({"lb", "around", "--view", "5", "--token-stdin"}, "lb ByPivot"), zeros, "token on the wire: lb around does not");
        CheckEq(token_of({"tournament", "standings", "--token-stdin"}, "rmr tournament"), zeros, "token on the wire: standings' definition does not");
        CheckEq(token_of({"tournament", "standings", "--token-stdin"}, "lb ByOwners"), zeros, "token on the wire: standings' own row does not");
        CheckEq(token_of({"rmr", "top", "--owner", "0c50476a8dc2d715"}, "rmr by_rank"), zeros, "token on the wire: a read with no token sends zeros");
        // and the owner key still came from the token
        {
            std::lock_guard<std::mutex> g(lock);
            seen.clear();
        }
        Doc d = Run({"rmr", "mine", "--server", at, "--token-stdin"}, &secret);
        CheckEq(d.S("owner"), ot::Hex64Text(rmr::OwnerKeyFromToken(Tok(0x5C))), "token on the wire: the owner key is derived locally");
    }
}

// ---- the game's tables file ------------------------------------------------------------------------------------------

void CheckTables() {
    const fs::path dir = ScratchDir() / "tables";
    std::error_code ec;
    fs::create_directories(dir, ec);
    auto write = [&](const std::string& name, const std::string& text) {
        WriteFileBytes(dir / name, text);
        return Utf8(dir / name);
    };

    const std::string good = write("good.json",
        "{\"schema\":1,\"game_version\":\"PATCH 10 - PREVIEW 2\",\"stats\":{\"available\":true,\"column_sets\":["
        "[{\"prop\":268435457,\"type\":1,\"attr\":1,\"agg\":3,\"raw\":\"00\"},{\"prop\":536870914,\"type\":2,\"attr\":65534,\"agg\":11,\"raw\":\"01\"}],"
        "[{\"prop\":1342177283,\"type\":5,\"attr\":2,\"agg\":9,\"raw\":\"02\"}]],"
        "\"views\":[{\"id\":382,\"column_set\":0,\"raw\":\"aa\"},{\"id\":383,\"column_set\":1,\"name\":\"Race\\u00a0wins\\n\\u0001x\"},{\"id\":999,\"column_set\":7},{\"id\":5}]},"
        "\"properties\":{\"available\":false,\"items\":[]}}");
    {
        Doc d = Run({"lb", "views", "--tables", good});
        CheckEq(d.S("tables.state"), "ok", "tables: ok");
        CheckEq(d.S("tables.game_version"), "PATCH 10 - PREVIEW 2", "tables: the game's version");
        CheckInt(d.N("tables.views"), 4, "tables: four views");
        CheckInt(d.Len("views"), 4, "tables: four listed");
        CheckInt(d.N("views.0.id"), 5, "tables: in id order");
        CheckInt(d.Len("views.0.columns"), 0, "tables: a view with no column set has no columns");
        CheckInt(d.N("views.1.id"), 382, "tables: view 382");
        CheckInt(d.Len("views.1.columns"), 2, "tables: its two columns");
        CheckInt(d.N("views.1.columns.0.attr"), 1, "tables: column attr");
        CheckEq(d.S("views.1.columns.0.prop"), "10000001", "tables: column property id as hex");
        CheckEq(d.S("views.1.columns.0.type"), "int32", "tables: column type");
        CheckEq(d.S("views.1.columns.0.agg"), "sum", "tables: column aggregation");
        CheckInt(d.N("views.1.columns.1.attr"), 65534, "tables: the rating column");
        CheckEq(d.S("views.1.columns.1.type"), "int64", "tables: its type");
        CheckEq(d.S("views.1.columns.1.agg"), "max", "tables: its aggregation");
        Check(d.IsNull("views.1.name"), "tables: no name in the game's dump today");
        CheckEq(d.S("views.2.name"), "Race\xC2\xA0winsx", "tables: a name from a later dump, cleaned");
        CheckEq(d.S("views.2.columns.0.agg"), "min", "tables: min");
        CheckEq(d.S("views.2.columns.0.type"), "float", "tables: float");
        CheckInt(d.Len("views.3.columns"), 0, "tables: a column set that is not there is no columns");
        Check(d.B("views.1.known"), "tables: known");
    }
    {
        Doc d = Run({"lb", "views", "--tables", Utf8(dir / "missing.json")});
        CheckEq(d.S("tables.state"), "not_found", "tables: a missing file");
        Check(d.B("ok"), "tables: a missing file is still an answer");
        CheckInt(d.Len("views"), 0, "tables: no views");
    }
    struct Bad {
        const char* name;
        const char* text;
        const char* state;
    };
    const Bad bad[] = {
        {"empty.json", "", "invalid"},
        {"junk.json", "not json", "invalid"},
        {"array.json", "[1,2]", "invalid"},
        {"schema2.json", "{\"schema\":2,\"stats\":{\"available\":true,\"column_sets\":[],\"views\":[]}}", "unknown_schema"},
        {"noschema.json", "{\"stats\":{\"available\":true,\"column_sets\":[],\"views\":[]}}", "unknown_schema"},
        {"unavailable.json", "{\"schema\":1,\"stats\":{\"available\":false,\"error\":\"no XVC2\\u0001 section\"}}", "stats_unavailable"},
        {"nostats.json", "{\"schema\":1}", "stats_unavailable"},
        {"nosets.json", "{\"schema\":1,\"stats\":{\"available\":true}}", "invalid"},
        {"trailing.json", "{\"schema\":1} x", "invalid"},
    };
    for (const Bad& b : bad) {
        Doc d = Run({"lb", "views", "--tables", write(b.name, b.text)});
        CheckEq(d.S("tables.state"), b.state, std::string("tables: ") + b.name);
        Check(d.B("ok") && d.Len("views") == 0, std::string("tables: ") + b.name + " is an answer with no views");
        Check(!d.S("tables.error").empty() || std::string(b.state) == "stats_unavailable", std::string("tables: ") + b.name + " says why");
    }
    {
        Doc d = Run({"lb", "views", "--tables", write("unavailable.json", bad[5].text)});
        CheckEq(d.S("tables.error"), "no XVC2 section", "tables: the game's own reason, cleaned");
    }
    {
        std::string big = "{\"schema\":1,\"pad\":\"";
        big += std::string(9 * 1024 * 1024, 'x');
        big += "\"}";
        Doc d = Run({"lb", "views", "--tables", write("big.json", big)});
        CheckEq(d.S("tables.state"), "invalid", "tables: a file over 8 MB");
    }
    {
        std::string many = "{\"schema\":1,\"stats\":{\"available\":true,\"column_sets\":[[]],\"views\":[";
        for (int i = 1; i <= 5000; ++i) many += std::string(i > 1 ? "," : "") + "{\"id\":" + std::to_string(i) + ",\"column_set\":0}";
        many += "]}}";
        Doc d = Run({"lb", "views", "--tables", write("many.json", many)});
        CheckInt(d.N("tables.views"), 4096, "tables: views are cut at 4096");
    }
    {
        // the columns come with a board
        Recorded r;
        r.lb[int(lb::Op::kByRank)] = FromHex(rec::kLbByRank);
        r.lb[int(lb::Op::kSize)] = FromHex(rec::kLbSize);
        HttpServer srv;
        srv.handler = [&](const HttpRequest& q, HttpReply& a) { RecordedHandler(r, q, a); };
        srv.Start();
        Doc d = Run({"lb", "top", "--server", srv.Address(), "--view", "382", "--tables", good});
        Check(d.B("view.known"), "board with tables: the view is known");
        CheckInt(d.Len("view.columns"), 2, "board with tables: its columns are listed once");
        CheckEq(d.S("view.columns.1.agg"), "max", "board with tables: aggregation of the rating column");
        Doc unknown = Run({"lb", "top", "--server", srv.Address(), "--view", "12345", "--tables", good});
        Check(unknown.Has("view.known") && !unknown.B("view.known") && !unknown.IsNull("view.known"), "board with tables: a view that is not in the file is not known");
        Doc sized = Run({"lb", "size", "--server", srv.Address(), "--view", "382,12345", "--tables", good});
        Check(sized.B("boards.0.known") && !sized.B("boards.1.known"), "sizes with tables: known and unknown");
        Doc missing = Run({"lb", "top", "--server", srv.Address(), "--view", "382", "--tables", Utf8(dir / "nope.json")});
        CheckEq(missing.S("tables.state"), "not_found", "board with a missing tables file: said");
        CheckEq(missing.S("server.state"), "ok", "board with a missing tables file: the board is read anyway");
        Check(missing.IsNull("view.known"), "board with a missing tables file: nothing is known");
    }
}

// ---- a server made of the real store code ----------------------------------------------------------------------------

void CheckLive() {
    const fs::path base = ScratchDir() / "live";
    StoreServer S(base);
    const std::string at = S.Address();
    const Bytes car(64, 0xAB);
    Bytes ud(64, 0);
    rmr::StoreBe32(&ud[rmr::kUdVehicle], 0x12C);
    rmr::StoreBe32(&ud[rmr::kUdPrice], 5000);
    rmr::StoreBe32(&ud[rmr::kUdForSale], 1);

    const Bytes alpha_p1 = Jpeg(1, 120), alpha_p2 = Jpeg(7, 90), bravo_p1 = Jpeg(3, 80);
    const auto alpha = S.Upload(0x11, "Alpha", {car, alpha_p1, alpha_p2}, ud);
    const auto bravo = S.Upload(0x22, "Bravo", {car, bravo_p1});
    const auto cora = S.Upload(0x33, "Cora", {car});
    Check(!alpha.records.empty() && !bravo.records.empty() && !cora.records.empty(), "live: the rides are uploaded");
    const uint32_t A = alpha.records[0].id, B = bravo.records[0].id, C = cora.records[0].id;
    for (uint8_t t : {uint8_t(0x22), uint8_t(0x33), uint8_t(0x44)}) S.Rate(t, A, 8);
    for (uint8_t t : {uint8_t(0x11), uint8_t(0x33), uint8_t(0x44)}) S.Rate(t, B, 6);
    for (uint8_t t : {uint8_t(0x11), uint8_t(0x22)}) S.Rate(t, C, 9);  // two ratings: not ranked at the game's minimum of three
    const std::string o1 = ot::Hex64Text(rmr::OwnerKeyFromToken(Tok(0x11))), o2 = ot::Hex64Text(rmr::OwnerKeyFromToken(Tok(0x22))),
                      o3 = ot::Hex64Text(rmr::OwnerKeyFromToken(Tok(0x33)));

    {
        Doc d = Run({"rmr", "table", "--server", at});
        CheckInt(d.N("ranked"), 2, "live table: two ride are ranked at the game's minimum of three ratings");
        Doc two = Run({"rmr", "table", "--server", at, "--min-ratings", "2"});
        CheckInt(two.N("ranked"), 3, "live table: three at two ratings");
        Doc one = Run({"rmr", "table", "--server", at, "--min-ratings", "1"});
        CheckInt(one.N("ranked"), 3, "live table: three at one rating");
    }
    {
        Doc d = Run({"rmr", "top", "--server", at, "--count", "1"});
        CheckInt(d.Len("rides"), 1, "live top: one ride");
        CheckEq(d.S("rides.0.gamertag"), "Alpha", "live top: the best ride");
        CheckInt(d.N("total_ranked"), 2, "live top: total");
        Doc second = Run({"rmr", "top", "--server", at, "--start", "2", "--count", "1"});
        CheckEq(second.S("rides.0.gamertag"), "Bravo", "live top: the second ride");
        CheckInt(second.N("rides.0.rank"), 2, "live top: its rank");
        Doc past = Run({"rmr", "top", "--server", at, "--start", "3"});
        CheckInt(past.Len("rides"), 0, "live top: nothing past the last rank");
        CheckEq(past.S("server.state"), "ok", "live top: and no failure");
        Doc low = Run({"rmr", "top", "--server", at, "--min-ratings", "2"});
        CheckInt(low.Len("rides"), 3, "live top: three rides at two ratings");
        CheckEq(low.S("rides.0.gamertag"), "Cora", "live top: by average, Cora (9.0 from two ratings) first");
        CheckEq(low.S("rides.2.gamertag"), "Bravo", "live top: and Bravo (6.0) last");
    }
    const fs::path photos = ScratchDir() / fs::path(std::u8string(u8"live-photos \u00e9"));
    {
        const int before = S.downloads;
        Doc d = Run({"rmr", "top", "--server", at, "--min-ratings", "2", "--photos", Utf8(photos), "--photo-slots", "2"});
        CheckInt(S.downloads - before, 3, "live photos: three photos exist and three are fetched");
        // Cora (average 9, two ratings) ranks first at min 2
        for (int k = 0; k < 3; ++k) {
            const std::string tag = d.S("rides." + std::to_string(k) + ".gamertag");
            if (tag == "Alpha") {
                CheckEq(d.S("rides." + std::to_string(k) + ".photos.0.state"), "saved", "live photos: Alpha's first");
                CheckEq(d.S("rides." + std::to_string(k) + ".photos.1.state"), "saved", "live photos: Alpha's second");
                Check(ReadFileBytes(photos / d.S("rides." + std::to_string(k) + ".photos.0.file")) == alpha_p1, "live photos: Alpha's first photo, byte for byte");
                Check(ReadFileBytes(photos / d.S("rides." + std::to_string(k) + ".photos.1.file")) == alpha_p2, "live photos: Alpha's second photo, byte for byte");
            } else if (tag == "Bravo") {
                CheckEq(d.S("rides." + std::to_string(k) + ".photos.0.state"), "saved", "live photos: Bravo's first");
                CheckEq(d.S("rides." + std::to_string(k) + ".photos.1.state"), "none", "live photos: Bravo has no second");
                Check(ReadFileBytes(photos / d.S("rides." + std::to_string(k) + ".photos.0.file")) == bravo_p1, "live photos: Bravo's photo, byte for byte");
            } else {
                CheckEq(d.S("rides." + std::to_string(k) + ".photos.0.state"), "none", "live photos: Cora has none");
            }
        }
        const int mid = S.downloads;
        Doc again = Run({"rmr", "top", "--server", at, "--min-ratings", "2", "--photos", Utf8(photos), "--photo-slots", "2"});
        CheckInt(S.downloads - mid, 0, "live photos: the second call fetches nothing");
        for (int k = 0; k < 3; ++k) {
            const std::string tag = again.S("rides." + std::to_string(k) + ".gamertag");
            if (tag == "Alpha") CheckEq(again.S("rides." + std::to_string(k) + ".photos.1.state"), "cached", "live photos: cached");
        }
        // a damaged copy is fetched again
        const fs::path first = photos / ot::PhotoFileName(alpha.records[0].file_ids[1], alpha_p1.size());
        WriteFileBytes(first, "garbage");
        const int damaged = S.downloads;
        Doc repair = Run({"rmr", "top", "--server", at, "--min-ratings", "2", "--photos", Utf8(photos)});
        CheckInt(S.downloads - damaged, 1, "live photos: a damaged copy is fetched again");
        Check(ReadFileBytes(first) == alpha_p1, "live photos: and replaced");
        // Alpha uploads a new car: new files, so new names, and the old ones are not used
        const auto again_up = S.Upload(0x11, "Alpha", {car, Jpeg(9, 130), Jpeg(11, 95)}, ud);
        for (uint8_t t : {uint8_t(0x22), uint8_t(0x33), uint8_t(0x44)}) S.Rate(t, again_up.records[0].id, 8);
        const int fresh = S.downloads;
        Doc newer = Run({"rmr", "top", "--server", at, "--photos", Utf8(photos), "--photo-slots", "2"});
        CheckInt(S.downloads - fresh, 2, "live photos: the new photos are fetched, Bravo's is still cached");
        CheckInt(newer.N("rides.0.photos.0.size"), 130, "live photos: the new size");
    }
    {
        Doc d = Run({"rmr", "mine", "--server", at, "--owner", o1});
        Check(d.B("found"), "live mine: found");
        CheckEq(d.S("ride.gamertag"), "Alpha", "live mine: the ride");
        CheckInt(d.N("ride.rank"), 1, "live mine: rank");
        CheckInt(d.N("ride.sale.price"), 5000, "live mine: the sale price");
        CheckInt(d.N("ride.photos.0.size"), 130, "live mine: the replaced ride's photo");
        Doc c = Run({"rmr", "mine", "--server", at, "--owner", o3});
        CheckInt(c.N("ride.rank"), 0, "live mine: an unranked ride has rank 0");
        Check(!c.B("ride.ranked"), "live mine: and is not ranked");
        CheckInt(c.N("ride.ratings"), 2, "live mine: with its two ratings");
        const std::string nobody = TokHex(0x77);
        Doc n = Run({"rmr", "mine", "--server", at, "--token-stdin"}, &nobody);
        Check(n.Has("found") && !n.B("found"), "live mine: a player with no ride has none");
    }
    {
        const fs::path out = ScratchDir() / "live-photo-cmd";
        const std::string id = ot::Hex32Text(bravo.records[0].file_ids[1]);
        Doc d = Run({"rmr", "photo", "--server", at, "--file", id, "--out", Utf8(out)});
        CheckEq(d.S("photos.0.state"), "saved", "live photo: saved");
        Check(ReadFileBytes(out / d.S("photos.0.file")) == bravo_p1, "live photo: the file");
        // file 0 of a ride is the car's configuration, not an image
        Doc car_file = Run({"rmr", "photo", "--server", at, "--file", ot::Hex32Text(bravo.records[0].file_ids[0]), "--out", Utf8(out)});
        CheckEq(car_file.S("photos.0.state"), "invalid", "live photo: the car's configuration is not a photo");
        Doc nothing = Run({"rmr", "photo", "--server", at, "--file", "52ffff00", "--out", Utf8(out)});
        CheckEq(nothing.S("photos.0.state"), "unavailable", "live photo: a file that is not there");
    }
    {
        // rating
        const std::string t5 = TokHex(0x55), t2 = TokHex(0x22);
        const std::string bravo_id = ot::Hex32Text(B);
        Doc r1 = Run({"rmr", "rate", "--server", at, "--record", bravo_id, "--rating", "10", "--confirm", "--token-stdin"}, &t5);
        CheckEq(r1.S("result"), "rated", "live rate: rated");
        CheckInt(r1.N("ratings"), 4, "live rate: four ratings");
        CheckNear(r1.D("average"), 7.0, 0.001, "live rate: average (6+6+6+10)/4");
        Doc r2 = Run({"rmr", "rate", "--server", at, "--record", bravo_id, "--rating", "2", "--confirm", "--token-stdin"}, &t5);
        CheckEq(r2.S("result"), "rated", "live rate: rated again");
        CheckInt(r2.N("ratings"), 4, "live rate: still four (a new rating replaces the old)");
        CheckNear(r2.D("average"), 5.0, 0.001, "live rate: average (6+6+6+2)/4");
        Doc own = Run({"rmr", "rate", "--server", at, "--record", bravo_id, "--rating", "10", "--confirm", "--token-stdin"}, &t2);
        CheckEq(own.S("result"), "refused", "live rate: not on the player's own ride");
        Doc gone = Run({"rmr", "rate", "--server", at, "--record", "52ffff00", "--rating", "10", "--confirm", "--token-stdin"}, &t5);
        CheckEq(gone.S("result"), "no_such_ride", "live rate: no such ride");
        Doc top = Run({"rmr", "top", "--server", at});
        for (int k = 0; k < static_cast<int>(top.Len("rides")); ++k)
            if (top.S("rides." + std::to_string(k) + ".gamertag") == "Bravo") CheckNear(top.D("rides." + std::to_string(k) + ".average"), 5.0, 0.001, "live rate: the ranking sees it");
        // without --confirm nothing is sent
        const int before = S.requests;
        Doc refused = Run({"rmr", "rate", "--server", at, "--record", bravo_id, "--rating", "1", "--token-stdin"}, &t5);
        CheckInt(refused.exit_code, 2, "live rate: no --confirm is a usage error");
        CheckInt(S.requests - before, 0, "live rate: and nothing reached the server");
    }
    // the request budget
    {
        for (int i = 0; i < 45; ++i) {
            const uint8_t tok = uint8_t(0x80 + i);
            const auto up = S.Upload(tok, "Bulk" + std::to_string(i), {car, Jpeg(uint8_t(i), 60 + size_t(i))});
            S.Rate(0x22, up.records[0].id, 5);
        }
        const int before = S.requests;
        const fs::path bulk = ScratchDir() / "live-bulk";
        Doc d = Run({"rmr", "top", "--server", at, "--min-ratings", "1", "--count", "50", "--photos", Utf8(bulk)});
        CheckEq(d.S("server.state"), "limit", "budget: a call stops at its request budget");
        CheckInt(d.N("server.requests"), 40, "budget: forty requests");
        CheckInt(S.requests - before, 40, "budget: and the server saw forty");
        CheckInt(d.Len("rides"), 48, "budget: the rides listed are all there");
        size_t unavailable = 0, saved = 0;
        for (size_t k = 0; k < d.Len("rides"); ++k) {
            const std::string st = d.S("rides." + std::to_string(k) + ".photos.0.state");
            unavailable += st == "unavailable";
            saved += st == "saved";
        }
        Check(saved > 30 && unavailable > 0, "budget: the photos after the budget are 'unavailable' (" + std::to_string(saved) + " saved, " + std::to_string(unavailable) + " not)");
    }

    // ---- leaderboards
    {
        S.LbWrite(0x11, "Alpha", 382, 120, 3);
        S.LbWrite(0x22, "Bravo", 382, 95, 1);
        S.LbWrite(0x33, "Cora", 382, 150, 5);
        S.LbWrite(0x44, "Dee", 382, 80, 0);
        S.LbWrite(0x55, "Eli", 382, 100, 2);
        Doc sz = Run({"lb", "size", "--server", at, "--view", "382,383"});
        CheckInt(sz.N("boards.0.total"), 5, "live lb size: five rows");
        CheckInt(sz.N("boards.1.total"), 0, "live lb size: an empty view");
        Doc top = Run({"lb", "top", "--server", at, "--view", "382", "--count", "3"});
        CheckInt(top.Len("rows"), 3, "live lb top: three rows");
        CheckEq(top.S("rows.0.gamertag"), "Cora", "live lb top: highest rating first");
        CheckEq(top.S("rows.2.gamertag"), "Eli", "live lb top: third");
        CheckInt(top.N("total"), 5, "live lb top: total");
        Doc page2 = Run({"lb", "top", "--server", at, "--view", "382", "--start", "4", "--count", "3"});
        CheckInt(page2.Len("rows"), 2, "live lb top: the end of the board");
        CheckInt(page2.N("rows.0.rank"), 4, "live lb top: rank 4");
        const std::string t3 = TokHex(0x33);
        Doc around = Run({"lb", "around", "--server", at, "--view", "382", "--token-stdin", "--span", "1"}, &t3);
        CheckInt(around.Len("rows"), 3, "live lb around: three rows");
        Check(around.B("found"), "live lb around: the player is in it");
        CheckEq(around.S("rows.0.gamertag"), "Cora", "live lb around: Cora is first, so the window starts at rank 1");
        Check(around.B("rows.0.mine"), "live lb around: Cora is the player");
        Doc outsider = Run({"lb", "around", "--server", at, "--view", "382", "--owner", "1234567800000009"});
        CheckInt(outsider.Len("rows"), 0, "live lb around: a player with no row gets no window");
        Check(outsider.Has("found") && !outsider.B("found"), "live lb around: found is false");
        Doc owners = Run({"lb", "owners", "--server", at, "--view", "382", "--owner", o1 + "," + o2 + ",1234567800000009"});
        CheckInt(owners.Len("rows"), 2, "live lb owners: the two with rows");
        Doc none = Run({"lb", "top", "--server", at, "--view", "9999"});
        CheckInt(none.N("total"), 0, "live lb top: a view nobody wrote");
        CheckInt(none.Len("rows"), 0, "live lb top: no rows");
        CheckEq(none.S("server.state"), "ok", "live lb top: still an answer");
    }

    // ---- the tournament
    {
        WriteFileBytes(S.dir / "rmr" / "tournament.txt",
                       "race = Hollywood Circuit 1\nslot = 360\ndescription = Test race\\n1st: TIS rims.\\n2nd-3rd: hydraulics.\nrestriction_type = 2\n"
                       "restriction_data = 0xFFFFFFFF\nprize_first = 2\n");
        const int64_t t_before = int64_t(std::time(nullptr));
        Doc d = Run({"tournament", "current", "--server", at});
        const tournament::Definition week = tournament::Current(t_before, {});
        CheckEq(d.S("server.state"), "ok", "live tournament: state");
        CheckInt(d.N("tournament.week"), week.week, "live tournament: this week");
        CheckInt(d.N("tournament.slot"), 360, "live tournament: the slot the server's file gives");
        CheckEq(d.S("tournament.race"), "Hollywood Circuit 1", "live tournament: the race");
        CheckEq(d.S("tournament.race_key"), "eee614c1", "live tournament: its key");
        CheckEq(d.S("tournament.lines.0"), "Test race", "live tournament: the text");
        CheckEq(d.S("tournament.prizes.0.place"), "1st", "live tournament: prize lines");
        CheckEq(d.S("tournament.prizes.0.rewards.0"), "TIS rims", "live tournament: a prize");
        CheckEq(d.S("tournament.prizes.1.rewards.0"), "hydraulics", "live tournament: the podium prize");
        CheckInt(d.N("tournament.starts"), week.starts, "live tournament: starts");
        CheckInt(d.N("tournament.ends"), week.ends, "live tournament: ends");
        Check(d.B("tournament.active"), "live tournament: active now");
        Check(d.N("tournament.seconds_left") > 0 && d.N("tournament.seconds_left") <= 604800, "live tournament: time left");

        // standings: slot 360 is in 351..381, so the game may write the times to the slot or to slot + 31
        Doc empty = Run({"tournament", "standings", "--server", at});
        CheckInt(empty.N("board.total"), 0, "live standings: nobody has a time yet");
        CheckEq(empty.S("board.view_basis"), "slot", "live standings: the slot when both are empty");
        CheckInt(empty.Len("board.candidates"), 2, "live standings: both views were asked");
        CheckInt(empty.N("board.candidates.1"), 391, "live standings: slot + 31");
        CheckInt(empty.N("server.requests"), 3, "live standings: the week and the two boards");
        S.LbWrite(0x11, "Alpha", 391, 100, 1);
        S.LbWrite(0x22, "Bravo", 391, 90, 1);
        const std::string t2 = TokHex(0x22);
        Doc plus = Run({"tournament", "standings", "--server", at, "--token-stdin"}, &t2);
        CheckEq(plus.S("board.view_basis"), "slot_plus_31", "live standings: the times are in slot + 31");
        CheckInt(plus.N("board.view.id"), 391, "live standings: that view");
        CheckInt(plus.N("board.total"), 2, "live standings: two times");
        CheckEq(plus.S("board.rows.0.gamertag"), "Alpha", "live standings: the best first");
        Check(plus.B("board.rows.0.in_week"), "live standings: written this week");
        CheckEq(plus.S("board.mine.gamertag"), "Bravo", "live standings: the player's own row");
        CheckInt(plus.N("board.mine.rank"), 2, "live standings: its rank");
        S.LbWrite(0x33, "Cora", 360, 50, 1);
        Doc both = Run({"tournament", "standings", "--server", at});
        CheckEq(both.S("board.view_basis"), "slot", "live standings: the slot wins when it has rows");
        CheckInt(both.N("board.total"), 1, "live standings: its one row");
        CheckInt(both.N("server.requests"), 2, "live standings: the second view is not asked when the first has rows");
        Doc forced = Run({"tournament", "standings", "--server", at, "--view", "391", "--count", "1"});
        CheckEq(forced.S("board.view_basis"), "override", "live standings: a view given");
        CheckInt(forced.N("board.total"), 2, "live standings: its rows");
        CheckInt(forced.Len("board.rows"), 1, "live standings: the page asked");
        // a slot below 351 has one candidate
        WriteFileBytes(S.dir / "rmr" / "tournament.txt", "race = Hollywood Circuit 1\nslot = 30\n");
        Doc low = Run({"tournament", "standings", "--server", at});
        CheckInt(low.Len("board.candidates"), 1, "live standings: a slot below 351 has one view");
        CheckInt(low.N("board.view.id"), 30, "live standings: it is the slot");
        // the next weeks, no server
        Doc next = Run({"tournament", "next"});
        CheckInt(next.N("weeks.0.week"), week.week + 1, "live next: the week after this one");
    }
    // rewards: a week that ended, settled by the first request for prizes (tournament_awards.h)
    {
        const fs::path base2 = ScratchDir() / "live-awards";
        StoreServer S2(base2);  // the server's files are read when the first tournament question comes, so seed them now
        const int64_t now = int64_t(std::time(nullptr));
        // week slot starts ends prize_first prize_podium podium settled
        WriteFileBytes(base2 / "rmr" / "tournament_weeks.txt",
                       "2900 30 " + std::to_string(now - 1000) + " " + std::to_string(now + 2) + " 7 6 2 0\n");
        S2.LbWrite(0x11, "Alpha", 30, 100, 1);
        S2.LbWrite(0x22, "Bravo", 30, 90, 1);
        S2.LbWrite(0x33, "Cora", 30, 50, 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(3200));
        auto rewards = [&](uint8_t tok) {
            const std::string t = TokHex(tok);
            return Run({"tournament", "rewards", "--server", S2.Address(), "--token-stdin"}, &t);
        };
        Doc first = rewards(0x11);
        CheckEq(first.S("server.state"), "ok", "live rewards: state");
        CheckInt(first.N("rewards.mask"), 7, "live rewards: first place wins all three");
        CheckEq(first.S("rewards.items.0.name"), "Audi R8", "live rewards: names");
        Doc second = rewards(0x22);
        CheckInt(second.N("rewards.mask"), 6, "live rewards: second place wins the rims and the hydraulics");
        Check(!second.B("rewards.items.0.won") && second.B("rewards.items.1.won") && second.B("rewards.items.2.won"), "live rewards: which ones");
        Doc third = rewards(0x33);
        CheckInt(third.N("rewards.mask"), 0, "live rewards: third place is out of the two prized places");
        Doc nobody = rewards(0x44);
        CheckInt(nobody.N("rewards.mask"), 0, "live rewards: nobody wins nothing");
        Doc again = rewards(0x11);
        CheckInt(again.N("rewards.mask"), 7, "live rewards: prizes stay");
    }
}

}  // namespace

int main() {
    StartSockets();
    std::srand(unsigned(std::time(nullptr)));
    CheckArguments();
    CheckHelpers();
    CheckJsonPieces();
    CheckRecordedAnswers();
    CheckHostile();
    CheckDeadlineAndToken();
    CheckTables();
    CheckLive();

    // The player's token is in no document, whatever happened.
    {
        std::set<std::string> secrets;
        for (int b : {0x11, 0x22, 0x33, 0x44, 0x55, 0x77, 0x7A, 0x5C}) secrets.insert(TokHex(uint8_t(b)));
        secrets.insert(HexOf(Bytes(16, 0)));  // the zero token the reads send must not show either
        size_t scanned = 0;
        for (const std::string& doc : g_all_docs) {
            ++scanned;
            for (const std::string& s : secrets)
                if (doc.find(s) != std::string::npos) {
                    ++g_failed;
                    std::printf("FAILED: a token is in a document: %s\n", doc.substr(0, 200).c_str());
                }
        }
        g_checks += int(scanned);
        std::printf("scanned %zu documents for the players' tokens\n", scanned);
    }
    std::error_code ec;
    fs::remove_all(ScratchDir(), ec);
    std::printf("online_tool_remote_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
