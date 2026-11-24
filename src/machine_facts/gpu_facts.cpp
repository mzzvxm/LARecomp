#include "gpu_facts.h"

#include "gpu/car_rt_scene.h"
#include "gpu/device.h"
#include "gpu/gpu.h"
#include "gpu/gpu_class.h"
#include "gpu/latency.h"
#include "gpu/ray_regen_plan.h"
#include "gpu/rt_tier.h"
#include "gpu/shadow_filter.h"
#include "gpu/temporal_dlss.h"
#include "settings/larecomp_settings.h"

#include <algorithm>
#include <cmath>
#include <format>

#if defined(LARECOMP_GPU_HAS_D3D12)
#include <dxgi1_6.h>
#include <plume_d3d12.h>
#include <wrl/client.h>
#endif
#if defined(LARECOMP_GPU_HAS_VULKAN)
#include <plume_vulkan.h>
#endif

namespace larecomp::machine {
namespace {

using namespace mcla::gpu;
namespace gc = gpu_class;

#if defined(LARECOMP_HAS_FFX)
constexpr bool kFfxBuilt = true;
#else
constexpr bool kFfxBuilt = false;
#endif
#if defined(LARECOMP_HAS_DLSS)
constexpr bool kDlssBuilt = true;
#else
constexpr bool kDlssBuilt = false;
#endif

Adapter DescribeAdapter(const gc::AdapterFacts& facts, uint32_t id, std::string driver) {
  const auto verdict = gc::Classify(facts);
  return {.name = std::string(facts.name), .vendor_id = facts.vendor_id, .device_id = id,
          .driver_version = std::move(driver), .vram_mb = facts.dedicated_bytes >> 20,
          .software = facts.software, .gpu_class = std::string(gc::ClassName(verdict.cls)),
          .shared_memory = facts.uma || facts.integrated, .class_why = verdict.why};
}

Device::Options ProbeOptions() {
  Device::Options options;
  options.preferred_adapter = settings::mcla_gpu_adapter.Get();
  const std::string api = cvar::GetFlagByName("graphics_backend");
  if (api == "auto") {
    std::string automatic_adapter;
    options.backend = ChooseAutoBackend(options.preferred_adapter, automatic_adapter);
    if (options.preferred_adapter.empty()) options.preferred_adapter = std::move(automatic_adapter);
  } else if (!ParseBackend(api, options.backend) || !BackendCompiled(options.backend)) {
    options.backend = kDefaultBackend;
  }
  return options;
}

#if defined(LARECOMP_GPU_HAS_D3D12)
std::string DxgiDriverVersion(IDXGIAdapter1& adapter) {
  LARGE_INTEGER version{};
  if (FAILED(adapter.CheckInterfaceSupport(__uuidof(IDXGIDevice), &version))) return {};
  const auto bits = static_cast<uint64_t>(version.QuadPart);
  return std::format("{}.{}.{}.{}", (bits >> 48) & 0xffff, (bits >> 32) & 0xffff,
                     (bits >> 16) & 0xffff, bits & 0xffff);
}

void ReadDxgiAdapters(GpuFacts& out, uint64_t selected_luid, bool read_uma) {
  Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return;
  Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
  for (UINT i = 0; SUCCEEDED(factory->EnumAdapters1(i, adapter.ReleaseAndGetAddressOf())); ++i) {
    DXGI_ADAPTER_DESC1 desc{};
    if (FAILED(adapter->GetDesc1(&desc)) || (desc.Flags & DXGI_ADAPTER_FLAG_REMOTE)) continue;
    const auto name = PathText(std::filesystem::path(desc.Description));
    gc::AdapterFacts facts;
    facts.name = name;
    facts.vendor_id = desc.VendorId;
    facts.dedicated_bytes = desc.DedicatedVideoMemory;
    facts.software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    if (read_uma && !facts.software && SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                                      IID_PPV_ARGS(&device)))) {
      D3D12_FEATURE_DATA_ARCHITECTURE architecture{};
      if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE, &architecture,
                                                sizeof(architecture)))) {
        facts.uma = architecture.UMA != 0;
      }
    }
    const auto luid = (uint64_t(uint32_t(desc.AdapterLuid.HighPart)) << 32) | desc.AdapterLuid.LowPart;
    if (selected_luid != 0 && luid == selected_luid) out.selected = out.adapters.size();
    out.adapters.push_back(DescribeAdapter(facts, desc.DeviceId, DxgiDriverVersion(*adapter.Get())));
  }
}

