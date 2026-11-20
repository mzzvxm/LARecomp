// The Director's TCP channel for agents: see line_server.h.

#include "line_server.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <process.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#endif

namespace mc::cinematic::director {

namespace {

#if defined(_WIN32)
using sock_t = SOCKET;
constexpr sock_t kBadSocket = INVALID_SOCKET;
void CloseSocket(sock_t s) { closesocket(s); }
bool WouldBlock() { return WSAGetLastError() == WSAEWOULDBLOCK; }
bool SetNonBlocking(sock_t s) {
    u_long mode = 1;
    return ioctlsocket(s, FIONBIO, &mode) == 0;
}
void EnsureSockets() {
    static const bool started = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;  // counted: harmless next to the SDK's and the online code's own
    }();
    (void)started;
}
int ProcessId() { return _getpid(); }
#else
using sock_t = int;
constexpr sock_t kBadSocket = -1;
void CloseSocket(sock_t s) { close(s); }
bool WouldBlock() { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR; }
bool SetNonBlocking(sock_t s) {
    const int flags = fcntl(s, F_GETFL, 0);
    return flags >= 0 && fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
}
void EnsureSockets() {
    static const bool once = [] {
        signal(SIGPIPE, SIG_IGN);  // a write to a closed connection is an error code, not the end of the game
        return true;
    }();
    (void)once;
}
int ProcessId() { return static_cast<int>(getpid()); }
#endif

sock_t S(std::intptr_t v) { return static_cast<sock_t>(v); }
std::intptr_t I(sock_t s) { return static_cast<std::intptr_t>(s); }

double SteadySeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

constexpr size_t kMaxOutBytes = 8u << 20;       // a client that does not read for this much is dropped
constexpr size_t kMaxConnections = kMaxClients + 4;  // sockets open at once, authenticated or not
constexpr double kAuthWindowS = 5.0;            // a connection that has not authenticated by then is closed

}  // namespace

// ---------------------------------------------------------------------------------------------------------------- token

std::string MakeToken() {
    std::random_device rd;
    uint64_t mix = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) ^ (static_cast<uint64_t>(ProcessId()) << 32);
    unsigned char bytes[16];
    for (int i = 0; i < 4; ++i) {
        uint32_t v = rd();
        mix = mix * 6364136223846793005ull + 1442695040888963407ull;  // the clock and the process id only add to the OS's entropy, they do not replace it
        v ^= static_cast<uint32_t>(mix >> 33);
        std::memcpy(bytes + i * 4, &v, 4);
    }
    static const char* kHex = "0123456789abcdef";
    std::string s;
    for (unsigned char b : bytes) {
        s.push_back(kHex[b >> 4]);
        s.push_back(kHex[b & 15]);
    }
    return s;
}

bool TokenEquals(std::string_view a, std::string_view b) {
    unsigned diff = static_cast<unsigned>(a.size() ^ b.size());
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) diff |= static_cast<unsigned>(static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]));
    return diff == 0;
}

// ---------------------------------------------------------------------------------------------------------------- a connection

struct LineServer::Client {
    sock_t sock = kBadSocket;
    double connected_at = 0.0;
    bool authed = false;
    int index = -1;
    std::string in;

    std::mutex out_mutex;
    std::string out;       // out_mutex
    bool dead = false;     // out_mutex: the socket is closed
    bool kill = false;     // out_mutex: drop at the next pass (the client did not read)
    bool close_after = false;  // out_mutex: drop when the out-buffer is empty

    mutable std::mutex stat_mutex;
    std::string name;
    std::string last_cmd;
    double last_cmd_time = 0.0;
    uint64_t commands = 0;
};

// ---------------------------------------------------------------------------------------------------------------- the server

LineServer::LineServer(Engine& engine, std::string token) : engine_(engine), token_(std::move(token)) {}

LineServer::~LineServer() {
    Stop();
}

