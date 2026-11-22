#include "browser_query.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <thread>

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

template <size_t N>
std::string Clean(const char (&raw)[N], bool keep_newlines = false) {
    return CleanWire(raw, N, keep_newlines);
}

// Runs f(0) .. f(count - 1), kParallelQueries at a time, and returns when all are done.
template <typename F>
void ParallelFor(size_t count, F&& f) {
    std::atomic<size_t> next{0};
    std::vector<std::thread> pool;
    for (int i = 0; i < kParallelQueries && size_t(i) < count; ++i) {
        pool.emplace_back([&] {
            for (size_t k; (k = next.fetch_add(1)) < count;) f(k);
        });
    }
    for (auto& t : pool) t.join();
}

void QueryAll(const std::vector<Target>& targets, const Options& opts, const std::map<std::string, uint32_t>& known,
              std::vector<ServerView>& results, std::vector<std::vector<uint8_t>>& icons) {
    results.assign(targets.size(), ServerView{});
    icons.assign(targets.size(), {});
    ParallelFor(targets.size(), [&](size_t k) {
        auto it = known.find(targets[k].address);
        results[k] = QueryServer(targets[k], it == known.end() ? 0 : it->second, opts, icons[k]);
    });
}

}  // namespace

size_t Utf8SequenceLength(const unsigned char* p, size_t avail) {
    if (avail == 0) return 0;
    const unsigned char c = p[0];
    if (c < 0x80) return 1;
    auto cont = [&](size_t i, unsigned lo = 0x80, unsigned hi = 0xBF) {
        return i < avail && p[i] >= lo && p[i] <= hi;
    };
    if (c >= 0xC2 && c <= 0xDF) return cont(1) ? 2 : 0;
    if (c >= 0xE0 && c <= 0xEF) {
        const unsigned lo = c == 0xE0 ? 0xA0 : 0x80;
        const unsigned hi = c == 0xED ? 0x9F : 0xBF;  // no surrogates
        return cont(1, lo, hi) && cont(2) ? 3 : 0;
    }
    if (c >= 0xF0 && c <= 0xF4) {
        const unsigned lo = c == 0xF0 ? 0x90 : 0x80;
        const unsigned hi = c == 0xF4 ? 0x8F : 0xBF;  // nothing above U+10FFFF
        return cont(1, lo, hi) && cont(2) && cont(3) ? 4 : 0;
    }
    return 0;
}

std::string CleanWire(const char* raw, size_t cap, bool keep_newlines) {
    size_t len = 0;
    while (len < cap && raw[len]) ++len;
    std::string out;
    const auto* p = reinterpret_cast<const unsigned char*>(raw);
    for (size_t i = 0; i < len;) {
        const size_t n = Utf8SequenceLength(p + i, len - i);
        if (n == 0) {
            ++i;
            continue;
        }
        const unsigned char c = p[i];
        if (n == 1 && (c < 0x20 || c == 0x7F) && !(keep_newlines && c == '\n')) {
            ++i;
            continue;
        }
        out.append(raw + i, n);
        i += n;
    }
    return out;
}

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

void AddTarget(std::vector<Target>& list, std::set<std::string>& seen, const std::string& text, bool listed,
               bool favorite, bool current) {
    std::string trimmed;
    for (char c : text)
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') trimmed += c;
    if (trimmed.empty()) return;
    Target t;
    t.port = kDefaultRelayPort;
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

void DecodeDetails(const DetailsMsg& a, ServerView& v) {
    v.name = Clean(a.name);
    v.description = Clean(a.description, true);
    v.region = Clean(a.region);
    v.tags = Clean(a.tags);
    v.players = a.players;
    v.rooms = a.rooms;
    v.flags = a.flags;
    v.icon_size = a.icon_size;
    v.icon_crc = a.icon_size ? a.icon_crc : 0;
    v.http_port = a.http_port;
    v.mods_required = a.mods_required;
    v.mods_optional = a.mods_optional;
    v.mods_crc = a.mods_crc;
}

void DecodeLegacy(const relay_info::Info& info, ServerView& v) {
    v.answered = true;
    v.legacy = true;
    v.ping_ms = info.ping_ms;
    v.name = CleanWire(info.name.data(), info.name.size());
    v.players = info.players;
    v.rooms = info.sessions;
    v.icon_size = info.icon_size;
    v.icon_crc = info.icon_size ? info.icon_crc : 0;
}

bool PngHeaderFits(const uint8_t* data, size_t size, uint32_t max_side) {
    static const uint8_t kSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (!data || size < 24 || std::memcmp(data, kSig, 8) != 0 || std::memcmp(data + 12, "IHDR", 4) != 0) return false;
    auto be32 = [&](size_t o) {
        return (uint32_t(data[o]) << 24) | (uint32_t(data[o + 1]) << 16) | (uint32_t(data[o + 2]) << 8) | uint32_t(data[o + 3]);
    };
    const uint32_t w = be32(16), h = be32(20);
    return w != 0 && h != 0 && w <= max_side && h <= max_side;
}

int AppendRooms(const RoomsMsg& page, std::set<uint32_t>& seen, std::vector<RoomView>& out) {
    const int count = std::min<int>(page.count, kRoomsPerPage);
    for (int i = 0; i < count; ++i) {
        const Room& r = page.rooms[i];
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
        out.push_back(std::move(rv));
    }
    return count;
}

ServerView QueryServer(const Target& t, uint32_t known_icon_crc, const Options& opts, std::vector<uint8_t>& icon_out) {
    ServerView v;
    v.address = t.address;
    v.host = t.host;
    v.port = t.port;
    v.listed = t.listed;
    v.favorite = t.favorite;
    v.current = t.current;
    v.queried = true;

    auto want_icon = [&](uint32_t crc) {
        return opts.fetch_icons && crc && crc != known_icon_crc && !(opts.have_icon && opts.have_icon(crc));
    };

    SocketHandle s = Open(t.host, t.port);
    if (s == kNoSocket) return v;

    DetailsMsg q{};
    FillHeader(q.hdr, kDetailsQuery);
    q.token = NewToken();
    DetailsMsg a{};
    const int rtt = Exchange(s, q, a, kDetails, opts.udp_timeout_ms);
    if (rtt >= 0) {
        v.answered = true;
        v.ping_ms = rtt;
        DecodeDetails(a, v);

        std::set<uint32_t> seen;
        for (int page = 0; page < kMaxRoomPages; ++page) {
            RoomsMsg rq{};
            FillHeader(rq.hdr, kRoomsQuery);
            rq.token = NewToken();
            rq.page = uint16_t(page);
            RoomsMsg ra{};
            if (Exchange(s, rq, ra, kRooms, opts.udp_timeout_ms) < 0) break;
            const int count = AppendRooms(ra, seen, v.room_list);
            if ((page + 1) * kRoomsPerPage >= ra.total || count == 0) break;
        }
        if (want_icon(v.icon_crc))
            icon_out = relay_info::FetchIcon(t.host, t.port, a.icon_crc, a.icon_size, opts.udp_timeout_ms);
    }
    CloseSocket(s);
    if (!v.answered) {
        // A relay from before the browser (16/10 or older) still answers the
        // online pop-up's info query: name, players, ping, icon. No rooms.
        const relay_info::Info info = relay_info::Query(t.host, t.port, opts.udp_timeout_ms);
        if (info.ok) {
            DecodeLegacy(info, v);
            if (want_icon(v.icon_crc))
                icon_out = relay_info::FetchIcon(t.host, t.port, info.icon_crc, info.icon_size, opts.udp_timeout_ms);
        }
    }
    return v;
}

std::vector<std::string> SplitLines(const std::string& body) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos < body.size()) {
        size_t nl = body.find('\n', pos);
        if (nl == std::string::npos) nl = body.size();
        out.push_back(body.substr(pos, nl - pos));
        pos = nl + 1;
    }
    return out;
}

