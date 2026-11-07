// MCLA GPU renderer -- AMD's FSR 3.1 frame generation through the FidelityFX API. See
// frame_generation_ffx.h.
//
// The loading, the version query and the context's creation follow the upscaler backend
// (temporal_ffx.cpp) step for step; what differs is the effect (frame generation), that one context
// is current at a time (a change of size retires it and makes another), and the per-frame calls.

#include "frame_generation_ffx.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#include <ffx_api.h>
#include <ffx_api_types.h>
#include <ffx_framegeneration.h>

#include "backend.h"
#include "device.h"
#include "ffx_common.h"
#include "frame.h"
#include "temporal_ffx_glue.h"

namespace mcla::gpu {

namespace {

constexpr const char* kPrefix = "[gpu] frame generation (ffx):";
constexpr uint32_t kMaxMessageLines = 24;
// The size of the context made once to see that the provider really loads here.
constexpr uint32_t kTrialWidth = 1280;
constexpr uint32_t kTrialHeight = 720;

using ffx_common::FileExists;
using ffx_common::MakeResource;
using ffx_common::ReturnName;
using ffx_common::Utf8;

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
  FgContextRequest request;
  std::string provider;
  ffxCreateContextDescFrameGeneration create{};
  ffxCreateContextDescFrameGenerationVersion version{};
  ffxOverrideVersion override_version{};
  ffx_glue::BackendDesc backend;
  bool dispatched = false;  // the first generation of a context starts the history
  uint64_t bytes = 0;       // the video memory it reports for itself
  bool held = false;        // a context the renderer asked for (not the probe's trial): its bytes
                            // are told to the device as given back when it is destroyed
};

}  // namespace

class FfxFrameGeneration final : public FrameGenerationBackend {
 public:
  FfxFrameGeneration() = default;
  ~FfxFrameGeneration() override { Shutdown(); }

  void Probe(Device& device, FrameRing& frames) override;
  FgBackendStatus Status() const override {
    std::lock_guard lock(mu_);
    return status_;
  }
  void Request(const FgContextRequest& request) override;
  bool Ready(const FgContextRequest& request) const override {
    std::lock_guard lock(mu_);
    return ctx_ && ctx_->request == request && !status_.failed;
  }
  uint64_t EstimateContextBytes(const FgContextRequest& request) const override {
    std::lock_guard lock(mu_);
    if (measured_bytes_ == 0 || measured_pixels_ == 0) {
      return 0;
    }
    // Linear in the generated frame's pixels from the last measurement: the part of a context that
    // does not grow with the size is counted again at each size above it, so where the two differ
    // the answer is the larger one.
    const uint64_t pixels = uint64_t(request.generation_size[0]) * request.generation_size[1];
    return measured_bytes_ * pixels / measured_pixels_;
  }
  void Release() override;
  void Fail(const std::string& why) override;
  bool Generate(plume::RenderCommandList* list, const FgInputs& in) override;
  void Shutdown() override;

 private:
  void WorkerMain();
  void ProbeRuntime();
  bool LoadRuntime(std::string* why);
  bool QueryVersions(std::string* why);
  // `out_of_memory` (when given) is set when the runtime refused for want of video memory.
  std::shared_ptr<Context> CreateContext(const FgContextRequest& request, std::string* why,
                                         bool* out_of_memory = nullptr);
  void DestroyContext(Context& context);
  void SetStatus(FgBackendState state, std::string reason);
  // Under `mu_`: the ready context goes after the frames that may use it.
  void RetireCurrentLocked(std::vector<std::shared_ptr<Context>>* retire);
  void RetireOutside(std::vector<std::shared_ptr<Context>>& retire);
  bool WantLocked() const {
    return want_ && !ctx_ && status_.state == FgBackendState::kAvailable && !status_.failed;
  }

  // Set by Probe, read by the worker afterwards.
  Device* device_ = nullptr;
  FrameRing* frames_ = nullptr;
  Backend api_ = Backend::kD3D12;
  backend::NativeDevice native_;
  std::wstring loader_path_;
  std::wstring effect_path_;
  bool probe_started_ = false;

