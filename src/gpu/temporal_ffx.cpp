// MCLA GPU renderer -- the FidelityFX upscaler backend. See temporal_ffx.h.

#include "temporal_ffx.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <ffx_api.h>
#include <ffx_api_types.h>
#include <ffx_upscale.h>

#include "backend.h"
#include "device.h"
#include "ffx_common.h"
#include "frame.h"
#include "temporal_ffx_glue.h"

namespace mcla::gpu {

namespace {

constexpr const char* kPrefix = "[gpu] ffx:";
constexpr uint32_t kMaxMessageLines = 24;
// The size of the context a 4.x provider is tried with (it only has to load).
constexpr uint32_t kTrialWidth = 1280;
constexpr uint32_t kTrialHeight = 720;

// The helpers the frame generation backend uses too (ffx_common.h).
using ffx_common::ExeDirectory;
using ffx_common::FileExists;
using ffx_common::FillBackend;
using ffx_common::LoadByFullPath;
using ffx_common::MakeResource;
using ffx_common::NewerThan;
using ffx_common::ParseVersion;
using ffx_common::ReturnName;
using ffx_common::Utf8;
using ffx_common::Version;

// The runtime's messages, from whatever thread it is on.
void FfxMessage(uint32_t type, const wchar_t* message) {
  static std::atomic<uint32_t> lines{0};
  if (lines.fetch_add(1, std::memory_order_relaxed) >= kMaxMessageLines) {
    return;
  }
  if (type == FFX_API_MESSAGE_TYPE_ERROR) {
    REXLOG_ERROR("{} runtime: {}", kPrefix, Utf8(message));
  } else {
    REXLOG_WARN("{} runtime: {}", kPrefix, Utf8(message));
  }
}

// One context and the descriptions it was made with (the runtime reads them until it is destroyed).
struct Context {
  ffxContext handle = nullptr;
  BackendRequest request;
  uint64_t version_id = 0;
  std::string provider;
  ffxCreateContextDescUpscale upscale{};
  ffxCreateContextDescUpscaleVersion version{};
  ffxOverrideVersion override_version{};
  ffx_glue::BackendDesc backend;
  bool dispatched = false;  // the first dispatch of a context resets the history
  uint32_t phases = 0;      // the runtime's jitter sequence length for its sizes (0: not asked)
  uint64_t bytes = 0;       // the video memory it reports for itself
};

}  // namespace

struct FfxBackend::Impl {
  // ---- set by Probe (render or lifecycle thread), read by the worker afterwards
  Device* device = nullptr;
  FrameRing* frames = nullptr;
  Backend api = Backend::kD3D12;
  backend::NativeDevice native;
  std::wstring loader_path;
  std::wstring upscaler_path;
  bool probe_started = false;

  // ---- the runtime (worker thread only, once loaded; the render thread reads `fn` after Ready)
  HMODULE loader = nullptr;
  HMODULE upscaler = nullptr;
  PfnFfxCreateContext create = nullptr;
  PfnFfxDestroyContext destroy = nullptr;
  PfnFfxConfigure configure = nullptr;
  PfnFfxQuery query = nullptr;
  PfnFfxDispatch dispatch = nullptr;
  std::vector<Version> versions;
  uint64_t id31 = 0;
  uint64_t id4 = 0;

  // ---- shared (under `mu`)
  mutable std::mutex mu;
  std::condition_variable cv;
  std::thread worker;
  bool stop = false;
  BackendStatus status;
  bool want = false;
  BackendRequest want_request;
  std::shared_ptr<Context> ctx;  // the ready context
  // The prepared context (make before break, Prepare/Commit): made in a slot of its own while `ctx`
  // goes on working, and taken over by `ctx` at Commit.
  bool next_want = false;
  BackendRequest next_request;
  std::shared_ptr<Context> next_ctx;
  std::vector<std::shared_ptr<Context>> doomed;
  bool failed[3] = {};           // by Upscaler: a creation of that kind failed

  // ---- render thread only
  uint64_t dispatches = 0;