bool LineServer::Start(uint16_t port, std::string* error) {
    if (running_.load(std::memory_order_acquire)) return true;
    EnsureSockets();
    sock_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == kBadSocket) {
        if (error) *error = "socket() failed";
        return false;
    }
#if defined(_WIN32)
    // Nobody else may bind this port while we hold it (SO_EXCLUSIVEADDRUSE), and no reuse of a port in TIME_WAIT is needed: the OS picks a free one.
    const BOOL exclusive = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
#else
    const int reuse = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
        CloseSocket(s);
        if (error) *error = "inet_pton failed";
        return false;
    }
    if (bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0 || listen(s, 8) != 0 || !SetNonBlocking(s)) {
        CloseSocket(s);
        if (error) *error = "cannot listen on 127.0.0.1:" + std::to_string(port);
        return false;
    }
    sockaddr_in bound{};
#if defined(_WIN32)
    int len = sizeof(bound);
#else
    socklen_t len = sizeof(bound);
#endif
    if (getsockname(s, reinterpret_cast<sockaddr*>(&bound), &len) != 0) {
        CloseSocket(s);
        if (error) *error = "getsockname failed";
        return false;
    }
    port_ = ntohs(bound.sin_port);
    listen_ = I(s);
    stop_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    engine_.SetEventSink([this](const std::string& line, std::string_view name) { BroadcastEvent(line, name); });
    thread_ = std::thread([this] { Run(); });
    return true;
}

void LineServer::Stop() {
    if (!running_.load(std::memory_order_acquire) && !thread_.joinable()) return;
    engine_.SetEventSink(nullptr);
    stop_.store(true, std::memory_order_release);
    if (thread_.joinable()) thread_.join();
    running_.store(false, std::memory_order_release);
}

std::vector<LineServer::ClientInfo> LineServer::Clients() const {
    std::vector<ClientInfo> out;
    std::lock_guard<std::mutex> lock(clients_mutex_);
    const double now = SteadySeconds();
    for (const auto& c : clients_) {
        if (!c->authed) continue;
        std::lock_guard<std::mutex> sl(c->stat_mutex);
        ClientInfo i;
        i.index = c->index;
        i.name = c->name;
        i.last_cmd = c->last_cmd;
        i.last_cmd_age_s = c->last_cmd.empty() ? 0.0 : now - c->last_cmd_time;
        i.commands = c->commands;
        out.push_back(std::move(i));
    }
    return out;
}

// Appends one line and sends what the socket takes now; the rest is flushed by the IO thread. Any thread.
static void SendNow(LineServer::Client& c, const std::string& line, bool close_after);

void LineServer::Send(const std::shared_ptr<Client>& c, const std::string& line) {
    SendNow(*c, line, false);
}

static void SendNow(LineServer::Client& c, const std::string& line, bool close_after) {
    std::lock_guard<std::mutex> lock(c.out_mutex);
    if (c.dead || c.kill) return;
    if (c.out.size() + line.size() + 1 > kMaxOutBytes) {
        c.kill = true;
        return;
    }
    c.out += line;
    c.out.push_back('\n');
    if (close_after) c.close_after = true;
    while (!c.out.empty()) {
        const int n = static_cast<int>(send(c.sock, c.out.data(), static_cast<int>(std::min<size_t>(c.out.size(), 1u << 20)), 0));
        if (n > 0) {
            c.out.erase(0, static_cast<size_t>(n));
            continue;
        }
        if (n < 0 && WouldBlock()) break;
        c.kill = true;  // the peer is gone
        break;
    }
}

void LineServer::SendReply(const std::shared_ptr<Client>& c, const Reply& reply) {
    SendNow(*c, ReplyLine(reply), false);
}