  // The runtime (worker thread only, once loaded; the render thread reads the functions after Ready).
  HMODULE loader_ = nullptr;
  HMODULE effect_ = nullptr;
  PfnFfxCreateContext create_ = nullptr;
  PfnFfxDestroyContext destroy_ = nullptr;
  PfnFfxConfigure configure_ = nullptr;
  PfnFfxQuery query_ = nullptr;
  PfnFfxDispatch dispatch_ = nullptr;
  uint64_t id31_ = 0;
  std::string name31_;

  // Shared, under `mu_`.
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::thread worker_;
  bool stop_ = false;
  FgBackendStatus status_;
  // What the last context the runtime reported on took, and the pixels of its generated frame.
  uint64_t measured_bytes_ = 0;
  uint64_t measured_pixels_ = 0;
  bool want_ = false;
  FgContextRequest want_request_;
  std::shared_ptr<Context> ctx_;
  std::vector<std::shared_ptr<Context>> doomed_;

  // Render thread only.
  uint64_t generations_ = 0;
};

// ---------------------------------------------------------------------------
// the probe
// ---------------------------------------------------------------------------

void FfxFrameGeneration::SetStatus(FgBackendState state, std::string reason) {
  std::lock_guard lock(mu_);
  status_.state = state;
  status_.reason = std::move(reason);
}

void FfxFrameGeneration::Probe(Device& device, FrameRing& frames) {
  if (probe_started_) {
    return;
  }
  probe_started_ = true;
  device_ = &device;
  frames_ = &frames;
  api_ = ActiveBackend();
  native_ = backend::NativeDeviceOf(device.device());

  // What needs no runtime: this device and the files.
  const backend::UpscalerSupport support = backend::QueryUpscalerSupport(device.device());
  if (!support.ok) {
    REXLOG_INFO("{} not offered on this device: {} ({})", kPrefix, support.reason, support.detail);
    SetStatus(FgBackendState::kUnavailable, support.reason);
    return;
  }
  const std::wstring dir = ffx_common::ExeDirectory();
  const wchar_t* suffix = api_ == Backend::kD3D12 ? L"dx12" : L"vk";
  loader_path_ = dir + L"amd_fidelityfx_loader_" + suffix + L".dll";
  effect_path_ = dir + L"amd_fidelityfx_framegeneration_" + suffix + L".dll";
  if (dir.empty() || !FileExists(loader_path_) || !FileExists(effect_path_)) {
    const std::string reason = std::format(
        "AMD's FidelityFX frame generation files for {} (amd_fidelityfx_loader_{}.dll and "
        "amd_fidelityfx_framegeneration_{}.dll) are not next to the game's exe",
        BackendName(api_), Utf8(suffix), Utf8(suffix));
    REXLOG_WARN("{} frame generation is off: {}. Run the game's setup again to get them.", kPrefix,
                reason);
    SetStatus(FgBackendState::kUnavailable, reason);
    return;
  }
  REXLOG_INFO("{} device can run FidelityFX ({}); loading the runtime on a worker thread", kPrefix,
              support.detail);
  SetStatus(FgBackendState::kProbing, {});
  worker_ = std::thread([this] { WorkerMain(); });
}

bool FfxFrameGeneration::LoadRuntime(std::string* why) {
  DWORD error = 0;
  loader_ = ffx_common::LoadByFullPath(loader_path_, &error);
  if (!loader_) {
    *why = std::format("the FidelityFX loader could not be opened (Windows error {})", error);
    return false;
  }
  // The effect DLL by its full path too, so that the loader's request for it by name meets the module
  // already loaded (AMD's documentation: a module of that name already in the process is reused).
  effect_ = ffx_common::LoadByFullPath(effect_path_, &error);
  if (!effect_) {
    *why = std::format("the FidelityFX frame generation DLL could not be opened (Windows error {})",
                       error);
    return false;
  }
  create_ = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(loader_, "ffxCreateContext"));
  destroy_ = reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(loader_, "ffxDestroyContext"));
  configure_ = reinterpret_cast<PfnFfxConfigure>(GetProcAddress(loader_, "ffxConfigure"));
  query_ = reinterpret_cast<PfnFfxQuery>(GetProcAddress(loader_, "ffxQuery"));
  dispatch_ = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(loader_, "ffxDispatch"));
  if (!create_ || !destroy_ || !configure_ || !query_ || !dispatch_) {
    *why = "the FidelityFX loader lacks the ffx functions";
    return false;
  }
  return true;
}