  void WorkerMain();
  void ProbeRuntime();
  bool LoadRuntime(std::string* why);
  bool QueryVersions(std::string* why);
  std::shared_ptr<Context> CreateContext(const BackendRequest& request, std::string* why);
  void DestroyContext(Context& context);
  void SetStatus(BackendState state, bool fsr31, bool fsr4, std::string reason);
  void FailLocked(std::unique_lock<std::mutex>& lock, Upscaler kind, bool whole,
                  const std::string& why);
  // Under `mu`: the ready context goes after the frames that may use it.
  void RetireCurrentLocked(std::vector<std::shared_ptr<Context>>* retire);
  // Under `mu`: the prepared one is not wanted any more (a context being made for it is freed by the
  // worker when it is done).
  void RetireNextLocked(std::vector<std::shared_ptr<Context>>* retire);
  // Under `mu`: a context is wanted and not there yet, for the current slot and for the prepared one.
  bool WantCurrentLocked() const {
    return want && !ctx && status.state == BackendState::kAvailable &&
           KindUsableLocked(want_request.upscaler);
  }
  bool WantNextLocked() const {
    return next_want && !next_ctx && status.state == BackendState::kAvailable &&
           KindUsableLocked(next_request.upscaler);
  }
  void RetireOutsideLock(std::vector<std::shared_ptr<Context>>& retire);
  bool KindUsableLocked(Upscaler kind) const {
    return kind == Upscaler::kFsr31 ? status.fsr31 && !failed[1]
         : kind == Upscaler::kFsr4  ? status.fsr4 && !failed[2]
                                    : false;
  }
};

FfxBackend::FfxBackend() : impl_(std::make_unique<Impl>()) {}

FfxBackend::~FfxBackend() {
  Shutdown();
}

// ---------------------------------------------------------------------------
// the probe
// ---------------------------------------------------------------------------

void FfxBackend::Impl::SetStatus(BackendState state, bool fsr31, bool fsr4, std::string reason) {
  {
    std::lock_guard lock(mu);
    status.state = state;
    status.fsr31 = fsr31;
    status.fsr4 = fsr4;
    status.reason = std::move(reason);
  }
  PublishUpscalerAvailability(state == BackendState::kAvailable || state == BackendState::kUnavailable,
                              fsr31, fsr4);
}

void FfxBackend::Probe(Device& device, FrameRing& frames) {
  Impl& i = *impl_;
  if (i.probe_started) {
    return;
  }
  i.probe_started = true;
  i.device = &device;
  i.frames = &frames;
  i.api = ActiveBackend();
  i.native = backend::NativeDeviceOf(device.device());

  // What needs no runtime: this device and the files.
  const backend::UpscalerSupport support = backend::QueryUpscalerSupport(device.device());
  if (!support.ok) {
    REXLOG_INFO("{} not offered on this device: {} ({})", kPrefix, support.reason, support.detail);
    i.SetStatus(BackendState::kUnavailable, false, false, support.reason);
    return;
  }
  const std::wstring dir = ExeDirectory();
  const wchar_t* suffix = i.api == Backend::kD3D12 ? L"dx12" : L"vk";
  i.loader_path = dir + L"amd_fidelityfx_loader_" + suffix + L".dll";
  i.upscaler_path = dir + L"amd_fidelityfx_upscaler_" + suffix + L".dll";
  if (dir.empty() || !FileExists(i.loader_path) || !FileExists(i.upscaler_path)) {
    const std::string reason = std::format(
        "AMD's FidelityFX files for {} (amd_fidelityfx_loader_{}.dll and "
        "amd_fidelityfx_upscaler_{}.dll) are not next to the game's exe",
        BackendName(i.api), Utf8(suffix), Utf8(suffix));
    REXLOG_WARN("{} the upscalers are off: {}. Run the game's setup again to get them.", kPrefix,
                reason);
    i.SetStatus(BackendState::kUnavailable, false, false, reason);
    return;
  }
  REXLOG_INFO("{} device can run FidelityFX ({}); loading the runtime on a worker thread", kPrefix,
              support.detail);
  i.SetStatus(BackendState::kProbing, false, false, {});
  i.worker = std::thread([&i] { i.WorkerMain(); });
}

bool FfxBackend::Impl::LoadRuntime(std::string* why) {
  DWORD error = 0;
  loader = LoadByFullPath(loader_path, &error);
  if (!loader) {
    *why = std::format("the FidelityFX loader could not be opened (Windows error {})", error);
    return false;
  }
  // The upscaler DLL by its full path too, so that the loader's own request for it by name meets the
  // module already loaded (CHECK: this is how the loader finds it).
  upscaler = LoadByFullPath(upscaler_path, &error);
  if (!upscaler) {
    *why = std::format("the FidelityFX upscaler could not be opened (Windows error {})", error);
    return false;
  }
  create = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(loader, "ffxCreateContext"));
  destroy = reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(loader, "ffxDestroyContext"));
  configure = reinterpret_cast<PfnFfxConfigure>(GetProcAddress(loader, "ffxConfigure"));
  query = reinterpret_cast<PfnFfxQuery>(GetProcAddress(loader, "ffxQuery"));
  dispatch = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(loader, "ffxDispatch"));
  if (!create || !destroy || !query || !dispatch) {
    *why = "the FidelityFX loader lacks the ffx functions";
    return false;
  }
  return true;
}