MasterAnswer FetchMaster(const std::string& address, const Options& opts) {
    MasterAnswer out;
    std::string host;
    uint16_t port = kMasterPort;
    SplitHostPort(address, host, port);
    const auto res = rmr::net::Post(host, port, "/servers", {}, opts.master_connect_timeout_ms,
                                    opts.master_io_timeout_ms);
    out.transport_ok = res.transport_ok;
    out.http_status = res.status;
    out.detail = res.error;
    if (!res.transport_ok || res.status != 200) {
        out.text = res.transport_ok ? "master answered HTTP " + std::to_string(res.status)
                                    : "master unreachable (" + address + ")";
        return out;
    }
    out.lines = SplitLines(std::string(res.body.begin(), res.body.end()));
    out.ok = true;
    return out;
}

Collected Collect(const Sources& src, const Options& opts, const std::map<std::string, uint32_t>& known) {
    Collected out;

    // Who to ask: the master's list, the player's own, the current relay.
    std::vector<Target> targets;
    std::set<std::string> seen;
    if (!src.master.empty()) {
        const MasterAnswer m = FetchMaster(src.master, opts);
        out.master.text = m.text;
        out.master.detail = m.detail;
        out.master.http_status = m.http_status;
        out.master.state = m.ok ? MasterStatus::State::kOk
                                : (m.transport_ok ? MasterStatus::State::kHttpError : MasterStatus::State::kUnreachable);
        out.master.lines = int(m.lines.size());
        for (const auto& a : m.lines) AddTarget(targets, seen, a, true, false, false);
    }
    size_t pos = 0;
    while (pos <= src.favorites.size()) {
        size_t comma = src.favorites.find(',', pos);
        if (comma == std::string::npos) comma = src.favorites.size();
        AddTarget(targets, seen, src.favorites.substr(pos, comma - pos), false, true, false);
        pos = comma + 1;
    }
    if (!src.current.empty()) AddTarget(targets, seen, src.current, false, false, true);

    std::vector<ServerView> results;
    std::vector<std::vector<uint8_t>> icons;
    QueryAll(targets, opts, known, results, icons);

    // An address of the player's own that is no relay may be a master
    // (the server list): its relays join the list and it leaves it. The addresses are tried together (a refused or
    // silent one costs the connect timeout, and a player may have several that are off), the answers read in order.
    std::vector<size_t> probe;
    for (size_t k = 0; k < results.size(); ++k)
        if (!results[k].answered && targets[k].favorite) probe.push_back(k);
    std::vector<MasterAnswer> answers(probe.size());
    ParallelFor(probe.size(), [&](size_t i) { answers[i] = FetchMaster(targets[probe[i]].address, opts); });
    std::vector<Target> more;
    int masters = 0;
    for (size_t i = 0; i < probe.size(); ++i) {
        if (!answers[i].ok) continue;
        ++masters;
        results[probe[i]].address.clear();  // dropped below
        for (const auto& a : answers[i].lines) AddTarget(more, seen, a, true, false, false);
    }
    if (!more.empty()) {
        std::vector<ServerView> more_results;
        std::vector<std::vector<uint8_t>> more_icons;
        QueryAll(more, opts, known, more_results, more_icons);
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
    out.master.via_favorites = masters;
    if (masters) out.master.text.clear();
    out.servers = std::move(results);
    out.icons = std::move(icons);
    return out;
}

}  // namespace larecomp::browser
