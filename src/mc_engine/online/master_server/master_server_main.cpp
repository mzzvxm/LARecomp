//
// larecomp_master_server: the list of relays the in-game server browser shows.
//
// Relays with `master = "<this host>:31003"` in their server.toml announce
// themselves here every 30 s over UDP. The master never lists an address on
// the relay's word: it sends a details query to the announced address and
// lists it only when the answer carries a proof made with the secret from the
// announce (browser::ListingProof), so
// a listed relay is one players can actually reach. A relay that stops
// answering drops off after ~100 s.
//
//   larecomp_master_server [--port 31003]
//
// UDP <port>  announces in, details probes out (../protocol/browser_protocol.h)
// TCP <port>  HTTP:
//   GET  /servers  -> text/plain, one "host:port" per line (what the game reads)
//   POST /servers  -> the same (the game's HTTP helper only POSTs)
//   GET  /         -> plain-text status: every listed relay with its name
//   POST /identity/v1         -> nick registry (identity_registry.h)
//   GET  /identity/v1/pubkey  -> the key relays pin as identity_master_key
//
//   --transfer-hold-hours N  how long a recovery code waits before the nick
//                            moves, while the owner can cancel (default 168)
//   --expire-days N          a nick unused this long is free again (default 90)
//
// Open UDP and TCP <port> inbound on the host's firewall.
//

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>

#include "../rate_my_ride/rmr_net.h"
#include "../protocol/browser_protocol.h"
#include "identity_registry.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
#define MASTER_CLOSESOCK closesocket
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
#define INVALID_SOCKET (-1)
#define MASTER_CLOSESOCK ::close
#endif

namespace br = larecomp::browser;

