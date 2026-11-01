// MCLA GPU renderer -- the D3D12 half of backend.h: one of the two translation
// units that reach past the Plume interface into a backend's own objects (the
// other is backend_vulkan.cpp). See backend.h for why each call exists.
//
// Design followed (not copied): reblue gpu/device.cpp CreateHostBuffer/
// CreateHostTexture/DescribeBackend/MemoryUsage and reeot gpu/device.cpp
// ReportCreationFailure, both BSD-3-Clause.

#include "backend_impl.h"

#include <algorithm>
#include <format>
#include <string>
#include <vector>

#include <dxgi1_6.h>
#include <wrl/client.h>

#include <rex/cvar.h>
#include <rex/logging.h>

#include <plume_d3d12.h>

namespace plume {
extern std::unique_ptr<RenderInterface> CreateD3D12Interface();
}  // namespace plume

REXCVAR_DEFINE_BOOL(
    mcla_gpu_gpu_based_validation, false, "MCLA/GPU",
    "With mcla_gpu_debug_layer on D3D12, enable GPU-based validation before device creation. "
    "Diagnostic; requires the matching D3D12SDKLayers.dll and a restart.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace mcla::gpu::backend::d3d12 {

bool EnableDebugLayer() {
  ID3D12Debug* debug = nullptr;
  if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))) || !debug) {
    REXLOG_ERROR("[gpu] D3D12 debug layer requested but D3D12GetDebugInterface failed "
                 "(Graphics Tools optional feature not installed?)");
    return false;
  }
  debug->EnableDebugLayer();
  if (REXCVAR_GET(mcla_gpu_gpu_based_validation)) {
    ID3D12Debug1* debug1 = nullptr;
    const HRESULT hr = debug->QueryInterface(IID_PPV_ARGS(&debug1));
    if (FAILED(hr) || !debug1) {
      REXLOG_ERROR("[gpu] GPU-based validation requested but ID3D12Debug1 is unavailable "
                   "({:#010x})",
                   static_cast<uint32_t>(hr));
      debug->Release();
      return false;
    }
    debug1->SetEnableGPUBasedValidation(TRUE);
    debug1->Release();
    REXLOG_INFO("[gpu] D3D12 GPU-based validation enabled before device creation");
  }
  debug->Release();
  return true;
}

namespace {

void __stdcall OnD3D12DebugMessage(D3D12_MESSAGE_CATEGORY category,
                                   D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID id,
                                   LPCSTR description, void*) {
  switch (severity) {
    case D3D12_MESSAGE_SEVERITY_CORRUPTION:
    case D3D12_MESSAGE_SEVERITY_ERROR:
      REXLOG_ERROR("[gpu] D3D12 debug layer (category {}, id {}): {}",
                   static_cast<int>(category), static_cast<int>(id),
                   description ? description : "");
      break;
    case D3D12_MESSAGE_SEVERITY_WARNING:
      REXLOG_WARN("[gpu] D3D12 debug layer (category {}, id {}): {}", static_cast<int>(category),
                  static_cast<int>(id), description ? description : "");
      break;
    default:
      break;
  }
}

}  // namespace

