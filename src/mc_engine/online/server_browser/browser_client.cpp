#include "browser_client.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <thread>

#include <rex/cvar.h>

#include "../rate_my_ride/rmr_net.h"
#include "../relay_info.h"

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

namespace larecomp::browser {
namespace {

using Clock = std::chrono::steady_clock;

constexpr int kQueryTimeoutMs = 800;
constexpr int kMaxServers = 128;
constexpr int kParallelQueries = 8;
constexpr int kMaxRoomPages = 16;  // 64 rooms
constexpr auto kAutoRefreshEvery = std::chrono::seconds(8);

void EnsureWinsock() {
#ifdef _WIN32
    static bool started = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    (void)started;
#endif
}

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

int Receive(SocketHandle s, void* buf, int size, int timeout_ms) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(s, &set);
    timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    if (select(int(s) + 1, &set, nullptr, nullptr, &tv) <= 0) return -1;
    return int(recv(s, static_cast<char*>(buf), size, 0));
}

uint32_t NewToken() {
    static std::mutex m;
    static std::mt19937 rng{std::random_device{}()};
    std::lock_guard<std::mutex> lk(m);
    return rng();
}

// Sends `query` and waits for an answer of the same type and size carrying
// the same token (the token is right after the header in every message).
// Returns the round trip in ms, or -1.
template <typename Msg>
int Exchange(SocketHandle s, Msg& query, Msg& answer, uint8_t answer_type, int timeout_ms) {
    const auto sent = Clock::now();
    if (send(s, reinterpret_cast<const char*>(&query), int(sizeof(query)), 0) != int(sizeof(query)))
        return -1;
    int left = timeout_ms;
    while (left > 0) {
        const int n = Receive(s, &answer, int(sizeof(answer)), left);
        const int elapsed =
            int(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - sent).count());
        if (n < 0) return -1;
        if (n == int(sizeof(answer)) && answer.hdr.magic == kMagic && answer.hdr.version == kVersion &&
            answer.hdr.type == answer_type && answer.token == query.token)
            return elapsed;
        left = timeout_ms - elapsed;
    }
    return -1;
}

void FillHeader(Header& h, uint8_t type) {
    h.magic = kMagic;
    h.version = kVersion;
    h.type = type;
}

// Strings off the wire: terminate, keep valid UTF-8, drop control
// characters (newlines only where asked).
template <size_t N>
std::string Clean(const char (&raw)[N], bool keep_newlines = false) {
    size_t len = 0;
    while (len < N && raw[len]) ++len;
    std::string out;
    const auto* p = reinterpret_cast<const unsigned char*>(raw);
    for (size_t i = 0; i < len;) {
        const unsigned char c = p[i];
        const size_t n = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
        bool ok = n != 0 && i + n <= len && !(n == 2 && c < 0xC2) && !(n == 4 && c > 0xF4);
        for (size_t k = 1; ok && k < n; ++k) ok = (p[i + k] & 0xC0) == 0x80;
        if (!ok) {
            ++i;
            continue;
        }
        if (n == 1 && (c < 0x20 || c == 0x7F) && !(keep_newlines && c == '\n')) {
            ++i;
            continue;
        }
        out.append(raw + i, n);
        i += n;
    }
    return out;
}

struct Target {
    std::string address;
    std::string host;
    uint16_t port = 0;
    bool listed = false;
    bool favorite = false;
    bool current = false;
};

void AddTarget(std::vector<Target>& list, std::set<std::string>& seen, const std::string& text,
               bool listed, bool favorite, bool current) {
    std::string trimmed;
    for (char c : text)
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') trimmed += c;
    if (trimmed.empty()) return;
    Target t;
    t.port = 31000;
    SplitHostPort(trimmed, t.host, t.port);
    if (t.host.empty() || t.host.size() > 253) return;
    for (char c : t.host)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-')) return;
    t.address = t.host + ":" + std::to_string(t.port);
    for (auto& existing : list) {
        if (existing.address == t.address) {
            existing.listed |= listed;
            existing.favorite |= favorite;
            existing.current |= current;
            return;
        }
    }
    if (list.size() >= size_t(kMaxServers) || !seen.insert(t.address).second) return;
    t.listed = listed;
    t.favorite = favorite;
    t.current = current;
    list.push_back(t);
}

