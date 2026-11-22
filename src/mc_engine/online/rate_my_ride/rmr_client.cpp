#ifndef REXGLUE_HAS_XEO3_TARGET
#include "rmr_client.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <random>
#include <thread>

#include "settings/larecomp_settings.h"
#include <rex/kernel/title_runtime.h>
#include <rex/xam/content_manager.h>
#include <rex/xam/user_profile.h>
#include <rex/path_settings.h>

#include "larecomp_log.h"
#include "../identity/identity_store.h"
#include "rmr_net.h"
#include "rmr_store.h"
#include "../leaderboards/lb_client.h"

// mcla_online_enable / mcla_online_server belong to the fork SDK's online
// client (src/kernel/xam/mcla_online.cpp) and are read by name: declaring them
// would leave an unresolved external on the stock SDK, which has no online
// client. There they read as absent, online counts as off, and Rate My Ride
// uses rmr_server or the local store.

namespace rmr::client {

namespace fs = std::filesystem;

namespace {

struct Target {
  bool remote = false;
  std::string host;
  uint16_t port = 0;
};

fs::path UserRoot() {
  auto* rt = REX_RUNTIME();
  fs::path root = rt ? rex::GetPathSettings().user_root.Get() : fs::path{};
  if (root.empty()) {
    std::error_code ec;
    root = fs::current_path(ec);
  }
  return root;
}

// The profile directory the identity key sits in, named the way the SDK names the profile's content directory
// (IContentManager::ProfileDirName: the sanitized user_name, from the user_name setting).
std::string ProfileName() {
  auto* rt = REX_RUNTIME();
  std::string name = rt ? rt->GetContentManager().ProfileDirName(rt->GetUserProfile().GetXUID())
                        : rex::xam::UserProfile::ConfiguredName();
  return name.empty() ? std::string("default") : name;
}

Target ResolveTarget() {
  Target t;
  int cfg_port = larecomp::settings::rmr_server_port.Get();
  uint16_t port = (cfg_port > 0 && cfg_port < 65536) ? static_cast<uint16_t>(cfg_port)
                                                     : rmr::kDefaultPort;
  const std::string server = larecomp::settings::rmr_server.Get();
  if (server == "local") return t;
  if (!server.empty()) {
    net::SplitHostPort(server, t.host, port);
    t.remote = !t.host.empty();
    t.port = port;
    return t;
  }
  if (larecomp::cvar::GetFlagByName("mcla_online_enable") == "true") {
    uint16_t relay_port = 0;  // the relay's own UDP port is not ours
    net::SplitHostPort(larecomp::cvar::GetFlagByName("mcla_online_server"), t.host, relay_port);
    t.remote = !t.host.empty();
    t.port = port;
  }
  return t;
}

std::string Describe(const Target& t) {
  if (t.remote) return "server " + t.host + ":" + std::to_string(t.port);
  return "local store " + (UserRoot() / "ratemyride").string();
}

// ------------------------------------------------------------ local -------

std::mutex g_local_mutex;
std::unique_ptr<Store> g_local;

Store* LocalStore() {
  std::lock_guard<std::mutex> lock(g_local_mutex);
  if (!g_local) {
    auto store = std::make_unique<Store>(UserRoot() / "ratemyride");
    std::string error;
    if (!store->Open(&error)) {
      LARECOMP_APP_ERROR("[rmr] local store unavailable: {}", error);
      return nullptr;
    }
    LARECOMP_APP_INFO("[rmr] local store {} ({})", store->dir().string(), store->Summary());
    store->AttachBoards(lb::client::LocalStoreForAwards());
    g_local = std::move(store);
  }
  return g_local.get();
}

Response Execute(const Target& target, const Request& req) {
  Response resp;
  const std::vector<uint8_t> body = EncodeRequest(req);
  std::vector<uint8_t> reply;
  if (target.remote) {
    net::PostResult r = net::Post(target.host, target.port, kHttpPath, body, net::kConnectTimeoutMs,
                                  net::kIoTimeoutMs);
    if (!r.transport_ok) {
      LARECOMP_APP_WARN("[rmr] {} {}:{}: {}", OpName(req.op), target.host, target.port, r.error);
      resp.err = r.timed_out ? kTimedOut : kConnection;
      return resp;
    }
    if (r.status != 200) {
      LARECOMP_APP_WARN("[rmr] {} {}:{}: HTTP {}", OpName(req.op), target.host, target.port,
                        r.status);
      resp.err = kUnknown;
      return resp;
    }
    reply = std::move(r.body);
  } else {
    Store* store = LocalStore();
    if (!store) {
      resp.err = kUnknown;
      return resp;
    }
    std::string line;
    reply = store->HandleWire(body, &line);
    if (Diag()) LARECOMP_APP_INFO("[rmr] local: {}", line);
  }
  if (!DecodeResponse(reply, resp)) {
    LARECOMP_APP_WARN("[rmr] {}: undecodable reply ({} bytes)", OpName(req.op), reply.size());
    resp = Response{};
    resp.err = kUnknown;
  }
  return resp;
}

// ------------------------------------------------------------ worker ------

struct Job {
  Target target;
  Request req;
  Done done;
};

std::mutex g_queue_mutex;
std::condition_variable g_queue_cv;
std::deque<Job> g_queue;
bool g_worker_started = false;

void WorkerMain() {
  for (;;) {
    Job job;
    {
      std::unique_lock<std::mutex> lock(g_queue_mutex);
      g_queue_cv.wait(lock, [] { return !g_queue.empty(); });
      job = std::move(g_queue.front());
      g_queue.pop_front();
    }
    job.done(Execute(job.target, job.req));
  }
}

// ------------------------------------------------------------ identity ----

std::mutex g_token_mutex;
std::string g_token_profile;
Token g_token{};

bool ParseHex(const std::string& hex, Token& out) {
  if (hex.size() != out.size() * 2) return false;
  for (size_t i = 0; i < out.size(); ++i) {
    unsigned v = 0;
    for (int k = 0; k < 2; ++k) {
      const char c = hex[i * 2 + k];
      v <<= 4;
      if (c >= '0' && c <= '9') v |= c - '0';
      else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
      else return false;
    }
    out[i] = static_cast<uint8_t>(v);
  }
  return true;
}

}  // namespace

bool Enabled() { return larecomp::settings::rmr_enabled.Get(); }
bool Diag() { return larecomp::settings::rmr_diag.Get(); }

bool ResolveServer(std::string& host, uint16_t& port) {
  const Target t = ResolveTarget();
  host = t.host;
  port = t.port;
  return t.remote;
}

fs::path UserDataRoot() { return UserRoot(); }

Token PlayerToken() {
  std::lock_guard<std::mutex> lock(g_token_mutex);
  const std::string profile = ProfileName();
  if (profile == g_token_profile) return g_token;

  // The token lives with the online identity, outside the save folder
  // (identity_store.h). Older builds kept it in the save folder: taken from
  // there once, then removed, so sharing a save no longer shares it.
  if (auto kept = larecomp::identity::RmrToken()) {
    g_token = *kept;
    g_token_profile = profile;
    return g_token;
  }

  const fs::path dir = UserRoot() / profile / "ratemyride";
  const fs::path file = dir / "identity.key";
  Token token{};
  bool ok = false;
  {
    std::ifstream f(file);
    std::string hex;
    ok = f && (f >> hex) && ParseHex(hex, token);
  }
  if (larecomp::identity::Loaded()) {
    if (!ok) {
      std::random_device rd;
      for (auto& b : token) b = static_cast<uint8_t>(rd());
    }
    larecomp::identity::SetRmrToken(token);
    if (larecomp::identity::RmrToken()) {
      std::error_code ec;
      if (ok) fs::remove(file, ec);
      LARECOMP_APP_INFO("[rmr] Rate My Ride identity for '{}' {} the online identity", profile,
                        ok ? "moved out of the save folder into" : "created with");
      g_token = token;
      g_token_profile = profile;
      return token;
    }
  }
  if (!ok) {
    std::random_device rd;
    std::mt19937_64 mix(static_cast<uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count()));
    for (auto& b : token) b = static_cast<uint8_t>(rd() ^ mix());
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::ofstream f(file, std::ios::trunc);
    static const char* kHex = "0123456789abcdef";
    for (uint8_t b : token) f << kHex[b >> 4] << kHex[b & 15];
    f << "\n";
    if (f) {
      LARECOMP_APP_INFO("[rmr] new Rate My Ride identity for '{}' in {}", profile, file.string());
    } else {
      LARECOMP_APP_WARN("[rmr] cannot save the Rate My Ride identity to {}; it will change "
                        "next launch",
                        file.string());
    }
  }
  g_token = token;
  g_token_profile = profile;
  return token;
}

uint64_t PlayerOwnerKey() { return OwnerKeyFromToken(PlayerToken()); }

std::string DescribeTarget() { return Describe(ResolveTarget()); }

void Submit(Request&& req, Done done) {
  Job job{ResolveTarget(), std::move(req), std::move(done)};
  job.req.token = PlayerToken();
  std::lock_guard<std::mutex> lock(g_queue_mutex);
  if (!g_worker_started) {
    std::thread(WorkerMain).detach();
    g_worker_started = true;
  }
  g_queue.push_back(std::move(job));
  g_queue_cv.notify_one();
}

}  // namespace rmr::client

#else  // REXGLUE_HAS_XEO3_TARGET
// XEO3 stubs: the XEO3 DLL does not carry the Rate My Ride backend.

#include <rex/ppc/context.h>

#endif  // REXGLUE_HAS_XEO3_TARGET
