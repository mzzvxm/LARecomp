//
// larecomp_rmr_server: the shared Rate My Ride and leaderboards server.
//
// Run it on the same machine as the online relay. Players with online enabled
// (mcla_online_enable) reach it at the relay's address on TCP 31002, so they
// rate and buy each other's rides; players without a server fall back to a
// local store on their own machine.
//
//   larecomp_rmr_server [--port 31002] [--data <dir>]
//
// <dir> (default: rmr_data next to the executable) holds rmr.db plus the
// uploaded files, and survives restarts. Open TCP 31002 inbound on the host's
// firewall, exactly like UDP 31000 for the relay.
//
// GET  /rmr/v1  -> plain-text status (handy from a browser)
// POST /rmr/v1  -> one game request (binary, see rmr_protocol.h)
// GET  /lb/v1   -> leaderboards status
// POST /lb/v1   -> one leaderboard request (binary, see leaderboards/lb_protocol.h)
//
// The leaderboards live in <dir>/leaderboards/leaderboards.db.
//

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>

#include "../leaderboards/lb_protocol.h"
#include "../leaderboards/lb_store.h"
#include "rmr_net.h"
#include "rmr_protocol.h"
#include "rmr_store.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

std::atomic<bool> g_stop{false};

void OnSignal(int) { g_stop.store(true); }

void Log(const std::string& msg) {
  char stamp[32];
  const std::time_t t = std::time(nullptr);
  std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
  std::printf("[%s] %s\n", stamp, msg.c_str());
  std::fflush(stdout);
}

std::filesystem::path ExeDir(const char* argv0) {
#if defined(_WIN32)
  wchar_t buf[MAX_PATH];
  const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
  if (n > 0 && n < MAX_PATH) return std::filesystem::path(buf).parent_path();
#endif
  std::error_code ec;
  auto p = std::filesystem::absolute(argv0, ec);
  return ec ? std::filesystem::current_path() : p.parent_path();
}

}  // namespace

int main(int argc, char** argv) {
  uint16_t port = rmr::kDefaultPort;
  std::filesystem::path data = ExeDir(argv[0]) / "rmr_data";

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if ((arg == "--port" || arg == "-p") && i + 1 < argc) {
      const int p = std::atoi(argv[++i]);
      if (p <= 0 || p > 65535) {
        std::fprintf(stderr, "invalid port '%s'\n", argv[i]);
        return 2;
      }
      port = static_cast<uint16_t>(p);
    } else if ((arg == "--data" || arg == "-d") && i + 1 < argc) {
      data = argv[++i];
    } else if (arg == "--help" || arg == "-h") {
      std::printf("usage: %s [--port %u] [--data <dir>]\n", argv[0], rmr::kDefaultPort);
      return 0;
    } else {
      std::fprintf(stderr, "unknown argument '%s' (try --help)\n", arg.c_str());
      return 2;
    }
  }

  std::signal(SIGINT, OnSignal);
  std::signal(SIGTERM, OnSignal);

  rmr::Store store(data);
  std::string error;
  if (!store.Open(&error)) {
    std::fprintf(stderr, "cannot open the ride database: %s\n", error.c_str());
    return 1;
  }
  lb::Store boards(data / "leaderboards");
  if (!boards.Open(&error)) {
    std::fprintf(stderr, "cannot open the leaderboards: %s\n", error.c_str());
    return 1;
  }
  Log("Rate My Ride server on TCP " + std::to_string(port) + ", data in " + data.string());
  Log(store.Summary());
  Log(boards.Summary());

  auto handler = [&store, &boards](const rmr::net::HttpRequest& req, rmr::net::HttpResponse& resp) {
    if (req.path == lb::kHttpPath) {
      if (req.method == "GET") {
        const std::string text = "Leaderboards: " + boards.Summary() + "\n";
        resp.content_type = "text/plain; charset=utf-8";
        resp.body.assign(text.begin(), text.end());
        return;
      }
      if (req.method != "POST") {
        resp.status = 405;
        return;
      }
      std::string line;
      resp.body = boards.HandleWire(req.body, &line);
      Log(req.peer + "  lb " + line);
      return;
    }
    if (req.path != rmr::kHttpPath) {
      resp.status = 404;
      return;
    }
    if (req.method == "GET") {
      const std::string text = "Rate My Ride server: " + store.Summary() + "\n";
      resp.content_type = "text/plain; charset=utf-8";
      resp.body.assign(text.begin(), text.end());
      return;
    }
    if (req.method != "POST") {
      resp.status = 405;
      return;
    }
    std::string line;
    resp.body = store.HandleWire(req.body, &line);
    Log(req.peer + "  " + line);
  };

  if (!rmr::net::Serve(port, handler, g_stop, &error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }
  Log("stopped (" + store.Summary() + "; " + boards.Summary() + ")");
  return 0;
}
