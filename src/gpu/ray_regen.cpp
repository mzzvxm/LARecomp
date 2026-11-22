// MCLA GPU renderer -- AMD FSR Ray Regeneration through the FidelityFX API. See ray_regen.h.

#include "ray_regen.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <format>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rex/logging/log_category.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d12.h>

#include <ffx_api.h>
#include <ffx_api_types.h>
#include <ffx_denoiser.h>

#include "settings/larecomp_settings.h"
#include "backend.h"
#include "device.h"
#include "ffx_common.h"
#include "frame.h"
#include "temporal_ffx_glue.h"

// The numbers ray_regen_rules.h keeps (so that it needs no SDK) are the SDK's own (FSR SDK 2.4.0: Ray Regeneration 1.2.1).
namespace rg = mcla::gpu::rayregen;
static_assert(rg::kEffectIdDenoiser == FFX_API_EFFECT_ID_DENOISER);
static_assert(rg::kHeaderVersion == uint32_t(FFX_DENOISER_VERSION));
static_assert(rg::kDispatchReset == uint32_t(FFX_DENOISER_DISPATCH_RESET));
static_assert(rg::kDispatchNonEncodedAlbedo == uint32_t(FFX_DENOISER_DISPATCH_NON_ENCODED_ALBEDO));
static_assert(rg::FlagOf(rg::Signal::kAmbientOcclusion) == uint32_t(FFX_DENOISER_SIGNAL_AMBIENT_OCCLUSION));
static_assert(rg::FlagOf(rg::Signal::kDirectDiffuse) == uint32_t(FFX_DENOISER_SIGNAL_DIRECT_DIFFUSE));
static_assert(rg::FlagOf(rg::Signal::kDirectSpecular) == uint32_t(FFX_DENOISER_SIGNAL_DIRECT_SPECULAR));
static_assert(rg::FlagOf(rg::Signal::kDominantLight) == uint32_t(FFX_DENOISER_SIGNAL_DOMINANT_LIGHT_VISIBILITY));
static_assert(rg::FlagOf(rg::Signal::kIndirectDiffuse) == uint32_t(FFX_DENOISER_SIGNAL_INDIRECT_DIFFUSE));
static_assert(rg::FlagOf(rg::Signal::kIndirectSpecular) == uint32_t(FFX_DENOISER_SIGNAL_INDIRECT_SPECULAR));
static_assert(rg::FlagOf(rg::Signal::kSpecularOcclusion) == uint32_t(FFX_DENOISER_SIGNAL_SPECULAR_OCCLUSION));
static_assert(sizeof(FfxApiMatrix4x4) == 16 * sizeof(float), "the matrices are copied from 16 floats, row-major");

namespace mcla::gpu::rayregen {

namespace {

constexpr const char* kPrefix = "[gpu] ray regen:";
constexpr uint32_t kMaxMessageLines = 24;
// The size and signal of the trial context: it only has to load (the runtime's answer for this card).
constexpr uint32_t kTrialWidth = 960;
constexpr uint32_t kTrialHeight = 540;
constexpr uint32_t kTrialSignals = 1u << 5;  // indirect specular (static_assert below)
static_assert(kTrialSignals == uint32_t(FFX_DENOISER_SIGNAL_INDIRECT_SPECULAR));

using ffx_common::ExeDirectory;
using ffx_common::FileExists;
using ffx_common::FillBackend;
using ffx_common::LoadByFullPath;
using ffx_common::MakeResource;
using ffx_common::ParseVersion;
using ffx_common::ReturnName;
using ffx_common::Utf8;
using ffx_common::Version;

std::mutex g_published_mu;
Published g_published;

void Publish(const std::function<void(Published&)>& edit) {
  std::lock_guard lock(g_published_mu);
  edit(g_published);
}

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

// The highest shader model the runtime (the Agility SDK next to the exe) and the driver offer: a runtime that does not know a model answers E_INVALIDARG, so ask downwards.
uint32_t HighestShaderModel(void* d3d12_device) {
  auto* dev = static_cast<ID3D12Device*>(d3d12_device);
  if (!dev) {
    return 0;
  }
  for (uint32_t candidate = 0x69; candidate >= 0x60; --candidate) {
    D3D12_FEATURE_DATA_SHADER_MODEL model = {};
    model.HighestShaderModel = D3D_SHADER_MODEL(candidate);
    if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, sizeof(model)))) {
      return uint32_t(model.HighestShaderModel);
    }
  }
  return 0;
}

