// Check of the --bind <ip> option of larecomp_master_server, larecomp_rmr_server and larecomp_relay_server
// (bind_address.h, and the listener rmr::net::Serve that the master and the RMR server share). A program of its own: no
// game, no ReXGlue, no real server. The only network it touches is 127.0.0.1 and 127.0.0.2 (both loopback): it never
// binds 0.0.0.0, so Windows Firewall has nothing to ask about and nothing here is reachable from the network. Exits 0 when
// every check holds.
//
//   cmake --build <build dir> --target larecomp_bind_address_test     (CMakeLists.txt)
//   <build dir>/larecomp_bind_address_test
//
// or, with no build directory at all (Windows, llvm-mingw or clang; from this folder):
//   clang++ -std=c++23 -D_CRT_SECURE_NO_WARNINGS bind_address_test.cpp rate_my_ride/rmr_net.cpp
//           rate_my_ride/rmr_protocol.cpp -lws2_32 -o bind_address_test.exe
//   (Linux: g++ -std=c++23 -pthread bind_address_test.cpp rate_my_ride/rmr_net.cpp rate_my_ride/rmr_protocol.cpp)
//
// The three servers' own main() functions are checked on the real programs by server_bind_check.ps1 (this folder): it starts
// them on --bind 127.0.0.1 and asks the system which addresses they listen on.
//
// What it pins:
//   - the address rules: every way to write "every interface", "localhost", dotted quads (and what is refused: names,
//     IPv6, host:port, leading zeros, short or long forms, multicast, broadcast, 0.x.x.x, signs, spaces inside);
//   - the order --bind, then LARECOMP_BIND, then every interface, and that a wrong value is an error, never a fallback;
//   - a server bound to 127.0.0.1 answers a client on 127.0.0.1 and refuses a connection to 127.0.0.2 (it would answer
//     there if it listened on every interface), and the other way round for a server bound to 127.0.0.2;
//   - the default is the wildcard address the servers always used, checked on the number only (no socket on 0.0.0.0 is
//     opened);
//   - the listener's errors name the address: an address this machine does not have, a port already in use.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "bind_address.h"
#include "rate_my_ride/rmr_net.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
static void CloseSock(socket_t s) { closesocket(s); }
static const socket_t kBad = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
static void CloseSock(socket_t s) { close(s); }
static const socket_t kBad = -1;
#endif

namespace on = larecomp::online;

namespace {

int g_checks = 0;
int g_failed = 0;
int g_skipped = 0;

void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("FAILED: %s\n", what.c_str());
  }
}

// ---- the address rules -------------------------------------------------------------------------------------------

struct Parsed {
  bool ok = false;
  on::BindAddress a;
  std::string error;
};

Parsed P(const std::string& text) {
  Parsed r;
  r.ok = on::ParseBindAddress(text, &r.a, &r.error);
  return r;
}

void ExpectAddress(const std::string& in, uint32_t host_order, const char* text, bool any, bool loopback) {
  const Parsed r = P(in);
  Check(r.ok, "\"" + in + "\" is accepted (" + r.error + ")");
  if (!r.ok) return;
  Check(r.a.host_order == host_order, "\"" + in + "\" is the number " + std::to_string(host_order) + ", got " +
                                          std::to_string(r.a.host_order));
  Check(r.a.text == text, "\"" + in + "\" reads back as " + text + ", got " + r.a.text);
  Check(r.a.any == any, "\"" + in + "\" any=" + (any ? "true" : "false"));
  Check(r.a.loopback == loopback, "\"" + in + "\" loopback=" + (loopback ? "true" : "false"));
}

void ExpectRefused(const std::string& in, const char* why_contains = nullptr) {
  const Parsed r = P(in);
  Check(!r.ok, "\"" + in + "\" is refused");
  if (r.ok) return;
  Check(!r.error.empty(), "\"" + in + "\" is refused with a message");
  if (why_contains) Check(r.error.find(why_contains) != std::string::npos, "\"" + in + "\" message mentions " + why_contains + ": " + r.error);
}

