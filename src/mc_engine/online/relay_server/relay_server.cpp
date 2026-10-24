// LARecomp — online matchmaker + packet relay for System Link.
//
// Two roles:
//  1. Session directory: hosts register their advertised game; clients query.
//  2. Relay: forwards emulated LAN broadcast/unicast game packets between peers
//     (loopback broadcast can't cross processes; this makes it work over WANs
//     too). Each peer is assigned a unique VIRTUAL IP used as its identity.
//
// The wire protocol is fixed (larecomp_net_protocol.h, byte-identical to the SDK's
// mcla_online_protocol.h). Everything in this file is server-internal.
//
// Identity. The game caches its own address (the vip) in its XNADDR for the
// whole run, so a vip must never change while a game is running. A peer is
// keyed by the session id its client sends in every register and heartbeat,
// not by its ip:port:
//  - NAT / VPN port change: the same session id from a new ip:port is the same
//    peer with the same vip; only the return address moves.
//  - Relay restart or a long outage: the client keeps heartbeating with the vip
//    it already has, and the relay adopts it back. Fresh vips come from a
//    counter persisted in relay_state.txt next to the exe, so a vip handed out
//    before a restart is never given to anyone else after it.
//  - Relay packets from an address that has not identified itself are dropped
//    (its next heartbeat, at most ~5 s away, identifies it) instead of minting
//    a new peer with a new vip.
//
// Rooms. A room is the set of peers that exchanged game data (unicast on any
// port but the discovery port 1001) in the last kLinkTtlMs, so it splits by
// itself when that traffic stops. LAN discovery never groups anyone: every
// host answers every finder, which says nothing about who plays with whom.
// Rooms scope in-game broadcasts and group the dashboard.
//
// Server browser. The relay describes itself (server.toml next to the exe:
// name, description, region, tags, icon) and its rooms to the in-game server
// browser, and announces itself to the master server so it gets listed there
// (../protocol/browser_protocol.h). It also serves the mods it plays
// with (mods/ next to the exe) over HTTP, so players can fetch them.
//
// Build: the larecomp_relay_server CMake target, or standalone
//        clang++ -O2 -std=c++17 relay_server.cpp -x c++ ../../../../thirdparty/monocypher/monocypher.c
//                -o larecomp-server.exe
//        (Windows / Winsock2 via the pragma below, or POSIX).
//
// Nicks. With identity_master_key in server.toml a gamertag only gets in from
// an IP that proved it owns it (../protocol/identity_protocol.h): the register
// of anyone else gets no ack, so their game finds no server.

#define _CRT_SECURE_NO_WARNINGS
#include "../protocol/larecomp_net_protocol.h"
#include "../protocol/identity_protocol.h"
#include "../protocol/browser_protocol.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <cstdarg>
#include <ctime>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
#define LARECOMP_CLOSESOCK closesocket
#define LARECOMP_LASTERR WSAGetLastError()
// Defined in mstcpip.h; declare it here so we don't depend on that header.
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netdb.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
#ifndef INVALID_SOCKET
#define INVALID_SOCKET (-1)
#endif
#ifndef SOCKET_ERROR
#define SOCKET_ERROR (-1)
#endif
#define LARECOMP_CLOSESOCK ::close
#define LARECOMP_LASTERR errno
#endif

using namespace larecomp::net;

namespace {

// The LAN discovery port: broadcasts on it reach every peer, and a unicast to
// it is a host answering a finder.
constexpr uint16_t kDiscoveryPort = 1001;
// How long a game-data link keeps two peers in the same room after its last
// packet. Players in one session trade keepalives far more often than this.
constexpr int64_t kLinkTtlMs = 20000;
// A host counts as live if it answered a LAN search this recently, or has
// players in its room. A game that already left its session keeps advertising
// itself as host, so this is what tells a real session from a stale one.
constexpr int64_t kHostReplyTtlMs = 90000;
// After a host answers a finder's LAN search, the two reach each other with
// in-game broadcasts (ports other than 1001) for this long, without joining a
// room. Covers whatever the join handshake broadcasts before the first
// unicast; the old relay got that by merging rooms on the discovery reply.
constexpr int64_t kLanReachMs = 30000;
// Vip range. 10.0.0.1 is never handed out fresh (see VipAllocatable).
constexpr uint32_t kVipFirst = 0x0A000002;
constexpr uint32_t kVipLast = 0x0AFFFFFE;

struct Peer {
  sockaddr_in addr{};       // control socket address (where we send to this peer)
  uint64_t sid = 0;         // the client's session id: this peer's identity
  uint32_t virtual_ip = 0;  // assigned identity (host order)
  uint32_t room = 0;        // lowest vip among the peers it trades game data with
  SessionInfo info{};       // last registered session (is_host says if advertised)
  int64_t last_seen_ms = 0;
  int64_t connect_ms = 0;   // first seen (for the "connected for" column)
  int64_t lan_reply_ms = 0; // last time it answered a LAN search (0 = never)
  uint64_t rx_pkts = 0, tx_pkts = 0, rx_bytes = 0, tx_bytes = 0;  // relay counters
  int rtt_ms = -1;          // last measured round trip (ms); -1 = unknown
  uint64_t ping_token = 0;  // token of the outstanding RTT probe
  int64_t ping_sent_ms = 0; // when that probe was sent
};

int64_t NowMs() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void LogInfo(const char* fmt, ...) {
  std::time_t t = std::time(nullptr);
  std::tm* tm_info = std::localtime(&t);
  char time_str[32];
  std::strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);

  std::printf("[%s] [mcla-server] ", time_str);

  va_list args;
  va_start(args, fmt);
  std::vprintf(fmt, args);
  va_end(args);
}

uint64_t KeyOf(const sockaddr_in& a) {
  return (static_cast<uint64_t>(a.sin_addr.s_addr) << 16) | a.sin_port;
}

void IpToStr(uint32_t net_ip, char* out, size_t n) {
  in_addr a;
  a.s_addr = net_ip;
  inet_ntop(AF_INET, &a, out, n);
}

std::string AddrStr(const sockaddr_in& a) {
  char ip[64];
  IpToStr(a.sin_addr.s_addr, ip, sizeof(ip));
  return std::string(ip) + ":" + std::to_string(ntohs(a.sin_port));
}

// Vips are host order; print all four octets (past 10.0.0.255 the last one
// alone is ambiguous).
std::string VipStr(uint32_t vip) {
  char b[20];
  std::snprintf(b, sizeof(b), "%u.%u.%u.%u", (vip >> 24) & 0xFF, (vip >> 16) & 0xFF,
                (vip >> 8) & 0xFF, vip & 0xFF);
  return b;
}

bool InVipRange(uint32_t v) {
  return (v & 0xFF000000u) == 0x0A000000u && (v & 0x00FFFFFFu) != 0;
}

// Addresses never handed out fresh, because the client mishandles them:
//  - low byte 0xFF: its sendto treats any x.x.x.255 as a LAN broadcast;
//  - low byte 0x00: network address;
//  - low 16 bits 0x0001: the client builds its fake MAC (abEnet) from the low
//    16 bits of its address, and a client that started offline uses 127.0.0.1,
//    which gives the same MAC as 10.x.0.1.
// A vip a client already carries is adopted even if it is one of these:
// changing it would strand that game.
bool VipAllocatable(uint32_t v) {
  if (!InVipRange(v) || v > kVipLast) return false;
  uint32_t lo = v & 0xFF;
  if (lo == 0x00 || lo == 0xFF) return false;
  if ((v & 0xFFFF) == 0x0001) return false;
  return true;
}

uint64_t LinkKey(uint32_t a, uint32_t b) {
  return a < b ? (static_cast<uint64_t>(a) << 32) | b : (static_cast<uint64_t>(b) << 32) | a;
}

// Allocator state, persisted so a relay restart keeps counting from where it
// stopped instead of handing out vips that running games still hold.
std::string g_state_path;

void SaveState(uint32_t next_vip) {
  if (g_state_path.empty()) return;
  FILE* f = std::fopen(g_state_path.c_str(), "w");
  if (!f) return;
  std::fprintf(f, "next_vip %u\n", next_vip);
  std::fclose(f);
}

uint32_t LoadState(bool* loaded) {
  *loaded = false;
  FILE* f = std::fopen(g_state_path.c_str(), "r");
  if (!f) return kVipFirst;
  unsigned v = 0;
  int ok = std::fscanf(f, "next_vip %u", &v);
  std::fclose(f);
  if (ok != 1 || !InVipRange(v)) return kVipFirst;
  *loaded = true;
  return v;
}

// Peer-supplied strings (gamertag, session name) arrive straight off the wire,
// so treat them as hostile: they may be unterminated -- printf would overread
// past the field -- and may carry control characters or escape sequences aimed
// at whatever console reads the log. Copy into a bounded buffer, hard
// terminate, and replace anything non printable.
template <size_t N>
struct SafeStr {
  char text[N + 1];

  explicit SafeStr(const char (&raw)[N]) {
    size_t i = 0;
    for (; i < N; ++i) {
      unsigned char c = static_cast<unsigned char>(raw[i]);
      if (c == 0) break;
      text[i] = (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '?';
    }
    text[i] = '\0';
    if (i == 0) {
      std::snprintf(text, sizeof(text), "<unnamed>");
    }
  }

  const char* c_str() const { return text; }
};

template <size_t N>
SafeStr<N> Safe(const char (&raw)[N]) {
  return SafeStr<N>(raw);
}

// ---------------------------------------------------------------------------
// Admin panel: web dashboard (read) + kick/ban (token-gated).
// The main relay thread OWNS the peer table. This HTTP server runs on its own
// thread and only touches three shared things under g_mtx: it READS the JSON
// snapshot the main loop publishes, and it PUSHES admin commands the main loop
// drains. It never touches the peer map directly, so the hot relay path stays
// lock-free.
// ---------------------------------------------------------------------------
struct AdminCmd {
  int type;         // 0 = kick (drop + short cooldown), 1 = ban (block ip), 2 = unban
  uint32_t ip_net;  // target ip, network byte order
};

std::mutex g_mtx;
std::string g_json = "{\"list\":[]}";  // latest state snapshot (built by main loop)
std::vector<AdminCmd> g_cmds;          // pending admin actions (drained by main loop)
std::string g_token;                   // admin token; every request must carry ?key=

// ---------------------------------------------------------------------------
// Server identity, shown in the players' online pop-up (larecomp
// src/mc_engine/online/relay_info.cpp) and in the in-game server browser
// (src/mc_engine/online/server_browser). Set by the operator in server.toml
// next to the exe:
//
//   name = "My Relay"                  47 bytes at most
//   description = "Line one\nLine two" 255 bytes at most
//   region = "BR"                      23 bytes at most
//   tags = "Tokyo, drift"              comma-separated, 95 bytes at most
//   icon = "relay_icon.png"            square PNG, 64 KB and 512x512 at most
//   public_address = "x.ply.gg:1234"   where players reach this relay; empty =
//                                      the machine's own IP and the relay port
//   master = "master.example:31003"    the server list to appear on; empty = unlisted
//   mods_dir = "mods"                  folder in the exe's folder with one sub-folder
//                                      per mod (mods/<mod>/..., like the game's
//                                      models/<mod>/); served to players over HTTP
//   mods_required = "nissan_s15, tokyo" comma-separated, or "*" = every mod in
//                                      mods_dir; the other mods there are optional
//   public_http_port = 0               TCP port players reach the HTTP service
//                                      (mods) on, when a tunnel maps it elsewhere;
//                                      0 = the relay port + 1
//   identity_master_key = "<64 hex>"   check nicks: only a player who proves he
//                                      owns his gamertag on that master (its
//                                      /identity/v1/pubkey) gets in; empty = no check
//   identity_master = "host:port"      where players get that proof; empty = master
//   identity_names = "a.example:1234"  more names players reach this relay by
//                                      (comma-separated), besides public_address
//                                      and the machine's own IPs
//   identity_grace_sec = 20            seconds a new game session plays before its
//                                      gamertag must be proved (0..120; once per
//                                      session, 6 new sessions per IP a minute)
//
// Strings are UTF-8; control characters are dropped. Without server.toml (or
// without a name in it) the older relay_name.txt / relay_icon.png still work.
// Everything is re-read every few seconds, so changes show up without a
// restart. The name can also be set from the panel API:
//   /api/name?key=<token>&name=<text>   (rewrites relay_name.txt; a name in
//                                        server.toml takes precedence)
//
// Asked for over this same UDP port with message types the online client
// never uses (20..27, v5 header), so it works through any tunnel the game
// traffic already uses. Every query must be as large as its answer (no
// amplification), and each source IP gets a small per-second budget.
constexpr uint8_t kInfoQueryType = 20;
constexpr uint8_t kInfoType = 21;
constexpr uint8_t kIconQueryType = 22;
constexpr uint8_t kIconChunkType = 23;
constexpr uint32_t kIconChunkBytes = 1024;
constexpr size_t kIconMaxBytes = 64 * 1024;
constexpr int kIdentityQueriesPerSec = 120;

#pragma pack(push, 1)
// Must match larecomp relay_info.cpp.
struct InfoMsg {
  MsgHeader hdr;
  uint32_t token;
  uint16_t players;
  uint16_t sessions;
  uint32_t icon_size;
  uint32_t icon_crc;
  char name[48];
};
struct IconChunkMsg {
  MsgHeader hdr;
  uint32_t crc;
  uint32_t offset;
  uint32_t total;
  uint16_t len;
  uint8_t data[kIconChunkBytes];
};
#pragma pack(pop)

// The parts of server.toml besides the name and icon.
struct Identity {
  std::string description;
  std::string region;
  std::string tags;
  std::string public_host;   // empty = the announce's source IP
  uint16_t public_port = 0;  // 0 = the relay's own port
  std::string master_host;   // empty = do not announce
  uint16_t master_port = larecomp::browser::kMasterPort;
  std::string mods_dir = "mods";  // a plain folder name inside the exe's folder
  std::string mods_required;      // comma list as written; "*" = all
  uint16_t public_http_port = 0;  // 0 = the relay port + 1
  // Nick check (../protocol/identity_protocol.h). Off unless a key is set.
  bool id_enforce = false;
  uint8_t id_master_key[32] = {};
  std::string id_master_host;  // told to players in the challenge
  uint16_t id_master_port = 0;
  std::vector<std::string> id_names;  // CanonicalRelayName forms from the operator
  // How long a new ip+gamertag plays before it must be proved. The SDK's first
  // probe goes out before it knows the gamertag ("Player") and gives up after
  // ~3 s; the proof needs a round trip to the master first.
  int id_grace_sec = 20;
};

std::string g_dir;       // the exe's folder, with a trailing slash
uint16_t g_relay_port = 0;  // the UDP port bound (set before the first LoadIdentity)
std::mutex g_ident_mtx;  // guards the four below (HTTP thread writes the name)
std::string g_server_name;
Identity g_ident;
std::vector<uint8_t> g_icon;
uint32_t g_icon_crc = 0;
// Random per run and sent to the master alone: the master lists this relay
// only if the details it reads back from the announced address carry a proof
// made with it (browser::ListingProof).
uint8_t g_secret[16] = {};

// Wakes the mods thread before its next 5 s pass when LoadIdentity sees
// mods_dir or mods_required change.
std::mutex g_mods_wake_mtx;
std::condition_variable g_mods_wake;
bool g_mods_dirty = false;

uint32_t Crc32(const uint8_t* p, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i) {
    c ^= p[i];
    for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
  }
  return ~c;
}