// The plume format as the rules tell formats apart (only what ffx_common::SurfaceFormat can describe: anything else is unknown and refused by the plan).
rr::Format RulesFormat(plume::RenderFormat f) {
  switch (f) {
    case plume::RenderFormat::R16G16B16A16_FLOAT: return rr::Format::kRgba16Float;
    case plume::RenderFormat::R32_FLOAT: return rr::Format::kR32Float;
    case plume::RenderFormat::R16_FLOAT: return rr::Format::kR16Float;
    case plume::RenderFormat::R8_UNORM: return rr::Format::kR8Unorm;
    case plume::RenderFormat::R16G16_FLOAT: return rr::Format::kRg16Float;
    default: return rr::Format::kUnknown;
  }
}
rr::Image RulesImage(const TemporalImage& image) {
  rr::Image out;
  out.present = image.texture != nullptr;
  out.width = image.width;
  out.height = image.height;
  out.format = RulesFormat(image.format);
  return out;
}

// One context and the descriptions it was made with (the runtime reads them until it is destroyed).
struct Context {
  ffxContext handle = nullptr;
  uint32_t signals = 0;
  uint32_t max_size[2] = {};
  uint64_t bytes = 0;
  ffxCreateContextDescDenoiser create{};
  ffxOverrideVersion override_version{};
  ffx_glue::BackendDesc backend;
};

}  // namespace

Published Current() {
  std::lock_guard lock(g_published_mu);
  return g_published;
}

struct RayRegen::Impl {
  std::mutex mu;
  std::condition_variable cv;
  std::thread worker;
  bool stop = false;
  bool probe_started = false;
  Device* device = nullptr;
  FrameRing* frames = nullptr;
  backend::NativeDevice native;

  HMODULE loader = nullptr;
  HMODULE denoiser = nullptr;
  PfnFfxCreateContext create = nullptr;
  PfnFfxDestroyContext destroy = nullptr;
  PfnFfxConfigure configure = nullptr;
  PfnFfxQuery query = nullptr;
  PfnFfxDispatch dispatch = nullptr;
  uint64_t version_id = 0;
  std::string version_name;

  uint32_t want_signals = 0;
  uint32_t want_size[2] = {};
  bool want_changed = false;
  std::shared_ptr<Context> ctx;                  // the live one (the render thread reads a copy)
  std::shared_ptr<Context> fresh;                // made by the worker, adopted by the render thread (Adopt)
  std::vector<std::shared_ptr<Context>> doomed;  // retired, to be destroyed by the worker
  std::atomic<bool> failed{false};               // dropped for the session (any thread reads it)
  rr::FailureBook book;                          // the render thread's count: three errors in a row drop it
  FrameClock clock;                              // the render thread's
  uint64_t dispatches = 0;
  bool refusal_logged = false;

  void WorkerMain();
  bool Open(std::string* why);
  bool QueryProviders(std::string* why);
  std::shared_ptr<Context> MakeContext(uint32_t signals, uint32_t w, uint32_t h, std::string* why);
  void DestroyContext(Context& context);
  void Retire(std::shared_ptr<Context> old);
  void Adopt();
  void Fail(const std::string& why);
};

RayRegen::RayRegen() : impl_(std::make_unique<Impl>()) {}

RayRegen::~RayRegen() {
  Shutdown();
}

// ---------------------------------------------------------------------------
// the probe
// ---------------------------------------------------------------------------

