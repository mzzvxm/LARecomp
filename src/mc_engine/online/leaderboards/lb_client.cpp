#ifndef REXGLUE_HAS_XEO3_TARGET
#include "lb_client.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>

#include <rex/cvar.h>

#include "larecomp_log.h"
#include "lb_store.h"
#include "../rate_my_ride/rmr_client.h"
#include "../rate_my_ride/rmr_net.h"

REXCVAR_DEFINE_BOOL(leaderboards_enabled, true, "MCLA/Multiplayer",
                    "Leaderboards: serve the Xbox LIVE stats boards (race wins, battle modes, "
                    "race times) from our own backend, shared through the Rate My Ride "
                    "server. Off = stock behaviour (empty boards).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(leaderboards_diag, false, "MCLA/Multiplayer",
                    "Log every leaderboard read and write in detail.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace lb::client {

namespace fs = std::filesystem;

namespace {

constexpr int kConnectTimeoutMs = 3000;
constexpr int kIoTimeoutMs = 15000;
constexpr int kWriteAttempts = 3;

struct Target {
  bool remote = false;
  std::string host;
  uint16_t port = 0;
};

Target ResolveTarget() {
  Target t;
  t.remote = rmr::client::ResolveServer(t.host, t.port);
  return t;
}

std::string Key(const Target& t) { return t.host + ":" + std::to_string(t.port); }

// Servers seen answering 404 on /lb/v1 (a Rate My Ride server from before the
// leaderboards). Guarded by the worker: only it touches this.
std::string g_no_service;

std::mutex g_local_mutex;
std::unique_ptr<Store> g_local;

Store* LocalStore() {
  std::lock_guard<std::mutex> lock(g_local_mutex);
  if (!g_local) {
    auto store = std::make_unique<Store>(rmr::client::UserDataRoot() / "leaderboards");
    std::string error;
    if (!store->Open(&error)) {
      LARECOMP_APP_ERROR("[leaderboards] local store unavailable: {}", error);
      return nullptr;
    }
    LARECOMP_APP_INFO("[leaderboards] local store {} ({})", store->dir().string(), store->Summary());
    g_local = std::move(store);
  }
  return g_local.get();
}

Response RunLocal(const std::vector<uint8_t>& body, Op op) {
  Response resp;
  Store* store = LocalStore();
  if (!store) {
    resp.err = kUnknown;
    return resp;
  }
  std::string line;
  const std::vector<uint8_t> reply = store->HandleWire(body, &line);
  if (Diag()) LARECOMP_APP_INFO("[leaderboards] local: {}", line);
  if (!DecodeResponse(reply, resp)) {
    resp = Response{};
    resp.err = kUnknown;
  }
  (void)op;
  return resp;
}

Response Execute(const Target& target, const Request& req) {
  const std::vector<uint8_t> body = EncodeRequest(req);
  if (!target.remote || g_no_service == Key(target)) return RunLocal(body, req.op);

  const int attempts = req.op == Op::kWrite ? kWriteAttempts : 1;
  Response resp;
  for (int attempt = 1; attempt <= attempts; ++attempt) {
    rmr::net::PostResult r = rmr::net::Post(target.host, target.port, kHttpPath, body,
                                            kConnectTimeoutMs, kIoTimeoutMs);
    if (r.transport_ok && r.status == 404) {
      g_no_service = Key(target);
      LARECOMP_APP_WARN(
          "[leaderboards] server {} has no leaderboards (an older larecomp_rmr_server); using "
          "the local store until it is updated",
          Key(target));
      return RunLocal(body, req.op);
    }
    if (r.transport_ok && r.status == 200) {
      if (DecodeResponse(r.body, resp)) return resp;
      LARECOMP_APP_WARN("[leaderboards] {}: undecodable reply ({} bytes)", OpName(req.op),
                        r.body.size());
      resp = Response{};
      resp.err = kUnknown;
      return resp;
    }
    resp = Response{};
    resp.err = !r.transport_ok ? (r.timed_out ? kTimedOut : kConnection) : kUnknown;
    LARECOMP_APP_WARN("[leaderboards] {} {} (attempt {}/{}): {}", OpName(req.op), Key(target),
                      attempt, attempts,
                      r.transport_ok ? "HTTP " + std::to_string(r.status) : r.error);
    if (attempt < attempts) std::this_thread::sleep_for(std::chrono::seconds(2 * attempt));
  }
  return resp;
}

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
    Response resp = Execute(job.target, job.req);
    if (job.done) job.done(std::move(resp));
  }
}

}  // namespace

bool Enabled() { return REXCVAR_GET(leaderboards_enabled); }
bool Diag() { return REXCVAR_GET(leaderboards_diag); }

uint64_t PlayerOwnerKey() { return rmr::client::PlayerOwnerKey(); }

std::string DescribeTarget() {
  const Target t = ResolveTarget();
  if (t.remote) return "server " + Key(t);
  return "local store " + (rmr::client::UserDataRoot() / "leaderboards").string();
}

void Submit(Request&& req, Done done) {
  Job job{ResolveTarget(), std::move(req), std::move(done)};
  job.req.token = rmr::client::PlayerToken();
  std::lock_guard<std::mutex> lock(g_queue_mutex);
  if (!g_worker_started) {
    std::thread(WorkerMain).detach();
    g_worker_started = true;
  }
  g_queue.push_back(std::move(job));
  g_queue_cv.notify_one();
}

}  // namespace lb::client

#else  // REXGLUE_HAS_XEO3_TARGET
// XEO3 stubs: the XEO3 DLL does not carry the leaderboard backend.

#include <rex/ppc/context.h>

#endif  // REXGLUE_HAS_XEO3_TARGET
