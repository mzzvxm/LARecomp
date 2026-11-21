#pragma once
//
// Minimal HTTP/1.1 over plain sockets for Rate My Ride: one POST per
// connection, Content-Length bodies, no TLS, no keep-alive. Enough for the
// game's few small requests, and plain HTTP keeps the server reachable from a
// browser or curl (GET /rmr/v1 reports its status) and ready for a web front
// end later.
//

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace rmr::net {

// The game's own timeouts for one request to the Rate My Ride / leaderboards server (rmr_client.cpp and lb_client.cpp use
// them; larecomp_online_tool asks with the same).
inline constexpr int kConnectTimeoutMs = 3000;
inline constexpr int kIoTimeoutMs = 15000;

struct PostResult {
  bool transport_ok = false;  // connected, sent, and got a complete response
  bool timed_out = false;
  int status = 0;             // HTTP status code
  std::vector<uint8_t> body;
  std::string error;          // human-readable transport failure
};

// Splits "host[:port]"; keeps `port` when none is given.
void SplitHostPort(const std::string& in, std::string& host, uint16_t& port);

PostResult Post(const std::string& host, uint16_t port, const std::string& path,
                const std::vector<uint8_t>& body, int connect_timeout_ms, int io_timeout_ms);

struct HttpRequest {
  std::string method;
  std::string path;
  std::string peer;  // "a.b.c.d:port"
  std::vector<uint8_t> body;
};

struct HttpResponse {
  int status = 200;
  std::string content_type = "application/octet-stream";
  std::vector<uint8_t> body;
};

using Handler = std::function<void(const HttpRequest&, HttpResponse&)>;

// Serves until `stop` turns true (checked about once a second). Connections are
// handled one at a time. Returns false when the port cannot be bound.
// `bind_ipv4` is the local IPv4 address to listen on as a number (127.0.0.1 =
// 0x7F000001, BindAddress::host_order); 0 (the default) is every interface.
// 127.0.0.1 keeps the server on this machine (the --bind option of the
// servers, ../bind_address.h).
bool Serve(uint16_t port, const Handler& handler, const std::atomic<bool>& stop,
           std::string* error, uint32_t bind_ipv4 = 0);

}  // namespace rmr::net