bool FfxBackend::Impl::QueryVersions(std::string* why) {
  ffx_glue::BackendDesc backend_desc;
  if (!FillBackend(api, native, &backend_desc)) {
    *why = "the device's native objects are not available";
    return false;
  }
  uint64_t count = 0;
  ffxQueryDescGetVersions q{};
  q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
  q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
  q.device = backend_desc.query_device;
  q.outputCount = &count;
  ffxReturnCode_t rc = query(nullptr, &q.header);
  if (rc != FFX_API_RETURN_OK || count == 0) {
    *why = std::format("the runtime lists no upscaler versions for this device ({}, {} found)",
                       ReturnName(rc), count);
    return false;
  }
  std::vector<uint64_t> ids(count);
  std::vector<const char*> names(count);
  q.versionIds = ids.data();
  q.versionNames = names.data();
  rc = query(nullptr, &q.header);
  if (rc != FFX_API_RETURN_OK) {
    *why = std::format("the runtime would not name its upscaler versions ({})", ReturnName(rc));
    return false;
  }
  versions.clear();
  for (uint64_t k = 0; k < std::min<uint64_t>(count, ids.size()); ++k) {
    Version v;
    v.id = ids[k];
    v.name = names[k] ? names[k] : "";
    ParseVersion(&v);
    versions.push_back(std::move(v));
  }
  return true;
}