bool FfxFrameGeneration::QueryVersions(std::string* why) {
  ffx_glue::BackendDesc backend_desc;
  if (!ffx_common::FillBackend(api_, native_, &backend_desc)) {
    *why = "the device's native objects are not available";
    return false;
  }
  uint64_t count = 0;
  ffxQueryDescGetVersions q{};
  q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
  q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
  q.device = backend_desc.query_device;
  q.outputCount = &count;
  ffxReturnCode_t rc = query_(nullptr, &q.header);
  if (rc != FFX_API_RETURN_OK || count == 0) {
    *why = std::format("the runtime lists no frame generation versions for this device ({}, {} found)",
                       ReturnName(rc), count);
    return false;
  }
  std::vector<uint64_t> ids(count);
  std::vector<const char*> names(count);
  q.versionIds = ids.data();
  q.versionNames = names.data();
  rc = query_(nullptr, &q.header);
  if (rc != FFX_API_RETURN_OK) {
    *why = std::format("the runtime would not name its frame generation versions ({})", ReturnName(rc));
    return false;
  }
  // The newest FSR 3.1 provider; the 4.x one is the machine learning frame generation (RDNA 4 only),
  // which this build does not offer. The names are copied: some live in the runtime's global memory.
  std::string listing;
  const ffx_common::Version* best = nullptr;
  std::vector<ffx_common::Version> versions;
  for (uint64_t k = 0; k < std::min<uint64_t>(count, ids.size()); ++k) {
    ffx_common::Version v;
    v.id = ids[k];
    v.name = names[k] ? names[k] : "";
    ffx_common::ParseVersion(&v);
    versions.push_back(std::move(v));
  }
  for (const ffx_common::Version& v : versions) {
    listing += std::format("{}{} (id {:#x})", listing.empty() ? "" : ", ", v.name, v.id);
    if (v.parsed && v.major == 3 && v.minor == 1 && (!best || ffx_common::NewerThan(v, *best))) {
      best = &v;
    }
  }
  REXLOG_INFO("{} the runtime offers these frame generation versions: {}", kPrefix, listing);
  if (!best) {
    *why = "the runtime offers no FSR 3.1 frame generation for this device (see the versions above)";
    return false;
  }
  id31_ = best->id;
  name31_ = best->name;
  return true;
}

void FfxFrameGeneration::ProbeRuntime() {
  std::string why;
  if (!LoadRuntime(&why)) {
    REXLOG_WARN("{} frame generation is off: {}", kPrefix, why);
    SetStatus(FgBackendState::kUnavailable, why);
    return;
  }
  ffxConfigureDescGlobalDebug config{};
  config.header.type = FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG;
  config.effectId = FFX_API_EFFECT_ID_FRAMEGENERATION;
  config.fpMessage = &FfxMessage;
  config.debugLevel = FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_WARNINGS;
  const ffxReturnCode_t rc = configure_(nullptr, &config.header);
  if (rc != FFX_API_RETURN_OK) {
    REXLOG_INFO("{} the runtime's message hook was not taken ({}); its messages are not logged",
                kPrefix, ReturnName(rc));
  }
  if (!QueryVersions(&why)) {
    REXLOG_WARN("{} frame generation is off: {}", kPrefix, why);
    SetStatus(FgBackendState::kUnavailable, why);
    return;
  }
  // The provider really loads on this device: one small context, made and destroyed.
  FgContextRequest trial;
  trial.render_size[0] = trial.generation_size[0] = kTrialWidth;
  trial.render_size[1] = trial.generation_size[1] = kTrialHeight;
  std::shared_ptr<Context> context = CreateContext(trial, &why);
  if (!context) {
    REXLOG_WARN("{} frame generation is off: {}", kPrefix, why);
    SetStatus(FgBackendState::kUnavailable, why);
    return;
  }
  DestroyContext(*context);
  REXLOG_INFO("{} ready: {}", kPrefix, name31_);
  SetStatus(FgBackendState::kAvailable, {});
}