void InstallDebugMessageSink(plume::RenderDevice* device) {
  auto* dev = static_cast<plume::D3D12Device*>(device);
  if (!dev || !dev->d3d) {
    return;
  }
  ID3D12InfoQueue1* queue = nullptr;
  if (FAILED(dev->d3d->QueryInterface(IID_PPV_ARGS(&queue))) || !queue) {
    REXLOG_ERROR("[gpu] the D3D12 debug layer is not active on this device (no ID3D12InfoQueue1): "
                 "its messages cannot be logged");
    return;
  }
  // Notes this renderer produces by design, kept out so that what is left is
  // worth reading (the callback honours the storage filter):
  // - CLEAR{RENDERTARGET,DEPTHSTENCIL}VIEW_MISMATCHINGCLEARVALUE: the EDRAM
  //   targets have no optimized clear value -- the guest clears them to
  //   whatever each frame needs -- so every clear carries this performance note;
  // - CREATEGRAPHICSPIPELINESTATE_RENDERTARGETVIEW_NOT_SET: a pixel shader that
  //   writes a colour slot its pass has no target in; the layer itself says
  //   writes to an unbound target are discarded, and the shader is still needed
  //   for what it discards and the depth it leaves.
  D3D12_MESSAGE_ID denied[] = {
      D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE,
      D3D12_MESSAGE_ID_CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE,
      D3D12_MESSAGE_ID_CREATEGRAPHICSPIPELINESTATE_RENDERTARGETVIEW_NOT_SET,
  };
  D3D12_INFO_QUEUE_FILTER filter = {};
  filter.DenyList.NumIDs = UINT(std::size(denied));
  filter.DenyList.pIDList = denied;
  if (FAILED(queue->PushStorageFilter(&filter))) {
    REXLOG_WARN("[gpu] could not filter the D3D12 debug layer's by-design notes");
  }
  DWORD cookie = 0;
  const HRESULT hr = queue->RegisterMessageCallback(&OnD3D12DebugMessage,
                                                    D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr,
                                                    &cookie);
  // The callback belongs to the device's queue and lives as long as the device.
  queue->Release();
  if (FAILED(hr)) {
    REXLOG_ERROR("[gpu] could not register the D3D12 debug message callback ({:#010x})",
                 static_cast<uint32_t>(hr));
    return;
  }
  REXLOG_INFO("[gpu] D3D12 debug layer messages (warnings and errors) go to this log");
}

void* CaptureDebugDevice(plume::RenderDevice* device) {
  auto* dev = static_cast<plume::D3D12Device*>(device);
  if (!dev || !dev->d3d) {
    return nullptr;
  }
  ID3D12DebugDevice* debug_device = nullptr;
  if (FAILED(dev->d3d->QueryInterface(IID_PPV_ARGS(&debug_device)))) {
    return nullptr;
  }
  return debug_device;
}

void ReportLiveObjects(void* debug_device) {
  auto* d = static_cast<ID3D12DebugDevice*>(debug_device);
  REXLOG_INFO("[gpu] D3D12 objects still alive after the renderer released everything (the "
              "device itself is held by this report):");
  d->ReportLiveDeviceObjects(D3D12_RLDO_DETAIL | D3D12_RLDO_IGNORE_INTERNAL);
  d->Release();
}

void ReleaseDebugDevice(void* debug_device) {
  static_cast<ID3D12DebugDevice*>(debug_device)->Release();
}

std::unique_ptr<plume::RenderInterface> CreateInterface() {
  return plume::CreateD3D12Interface();
}

bool IsNullBacked(const plume::RenderTexture* texture) {
  return !texture || static_cast<const plume::D3D12Texture*>(texture)->d3d == nullptr;
}

bool IsNullBacked(const plume::RenderTextureView* view) {
  // A CPU-side view description; it only names its texture.
  return !view || static_cast<const plume::D3D12TextureView*>(view)->texture == nullptr;
}

bool IsNullBacked(const plume::RenderBuffer* buffer) {
  return !buffer || static_cast<const plume::D3D12Buffer*>(buffer)->d3d == nullptr;
}

bool IsNullBacked(const plume::RenderPipeline* graphics_pipeline) {
  return !graphics_pipeline ||
         static_cast<const plume::D3D12GraphicsPipeline*>(graphics_pipeline)->d3d == nullptr;
}

bool IsNullBackedCompute(const plume::RenderPipeline* compute_pipeline) {
  return !compute_pipeline ||
         static_cast<const plume::D3D12ComputePipeline*>(compute_pipeline)->d3d == nullptr;
}

