#include "rmr_net.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "rmr_protocol.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace rmr::net {

namespace {

#if defined(_WIN32)
using socket_t = SOCKET;
constexpr socket_t kBadSocket = INVALID_SOCKET;
void CloseSocket(socket_t s) { closesocket(s); }
int LastError() { return WSAGetLastError(); }
bool WouldBlock(int e) { return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS; }
#else
using socket_t = int;
constexpr socket_t kBadSocket = -1;
void CloseSocket(socket_t s) { close(s); }
int LastError() { return errno; }
bool WouldBlock(int e) { return e == EINPROGRESS || e == EWOULDBLOCK || e == EAGAIN; }
#endif

constexpr size_t kMaxHeaderBytes = 16 * 1024;

bool EnsureStartup() {
#if defined(_WIN32)
  static std::once_flag once;
  static bool ok = false;
  std::call_once(once, [] {
    WSADATA wsa;
    ok = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
  });
  return ok;
#else
  return true;
#endif
}

void SetBlocking(socket_t s, bool blocking) {
#if defined(_WIN32)
  u_long mode = blocking ? 0 : 1;
  ioctlsocket(s, FIONBIO, &mode);
#else
  int flags = fcntl(s, F_GETFL, 0);
  fcntl(s, F_SETFL, blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK));
#endif
}

void SetIoTimeouts(socket_t s, int ms) {
#if defined(_WIN32)
  DWORD t = static_cast<DWORD>(ms);
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&t), sizeof(t));
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&t), sizeof(t));
#else
  timeval tv{ms / 1000, (ms % 1000) * 1000};
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
  int one = 1;
  setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
}

bool SendAll(socket_t s, const uint8_t* data, size_t size) {
  while (size > 0) {
    const int chunk = static_cast<int>(std::min<size_t>(size, 64 * 1024));
    const int n = send(s, reinterpret_cast<const char*>(data), chunk, 0);
    if (n <= 0) return false;
    data += n;
    size -= static_cast<size_t>(n);
  }
  return true;
}

std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// Reads one HTTP message (request or response) into `head` (start line +
// headers, without the blank line) and `body`. `max_body` bounds the
// Content-Length the peer may announce. Without Content-Length (responses
// only), reads until the peer closes.
bool ReadMessage(socket_t s, std::string& head, std::vector<uint8_t>& body, size_t max_body,
                 bool allow_until_close, bool& timed_out) {
  std::string buf;
  size_t header_end = std::string::npos;
  char chunk[8192];
  timed_out = false;
  while (header_end == std::string::npos) {
    const int n = recv(s, chunk, sizeof(chunk), 0);
    if (n <= 0) {
      timed_out = n < 0;
      return false;
    }
    buf.append(chunk, static_cast<size_t>(n));
    header_end = buf.find("\r\n\r\n");
    if (header_end == std::string::npos && buf.size() > kMaxHeaderBytes) return false;
  }
  head = buf.substr(0, header_end);
  body.assign(buf.begin() + static_cast<std::ptrdiff_t>(header_end + 4), buf.end());

  const std::string lower = Lower(head);
  const size_t cl = lower.find("\r\ncontent-length:");
  if (cl == std::string::npos) {
    if (!allow_until_close) return body.empty();
    for (;;) {
      const int n = recv(s, chunk, sizeof(chunk), 0);
      if (n == 0) return true;
      if (n < 0) {
        timed_out = true;
        return false;
      }
      body.insert(body.end(), chunk, chunk + n);
      if (body.size() > max_body) return false;
    }
  }
  const unsigned long long want = std::strtoull(lower.c_str() + cl + 17, nullptr, 10);
  if (want > max_body) return false;
  while (body.size() < want) {
    const int n = recv(s, chunk, sizeof(chunk), 0);
    if (n <= 0) {
      timed_out = n < 0;
      return false;
    }
    body.insert(body.end(), chunk, chunk + n);
  }
  body.resize(static_cast<size_t>(want));
  return true;
}

const char* StatusText(int status) {
  switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Payload Too Large";
    default: return "Internal Server Error";
  }
}

}  // namespace

void SplitHostPort(const std::string& in, std::string& host, uint16_t& port) {
  host = in;
  while (!host.empty() && std::isspace(static_cast<unsigned char>(host.back()))) host.pop_back();
  while (!host.empty() && std::isspace(static_cast<unsigned char>(host.front()))) host.erase(0, 1);
  if (host.rfind("http://", 0) == 0) host = host.substr(7);
  const size_t slash = host.find('/');
  if (slash != std::string::npos) host.resize(slash);
  const size_t colon = host.rfind(':');
  if (colon != std::string::npos) {
    const int p = std::atoi(host.c_str() + colon + 1);
    if (p > 0 && p < 65536) port = static_cast<uint16_t>(p);
    host.resize(colon);
  }
}