void FfxBackend::Impl::ProbeRuntime() {
  std::string why;
  if (!LoadRuntime(&why)) {
    REXLOG_WARN("{} the upscalers are off: {}", kPrefix, why);
    SetStatus(BackendState::kUnavailable, false, false, why);
    return;
  }
  if (configure) {
    ffxConfigureDescGlobalDebug config{};
    config.header.type = FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG;
    config.effectId = FFX_API_EFFECT_ID_UPSCALE;
    config.fpMessage = &FfxMessage;
    config.debugLevel = FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_WARNINGS;
    const ffxReturnCode_t rc = configure(nullptr, &config.header);
    if (rc != FFX_API_RETURN_OK) {
      REXLOG_INFO("{} the runtime's message hook was not taken ({}); its messages are not logged",
                  kPrefix, ReturnName(rc));
    }
  }
  if (!QueryVersions(&why)) {
    REXLOG_WARN("{} the upscalers are off: {}", kPrefix, why);
    SetStatus(BackendState::kUnavailable, false, false, why);
    return;
  }
  // The newest provider of each family. CHECK: that the names read "major.minor.patch" somewhere
  // (the log below shows them); a version that does not parse is listed and never chosen.
  const Version* best31 = nullptr;
  const Version* best4 = nullptr;
  std::string listing;
  for (const Version& v : versions) {
    listing += std::format("{}{} (id {:#x})", listing.empty() ? "" : ", ", v.name, v.id);
    if (!v.parsed) {
      continue;
    }
    if (v.major == 3 && v.minor == 1 && (!best31 || NewerThan(v, *best31))) {
      best31 = &v;
    }
    if (v.major >= 4 && (!best4 || NewerThan(v, *best4))) {
      best4 = &v;
    }
  }
  REXLOG_INFO("{} the runtime offers these upscaler versions: {}", kPrefix, listing);
  id31 = best31 ? best31->id : 0;
  id4 = best4 ? best4->id : 0;

  bool fsr4 = false;
  if (id4) {
    // The 4.x provider is listed; does a context really get it?
    BackendRequest trial;
    trial.upscaler = Upscaler::kFsr4;
    trial.render_size[0] = trial.output_size[0] = kTrialWidth;
    trial.render_size[1] = trial.output_size[1] = kTrialHeight;
    std::string trial_why;
    std::shared_ptr<Context> context = CreateContext(trial, &trial_why);
    if (context) {
      fsr4 = true;
      DestroyContext(*context);
    } else {
      REXLOG_INFO("{} FSR 4 is not offered: {}", kPrefix, trial_why);
    }
  } else {
    REXLOG_INFO("{} FSR 4 is not offered: the runtime lists no 4.x upscaler for this device", kPrefix);
  }
  const bool fsr31 = id31 != 0;
  if (!fsr31 && !fsr4) {
    why = "the runtime offers no upscaler this build can use (see the versions above)";
    REXLOG_WARN("{} the upscalers are off: {}", kPrefix, why);
    SetStatus(BackendState::kUnavailable, false, false, why);
    return;
  }
  REXLOG_INFO("{} ready: FSR 3.1 {}, FSR 4 {}", kPrefix, fsr31 ? "listed" : "not offered",
              fsr4 ? "listed" : "not offered");
  SetStatus(BackendState::kAvailable, fsr31, fsr4, {});
}

// ---------------------------------------------------------------------------
// contexts
// ---------------------------------------------------------------------------

std::shared_ptr<Context> FfxBackend::Impl::CreateContext(const BackendRequest& request,
                                                         std::string* why) {
  const uint64_t id = request.upscaler == Upscaler::kFsr4 ? id4 : id31;
  if (!id) {
    *why = "no provider of that version";
    return {};
  }
  auto context = std::make_shared<Context>();
  context->request = request;
  context->version_id = id;
  if (!FillBackend(api, native, &context->backend)) {
    *why = "the device's native objects are not available";
    return {};
  }
  const uint32_t flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_DEPTH_INVERTED |
                         FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
  context->upscale.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
  context->upscale.flags = flags;
  context->upscale.maxRenderSize = {request.render_size[0], request.render_size[1]};
  context->upscale.maxUpscaleSize = {request.output_size[0], request.output_size[1]};
  context->upscale.fpMessage = &FfxMessage;
  context->version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
  context->version.version = FFX_UPSCALER_VERSION;
  context->override_version.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
  context->override_version.versionId = id;
  context->upscale.header.pNext = &context->version.header;
  context->version.header.pNext = context->backend.header;
  context->backend.header->pNext = &context->override_version.header;
  context->override_version.header.pNext = nullptr;

  const auto begin = std::chrono::steady_clock::now();
  const ffxReturnCode_t rc = create(&context->handle, &context->upscale.header, nullptr);
  if (rc != FFX_API_RETURN_OK || !context->handle) {
    context->handle = nullptr;
    *why = std::format("ffxCreateContext returned {} ({})", rc, ReturnName(rc));
    return {};
  }
  const double ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();

  // The provider the context really got: AMD's API falls back to an older one without a word.
  ffxQueryGetProviderVersion provider{};
  provider.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
  const ffxReturnCode_t qrc = query(&context->handle, &provider.header);
  if (qrc == FFX_API_RETURN_OK && provider.versionName) {
    context->provider = provider.versionName;
  }
  if (qrc == FFX_API_RETURN_OK && provider.versionId != id) {
    *why = std::format("the runtime made a context with provider '{}' instead of the one asked for",
                       context->provider);
    DestroyContext(*context);
    return {};
  }

  // What it costs and how many jitter phases it wants (ours is 8 at ratio 1).
  uint64_t usage_bytes = 0;
  uint64_t megabytes = 0;
  {
    FfxApiEffectMemoryUsage usage{};
    ffxQueryDescUpscaleGetGPUMemoryUsage q{};
    q.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GPU_MEMORY_USAGE;
    q.gpuMemoryUsageUpscaler = &usage;
    if (query(&context->handle, &q.header) == FFX_API_RETURN_OK) {
      usage_bytes = usage.totalUsageInBytes;
      megabytes = usage_bytes >> 20;
    }
  }
  int32_t phases = 0;
  {
    ffxQueryDescUpscaleGetJitterPhaseCount q{};
    q.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT;
    q.renderWidth = request.render_size[0];
    q.displayWidth = request.output_size[0];
    q.pOutPhaseCount = &phases;
    query(&context->handle, &q.header);
  }
  context->phases = phases > 0 ? uint32_t(phases) : 0u;
  context->bytes = usage_bytes;
  REXLOG_INFO("{} context for {} at {}x{} (output {}x{}) made in {:.0f} ms: provider '{}', about {} "
              "MB, {} jitter phases (the renderer follows that count)",
              kPrefix, UpscalerName(request.upscaler), request.render_size[0], request.render_size[1],
              request.output_size[0], request.output_size[1], ms,
              context->provider.empty() ? "?" : context->provider, megabytes, phases);
  return context;
}