// What one relay says about itself, its rooms and (when it changed) its icon.
ServerView QueryServer(const Target& t, uint32_t known_icon_crc, std::vector<uint8_t>& icon_out) {
    ServerView v;
    v.address = t.address;
    v.host = t.host;
    v.port = t.port;
    v.listed = t.listed;
    v.favorite = t.favorite;
    v.current = t.current;
    v.queried = true;

    SocketHandle s = Open(t.host, t.port);
    if (s == kNoSocket) return v;

    DetailsMsg q{};
    FillHeader(q.hdr, kDetailsQuery);
    q.token = NewToken();
    DetailsMsg a{};
    const int rtt = Exchange(s, q, a, kDetails, kQueryTimeoutMs);
    if (rtt >= 0) {
        v.answered = true;
        v.ping_ms = rtt;
        v.name = Clean(a.name);
        v.description = Clean(a.description, true);
        v.region = Clean(a.region);
        v.tags = Clean(a.tags);
        v.players = a.players;
        v.rooms = a.rooms;
        v.icon_crc = a.icon_size ? a.icon_crc : 0;
        v.http_port = a.http_port;
        v.mods_required = a.mods_required;
        v.mods_optional = a.mods_optional;
        v.mods_crc = a.mods_crc;

        std::set<uint32_t> seen;
        for (int page = 0; page < kMaxRoomPages; ++page) {
            RoomsMsg rq{};
            FillHeader(rq.hdr, kRoomsQuery);
            rq.token = NewToken();
            rq.page = uint16_t(page);
            RoomsMsg ra{};
            if (Exchange(s, rq, ra, kRooms, kQueryTimeoutMs) < 0) break;
            const int count = std::min<int>(ra.count, kRoomsPerPage);
            for (int i = 0; i < count; ++i) {
                const Room& r = ra.rooms[i];
                if (!seen.insert(r.host_vip).second) continue;  // moved between pages
                RoomView rv;
                rv.host_vip = r.host_vip;
                rv.host_rtt_ms = r.host_rtt_ms == 0xFFFF ? -1 : r.host_rtt_ms;
                rv.cur_players = r.cur_players;
                rv.max_players = r.max_players;
                rv.active = (r.flags & kRoomActive) != 0;
                rv.gamertag = Clean(r.gamertag);
                rv.name = Clean(r.name);
                const int props = std::min<int>(r.prop_count, kRoomProps);
                rv.props.assign(r.props, r.props + props);
                v.room_list.push_back(std::move(rv));
            }
            if ((page + 1) * kRoomsPerPage >= ra.total || count == 0) break;
        }
        if (v.icon_crc && v.icon_crc != known_icon_crc)
            icon_out = relay_info::FetchIcon(t.host, t.port, a.icon_crc, a.icon_size, kQueryTimeoutMs);
    }
    CloseSocket(s);
    if (!v.answered) {
        // A relay from before the browser (16/10 or older) still answers the
        // online pop-up's info query: name, players, ping, icon. No rooms.
        const relay_info::Info info = relay_info::Query(t.host, t.port, kQueryTimeoutMs);
        if (info.ok) {
            v.answered = true;
            v.legacy = true;
            v.ping_ms = info.ping_ms;
            v.name = info.name;
            v.players = info.players;
            v.rooms = info.sessions;
            v.icon_crc = info.icon_size ? info.icon_crc : 0;
            if (v.icon_crc && v.icon_crc != known_icon_crc)
                icon_out = relay_info::FetchIcon(t.host, t.port, info.icon_crc, info.icon_size, kQueryTimeoutMs);
        }
    }
    return v;
}