bool IsNullBacked(const plume::RenderPipelineLayout* layout) {
  return !layout ||
         static_cast<const plume::D3D12PipelineLayout*>(layout)->rootSignature == nullptr;
}

bool IsNullBacked(const plume::RenderShader* shader) {
  // D3D12 keeps the bytecode; nothing is compiled until a pipeline uses it.
  return !shader || static_cast<const plume::D3D12Shader*>(shader)->d3d.empty();
}

bool IsNullBacked(const plume::RenderSampler* sampler) {
  return !sampler;
}

bool IsNullBacked(const plume::RenderQueryPool* pool) {
  if (!pool) {
    return true;
  }
  const auto* p = static_cast<const plume::D3D12QueryPool*>(pool);
  return p->d3d == nullptr || IsNullBacked(p->readbackBuffer.get());
}

bool IsNullBacked(const plume::RenderCommandQueue* queue) {
  return !queue || static_cast<const plume::D3D12CommandQueue*>(queue)->d3d == nullptr;
}

bool IsNullBacked(const plume::RenderFramebuffer* framebuffer) {
  // A D3D12 framebuffer is its RTV/DSV descriptors: failed if any of them
  // could not be allocated.
  if (!framebuffer) {
    return true;
  }
  const auto* f = static_cast<const plume::D3D12Framebuffer*>(framebuffer);
  for (uint32_t offset : f->colorTargetAllocatorOffsets) {
    if (offset == plume::D3D12DescriptorHeapAllocator::INVALID_OFFSET) {
      return true;
    }
  }
  return f->depthTarget != nullptr &&
         f->depthTargetAllocatorOffset == plume::D3D12DescriptorHeapAllocator::INVALID_OFFSET;
}

bool DescriptorSetAllocated(const plume::RenderDescriptorSet* set, uint32_t view_count,
                            uint32_t sampler_count) {
  if (!set) {
    return false;
  }
  const auto* s = static_cast<const plume::D3D12DescriptorSet*>(set);
  return s->viewAllocation.count == view_count && s->samplerAllocation.count == sampler_count;
}

bool TimestampsSupported(plume::RenderDevice* device) {
  const auto* dev = static_cast<const plume::D3D12Device*>(device);
  return dev && dev->timestampFrequency > 1;
}

std::string DescribeDevice(plume::RenderDevice* device) {
  auto* dev = static_cast<plume::D3D12Device*>(device);
  if (!dev || !dev->d3d) {
    return "D3D12";
  }
  static const D3D_FEATURE_LEVEL kLevels[] = {D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1,
                                              D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1,
                                              D3D_FEATURE_LEVEL_11_0};
  D3D12_FEATURE_DATA_FEATURE_LEVELS levels = {};
  levels.NumFeatureLevels = static_cast<UINT>(std::size(kLevels));
  levels.pFeatureLevelsRequested = kLevels;
  if (FAILED(dev->d3d->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &levels,
                                           sizeof(levels)))) {
    return "D3D12";
  }
  const uint32_t fl = static_cast<uint32_t>(levels.MaxSupportedFeatureLevel);
  return std::format("D3D12 {}_{}", (fl >> 12) & 0xF, (fl >> 8) & 0xF);
}

VideoMemory QueryVideoMemory(plume::RenderDevice* device) {
  VideoMemory out;
  auto* dev = static_cast<plume::D3D12Device*>(device);
  if (!dev || !dev->adapter) {
    return out;
  }
  IDXGIAdapter3* adapter = nullptr;
  if (FAILED(dev->adapter->QueryInterface(IID_PPV_ARGS(&adapter))) || !adapter) {
    return out;
  }
  DXGI_QUERY_VIDEO_MEMORY_INFO info = {};
  if (SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
    out.used = info.CurrentUsage;
    out.budget = info.Budget;
  }
  adapter->Release();
  return out;
}