void FfxBackend::Impl::DestroyContext(Context& context) {
  if (context.handle && destroy) {
    destroy(&context.handle, nullptr);
  }
  context.handle = nullptr;
}

void FfxBackend::Impl::WorkerMain() {
  ProbeRuntime();
  std::unique_lock lock(mu);
  for (;;) {
    cv.wait(lock, [this] {
      return stop || !doomed.empty() || WantCurrentLocked() || WantNextLocked();
    });
    if (stop) {
      break;
    }
    if (!doomed.empty()) {
      std::vector<std::shared_ptr<Context>> list = std::move(doomed);
      doomed.clear();
      lock.unlock();
      for (const auto& c : list) {
        DestroyContext(*c);
      }
      list.clear();
      lock.lock();
    }
    // The current slot first (the frames wait for it), then the prepared one: one context at a time.
    const bool for_current = WantCurrentLocked();
    if (for_current || WantNextLocked()) {
      const BackendRequest request = for_current ? want_request : next_request;
      lock.unlock();
      std::string why;
      std::shared_ptr<Context> made = CreateContext(request, &why);
      lock.lock();
      if (!made) {
        FailLocked(lock, request.upscaler, false, std::format("its context could not be made: {}", why));
      } else if (for_current && want && want_request == request && !ctx && !stop) {
        status.provider = made->provider;
        status.context_bytes = made->bytes;
        ctx = std::move(made);
      } else if (!for_current && next_want && next_request == request && !next_ctx && !stop) {
        next_ctx = std::move(made);
      } else {
        // The request moved on while it was made.
        doomed.push_back(std::move(made));
      }
    }
  }
  // Shutdown: what is left (the GPU is idle, the renderer drained its frames).
  std::vector<std::shared_ptr<Context>> list = std::move(doomed);
  doomed.clear();
  if (ctx) {
    list.push_back(std::move(ctx));
    ctx.reset();
  }
  if (next_ctx) {
    list.push_back(std::move(next_ctx));
    next_ctx.reset();
  }
  lock.unlock();
  for (const auto& c : list) {
    DestroyContext(*c);
  }
}