namespace {

// Limits that keep one announcer from filling the list or using the master to
// send probes at someone else.
constexpr size_t kMaxListed = 1024;
constexpr int kMaxListedPerSourceIp = 16;
constexpr int kAnnouncesPerSourceIpPerMinute = 40;
constexpr int64_t kProbeTimeoutMs = 3000;
constexpr size_t kMaxPendingProbes = 4096;

std::atomic<bool> g_stop{false};

void OnSignal(int) { g_stop.store(true); }

void Log(const std::string& msg) {
  char stamp[32];
  const std::time_t t = std::time(nullptr);
  std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
  std::printf("[%s] %s\n", stamp, msg.c_str());
  std::fflush(stdout);
}

int64_t NowMs() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::string IpStr(uint32_t net_ip) {
  char b[64];
  in_addr a;
  a.s_addr = net_ip;
  inet_ntop(AF_INET, &a, b, sizeof(b));
  return b;
}

// A host name or IPv4 as announced: letters, digits, dots and dashes only.
bool CleanHost(const char (&raw)[64], std::string& out) {
  out.clear();
  for (char c : raw) {
    if (c == 0) break;
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == '.' || c == '-';
    if (!ok) return false;
    out += c;
  }
  return true;
}

// Printable text from a relay, for the status page and the log only.
template <size_t N>
std::string Printable(const char (&raw)[N]) {
  std::string out;
  for (size_t i = 0; i < N && raw[i]; ++i) {
    const unsigned char c = static_cast<unsigned char>(raw[i]);
    out += (c >= 0x20 && c != 0x7F) ? static_cast<char>(c) : '?';
  }
  return out;
}

struct Listing {
  std::string host;  // as players connect to it
  uint16_t port = 0;
  uint32_t announcer_ip = 0;  // network order
  std::string secret;         // the announcing relay run's, 16 bytes
  std::string name;
  int players = 0;
  int rooms = 0;
  int64_t verified_ms = 0;
};

struct Probe {
  std::string key;  // "host:port"
  std::string host;
  uint16_t port = 0;
  uint32_t announcer_ip = 0;
  std::string secret;  // 16 bytes
  sockaddr_in resolved{};
  int64_t deadline_ms = 0;
};

std::mutex g_mtx;
std::map<std::string, Listing> g_listed;           // key "host:port"
std::unordered_map<uint32_t, Probe> g_pending;     // token -> probe sent
std::deque<Probe> g_to_resolve;                    // announces waiting for a DNS lookup
std::condition_variable g_resolve_cv;
socket_t g_sock = INVALID_SOCKET;

void Expire(int64_t now) {
  for (auto it = g_listed.begin(); it != g_listed.end();) {
    if (now - it->second.verified_ms > int64_t(br::kListingTtlSec) * 1000) {
      Log("unlisted " + it->first + " \"" + it->second.name + "\" (stopped answering)");
      it = g_listed.erase(it);
    } else {
      ++it;
    }
  }
  for (auto it = g_pending.begin(); it != g_pending.end();) {
    if (now > it->second.deadline_ms)
      it = g_pending.erase(it);
    else
      ++it;
  }
}

// Resolves announced host names off the UDP thread (a lookup can take
// seconds) and sends the details probe.
void ResolveLoop() {
  std::mt19937 rng{std::random_device{}()};
  for (;;) {
    Probe p;
    {
      std::unique_lock<std::mutex> lk(g_mtx);
      g_resolve_cv.wait_for(lk, std::chrono::seconds(1),
                            [] { return !g_to_resolve.empty() || g_stop.load(); });
      if (g_stop.load()) return;
      if (g_to_resolve.empty()) continue;
      p = std::move(g_to_resolve.front());
      g_to_resolve.pop_front();
    }
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    const std::string service = std::to_string(p.port);
    if (getaddrinfo(p.host.c_str(), service.c_str(), &hints, &res) != 0 || !res) continue;
    std::memcpy(&p.resolved, res->ai_addr, sizeof(p.resolved));
    freeaddrinfo(res);

    br::DetailsMsg q{};
    q.hdr.magic = br::kMagic;
    q.hdr.version = br::kVersion;
    q.hdr.type = br::kDetailsQuery;
    q.token = rng();
    p.deadline_ms = NowMs() + kProbeTimeoutMs;
    {
      std::lock_guard<std::mutex> lk(g_mtx);
      if (g_pending.size() >= kMaxPendingProbes) continue;
      g_pending[q.token] = p;
    }
    sendto(g_sock, reinterpret_cast<const char*>(&q), sizeof(q), 0,
           reinterpret_cast<const sockaddr*>(&p.resolved), sizeof(p.resolved));
  }
}

void UdpLoop() {
  std::unordered_map<uint32_t, std::pair<int64_t, int>> rate;  // ip -> {minute, count}
  uint8_t buf[2048];
  int64_t last_expire = NowMs();
  while (!g_stop.load()) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(g_sock, &set);
    timeval tv{0, 250000};
    const int sel = select(int(g_sock) + 1, &set, nullptr, nullptr, &tv);
    const int64_t now = NowMs();
    if (now - last_expire >= 1000) {
      last_expire = now;
      std::lock_guard<std::mutex> lk(g_mtx);
      Expire(now);
    }
    if (sel <= 0) continue;

    sockaddr_in src{};
    socklen_t len = sizeof(src);
    const int n = recvfrom(g_sock, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                           reinterpret_cast<sockaddr*>(&src), &len);
    if (n < int(sizeof(br::Header))) continue;
    const auto* hdr = reinterpret_cast<const br::Header*>(buf);
    if (hdr->magic != br::kMagic || hdr->version != br::kVersion) continue;

    if (hdr->type == br::kAnnounce && n == int(sizeof(br::AnnounceMsg))) {
      const auto* m = reinterpret_cast<const br::AnnounceMsg*>(buf);
      const uint32_t ip = src.sin_addr.s_addr;
      auto& r = rate[ip];
      if (r.first != now / 60000) r = {now / 60000, 0};
      if (++r.second > kAnnouncesPerSourceIpPerMinute) continue;
      if (rate.size() > 65536) rate.clear();

      Probe p;
      if (!CleanHost(m->public_host, p.host) || m->public_port == 0) continue;
      if (p.host.empty()) p.host = IpStr(ip);
      p.port = m->public_port;
      p.key = p.host + ":" + std::to_string(p.port);
      p.announcer_ip = ip;
      p.secret.assign(reinterpret_cast<const char*>(m->secret), sizeof(m->secret));

      std::lock_guard<std::mutex> lk(g_mtx);
      auto it = g_listed.find(p.key);
      if (it == g_listed.end()) {
        if (g_listed.size() >= kMaxListed) continue;
        int from_ip = 0;
        for (const auto& kv : g_listed)
          if (kv.second.announcer_ip == ip) ++from_ip;
        if (from_ip >= kMaxListedPerSourceIp) continue;
      }
      if (g_to_resolve.size() < kMaxPendingProbes) {
        g_to_resolve.push_back(std::move(p));
        g_resolve_cv.notify_one();
      }
    } else if (hdr->type == br::kDetails && n == int(sizeof(br::DetailsMsg))) {
      const auto* a = reinterpret_cast<const br::DetailsMsg*>(buf);
      std::lock_guard<std::mutex> lk(g_mtx);
      auto it = g_pending.find(a->token);
      if (it == g_pending.end()) continue;
      const Probe p = it->second;
      // The answer must come from where the probe went, and from the relay
      // run that announced.
      if (src.sin_addr.s_addr != p.resolved.sin_addr.s_addr || src.sin_port != p.resolved.sin_port)
        continue;
      g_pending.erase(it);
      uint8_t secret[16];
      std::memcpy(secret, p.secret.data(), sizeof(secret));
      if (a->proof != br::ListingProof(secret, a->token)) continue;
      const bool fresh = !g_listed.count(p.key) || g_listed[p.key].secret != p.secret;
      Listing& l = g_listed[p.key];
      l.host = p.host;
      l.port = p.port;
      l.announcer_ip = p.announcer_ip;
      l.secret = p.secret;
      l.name = Printable(a->name);
      l.players = a->players;
      l.rooms = a->rooms;
      l.verified_ms = now;
      if (fresh) Log("listed " + p.key + " \"" + l.name + "\" (announced from " + IpStr(p.announcer_ip) + ")");
    }
  }
}

}  // namespace

