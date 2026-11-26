// MCLA GPU renderer -- the device. See device.h.

#include "device.h"

#include "crash_diag_rules.h"
#include "device_fault_format.h"
#include "format.h"
#include "car_rt_scene.h"
#include "gpu_class.h"
#include "rt_tier.h"
#include "temporal_dlss.h"
#if defined(LARECOMP_HAS_FFX)
#include "temporal_ffx.h"
#endif
#include "vulkan_pipeline_cache.h"
#include "windows_floor.h"

#include <fstream>
#include <mutex>

#include "settings/larecomp_settings.h"
#include "spdlog_console.h"
#include <rex/logging/log_category.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif


namespace mcla::gpu {

namespace {

// The Windows version and the update revision of its build (windows_floor.h); zeros where they cannot
// be read (not Windows, or a failed call).
windows_floor::WindowsBuild ReadWindowsBuild() {
  windows_floor::WindowsBuild out;
#if defined(_WIN32)
  struct VersionInfo {
    ULONG size, major, minor, build, platform;
    WCHAR service_pack[128];
  } info{};
  info.size = sizeof(info);
  using RtlGetVersionFn = LONG(WINAPI*)(VersionInfo*);
  if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
    if (auto get_version = reinterpret_cast<RtlGetVersionFn>(
            reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")))) {
      if (get_version(&info) == 0) {
        out.major = info.major;
        out.minor = info.minor;
        out.build = info.build;
      }
    }
  }
  DWORD revision = 0;
  DWORD size = sizeof(revision);
  if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"UBR",
                   RRF_RT_REG_DWORD, nullptr, &revision, &size) == ERROR_SUCCESS) {
    out.revision = revision;
  }
#endif
  return out;
}

// What a refused Direct3D 12 start says about this Windows, "" when the Windows is not the cause
// (windows_floor.h: the game's own Direct3D 12 runtime needs Windows 10 1909 with its update).
std::string D3D12WindowsHint() {
  return windows_floor::Explain(ReadWindowsBuild());
}

// Why debug names are wanted (Device::object_names), or null when nothing is known to read them.
const char* ObjectNamesReason(bool debug_layer) {
  if (larecomp::settings::mcla_gpu_object_names.Get()) {
    return "mcla_gpu_object_names";
  }
  if (debug_layer) {
    return "the debug layer";
  }
  if (larecomp::settings::mcla_gpu_perf_markers.Get()) {
    return "mcla_gpu_perf_markers";
  }
#ifndef NDEBUG
  // Plume names Vulkan objects in the same builds.
  return "a build without NDEBUG";
#else
#if defined(_WIN32)
  // A tool that starts the game injects its DLL before the device exists, so the module is
  // there when this runs. (Vulkan names exist only in a Plume build without NDEBUG: on Vulkan
  // this changes nothing but the formatting the callers skip.)
  static constexpr struct {
    const wchar_t* module;
    const char* tool;
  } kTools[] = {
      {L"renderdoc.dll", "RenderDoc"},
      {L"WinPixGpuCapturer.dll", "PIX"},
      // CHECK: the module name is from memory of Nsight Graphics' interception layer; not seen
      // loaded here. A tool this misses is covered by mcla_gpu_object_names.
      {L"Nvda.Graphics.Interception.dll", "Nsight Graphics"},
  };
  for (const auto& tool : kTools) {
    if (GetModuleHandleW(tool.module)) {
      return tool.tool;
    }
  }
#endif
  return nullptr;
#endif
}

}  // namespace

uint32_t WindowsBuildNumber() { return ReadWindowsBuild().build; }

Device::Device() = default;

Device::~Device() {
  Shutdown();
}

void Device::Refuse(const std::string& what) {
  const std::string line =
      std::string(ActiveBackend() == Backend::kD3D12 ? "Direct3D 12" : "Vulkan") + what;
  if (refusals_.find(line) != std::string::npos) {
    return;  // the same backend tried again (Streamline's hook dropped): said once
  }
  refusals_ += (refusals_.empty() ? "" : "\n") + line;
}