void FfxBackend::Impl::FailLocked(std::unique_lock<std::mutex>& lock, Upscaler kind, bool whole,
                                  const std::string& why) {
  // Under `mu`. One log line; the kind (or all of them) is off for the session.
  const bool first = !(whole ? status.failed : failed[size_t(kind)]);
  failed[size_t(kind)] = true;
  if (whole) {
    status.failed = true;
    status.fsr31 = false;
    status.fsr4 = false;
  } else if (kind == Upscaler::kFsr31) {
    status.fsr31 = false;
  } else if (kind == Upscaler::kFsr4) {
    status.fsr4 = false;
  }
  status.reason = why;
  // The slot that asked for this kind stops asking; a slot for another kind (the prepared context of
  // a live change, or the current one) goes on.
  if (whole || want_request.upscaler == kind) {
    want = false;
  }
  if (whole || next_request.upscaler == kind) {
    next_want = false;
  }
  if (!status.fsr31 && !status.fsr4) {
    status.state = BackendState::kUnavailable;
  }
  const bool fsr31 = status.fsr31;
  const bool fsr4 = status.fsr4;
  lock.unlock();
  if (first) {
    REXLOG_WARN("{} {} is off for this session: {}", kPrefix,
                whole ? "the upscaler" : UpscalerName(kind), why);
  }
  PublishUpscalerAvailability(true, fsr31, fsr4);
  lock.lock();
}

// ---------------------------------------------------------------------------
// the render thread's calls
// ---------------------------------------------------------------------------

BackendStatus FfxBackend::Status() const {
  std::lock_guard lock(impl_->mu);
  return impl_->status;
}

void FfxBackend::Impl::RetireCurrentLocked(std::vector<std::shared_ptr<Context>>* retire) {
  status.context_bytes = 0;
  if (ctx) {
    retire->push_back(std::move(ctx));
    ctx.reset();
  }
}

void FfxBackend::Impl::RetireNextLocked(std::vector<std::shared_ptr<Context>>* retire) {
  next_want = false;
  if (next_ctx) {
    retire->push_back(std::move(next_ctx));
    next_ctx.reset();
  }
}

void FfxBackend::Impl::RetireOutsideLock(std::vector<std::shared_ptr<Context>>& retire) {
  // The context may be named by a list the GPU has not finished: it goes to the worker once the
  // frames recorded so far are done.
  for (std::shared_ptr<Context>& old : retire) {
    if (!frames) {
      std::lock_guard lock(mu);
      doomed.push_back(std::move(old));
      cv.notify_all();
      continue;
    }
    frames->RetireAction([this, old] {
      {
        std::lock_guard lock(mu);
        doomed.push_back(old);
      }
      cv.notify_all();
    });
  }
  retire.clear();
}

void FfxBackend::Request(const BackendRequest& request) {
  Impl& i = *impl_;
  std::vector<std::shared_ptr<Context>> retire;
  {
    std::unique_lock lock(i.mu);
    if (i.status.state != BackendState::kAvailable || !i.KindUsableLocked(request.upscaler)) {
      return;
    }
    if (i.want && i.want_request == request) {
      return;
    }
    if (i.ctx && !(i.ctx->request == request)) {
      i.RetireCurrentLocked(&retire);
    }
    i.want = true;
    i.want_request = request;
  }
  i.cv.notify_all();
  i.RetireOutsideLock(retire);
}

bool FfxBackend::Ready(const BackendRequest& request) const {
  const Impl& i = *impl_;
  std::lock_guard lock(i.mu);
  return i.ctx && i.ctx->request == request && !i.status.failed;
}

uint32_t FfxBackend::JitterPhaseCount() const {
  const Impl& i = *impl_;
  std::lock_guard lock(i.mu);
  return i.ctx && !i.status.failed ? i.ctx->phases : 0u;
}