void ReadD3dCaps(GpuFacts& out, ID3D12Device& device) {
  constexpr D3D_FEATURE_LEVEL kLevels[] = {D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1,
      D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  D3D12_FEATURE_DATA_FEATURE_LEVELS levels{UINT(std::size(kLevels)), kLevels, {}};
  if (SUCCEEDED(device.CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &levels, sizeof(levels)))) {
    out.api_version = std::format("{}_{}", (levels.MaxSupportedFeatureLevel >> 12) & 15,
                                  (levels.MaxSupportedFeatureLevel >> 8) & 15);
  }
  for (int version = 0x69; version >= 0x60; --version) {
    D3D12_FEATURE_DATA_SHADER_MODEL shader{static_cast<D3D_SHADER_MODEL>(version)};
    if (SUCCEEDED(device.CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shader, sizeof(shader)))) {
      out.shader_model = std::format("{}.{}", shader.HighestShaderModel >> 4,
                                     shader.HighestShaderModel & 15);
      break;
    }
  }
  D3D12_FEATURE_DATA_D3D12_OPTIONS5 options{};
  if (SUCCEEDED(device.CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options, sizeof(options)))) {
    out.dxr_1_1 = options.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1;
    out.ray_query = out.dxr_1_1;
  }
}
#endif

#if defined(LARECOMP_GPU_HAS_VULKAN)
void ReadVulkanAdapters(GpuFacts& out, const plume::VulkanDevice& device) {
  const auto instance = device.renderInterface->instance;
  uint32_t count = 0;
  if (vkEnumeratePhysicalDevices(instance, &count, nullptr) != VK_SUCCESS || count == 0) return;
  std::vector<VkPhysicalDevice> adapters(count);
  const auto result = vkEnumeratePhysicalDevices(instance, &count, adapters.data());
  if (result != VK_SUCCESS && result != VK_INCOMPLETE) return;
  adapters.resize(count);
  for (VkPhysicalDevice adapter : adapters) {
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(adapter, &props);
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(adapter, &memory);
    gc::AdapterFacts facts;
    facts.name = props.deviceName;
    facts.vendor_id = props.vendorID;
    facts.integrated = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
    facts.software = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ||
                     props.deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU;
    if (!facts.integrated) {
      for (uint32_t i = 0; i < memory.memoryHeapCount; ++i) {
        if (memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
          facts.dedicated_bytes += memory.memoryHeaps[i].size;
        }
      }
    }
    if (adapter == device.physicalDevice) out.selected = out.adapters.size();
    out.adapters.push_back(DescribeAdapter(facts, props.deviceID, std::to_string(props.driverVersion)));
  }
}
#endif

void ReadDevice(GpuFacts& out, const Device& device) {
  const bool vulkan = ActiveBackend() == Backend::kVulkan;
  out.api = vulkan ? "vulkan" : "d3d12";
  const auto ids = backend::QueryAdapterIds(device.device());
  const auto facts = gc::FactsFor(device.description(), device.capabilities());
  Adapter selected = DescribeAdapter(facts, ids.device_id, {});
#if defined(LARECOMP_GPU_HAS_VULKAN)
  if (vulkan) {
    const auto& native = *static_cast<plume::VulkanDevice*>(device.device());
    ReadVulkanAdapters(out, native);
    const uint32_t api = native.physicalDeviceProperties.apiVersion;
    out.api_version = std::format("{}.{}.{}", VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api),
                                  VK_API_VERSION_PATCH(api));
    out.dxr_1_1 = false;
    out.ray_query = device.capabilities().rayQuery;
    selected.driver_version = std::to_string(device.description().driverVersion);
  }
#endif
#if defined(LARECOMP_GPU_HAS_D3D12)
  if (!vulkan) {
    const auto native = backend::NativeDeviceOf(device.device());
    ReadDxgiAdapters(out, native.adapter_luid, /*read_uma=*/true);
    ReadD3dCaps(out, *static_cast<ID3D12Device*>(native.device));
  } else {
    // DXGI's dotted driver version is also what the Windows L0 reader sees on a Vulkan start.
    // CHECK: compare the same adapter's name and driver version with L0 on both Windows backends.
    GpuFacts windows;
    ReadDxgiAdapters(windows, 0, /*read_uma=*/false);
    const auto match = std::ranges::find_if(windows.adapters, [&](const Adapter& adapter) {
      return adapter.vendor_id == ids.vendor_id && adapter.device_id == ids.device_id &&
             adapter.name == selected.name;
    });
    if (match != windows.adapters.end() && !match->driver_version.empty()) {
      selected.driver_version = match->driver_version;
    }
  }