void TestParse() {
  ExpectAddress("127.0.0.1", 0x7F000001u, "127.0.0.1", false, true);
  ExpectAddress("127.0.0.2", 0x7F000002u, "127.0.0.2", false, true);
  ExpectAddress("127.255.255.254", 0x7FFFFFFEu, "127.255.255.254", false, true);
  ExpectAddress("192.168.1.20", 0xC0A80114u, "192.168.1.20", false, false);
  ExpectAddress("10.0.0.5", 0x0A000005u, "10.0.0.5", false, false);
  ExpectAddress("26.24.254.19", 0x1A18FE13u, "26.24.254.19", false, false);  // a Radmin VPN address
  ExpectAddress("223.255.255.255", 0xDFFFFFFFu, "223.255.255.255", false, false);
  ExpectAddress("1.0.0.1", 0x01000001u, "1.0.0.1", false, false);
  // Every way to say "every interface".
  for (const char* w : {"0.0.0.0", "any", "all", "*", "ANY", "All"}) ExpectAddress(w, 0, "0.0.0.0", true, false);
  // localhost, in any case, is 127.0.0.1.
  for (const char* w : {"localhost", "LOCALHOST", "LocalHost"}) ExpectAddress(w, 0x7F000001u, "127.0.0.1", false, true);
  // Spaces around the value are ignored (an environment variable set with a trailing space); inside they are not.
  ExpectAddress("  127.0.0.1  ", 0x7F000001u, "127.0.0.1", false, true);
  ExpectAddress("\t127.0.0.1\r\n", 0x7F000001u, "127.0.0.1", false, true);
  ExpectRefused("127.0. 0.1");
  ExpectRefused("127 .0.0.1");

  ExpectRefused("", "empty");
  ExpectRefused("   ", "empty");
  ExpectRefused("localhosts");
  ExpectRefused("example.com");
  ExpectRefused("my-pc");
  ExpectRefused("::1", "IPv6");
  ExpectRefused("[::1]", "IPv6");
  ExpectRefused("fe80::1", "IPv6");
  ExpectRefused("127.0.0.1:31000", "host:port");
  ExpectRefused("127.0.0.1:", "host:port");
  ExpectRefused("127.0.0");
  ExpectRefused("127.0.0.1.1");
  ExpectRefused("127.0.0.1.");
  ExpectRefused(".127.0.0.1");
  ExpectRefused("127..0.1");
  ExpectRefused("127.0.0.256");
  ExpectRefused("256.0.0.1");
  ExpectRefused("999.999.999.999");
  ExpectRefused("1234.0.0.1");
  ExpectRefused("127.0.0.-1");
  ExpectRefused("+127.0.0.1");
  ExpectRefused("127.0.0.0x1");
  ExpectRefused("127.000.000.001");
  ExpectRefused("010.0.0.1");
  ExpectRefused("00.0.0.1");
  ExpectRefused("1e2.0.0.1");
  ExpectRefused("2130706433");  // 127.0.0.1 as one number: not accepted
  ExpectRefused("0x7f000001");
  // Not addresses a server can listen on.
  ExpectRefused("224.0.0.1", "multicast");
  ExpectRefused("239.255.255.250", "multicast");
  ExpectRefused("255.255.255.255", "broadcast");
  ExpectRefused("240.0.0.1");
  ExpectRefused("0.0.0.1", "0.x.x.x");
  ExpectRefused("0.1.2.3");
  // Embedded NUL: the rest is not read.
  ExpectRefused(std::string("127.0.0.1\0x", 11));
  // A refusal leaves the output alone.
  on::BindAddress keep;
  keep.host_order = 0x01020304u;
  keep.text = "kept";
  std::string error;
  Check(!on::ParseBindAddress("nope", &keep, &error) && keep.host_order == 0x01020304u && keep.text == "kept", "a refusal leaves the output as it was");
}