// Valid UTF-8 without control characters (newlines kept only when asked),
// trimmed, at most max_bytes and never cut inside a character: the text ends
// up on other people's screens.
std::string CleanText(const std::string& in, size_t max_bytes, bool keep_newlines) {
  std::string out;
  const auto* p = reinterpret_cast<const unsigned char*>(in.data());
  const size_t n = in.size();
  for (size_t i = 0; i < n;) {
    const unsigned char c = p[i];
    const size_t len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
    bool ok = len != 0 && i + len <= n;
    for (size_t k = 1; ok && k < len; ++k) ok = (p[i + k] & 0xC0) == 0x80;
    if (ok && len == 2) ok = c >= 0xC2;  // overlong
    if (ok && len == 3)
      ok = !(c == 0xE0 && p[i + 1] < 0xA0) &&  // overlong
           !(c == 0xED && p[i + 1] >= 0xA0);   // surrogates
    if (ok && len == 4)
      ok = c <= 0xF4 && !(c == 0xF0 && p[i + 1] < 0x90) && !(c == 0xF4 && p[i + 1] >= 0x90);
    if (!ok) {
      ++i;
      continue;
    }
    if (len == 1) {
      if (c == '\r' || c == '\t') {
        if (out.size() + 1 > max_bytes) break;
        out += ' ';
        ++i;
        continue;
      }
      if (c == '\n' && !keep_newlines) break;
      if ((c < 0x20 && c != '\n') || c == 0x7F) {
        ++i;
        continue;
      }
    }
    if (out.size() + len > max_bytes) break;
    out.append(in, i, len);
    i += len;
  }
  while (!out.empty() && (out.back() == ' ' || out.back() == '\n')) out.pop_back();
  const size_t b = out.find_first_not_of(" \n");
  return b == std::string::npos ? std::string() : out.substr(b);
}

std::string CleanName(const std::string& in) { return CleanText(in, 47, false); }

// "host" or "host:port"; keeps `port` when none is given or it is invalid.
void SplitHostPort(const std::string& in, std::string& host, uint16_t& port) {
  host = in;
  const size_t colon = in.rfind(':');
  if (colon == std::string::npos) return;
  const int p = std::atoi(in.c_str() + colon + 1);
  host.resize(colon);
  if (p > 0 && p < 65536) port = static_cast<uint16_t>(p);
}

// The subset of TOML server.toml needs: one `key = value` per line, values in
// double quotes (with \" \\ \n \t escapes) or bare, # comments.
std::unordered_map<std::string, std::string> ReadToml(const std::string& path, bool* found) {
  std::unordered_map<std::string, std::string> kv;
  *found = false;
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return kv;
  *found = true;
  char line[2048];
  while (std::fgets(line, sizeof(line), f)) {
    std::string s = line;
    if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF &&
        static_cast<unsigned char>(s[1]) == 0xBB && static_cast<unsigned char>(s[2]) == 0xBF)
      s.erase(0, 3);  // BOM (Notepad)
    const size_t eq = s.find('=');
    const size_t start = s.find_first_not_of(" \t");
    if (eq == std::string::npos || start == std::string::npos || s[start] == '#' || start >= eq)
      continue;
    std::string key = s.substr(start, eq - start);
    while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
    const size_t v = s.find_first_not_of(" \t", eq + 1);
    if (v == std::string::npos) continue;
    std::string value;
    if (s[v] == '"') {
      for (size_t i = v + 1; i < s.size() && s[i] != '"'; ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
          const char e = s[++i];
          value += e == 'n' ? '\n' : e == 't' ? '\t' : e;
        } else {
          value += s[i];
        }
      }
    } else {
      const size_t end = s.find_first_of("#\r\n", v);
      value = s.substr(v, end == std::string::npos ? std::string::npos : end - v);
      while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.pop_back();
    }
    kv[key] = value;
  }
  std::fclose(f);
  return kv;
}

// A PNG with its IHDR inside the size limits, or nothing.
bool IconLooksValid(const std::vector<uint8_t>& d) {
  static const uint8_t kSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  if (d.size() < 33 || d.size() > kIconMaxBytes || std::memcmp(d.data(), kSig, 8) != 0)
    return false;
  if (std::memcmp(d.data() + 12, "IHDR", 4) != 0) return false;
  auto be32 = [&](size_t o) {
    return (uint32_t(d[o]) << 24) | (uint32_t(d[o + 1]) << 16) | (uint32_t(d[o + 2]) << 8) |
           uint32_t(d[o + 3]);
  };
  const uint32_t w = be32(16), h = be32(20);
  return w > 0 && h > 0 && w <= 512 && h <= 512;
}

void LoadIdentity(bool log) {
  bool toml_found = false;
  const auto toml = ReadToml(g_dir + "server.toml", &toml_found);
  auto get = [&](const char* k) {
    auto it = toml.find(k);
    return it == toml.end() ? std::string() : it->second;
  };

  std::string name = CleanName(get("name"));
  if (name.empty()) {
    if (FILE* f = std::fopen((g_dir + "relay_name.txt").c_str(), "rb")) {
      char line[256] = {};
      if (std::fgets(line, sizeof(line), f)) name = CleanName(line);
      std::fclose(f);
    }
  }

  Identity id;
  id.description = CleanText(get("description"), 255, true);
  id.region = CleanText(get("region"), 23, false);
  id.tags = CleanText(get("tags"), 95, false);
  SplitHostPort(CleanText(get("public_address"), 127, false), id.public_host, id.public_port);
  if (id.public_host.size() > 63) id.public_host.clear();  // does not fit the announce
  SplitHostPort(CleanText(get("master"), 127, false), id.master_host, id.master_port);
  // Same rule as the icon: a plain name inside the exe's folder, so the HTTP
  // service never serves anything outside it.
  id.mods_dir = CleanText(get("mods_dir"), 127, false);
  if (id.mods_dir.empty() || id.mods_dir.find_first_of("/\\:") != std::string::npos ||
      id.mods_dir.find("..") != std::string::npos)
    id.mods_dir = "mods";
  id.mods_required = CleanText(get("mods_required"), 1023, false);
  const int http_port = std::atoi(get("public_http_port").c_str());
  id.public_http_port = http_port > 0 && http_port < 65536 ? static_cast<uint16_t>(http_port) : 0;
  {
    namespace ident = larecomp::identity;
    const std::string key = CleanText(get("identity_master_key"), 64, false);
    id.id_enforce = !key.empty() && ident::FromHex(key, id.id_master_key, 32);
    if (!key.empty() && !id.id_enforce && log)
      LogInfo("WARNING identity_master_key is not 64 hex digits: nicks are NOT checked\n");
    id.id_master_port = larecomp::browser::kMasterPort;
    SplitHostPort(CleanText(get("identity_master"), 127, false), id.id_master_host, id.id_master_port);
    if (id.id_master_host.empty()) {
      id.id_master_host = id.master_host;
      id.id_master_port = id.master_port;
    }
    if (id.id_master_host.size() > 63) id.id_master_host.clear();
    const std::string names = CleanText(get("identity_names"), 1023, false);
    for (size_t pos = 0; pos <= names.size();) {
      size_t comma = names.find(',', pos);
      if (comma == std::string::npos) comma = names.size();
      std::string one = names.substr(pos, comma - pos);
      while (!one.empty() && one.front() == ' ') one.erase(one.begin());
      if (!one.empty()) {
        std::string host;
        uint16_t p = g_relay_port;
        SplitHostPort(one, host, p);
        id.id_names.push_back(ident::CanonicalRelayName(host, p));
      }
      pos = comma + 1;
    }
    if (!id.public_host.empty())
      id.id_names.push_back(ident::CanonicalRelayName(
          id.public_host, id.public_port ? id.public_port : g_relay_port));
    const std::string grace = get("identity_grace_sec");
    if (!grace.empty()) id.id_grace_sec = (std::max)(0, (std::min)(120, std::atoi(grace.c_str())));
  }

  // The icon file name stays inside the exe's folder.
  std::string icon_file = get("icon");
  if (icon_file.empty() || icon_file.find_first_of("/\\:") != std::string::npos ||
      icon_file.find("..") != std::string::npos)
    icon_file = "relay_icon.png";
  std::vector<uint8_t> icon;
  bool icon_rejected = false;
  if (FILE* f = std::fopen((g_dir + icon_file).c_str(), "rb")) {
    uint8_t buf[4096];
    size_t got;
    while ((got = std::fread(buf, 1, sizeof(buf), f)) > 0 && icon.size() <= kIconMaxBytes)
      icon.insert(icon.end(), buf, buf + got);
    std::fclose(f);
    if (!IconLooksValid(icon)) {
      icon_rejected = !icon.empty();
      icon.clear();
    }
  }
  const uint32_t crc = icon.empty() ? 0 : Crc32(icon.data(), icon.size());

  std::lock_guard<std::mutex> lk(g_ident_mtx);
  const bool changed = name != g_server_name || crc != g_icon_crc ||
                       id.description != g_ident.description || id.region != g_ident.region ||
                       id.tags != g_ident.tags || id.public_host != g_ident.public_host ||
                       id.public_port != g_ident.public_port || id.master_host != g_ident.master_host ||
                       id.master_port != g_ident.master_port || id.id_enforce != g_ident.id_enforce ||
                       std::memcmp(id.id_master_key, g_ident.id_master_key, 32) != 0 ||
                       id.id_master_host != g_ident.id_master_host || id.id_names != g_ident.id_names;
  const bool mods_changed =
      id.mods_dir != g_ident.mods_dir || id.mods_required != g_ident.mods_required;
  if (mods_changed) {
    std::lock_guard<std::mutex> wk(g_mods_wake_mtx);
    g_mods_dirty = true;
    g_mods_wake.notify_one();
  }
  g_server_name = name;
  g_ident = id;
  g_icon = std::move(icon);
  g_icon_crc = crc;
  if (log || changed) {
    const std::string icon_text =
        g_icon.empty() ? (icon_rejected ? icon_file + " REJECTED (PNG, <=64 KB, <=512x512)"
                                        : "(none; " + icon_file + ")")
                       : std::to_string(g_icon.size()) + " bytes";
    const std::string listing =
        g_ident.master_host.empty()
            ? std::string("unlisted (no master in server.toml)")
            : "listed on " + g_ident.master_host + ":" + std::to_string(g_ident.master_port);
    LogInfo("server name: %s | icon: %s\n",
            g_server_name.empty() ? "(none; server.toml)" : g_server_name.c_str(), icon_text.c_str());
    LogInfo("server browser: region \"%s\" | tags \"%s\" | %zu-byte description | %s%s\n",
            g_ident.region.c_str(), g_ident.tags.c_str(), g_ident.description.size(),
            listing.c_str(), toml_found ? "" : " | server.toml not found");
    if (g_ident.id_enforce) {
      std::string names;
      for (const auto& n : g_ident.id_names) names += (names.empty() ? "" : ", ") + n;
      LogInfo("nick check: ON (master key %s, proofs from %s:%u) | names: %s + this machine's IPs\n",
              larecomp::identity::Fingerprint(g_ident.id_master_key).c_str(),
              g_ident.id_master_host.empty() ? "(none!)" : g_ident.id_master_host.c_str(),
              g_ident.id_master_port, names.empty() ? "(none)" : names.c_str());
    } else {
      LogInfo("nick check: off (no identity_master_key in server.toml)\n");
    }
  }
}

bool SaveName(const std::string& raw) {
  const std::string name = CleanName(raw);
  FILE* f = std::fopen((g_dir + "relay_name.txt").c_str(), "wb");
  if (!f) return false;
  std::fprintf(f, "%s\n", name.c_str());
  std::fclose(f);
  LoadIdentity(false);
  return true;
}

// %XX and '+' decoding for the name parameter. In a URL path '+' is a plain
// character (a mod file may be named "a+b.bin"), so paths pass plus_is_space
// false.
std::string UrlDecode(const std::string& s, bool plus_is_space = true) {
  std::string o;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '+' && plus_is_space) {
      o += ' ';
    } else if (s[i] == '%' && i + 2 < s.size()) {
      o += static_cast<char>(std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16));
      i += 2;
    } else {
      o += s[i];
    }
  }
  return o;
}

// ---------------------------------------------------------------------------
// Mods the relay plays with (mods_dir next to the exe, one sub-folder per
// mod), described by a manifest and served over HTTP so players can fetch
// what they miss (browser_protocol.h has the format). Only files listed in
// the manifest are ever served: the HTTP side looks names up in the map built
// here and never opens a path it was sent.
//
// A dedicated thread rebuilds the manifest every 5 s (sooner when server.toml
// changes the mods keys) and publishes it as an immutable snapshot under
// g_mods_mtx. Hashing a big mod takes seconds, so neither the relay loop nor
// the HTTP accept loop ever waits for it; files whose size and write time did
// not change keep their hash from the previous pass.
// ---------------------------------------------------------------------------
namespace fs = std::filesystem;

constexpr size_t kModsMax = 64;
constexpr size_t kModFilesMax = 20000;
constexpr uint64_t kModBytesMax = 4ull << 30;
constexpr size_t kModHashChunk = 1 << 20;
constexpr int kDownloadsMax = 16;  // concurrent /mods/file threads
constexpr size_t kDownloadChunk = 256 * 1024;

struct ModFile {
  fs::path abs;
  uint64_t size = 0;
};

struct ModsState {
  std::string manifest;                            // empty = no mods
  std::unordered_map<std::string, ModFile> files;  // "<mod>/<relative path>"
  int required = 0, optional = 0;
  size_t file_count = 0;
  uint64_t bytes = 0;
  uint32_t crc = 0;  // Crc32 of the manifest; 0 = no mods
};

std::mutex g_mods_mtx;  // guards the pointer; a published state never changes
std::shared_ptr<const ModsState> g_mods = std::make_shared<const ModsState>();
std::atomic<bool> g_http_bound{false};  // the HTTP service is listening
std::atomic<int> g_downloads{0};        // /mods/file threads running

std::shared_ptr<const ModsState> ModsSnapshot() {
  std::lock_guard<std::mutex> lk(g_mods_mtx);
  return g_mods;
}

// UTF-8 <-> path, the same under C++17 (standalone build) and C++20+ (CMake),
// where u8string became std::u8string.
fs::path PathFromUtf8(const std::string& s) {
#if defined(__cpp_char8_t)
  return fs::path(std::u8string(s.begin(), s.end()));
#else
  return fs::u8path(s);
#endif
}