void LineServer::FlushOut(const std::shared_ptr<Client>& c) {
    std::lock_guard<std::mutex> lock(c->out_mutex);
    while (!c->dead && !c->out.empty()) {
        const int n = static_cast<int>(send(c->sock, c->out.data(), static_cast<int>(std::min<size_t>(c->out.size(), 1u << 20)), 0));
        if (n > 0) {
            c->out.erase(0, static_cast<size_t>(n));
            continue;
        }
        if (n < 0 && WouldBlock()) return;
        c->kill = true;
        return;
    }
}

void LineServer::Drop(const std::shared_ptr<Client>& c, const char* /*why*/) {
    bool was_authed = false;
    {
        std::lock_guard<std::mutex> lock(c->out_mutex);
        if (c->dead) return;
        c->dead = true;
        CloseSocket(c->sock);
        c->sock = kBadSocket;
        was_authed = c->authed;
    }
    std::string name;
    {
        std::lock_guard<std::mutex> sl(c->stat_mutex);
        name = c->name;
    }
    if (was_authed) {
        engine_.ResetClientFilter(c->index);
        Json f = Json::Object();
        f.Set("client", c->index);
        f.Set("name", name);
        f.Set("connected", false);
        engine_.Publish("agent", std::move(f));
    }
}

void LineServer::BroadcastEvent(const std::string& line, std::string_view name) {
    std::vector<std::shared_ptr<Client>> targets;
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        for (const auto& c : clients_) {
            if (c->authed && engine_.ShouldSend(c->index, name)) targets.push_back(c);
        }
    }
    for (const auto& c : targets) Send(c, line);
}

void LineServer::AcceptNew() {
    while (true) {
        sockaddr_in peer{};
#if defined(_WIN32)
        int len = sizeof(peer);
#else
        socklen_t len = sizeof(peer);
#endif
        sock_t s = accept(S(listen_), reinterpret_cast<sockaddr*>(&peer), &len);
        if (s == kBadSocket) return;
        if (!SetNonBlocking(s)) {
            CloseSocket(s);
            continue;
        }
        size_t open;
        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            open = clients_.size();
        }
        if (open >= kMaxConnections) {
            CloseSocket(s);
            continue;
        }
        auto c = std::make_shared<Client>();
        c->sock = s;
        c->connected_at = SteadySeconds();
        std::lock_guard<std::mutex> lock(clients_mutex_);
        clients_.push_back(std::move(c));
    }
}

void LineServer::ReadFrom(const std::shared_ptr<Client>& c) {
    char buf[65536];
    while (true) {
        const int n = static_cast<int>(recv(c->sock, buf, sizeof(buf), 0));
        if (n > 0) {
            c->in.append(buf, static_cast<size_t>(n));
            size_t start = 0;
            while (true) {
                const size_t nl = c->in.find('\n', start);
                if (nl == std::string::npos) break;
                std::string line = c->in.substr(start, nl - start);
                start = nl + 1;
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.size() > kMaxLineBytes) {
                    Reply r;
                    r.ok = false;
                    r.code = ErrorCode::kBadArgs;
                    r.message = "a line is longer than 1 MiB";
                    SendNow(*c, ReplyLine(r), true);
                    c->in.clear();
                    return;
                }
                if (!line.empty()) HandleLine(c, line);
                {
                    std::lock_guard<std::mutex> lock(c->out_mutex);
                    if (c->dead || c->close_after || c->kill) {
                        c->in.clear();
                        return;
                    }
                }
            }
            c->in.erase(0, start);
            if (c->in.size() > kMaxLineBytes) {
                Reply r;
                r.ok = false;
                r.code = ErrorCode::kBadArgs;
                r.message = "a line is longer than 1 MiB";
                SendNow(*c, ReplyLine(r), true);
                c->in.clear();
                return;
            }
            continue;
        }
        if (n < 0 && WouldBlock()) return;
        std::lock_guard<std::mutex> lock(c->out_mutex);
        c->kill = true;  // closed by the peer (n == 0) or broken
        return;
    }
}