int64_t DeviceRemovedReason(plume::RenderDevice* device) {
  auto* dev = static_cast<plume::D3D12Device*>(device);
  if (!dev || !dev->d3d) {
    return 0;
  }
  const HRESULT reason = dev->d3d->GetDeviceRemovedReason();
  return FAILED(reason) ? static_cast<int64_t>(static_cast<uint32_t>(reason)) : 0;
}

namespace {

// The entry of Windows' graphics settings for this exe: the value named by the exe's full path in
// HKCU, Software, Microsoft, DirectX, UserGpuPreferences, with data like "GpuPreference=2;".
// 0 = let Windows decide, 1 = power saving, 2 = high performance, -1 = no entry.
int WindowsGpuPreference() {
  wchar_t exe[MAX_PATH];
  const DWORD length = GetModuleFileNameW(nullptr, exe, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) {
    return -1;
  }
  wchar_t data[128];
  DWORD size = sizeof(data);
  if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\DirectX\\UserGpuPreferences", exe,
                   RRF_RT_REG_SZ, nullptr, data, &size) != ERROR_SUCCESS) {
    return -1;
  }
  constexpr wchar_t kItem[] = L"GpuPreference=";
  const wchar_t* item = wcsstr(data, kItem);
  const wchar_t digit = item ? item[std::size(kItem) - 1] : L'\0';
  return digit >= L'0' && digit <= L'2' ? digit - L'0' : -1;
}

}  // namespace

std::string DefaultAdapterName() {
  Microsoft::WRL::ComPtr<IDXGIFactory1> factory1;
  Microsoft::WRL::ComPtr<IDXGIFactory6> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory1))) || FAILED(factory1.As(&factory))) {
    REXLOG_INFO("[gpu] D3D12 adapter: no IDXGIFactory6 (Windows before 10 1803); Plume picks the "
                "adapter with the most video memory");
    return {};
  }
  const int preference = WindowsGpuPreference();
  const DXGI_GPU_PREFERENCE order = preference == 1 ? DXGI_GPU_PREFERENCE_MINIMUM_POWER
                                                    : DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE;
  // Windows' order, without the adapters Plume skips too (software, remote, no D3D12 at 11_0).
  std::vector<std::string> names;
  Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
  for (UINT i = 0; SUCCEEDED(factory->EnumAdapterByGpuPreference(i, order, IID_PPV_ARGS(&adapter)));
       ++i, adapter.Reset()) {
    DXGI_ADAPTER_DESC1 desc{};
    if (FAILED(adapter->GetDesc1(&desc)) ||
        (desc.Flags & (DXGI_ADAPTER_FLAG_SOFTWARE | DXGI_ADAPTER_FLAG_REMOTE)) ||
        FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device),
                                 nullptr))) {
      continue;
    }
    char name[512];  // the same UTF-8 name Plume compares with
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name),
                                          nullptr, nullptr);
    names.emplace_back(name, bytes > 0 ? bytes - 1 : 0);
  }
  if (names.empty()) {
    REXLOG_INFO("[gpu] D3D12 adapter: none listed by IDXGIFactory6; Plume picks");
    return {};
  }
  if (std::count(names.begin(), names.end(), names.front()) > 1) {
    REXLOG_INFO("[gpu] D3D12 adapter: '{}' comes first but {} adapters have that name, which "
                "Plume cannot tell apart; Plume picks", names.front(),
                std::count(names.begin(), names.end(), names.front()));
    return {};
  }
  const char* why = preference == 1   ? "Windows' graphics setting for this app: power saving"
                    : preference == 2 ? "Windows' graphics setting for this app: high performance"
                    : preference == 0 ? "Windows' graphics setting for this app: let Windows "
                                        "decide; the high-performance GPU"
                                      : "no graphics setting for this app; the high-performance GPU";
  REXLOG_INFO("[gpu] D3D12 adapter '{}': {} (mcla_gpu_adapter empty)", names.front(), why);
  return names.front();
}

}  // namespace mcla::gpu::backend::d3d12