std::string Utf8Of(const fs::path& p) {
  const auto s = p.generic_u8string();
  return std::string(s.begin(), s.end());
}

bool ModNameOk(const std::string& n) {
  if (n.empty() || n[0] == '.') return false;
  for (const char c : n)
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
          c == '.' || c == '-'))
      return false;
  return true;
}

// Programs and scripts are never handed to players, whatever the operator
// dropped in the folder.
bool BlockedModExtension(const fs::path& p) {
  std::string e = Utf8Of(p.extension());
  for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  static const char* const kBlocked[] = {".exe", ".dll", ".bat", ".cmd", ".com", ".ps1", ".vbs",
                                         ".js",  ".msi", ".scr", ".lnk", ".sh"};
  for (const char* b : kBlocked)
    if (e == b) return true;
  return false;
}

bool HashModFile(const fs::path& p, uint64_t* out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  std::vector<char> buf(kModHashChunk);
  larecomp::browser::Fnv1a64 h;
  while (f) {
    f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
    const std::streamsize got = f.gcount();
    if (got > 0) h.Add(buf.data(), static_cast<size_t>(got));
  }
  if (f.bad()) return false;
  *out = h.h;
  return true;
}

struct ModHashEntry {
  uint64_t size = 0;
  int64_t mtime = 0;
  uint64_t hash = 0;
  bool seen = false;
};
using ModHashCache = std::map<fs::path, ModHashEntry>;

std::string HumanBytes(uint64_t b) {
  char s[32];
  if (b >= (1ull << 20))
    std::snprintf(s, sizeof(s), "%llu MB", static_cast<unsigned long long>((b + (1ull << 19)) >> 20));
  else
    std::snprintf(s, sizeof(s), "%llu KB", static_cast<unsigned long long>((b + 1023) >> 10));
  return s;
}

// Scans root and builds the manifest. Anything worth telling the operator
// goes to `notes` (logged by the caller only when they change, so a bad file
// does not repeat in the log every 5 s).
std::shared_ptr<ModsState> BuildMods(const fs::path& root, const std::string& required_list,
                                     ModHashCache& cache, std::vector<std::string>& notes) {
  auto st = std::make_shared<ModsState>();
  const std::string root_name = Utf8Of(root.filename());

  // "*" = all; otherwise names, comma-separated.
  bool all_required = false;
  std::unordered_set<std::string> required;
  {
    size_t i = 0;
    while (i <= required_list.size()) {
      size_t comma = required_list.find(',', i);
      if (comma == std::string::npos) comma = required_list.size();
      std::string n = required_list.substr(i, comma - i);
      const size_t b = n.find_first_not_of(" \t");
      const size_t e = n.find_last_not_of(" \t");
      n = b == std::string::npos ? std::string() : n.substr(b, e - b + 1);
      if (n == "*")
        all_required = true;
      else if (!n.empty())
        required.insert(n);
      i = comma + 1;
    }
  }

  std::error_code ec;
  if (!fs::is_directory(root, ec)) {
    for (const auto& n : required)
      notes.push_back("required mod \"" + n + "\" listed, but " + root_name + "/ does not exist");
    cache.clear();
    return st;
  }

  std::vector<std::string> mods;
  for (fs::directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
       !ec && it != end; it.increment(ec)) {
    std::error_code e2;
    if (it->is_symlink(e2) || !it->is_directory(e2)) continue;
    const std::string n = Utf8Of(it->path().filename());
    if (n.empty() || n[0] == '.') continue;
    if (!ModNameOk(n)) {
      notes.push_back("skipped folder \"" + n + "\": a mod name is letters, digits, _ . - only");
      continue;
    }
    mods.push_back(n);
  }
  std::sort(mods.begin(), mods.end());

  struct Found {
    std::string rel;
    fs::path abs;
    uint64_t size;
    int64_t mtime;
  };
  std::unordered_set<std::string> included;
  for (size_t m = 0; m < mods.size(); ++m) {
    const std::string& mod = mods[m];
    if (included.size() >= kModsMax) {
      notes.push_back("more than " + std::to_string(kModsMax) + " mods: skipped \"" + mod +
                      "\" and the rest");
      break;
    }
    const fs::path mod_root = root / PathFromUtf8(mod);
    std::vector<Found> found;
    uint64_t mod_bytes = 0;
    std::error_code wec;
    fs::recursive_directory_iterator it(mod_root, fs::directory_options::skip_permission_denied, wec);
    for (const fs::recursive_directory_iterator end; !wec && it != end; it.increment(wec)) {
      std::error_code e2;
      const fs::directory_entry& de = *it;
      const std::string name = Utf8Of(de.path().filename());
      const bool is_dir = de.is_directory(e2);
      // Hidden ('.' names: .git, .cache, ...) and links (could point outside
      // the folder) are neither listed nor walked into.
      if (name.empty() || name[0] == '.' || de.is_symlink(e2)) {
        if (is_dir) it.disable_recursion_pending();
        continue;
      }
      if (is_dir || !de.is_regular_file(e2)) continue;
      const std::string rel = Utf8Of(de.path().lexically_relative(mod_root));
      bool rel_ok = !rel.empty() && rel[0] != '/' && rel.find("..") == std::string::npos;
      for (const char c : rel)
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F || c == '\\' || c == ':') rel_ok = false;
      if (!rel_ok) {
        notes.push_back("skipped " + root_name + "/" + mod + "/" + rel + ": name not allowed in the manifest");
        continue;
      }
      if (BlockedModExtension(de.path())) {
        notes.push_back("skipped " + root_name + "/" + mod + "/" + rel + ": programs and scripts are never served");
        continue;
      }
      const uint64_t size = de.file_size(e2);
      if (e2) continue;
      const int64_t mtime = static_cast<int64_t>(de.last_write_time(e2).time_since_epoch().count());
      if (e2) continue;
      found.push_back({rel, de.path(), size, mtime});
      mod_bytes += size;
    }
    if (wec) notes.push_back("could not read all of " + root_name + "/" + mod + " (" + wec.message() + ")");

    // A mod goes in whole or not at all: half a mod is worse than none.
    if (st->file_count + found.size() > kModFilesMax || st->bytes + mod_bytes > kModBytesMax) {
      notes.push_back("limit of " + std::to_string(kModFilesMax) + " files / 4 GB reached: skipped \"" +
                      mod + "\" and the rest");
      break;
    }
    std::sort(found.begin(), found.end(), [](const Found& a, const Found& b) { return a.rel < b.rel; });

    std::string lines;
    size_t count = 0;
    uint64_t bytes = 0;
    for (const Found& f : found) {
      ModHashEntry& ce = cache[f.abs];
      if (ce.size != f.size || ce.mtime != f.mtime || ce.hash == 0) {
        uint64_t h = 0;
        if (!HashModFile(f.abs, &h)) {
          notes.push_back("could not read " + root_name + "/" + mod + "/" + f.rel + "; skipped");
          cache.erase(f.abs);
          continue;
        }
        ce.size = f.size;
        ce.mtime = f.mtime;
        ce.hash = h;
      }
      ce.seen = true;
      char hex[17];
      std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(ce.hash));
      lines += "file\t" + mod + "\t" + f.rel + "\t" + std::to_string(f.size) + "\t" + hex + "\n";
      st->files[mod + "/" + f.rel] = ModFile{f.abs, f.size};
      ++count;
      bytes += f.size;
    }
    const bool req = all_required || required.count(mod) != 0;
    st->manifest += "mod\t" + mod + "\t" + (req ? "1" : "0") + "\t" + std::to_string(count) + "\t" +
                    std::to_string(bytes) + "\n" + lines;
    (req ? st->required : st->optional) += 1;
    st->file_count += count;
    st->bytes += bytes;
    included.insert(mod);
  }
  for (const auto& n : required)
    if (!included.count(n))
      notes.push_back("required mod \"" + n + "\" is not in " + root_name + "/");

  // Forget files that are gone, so the cache does not grow forever.
  for (auto it = cache.begin(); it != cache.end();) {
    if (!it->second.seen) {
      it = cache.erase(it);
    } else {
      it->second.seen = false;
      ++it;
    }
  }
  st->crc = st->manifest.empty()
                ? 0
                : Crc32(reinterpret_cast<const uint8_t*>(st->manifest.data()), st->manifest.size());
  return st;
}

// One rebuild pass; publishes the new state only when the manifest changed.
// Called once at boot (before any thread serves it) and then only from
// ModsLoop, which owns the hash cache.
void RefreshMods(bool boot) {
  static ModHashCache cache;
  static std::vector<std::string> last_notes;
  std::string dir, required;
  {
    std::lock_guard<std::mutex> lk(g_ident_mtx);
    dir = g_ident.mods_dir;
    required = g_ident.mods_required;
  }
  std::vector<std::string> notes;
  std::shared_ptr<ModsState> st;
  try {
    st = BuildMods(fs::path(g_dir) / PathFromUtf8(dir), required, cache, notes);
  } catch (const std::exception& e) {
    // Keep serving the last good manifest.
    notes.push_back(std::string("scan failed: ") + e.what());
  }
  if (notes != last_notes) {
    for (const auto& n : notes) LogInfo("mods: %s\n", n.c_str());
    last_notes = notes;
  }
  if (!st) return;
  bool changed;
  {
    std::lock_guard<std::mutex> lk(g_mods_mtx);
    changed = st->manifest != g_mods->manifest;
    if (changed) g_mods = st;
  }
  if (changed || boot) {
    if (st->manifest.empty())
      LogInfo("mods: none (%s/ next to the exe)\n", dir.c_str());
    else
      LogInfo("mods: %d required, %d optional, %zu files, %s (crc %08x)\n", st->required, st->optional,
              st->file_count, HumanBytes(st->bytes).c_str(), st->crc);
  }
}

void ModsLoop() {
  for (;;) {
    {
      std::unique_lock<std::mutex> lk(g_mods_wake_mtx);
      g_mods_wake.wait_for(lk, std::chrono::seconds(5), [] { return g_mods_dirty; });
      g_mods_dirty = false;
    }
    RefreshMods(false);
  }
}

// send() until every byte is out: a big reply goes out in pieces.
bool SendAll(socket_t c, const char* p, size_t n) {
#ifdef MSG_NOSIGNAL
  const int flags = MSG_NOSIGNAL;  // a client that hung up must not kill the relay (SIGPIPE)
#else
  const int flags = 0;
#endif
  while (n > 0) {
    const int chunk = n > (1u << 30) ? (1 << 30) : static_cast<int>(n);
    const int s = send(c, p, chunk, flags);
    if (s <= 0) return false;
    p += s;
    n -= static_cast<size_t>(s);
  }
  return true;
}

void SendHttpHead(socket_t c, const char* status, const char* ctype, uint64_t length) {
  std::string h = "HTTP/1.1 ";
  h += status;
  h += "\r\nContent-Type: ";
  h += ctype;
  h += "\r\nContent-Length: " + std::to_string(length);
  h += "\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n";
  SendAll(c, h.data(), h.size());
}

void SendHttpReply(socket_t c, const char* status, const char* ctype, const std::string& body) {
  SendHttpHead(c, status, ctype, body.size());
  if (!body.empty()) SendAll(c, body.data(), body.size());
}

// A stalled client must not hold a thread (or the accept loop) forever.
void SetSocketTimeouts(socket_t c, int recv_ms, int send_ms) {
#ifdef _WIN32
  DWORD r = static_cast<DWORD>(recv_ms), s = static_cast<DWORD>(send_ms);
  setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&r), sizeof(r));
  setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&s), sizeof(s));
#else
  timeval r{recv_ms / 1000, (recv_ms % 1000) * 1000}, s{send_ms / 1000, (send_ms % 1000) * 1000};
  setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &r, sizeof(r));
  setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &s, sizeof(s));
#endif
}

// One /mods/file download, on its own thread: it owns the socket and the
// download slot the accept loop took for it.
void ServeModFile(socket_t c, ModFile f) {
  std::ifstream in(f.abs, std::ios::binary);
  std::error_code ec;
  const uint64_t size_now = in ? fs::file_size(f.abs, ec) : 0;
  if (!in || ec) {
    SendHttpReply(c, "404 Not Found", "text/plain", "not found");
  } else if (size_now != f.size) {
    // Changed since the manifest was built; the next pass lists it again.
    SendHttpReply(c, "503 Service Unavailable", "text/plain", "file changed; fetch the manifest again");
  } else {
    SendHttpHead(c, "200 OK", "application/octet-stream", f.size);
    std::vector<char> buf(kDownloadChunk);
    uint64_t left = f.size;
    while (left > 0) {
      const size_t want = left < buf.size() ? static_cast<size_t>(left) : buf.size();
      in.read(buf.data(), static_cast<std::streamsize>(want));
      const std::streamsize got = in.gcount();
      // A short file ends the body early; the client sees fewer bytes than
      // Content-Length and the hash check fails, so it retries.
      if (got <= 0 || !SendAll(c, buf.data(), static_cast<size_t>(got))) break;
      left -= static_cast<uint64_t>(got);
    }
  }
  LARECOMP_CLOSESOCK(c);
  --g_downloads;
}

std::string JsonEscape(const char* s) {
  std::string o;
  for (const char* p = s; *p; ++p) {
    unsigned char c = static_cast<unsigned char>(*p);
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (c < 0x20) {
          char b[8];
          std::snprintf(b, sizeof(b), "\\u%04x", c);
          o += b;
        } else {
          o += static_cast<char>(c);
        }
    }
  }
  return o;
}

std::string QueryParam(const std::string& q, const std::string& key) {
  size_t i = 0;
  while (i < q.size()) {
    size_t amp = q.find('&', i);
    std::string kv = q.substr(i, amp == std::string::npos ? std::string::npos : amp - i);
    size_t eq = kv.find('=');
    if (eq != std::string::npos && kv.substr(0, eq) == key) return kv.substr(eq + 1);
    if (amp == std::string::npos) break;
    i = amp + 1;
  }
  return "";
}