// ---------------------------------------------------------------------------
// contexts
// ---------------------------------------------------------------------------

std::shared_ptr<Context> FfxFrameGeneration::CreateContext(const FgContextRequest& request,
                                                           std::string* why, bool* out_of_memory) {
  auto context = std::make_shared<Context>();
  context->request = request;
  if (!ffx_common::FillBackend(api_, native_, &context->backend)) {
    *why = "the device's native objects are not available";
    return {};
  }
  // Depth is stored 1 near / 0 far; the vectors are at render size with the jitter already taken out
  // (so neither the display-resolution nor the jitter-cancellation flag); the colour is the codes
  // the generation pass writes (an SRGB transfer, not scene-referred light).
  context->create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
  context->create.flags = FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED;
  context->create.displaySize = {request.generation_size[0], request.generation_size[1]};
  context->create.maxRenderSize = {request.render_size[0], request.render_size[1]};
  context->create.backBufferFormat = FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT;
  // As of SDK 2.1 the new calls (the V2 preparation) need the version the application was built for.
  context->version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
  context->version.version = FFX_FRAMEGENERATION_VERSION;
  context->override_version.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
  context->override_version.versionId = id31_;
  context->create.header.pNext = &context->version.header;
  context->version.header.pNext = context->backend.header;
  context->backend.header->pNext = &context->override_version.header;
  context->override_version.header.pNext = nullptr;

  const auto begin = std::chrono::steady_clock::now();
  const ffxReturnCode_t rc = create_(&context->handle, &context->create.header, nullptr);
  if (rc != FFX_API_RETURN_OK || !context->handle) {
    context->handle = nullptr;
    *why = std::format("ffxCreateContext returned {} ({})", rc, ReturnName(rc));
    if (out_of_memory) {
      *out_of_memory = rc == FFX_API_RETURN_ERROR_MEMORY;
    }
    return {};
  }
  const double ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();

  // The provider the context really got: AMD's API falls back to another one without a word.
  ffxQueryGetProviderVersion provider{};
  provider.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
  const ffxReturnCode_t qrc = query_(&context->handle, &provider.header);
  if (qrc == FFX_API_RETURN_OK && provider.versionName) {
    context->provider = provider.versionName;
  }
  if (qrc == FFX_API_RETURN_OK && provider.versionId != id31_) {
    *why = std::format("the runtime made a context with provider '{}' instead of the FSR 3.1 one",
                       context->provider);
    DestroyContext(*context);
    return {};
  }
  uint64_t megabytes = 0;
  {
    FfxApiEffectMemoryUsage usage{};
    ffxQueryDescFrameGenerationGetGPUMemoryUsage q{};
    q.header.type = FFX_API_QUERY_DESC_TYPE_FRAMEGENERATION_GPU_MEMORY_USAGE;
    q.gpuMemoryUsageFrameGeneration = &usage;
    if (query_(&context->handle, &q.header) == FFX_API_RETURN_OK) {
      context->bytes = usage.totalUsageInBytes;
      megabytes = context->bytes >> 20;
      const uint64_t pixels = uint64_t(request.generation_size[0]) * request.generation_size[1];
      if (context->bytes != 0 && pixels != 0) {
        std::lock_guard lock(mu_);
        measured_bytes_ = context->bytes;
        measured_pixels_ = pixels;
      }
    }
  }
  REXLOG_INFO("{} context for {}x{} (render {}x{}) made in {:.0f} ms: provider '{}', about {} MB",
              kPrefix, request.generation_size[0], request.generation_size[1],
              request.render_size[0], request.render_size[1], ms,
              context->provider.empty() ? "?" : context->provider, megabytes);
  return context;
}

void FfxFrameGeneration::DestroyContext(Context& context) {
  if (context.handle && destroy_) {
    destroy_(&context.handle, nullptr);
  }
  context.handle = nullptr;
  if (context.held) {
    // The runtime's own images are not the device's, so the device is told what went: a creation
    // that failed for want of room may be tried again (Device::NoteReleased).
    context.held = false;
    if (device_) {
      device_->NoteReleased(context.bytes);
    }
  }
}