void TestResolve() {
  on::BindAddress a;
  std::string error;
  // Nothing given: every interface, the default the servers always had.
  Check(on::ResolveBindAddress(nullptr, nullptr, &a, &error), "nothing given resolves");
  Check(a.any && a.host_order == 0 && a.text == "0.0.0.0" && !a.loopback, "nothing given is every interface (0.0.0.0)");
  Check(a.source.find("default") != std::string::npos, "its source says default: " + a.source);
  Check(on::ResolveBindAddress(nullptr, "", &a, &error) && a.any, "an empty environment value is the same as none (set X= clears it)");
  // The environment alone.
  Check(on::ResolveBindAddress(nullptr, "127.0.0.1", &a, &error) && a.host_order == 0x7F000001u && a.loopback, "LARECOMP_BIND=127.0.0.1");
  Check(a.source == on::kBindEnvVar, "its source is LARECOMP_BIND: " + a.source);
  Check(on::ResolveBindAddress(nullptr, "localhost", &a, &error) && a.host_order == 0x7F000001u, "LARECOMP_BIND=localhost");
  Check(on::ResolveBindAddress(nullptr, " 10.1.2.3 ", &a, &error) && a.text == "10.1.2.3", "LARECOMP_BIND with spaces");
  // The option wins.
  Check(on::ResolveBindAddress("127.0.0.2", "10.1.2.3", &a, &error) && a.host_order == 0x7F000002u, "--bind beats LARECOMP_BIND");
  Check(a.source == "--bind", "its source is --bind: " + a.source);
  Check(on::ResolveBindAddress("0.0.0.0", "127.0.0.1", &a, &error) && a.any, "--bind 0.0.0.0 beats a loopback LARECOMP_BIND (every interface on purpose)");
  Check(on::ResolveBindAddress("127.0.0.1", nullptr, &a, &error) && a.loopback, "--bind alone");
  // A wrong value is an error naming where it came from, never a fallback.
  a = on::BindAddress{};
  Check(!on::ResolveBindAddress("example.com", nullptr, &a, &error), "--bind with a name is refused");
  Check(error.rfind("--bind ", 0) == 0 && error.find("example.com") != std::string::npos, "the message names --bind and the value: " + error);
  Check(!on::ResolveBindAddress("example.com", "127.0.0.1", &a, &error), "a wrong --bind is not rescued by LARECOMP_BIND");
  Check(!on::ResolveBindAddress("", "127.0.0.1", &a, &error), "an empty --bind value is refused, not read as unset");
  Check(!on::ResolveBindAddress(nullptr, "300.1.1.1", &a, &error), "a wrong LARECOMP_BIND is refused");
  Check(error.rfind(on::kBindEnvVar, 0) == 0 && error.find("300.1.1.1") != std::string::npos, "the message names LARECOMP_BIND and the value: " + error);
  Check(a.any && a.host_order == 0, "after refusals the output is still the default");
  // The real environment: variable set -> read; the option still wins.
  {
#if defined(_WIN32)
    _putenv_s(on::kBindEnvVar, "127.0.0.2");
#else
    setenv(on::kBindEnvVar, "127.0.0.2", 1);
#endif
    Check(on::ResolveBindAddress(nullptr, &a, &error) && a.host_order == 0x7F000002u, "the real environment is read (LARECOMP_BIND=127.0.0.2)");
    Check(on::ResolveBindAddress("127.0.0.1", &a, &error) && a.host_order == 0x7F000001u, "and --bind beats it");
#if defined(_WIN32)
    _putenv_s(on::kBindEnvVar, "");
#else
    unsetenv(on::kBindEnvVar);
#endif
    Check(on::ResolveBindAddress(nullptr, &a, &error) && a.any, "a cleared variable is every interface");
  }
}

// ---- real sockets, loopback only ---------------------------------------------------------------------------------