// Self-contained dashboard. Reads ?key= from the URL and passes it to every API
// call. Polls /api/state ~1.5 s. No external assets (CSP-safe, works offline).
const char* kDashboardHtml = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>MCLA Server</title><style>
:root{--bg:#0d1117;--card:#161b22;--bd:#30363d;--tx:#e6edf3;--mut:#8b949e;--acc:#2f81f7;--ok:#3fb950;--warn:#d29922;--bad:#f85149;--host:#a371f7}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--tx);font:14px/1.4 system-ui,Segoe UI,Roboto,sans-serif}
header{padding:14px 18px;border-bottom:1px solid var(--bd);display:flex;gap:20px;align-items:center;flex-wrap:wrap;background:var(--card)}
h1{font-size:16px;margin:0;font-weight:600}.stat{font-size:12px;color:var(--mut)}.stat b{color:var(--tx);font-size:15px;display:block}
main{padding:18px;max-width:1200px;margin:0 auto;display:grid;gap:18px}
.card{background:var(--card);border:1px solid var(--bd);border-radius:8px;padding:14px}
.card h2{font-size:13px;text-transform:uppercase;letter-spacing:.5px;color:var(--mut);margin:0 0 12px}
table{width:100%;border-collapse:collapse;font-size:13px}th,td{text-align:left;padding:7px 9px;border-bottom:1px solid var(--bd);white-space:nowrap}
th{color:var(--mut);font-weight:500;font-size:11px;text-transform:uppercase}tr:last-child td{border-bottom:0}
.tag{color:var(--host);font-weight:600}.mono{font-family:ui-monospace,Consolas,monospace;color:var(--mut)}
.pill{display:inline-block;padding:1px 7px;border-radius:10px;font-size:11px;font-weight:600}
.pill.host{background:rgba(163,113,247,.15);color:var(--host)}.pill.ban{background:rgba(248,81,73,.15);color:var(--bad)}
.pill.idle{background:rgba(139,148,158,.18);color:var(--mut)}
.rtt{font-weight:600}.g{color:var(--ok)}.y{color:var(--warn)}.r{color:var(--bad)}
button{background:#21262d;color:var(--tx);border:1px solid var(--bd);border-radius:6px;padding:4px 9px;font-size:12px;cursor:pointer}
button:hover{border-color:var(--acc)}button.bad:hover{border-color:var(--bad);color:var(--bad)}
.sess{border:1px solid var(--bd);border-radius:8px;padding:12px;margin-bottom:10px}
.sess .hd{display:flex;justify-content:space-between;align-items:center;margin-bottom:8px}.sess .nm{font-weight:600}
.mem{display:inline-flex;gap:6px;align-items:center;background:#21262d;border-radius:6px;padding:3px 8px;margin:3px 4px 0 0;font-size:12px}
svg{width:100%;height:340px;background:#0a0e14;border-radius:6px}
.err{color:var(--bad);padding:20px;text-align:center}.empty{color:var(--mut);padding:16px;text-align:center}
</style></head><body>
<header><h1>MCLA <span style="color:var(--acc)">Server</span></h1>
<div class=stat>Uptime<b id=up>-</b></div><div class=stat>Peers<b id=np>-</b></div>
<div class=stat>Sessions<b id=nh>-</b></div><div class=stat>Relayed<b id=rl>-</b></div>
<div class=stat>Send fails<b id=sf>-</b></div><div class=stat>Dropped<b id=dr>-</b></div><div class=stat>RX pkts<b id=rx>-</b></div>
<div class=stat style="margin-left:auto" id=status>connecting…</div></header>
<main>
<div class=card><h2>Sessions &amp; players</h2><div id=sessions></div><div class=stat id=idle style="margin-top:8px"></div></div>
<div class=card><h2>Topology (observed game traffic)</h2><svg id=topo viewBox="0 0 1000 340"></svg>
<div class=stat style="margin-top:6px">Nodes = peers on a ring. A <span class=g>green line</span> = one peer actually sent game data to another in the last 5&nbsp;s (discovery excluded). Every line into one node (<span class=tag>◆</span> host) and none between guests = <b>star</b> (the visibility bug). Lines between everyone = healthy <b>mesh</b>. A session box = players trading game data right now; it splits by itself ~20&nbsp;s after they stop.</div></div>
<div class=card><h2>All peers</h2><div id=peers></div></div>
</main>
<script>
const KEY=new URLSearchParams(location.search).get('key')||'';
const api=p=>fetch(p+(p.includes('?')?'&':'?')+'key='+encodeURIComponent(KEY));
const dur=ms=>{let s=ms/1000|0;if(s<60)return s+'s';let m=s/60|0;if(m<60)return m+'m'+(s%60)+'s';return (m/60|0)+'h'+(m%60)+'m'};
const num=n=>n>=1e6?(n/1e6).toFixed(1)+'M':n>=1e3?(n/1e3).toFixed(1)+'k':(''+n);
const rttc=r=>r<0?'':r<80?'g':r<160?'y':'r';
const esc=s=>(s+'').replace(/[&<>]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;'}[c]));
const short=v=>(v||'').split('.').slice(2).join('.');
async function act(kind,ip){if(kind!=='unban'&&!confirm(kind+' '+ip+'?'))return;const r=await api('/api/'+kind+'?ip='+ip);if(!r.ok)alert('failed: '+r.status);setTimeout(load,300)}
function load(){api('/api/state').then(r=>{if(!r.ok)throw r.status;return r.json()}).then(render).catch(e=>{
  document.getElementById('status').innerHTML='<span class=r>offline / bad token ('+e+')</span>'})}
function render(d){
 document.getElementById('status').innerHTML='<span class=g>● live</span>';
 up.textContent=dur(d.uptime_ms);np.textContent=d.peers;
 nh.textContent=d.hosts_active+(d.hosts>d.hosts_active?' (+'+(d.hosts-d.hosts_active)+' idle)':'');
 rl.textContent=num(d.relayed);sf.textContent=d.sendfail;dr.textContent=num((d.drop_src||0)+(d.drop_dst||0));rx.textContent=num(d.rx);
 const L=d.list||[],banned=new Set(d.banned||[]);
 // sessions = rooms: peers trading game data with each other right now
 const rooms={};L.forEach(p=>{(rooms[p.room]=rooms[p.room]||[]).push(p)});
 const S=document.getElementById('sessions');S.innerHTML='';let shown=0;
 Object.keys(rooms).forEach(r=>{const mem=rooms[r];const live=mem.filter(p=>p.host_state==='active');
  if(mem.length<2&&!live.length)return;shown++;
  const h=live[0]||mem.find(p=>p.is_host)||mem[0];
  const div=document.createElement('div');div.className='sess';
  div.innerHTML='<div class=hd><span class=nm>'+esc(h.is_host?(h.name||h.gamertag):h.gamertag)+(live.length?' <span class="pill host">HOST</span>':'')
   +(live.length>1?' <span class="pill ban">'+live.length+' hosts in one room</span>':'')+'</span>'
   +'<span class=mono>'+(h.is_host?esc(h.gamemode)+' / map '+esc(h.map)+' · ':'')+mem.length+'/'+(h.max||'?')+' players</span></div>'
   +mem.map(p=>'<span class=mem>'+(p.host_state==='active'?'<span class=tag>◆</span> ':'')+esc(p.gamertag)+' <span class=mono>'+short(p.vip)+'</span> '
     +(p.rtt>=0?'<span class="rtt '+rttc(p.rtt)+'">'+p.rtt+'ms</span>':'')+'</span>').join('');
  S.appendChild(div)});
 if(!shown)S.innerHTML='<div class=empty>no active sessions</div>';
 const idle=L.filter(p=>p.host_state==='idle');
 document.getElementById('idle').innerHTML=idle.length?'Advertised as host but idle (session ended, or nobody searched in 90&nbsp;s): '
   +idle.map(p=>esc(p.gamertag)+' <span class=mono>'+short(p.vip)+'</span>').join(', '):'';
 // peers table
 const P=document.getElementById('peers');
 if(!L.length){P.innerHTML='<div class=empty>no peers connected</div>'}else{
 P.innerHTML='<table><tr><th>Gamertag</th><th>XUID</th><th>VIP</th><th>IP:Port</th><th>Room</th><th>RTT</th><th>Conn</th><th>RX</th><th>TX</th><th></th></tr>'
  +L.map(p=>{const b=banned.has(p.ip);return '<tr><td>'+(p.host_state==='active'?'<span class=tag>◆</span> ':'')+esc(p.gamertag)
   +(p.host_state==='idle'?' <span class="pill idle">idle host</span>':'')
   +(b?' <span class="pill ban">BANNED</span>':'')+'</td><td class=mono>'+p.xuid+'</td><td class=mono>'+p.vip
   +'</td><td class=mono>'+esc(p.ip)+':'+p.port+'</td><td class=mono>'+short(p.room)+'</td>'
   +'<td class="rtt '+rttc(p.rtt)+'">'+(p.rtt>=0?p.rtt+'ms':'-')+'</td><td class=mono>'+dur(p.conn_ms)+'</td>'
   +'<td class=mono>'+num(p.rx_pkts)+'</td><td class=mono>'+num(p.tx_pkts)+'</td>'
   +'<td><button onclick="act(\'kick\',\''+p.ip+'\')">Kick</button> '
   +(b?'<button onclick="act(\'unban\',\''+p.ip+'\')">Unban</button>':'<button class=bad onclick="act(\'ban\',\''+p.ip+'\')">Ban</button>')
   +'</td></tr>'}).join('')+'</table>'}
 drawTopo(L,d.edges||[]);
}
function drawTopo(L,edges){const svg=document.getElementById('topo');svg.innerHTML='';
 if(!L.length)return;
 const ns='http://www.w3.org/2000/svg';
 const idx={};L.forEach((p,i)=>idx[p.vip]=i);
 const cx=500,cy=170,rx=440,ry=132;
 const pos=L.map((p,i)=>{const a=2*Math.PI*i/L.length-Math.PI/2;
   return{p,x:cx+Math.cos(a)*(L.length>1?rx:0),y:cy+Math.sin(a)*(L.length>1?ry:0)}});
 // directed edges (observed game flows). Draw as lines, green when both ways.
 const seen={};
 edges.forEach(e=>{const a=idx[e.f],b=idx[e.t];if(a==null||b==null||a===b)return;
   const bidir=edges.some(x=>x.f===e.t&&x.t===e.f);const key=[e.f,e.t].sort().join('-');
   if(bidir&&seen[key])return;seen[key]=1;
   const l=document.createElementNS(ns,'line');l.setAttribute('x1',pos[a].x);l.setAttribute('y1',pos[a].y);
   l.setAttribute('x2',pos[b].x);l.setAttribute('y2',pos[b].y);
   l.setAttribute('stroke',bidir?'#3fb950':'#d29922');l.setAttribute('stroke-opacity','.6');l.setAttribute('stroke-width','2');svg.appendChild(l)});
 pos.forEach(o=>{const host=o.p.host_state==='active'||o.p.host_state==='idle';const c=document.createElementNS(ns,host?'rect':'circle');
   if(host){c.setAttribute('x',o.x-9);c.setAttribute('y',o.y-9);c.setAttribute('width',18);c.setAttribute('height',18);c.setAttribute('transform','rotate(45 '+o.x+' '+o.y+')');c.setAttribute('fill',o.p.host_state==='active'?'#a371f7':'#6e7681')}
   else{c.setAttribute('cx',o.x);c.setAttribute('cy',o.y);c.setAttribute('r',9);c.setAttribute('fill','#2f81f7')}
   c.setAttribute('stroke','#0a0e14');c.setAttribute('stroke-width','2');svg.appendChild(c);
   const t=document.createElementNS(ns,'text');t.setAttribute('x',o.x);t.setAttribute('y',o.y-15);t.setAttribute('fill','#c9d1d9');
   t.setAttribute('font-size','11');t.setAttribute('text-anchor','middle');t.textContent=o.p.gamertag+' ('+short(o.p.vip)+')';svg.appendChild(t)})
}
load();setInterval(load,1500);
</script></body></html>)HTML";

// Minimal HTTP/1.1 server for the dashboard. One thread; short-lived, one
// request per connection (Connection: close). Everything is token-gated but
// the mods (GET /mods/manifest, GET /mods/file/<mod>/<path>), which every
// player may fetch; a file download runs on its own thread so a slow one
// never holds up the dashboard.
// Listens on all interfaces, unless the relay itself was bound to loopback
// (a local-only relay keeps its dashboard local too).
void HttpServer(uint16_t port, uint32_t relay_bind_net) {
  socket_t ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (ls == INVALID_SOCKET) return;
  int yes = 1;
  setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  const bool loopback = (ntohl(relay_bind_net) >> 24) == 127;
  a.sin_addr.s_addr = loopback ? relay_bind_net : htonl(INADDR_ANY);
  if (bind(ls, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == SOCKET_ERROR) {
    LogInfo("admin dashboard bind :%u failed (err %d)\n", port, LARECOMP_LASTERR);
    LARECOMP_CLOSESOCK(ls);
    return;
  }
  if (listen(ls, 8) == SOCKET_ERROR) {
    LogInfo("admin dashboard listen :%u failed (err %d)\n", port, LARECOMP_LASTERR);
    LARECOMP_CLOSESOCK(ls);
    return;
  }
  g_http_bound = true;
  LogInfo("admin dashboard: http://127.0.0.1:%u/?key=%s\n", port, g_token.c_str());
  LogInfo("mods (public): http://127.0.0.1:%u%s\n", port, larecomp::browser::kModManifestPath);

  for (;;) {
    sockaddr_in cli{};
    socklen_t cl = sizeof(cli);
    socket_t c = accept(ls, reinterpret_cast<sockaddr*>(&cli), &cl);
    if (c == INVALID_SOCKET) continue;
    // Anyone may connect now (the mods are public): one that sends nothing
    // must not stall this loop.
    SetSocketTimeouts(c, 5000, 30000);

    std::string req;
    char b[2048];
    for (int i = 0; i < 8; ++i) {
      int r = recv(c, b, sizeof(b), 0);
      if (r <= 0) break;
      req.append(b, r);
      if (req.find("\r\n\r\n") != std::string::npos) break;
    }
    std::string method, target;
    size_t sp1 = req.find(' ');
    size_t sp2 = (sp1 == std::string::npos) ? std::string::npos : req.find(' ', sp1 + 1);
    if (sp1 != std::string::npos && sp2 != std::string::npos) {
      method = req.substr(0, sp1);
      target = req.substr(sp1 + 1, sp2 - sp1 - 1);
    }
    std::string path = target, query;
    size_t qm = target.find('?');
    if (qm != std::string::npos) {
      path = target.substr(0, qm);
      query = target.substr(qm + 1);
    }

    auto reply = [&](const char* status, const char* ctype, const std::string& body) {
      SendHttpReply(c, status, ctype, body);  // loops on partial sends
    };

    // Public: the mods. Checked before the token.
    namespace br = larecomp::browser;
    const std::string file_prefix = br::kModFilePrefix;
    if (method == "GET" && path == br::kModManifestPath) {
      reply("200 OK", "text/plain; charset=utf-8", ModsSnapshot()->manifest);
      LARECOMP_CLOSESOCK(c);
      continue;
    }
    if (method == "GET" && path.compare(0, file_prefix.size(), file_prefix) == 0) {
      const std::string key = UrlDecode(path.substr(file_prefix.size()), false);
      bool ok = !key.empty() && key[0] != '/' && key.find("..") == std::string::npos &&
                key.find_first_of("\\:") == std::string::npos;
      for (const char ch : key)
        if (static_cast<unsigned char>(ch) < 0x20) ok = false;
      if (!ok) {
        reply("400 Bad Request", "text/plain", "bad path");
        LARECOMP_CLOSESOCK(c);
        continue;
      }
      // Only what the manifest lists: the path is a key, never opened as is.
      const auto mods = ModsSnapshot();
      const auto it = mods->files.find(key);
      if (it == mods->files.end()) {
        reply("404 Not Found", "text/plain", "not found");
        LARECOMP_CLOSESOCK(c);
        continue;
      }
      if (g_downloads.fetch_add(1) >= kDownloadsMax) {
        --g_downloads;
        reply("503 Service Unavailable", "text/plain", "too many downloads; try again shortly");
        LARECOMP_CLOSESOCK(c);
        continue;
      }
      try {
        std::thread(ServeModFile, c, it->second).detach();  // owns c and the slot now
      } catch (const std::exception&) {
        --g_downloads;
        reply("503 Service Unavailable", "text/plain", "try again shortly");
        LARECOMP_CLOSESOCK(c);
      }
      continue;
    }

    if (path == "/" || path == "/index.html") {
      reply("200 OK", "text/html; charset=utf-8", kDashboardHtml);
    } else if (QueryParam(query, "key") != g_token) {
      reply("403 Forbidden", "text/plain", "bad or missing ?key=");
    } else if (path == "/api/state") {
      std::string j;
      {
        std::lock_guard<std::mutex> lk(g_mtx);
        j = g_json;
      }
      reply("200 OK", "application/json", j);
    } else if (path == "/api/kick" || path == "/api/ban" || path == "/api/unban") {
      in_addr ia{};
      std::string ips = QueryParam(query, "ip");
      if (inet_pton(AF_INET, ips.c_str(), &ia) == 1) {
        AdminCmd cmd;
        cmd.type = path == "/api/kick" ? 0 : (path == "/api/ban" ? 1 : 2);
        cmd.ip_net = ia.s_addr;
        {
          std::lock_guard<std::mutex> lk(g_mtx);
          g_cmds.push_back(cmd);
        }
        reply("200 OK", "application/json", "{\"ok\":true}");
      } else {
        reply("400 Bad Request", "text/plain", "bad ip");
      }
    } else if (path == "/api/name") {
      if (SaveName(UrlDecode(QueryParam(query, "name")))) {
        std::string n;
        {
          std::lock_guard<std::mutex> lk(g_ident_mtx);
          n = g_server_name;
        }
        reply("200 OK", "application/json", "{\"ok\":true,\"name\":\"" + JsonEscape(n.c_str()) + "\"}");
      } else {
        reply("500 Internal Server Error", "text/plain", "could not write relay_name.txt");
      }
    } else {
      reply("404 Not Found", "text/plain", "not found");
    }
    LARECOMP_CLOSESOCK(c);
  }
}

// Tells the master server in server.toml (if any) that this relay is up,
// every kAnnounceEverySec. Own thread and own socket: resolving the master's
// name can block, and the relay loop must not. The master checks the claim by
// asking the announced address for details before it lists anything.
void AnnounceLoop(uint16_t relay_port) {
  namespace br = larecomp::browser;
  socket_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == INVALID_SOCKET) {
    LogInfo("master announce: socket failed (err %d); this relay will not be listed\n", LARECOMP_LASTERR);
    return;
  }
  std::string last_error = "<nothing sent yet>";
  for (;;) {
    Identity id;
    {
      std::lock_guard<std::mutex> lk(g_ident_mtx);
      id = g_ident;
    }
    if (!id.master_host.empty()) {
      addrinfo hints{};
      hints.ai_family = AF_INET;
      hints.ai_socktype = SOCK_DGRAM;
      addrinfo* res = nullptr;
      const std::string service = std::to_string(id.master_port);
      std::string error;
      if (getaddrinfo(id.master_host.c_str(), service.c_str(), &hints, &res) == 0 && res) {
        br::AnnounceMsg m{};
        m.hdr.magic = kMagic;
        m.hdr.version = kProtocolVersion;
        m.hdr.type = br::kAnnounce;
        std::memcpy(m.secret, g_secret, sizeof(m.secret));
        m.public_port = id.public_port ? id.public_port : relay_port;
        std::snprintf(m.public_host, sizeof(m.public_host), "%s", id.public_host.c_str());
        if (sendto(s, reinterpret_cast<const char*>(&m), sizeof(m), 0, res->ai_addr,
                   static_cast<int>(res->ai_addrlen)) == SOCKET_ERROR)
          error = "send failed (err " + std::to_string(LARECOMP_LASTERR) + ")";
        freeaddrinfo(res);
      } else {
        error = "cannot resolve " + id.master_host;
      }
      if (error != last_error) {
        if (error.empty())
          LogInfo("master announce: sending to %s:%u as %s:%u\n", id.master_host.c_str(), id.master_port,
                  id.public_host.empty() ? "<this machine's IP>" : id.public_host.c_str(),
                  id.public_port ? id.public_port : relay_port);
        else
          LogInfo("master announce: %s\n", error.c_str());
        last_error = error;
      }
    }
    std::this_thread::sleep_for(std::chrono::seconds(br::kAnnounceEverySec));
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  // Host config -- set it in server.toml next to the exe (see server.toml.example)
  // or as command-line args:  larecomp_relay_server [bind_ip] [port]
  //   bind_ip : which local network to listen on. "0.0.0.0" (default) = ALL
  //             interfaces -- normal, and what works for a public IP, playit.gg
  //             or Hamachi. Give a specific LAN/Hamachi IP only to restrict it
  //             to that one adapter.
  //   port    : UDP port (default 31000). Players / the tunnel must use this.
  std::string bind_ip = (argc >= 2 && argv[1][0]) ? argv[1] : "0.0.0.0";
  int port_arg = (argc >= 3 && argv[2][0]) ? std::atoi(argv[2]) : kServerPort;
  uint16_t port = (port_arg > 0 && port_arg <= 65535) ? static_cast<uint16_t>(port_arg) : kServerPort;

#ifdef _WIN32
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    LogInfo("WSAStartup failed\n");
    return 1;
  }
#endif

  // Resolve the bind address. Aliases for "all interfaces" map to INADDR_ANY;
  // anything else must be a valid local IPv4. Bad input -> warn + fall back to
  // all interfaces rather than refuse to boot.
  uint32_t bind_net;  // network byte order
  const bool any = bind_ip.empty() || bind_ip == "0.0.0.0" || bind_ip == "any" ||
                   bind_ip == "all" || bind_ip == "*";
  if (any) {
    bind_net = htonl(INADDR_ANY);
    bind_ip = "0.0.0.0";
  } else {
    in_addr parsed{};
    if (inet_pton(AF_INET, bind_ip.c_str(), &parsed) == 1) {
      bind_net = parsed.s_addr;
    } else {
      LogInfo("invalid bind IP \"%s\" -- listening on 0.0.0.0 (all) instead\n",
                  bind_ip.c_str());
      bind_net = htonl(INADDR_ANY);
      bind_ip = "0.0.0.0";
    }
  }

  socket_t sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock == INVALID_SOCKET) {
    LogInfo("socket failed\n");
    return 1;
  }
  int yes = 1;
  setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));

  // Big socket buffers: a burst of game packets must not overflow the kernel
  // queue (overflow = silently dropped datagrams = desync). 4 MB each way.
  int bufsz = 4 * 1024 * 1024;
  setsockopt(sock, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&bufsz), sizeof(bufsz));
  setsockopt(sock, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&bufsz), sizeof(bufsz));