#endif
  if (out.selected) {
    if (selected.driver_version.empty()) selected.driver_version = out.adapters[*out.selected].driver_version;
    // Retain the coarse classification in the adapter list. Only the top-level GPU follows forcing.
    out.adapters[*out.selected] = std::move(selected);
  } else {
    out.selected = out.adapters.size();
    out.adapters.push_back(std::move(selected));
  }
  const Adapter& adapter = out.adapters[*out.selected];
  const auto normalized = gc::NormalizeName(adapter.name);
  unsigned rx = 0;
  out.rtx = adapter.vendor_id == gc::kVendorNvidia &&
            normalized.find("rtx") != std::string::npos;
  out.rx9000 = adapter.vendor_id == gc::kVendorAmd &&
               gc::speed_detail::SeriesNumber(normalized, "rx", &rx) && rx >= 9000 && rx < 10000;
  out.denoiser = *out.rtx ? "nvidia" : adapter.vendor_id == gc::kVendorAmd ? "amd" : "none";
  const bool can_trace = CarRtSupported(device.capabilities(), vulkan, device.wave_ops());
  const bool rt = can_trace && gc::CurrentRtRung() != gc::RtTier::kNone;
  out.car_rt = rt;
  out.city_rt_shadows = rt;
  out.fsr1 = true;
  // FidelityFX gives the same device answer before opening a provider.
  if (!kFfxBuilt || !backend::QueryUpscalerSupport(device.device()).ok) out.fsr31 = out.fsr4 = false;
  if (adapter.vendor_id != gc::kVendorAmd) out.fsr4 = false;
}

bool Present(const std::filesystem::path& path) {
  std::error_code error;
  return std::filesystem::is_regular_file(path, error);
}

void ReadFiles(GpuFacts& out, const std::filesystem::path& folder, bool probe) {
  const bool d3d = out.api == "d3d12";
  const auto loader = d3d ? "amd_fidelityfx_loader_dx12.dll" : "amd_fidelityfx_loader_vk.dll";
  const auto upscaler = d3d ? "amd_fidelityfx_upscaler_dx12.dll" : "amd_fidelityfx_upscaler_vk.dll";
  const auto fg = d3d ? "amd_fidelityfx_framegeneration_dx12.dll"
                      : "amd_fidelityfx_framegeneration_vk.dll";
  out.fidelityfx_files = Present(folder / loader) && Present(folder / upscaler);
  out.streamline_files = Present(folder / "sl.interposer.dll") && Present(folder / "sl.common.dll") &&
                         Present(folder / "sl.dlss.dll") && Present(folder / "nvngx_dlss.dll");
  if (!probe || !out.selected || out.api.empty()) return;

  if (!*out.fidelityfx_files) out.fsr31 = out.fsr4 = false;
  // The installed FSR 3.1 provider is listed on non-AMD cards from the same device and file
  // checks as the renderer's lazy path. AMD provider versions need a runtime query.
  if (!out.fsr31 && out.adapters[*out.selected].vendor_id != gc::kVendorAmd) out.fsr31 = true;
#if defined(LARECOMP_HAS_FFX_FG)
  if (!Present(folder / loader) || !Present(folder / fg)) out.fsr31_fg = false;
#else
  (void)fg;
  out.fsr31_fg = false;
#endif
  if (!out.rtx.value_or(false) || !kDlssBuilt) {
    out.dlss = "no";
    out.dlss_fg = false;
  } else if (!*out.streamline_files) {
    out.dlss = "needs_setup";
    out.dlss_fg = false;
  } else if (!dlss::NameLooksLikeFrameGeneration(out.adapters[*out.selected].name) ||
             !dlss::FrameGenerationFilesPresent(nullptr)) {
    out.dlss_fg = false;
  }
  if (out.denoiser != "nvidia") out.dlss_rr = "no";
  if (out.denoiser != "amd") out.ray_regen = "no";
  else if (!rayregen::BuildHas() || !d3d || !out.rx9000.value_or(false)) out.ray_regen = "unsupported";
  else if (!Present(folder / loader) || !Present(folder / "amd_fidelityfx_denoiser_dx12.dll")) {
    out.ray_regen = "needs_setup";
  }
}

void ReadTier(GpuFacts& out) {
  if (!out.selected || !gc::Known()) return;
  const auto cls = gc::Current();
  out.class_forced = gc::ClassForced() ? std::optional<std::string>(gc::ClassName(cls)) : std::nullopt;
  out.handheld = gc::Handheld();
  out.rt_tier = gc::RtTierName(gc::CurrentRtTier());
  out.rt_rung = gc::RtTierName(gc::CurrentRtRung());
  bool benchmarked;
  out.rt_tier_why = gc::MachineRtReason(benchmarked);
  out.rt_benchmarked = benchmarked;
  out.pcss = shadow_filter::PcssRuns();
}

}  // namespace