void FfxFrameGeneration::WorkerMain() {
  ProbeRuntime();
  std::unique_lock lock(mu_);
  for (;;) {
    cv_.wait(lock, [this] { return stop_ || !doomed_.empty() || WantLocked(); });
    if (stop_) {
      break;
    }
    if (!doomed_.empty()) {
      std::vector<std::shared_ptr<Context>> list = std::move(doomed_);
      doomed_.clear();
      lock.unlock();
      for (const auto& c : list) {
        DestroyContext(*c);
      }
      list.clear();
      lock.lock();
    }
    if (WantLocked()) {
      const FgContextRequest request = want_request_;
      lock.unlock();
      std::string why;
      bool out_of_memory = false;
      std::shared_ptr<Context> made = CreateContext(request, &why, &out_of_memory);
      lock.lock();
      if (!made && out_of_memory) {
        // The card is full: not a failure of the session. Nothing is asked again until the renderer
        // releases the request (it switches frame generation off and looks again later).
        if (!status_.no_memory) {
          REXLOG_WARN("{} the context for {}x{} could not be made: the runtime is out of video "
                      "memory ({}); frame generation goes off until there is room again",
                      kPrefix, request.generation_size[0], request.generation_size[1], why);
        }
        status_.no_memory = true;
        status_.reason = "AMD's context could not be made: the card ran out of video memory";
        want_ = false;
      } else if (!made) {
        // Off for the session, with one line; the renderer reads it from Status().
        if (!status_.failed) {
          REXLOG_WARN("{} frame generation is off for this session: its context could not be made: "
                      "{}",
                      kPrefix, why);
        }
        status_.failed = true;
        status_.reason = std::format("its context could not be made: {}", why);
        want_ = false;
      } else if (want_ && want_request_ == request && !ctx_ && !stop_) {
        made->held = true;
        status_.provider = made->provider;
        status_.context_bytes = made->bytes;
        ctx_ = std::move(made);
      } else {
        made->held = true;
        doomed_.push_back(std::move(made));  // the request moved on while it was made
      }
    }
  }
  // Shutdown: what is left (the GPU is idle, the renderer drained its frames).
  std::vector<std::shared_ptr<Context>> list = std::move(doomed_);
  doomed_.clear();
  if (ctx_) {
    list.push_back(std::move(ctx_));
    ctx_.reset();
  }
  lock.unlock();
  for (const auto& c : list) {
    DestroyContext(*c);
  }
}

void FfxFrameGeneration::RetireCurrentLocked(std::vector<std::shared_ptr<Context>>* retire) {
  status_.context_bytes = 0;
  if (ctx_) {
    retire->push_back(std::move(ctx_));
    ctx_.reset();
  }
}

void FfxFrameGeneration::RetireOutside(std::vector<std::shared_ptr<Context>>& retire) {
  // The context may be named by a list the GPU has not finished: it goes to the worker once the
  // frames recorded so far are done.
  for (std::shared_ptr<Context>& old : retire) {
    if (!frames_) {
      std::lock_guard lock(mu_);
      doomed_.push_back(std::move(old));
      cv_.notify_all();
      continue;
    }
    frames_->RetireAction([this, old] {
      {
        std::lock_guard lock(mu_);
        doomed_.push_back(old);
      }
      cv_.notify_all();
    });
  }
  retire.clear();
}

void FfxFrameGeneration::Request(const FgContextRequest& request) {
  std::vector<std::shared_ptr<Context>> retire;
  {
    std::lock_guard lock(mu_);
    if (status_.state != FgBackendState::kAvailable || status_.failed || status_.no_memory) {
      return;
    }
    if (want_ && want_request_ == request) {
      return;
    }
    if (ctx_ && !(ctx_->request == request)) {
      RetireCurrentLocked(&retire);
    }
    want_ = true;
    want_request_ = request;
  }
  cv_.notify_all();
  RetireOutside(retire);
}

void FfxFrameGeneration::Release() {
  std::vector<std::shared_ptr<Context>> retire;
  {
    std::lock_guard lock(mu_);
    want_ = false;
    status_.no_memory = false;
    RetireCurrentLocked(&retire);
  }
  RetireOutside(retire);
}