bool Device::Initialize(const Options& options) {
  if (device_) {
    return true;
  }
  if (options.game_session) {
    loss_report_path_ = LarecompLogFile();
    const std::filesystem::path log_dir = loss_report_path_.empty()
        ? LarecompLogFolder() : loss_report_path_.parent_path();
    loss_marker_path_ = log_dir / "gpu-device-lost";
    if (loss_report_path_.empty()) {
      loss_report_path_ = log_dir / "larecomp.log";
    }
    loss_report_path_.replace_extension(".gpu-crash.txt");
    std::error_code marker_error;
    // CHECK: after a real loss, Auto enables capture for exactly one clean restart.
    const bool previous_loss = device_fault::ConsumeLossMarker(loss_marker_path_, marker_error);
    if (marker_error) {
      REXLOG_WARN("[gpu] could not consume device-loss marker: {}", marker_error.message());
    }
    const std::string mode = larecomp::settings::mcla_gpu_crash_diag.Get();
    crash_diagnostics_requested_ = device_fault::CrashDiagnosticsEnabled(mode, previous_loss);
    REXLOG_INFO("[gpu] crash diagnostics: {} -> {} (previous device loss: {})", mode,
                crash_diagnostics_requested_ ? "on" : "off", previous_loss ? "yes" : "no");
  }
  requested_backend_ = options.backend;
  // The backend asked for, then the other one the build has. Running another
  // than the one asked for is never quiet: logged here, and the caller tells
  // the user (fell_back()).
  const Backend order[2] = {
      options.backend,
      options.backend == Backend::kD3D12 ? Backend::kVulkan : Backend::kD3D12};
  for (const Backend backend : order) {
    if (!BackendCompiled(backend)) {
      continue;
    }
    if (backend != options.backend) {
      REXLOG_ERROR("[gpu] {} could not be started (graphics_backend asks for it; the reason is "
                   "above); trying {}",
                   BackendName(options.backend), BackendName(backend));
    }
    SelectBackend(backend);
    if (Open(options)) {
      return true;
    }
    // A Vulkan interface or device that was made through NVIDIA Streamline's hook (DLSS is the
    // choice, temporal_dlss.h) and could not be made: once more without it, before another
    // backend is tried. DLSS is then off for the rest of the process.
    if (backend == Backend::kVulkan && dlss::VulkanInstanceProcAddr() != nullptr) {
      dlss::AbandonVulkanHook(
          "the Vulkan instance or device could not be made through Streamline's functions");
      if (Open(options)) {
        return true;
      }
    }
  }
  REXLOG_ERROR("[gpu] no graphics backend could be started");
  return false;
}