// The exe's folder, with a trailing slash: where the registry files live
// however the master is started.
std::string ExeDir(const char* argv0) {
  std::string exe;
#if defined(_WIN32)
  char path[MAX_PATH] = {};
  const DWORD len = GetModuleFileNameA(nullptr, path, MAX_PATH);
  if (len > 0 && len < MAX_PATH) exe.assign(path, len);
#endif
  if (exe.empty() && argv0) exe = argv0;
  const size_t slash = exe.find_last_of("\\/");
  return slash == std::string::npos ? std::string() : exe.substr(0, slash + 1);
}

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);  // registry lines show up as they happen
  uint16_t port = br::kMasterPort;
  larecomp::identity::RegistryConfig id_config;
  id_config.dir = ExeDir(argc > 0 ? argv[0] : nullptr);
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--port" && i + 1 < argc) {
      const int p = std::atoi(argv[++i]);
      if (p > 0 && p < 65536) port = uint16_t(p);
    } else if (arg == "--transfer-hold-hours" && i + 1 < argc) {
      id_config.transfer_hold_sec = int64_t(std::atof(argv[++i]) * 3600);
    } else if (arg == "--expire-days" && i + 1 < argc) {
      id_config.expire_sec = int64_t(std::atof(argv[++i]) * 86400);
    } else if (arg == "--transfer-hold-sec" && i + 1 < argc) {  // tests
      id_config.transfer_hold_sec = std::atoll(argv[++i]);
    } else if (arg == "--expire-sec" && i + 1 < argc) {  // tests
      id_config.expire_sec = std::atoll(argv[++i]);
    } else {
      std::printf("usage: larecomp_master_server [--port %u] [--transfer-hold-hours 168] "
                  "[--expire-days 90]\n",
                  unsigned(br::kMasterPort));
      return 2;
    }
  }
  larecomp::identity::Registry registry;
  {
    std::string error;
    if (!registry.Open(id_config, &error)) {
      Log("nick registry: " + error);
      return 1;
    }
  }
  const std::string master_pub =
      larecomp::identity::ToHex(registry.public_key(), sizeof(registry.public_key()));
  std::signal(SIGINT, OnSignal);
  std::signal(SIGTERM, OnSignal);

#if defined(_WIN32)
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    Log("WSAStartup failed");
    return 1;
  }
#endif
  g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  if (g_sock == INVALID_SOCKET || bind(g_sock, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
    Log("cannot bind UDP " + std::to_string(port) + " (in use?)");
    return 1;
  }
#if defined(_WIN32)
  {
    // A probe to a dead relay comes back as ICMP unreachable; without this
    // the next recvfrom fails with WSAECONNRESET.
    BOOL off = FALSE;
    DWORD ret = 0;
    WSAIoctl(g_sock, SIO_UDP_CONNRESET, &off, sizeof(off), nullptr, 0, &ret, nullptr, nullptr);
  }
#endif
  Log("master server: announces on UDP " + std::to_string(port) + ", list on http://<this host>:" +
      std::to_string(port) + "/servers");
  Log("nick registry: " + std::to_string(registry.size()) +
      " nick(s); relays that check nicks set identity_master_key = \"" + master_pub +
      "\" in server.toml");

  std::thread udp(UdpLoop);
  std::thread resolver(ResolveLoop);

  std::string error;
  const bool served = rmr::net::Serve(
      port,
      [&](const rmr::net::HttpRequest& req, rmr::net::HttpResponse& res) {
        std::string path = req.path;
        if (const size_t q = path.find('?'); q != std::string::npos) path.resize(q);
        std::string body;
        res.content_type = "text/plain; charset=utf-8";
        if (path == larecomp::identity::kMasterPath && req.method == "POST") {
          const std::string peer_ip = req.peer.substr(0, req.peer.rfind(':'));
          body = registry.Handle(std::string(req.body.begin(), req.body.end()), peer_ip);
          res.body.assign(body.begin(), body.end());
          return;
        }
        if (path == larecomp::identity::kMasterKeyPath && req.method == "GET") {
          body = master_pub + "\n";
          res.body.assign(body.begin(), body.end());
          return;
        }
        std::lock_guard<std::mutex> lk(g_mtx);
        if (path == "/servers" && (req.method == "GET" || req.method == "POST")) {
          for (const auto& kv : g_listed) body += kv.first + "\n";
        } else if (path == "/" && req.method == "GET") {
          body = "larecomp master server: " + std::to_string(g_listed.size()) + " relay(s) listed, " +
                 std::to_string(registry.size()) + " nick(s) registered\nidentity_master_key = " +
                 master_pub + "\n\n";
          for (const auto& kv : g_listed)
            body += kv.first + "  \"" + kv.second.name + "\"  players " +
                    std::to_string(kv.second.players) + "  rooms " + std::to_string(kv.second.rooms) + "\n";
        } else {
          res.status = 404;
          body = "not found\n";
        }
        res.body.assign(body.begin(), body.end());
      },
      g_stop, &error);
  if (!served) Log("cannot serve HTTP on TCP " + std::to_string(port) + ": " + error);
  g_stop.store(true);
  g_resolve_cv.notify_all();
  udp.join();
  resolver.join();
  MASTER_CLOSESOCK(g_sock);
  return served ? 0 : 1;
}