// The master's list, as "host:port" lines. False when `address` is not a
// master (or does not answer).
bool FetchMaster(const std::string& address, std::vector<std::string>& out, std::string& error) {
    std::string host;
    uint16_t port = kMasterPort;
    SplitHostPort(address, host, port);
    const auto res = rmr::net::Post(host, port, "/servers", {}, 1500, 3000);
    if (!res.transport_ok || res.status != 200) {
        error = res.transport_ok ? "master answered HTTP " + std::to_string(res.status)
                                 : "master unreachable (" + address + ")";
        return false;
    }
    std::string body(res.body.begin(), res.body.end());
    size_t pos = 0;
    while (pos < body.size()) {
        size_t nl = body.find('\n', pos);
        if (nl == std::string::npos) nl = body.size();
        out.push_back(body.substr(pos, nl - pos));
        pos = nl + 1;
    }
    return true;
}

void QueryAll(const std::vector<Target>& targets, const std::map<std::string, uint32_t>& known,
              std::vector<ServerView>& results, std::vector<std::vector<uint8_t>>& icons) {
    results.assign(targets.size(), ServerView{});
    icons.assign(targets.size(), {});
    std::atomic<size_t> next{0};
    std::vector<std::thread> pool;
    for (int i = 0; i < kParallelQueries && size_t(i) < targets.size(); ++i) {
        pool.emplace_back([&] {
            for (size_t k; (k = next.fetch_add(1)) < targets.size();) {
                auto it = known.find(targets[k].address);
                results[k] = QueryServer(targets[k], it == known.end() ? 0 : it->second, icons[k]);
            }
        });
    }
    for (auto& t : pool) t.join();
}

}  // namespace

void SplitHostPort(const std::string& in, std::string& host, uint16_t& port) {
    host = in;
    const size_t colon = in.rfind(':');
    if (colon == std::string::npos) return;
    const int p = std::atoi(in.c_str() + colon + 1);
    host.resize(colon);
    if (p > 0 && p < 65536) port = uint16_t(p);
}

bool SendTargetHint(const std::string& host, uint16_t port, uint32_t host_vip, int timeout_ms) {
    SocketHandle s = Open(host, port);
    if (s == kNoSocket) return false;
    TargetHintMsg q{};
    FillHeader(q.hdr, kTargetHint);
    q.token = NewToken();
    q.host_vip = host_vip;
    TargetHintMsg a{};
    const bool ok = Exchange(s, q, a, kTargetHint, timeout_ms) >= 0;
    CloseSocket(s);
    return ok;
}

struct Client::State {
    mutable std::mutex mutex;
    std::condition_variable wake;
    bool stop = false;
    bool refresh_asked = true;
    bool auto_refresh = false;
    Snapshot snapshot;
    std::map<std::string, uint32_t> icon_crcs;  // address -> crc already fetched
    std::vector<IconUpdate> icons;
};

namespace {

void Worker(std::shared_ptr<Client::State> st) {
    Clock::time_point last{};
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(st->mutex);
            st->wake.wait_for(lk, std::chrono::milliseconds(500), [&] {
                return st->stop || st->refresh_asked ||
                       (st->auto_refresh && Clock::now() - last >= kAutoRefreshEvery);
            });
            if (st->stop) return;
            if (!st->refresh_asked && !(st->auto_refresh && Clock::now() - last >= kAutoRefreshEvery))
                continue;
            st->refresh_asked = false;
            st->snapshot.refreshing = true;
            ++st->snapshot.generation;
        }
        last = Clock::now();