bool Device::Open(const Options& options) {
  query_readback_warned_ = false;
  if (options.force_refusal) {
    // The test of the compatibility renderer offer: the failure path as a PC that cannot run the renderer takes it, nothing created.
    REXLOG_WARN("[gpu] mcla_gpu_force_refusal: {} is refused on purpose (a test of the compatibility renderer offer)", BackendName());
    Refuse(": refused on purpose (mcla_gpu_force_refusal, a test)");
    return false;
  }
  // After nothing, before the interface: the debug layer has to be enabled
  // before the first D3D12 device of the process exists.
  const bool debug_layer = options.debug_layer && backend::EnableDebugLayer();
  if (debug_layer) {
    REXLOG_INFO("[gpu] {} debug layer enabled (mcla_gpu_debug_layer)", BackendName());
  }
  crash_diagnostics_ = crash_diagnostics_requested_ && backend::EnableCrashDiagnostics();

  // NVIDIA Streamline on Vulkan has to start before the instance exists (only when DLSS is the
  // saved choice, on an NVIDIA GPU; nothing otherwise, and nothing on D3D12).
  dlss::BeforeDevice(ActiveBackend(), options.preferred_adapter);

  interface_ = backend::CreateInterface();
  if (!interface_) {
    REXLOG_ERROR("[gpu] Plume could not create the {} interface", BackendName());
    const std::string windows = ActiveBackend() == Backend::kD3D12 ? D3D12WindowsHint() : std::string();
    if (!windows.empty()) {
      REXLOG_ERROR("[gpu] Direct3D 12 could not be started, and {}", windows);
    }
    Refuse(windows.empty()
               ? std::string(": the graphics driver could not be started (is it installed and up to date?)")
               : ": the driver could not be started, and " + windows);
    return false;
  }

  // The user's adapter always wins; without one, the backend may name its own choice
  // (backend::DefaultAdapterName), and an empty name leaves it to Plume.
  const std::string adapter = options.preferred_adapter.empty() ? backend::DefaultAdapterName()
                                                                 : options.preferred_adapter;
  device_ = interface_->createDevice(adapter);
  if (!device_) {
    REXLOG_ERROR("[gpu] Plume could not create a {} device{}", BackendName(),
                 adapter.empty() ? std::string() : " on adapter '" + adapter + "'");
    // On Windows 10 before 1909 or its update the game's own Direct3D 12 runtime (the D3D12 folder next to
    // the exe) is not accepted and the device cannot be made: say that and not "no card has a driver".
    // CHECK: on a Windows 10 2004 before 19041.804 (or 1909 before 18363.1377) the box and the log name
    // the update; on a current Windows 10 or 11 this branch says what it always did.
    const std::string windows = ActiveBackend() == Backend::kD3D12 ? D3D12WindowsHint() : std::string();
    if (!windows.empty()) {
      REXLOG_ERROR("[gpu] Direct3D 12 could not be started, and {}", windows);
    }
    Refuse(windows.empty() ? std::string(": no graphics card here has a driver for it")
                           : ": the device could not be made, and " + windows);
    interface_.reset();
    return false;
  }
  if (!adapter.empty() && device_->getDescription().name != adapter) {
    // Plume lets a later adapter with more dedicated video memory replace the named one.
    REXLOG_WARN("[gpu] adapter '{}' was asked for but the device is on '{}'", adapter,
                device_->getDescription().name);
  }
  backend_info_ = backend::DescribeDevice(device_.get());
  {
    // Once: which Windows this log is from (the Direct3D 12 runtime's floor, windows_floor.h).
    static std::once_flag windows_logged;
    std::call_once(windows_logged, [] {
      const windows_floor::WindowsBuild windows = ReadWindowsBuild();
      if (windows.build != 0) {
        REXLOG_INFO("[gpu] {}", windows_floor::Describe(windows));
      }
    });
  }
  debug_layer_ = debug_layer;
  backend::InstallDebugMessageSink(device_.get(), debug_layer_);
  const char* names_reason =
      crash_diagnostics_ ? "GPU crash diagnostics" : ObjectNamesReason(debug_layer_);
  names_ = names_reason != nullptr;
  if (names_) {
    REXLOG_INFO("[gpu] object names on ({})", names_reason);
  } else {
    REXLOG_INFO("[gpu] object names off (no capture tool is loaded; mcla_gpu_object_names sets "
                "them)");
  }

  // The XenosRecomp shaders read their constant buffers through buffer device
  // addresses on Vulkan (shader_common.h: vk::RawBufferLoad off push-constant
  // addresses), and every shader of this renderer samples through the bindless
  // sets (descriptors.h), which need descriptor indexing. A device without
  // either cannot run a single draw, so it is refused here rather than at the
  // first one. D3D12 has both by definition at the feature level Plume asks for.
  if (ActiveBackend() == Backend::kVulkan) {
    const auto& caps = device_->getCapabilities();
    const char* missing = !caps.bufferDeviceAddress ? "buffer device address"
                          : !caps.descriptorIndexing ? "descriptor indexing"
                                                     : nullptr;
    if (missing) {
      REXLOG_ERROR("[gpu] {} on {} has no {}; this renderer needs it", backend_info_,
                   device_->getDescription().name, missing);
      Refuse(" on " + device_->getDescription().name + ": it has no " + missing);
      device_.reset();
      interface_.reset();
      return false;
    }
  }
  // The rest of what the renderer needs that Plume does not check (backend.h): refused here, in
  // words, rather than as a root signature or a pipeline that fails afterwards.
  const std::string problem = backend::RequirementProblem(device_.get());
  if (!problem.empty()) {
    REXLOG_ERROR("[gpu] {} on {} cannot run this renderer: {}", backend_info_,
                 device_->getDescription().name, problem);
    Refuse(" on " + device_->getDescription().name + ": " + problem);
    device_.reset();
    interface_.reset();
    return false;
  }

  queue_ = device_->createCommandQueue(plume::RenderCommandListType::DIRECT);
  if (backend::IsNullBacked(queue_.get())) {
    REXLOG_ERROR("[gpu] Plume could not create the direct queue on {}", backend_info_);
    Refuse(" on " + device_->getDescription().name +
           ": the graphics card would not take a command queue");
    queue_.reset();
    device_.reset();
    interface_.reset();
    return false;
  }
  if (crash_diagnostics_) {
    backend::NameCrashQueue(queue_.get());
  }

  if (ActiveBackend() == Backend::kVulkan &&
      larecomp::settings::mcla_gpu_vulkan_pipeline_cache.Get()) {
    auto cache = std::make_unique<VulkanPipelineCache>();
    if (cache->Initialize(*device_)) {
      vulkan_pipeline_cache_ = std::move(cache);
    }
  }

  loss_probe_ = backend::CreateLossProbe(device_.get());
  REXLOG_INFO("[gpu] device loss: {}",
              backend::DescribeLossReporting(device_.get(), loss_probe_, crash_diagnostics_));

  const auto& caps = device_->getCapabilities();
  const auto vram = QueryVideoMemory();
  REXLOG_INFO(
      "[gpu] {} on {} | dedicated VRAM {} MiB, budget {} | bindless={} bda={} "
      "present_wait={} gpu_upload_heap={} uma={}",
      backend_info_, device_->getDescription().name,
      device_->getDescription().dedicatedVideoMemory >> 20,
      vram.budget ? std::to_string(vram.budget >> 20) + " MiB" : std::string("not reported"),
      caps.descriptorIndexing, caps.bufferDeviceAddress, caps.presentWait, caps.gpuUploadHeap,
      caps.uma);
  // How that budget was made (which heaps or segments, the RAM cap and the floor of a shared-memory
  // GPU), once: the answer to "is the budget the caches trim against the memory the GPU can use".
  REXLOG_INFO("[gpu] {}", backend::DescribeVideoMemory(device_.get()));
  // What a compatibility report asks of a player's log (perf/COMPAT-AUDIT.md section 9): one line.
  if (const std::string capabilities = backend::DescribeCapabilities(device_.get());
      !capabilities.empty()) {
    REXLOG_INFO("[gpu] capabilities: {}", capabilities);
  }
  // Adapter classification is used for the recommended preset and diagnostics.
  gpu_class::Initialize(device_->getDescription(), caps);
  // The ray tracing tier beside it (rt_tier.h): whether this renderer can trace rays on the device (the same answer the car's ray tracing asks for:
  // CarRtSupported), the PCI device id Plume's description does not carry, and the tables; its own "[gpu] ray tracing tier: ..." line says which and
  // why. The benchmark, if it runs, comes later (the renderer's start, rt_bench.h).
  {
    gpu_class::RtProbe probe;
    probe.vulkan = ActiveBackend() == Backend::kVulkan;
    probe.api_reason = CarRtUnsupportedReason(caps, probe.vulkan, wave_ops());
    probe.api_rt = probe.api_reason.empty();
    probe.device_id = backend::QueryAdapterIds(device_.get()).device_id;
    probe.driver_version = device_->getDescription().driverVersion;
    gpu_class::InitializeRt(device_->getDescription(), caps, probe);
  }

  // A kD24S8 surface (the shadow atlas) is a D24_UNORM_S8_UINT target on the host. AMD's
  // Vulkan driver lists no 24-bit depth format, yet creates the image when asked; nothing
  // drawn into it can be read back, so the sun's shadow lookup finds nothing and every
  // pixel is in shadow. Plume reports from the format's features whether the device has
  // it (the creation call is no test); where it does not, kD24S8 is kept as
  // D32_FLOAT_S8_UINT, which the spec requires of any device that lacks D24. D3D12 always
  // reports it.
  format::SetDepth24Unsupported(!caps.depth24Stencil8);
  REXLOG_INFO("[gpu] depth: D24_UNORM_S8_UINT targets {}",
              caps.depth24Stencil8 ? "are used"
                                   : "are not listed: kD24S8 surfaces use D32_FLOAT_S8_UINT");
  return true;
}