// CHECK: ffxCreateContext on the worker for the prepared slot while the render thread dispatches the
// current context (the same loader and provider, two contexts): D3D12 and Vulkan, debug layer and
// validation, and that a dispatch is not slowed while the worker makes the next one.
void FfxBackend::Prepare(const BackendRequest& request) {
  Impl& i = *impl_;
  std::vector<std::shared_ptr<Context>> retire;
  {
    std::lock_guard lock(i.mu);
    if (i.status.state != BackendState::kAvailable || !i.KindUsableLocked(request.upscaler)) {
      return;
    }
    if ((i.ctx && i.ctx->request == request) || (i.want && i.want_request == request)) {
      // The current context is, or is being made as, the one asked for: nothing to prepare beside it.
      i.RetireNextLocked(&retire);
    } else if (i.next_want && i.next_request == request) {
      return;
    } else {
      i.RetireNextLocked(&retire);  // a different prepared one goes
      i.next_want = true;
      i.next_request = request;
    }
  }
  i.cv.notify_all();
  i.RetireOutsideLock(retire);
}

bool FfxBackend::PreparedReady(const BackendRequest& request) const {
  const Impl& i = *impl_;
  std::lock_guard lock(i.mu);
  if (i.status.failed) {
    return false;
  }
  return (i.ctx && i.ctx->request == request) || (i.next_ctx && i.next_ctx->request == request);
}

bool FfxBackend::Commit(const BackendRequest& request) {
  Impl& i = *impl_;
  std::vector<std::shared_ptr<Context>> retire;
  {
    std::lock_guard lock(i.mu);
    if (i.status.failed) {
      return false;
    }
    if (i.ctx && i.ctx->request == request) {
      i.want = true;  // the current context already is it
      i.want_request = request;
      i.RetireNextLocked(&retire);
    } else if (i.next_ctx && i.next_ctx->request == request) {
      if (i.ctx) {
        retire.push_back(std::move(i.ctx));
      }
      i.ctx = std::move(i.next_ctx);
      i.next_ctx.reset();
      i.next_want = false;
      i.want = true;
      i.want_request = request;
      i.status.provider = i.ctx->provider;
      i.status.context_bytes = i.ctx->bytes;
    } else {
      return false;
    }
  }
  i.RetireOutsideLock(retire);
  return true;
}

void FfxBackend::CancelPrepared() {
  Impl& i = *impl_;
  std::vector<std::shared_ptr<Context>> retire;
  {
    std::lock_guard lock(i.mu);
    i.RetireNextLocked(&retire);
  }
  i.RetireOutsideLock(retire);
}

void FfxBackend::Release() {
  Impl& i = *impl_;
  std::vector<std::shared_ptr<Context>> retire;
  {
    std::lock_guard lock(i.mu);
    i.want = false;
    i.RetireCurrentLocked(&retire);
    i.RetireNextLocked(&retire);
  }
  i.RetireOutsideLock(retire);
}

void FfxBackend::Fail(const std::string& why) {
  // The upscaler is off for the session (one log line), the context goes after the frames that may
  // use it, and the frame in hand is plain.
  Impl& i = *impl_;
  std::vector<std::shared_ptr<Context>> retire;
  {
    std::unique_lock lock(i.mu);
    i.FailLocked(lock, Upscaler::kOff, true, why);
    i.RetireCurrentLocked(&retire);
    i.RetireNextLocked(&retire);
  }
  i.RetireOutsideLock(retire);
}