void LineServer::HandleAuth(const std::shared_ptr<Client>& c, const Json& obj, bool has_id, int64_t id) {
    Reply r;
    r.has_id = has_id;
    r.id = id;
    const auto refuse = [&](const std::string& why) {
        r.ok = false;
        r.code = ErrorCode::kRefused;
        r.message = why;
        SendNow(*c, ReplyLine(r), true);
    };
    std::string token = obj.StringOr("token", "");
    if (token.empty()) token = obj.Get("args").StringOr("token", "");
    if (!TokenEquals(token, token_)) return refuse("the token is wrong (it is in director.json)");
    if (const Json* p = obj.Find("proto"); p && p->is_number() && p->AsInt() != kProtocolVersion) {
        return refuse("this game speaks protocol " + std::to_string(kProtocolVersion));
    }
    int index = -1;
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        for (int i = 0; i < static_cast<int>(kMaxClients) && index < 0; ++i) {
            const bool used = std::any_of(clients_.begin(), clients_.end(), [&](const auto& o) { return o->authed && o->index == i; });
            if (!used) index = i;
        }
    }
    if (index < 0) return refuse("too many clients (" + std::to_string(kMaxClients) + " at most)");
    std::string name = obj.StringOr("name", "");
    if (name.empty()) name = obj.Get("args").StringOr("name", "");
    if (name.size() > 64) name.resize(64);
    if (name.empty()) name = "agent " + std::to_string(index);
    {
        std::lock_guard<std::mutex> sl(c->stat_mutex);
        c->name = name;
    }
    engine_.ResetClientFilter(index);
    Json hello = hello_ ? hello_() : Json::Object();
    if (!hello.is_object()) hello = Json::Object();
    hello.Set("proto", kProtocolVersion);
    hello.Set("client", index);
    r.ok = true;
    r.result = std::move(hello);
    SendNow(*c, ReplyLine(r), false);
    // The others hear that this agent is in; it does not hear about itself (it is not authenticated yet while the event goes out). The auth reply is
    // the first line it reads.
    Json f = Json::Object();
    f.Set("client", index);
    f.Set("name", name);
    f.Set("connected", true);
    engine_.Publish("agent", std::move(f));
    std::lock_guard<std::mutex> lock(clients_mutex_);
    c->index = index;
    c->authed = true;
}

void LineServer::HandleLine(const std::shared_ptr<Client>& c, const std::string& line) {
    Json obj;
    std::string err;
    if (!Json::Parse(line, &obj, &err)) {
        Reply r;
        r.ok = false;
        r.code = ErrorCode::kBadArgs;
        r.message = "not valid JSON: " + err;
        SendNow(*c, ReplyLine(r), !c->authed);
        return;
    }
    bool has_id = false;
    int64_t id = 0;
    if (obj.is_object()) {
        if (const Json* j = obj.Find("id"); j && j->is_integer()) {
            has_id = true;
            id = j->AsInt();
        }
    }
    if (!c->authed) {
        if (obj.is_object() && obj.Get("cmd").is_string() && obj.Get("cmd").AsString() == "auth") return HandleAuth(c, obj, has_id, id);
        Reply r;
        r.has_id = has_id;
        r.id = id;
        r.ok = false;
        r.code = ErrorCode::kRefused;
        r.message = "authenticate first: {\"cmd\":\"auth\",\"token\":\"<from director.json>\"}";
        SendNow(*c, ReplyLine(r), true);
        return;
    }
    Posted posted;
    int64_t pid = 0;
    bool phas = false;
    if (!ParseRequest(obj, &posted, &err, &pid, &phas)) {
        Reply r;
        r.has_id = phas;
        r.id = pid;
        r.ok = false;
        r.code = ErrorCode::kBadArgs;
        r.message = err;
        SendNow(*c, ReplyLine(r), false);
        return;
    }
    if (posted.cmd == "auth") {
        Reply r;
        r.has_id = phas;
        r.id = pid;
        r.ok = false;
        r.code = ErrorCode::kRefused;
        r.message = "already authenticated";
        SendNow(*c, ReplyLine(r), false);
        return;
    }
    {
        std::lock_guard<std::mutex> sl(c->stat_mutex);
        c->last_cmd = FormatConsoleLine(posted.cmd, posted.args);
        c->last_cmd_time = SteadySeconds();
        ++c->commands;
    }
    posted.client = c->index;
    posted.source = Source::kAgent;
    const std::weak_ptr<Client> weak = c;
    posted.on_reply = [weak](const Reply& reply) {
        if (const auto client = weak.lock()) SendNow(*client, ReplyLine(reply), false);
    };
    if (!engine_.Post(std::move(posted))) {
        Reply r;
        r.has_id = phas;
        r.id = pid;
        r.ok = false;
        r.code = engine_.enabled() ? ErrorCode::kNotReady : ErrorCode::kRefused;
        r.message = engine_.enabled() ? "the Director's queue is full: wait for the replies you have coming" : "the Director is off";
        SendNow(*c, ReplyLine(r), false);
    }
}