PostResult Post(const std::string& host, uint16_t port, const std::string& path,
                const std::vector<uint8_t>& body, int connect_timeout_ms, int io_timeout_ms) {
  PostResult res;
  if (!EnsureStartup()) {
    res.error = "socket startup failed";
    return res;
  }

  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  addrinfo* ai = nullptr;
  const std::string port_str = std::to_string(port);
  if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &ai) != 0 || !ai) {
    res.error = "cannot resolve '" + host + "'";
    return res;
  }

  socket_t s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
  if (s == kBadSocket) {
    freeaddrinfo(ai);
    res.error = "socket() failed";
    return res;
  }

  // Non-blocking connect so an unreachable server costs connect_timeout_ms,
  // not the OS default of ~20 s.
  SetBlocking(s, false);
  int rc = connect(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen));
  freeaddrinfo(ai);
  if (rc != 0) {
    if (!WouldBlock(LastError())) {
      CloseSocket(s);
      res.error = "connect failed (" + std::to_string(LastError()) + ")";
      return res;
    }
    fd_set wr, ex;
    FD_ZERO(&wr);
    FD_ZERO(&ex);
    FD_SET(s, &wr);
    FD_SET(s, &ex);
    timeval tv{connect_timeout_ms / 1000, (connect_timeout_ms % 1000) * 1000};
    rc = select(static_cast<int>(s + 1), nullptr, &wr, &ex, &tv);
    int so_error = 0;
    socklen_t len = sizeof(so_error);
    getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_error), &len);
    if (rc <= 0 || so_error != 0 || !FD_ISSET(s, &wr)) {
      CloseSocket(s);
      res.timed_out = rc == 0;
      res.error = rc == 0 ? "connect timed out" : "connection refused";
      return res;
    }
  }
  SetBlocking(s, true);
  SetIoTimeouts(s, io_timeout_ms);

  std::string head = "POST " + path + " HTTP/1.1\r\nHost: " + host + ":" + port_str +
                     "\r\nUser-Agent: larecomp-rmr\r\nContent-Type: application/octet-stream"
                     "\r\nContent-Length: " + std::to_string(body.size()) +
                     "\r\nConnection: close\r\n\r\n";
  if (!SendAll(s, reinterpret_cast<const uint8_t*>(head.data()), head.size()) ||
      !SendAll(s, body.data(), body.size())) {
    CloseSocket(s);
    res.error = "send failed";
    return res;
  }

  std::string resp_head;
  bool timed_out = false;
  const bool ok = ReadMessage(s, resp_head, res.body, kMaxRequestBytes, true, timed_out);
  CloseSocket(s);
  if (!ok) {
    res.timed_out = timed_out;
    res.error = timed_out ? "response timed out" : "bad or truncated response";
    return res;
  }
  // "HTTP/1.1 200 OK"
  const size_t sp = resp_head.find(' ');
  res.status = sp == std::string::npos ? 0 : std::atoi(resp_head.c_str() + sp + 1);
  res.transport_ok = res.status > 0;
  if (!res.transport_ok) res.error = "not an HTTP response";
  return res;
}

bool Serve(uint16_t port, const Handler& handler, const std::atomic<bool>& stop,
           std::string* error) {
  if (!EnsureStartup()) {
    if (error) *error = "socket startup failed";
    return false;
  }
  socket_t ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (ls == kBadSocket) {
    if (error) *error = "socket() failed";
    return false;
  }
#if !defined(_WIN32)
  int one = 1;
  setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#endif
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(ls, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(ls, 16) != 0) {
    if (error) *error = "cannot listen on TCP port " + std::to_string(port) + " (" +
                        std::to_string(LastError()) + ")";
    CloseSocket(ls);
    return false;
  }

  while (!stop.load()) {
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(ls, &rd);
    timeval tv{1, 0};
    if (select(static_cast<int>(ls + 1), &rd, nullptr, nullptr, &tv) <= 0) continue;

    sockaddr_in peer{};
    socklen_t plen = sizeof(peer);
    socket_t cs = accept(ls, reinterpret_cast<sockaddr*>(&peer), &plen);
    if (cs == kBadSocket) continue;
    SetIoTimeouts(cs, 5000);

    HttpRequest req;
    char ip[64] = {};
    inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
    req.peer = std::string(ip) + ":" + std::to_string(ntohs(peer.sin_port));

    HttpResponse resp;
    std::string head;
    bool timed_out = false;
    if (!ReadMessage(cs, head, req.body, kMaxRequestBytes, false, timed_out)) {
      resp.status = 400;
    } else {
      // "POST /rmr/v1 HTTP/1.1"
      const size_t a = head.find(' ');
      const size_t b = a == std::string::npos ? a : head.find(' ', a + 1);
      if (a == std::string::npos || b == std::string::npos) {
        resp.status = 400;
      } else {
        req.method = head.substr(0, a);
        req.path = head.substr(a + 1, b - a - 1);
        handler(req, resp);
      }
    }
    std::string out = "HTTP/1.1 " + std::to_string(resp.status) + " " + StatusText(resp.status) +
                      "\r\nContent-Type: " + resp.content_type +
                      "\r\nContent-Length: " + std::to_string(resp.body.size()) +
                      "\r\nConnection: close\r\n\r\n";
    SendAll(cs, reinterpret_cast<const uint8_t*>(out.data()), out.size());
    SendAll(cs, resp.body.data(), resp.body.size());
#if defined(_WIN32)
    shutdown(cs, SD_SEND);
#else
    shutdown(cs, SHUT_WR);
#endif
    CloseSocket(cs);
  }
  CloseSocket(ls);
  return true;
}

}  // namespace rmr::net