void FfxFrameGeneration::Fail(const std::string& why) {
  std::vector<std::shared_ptr<Context>> retire;
  bool first = false;
  {
    std::lock_guard lock(mu_);
    first = !status_.failed;
    status_.failed = true;
    status_.reason = why;
    want_ = false;
    RetireCurrentLocked(&retire);
  }
  if (first) {
    REXLOG_WARN("{} frame generation is off for this session: {}", kPrefix, why);
  }
  RetireOutside(retire);
}

// ---------------------------------------------------------------------------
// the calls of a frame
// ---------------------------------------------------------------------------

// CHECK: the whole of this function against the runtime: the call order (configure, prepare V2,
// generate) is the one AMD's documentation requires; the V2 preparation on the 3.1 provider (it
// answers FFX_API_RETURN_PROVIDER_NO_SUPPORT_NEW_DESCTYPE when it does not take it, which is a
// failure here with that text in the log); the camera vectors; viewSpaceToMetersFactor 1; the
// resource states handed over (COMPUTE_READ, UNORDERED_ACCESS: the runtime takes its resources back
// to them, as the upscaler's dispatch does); and that nothing it changes in the list state is missed
// by backend::InvalidateAfterExternalWork (the caller's).
bool FfxFrameGeneration::Generate(plume::RenderCommandList* list, const FgInputs& in) {
  std::shared_ptr<Context> context;
  {
    std::lock_guard lock(mu_);
    context = ctx_;
  }
  const auto fail = [&](const std::string& why) {
    Fail(why);
    return false;
  };
  if (!context || !context->handle) {
    return fail("a generation was asked for with no context");
  }
  void* native_list = backend::NativeListHandle(list);
  if (!native_list) {
    return fail("the command list has no native object");
  }
  // AMD's Vulkan backend (ffx_vk.cpp, ffxUnregisterResources) counts a frame at the end of every
  // dispatch of an effect and, with FFX_MAX_QUEUED_FRAMES 4, destroys the image views a dispatch
  // registered when the third one after it ends. The preparation and the generation are two
  // dispatches, so the views of a frame's preparation go when the NEXT frame's generation ends: while
  // the render queue may still be executing that frame (two are in flight), with the descriptor set
  // that holds the view bound in its command buffer: VUID-vkDestroyImageView-imageView-01026, seen
  // once in a validation run (frame generation on, next2-vk-val). One frame in flight at this point
  // makes its views safe to destroy: the previous frame must have completed before this one's calls
  // are recorded. Nearly always it has (the GPU needs less than a frame of the CPU's time). The
  // D3D12 backend writes its descriptors on a ring of the same size, which two frames of lag already
  // covers, and destroys nothing.
  // CHECK: this is read from the SDK's source at tag v1.1.4 (sdk/src/backends/vk/ffx_vk.cpp);
  // the DLL we ship reports provider 3.1.6 and its source was not read. The run that settles it:
  // Vulkan validation with frame generation on and a run of some minutes (MSAA changes among
  // them): VUID-vkDestroyImageView-imageView-01026 must not come again; and that the wait costs no
  // visible frame time (the render thread's frame time with the wait; it only waits where the GPU is
  // slower than the CPU frame).
  if (ActiveBackend() == Backend::kVulkan && frames_) {
    frames_->WaitPreviousFrame();
  }
  const int32_t gen_w = int32_t(in.generation_size[0]);
  const int32_t gen_h = int32_t(in.generation_size[1]);

  // 1. The frame's configuration.
  ffxConfigureDescFrameGeneration config{};
  config.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
  config.frameGenerationEnabled = true;
  config.allowAsyncWorkloads = false;
  config.flags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
  config.generationRect = {0, 0, gen_w, gen_h};
  config.frameID = in.frame_id;
  ffxReturnCode_t rc = configure_(&context->handle, &config.header);
  if (rc != FFX_API_RETURN_OK) {
    return fail(std::format("ffxConfigure returned {} ({})", rc, ReturnName(rc)));
  }

  // 2. The preparation: this frame's depth, vectors, jitter and camera.
  ffxDispatchDescFrameGenerationPrepareV2 prepare{};
  prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
  prepare.frameID = in.frame_id;
  prepare.flags = 0;
  prepare.commandList = native_list;
  prepare.renderSize = {in.render_size[0], in.render_size[1]};
  prepare.jitterOffset = {in.jitter[0], in.jitter[1]};
  prepare.motionVectorScale = {in.motion_scale[0], in.motion_scale[1]};
  prepare.frameTimeDelta = in.frame_time_ms;
  prepare.reset = in.reset || !context->dispatched;
  prepare.cameraNear = in.camera_near;
  prepare.cameraFar = in.camera_far;
  prepare.cameraFovAngleVertical = in.camera_fov_y;
  // CHECK: the game's world unit is about a metre (a car at 64 mph covers 28.7 units a second).
  prepare.viewSpaceToMetersFactor = 1.0f;
  for (int i = 0; i < 3; ++i) {
    prepare.cameraPosition[i] = in.camera_position[i];
    prepare.cameraUp[i] = in.camera_up[i];
    prepare.cameraRight[i] = in.camera_right[i];
    prepare.cameraForward[i] = in.camera_forward[i];
  }
  if (!MakeResource(in.depth, FFX_API_RESOURCE_STATE_COMPUTE_READ, &prepare.depth) ||
      !MakeResource(in.motion, FFX_API_RESOURCE_STATE_COMPUTE_READ, &prepare.motionVectors)) {
    return fail("the depth or the motion vectors have no native object or a format the runtime "
                "does not know");
  }
  rc = dispatch_(&context->handle, &prepare.header);
  if (rc != FFX_API_RETURN_OK) {
    return fail(std::format("the preparation (ffxDispatch) returned {} ({})", rc, ReturnName(rc)));
  }

  // 3. The generation: the current colour in, the interpolated frame out.
  ffxDispatchDescFrameGeneration generate{};
  generate.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION;
  generate.commandList = native_list;
  if (!MakeResource(in.color, FFX_API_RESOURCE_STATE_COMPUTE_READ, &generate.presentColor) ||
      !MakeResource(in.output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS, &generate.outputs[0])) {
    return fail("the colour or the output image has no native object or a format the runtime does "
                "not know");
  }
  generate.numGeneratedFrames = 1;
  generate.reset = prepare.reset;
  // The codes are the game's (the display table comes later): the transfer function only steers the
  // runtime's luminance heuristics.
  generate.backbufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
  generate.minMaxLuminance[0] = 0.0f;
  generate.minMaxLuminance[1] = 1.0f;
  generate.generationRect = {0, 0, gen_w, gen_h};
  generate.frameID = in.frame_id;
  rc = dispatch_(&context->handle, &generate.header);
  if (rc != FFX_API_RETURN_OK) {
    return fail(std::format("the generation (ffxDispatch) returned {} ({})", rc, ReturnName(rc)));
  }
  context->dispatched = true;
  if (generations_++ == 0) {
    REXLOG_INFO("{} first generation recorded: render {}x{}, generated {}x{}, jitter {:+.3f}, "
                "{:+.3f}, near {:.3f}, far {:.1f}, field of view {:.2f} deg, reset {}",
                kPrefix, in.render_size[0], in.render_size[1], in.generation_size[0],
                in.generation_size[1], in.jitter[0], in.jitter[1], in.camera_near, in.camera_far,
                double(in.camera_fov_y) * 57.29577951308232, prepare.reset ? "yes" : "no");
  }
  return true;
}

void FfxFrameGeneration::Shutdown() {
  {
    std::lock_guard lock(mu_);
    stop_ = true;
    want_ = false;
  }
  cv_.notify_all();
  if (worker_.joinable()) {
    worker_.join();
  }
  // The worker destroyed the contexts on its way out; one the render thread still holds a copy of (a
  // retire action that never ran) is destroyed here: the GPU is idle.
  {
    std::lock_guard lock(mu_);
    ctx_.reset();
  }
  // The modules stay loaded to the end of the process: the loader may hold the effect.
  effect_ = nullptr;
  loader_ = nullptr;
  frames_ = nullptr;
  probe_started_ = false;
}

std::unique_ptr<FrameGenerationBackend> MakeFfxFrameGeneration() {
  return std::make_unique<FfxFrameGeneration>();
}

}  // namespace mcla::gpu