#ifdef _WIN32
  // Stop a relayed peer's ICMP port-unreachable from making the NEXT recvfrom
  // fail with WSAECONNRESET (which would otherwise look like a dead socket and
  // cost us packets). Standard Windows UDP-server fix.
  {
    BOOL off = FALSE;
    DWORD ret = 0;
    WSAIoctl(sock, SIO_UDP_CONNRESET, &off, sizeof(off), nullptr, 0, &ret, nullptr, nullptr);
  }
#endif

  sockaddr_in bind_addr{};
  bind_addr.sin_family = AF_INET;
  bind_addr.sin_port = htons(port);
  bind_addr.sin_addr.s_addr = bind_net;
  if (bind(sock, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr)) == SOCKET_ERROR) {
    LogInfo("bind %s:%u failed (err %d) -- port in use, or that IP is not a local adapter?\n",
                bind_ip.c_str(), port, LARECOMP_LASTERR);
    return 1;
  }
#ifdef _WIN32
  u_long nonblock = 1;
  ioctlsocket(sock, FIONBIO, &nonblock);
#else
  fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0) | O_NONBLOCK);
#endif
  LogInfo("relay v5 (2026-10-23): identity by game session id, rooms by game traffic, server browser, "
          "nick check\n");
  LogInfo("matchmaker+relay listening on %s:%u\n", bind_ip.c_str(), port);

  // Allocator state lives next to the exe, so it is the same file however the
  // relay is started (double click, a .bat, a shortcut with another cwd).
  {
    std::string exe;
#ifdef _WIN32
    char path[MAX_PATH] = {};
    DWORD len = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (len > 0 && len < MAX_PATH) exe.assign(path, len);
#endif
    if (exe.empty() && argc >= 1 && argv[0]) exe = argv[0];
    size_t slash = exe.find_last_of("\\/");
    g_dir = slash == std::string::npos ? std::string() : exe.substr(0, slash + 1);
    g_state_path = g_dir + "relay_state.txt";
  }
  g_relay_port = port;
  {
    std::random_device rd;
    for (uint8_t& b : g_secret) b = static_cast<uint8_t>(rd());
  }
  LoadIdentity(true);
  // The first manifest is built here, so the first details answer already
  // counts the mods (hashing ~300 MB takes a second or two, once); later
  // rebuilds run on their own thread.
  RefreshMods(true);
  std::thread(ModsLoop).detach();
  // Names a player may address this relay by in a nick proof, besides the
  // operator's (Identity::id_names): this machine's own IPs on the relay port.
  std::vector<std::string> local_names;
  {
    namespace ident = larecomp::identity;
    local_names.push_back(ident::CanonicalRelayName("127.0.0.1", port));
    local_names.push_back(ident::CanonicalRelayName("localhost", port));
    if (bind_ip != "0.0.0.0") {
      local_names.push_back(ident::CanonicalRelayName(bind_ip, port));
    } else {
      char host[256] = {};
      addrinfo hints{};
      hints.ai_family = AF_INET;
      addrinfo* res = nullptr;
      if (gethostname(host, sizeof(host) - 1) == 0 && getaddrinfo(host, nullptr, &hints, &res) == 0) {
        for (addrinfo* r = res; r; r = r->ai_next) {
          char ip[64];
          IpToStr(reinterpret_cast<sockaddr_in*>(r->ai_addr)->sin_addr.s_addr, ip, sizeof(ip));
          local_names.push_back(ident::CanonicalRelayName(ip, port));
        }
        freeaddrinfo(res);
      }
    }
  }
  // Nick check: (source ip, normalized nick) -> admitted until (ms). A
  // register with a gamertag is only taken from an ip that proved it.
  std::map<std::pair<uint32_t, std::string>, int64_t> admitted;
  // Grace: game session id -> first seen (ms). Per session, not per name: the
  // SDK's probe always says "Player", and every launch is a new session. Kept
  // an hour, so a session dropped for no proof gets no second grace; at most
  // kGraceSessionsPerIpPerMin new ones per ip.
  std::unordered_map<uint64_t, int64_t> grace_start;
  std::unordered_map<uint32_t, std::pair<int64_t, int>> grace_rate;  // ip -> {minute, count}
  constexpr int kGraceSessionsPerIpPerMin = 6;
  constexpr int64_t kGraceMemoryMs = 3600 * 1000;
  int64_t last_refusal_log = 0;
  auto challenge_nonce = [&](uint32_t net_ip, int64_t epoch, uint8_t (&out)[16]) {
    uint8_t msg[12];
    std::memcpy(msg, &net_ip, 4);
    std::memcpy(msg + 4, &epoch, 8);
    crypto_blake2b_keyed(out, 16, g_secret, sizeof(g_secret), msg, sizeof(msg));
  };
  int64_t last_identity_load = NowMs();
  // Per-source budget for the identity queries: ip -> {second, count}.
  std::unordered_map<uint32_t, std::pair<int64_t, int>> identity_rate;
  // Browser target hints: source ip (network order) -> {host vip, expiry ms}.
  // The next host list sent to that ip starts with that host.
  std::unordered_map<uint32_t, std::pair<uint32_t, int64_t>> target_hints;
  bool state_loaded = false;
  uint32_t next_vip = LoadState(&state_loaded);
  LogInfo("vip allocator: next %s (%s %s)\n", VipStr(next_vip).c_str(),
          state_loaded ? "resumed from" : "new state file", g_state_path.c_str());

  std::unordered_map<uint64_t, Peer> peers;            // key = client session id
  std::unordered_map<uint64_t, uint64_t> addr_to_sid;  // control addr key -> session id
  std::unordered_map<uint32_t, uint64_t> vip_to_sid;   // virtual ip -> session id
  // Game-data links (unicast off the discovery port), undirected:
  // key = LinkKey(vip a, vip b), value = last packet (ms). Rooms are built from these.
  std::unordered_map<uint64_t, int64_t> links;
  // Host <-> finder pairs that may reach each other with in-game broadcasts
  // after a LAN search answer: key = LinkKey, value = expiry (ms).
  std::unordered_map<uint64_t, int64_t> lan_reach;
  std::unordered_set<uint64_t> seen_flows;             // FLOW-DIAG: unique (src,dst,port) logged once
  int64_t last_sweep = NowMs();
  uint64_t rx_packets = 0, relayed = 0, send_fails = 0;
  uint64_t drops_unknown_src = 0, drops_unknown_dst = 0;
  int64_t last_unknown_log = 0;
  uint8_t rbuf[4096];  // one reused recv buffer (max datagram ~1.4 KB)
  int last_logged_peers = -1;
  int last_logged_hosts = -1;
  int last_logged_active = -1;

  // Admin panel state -- owned by this (relay) thread. The HTTP thread only
  // pushes commands into g_cmds; we drain them here.
  std::unordered_set<uint32_t> banned_ips;           // network-order IPs, blocked
  std::unordered_map<uint32_t, int64_t> kick_until;  // ip -> cooldown end (ms)
  // Observed game-traffic edges for the topology graph: key=(src_vip<<32|dst_vip),
  // value=last_seen_ms. Discovery (port 1001) is excluded -- only real per-peer
  // game data, so the graph shows star (bug) vs full mesh (ok).
  std::unordered_map<uint64_t, int64_t> flow_edges;
  int64_t start_ms = NowMs();
  int64_t last_ping = start_ms;
  int64_t last_json = start_ms;
  int64_t last_rooms = start_ms;
  {
    std::srand(static_cast<unsigned>(std::time(nullptr)) ^ static_cast<unsigned>(NowMs()));
    char t[9];
    std::snprintf(t, sizeof(t), "%08x",
                  (static_cast<unsigned>(std::rand()) << 16) ^ static_cast<unsigned>(std::rand()) ^
                      static_cast<unsigned>(NowMs()));
    g_token = t;
  }
  std::thread(HttpServer, static_cast<uint16_t>(port + 1), bind_net).detach();
  std::thread(AnnounceLoop, port).detach();

  auto send_to = [&](const void* data, int len, const sockaddr_in& dst) {
    int s = sendto(sock, reinterpret_cast<const char*>(data), len, 0,
                   reinterpret_cast<const sockaddr*>(&dst), sizeof(dst));
    if (s == SOCKET_ERROR)
      ++send_fails;
    else
      ++relayed;
  };

  // Next fresh vip. Never reuses one: the counter only moves forward and is
  // persisted, and anything a live peer holds is skipped.
  auto alloc_vip = [&]() -> uint32_t {
    for (uint32_t tries = 0; tries < 0x01000000u; ++tries) {
      uint32_t v = next_vip;
      next_vip = (next_vip >= kVipLast) ? kVipFirst : next_vip + 1;
      if (!VipAllocatable(v) || vip_to_sid.count(v)) continue;
      SaveState(next_vip);
      return v;
    }
    return 0;  // 16M live peers: cannot happen
  };

  auto remove_peer = [&](uint64_t sid, const char* why) {
    auto it = peers.find(sid);
    if (it == peers.end()) return;
    const Peer& p = it->second;
    LogInfo("peer %s gt=\"%s\" %s\n", VipStr(p.virtual_ip).c_str(),
            Safe(p.info.gamertag).c_str(), why);
    auto a = addr_to_sid.find(KeyOf(p.addr));
    if (a != addr_to_sid.end() && a->second == sid) addr_to_sid.erase(a);
    auto v = vip_to_sid.find(p.virtual_ip);
    if (v != vip_to_sid.end() && v->second == sid) vip_to_sid.erase(v);
    peers.erase(it);
  };

  auto peer_by_addr = [&](const sockaddr_in& a) -> Peer* {
    auto it = addr_to_sid.find(KeyOf(a));
    if (it == addr_to_sid.end()) return nullptr;
    auto p = peers.find(it->second);
    return p == peers.end() ? nullptr : &p->second;
  };

  // Register / heartbeat: find the peer by its session id, following it to a
  // new ip:port if it moved, or create it -- adopting the vip it already
  // carries when it has one.
  auto identify = [&](const sockaddr_in& src, const SessionInfo& s) -> Peer& {
    const uint64_t akey = KeyOf(src);
    // A client that sends no session id is identified by its address, as before.
    const uint64_t sid = s.session_id ? s.session_id : (0x8000000000000000ull | akey);
    const int64_t now = NowMs();

    auto a = addr_to_sid.find(akey);
    if (a != addr_to_sid.end() && a->second != sid) {
      // This ip:port now speaks for another session: that game process is gone.
      remove_peer(a->second, "replaced by a new game on the same address");
    }

    auto it = peers.find(sid);
    if (it != peers.end()) {
      Peer& p = it->second;
      if (KeyOf(p.addr) != akey) {
        LogInfo("peer %s gt=\"%s\" moved %s -> %s (same game, same vip)\n",
                VipStr(p.virtual_ip).c_str(), Safe(s.gamertag).c_str(), AddrStr(p.addr).c_str(),
                AddrStr(src).c_str());
        auto old = addr_to_sid.find(KeyOf(p.addr));
        if (old != addr_to_sid.end() && old->second == sid) addr_to_sid.erase(old);
        p.addr = src;
        addr_to_sid[akey] = sid;
      }
      p.last_seen_ms = now;
      return p;
    }

    Peer p;
    p.addr = src;
    p.sid = sid;
    p.connect_ms = now;
    p.last_seen_ms = now;
    const uint32_t claimed = s.virtual_ip;
    if (claimed != 0 && InVipRange(claimed) && !vip_to_sid.count(claimed)) {
      // The game has this vip cached in its own address since it booted; any
      // other one would strand it. Seen after a relay restart or an outage
      // longer than the peer timeout.
      p.virtual_ip = claimed;
      if (claimed >= next_vip && claimed < kVipLast) {
        next_vip = claimed + 1;
        SaveState(next_vip);
      }
      LogInfo("peer %s gt=\"%s\" from %s: re-adopted its vip\n", VipStr(claimed).c_str(),
              Safe(s.gamertag).c_str(), AddrStr(src).c_str());
    } else {
      if (claimed != 0) {
        LogInfo("WARNING peer gt=\"%s\" from %s claims vip %s, which is %s; giving it a new one "
                "(that game must be restarted to play)\n",
                Safe(s.gamertag).c_str(), AddrStr(src).c_str(), VipStr(claimed).c_str(),
                vip_to_sid.count(claimed) ? "held by another game" : "outside 10.0.0.0/8");
      }
      p.virtual_ip = alloc_vip();
      LogInfo("new peer %s  vip=%s\n", AddrStr(src).c_str(), VipStr(p.virtual_ip).c_str());
    }
    p.room = p.virtual_ip;
    vip_to_sid[p.virtual_ip] = sid;
    addr_to_sid[akey] = sid;
    return peers.emplace(sid, p).first->second;
  };

  // Rooms = connected components of the live game-data links; a room is named
  // by its lowest vip. Stale links (and links to peers that are gone) drop out
  // here, which is what splits a room when players stop talking.
  auto recompute_rooms = [&](int64_t now) {
    std::unordered_map<uint32_t, uint32_t> parent;
    parent.reserve(peers.size() * 2 + 1);
    for (const auto& kv : peers) parent[kv.second.virtual_ip] = kv.second.virtual_ip;
    auto root = [&](uint32_t x) {
      uint32_t r = x;
      while (parent[r] != r) r = parent[r];
      while (parent[x] != r) {
        uint32_t next = parent[x];
        parent[x] = r;
        x = next;
      }
      return r;
    };
    for (auto it = links.begin(); it != links.end();) {
      const uint32_t a = static_cast<uint32_t>(it->first >> 32);
      const uint32_t b = static_cast<uint32_t>(it->first & 0xFFFFFFFFu);
      if (now - it->second > kLinkTtlMs || !parent.count(a) || !parent.count(b)) {
        it = links.erase(it);
        continue;
      }
      uint32_t ra = root(a), rb = root(b);
      if (ra != rb) {
        // Hang the larger root under the smaller, so every root is its component's minimum.
        if (ra < rb)
          parent[rb] = ra;
        else
          parent[ra] = rb;
      }
      ++it;
    }
    for (auto& kv : peers) kv.second.room = root(kv.second.virtual_ip);
    for (auto it = lan_reach.begin(); it != lan_reach.end();) {
      if (now >= it->second)
        it = lan_reach.erase(it);
      else
        ++it;
    }
  };

  // A new game-data link joins two rooms at once (the periodic recompute only
  // has to handle splits), so a broadcast sent right after the first unicast
  // already reaches the new member.
  auto merge_rooms = [&](uint32_t a, uint32_t b) {
    if (a == b) return;
    const uint32_t keep = a < b ? a : b;
    const uint32_t gone = a < b ? b : a;
    for (auto& kv : peers)
      if (kv.second.room == gone) kv.second.room = keep;
  };

  auto room_sizes = [&]() {
    std::unordered_map<uint32_t, int> sizes;
    for (const auto& kv : peers) ++sizes[kv.second.room];
    return sizes;
  };

  // "active": advertised host with proof of a live session; "idle": advertised
  // but no sign of one (its game left the session, or nobody searched lately);
  // "": not a host.
  auto host_state = [&](const Peer& p, int64_t now,
                        const std::unordered_map<uint32_t, int>& sizes) -> const char* {
    if (!p.info.is_host) return "";
    if (p.lan_reply_ms != 0 && now - p.lan_reply_ms <= kHostReplyTtlMs) return "active";
    auto it = sizes.find(p.room);
    if (it != sizes.end() && it->second >= 2) return "active";
    return "idle";
  };

  for (;;) {
    // Block until a packet arrives (instant wake -> no fixed poll latency) or a
    // short timeout fires so the sweep/stats still run when idle.
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(sock, &rfds);
    timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 250000;  // 250 ms
    int sel = select(static_cast<int>(sock) + 1, &rfds, nullptr, nullptr, &tv);

    if (sel > 0 && FD_ISSET(sock, &rfds)) {
      // Drain the whole receive queue this wake so a burst can't sit in the
      // kernel buffer adding latency. Cap so housekeeping stays responsive.
      for (int drained = 0; drained < 8192; ++drained) {
        sockaddr_in src{};
        socklen_t src_len = sizeof(src);
        int n = recvfrom(sock, reinterpret_cast<char*>(rbuf), sizeof(rbuf), 0,
                         reinterpret_cast<sockaddr*>(&src), &src_len);
        if (n < 0)
          break;  // EWOULDBLOCK / WSAEWOULDBLOCK -> queue empty
        ++rx_packets;
        // Admin enforcement: drop anything from a banned or freshly-kicked IP.
        {
          uint32_t sip = src.sin_addr.s_addr;
          if (banned_ips.count(sip)) continue;
          auto kc = kick_until.find(sip);
          if (kc != kick_until.end()) {
            if (NowMs() < kc->second) continue;
            kick_until.erase(kc);
          }
        }
        if (n < static_cast<int>(sizeof(MsgHeader)))
          continue;
        auto* hdr = reinterpret_cast<MsgHeader*>(rbuf);
        if (hdr->magic != kMagic || hdr->version != kProtocolVersion)
          continue;

        // Nick check: hello -> challenge, proof -> ack. Stateless until a
        // proof checks out; same per-IP budget as the queries below.
        if (hdr->type == larecomp::identity::kHello || hdr->type == larecomp::identity::kProof) {
          namespace ident = larecomp::identity;
          const bool hello = hdr->type == ident::kHello;
          if (n != static_cast<int>(hello ? sizeof(ident::HelloMsg) : sizeof(ident::ProofMsg)))
            continue;
          const int64_t sec = NowMs() / 1000;
          auto& rate = identity_rate[src.sin_addr.s_addr];
          if (rate.first != sec) rate = {sec, 0};
          if (++rate.second > kIdentityQueriesPerSec) continue;
          if (identity_rate.size() > 4096) identity_rate.clear();

          Identity id;
          {
            std::lock_guard<std::mutex> lk(g_ident_mtx);
            id = g_ident;
          }
          const int64_t epoch = ident::NowUnix() / ident::kNonceEpochSec;
          if (hello) {
            const auto* q = reinterpret_cast<const ident::HelloMsg*>(rbuf);
            ident::ChallengeMsg a{};
            a.hdr.magic = kMagic;
            a.hdr.version = kProtocolVersion;
            a.hdr.type = ident::kChallenge;
            a.token = q->token;
            a.enforced = id.id_enforce ? 1 : 0;
            challenge_nonce(src.sin_addr.s_addr, epoch, a.nonce);
            std::memcpy(a.master_pubkey, id.id_master_key, 32);
            a.master_port = id.id_master_port;
            std::snprintf(a.master_host, sizeof(a.master_host), "%s", id.id_master_host.c_str());
            send_to(&a, static_cast<int>(sizeof(a)), src);
            continue;
          }
          const auto* q = reinterpret_cast<const ident::ProofMsg*>(rbuf);
          ident::ProofAckMsg a{};
          a.hdr.magic = kMagic;
          a.hdr.version = kProtocolVersion;
          a.hdr.type = ident::kProofAck;
          a.token = q->token;
          const std::string nick = ident::TicketNick(q->ticket);
          uint8_t nonce_now[16], nonce_prev[16];
          challenge_nonce(src.sin_addr.s_addr, epoch, nonce_now);
          challenge_nonce(src.sin_addr.s_addr, epoch - 1, nonce_prev);
          const std::string relay = ident::NickFromField(q->relay, sizeof(q->relay));
          auto named = [&](const std::vector<std::string>& v) {
            return std::find(v.begin(), v.end(), relay) != v.end();
          };
          uint8_t pub[32];
          std::memcpy(pub, q->ticket.body.pubkey, 32);
          uint8_t sig[64];
          std::memcpy(sig, q->sig, 64);
          if (!id.id_enforce) {
            a.status = ident::kProofNotEnforced;
          } else if (crypto_verify16(q->nonce, nonce_now) != 0 &&
                     crypto_verify16(q->nonce, nonce_prev) != 0) {
            a.status = ident::kProofBadNonce;
          } else if (!named(local_names) && !named(id.id_names)) {
            a.status = ident::kProofWrongRelay;
          } else if (!ident::CheckTicket(id.id_master_key, q->ticket, ident::NowUnix())) {
            a.status = int64_t(q->ticket.body.expires) <= ident::NowUnix() ? ident::kProofExpired
                                                                         : ident::kProofBadTicket;
          } else if (!ident::CheckSignature(sig, pub, ident::ProofMessage(q->nonce, q->relay, q->ticket))) {
            a.status = ident::kProofBadSignature;
          } else {
            a.status = ident::kProofOk;
            a.admit_sec = static_cast<uint32_t>(ident::kAdmitSec);
            if (admitted.size() > 65536) admitted.clear();
            auto& until = admitted[{src.sin_addr.s_addr, ident::NormalizeNick(nick)}];
            if (until < NowMs()) {
              char ip[64];
              IpToStr(src.sin_addr.s_addr, ip, sizeof(ip));
              LogInfo("nick \"%s\" verified from %s (key %s)\n", Safe(q->ticket.body.nick).c_str(), ip,
                      ident::Fingerprint(pub).c_str());
            }
            until = NowMs() + ident::kAdmitSec * 1000;
          }
          if (a.status != ident::kProofOk && a.status != ident::kProofNotEnforced) {
            char ip[64];
            IpToStr(src.sin_addr.s_addr, ip, sizeof(ip));
            LogInfo("nick proof for \"%s\" from %s refused (status %u, signed for \"%s\")\n",
                    Safe(q->ticket.body.nick).c_str(), ip, unsigned(a.status), Safe(q->relay).c_str());
          }
          send_to(&a, static_cast<int>(sizeof(a)), src);
          continue;
        }

        // Server browser: details and rooms. Stateless like the identity
        // queries below, and on the same per-IP budget.
        if (hdr->type == larecomp::browser::kDetailsQuery || hdr->type == larecomp::browser::kRoomsQuery) {
          namespace br = larecomp::browser;
          const bool details = hdr->type == br::kDetailsQuery;
          if (n != static_cast<int>(details ? sizeof(br::DetailsMsg) : sizeof(br::RoomsMsg)))
            continue;  // a query is as large as its answer
          const int64_t sec = NowMs() / 1000;
          auto& rate = identity_rate[src.sin_addr.s_addr];
          if (rate.first != sec) rate = {sec, 0};
          if (++rate.second > kIdentityQueriesPerSec) continue;
          if (identity_rate.size() > 4096) identity_rate.clear();

          const int64_t now_ms = NowMs();
          const auto sizes = room_sizes();
          if (details) {
            const auto* q = reinterpret_cast<const br::DetailsMsg*>(rbuf);
            br::DetailsMsg a{};
            a.hdr.magic = kMagic;
            a.hdr.version = kProtocolVersion;
            a.hdr.type = br::kDetails;
            a.token = q->token;
            a.proof = br::ListingProof(g_secret, q->token);
            int rooms = 0;
            for (const auto& kv : peers)
              if (std::strcmp(host_state(kv.second, now_ms, sizes), "active") == 0) ++rooms;
            a.players = static_cast<uint16_t>(peers.size() > 65535 ? 65535 : peers.size());
            a.rooms = static_cast<uint16_t>(rooms);
            a.relay_port = port;
            {
              std::lock_guard<std::mutex> lk(g_ident_mtx);
              a.icon_size = static_cast<uint32_t>(g_icon.size());
              a.icon_crc = g_icon_crc;
              std::snprintf(a.name, sizeof(a.name), "%s", g_server_name.c_str());
              std::snprintf(a.description, sizeof(a.description), "%s", g_ident.description.c_str());
              std::snprintf(a.region, sizeof(a.region), "%s", g_ident.region.c_str());
              std::snprintf(a.tags, sizeof(a.tags), "%s", g_ident.tags.c_str());
              if (g_ident.id_enforce) a.flags |= larecomp::identity::kRelayVerifiedNicks;
              if (g_http_bound)
                a.http_port = g_ident.public_http_port ? g_ident.public_http_port
                                                       : static_cast<uint16_t>(port + 1);
            }
            {
              const auto mods = ModsSnapshot();
              a.mods_required = static_cast<uint8_t>(mods->required > 255 ? 255 : mods->required);
              a.mods_optional = static_cast<uint8_t>(mods->optional > 255 ? 255 : mods->optional);
              a.mods_crc = mods->crc;
            }
            send_to(&a, static_cast<int>(sizeof(a)), src);
          } else {
            const auto* q = reinterpret_cast<const br::RoomsMsg*>(rbuf);
            // Every advertised host, live ones first, then by vip so pages
            // stay in the same order between queries.
            std::vector<const Peer*> hosts;
            for (const auto& kv : peers)
              if (kv.second.info.is_host) hosts.push_back(&kv.second);
            std::vector<std::pair<bool, const Peer*>> order;
            for (const Peer* h : hosts)
              order.push_back({std::strcmp(host_state(*h, now_ms, sizes), "active") == 0, h});
            std::sort(order.begin(), order.end(), [](const auto& x, const auto& y) {
              if (x.first != y.first) return x.first;
              return x.second->virtual_ip < y.second->virtual_ip;
            });
            br::RoomsMsg a{};
            a.hdr.magic = kMagic;
            a.hdr.version = kProtocolVersion;
            a.hdr.type = br::kRooms;
            a.token = q->token;
            a.page = q->page;
            a.total = static_cast<uint16_t>(order.size() > 65535 ? 65535 : order.size());
            const size_t first = static_cast<size_t>(q->page) * br::kRoomsPerPage;
            for (size_t i = first; i < order.size() && a.count < br::kRoomsPerPage; ++i) {
              const Peer& h = *order[i].second;
              br::Room& r = a.rooms[a.count++];
              r.host_vip = h.virtual_ip;
              r.host_rtt_ms = h.rtt_ms < 0 ? 0xFFFF : static_cast<uint16_t>(h.rtt_ms > 0xFFFE ? 0xFFFE : h.rtt_ms);
              auto it = sizes.find(h.room);
              const int cur = it == sizes.end() ? 1 : it->second;
              r.cur_players = static_cast<uint8_t>(cur > 255 ? 255 : cur);
              r.max_players = h.info.max_players;
              r.flags = order[i].first ? br::kRoomActive : 0;
              std::snprintf(r.gamertag, sizeof(r.gamertag), "%s", Safe(h.info.gamertag).c_str());
              if (h.info.name[0]) std::snprintf(r.name, sizeof(r.name), "%s", Safe(h.info.name).c_str());
              const int props = h.info.prop_count < br::kRoomProps ? h.info.prop_count : br::kRoomProps;
              for (int k = 0; k < props; ++k) {
                r.props[k].id = h.info.props[k].id;
                r.props[k].value = h.info.props[k].value;
              }
              r.prop_count = static_cast<uint8_t>(props);
            }
            send_to(&a, static_cast<int>(sizeof(a)), src);
          }
          continue;
        }

        // Target hint: the browser on this IP picked a host; the game's next
        // host list starts with it. Same budget as the queries above; the ack
        // is the query itself, so it never amplifies.
        if (hdr->type == larecomp::browser::kTargetHint) {
          namespace br = larecomp::browser;
          if (n != static_cast<int>(sizeof(br::TargetHintMsg)))
            continue;
          const int64_t sec = NowMs() / 1000;
          auto& rate = identity_rate[src.sin_addr.s_addr];
          if (rate.first != sec) rate = {sec, 0};
          if (++rate.second > kIdentityQueriesPerSec) continue;
          if (identity_rate.size() > 4096) identity_rate.clear();

          br::TargetHintMsg a = *reinterpret_cast<const br::TargetHintMsg*>(rbuf);
          const uint32_t sip = src.sin_addr.s_addr;
          if (a.host_vip == 0) {
            target_hints.erase(sip);
          } else {
            if (target_hints.size() > 4096) target_hints.clear();
            auto& th = target_hints[sip];
            // Log only a change, not every resend of the same pick.
            if (th.first != a.host_vip) {
              char ip[64];
              IpToStr(sip, ip, sizeof(ip));
              LogInfo("target hint from %s: %s\n", ip, VipStr(a.host_vip).c_str());
            }
            th = {a.host_vip, NowMs() + 60000};
          }
          a.hdr.type = br::kTargetHint;
          send_to(&a, static_cast<int>(sizeof(a)), src);
          continue;
        }

        // Server identity (name / player count / icon). Stateless: no peer is
        // created or touched, and nothing is counted as a drop.
        if (hdr->type == kInfoQueryType || hdr->type == kIconQueryType) {
          const bool info = hdr->type == kInfoQueryType;
          if (n != static_cast<int>(info ? sizeof(InfoMsg) : sizeof(IconChunkMsg)))
            continue;  // a query is as large as its answer
          const int64_t sec = NowMs() / 1000;
          auto& rate = identity_rate[src.sin_addr.s_addr];
          if (rate.first != sec) rate = {sec, 0};
          if (++rate.second > kIdentityQueriesPerSec) continue;
          if (identity_rate.size() > 4096) identity_rate.clear();

          if (info) {
            const auto* q = reinterpret_cast<const InfoMsg*>(rbuf);
            InfoMsg a{};
            a.hdr.magic = kMagic;
            a.hdr.version = kProtocolVersion;
            a.hdr.type = kInfoType;
            a.token = q->token;
            const int64_t now_ms = NowMs();
            const auto sizes = room_sizes();
            int sessions = 0;
            for (const auto& kv : peers)
              if (std::strcmp(host_state(kv.second, now_ms, sizes), "active") == 0) ++sessions;
            a.players = static_cast<uint16_t>(peers.size() > 65535 ? 65535 : peers.size());
            a.sessions = static_cast<uint16_t>(sessions);
            {
              std::lock_guard<std::mutex> lk(g_ident_mtx);
              a.icon_size = static_cast<uint32_t>(g_icon.size());
              a.icon_crc = g_icon_crc;
              std::snprintf(a.name, sizeof(a.name), "%s", g_server_name.c_str());
            }
            send_to(&a, static_cast<int>(sizeof(a)), src);
          } else {
            const auto* q = reinterpret_cast<const IconChunkMsg*>(rbuf);
            IconChunkMsg a{};
            a.hdr.magic = kMagic;
            a.hdr.version = kProtocolVersion;
            a.hdr.type = kIconChunkType;
            {
              std::lock_guard<std::mutex> lk(g_ident_mtx);
              // Only for the icon the asker was told about; a stale crc (the
              // icon changed meanwhile) gets nothing and the client starts over.
              if (g_icon.empty() || q->crc != g_icon_crc || q->offset >= g_icon.size()) continue;
              a.crc = g_icon_crc;
              a.offset = q->offset;
              a.total = static_cast<uint32_t>(g_icon.size());
              const size_t left = g_icon.size() - q->offset;
              a.len = static_cast<uint16_t>(left < kIconChunkBytes ? left : kIconChunkBytes);
              std::memcpy(a.data, g_icon.data() + q->offset, a.len);
            }
            send_to(&a, static_cast<int>(sizeof(a)), src);
          }
          continue;
        }

        switch (static_cast<MsgType>(hdr->type)) {
          case MsgType::kRegister:
          case MsgType::kHeartbeat: {
            if (n >= static_cast<int>(sizeof(RegisterMsg))) {
              auto* m = reinterpret_cast<RegisterMsg*>(rbuf);
              const bool is_register = static_cast<MsgType>(hdr->type) == MsgType::kRegister;
              bool enforce;
              int grace_sec;
              {
                std::lock_guard<std::mutex> lk(g_ident_mtx);
                enforce = g_ident.id_enforce;
                grace_sec = g_ident.id_grace_sec;
              }
              if (enforce) {
                namespace ident = larecomp::identity;
                const std::string nick =
                    ident::NickFromField(m->session.gamertag, sizeof(m->session.gamertag));
                const std::pair<uint32_t, std::string> who{src.sin_addr.s_addr, ident::NormalizeNick(nick)};
                const int64_t now_ms = NowMs();
                auto ad = admitted.find(who);
                bool in = ad != admitted.end() && ad->second >= now_ms;
                if (!in && grace_sec > 0) {
                  const uint64_t sid = m->session.session_id ? m->session.session_id
                                                             : (0x8000000000000000ull | KeyOf(src));
                  auto g = grace_start.find(sid);
                  if (g == grace_start.end()) {
                    auto& rate = grace_rate[src.sin_addr.s_addr];
                    const int64_t minute = ident::NowUnix() / 60;  // wall clock: "per minute" as anyone reads it
                    if (rate.first != minute) rate = {minute, 0};
                    if (grace_rate.size() > 65536) grace_rate.clear();
                    if (++rate.second <= kGraceSessionsPerIpPerMin && grace_start.size() < 65536)
                      g = grace_start.emplace(sid, now_ms).first;
                  }
                  in = g != grace_start.end() && now_ms - g->second < int64_t(grace_sec) * 1000;
                }
                if (!in) {
                  // No ack: the game never learns a vip and shows no server.
                  // A peer already in (its proof lapsed) is dropped.
                  if (m->session.session_id) {
                    auto known = peers.find(m->session.session_id);
                    if (known != peers.end()) remove_peer(known->first, "nick no longer verified");
                  }
                  if (NowMs() - last_refusal_log > 2000) {
                    last_refusal_log = NowMs();
                    LogInfo("REFUSED gt=\"%s\" from %s: nick not verified on this relay\n",
                            Safe(m->session.gamertag).c_str(), AddrStr(src).c_str());
                  }
                  break;
                }
              }
              const uint32_t claimed = m->session.virtual_ip;
              Peer& p = identify(src, m->session);
              p.info = m->session;
              p.info.host_ip = src.sin_addr.s_addr;
              p.info.virtual_ip = p.virtual_ip;
              if (is_register) {
                LogInfo("REGISTER vip=%s gt=\"%s\" \"%s\" port=%u host=%u %u/%u\n",
                        VipStr(p.virtual_ip).c_str(), Safe(p.info.gamertag).c_str(),
                        Safe(p.info.name).c_str(), p.info.host_port, p.info.is_host,
                        p.info.cur_players, p.info.max_players);
              }
              // A register always gets an ack: that is how a client learns its
              // vip. A heartbeat gets one only when the client does not carry
              // the vip we hold for it (its first acks were lost), so it can
              // still learn it.
              if (is_register || claimed != p.virtual_ip) {
                RegisterMsg ack = *m;
                ack.hdr.type = static_cast<uint8_t>(MsgType::kRegisterAck);
                ack.session = p.info;
                send_to(&ack, static_cast<int>(sizeof(ack)), src);
              }
            }
            break;
          }
          case MsgType::kPong: {
            if (n >= static_cast<int>(sizeof(PingMsg))) {
              auto* pm = reinterpret_cast<PingMsg*>(rbuf);
              Peer* p = peer_by_addr(src);
              if (p && p->ping_token == pm->token) {
                p->rtt_ms = static_cast<int>(NowMs() - p->ping_sent_ms);
                p->last_seen_ms = NowMs();
              }
            }
            break;
          }
          case MsgType::kUnregister: {
            Peer* p = peer_by_addr(src);
            if (p) remove_peer(p->sid, "unregistered");
            break;
          }
          case MsgType::kQuery: {
            // Host list in ONE datagram (cap so it never fragments). Lobby
            // browse only -- not the game-data path, and not what System Link
            // uses to find games (that is the LAN broadcast on port 1001).
            constexpr int kMaxList =
                static_cast<int>((1200 - sizeof(ListMsg)) / sizeof(SessionInfo));
            uint8_t out[sizeof(ListMsg) + kMaxList * sizeof(SessionInfo)];
            auto* lm = reinterpret_cast<ListMsg*>(out);
            lm->hdr.magic = kMagic;
            lm->hdr.version = kProtocolVersion;
            lm->hdr.type = static_cast<uint8_t>(MsgType::kList);
            Peer* requester = peer_by_addr(src);
            const uint64_t self = requester ? requester->sid : 0;
            const auto sizes = room_sizes();
            int count = 0;
            auto add = [&](const Peer& pr) {
              // Advertise the REAL current player count = peers in this host's
              // room (the client only knows its own tally).
              SessionInfo si = pr.info;
              auto it = sizes.find(pr.room);
              int cur = it == sizes.end() ? 1 : it->second;
              si.cur_players = static_cast<uint8_t>(cur > 255 ? 255 : cur);
              std::memcpy(out + sizeof(ListMsg) + count * sizeof(SessionInfo), &si,
                          sizeof(SessionInfo));
              ++count;
            };
            // The host the browser on this IP picked goes first: the list only
            // holds kMaxList hosts, so with more it might never show up.
            bool has_hint = false;
            uint64_t hinted = 0;
            auto th = target_hints.find(src.sin_addr.s_addr);
            if (th != target_hints.end()) {
              if (NowMs() >= th->second.second) {
                target_hints.erase(th);
              } else {
                auto vs = vip_to_sid.find(th->second.first);
                auto hp = vs == vip_to_sid.end() ? peers.end() : peers.find(vs->second);
                if (hp != peers.end() && hp->second.info.is_host && hp->first != self) {
                  has_hint = true;
                  hinted = hp->first;
                  add(hp->second);
                }
              }
            }
            for (auto& [sid, pr] : peers) {
              if (!pr.info.is_host)
                continue;
              if (sid == self)
                continue;  // never return the requester's own session to itself
              if (has_hint && sid == hinted)
                continue;  // already first
              if (count >= kMaxList)
                break;
              add(pr);
            }
            lm->count = static_cast<uint16_t>(count);
            send_to(out, static_cast<int>(sizeof(ListMsg) + count * sizeof(SessionInfo)), src);
            break;
          }
          case MsgType::kRelay: {
            if (n < static_cast<int>(sizeof(RelayMsg)))
              break;
            auto* rm = reinterpret_cast<RelayMsg*>(rbuf);
            uint16_t inner_len = rm->inner_len;
            if (inner_len > kMaxInner || n < static_cast<int>(sizeof(RelayMsg)) + inner_len)
              break;
            Peer* sp = peer_by_addr(src);
            if (!sp) {
              // Not identified yet: a game whose ip:port changed, or any game
              // right after a relay restart. Its next heartbeat (<= ~5 s)
              // identifies it with the vip it already has; minting a new peer
              // here would give it a second, wrong vip.
              ++drops_unknown_src;
              int64_t now = NowMs();
              if (now - last_unknown_log >= 2000) {
                last_unknown_log = now;
                LogInfo("relay from unidentified %s dropped, waiting for its heartbeat (%llu so far)\n",
                        AddrStr(src).c_str(), static_cast<unsigned long long>(drops_unknown_src));
              }
              break;
            }
            Peer& sender = *sp;
            const int64_t now = NowMs();
            sender.last_seen_ms = now;
            ++sender.rx_pkts;
            sender.rx_bytes += inner_len;
            // FLOW-DIAG: log each unique (src vip, dst, port) once to reveal the
            // communication graph (is it a full mesh or a star through the host?).
            {
              uint64_t fk = (static_cast<uint64_t>(sender.virtual_ip & 0xFFFFFF) << 40) |
                            (static_cast<uint64_t>(rm->addr & 0xFFFFFF) << 16) |
                            static_cast<uint64_t>(rm->src_port);
              if (seen_flows.insert(fk).second) {
                if (rm->addr == 0) {
                  LogInfo("FLOW %s -> BCAST port=%u room=%s\n", VipStr(sender.virtual_ip).c_str(),
                          rm->src_port, VipStr(sender.room).c_str());
                } else {
                  LogInfo("FLOW %s -> %s port=%u\n", VipStr(sender.virtual_ip).c_str(),
                          VipStr(rm->addr).c_str(), rm->src_port);
                }
              }
            }
            // Build the deliver packet on the stack (no per-packet heap alloc),
            // tagged with the SENDER's virtual ip.
            uint8_t out[sizeof(RelayMsg) + kMaxInner];
            auto* dm = reinterpret_cast<RelayMsg*>(out);
            dm->hdr.magic = kMagic;
            dm->hdr.version = kProtocolVersion;
            dm->hdr.type = static_cast<uint8_t>(MsgType::kRelayDeliver);
            dm->addr = sender.virtual_ip;
            dm->src_port = rm->src_port;
            dm->inner_len = inner_len;
            std::memcpy(out + sizeof(RelayMsg), reinterpret_cast<uint8_t*>(rm) + sizeof(RelayMsg),
                        inner_len);
            const int out_len = static_cast<int>(sizeof(RelayMsg) + inner_len);

            if (rm->addr == 0) {
              // Broadcast. Discovery (port 1001) is GLOBAL so finders see every
              // hosted game; other ports (in-game data) are scoped to the
              // sender's room so simultaneous games don't leak into each other.
              const bool global = (rm->src_port == kDiscoveryPort);
              const uint32_t room = sender.room;
              for (auto& [sid, pr] : peers) {
                if (sid == sender.sid)
                  continue;
                if (!global && pr.room != room) {
                  auto r = lan_reach.find(LinkKey(sender.virtual_ip, pr.virtual_ip));
                  if (r == lan_reach.end() || now >= r->second)
                    continue;
                }
                send_to(out, out_len, pr.addr);
                ++pr.tx_pkts;
                pr.tx_bytes += inner_len;
                if (!global)
                  flow_edges[(static_cast<uint64_t>(sender.virtual_ip) << 32) | pr.virtual_ip] = now;
              }
            } else {
              auto vit = vip_to_sid.find(rm->addr);
              auto pit = vit == vip_to_sid.end() ? peers.end() : peers.find(vit->second);
              if (pit == peers.end()) {
                ++drops_unknown_dst;  // that vip is offline or not identified yet
                break;
              }
              Peer& dst = pit->second;
              send_to(out, out_len, dst.addr);
              ++dst.tx_pkts;
              dst.tx_bytes += inner_len;
              if (rm->src_port == kDiscoveryPort) {
                // A unicast to the discovery port is a host answering a LAN
                // search: proof its session is live. It groups no one, but
                // lets the pair reach each other's broadcasts for a while.
                sender.lan_reply_ms = now;
                lan_reach[LinkKey(sender.virtual_ip, dst.virtual_ip)] = now + kLanReachMs;
              } else {
                links[LinkKey(sender.virtual_ip, dst.virtual_ip)] = now;
                merge_rooms(sender.room, dst.room);
                flow_edges[(static_cast<uint64_t>(sender.virtual_ip) << 32) | dst.virtual_ip] = now;
              }
            }
            break;
          }
          default:
            break;
        }
      }
    }

    int64_t now = NowMs();

    // Drain admin commands from the dashboard (kick / ban / unban).
    {
      std::vector<AdminCmd> cmds;
      {
        std::lock_guard<std::mutex> lk(g_mtx);
        cmds.swap(g_cmds);
      }
      for (const auto& cm : cmds) {
        if (cm.type == 1) banned_ips.insert(cm.ip_net);
        if (cm.type == 2) {
          banned_ips.erase(cm.ip_net);
          kick_until.erase(cm.ip_net);
        }
        if (cm.type == 0) kick_until[cm.ip_net] = now + 30000;  // 30s cool-off
        if (cm.type == 0 || cm.type == 1) {
          char ip[64];
          IpToStr(cm.ip_net, ip, sizeof(ip));
          std::vector<uint64_t> doomed;
          for (const auto& kv : peers)
            if (kv.second.addr.sin_addr.s_addr == cm.ip_net) doomed.push_back(kv.first);
          for (uint64_t sid : doomed)
            remove_peer(sid, cm.type == 1 ? "banned by admin" : "kicked by admin");
          if (!doomed.empty()) LogInfo("admin %s %s\n", cm.type == 1 ? "BAN" : "KICK", ip);
        }
      }
    }

    // Rooms follow the game traffic; refresh a few times a second.
    if (now - last_rooms >= 250) {
      last_rooms = now;
      recompute_rooms(now);
    }

    // RTT probes: fresh token to every peer every 2 s (peer echoes kPong).
    if (now - last_ping >= 2000) {
      last_ping = now;
      PingMsg pm{};
      pm.hdr.magic = kMagic;
      pm.hdr.version = kProtocolVersion;
      pm.hdr.type = static_cast<uint8_t>(MsgType::kPing);
      for (auto& kv : peers) {
        Peer& pr = kv.second;
        pr.ping_token =
            (static_cast<uint64_t>(std::rand()) << 32) ^ static_cast<uint64_t>(now) ^ pr.virtual_ip;
        pr.ping_sent_ms = now;
        pm.token = pr.ping_token;
        sendto(sock, reinterpret_cast<const char*>(&pm), sizeof(pm), 0,
               reinterpret_cast<const sockaddr*>(&pr.addr), sizeof(pr.addr));
      }
    }

    // relay_name.txt / relay_icon.png are picked up while running.
    if (now - last_identity_load >= 5000) {
      last_identity_load = now;
      LoadIdentity(false);
    }

    // Publish a JSON snapshot for the dashboard every 500 ms.
    if (now - last_json >= 500) {
      last_json = now;
      const auto sizes = room_sizes();
      int hosts_n = 0, hosts_active = 0;
      for (const auto& kv : peers) {
        const char* hs = host_state(kv.second, now, sizes);
        if (hs[0]) ++hosts_n;
        if (std::strcmp(hs, "active") == 0) ++hosts_active;
      }
      std::string j = "{\"uptime_ms\":" + std::to_string(now - start_ms);
      j += ",\"peers\":" + std::to_string(peers.size());
      j += ",\"hosts\":" + std::to_string(hosts_n);
      j += ",\"hosts_active\":" + std::to_string(hosts_active);
      j += ",\"rx\":" + std::to_string(rx_packets);
      j += ",\"relayed\":" + std::to_string(relayed);
      j += ",\"sendfail\":" + std::to_string(send_fails);
      j += ",\"drop_src\":" + std::to_string(drops_unknown_src);
      j += ",\"drop_dst\":" + std::to_string(drops_unknown_dst);
      j += ",\"next_vip\":\"" + VipStr(next_vip) + "\"";
      {
        std::lock_guard<std::mutex> lk(g_ident_mtx);
        j += ",\"name\":\"" + JsonEscape(g_server_name.c_str()) + "\"";
        j += ",\"icon_bytes\":" + std::to_string(g_icon.size());
      }
      j += ",\"banned\":[";
      {
        bool f = true;
        for (auto ip : banned_ips) {
          char b[64];
          IpToStr(ip, b, sizeof(b));
          j += f ? "\"" : ",\"";
          j += b;
          j += "\"";
          f = false;
        }
      }
      j += "],\"list\":[";
      bool first = true;
      for (const auto& kv : peers) {
        const Peer& pr = kv.second;
        if (!first) j += ",";
        first = false;
        char ip[64];
        IpToStr(pr.addr.sin_addr.s_addr, ip, sizeof(ip));
        char xb[24], sb[24];
        std::snprintf(xb, sizeof(xb), "%016llX", static_cast<unsigned long long>(pr.info.xuid));
        std::snprintf(sb, sizeof(sb), "%016llX", static_cast<unsigned long long>(pr.sid));
        j += "{\"vip\":\"" + VipStr(pr.virtual_ip) + "\"";
        j += ",\"sid\":\"" + std::string(sb) + "\"";
        j += ",\"ip\":\"" + std::string(ip) + "\"";
        j += ",\"port\":" + std::to_string(ntohs(pr.addr.sin_port));
        j += ",\"gamertag\":\"" + JsonEscape(Safe(pr.info.gamertag).c_str()) + "\"";
        j += ",\"name\":\"" + JsonEscape(Safe(pr.info.name).c_str()) + "\"";
        j += ",\"xuid\":\"" + std::string(xb) + "\"";
        j += ",\"is_host\":" + std::string(pr.info.is_host ? "true" : "false");
        j += ",\"host_state\":\"" + std::string(host_state(pr, now, sizes)) + "\"";
        j += ",\"lan_reply_age_ms\":" +
             std::to_string(pr.lan_reply_ms ? now - pr.lan_reply_ms : -1);
        j += ",\"room\":\"" + VipStr(pr.room) + "\"";
        j += ",\"gamemode\":" + std::to_string(pr.info.gamemode);
        j += ",\"map\":" + std::to_string(pr.info.map);
        j += ",\"max\":" + std::to_string(pr.info.max_players);
        j += ",\"rtt\":" + std::to_string(pr.rtt_ms);
        j += ",\"conn_ms\":" + std::to_string(now - pr.connect_ms);
        j += ",\"rx_pkts\":" + std::to_string(pr.rx_pkts);
        j += ",\"tx_pkts\":" + std::to_string(pr.tx_pkts);
        j += "}";
      }
      j += "],\"edges\":[";
      {
        bool ef = true;
        for (auto it = flow_edges.begin(); it != flow_edges.end();) {
          if (now - it->second > 5000) {  // stale: peer stopped sending on that link
            it = flow_edges.erase(it);
            continue;
          }
          uint32_t s = static_cast<uint32_t>(it->first >> 32);
          uint32_t d = static_cast<uint32_t>(it->first & 0xFFFFFFFF);
          j += ef ? "{" : ",{";
          j += "\"f\":\"" + VipStr(s) + "\",\"t\":\"" + VipStr(d) + "\"}";
          ef = false;
          ++it;
        }
      }
      j += "]}";
      {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_json.swap(j);
      }

      // Log peer / host count changes (computed with the same rules as the dashboard).
      int current_peers = static_cast<int>(peers.size());
      if (current_peers != last_logged_peers || hosts_n != last_logged_hosts ||
          hosts_active != last_logged_active) {
        last_logged_peers = current_peers;
        last_logged_hosts = hosts_n;
        last_logged_active = hosts_active;
        LogInfo("peers=%d hosts=%d (active=%d) | rx=%llu relayed=%llu sendfail=%llu dropped=%llu\n",
                current_peers, hosts_n, hosts_active, static_cast<unsigned long long>(rx_packets),
                static_cast<unsigned long long>(relayed),
                static_cast<unsigned long long>(send_fails),
                static_cast<unsigned long long>(drops_unknown_src + drops_unknown_dst));
      }
    }

    if (now - last_sweep >= 1000) {
      last_sweep = now;
      std::vector<uint64_t> expired;
      for (const auto& kv : peers)
        if (now - kv.second.last_seen_ms > kSessionTimeoutSec * 1000) expired.push_back(kv.first);
      // A timed-out game keeps its vip: nothing else gets it (the counter only
      // moves forward), and it is adopted back if that game returns.
      for (uint64_t sid : expired) remove_peer(sid, "timed out");
      for (auto it = admitted.begin(); it != admitted.end();) {
        if (it->second < now)
          it = admitted.erase(it);
        else
          ++it;
      }
      for (auto it = grace_start.begin(); it != grace_start.end();) {
        if (now - it->second > kGraceMemoryMs)
          it = grace_start.erase(it);
        else
          ++it;
      }
      // A peer whose grace ran out without a proof leaves now, not at its
      // next heartbeat.
      bool enforce_now;
      int grace_now;
      {
        std::lock_guard<std::mutex> lk(g_ident_mtx);
        enforce_now = g_ident.id_enforce;
        grace_now = g_ident.id_grace_sec;
      }
      if (enforce_now) {
        namespace ident = larecomp::identity;
        std::vector<uint64_t> unproved;
        for (const auto& kv : peers) {
          const std::pair<uint32_t, std::string> who{
              kv.second.addr.sin_addr.s_addr,
              ident::NormalizeNick(ident::NickFromField(kv.second.info.gamertag, sizeof(kv.second.info.gamertag)))};
          auto ad = admitted.find(who);
          if (ad != admitted.end() && ad->second >= now) continue;
          auto g = grace_start.find(kv.first);
          if (g != grace_start.end() && now - g->second < int64_t(grace_now) * 1000) continue;
          unproved.push_back(kv.first);
        }
        for (uint64_t sid : unproved) remove_peer(sid, "nick not proved in time");
      }
    }
  }

  LARECOMP_CLOSESOCK(sock);
#ifdef _WIN32
  WSACleanup();
#endif
  return 0;
}