void Device::Shutdown(const std::function<void(const char*)>& part) {
  if (vulkan_pipeline_cache_) {
    if (part) part("the Vulkan pipeline cache");
    SaveVulkanPipelineCache();
    vulkan_pipeline_cache_->LogSummary();
    vulkan_pipeline_cache_.reset();
  }
  // With the debug layer on, what is still alive once every wrapper is gone is
  // a leak; the report lists it by name.
  backend::LiveObjectReport report;
  if (debug_layer_) {
    report.Capture(device_.get());
  }
  // Each part is one call into the driver. The caller times them (renderer.cpp ShutdownClock): on Vulkan the
  // device's release is vmaDestroyAllocator and vkDestroyDevice, the interface's is vkDestroyInstance; which of
  // them took the seconds of the slow closes of 2026-11-20 to 11-22 is not known yet.
  if (part) part("the queue and the loss probe");
  queue_.reset();
  if (device_) {
    backend::DestroyLossProbe(device_.get(), loss_probe_);
  }
  loss_probe_ = nullptr;
  if (part) part("the device (the driver's own teardown)");
  device_.reset();
  if (part) part("the graphics interface (the instance)");
  interface_.reset();
  report.ReportAndRelease();
  debug_layer_ = false;
}

bool Device::LoadedVulkanPipelineCache() const {
  return vulkan_pipeline_cache_ && vulkan_pipeline_cache_->loaded();
}