        // Who to ask: the master's list, the player's own, the current relay.
        std::vector<Target> targets;
        std::set<std::string> seen;
        std::string master_status;
        const std::string master = rex::cvar::GetFlagByName("online_master_server");
        if (!master.empty()) {
            std::vector<std::string> listed;
            FetchMaster(master, listed, master_status);
            for (const auto& a : listed) AddTarget(targets, seen, a, true, false, false);
        }
        const std::string favorites = rex::cvar::GetFlagByName("server_browser_servers");
        size_t pos = 0;
        while (pos <= favorites.size()) {
            size_t comma = favorites.find(',', pos);
            if (comma == std::string::npos) comma = favorites.size();
            AddTarget(targets, seen, favorites.substr(pos, comma - pos), false, true, false);
            pos = comma + 1;
        }
        {
            std::string host;
            uint16_t port = 0;
            relay_info::ConfiguredServer(host, port);
            if (!rex::cvar::GetFlagByName("mcla_online_server").empty())
                AddTarget(targets, seen, host + ":" + std::to_string(port), false, false, true);
        }

        std::vector<ServerView> results;
        std::vector<std::vector<uint8_t>> icons;
        std::map<std::string, uint32_t> known;
        {
            std::lock_guard<std::mutex> lk(st->mutex);
            known = st->icon_crcs;
        }
        QueryAll(targets, known, results, icons);

        // An address of the player's own that is no relay may be a master
        // (the server list): its relays join the list and it leaves it.
        std::vector<Target> more;
        int masters = 0;
        for (size_t k = 0; k < results.size(); ++k) {
            if (results[k].answered || !targets[k].favorite) continue;
            std::vector<std::string> listed;
            std::string ignored;
            if (!FetchMaster(targets[k].address, listed, ignored)) continue;
            ++masters;
            results[k].address.clear();  // dropped below
            for (const auto& a : listed) AddTarget(more, seen, a, true, false, false);
        }
        if (!more.empty()) {
            std::vector<ServerView> more_results;
            std::vector<std::vector<uint8_t>> more_icons;
            QueryAll(more, known, more_results, more_icons);
            for (size_t k = 0; k < more_results.size(); ++k) {
                results.push_back(std::move(more_results[k]));
                icons.push_back(std::move(more_icons[k]));
            }
        }
        for (size_t k = results.size(); k-- > 0;) {
            if (!results[k].address.empty()) continue;
            results.erase(results.begin() + std::ptrdiff_t(k));
            icons.erase(icons.begin() + std::ptrdiff_t(k));
        }
        if (masters) master_status.clear();
        if (master.empty() && !masters)
            master_status = "no server list set (online_master_server)";

        std::lock_guard<std::mutex> lk(st->mutex);
        for (size_t k = 0; k < results.size(); ++k) {
            if (icons[k].empty()) continue;
            st->icon_crcs[results[k].address] = results[k].icon_crc;
            st->icons.push_back({results[k].address, results[k].icon_crc, std::move(icons[k])});
        }
        st->snapshot.servers = std::move(results);
        st->snapshot.master_status = master_status;
        st->snapshot.refreshing = false;
        ++st->snapshot.generation;
    }
}

}  // namespace

Client::Client() : state_(std::make_shared<State>()) { std::thread(Worker, state_).detach(); }

Client::~Client() {
    std::lock_guard<std::mutex> lk(state_->mutex);
    state_->stop = true;
    state_->wake.notify_all();
}

void Client::Refresh() {
    std::lock_guard<std::mutex> lk(state_->mutex);
    state_->refresh_asked = true;
    state_->wake.notify_all();
}

void Client::SetAutoRefresh(bool on) {
    std::lock_guard<std::mutex> lk(state_->mutex);
    if (on && !state_->auto_refresh) state_->refresh_asked = true;
    state_->auto_refresh = on;
    state_->wake.notify_all();
}

Snapshot Client::Take() const {
    std::lock_guard<std::mutex> lk(state_->mutex);
    return state_->snapshot;
}

std::vector<IconUpdate> Client::TakeIcons() {
    std::vector<IconUpdate> out;
    std::lock_guard<std::mutex> lk(state_->mutex);
    out.swap(state_->icons);
    return out;
}

}  // namespace larecomp::browser