// A free TCP port on 127.0.0.1: bind to port 0, read it, close it.
uint16_t FreePort() {
  socket_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(0x7F000001u);
  a.sin_port = 0;
  uint16_t port = 0;
  if (s != kBad && bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0) {
    socklen_t len = sizeof(a);
    getsockname(s, reinterpret_cast<sockaddr*>(&a), &len);
    port = ntohs(a.sin_port);
  }
  if (s != kBad) CloseSock(s);
  return port;
}

// Can this machine bind 127.0.0.2 (the rest of 127/8 is loopback on Windows and Linux; not everywhere)?
bool HaveSecondLoopback() {
  socket_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(0x7F000002u);
  a.sin_port = 0;
  const bool ok = s != kBad && bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0;
  if (s != kBad) CloseSock(s);
  return ok;
}

// Does a TCP connection to ip:port complete within `ms`? A listening socket on loopback completes it at once; on a closed
// port Windows takes about two seconds to report the refusal, which counts as "no" here after `ms`.
bool CanConnect(uint32_t ip_host_order, uint16_t port, int ms = 500) {
  socket_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == kBad) return false;
#if defined(_WIN32)
  u_long nonblocking = 1;
  ioctlsocket(s, FIONBIO, &nonblocking);
#else
  fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(ip_host_order);
  a.sin_port = htons(port);
  bool ok = connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0;
  if (!ok) {
    fd_set wr, ex;
    FD_ZERO(&wr);
    FD_ZERO(&ex);
    FD_SET(s, &wr);
    FD_SET(s, &ex);
    timeval tv{ms / 1000, (ms % 1000) * 1000};
    if (select(static_cast<int>(s + 1), nullptr, &wr, &ex, &tv) > 0 && FD_ISSET(s, &wr) && !FD_ISSET(s, &ex)) {
      int err = 0;
      socklen_t len = sizeof(err);
      getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
      ok = err == 0;
    }
  }
  CloseSock(s);
  return ok;
}

struct Listener {
  std::atomic<bool> stop{false};
  std::atomic<bool> done{false};
  std::thread th;
  bool served = true;
  std::string error;
  uint16_t port = 0;