bool Device::SaveVulkanPipelineCache() {
  return !vulkan_pipeline_cache_ || vulkan_pipeline_cache_->Save(*device_, IsLost());
}

std::unique_ptr<plume::RenderTexture> Device::CreateTexture(const plume::RenderTextureDesc& desc,
                                                            const char* name) {
  if (!device_ || IsLost()) {
    return nullptr;
  }
  const uint64_t backoff_key = BackoffKey(desc);
  if (Blocked(backoff_key)) {
    return nullptr;
  }
  auto texture = device_->createTexture(desc);
  if (backend::IsNullBacked(texture.get())) {
    if (LogThisFailure()) {
      REXLOG_ERROR("[gpu] texture '{}' failed: {}x{}x{} mips={} fmt={} flags={:#x} samples={}",
                   name ? name : "?", desc.width, desc.height, desc.depth, desc.mipLevels,
                   static_cast<uint32_t>(desc.format), static_cast<uint32_t>(desc.flags),
                   static_cast<uint32_t>(desc.multisampling.sampleCount));
    }
    ReportCreationFailure("texture", name);
    NoteFailed(backoff_key, TextureBytes(desc), /*video_memory=*/true);
    return nullptr;
  }
  Name(*texture, name);
  return texture;
}

std::unique_ptr<plume::RenderTextureView> Device::CreateTextureView(
    const plume::RenderTexture& texture, const plume::RenderTextureViewDesc& desc,
    const char* name) {
  if (!device_ || IsLost()) {
    return nullptr;
  }
  auto view = texture.createTextureView(desc);
  if (backend::IsNullBacked(view.get())) {
    if (LogThisFailure()) {
      REXLOG_ERROR("[gpu] texture view of '{}' failed: fmt={} dim={} mips={}", name ? name : "?",
                   static_cast<uint32_t>(desc.format), static_cast<uint32_t>(desc.dimension),
                   desc.mipLevels);
    }
    ReportCreationFailure("texture view", name);
    return nullptr;
  }
  return view;
}

std::unique_ptr<plume::RenderSampler> Device::CreateSampler(const plume::RenderSamplerDesc& desc,
                                                            const char* name) {
  if (!device_ || IsLost()) {
    return nullptr;
  }
  auto sampler = device_->createSampler(desc);
  if (backend::IsNullBacked(sampler.get())) {
    if (LogThisFailure()) {
      REXLOG_ERROR("[gpu] sampler '{}' failed", name ? name : "?");
    }
    ReportCreationFailure("sampler", name);
    return nullptr;
  }
  return sampler;
}

std::unique_ptr<plume::RenderDescriptorSet> Device::CreateDescriptorSet(
    const plume::RenderDescriptorSetBuilder& builder, uint32_t view_count, uint32_t sampler_count,
    const char* name) {
  if (!device_ || IsLost()) {
    return nullptr;
  }
  auto set = builder.create(device_.get());
  if (!backend::DescriptorSetAllocated(set.get(), view_count, sampler_count)) {
    if (LogThisFailure()) {
      REXLOG_ERROR(
          "[gpu] descriptor set '{}' failed: {} views + {} samplers (the shader-visible heaps are "
          "shared by every set; a full heap is the usual cause)",
          name ? name : "?", view_count, sampler_count);
    }
    ReportCreationFailure("descriptor set", name);
    return nullptr;
  }
  return set;
}

std::unique_ptr<plume::RenderPipelineLayout> Device::CreatePipelineLayout(
    const plume::RenderPipelineLayoutBuilder& builder, const char* name) {
  if (!device_ || IsLost()) {
    return nullptr;
  }
  auto layout = builder.create(device_.get());
  if (backend::IsNullBacked(layout.get())) {
    if (LogThisFailure()) {
      REXLOG_ERROR("[gpu] pipeline layout '{}' failed", name ? name : "?");
    }
    ReportCreationFailure("pipeline layout", name);
    return nullptr;
  }
  return layout;
}