bool FfxBackend::Upscale(plume::RenderCommandList* list, const TemporalInputs& in) {
  Impl& i = *impl_;
  std::shared_ptr<Context> context;
  {
    std::lock_guard lock(i.mu);
    context = i.ctx;
  }
  const auto fail = [&](const std::string& why) {
    Fail(why);
    return false;
  };
  if (!context || !context->handle) {
    return fail("a dispatch was asked for with no context");
  }
  void* native_list = backend::NativeListHandle(list);
  if (!native_list) {
    return fail("the command list has no native object");
  }
  ffxDispatchDescUpscale d{};
  d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
  d.commandList = native_list;
  if (!MakeResource(in.color, FFX_API_RESOURCE_STATE_COMPUTE_READ, &d.color) ||
      !MakeResource(in.depth, FFX_API_RESOURCE_STATE_COMPUTE_READ, &d.depth) ||
      !MakeResource(in.motion, FFX_API_RESOURCE_STATE_COMPUTE_READ, &d.motionVectors) ||
      !MakeResource(in.output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS, &d.output)) {
    return fail("an input image has no native object or a format the runtime does not know");
  }
  // exposure stays empty: automatic exposure. The reactive mask is the layers no vector is right for
  // (upscaler_masks.h: the cars' shadow decals, particles, light volumes), in the frames that have one:
  // their history is trusted less in proportion to their opacity. The transparency and composition
  // mask is the car glass: its pixel lock is reduced (FSR's "alpha of special objects").
  // CHECK: FSR 4 (the ML provider) may ignore both masks; FSR 3.1 reads them.
  if (in.mask.texture &&
      !MakeResource(in.mask, FFX_API_RESOURCE_STATE_COMPUTE_READ, &d.reactive)) {
    return fail("the reactive mask has no native object or a format the runtime does not know");
  }
  if (in.transparency.texture &&
      !MakeResource(in.transparency, FFX_API_RESOURCE_STATE_COMPUTE_READ,
                    &d.transparencyAndComposition)) {
    return fail("the transparency mask has no native object or a format the runtime does not know");
  }
  d.jitterOffset = {in.jitter[0], in.jitter[1]};
  // The vectors are previous minus current, in render pixels (measured in game against the world
  // point seen at sample pixels), which is what FSR reads with a scale of 1: pixelSpaceMV = vector *
  // scale.
  d.motionVectorScale = {in.motion_scale[0], in.motion_scale[1]};
  d.renderSize = {in.render_size[0], in.render_size[1]};
  d.upscaleSize = {in.output_size[0], in.output_size[1]};
  d.enableSharpening = in.sharpness > 0.0f;
  d.sharpness = in.sharpness;
  d.frameTimeDelta = in.frame_time_ms;
  d.preExposure = 1.0f;
  d.reset = in.reset || !context->dispatched;
  d.cameraNear = in.camera_near;
  d.cameraFar = in.camera_far;
  d.cameraFovAngleVertical = in.camera_fov_y;
  // CHECK: the game's world unit is about a metre (a car at 64 mph covers 28.7 units a second).
  d.viewSpaceToMetersFactor = 1.0f;

  const ffxReturnCode_t rc = i.dispatch(&context->handle, &d.header);
  if (rc != FFX_API_RETURN_OK) {
    return fail(std::format("ffxDispatch returned {} ({})", rc, ReturnName(rc)));
  }
  context->dispatched = true;
  if (i.dispatches++ == 0) {
    REXLOG_INFO("{} first dispatch recorded: {}x{} to {}x{}, jitter {:+.3f}, {:+.3f}, near {:.3f}, "
                "far {:.1f}, field of view {:.2f} deg, sharpness {:.2f}, reset {}",
                kPrefix, in.render_size[0], in.render_size[1], in.output_size[0], in.output_size[1],
                in.jitter[0], in.jitter[1], in.camera_near, in.camera_far,
                double(in.camera_fov_y) * 57.29577951308232, in.sharpness, d.reset ? "yes" : "no");
  }
  return true;
}

void FfxBackend::Shutdown() {
  Impl& i = *impl_;
  {
    std::lock_guard lock(i.mu);
    i.stop = true;
    i.want = false;
    i.next_want = false;
  }
  i.cv.notify_all();
  if (i.worker.joinable()) {
    i.worker.join();
  }
  // The worker destroyed the contexts on its way out; a context the render thread still holds a
  // copy of (a retire action that never ran) is destroyed here: the GPU is idle.
  {
    std::lock_guard lock(i.mu);
    i.ctx.reset();
    i.next_ctx.reset();
  }
  if (i.upscaler) {
    // The modules stay loaded to the end of the process: the loader may hold the upscaler.
    i.upscaler = nullptr;
  }
  i.loader = nullptr;
  i.frames = nullptr;
  i.probe_started = false;
}

std::unique_ptr<TemporalBackend> MakeFfxBackend() {
  return std::make_unique<FfxBackend>();
}

}  // namespace mcla::gpu