GpuFacts ReadGpuFacts(bool probe, const std::filesystem::path& folder) {
  GpuFacts out;
  if (probe) {
    // CHECK: both backends should open without a window, shader pack or guest runtime.
    Device device;
    if (device.Initialize(ProbeOptions())) ReadDevice(out, device);
    ReadTier(out);
  } else if (const Device* device = MachineFactsDevice()) {
    ReadDevice(out, *device);
    RefreshGpuFacts(out);
  }
  // CHECK: compatibility-renderer starts have no Plume device. The contract should carry
  // identity when the SDK exposes its active adapter, including any D3D12/Vulkan fallback.
  ReadFiles(out, folder, probe);
  return out;
}

void RefreshGpuFacts(GpuFacts& out) {
  if (!MachineFactsDevice()) return;
  ReadTier(out);
  out.car_rt = CarRtReflAvailable();
  out.city_rt_shadows = CityRtShadowsAvailable();
  out.gi_update = RtGiUpdateBuilt();
  out.paper_white = static_cast<uint32_t>(std::lround(HdrAutoPaperWhiteNits()));
  out.peak_nits = static_cast<uint32_t>(std::lround(HdrAutoPeakNits()));

  const auto upscalers = GetUpscalerAvailability();
  if (upscalers.probed) {
    out.fsr31 = upscalers.fsr31;
    out.fsr4 = upscalers.fsr4;
  }
  if (!kDlssBuilt) {
    out.dlss = "no";
    out.refused_modes.emplace();
  } else if (upscalers.dlss_probed) {
    out.dlss = upscalers.dlss_needs_setup ? "needs_setup" : upscalers.dlss_after_restart
               ? "after_restart" : upscalers.dlss ? "ready" : "no";
    // A zero mask is also the value before any mode has been tried.
    if (upscalers.dlss_modes_refused != 0 || !upscalers.dlss) {
      out.refused_modes.emplace();
      for (uint32_t i = 0; i < uint32_t(UpscaleMode::kCount); ++i) {
        const auto mode = static_cast<UpscaleMode>(i);
        if (upscalers.DlssModeRefused(mode)) {
          out.refused_modes->emplace_back(ModeInfoOf(Upscaler::kDlss, mode).cvar);
        }
      }
    } else {
      out.refused_modes.reset();
    }
  }

  const auto fg = GetFrameGenerationAvailability();
  if (fg.probed) out.fsr31_fg = fg.available && !fg.failed;
  const auto dlss_fg = GetDlssFrameGenerationAvailability();
  if (dlss_fg.probed) {
    out.dlss_fg = (dlss_fg.supported || dlss_fg.after_restart) && !dlss_fg.failed;
    out.dlss_after_restart = dlss_fg.after_restart;
    if (dlss_fg.listed) out.dlss_multiplier = dlss_fg.max_multiplier;
    else out.dlss_multiplier.reset();
  }
  switch (latency::AvailableKind()) {
    case latency::Kind::kNone: out.latency = "none"; break;
    case latency::Kind::kReflex: out.latency = "reflex"; break;
    case latency::Kind::kAntiLag: out.latency = "antilag2"; break;
  }
  out.latency_auto = latency::AutoIsOn();

  const auto rr = dlss::GetRayReconstructionFacts();
  const auto amd = rayregen::MenuFacts();
  if (out.denoiser != "nvidia") out.dlss_rr = "no";
  else if (!rr.built || rr.failed) out.dlss_rr = "unsupported";
  else if (!rr.files_ok) out.dlss_rr = "needs_setup";
  else if (rr.loadable && !rr.asked) out.dlss_rr = "after_restart";
  else if (rr.queried) out.dlss_rr = rr.supported ? "ready" : "unsupported";
  else out.dlss_rr.reset();
  if (out.denoiser != "amd") out.ray_regen = "no";
  else if (!amd.build_has || !out.rx9000.value_or(false) ||
           amd.device != rayregen::DeviceVerdict::kOk || amd.failed) out.ray_regen = "unsupported";
  else if (!amd.files_ok) out.ray_regen = "needs_setup";
  else if (amd.probed) out.ray_regen = amd.supported ? "ready" : "unsupported";
  else out.ray_regen.reset();

  // A lazy provider can remain unanswered until its row is used. It can update a final file later.
  // CHECK: verify the provisional-to-final transition with Streamline, FidelityFX and a build
  // that omits both, then force the GPU class and check the published class and RT rung.
  out.pending = (kFfxBuilt && !upscalers.probed) || (kDlssBuilt && !upscalers.dlss_probed) ||
                !fg.probed || !dlss_fg.probed ||
                (out.denoiser == "nvidia" && rr.asked && !rr.queried);
}

}  // namespace larecomp::machine