std::unique_ptr<plume::RenderShader> Device::CreateShader(const void* data, uint64_t size,
                                                          const char* entry_point,
                                                          const char* name) {
  if (!device_ || IsLost()) {
    return nullptr;
  }
  if (!data || size == 0) {
    if (LogThisFailure()) {
      REXLOG_ERROR("[gpu] shader '{}' has no bytecode", name ? name : "?");
    }
    return nullptr;
  }
  const plume::RenderShaderFormat format = HostShaderFormat();
  auto shader = device_->createShader(data, size, entry_point, format);
  if (backend::IsNullBacked(shader.get())) {
    if (LogThisFailure()) {
      REXLOG_ERROR("[gpu] shader '{}' failed: {} bytes of {}", name ? name : "?", size,
                   format == plume::RenderShaderFormat::DXIL ? "DXIL" : "SPIR-V");
    }
    ReportCreationFailure("shader", name);
    return nullptr;
  }
  Name(*shader, name);
  return shader;
}

std::unique_ptr<plume::RenderQueryPool> Device::CreateQueryPool(uint32_t query_count,
                                                                const char* name,
                                                                plume::RenderQueryType type) {
  if (!device_ || IsLost()) {
    return nullptr;
  }
  auto pool = device_->createQueryPool(query_count, type);
  if (!pool || backend::IsNullBacked(pool.get())) {
    if (LogThisFailure()) {
      REXLOG_ERROR("[gpu] query pool '{}' of {} {} queries failed", name ? name : "?", query_count,
                   type == plume::RenderQueryType::OCCLUSION ? "occlusion" : "timestamp");
    }
    ReportCreationFailure("query pool", name);
    return nullptr;
  }
  return pool;
}

const uint64_t* Device::ReadQueryResults(plume::RenderQueryPool* pool) {
  if (!pool || IsLost()) {
    return nullptr;
  }
  const auto readback = backend::ReadQueryResults(*pool);
  if (!readback.values) {
    const bool lost = CheckLost("query readback");
    if (!query_readback_warned_ || lost) {
      REXLOG_WARN("[gpu] query readback failed ({:#010x}, {} queries)",
                  readback.error, pool->getCount());
      query_readback_warned_ = true;
    }
  }
  return readback.values;
}

std::unique_ptr<plume::RenderFramebuffer> Device::CreateFramebuffer(
    const plume::RenderFramebufferDesc& desc, const char* name) {
  if (!device_ || IsLost()) {
    return nullptr;
  }
  auto framebuffer = device_->createFramebuffer(desc);
  if (backend::IsNullBacked(framebuffer.get())) {
    if (LogThisFailure()) {
      REXLOG_ERROR("[gpu] framebuffer '{}' of {} colour targets{} failed", name ? name : "?",
                   desc.colorAttachmentsCount, desc.depthAttachment ? " and depth" : "");
    }
    ReportCreationFailure("framebuffer", name);
    return nullptr;
  }
  return framebuffer;
}

std::unique_ptr<plume::RenderBuffer> Device::CreateBuffer(const plume::RenderBufferDesc& desc,
                                                          const char* name) {
  if (!device_ || IsLost()) {
    return nullptr;
  }
  const uint64_t backoff_key = BackoffKey(desc);
  if (Blocked(backoff_key)) {
    return nullptr;
  }
  auto buffer = device_->createBuffer(desc);
  if (backend::IsNullBacked(buffer.get())) {
    if (LogThisFailure()) {
      REXLOG_ERROR("[gpu] buffer '{}' failed: {} bytes heap={} flags={:#x}", name ? name : "?",
                   desc.size, static_cast<uint32_t>(desc.heapType),
                   static_cast<uint32_t>(desc.flags));
    }
    ReportCreationFailure("buffer", name);
    NoteFailed(backoff_key, desc.size, desc.heapType == plume::RenderHeapType::DEFAULT);
    return nullptr;
  }
  Name(*buffer, name);
  return buffer;
}

std::unique_ptr<plume::RenderPipeline> Device::CreateGraphicsPipeline(
    const plume::RenderGraphicsPipelineDesc& desc, const char* name) {
  if (!device_ || IsLost()) {
    return nullptr;
  }
  std::string failure;
  auto pipeline = backend::CreateGraphicsPipeline(*device_, desc, failure);
  if (!pipeline) {
    REXLOG_ERROR("[gpu] graphics pipeline '{}' failed: {}", name ? name : "?", failure);
    LogThisFailure();  // Rate-limit the follow-up VRAM query and report.
    ReportCreationFailure("graphics pipeline", name);
    return nullptr;
  }
  if (vulkan_pipeline_cache_) {
    vulkan_pipeline_cache_->NotePipelineCreated();
  }
  Name(*pipeline, name);
  return pipeline;
}