  Listener(uint16_t p, uint32_t bind_ipv4) : port(p) {
    th = std::thread([this, bind_ipv4] {
      rmr::net::Handler handler = [](const rmr::net::HttpRequest& req, rmr::net::HttpResponse& res) {
        const std::string body = "bound ok, you are " + req.peer.substr(0, req.peer.rfind(':'));
        res.content_type = "text/plain";
        res.body.assign(body.begin(), body.end());
      };
      served = rmr::net::Serve(port, handler, stop, &error, bind_ipv4);
      done.store(true);
    });
  }
  // Waits until it listens (a connect on its own address works) or Serve gave up.
  bool WaitUp(uint32_t ip) {
    for (int i = 0; i < 40 && !done.load(); ++i) {
      if (CanConnect(ip, port, 250)) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
  }
  // Serve returned on its own (it could not listen) within `ms`; a server that listens never returns, so this is false for it.
  bool ReturnedWithin(int ms) {
    for (int i = 0; i < ms / 20 && !done.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    return done.load();
  }
  void Stop() {
    stop.store(true);
    if (th.joinable()) th.join();
  }
  ~Listener() { Stop(); }
};

void TestListener() {
#if defined(_WIN32)
  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
  const bool second = HaveSecondLoopback();
  if (!second) {
    ++g_skipped;
    std::printf("SKIP: this machine cannot bind 127.0.0.2; the \"refuses the other loopback address\" checks are not run\n");
  }

  // A server bound to 127.0.0.1.
  {
    const uint16_t port = FreePort();
    Check(port != 0, "a free port was found");
    Listener l(port, 0x7F000001u);
    Check(l.WaitUp(0x7F000001u), "the server bound to 127.0.0.1 listens on 127.0.0.1");
    const auto r = rmr::net::Post("127.0.0.1", port, "/x", {}, 2000, 4000);
    Check(r.transport_ok && r.status == 200, "a client reaches it on 127.0.0.1 (status " + std::to_string(r.status) + " " + r.error + ")");
    const std::string body(r.body.begin(), r.body.end());
    Check(body == "bound ok, you are 127.0.0.1", "and the handler answered: " + body);
    if (second) Check(!CanConnect(0x7F000002u, port), "it does NOT answer on 127.0.0.2 (it would, listening on every interface)");
    l.Stop();
    Check(l.served, "it stopped cleanly");
  }
  // The other way round: bound to 127.0.0.2 (the positive control for the refusal above).
  if (second) {
    const uint16_t port = FreePort();
    Listener l(port, 0x7F000002u);
    Check(l.WaitUp(0x7F000002u), "the server bound to 127.0.0.2 listens on 127.0.0.2");
    const auto r = rmr::net::Post("127.0.0.2", port, "/x", {}, 2000, 4000);
    Check(r.transport_ok && r.status == 200, "a client reaches it on 127.0.0.2");
    Check(!CanConnect(0x7F000001u, port), "it does NOT answer on 127.0.0.1");
    l.Stop();
  }
  // Two servers, one port, two loopback addresses: --bind lets the same port serve two apart.
  if (second) {
    const uint16_t port = FreePort();
    Listener a(port, 0x7F000001u);
    Check(a.WaitUp(0x7F000001u), "first of two on one port");
    Listener b(port, 0x7F000002u);
    Check(b.WaitUp(0x7F000002u), "second of two on the same port, other address");
    const auto ra = rmr::net::Post("127.0.0.1", port, "/x", {}, 2000, 4000);
    const auto rb = rmr::net::Post("127.0.0.2", port, "/x", {}, 2000, 4000);
    Check(ra.transport_ok && rb.transport_ok, "both answer on their own address");
    Check(a.served && b.served, "neither failed to bind");
  }
  // An address this machine does not have: the listener says so, with the address, and returns.
  {
    const uint16_t port = FreePort();
    Listener l(port, 0xC0000201u);  // 192.0.2.1 (TEST-NET-1, documentation range: not on this machine; nothing is sent)
    Check(l.ReturnedWithin(5000), "an address that is not this machine's: Serve gives up");
    l.Stop();
    Check(!l.served, "an address that is not this machine's: Serve returns false");
    Check(l.error.find("192.0.2.1:" + std::to_string(port)) != std::string::npos, "and the message names the address and port: " + l.error);
  }
  // A port already in use on the same address.
  {
    const uint16_t port = FreePort();
    Listener first(port, 0x7F000001u);
    Check(first.WaitUp(0x7F000001u), "the first server is up");
    Listener second_one(port, 0x7F000001u);
    Check(second_one.ReturnedWithin(5000), "the same port and address again: Serve gives up");
    second_one.Stop();
    Check(!second_one.served, "the same port and address again: Serve returns false");
    Check(second_one.error.find("127.0.0.1:" + std::to_string(port)) != std::string::npos, "and the message names the address and port: " + second_one.error);
    Check(CanConnect(0x7F000001u, port), "the first server is still up");
  }
  // The wildcard default is the number the servers always listened on. Checked on the value only: this test never opens a
  // socket on 0.0.0.0.
  {
    on::BindAddress a;
    std::string error;
    Check(on::ResolveBindAddress(nullptr, nullptr, &a, &error) && htonl(a.host_order) == htonl(INADDR_ANY),
          "the default is INADDR_ANY, what the servers bound before --bind existed");
  }
}

}  // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);  // a hung check still shows how far it got
  TestParse();
  TestResolve();
  TestListener();
  std::printf("bind_address_test: %d checks, %d failed%s\n", g_checks, g_failed, g_skipped ? " (some skipped, see above)" : "");
  return g_failed ? 1 : 0;
}