void RayRegen::Probe(Device& device, FrameRing& frames) {
  Impl& i = *impl_;
  if (i.probe_started) {
    return;
  }
  i.probe_started = true;
  i.device = &device;
  i.frames = &frames;
  Facts facts;
  facts.build_has = true;
  // What needs no runtime: this device. Nothing of AMD's is touched on a PC that cannot run it.
  i.native = backend::NativeDeviceOf(device.device());
  DeviceInputs in;
  in.vendor_id = uint32_t(device.description().vendor);
  in.d3d12 = mcla::gpu::ActiveBackend() == mcla::gpu::Backend::kD3D12;
  in.shader_model = in.d3d12 ? HighestShaderModel(i.native.device) : 0;
  facts.device = DeviceOk(in);
  if (facts.device != DeviceVerdict::kOk) {
    REXLOG_INFO("{} not offered on this device: {} (vendor {:#06x}, shader model {}.{})", kPrefix, DeviceVerdictText(facts.device), in.vendor_id, in.shader_model >> 4, in.shader_model & 0xF);
    Publish([&](Published& p) { p.facts = facts; });
    return;
  }
  const std::wstring dir = ExeDirectory();
  facts.files_ok = !dir.empty() && FileExists(dir + L"amd_fidelityfx_loader_dx12.dll") && FileExists(dir + L"amd_fidelityfx_denoiser_dx12.dll");
  if (!facts.files_ok) {
    REXLOG_WARN("{} AMD's amd_fidelityfx_loader_dx12.dll and amd_fidelityfx_denoiser_dx12.dll are not next to the game's exe: Ray Regeneration is off. Run the game's setup again to get them.", kPrefix);
    Publish([&](Published& p) { p.facts = facts; });
    return;
  }
  Publish([&](Published& p) { p.facts = facts; });
  REXLOG_INFO("{} this device can run it (an AMD GPU, Direct3D 12, shader model {}.{}); opening AMD's runtime on a worker thread", kPrefix, in.shader_model >> 4, in.shader_model & 0xF);
  i.worker = std::thread([&i] { i.WorkerMain(); });
}