std::unique_ptr<plume::RenderPipeline> Device::CreateComputePipeline(
    const plume::RenderComputePipelineDesc& desc, const char* name) {
  if (!device_ || IsLost()) {
    return nullptr;
  }
  auto pipeline = device_->createComputePipeline(desc);
  if (backend::IsNullBackedCompute(pipeline.get())) {
    if (LogThisFailure()) {
      REXLOG_ERROR("[gpu] compute pipeline '{}' failed", name ? name : "?");
    }
    ReportCreationFailure("compute pipeline", name);
    return nullptr;
  }
  if (vulkan_pipeline_cache_) {
    vulkan_pipeline_cache_->NotePipelineCreated();
  }
  Name(*pipeline, name);
  return pipeline;
}

bool Device::CheckLost(const char* context) {
  if (IsLost()) {
    return true;
  }
  if (!device_) {
    return false;
  }
  const int64_t reason = backend::DeviceRemovedReason(device_.get(), loss_probe_);
  if (reason == 0) {
    return false;
  }
  const std::lock_guard lock(loss_report_mutex_);
  if (!lost_.exchange(true, std::memory_order_acq_rel)) {
    // Persist the loss before asking the driver for details. The renderer can
    // append its own trail later, even if another thread first detected loss.
    if (!loss_marker_path_.empty()) {
      std::ofstream marker(loss_marker_path_, std::ios::binary);
      marker.close();
      if (!marker) {
        REXLOG_WARN("[gpu] could not write device-loss marker");
      }
    }
    const std::string summary = std::format(
        "device lost, noticed at {}: reason {} (frame {}, {} on {})",
        context ? context : "?",
        device_fault::RemovedReasonText(ActiveBackend() == Backend::kVulkan, reason),
        frame(), BackendName(), device_->getDescription().name);
    WriteLossReport(std::span(&summary, 1), false);
    rex::log::Flush();
    if (crash_diagnostics_ || (loss_report_path_.empty() && ActiveBackend() == Backend::kVulkan)) {
      WriteLossReport(backend::DescribeDeviceFault(device_.get()), true);
    }
    if (!loss_report_path_.empty()) {
      REXLOG_ERROR("[gpu] device-loss report: {}", loss_report_path_.filename().string());
    }
    rex::log::Flush();
  }
  return true;
}

void Device::WriteLossReport(std::span<const std::string> lines, bool append) {
  if (lines.empty()) {
    return;
  }
  for (const std::string& line : lines) {
    REXLOG_ERROR("[gpu] {}", line);
  }
  if (loss_report_path_.empty()) {
    return;
  }
  std::ofstream report(loss_report_path_, append ? std::ios::app : std::ios::trunc);
  for (const std::string& line : lines) {
    report << line << '\n';
  }
  report.close();
  if (!report) {
    REXLOG_WARN("[gpu] could not write device-loss report");
  }
}

void Device::AppendLossReport(std::span<const std::string> lines) {
  const std::lock_guard lock(loss_report_mutex_);
  WriteLossReport(lines, true);
  rex::log::Flush();
}

void Device::MaybeForceRemoval() {
  if (force_removal_attempted_ || loss_report_path_.empty() || frame() < 120 ||
      !larecomp::settings::mcla_gpu_force_device_removal.Get() || !device_ || IsLost()) {
    return;
  }
  force_removal_attempted_ = true;
  REXLOG_WARN("[gpu] mcla_gpu_force_device_removal: requesting removal after frame submit");
  if (!backend::ForceDeviceRemoval(device_.get())) {
    REXLOG_WARN("[gpu] this backend or runtime cannot force device removal");
  }
}

namespace {
// The failure the calling thread is reporting: logged, or only counted.
thread_local bool t_log_failure = true;
}  // namespace

bool Device::LogThisFailure() {
  // A creation that fails keeps failing while its cause lasts (a full VRAM
  // budget asks for the same texture every frame): the first 16 are logged in
  // full, then one in 256 with the count, instead of two lines and a budget
  // query per failure.
  const uint64_t n = ++creation_failures_;
  t_log_failure = n <= 16 || n % 256 == 0;
  if (n == 16) {
    REXLOG_ERROR("[gpu] 16 creations have failed: from now on one in 256 is logged");
  } else if (n > 16 && t_log_failure) {
    REXLOG_ERROR("[gpu] {} creations have failed so far", n);
  }
  return t_log_failure;
}