void LineServer::Run() {
    while (!stop_.load(std::memory_order_acquire)) {
        fd_set rd, wr;
        FD_ZERO(&rd);
        FD_ZERO(&wr);
        sock_t maxfd = S(listen_);
        FD_SET(S(listen_), &rd);
        std::vector<std::shared_ptr<Client>> snapshot;
        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            snapshot = clients_;
        }
        for (const auto& c : snapshot) {
            bool want_write = false;
            {
                std::lock_guard<std::mutex> lock(c->out_mutex);
                if (c->dead) continue;
                want_write = !c->out.empty();
            }
            FD_SET(c->sock, &rd);
            if (want_write) FD_SET(c->sock, &wr);
#if !defined(_WIN32)
            maxfd = std::max(maxfd, c->sock);
#endif
        }
        timeval tv{0, 50 * 1000};
        const int ready = select(static_cast<int>(maxfd + 1), &rd, &wr, nullptr, &tv);
        if (ready > 0) {
            if (FD_ISSET(S(listen_), &rd)) AcceptNew();
            for (const auto& c : snapshot) {
                {
                    std::lock_guard<std::mutex> lock(c->out_mutex);
                    if (c->dead) continue;
                }
                if (FD_ISSET(c->sock, &rd)) ReadFrom(c);
                {
                    std::lock_guard<std::mutex> lock(c->out_mutex);
                    if (c->dead) continue;
                }
                if (FD_ISSET(c->sock, &wr)) FlushOut(c);
            }
        }
        // housekeeping: the dead, the killed, the ones that said their last word, the ones that never authenticated
        const double now = SteadySeconds();
        std::vector<std::shared_ptr<Client>> gone;
        for (const auto& c : snapshot) {
            bool drop = false;
            {
                std::lock_guard<std::mutex> lock(c->out_mutex);
                if (c->dead) {
                    drop = true;
                } else if (c->kill || (c->close_after && c->out.empty())) {
                    drop = true;
                }
            }
            if (!drop && !c->authed && now - c->connected_at > kAuthWindowS) drop = true;
            if (drop) {
                Drop(c, "housekeeping");
                gone.push_back(c);
            }
        }
        if (!gone.empty()) {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            clients_.erase(std::remove_if(clients_.begin(), clients_.end(), [&](const auto& c) { return std::find(gone.begin(), gone.end(), c) != gone.end(); }), clients_.end());
        }
    }
    // the end: tell the agents' side the connection is closing by closing it
    std::vector<std::shared_ptr<Client>> all;
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        all.swap(clients_);
    }
    for (const auto& c : all) Drop(c, "the server stopped");
    CloseSocket(S(listen_));
    listen_ = -1;
}

}  // namespace mc::cinematic::director