bool RayRegen::Impl::Open(std::string* why) {
  const std::wstring dir = ExeDirectory();
  DWORD error = 0;
  loader = LoadByFullPath(dir + L"amd_fidelityfx_loader_dx12.dll", &error);
  if (!loader) {
    *why = std::format("the FidelityFX loader could not be opened (Windows error {})", error);
    return false;
  }
  // The denoiser DLL by its full path too, so that the loader's own request for it by name meets the module already loaded (the loader looks for a loaded module first, then the exe's folder).
  denoiser = LoadByFullPath(dir + L"amd_fidelityfx_denoiser_dx12.dll", &error);
  if (!denoiser) {
    *why = std::format("the FidelityFX denoiser could not be opened (Windows error {})", error);
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
  if (configure) {
    ffxConfigureDescGlobalDebug config{};
    config.header.type = FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG;
    config.effectId = FFX_API_EFFECT_ID_DENOISER;
    config.fpMessage = &FfxMessage;
    config.debugLevel = FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_WARNINGS;
    const ffxReturnCode_t rc = configure(nullptr, &config.header);
    if (rc != FFX_API_RETURN_OK) {
      REXLOG_INFO("{} the runtime's message hook was not taken ({}); its messages are not logged", kPrefix, ReturnName(rc));
    }
  }
  return true;
}

bool RayRegen::Impl::QueryProviders(std::string* why) {
  ffx_glue::BackendDesc backend_desc;
  if (!FillBackend(mcla::gpu::Backend::kD3D12, native, &backend_desc)) {
    *why = "the device's native objects are not available";
    return false;
  }
  uint64_t count = 0;
  ffxQueryDescGetVersions q{};
  q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
  q.createDescType = FFX_API_EFFECT_ID_DENOISER;  // CHECK: AMD's docs and sample pass the effect id here; the upscaler backend passes the create description's type (FFX_API_CREATE_CONTEXT_DESC_TYPE_DENOISER): the loader may take either
  q.device = backend_desc.query_device;
  q.outputCount = &count;
  ffxReturnCode_t rc = query(nullptr, &q.header);
  if (rc != FFX_API_RETURN_OK || count == 0) {
    *why = std::format("the runtime lists no Ray Regeneration provider for this device ({}, {} found)", ReturnName(rc), count);
    return false;
  }
  std::vector<uint64_t> ids(count);
  std::vector<const char*> names(count);
  q.versionIds = ids.data();
  q.versionNames = names.data();
  rc = query(nullptr, &q.header);
  if (rc != FFX_API_RETURN_OK) {
    *why = std::format("the runtime would not name its Ray Regeneration providers ({})", ReturnName(rc));
    return false;
  }
  std::vector<ProviderVersion> list;
  std::vector<std::string> copies;  // the names may be overwritten by the next query (AMD's FSR API guide): copied
  std::string listing;
  for (uint64_t k = 0; k < std::min<uint64_t>(count, ids.size()); ++k) {
    Version v;
    v.id = ids[k];
    v.name = names[k] ? names[k] : "";
    ParseVersion(&v);
    ProviderVersion p;
    p.id = v.id;
    p.major = v.major;
    p.minor = v.minor;
    p.patch = v.patch;
    p.driver_provided = IsDriverProvided(v.id, v.name);
    list.push_back(p);
    copies.push_back(v.name);
    listing += std::format("{}{} (id {:#x}{})", listing.empty() ? "" : ", ", v.name, v.id, p.driver_provided ? ", from the driver" : "");
  }
  REXLOG_INFO("{} the runtime offers these Ray Regeneration providers: {}", kPrefix, listing);
  const int best = PickProvider(list.data(), list.size());
  if (best < 0) {
    *why = "the runtime's list of providers is empty";
    return false;
  }
  version_id = list[size_t(best)].id;
  version_name = copies[size_t(best)];
  return true;
}

std::shared_ptr<Context> RayRegen::Impl::MakeContext(uint32_t signals, uint32_t w, uint32_t h, std::string* why) {
  if (const uint32_t undriven = UndrivenSignals(signals)) {
    *why = std::format("this build does not drive the signal(s) {:#x} yet", undriven);
    return {};
  }
  auto context = std::make_shared<Context>();
  context->signals = signals;
  context->max_size[0] = w;
  context->max_size[1] = h;
  if (!FillBackend(mcla::gpu::Backend::kD3D12, native, &context->backend)) {
    *why = "the device's native objects are not available";
    return {};
  }
  context->create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_DENOISER;
  context->create.version = FFX_DENOISER_VERSION;
  context->create.maxRenderSize = {w, h};
  context->create.signalFlags = signals;
  context->create.checkerboardSignalFlags = 0;  // checkerboard (half width) signals are a later option
  context->create.flags = 0;
  context->override_version.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
  context->override_version.versionId = version_id;
  context->create.header.pNext = context->backend.header;
  context->backend.header->pNext = &context->override_version.header;
  context->override_version.header.pNext = nullptr;

  // What it will ask for, before it is made (the room check's input; the runtime's own figure replaces the rules' table).
  {
    FfxApiEffectMemoryUsage usage{};
    ffxQueryDescDenoiserGetGPUMemoryUsage q{};
    q.header.type = FFX_API_QUERY_DESC_TYPE_DENOISER_GPU_MEMORY_USAGE;
    q.device = native.device;
    q.maxRenderSize = {w, h};
    q.signalFlags = signals;
    q.checkerboardSignalFlags = 0;
    q.flags = 0;
    q.gpuMemoryUsage = &usage;
    if (query(nullptr, &q.header) == FFX_API_RETURN_OK) {
      context->bytes = usage.totalUsageInBytes;
    }
  }
  const auto begin = std::chrono::steady_clock::now();
  const ffxReturnCode_t rc = create(&context->handle, &context->create.header, nullptr);
  if (rc != FFX_API_RETURN_OK || !context->handle) {
    context->handle = nullptr;
    *why = std::format("ffxCreateContext returned {} ({})", rc, ReturnName(rc));
    return {};
  }
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
  REXLOG_INFO("{} a context for signals {:#x} up to {}x{} is made in {:.0f} ms ({:.0f} MiB by the runtime's own count, {:.0f} MiB by the rules' table)", kPrefix, signals, w, h, ms,
              double(context->bytes) / double(rr::kMiB), double(EstimateBytes(signals, w, h)) / double(rr::kMiB));
  return context;
}

void RayRegen::Impl::DestroyContext(Context& context) {
  if (context.handle && destroy) {
    destroy(&context.handle, nullptr);
  }
  context.handle = nullptr;
}

void RayRegen::Impl::Retire(std::shared_ptr<Context> old) {
  // The context may be named by a list the GPU has not finished: it goes to the worker once the frames recorded so far are done.
  if (!old) {
    return;
  }
  if (!frames) {
    std::lock_guard lock(mu);
    doomed.push_back(std::move(old));
    cv.notify_all();
    return;
  }
  frames->RetireAction([this, old] {
    {
      std::lock_guard lock(mu);
      doomed.push_back(old);
    }
    cv.notify_all();
  });
}

// The render thread takes a context the worker made: the one it replaces goes to Retire (frames may still name it), and the history starts again.
void RayRegen::Impl::Adopt() {
  std::shared_ptr<Context> previous;
  {
    std::lock_guard lock(mu);
    if (!fresh) {
      return;
    }
    previous = std::move(ctx);
    ctx = std::move(fresh);
    fresh.reset();
  }
  clock.Forget();
  Retire(std::move(previous));
}

void RayRegen::Impl::Fail(const std::string& why) {
  if (!failed.exchange(true)) {
    REXLOG_WARN("{} Ray Regeneration is off for this session: {}. The effects use our own denoisers from now on.", kPrefix, why);
  }
  Publish([&](Published& p) {
    p.facts.failed = true;
    p.failed_why = why;
    p.context_making = false;
  });
}

void RayRegen::Impl::WorkerMain() {
  Facts facts;
  Publish([&](Published& p) { facts = p.facts; });
  std::string why;
  if (!Open(&why) || !QueryProviders(&why)) {
    REXLOG_WARN("{} Ray Regeneration is off: {}", kPrefix, why);
    facts.probed = true;
    facts.supported = false;
    Publish([&](Published& p) { p.facts = facts; });
  } else {
    // The runtime lists a provider; does a context really get made on this card? (RDNA 3 and older list none; an old driver may list one and still refuse.)
    std::shared_ptr<Context> trial = MakeContext(kTrialSignals, kTrialWidth, kTrialHeight, &why);
    facts.probed = true;
    facts.supported = trial != nullptr;
    if (trial) {
      DestroyContext(*trial);
      REXLOG_INFO("{} ready: provider {} makes a context on this card", kPrefix, version_name);
    } else {
      REXLOG_INFO("{} not offered: {}", kPrefix, why);
    }
    Publish([&](Published& p) {
      p.facts = facts;
      p.provider = version_name;
    });
  }
  std::unique_lock lock(mu);
  for (;;) {
    cv.wait(lock, [this] { return stop || !doomed.empty() || want_changed; });
    std::vector<std::shared_ptr<Context>> gone;
    gone.swap(doomed);
    const bool make = want_changed && facts.supported && !failed.load();
    const uint32_t signals = want_signals;
    const uint32_t size[2] = {want_size[0], want_size[1]};
    want_changed = false;
    const bool leaving = stop;
    lock.unlock();
    for (std::shared_ptr<Context>& old : gone) {
      DestroyContext(*old);
    }
    if (make && !leaving && signals != 0) {
      std::string make_why;
      std::shared_ptr<Context> made = MakeContext(signals, size[0], size[1], &make_why);
      if (made) {
        std::shared_ptr<Context> unused;  // an earlier one the render thread never adopted: no frame named it, it can go at once
        {
          std::lock_guard guard(mu);
          unused = std::move(fresh);
          fresh = made;
        }
        if (unused) {
          DestroyContext(*unused);
        }
        Publish([&](Published& p) {
          p.context_bytes = made->bytes;
          p.context_making = false;
        });
      } else {
        Fail(make_why);
      }
    }
    lock.lock();
    if (leaving) {
      // The GPU is idle at shutdown: everything goes.
      for (std::shared_ptr<Context>& old : doomed) {
        DestroyContext(*old);
      }
      doomed.clear();
      for (std::shared_ptr<Context>* c : {&ctx, &fresh}) {
        if (*c) {
          DestroyContext(**c);
          c->reset();
        }
      }
      return;
    }
  }
}

// ---------------------------------------------------------------------------
// contexts and the dispatch
// ---------------------------------------------------------------------------

void RayRegen::Request(uint32_t signals, uint32_t max_w, uint32_t max_h) {
  Impl& i = *impl_;
  i.Adopt();
  std::shared_ptr<Context> release;
  {
    std::lock_guard lock(i.mu);
    if (!i.worker.joinable() || i.stop || i.failed.load()) {
      return;
    }
    if (signals == 0) {
      // Nothing is asked for: the live context goes (after the frames that may name it) and a pending one is forgotten.
      i.want_changed = false;
      i.want_signals = 0;
      release = std::move(i.ctx);
      if (!release) {
        return;
      }
    } else {
      const bool live = i.ctx && i.ctx->signals == signals && i.ctx->max_size[0] >= max_w && i.ctx->max_size[1] >= max_h;
      const bool pending = i.want_changed && i.want_signals == signals && i.want_size[0] >= max_w && i.want_size[1] >= max_h;
      if (live || pending) {
        return;
      }
      i.want_signals = signals;
      i.want_size[0] = max_w;
      i.want_size[1] = max_h;
      i.want_changed = true;
    }
  }
  if (release) {
    i.clock.Forget();
    i.Retire(std::move(release));
    Publish([&](Published& p) {
      p.context_bytes = 0;
      p.context_making = false;
    });
    return;
  }
  Publish([&](Published& p) { p.context_making = true; });
  i.cv.notify_all();
}

bool RayRegen::Ready(uint32_t signals, uint32_t w, uint32_t h) {
  Impl& i = *impl_;
  i.Adopt();
  std::lock_guard lock(i.mu);
  return i.ctx && i.ctx->handle && i.ctx->signals == signals && i.ctx->max_size[0] >= w && i.ctx->max_size[1] >= h && !i.failed.load();
}

void RayRegen::ForgetHistory() {
  impl_->clock.Forget();
}

bool RayRegen::Dispatch(plume::RenderCommandList* list, const DispatchFrame& frame) {
  Impl& i = *impl_;
  i.Adopt();
  std::shared_ptr<Context> context;
  {
    std::lock_guard lock(i.mu);
    context = i.ctx;
  }
  const auto refuse = [&](const std::string& why) {
    if (!i.refusal_logged) {
      i.refusal_logged = true;
      REXLOG_WARN("{} a dispatch was refused: {} (logged once; the effect keeps its own denoiser for the frame)", kPrefix, why);
    }
    i.book.Note(rr::FailureBook::Event::kRefused);
    return false;
  };
  if (!context || !context->handle || i.failed.load()) {
    return refuse("there is no context");
  }
  // The images against the context, in the rules' words.
  Inputs plan_in;
  plan_in.linear_depth = RulesImage(frame.linear_depth);
  plan_in.motion = RulesImage(frame.motion);
  plan_in.normals = RulesImage(frame.normals);
  plan_in.specular_albedo = RulesImage(frame.specular_albedo);
  plan_in.diffuse_albedo = RulesImage(frame.diffuse_albedo);
  for (size_t s = 0; s < kSignalCount; ++s) {
    plan_in.signal_in[s] = RulesImage(frame.signals[s].input);
    plan_in.signal_out[s] = RulesImage(frame.signals[s].output);
  }
  plan_in.render_size[0] = frame.render_size[0];
  plan_in.render_size[1] = frame.render_size[1];
  plan_in.context_max_size[0] = context->max_size[0];
  plan_in.context_max_size[1] = context->max_size[1];
  plan_in.context_signals = context->signals;
  plan_in.dispatch_signals = frame.dispatch_signals;
  const DispatchPlan plan = PlanDispatch(plan_in);
  if (plan.verdict != Verdict::kOk) {
    return refuse(std::format("{} ({})", VerdictText(plan.verdict), plan.culprit));
  }
  void* native_list = backend::NativeListHandle(list);
  if (!native_list) {
    return refuse("the command list has no native object");
  }
  const uint32_t in_state = frame.input_state ? frame.input_state : uint32_t(FFX_API_RESOURCE_STATE_COMPUTE_READ);
  const uint32_t out_state = frame.output_state ? frame.output_state : uint32_t(FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);

  // These live until ffxDispatch returns (the runtime walks the chain).
  ffxDispatchDescDenoiser d{};
  d.header.type = FFX_API_DISPATCH_DESC_TYPE_DENOISER;
  d.commandList = native_list;
  bool images = MakeResource(frame.linear_depth, in_state, &d.linearDepth) && MakeResource(frame.motion, in_state, &d.motionVectors) && MakeResource(frame.normals, in_state, &d.normals);
  // The albedos may stay empty only where no signal reads them (AMD: zero-initialised); the plan checked that.
  if (frame.specular_albedo.texture) images = images && MakeResource(frame.specular_albedo, in_state, &d.specularAlbedo);
  if (frame.diffuse_albedo.texture) images = images && MakeResource(frame.diffuse_albedo, in_state, &d.diffuseAlbedo);
  const MotionScale scale = MotionScaleFor(frame.render_size[0], frame.render_size[1]);
  d.motionVectorScale = {scale.x, scale.y, scale.z};
  d.jitterOffsets = {frame.jitter[0], frame.jitter[1]};
  d.cameraPositionDelta = {frame.camera_position_delta[0], frame.camera_position_delta[1], frame.camera_position_delta[2]};
  std::memcpy(&d.view, frame.view, sizeof(d.view));
  std::memcpy(&d.projection, frame.projection, sizeof(d.projection));
  d.linearDepthBounds = {frame.depth_bounds.min, frame.depth_bounds.max};
  d.renderSize = {frame.render_size[0], frame.render_size[1]};
  const FrameClock::Step step = i.clock.Next(frame.game_frame, frame.force_reset);
  d.frameIndex = step.index;
  d.flags = DispatchFlags(step.reset, frame.shared_linear_albedo);

  ffxDispatchDescHeader* prev = &d.header;
  const auto link = [&prev](auto& desc) {
    prev->pNext = &desc.header;
    prev = &desc.header;
  };
  // One description per signal of the context, in the order of the flags.
  ffxDispatchDescDenoiserAmbientOcclusion ao{};
  ffxDispatchDescDenoiserDirectDiffuse dd{};
  ffxDispatchDescDenoiserDirectSpecular ds{};
  ffxDispatchDescDenoiserIndirectDiffuse id{};
  ffxDispatchDescDenoiserIndirectSpecular is{};
  ffxDispatchDescDenoiserSpecularOcclusion so{};
  const auto fill = [&](Signal s, FfxApiDenoiserSignal* out) {
    const SignalImages& images_of = frame.signals[size_t(s)];
    out->checkerboardOrigin = 0;
    return MakeResource(images_of.input, in_state, &out->input) && MakeResource(images_of.output, out_state, &out->output);
  };
  if (HasFlag(context->signals, Signal::kAmbientOcclusion)) {
    ao.header.type = FFX_API_DISPATCH_DESC_TYPE_DENOISER_AMBIENT_OCCLUSION;
    images = images && fill(Signal::kAmbientOcclusion, &ao.signal);
    link(ao);
  }
  if (HasFlag(context->signals, Signal::kDirectDiffuse)) {
    dd.header.type = FFX_API_DISPATCH_DESC_TYPE_DENOISER_DIRECT_DIFFUSE;
    images = images && fill(Signal::kDirectDiffuse, &dd.signal);
    link(dd);
  }
  if (HasFlag(context->signals, Signal::kDirectSpecular)) {
    ds.header.type = FFX_API_DISPATCH_DESC_TYPE_DENOISER_DIRECT_SPECULAR;
    images = images && fill(Signal::kDirectSpecular, &ds.signal);
    link(ds);
  }
  if (HasFlag(context->signals, Signal::kIndirectDiffuse)) {
    id.header.type = FFX_API_DISPATCH_DESC_TYPE_DENOISER_INDIRECT_DIFFUSE;
    images = images && fill(Signal::kIndirectDiffuse, &id.signal);
    link(id);
  }
  if (HasFlag(context->signals, Signal::kIndirectSpecular)) {
    is.header.type = FFX_API_DISPATCH_DESC_TYPE_DENOISER_INDIRECT_SPECULAR;
    images = images && fill(Signal::kIndirectSpecular, &is.signal);
    link(is);
  }
  if (HasFlag(context->signals, Signal::kSpecularOcclusion)) {
    so.header.type = FFX_API_DISPATCH_DESC_TYPE_DENOISER_SPECULAR_OCCLUSION;
    images = images && fill(Signal::kSpecularOcclusion, &so.signal);
    link(so);
  }
  if (!images) {
    return refuse("an image has no native object or a format the runtime does not know");
  }
  // ffx::Dispatch (the C++ helper) resets pNext: the C function is called with the chain built above.
  const ffxReturnCode_t rc = i.dispatch(&context->handle, &d.header);
  // The runtime recorded commands of its own and may have bound heaps and root state: the host restores the list's state (the caller of an upscaler does the same).
  backend::InvalidateAfterExternalWork(list);
  if (rc != FFX_API_RETURN_OK) {
    if (i.book.Note(rr::FailureBook::Event::kError)) {
      i.Fail(std::format("ffxDispatch returned {} ({}) three times in a row", rc, ReturnName(rc)));
    }
    if (!i.refusal_logged) {
      i.refusal_logged = true;
      REXLOG_WARN("{} ffxDispatch returned {} ({}) (logged once)", kPrefix, rc, ReturnName(rc));
    }
    return false;
  }
  i.book.Note(rr::FailureBook::Event::kRan);
  if (i.dispatches++ == 0) {
    REXLOG_INFO("{} first dispatch recorded: signals {:#x} at {}x{}, frame index {}, reset {}, jitter {:+.3f}, {:+.3f}, depth bounds {:.1f} .. {:.1f}, albedo {}", kPrefix, context->signals,
                frame.render_size[0], frame.render_size[1], step.index, step.reset ? "yes" : "no", frame.jitter[0], frame.jitter[1], frame.depth_bounds.min, frame.depth_bounds.max,
                frame.shared_linear_albedo ? "linear (non-encoded flag)" : "sqrt-encoded");
  }
  return true;
}

void RayRegen::Shutdown() {
  Impl& i = *impl_;
  {
    std::lock_guard lock(i.mu);
    i.stop = true;
  }
  i.cv.notify_all();
  if (i.worker.joinable()) {
    i.worker.join();
  }
  // The modules stay loaded to the end of the process: the loader may hold the denoiser.
  i.denoiser = nullptr;
  i.loader = nullptr;
  i.frames = nullptr;
  i.probe_started = false;
}

RayRegen& Instance() {
  static RayRegen runtime;
  return runtime;
}

void StartIfWanted(Device& device, FrameRing& frames) {
  rr::DebugSwitch debug = rr::DebugSwitch::kRow;
  if (!rr::ParseDebugSwitch(larecomp::settings::mcla_gpu_ray_regen.Get(), &debug)) {
    debug = rr::DebugSwitch::kRow;
  }
  if (!ProbeWanted(debug, AnySignalBuilt())) {
    return;
  }
  REXLOG_INFO("{} mcla_gpu_ray_regen = {}: asking this device", kPrefix, rr::DebugSwitchName(debug));
  Instance().Probe(device, frames);
}

void StopAll() {
  Instance().Shutdown();
}

}  // namespace mcla::gpu::rayregen