void Device::ReportCreationFailure(const char* what, const char* name) {
  if (CheckLost(what)) {
    return;
  }
  if (!t_log_failure) {
    return;
  }
  // Device alive and the allocation still failed: memory, or an invalid
  // description the backend refused. The two need opposite fixes, so the
  // budget is printed to tell them apart.
  const auto vram = QueryVideoMemory();
  REXLOG_ERROR("[gpu] {} '{}' failed with the device alive: VRAM {} of {} MiB in use",
               what, name ? name : "?", vram.used >> 20, vram.budget >> 20);
}

backend::VideoMemory Device::QueryVideoMemory() const {
  backend::VideoMemory memory =
      device_ ? backend::QueryVideoMemory(device_.get()) : backend::VideoMemory{};
  // mcla_gpu_vram_budget_mb (debug): a smaller budget than the adapter's, for the tests of a small card.
  const uint64_t pretend = uint64_t(larecomp::settings::mcla_gpu_vram_budget_mb.Get()) << 20;
  if (pretend != 0 && memory.budget > pretend) {
    memory.budget = pretend;
  }
  return memory;
}

namespace {

constexpr uint64_t Mix(uint64_t hash, uint64_t value) {
  return hash ^ (value + 0x9E3779B97F4A7C15ull + (hash << 6) + (hash >> 2));
}

}  // namespace

uint64_t Device::BackoffKey(const plume::RenderTextureDesc& desc) {
  uint64_t key = 0x7465787475726500ull;
  key = Mix(key, uint64_t(desc.dimension));
  key = Mix(key, uint64_t(desc.width) << 32 | desc.height);
  key = Mix(key, uint64_t(desc.depth) << 32 | desc.mipLevels);
  key = Mix(key, uint64_t(desc.arraySize) << 32 | uint64_t(desc.format));
  key = Mix(key, uint64_t(desc.flags) << 32 | uint64_t(desc.multisampling.sampleCount));
  key = Mix(key, uint64_t(desc.textureArrangement) << 1 | (desc.committed ? 1u : 0u));
  return key;
}

uint64_t Device::BackoffKey(const plume::RenderBufferDesc& desc) {
  uint64_t key = 0x627566666572ull;
  key = Mix(key, desc.size);
  key = Mix(key, uint64_t(desc.heapType) << 32 | uint64_t(desc.flags));
  key = Mix(key, desc.committed ? 1u : 0u);
  return key;
}

bool Device::Blocked(uint64_t key) {
  if (backoff_count_.load(std::memory_order_relaxed) == 0) {
    return false;
  }
  std::lock_guard<std::mutex> lock(backoff_mutex_);
  const auto it = backoff_.find(key);
  if (it == backoff_.end()) {
    return false;
  }
  const Backoff& entry = it->second;
  const uint64_t now = frame();
  // Enough memory was given back, and long enough ago for the GPU to have let go of it.
  const bool freed =
      released_bytes() - entry.released_at >= entry.bytes &&
      now >= release_frame_.load(std::memory_order_relaxed) + kReleaseSettleFrames;
  if (now < entry.retry_frame && !freed) {
    skipped_.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  backoff_.erase(it);
  backoff_count_.store(uint32_t(backoff_.size()), std::memory_order_relaxed);
  return false;
}

void Device::NoteFailed(uint64_t key, uint64_t bytes, bool video_memory) {
  if (IsLost()) {
    return;
  }
  if (video_memory) {
    uint64_t seen = failed_bytes_.load(std::memory_order_relaxed);
    while (seen < bytes &&
           !failed_bytes_.compare_exchange_weak(seen, bytes, std::memory_order_relaxed)) {
    }
  }
  std::lock_guard<std::mutex> lock(backoff_mutex_);
  if (backoff_.size() >= kBackoffEntries) {
    backoff_.clear();
  }
  Backoff& entry = backoff_[key];
  entry.retry_frame = frame() + (video_memory ? kBackoffFrames : kUploadBackoffFrames);
  entry.released_at = released_bytes();
  entry.bytes = std::max<uint64_t>(bytes, 1);
  backoff_count_.store(uint32_t(backoff_.size()), std::memory_order_relaxed);
}

void Device::NoteReleased(uint64_t bytes) {
  if (bytes == 0) {
    return;
  }
  released_bytes_.fetch_add(bytes, std::memory_order_relaxed);
  release_frame_.store(frame(), std::memory_order_relaxed);
}

bool Device::TextureBlocked(const plume::RenderTextureDesc& desc) {
  return device_ && !IsLost() && Blocked(BackoffKey(desc));
}

}  // namespace mcla::gpu
