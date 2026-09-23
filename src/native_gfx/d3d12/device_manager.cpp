#include "device_manager.h"

#include <algorithm>
#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(mcla_native_gfx_own_device, true, "MCLA/NativeGfx",
                    "Use native DeviceManager to own the D3D12 device and direct command queue "
                    "instead of borrowing from RexGlue/Xenia provider. Enables high-performance "
                    "adapter selection, Agility SDK features, and drops the shared submit mutex.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace mcla::native_gfx {

DeviceManager& DeviceManager::Instance() {
  static DeviceManager s_instance;
  return s_instance;
}

DeviceManager::~DeviceManager() {
  Shutdown();
}

void DeviceManager::Shutdown() {
  direct_queue_.Reset();
  device_.Reset();
  adapter_.Reset();
  dxgi_factory_.Reset();
  caps_ = {};
}

bool DeviceManager::SelectAdapter(IDXGIFactory2* factory, IDXGIAdapter1** out_adapter) {
  if (!factory || !out_adapter) {
    return false;
  }
  *out_adapter = nullptr;

  // Prefer IDXGIFactory6::EnumAdapterByGpuPreference to select the high-performance GPU
  Microsoft::WRL::ComPtr<IDXGIFactory6> factory6;
  if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&factory6)))) {
    Microsoft::WRL::ComPtr<IDXGIAdapter1> best_adapter;
    for (UINT adapter_idx = 0;
         factory6->EnumAdapterByGpuPreference(adapter_idx, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                              IID_PPV_ARGS(&best_adapter)) != DXGI_ERROR_NOT_FOUND;
         ++adapter_idx) {
      DXGI_ADAPTER_DESC1 desc = {};
      best_adapter->GetDesc1(&desc);
      if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
        continue;
      }
      if (SUCCEEDED(D3D12CreateDevice(best_adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                      _uuidof(ID3D12Device), nullptr))) {
        *out_adapter = best_adapter.Detach();
        return true;
      }
    }
  }

  // Fallback: enumerate adapters and select the discrete GPU with highest VRAM
  Microsoft::WRL::ComPtr<IDXGIAdapter1> current_adapter;
  Microsoft::WRL::ComPtr<IDXGIAdapter1> selected_adapter;
  SIZE_T max_vram = 0;

  for (UINT i = 0; factory->EnumAdapters1(i, &current_adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
    DXGI_ADAPTER_DESC1 desc = {};
    current_adapter->GetDesc1(&desc);
    if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
      continue;
    }
    if (SUCCEEDED(D3D12CreateDevice(current_adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                    _uuidof(ID3D12Device), nullptr))) {
      if (desc.DedicatedVideoMemory > max_vram || !selected_adapter) {
        max_vram = desc.DedicatedVideoMemory;
        selected_adapter = current_adapter;
      }
    }
  }

  if (selected_adapter) {
    *out_adapter = selected_adapter.Detach();
    return true;
  }
  return false;
}

void DeviceManager::QueryCapabilities() {
  if (!device_) {
    return;
  }

  // Feature Level check (from 12_2 down to 11_0)
  static constexpr D3D_FEATURE_LEVEL kLevels[] = {
      D3D_FEATURE_LEVEL_12_2,
      D3D_FEATURE_LEVEL_12_1,
      D3D_FEATURE_LEVEL_12_0,
      D3D_FEATURE_LEVEL_11_1,
      D3D_FEATURE_LEVEL_11_0,
  };
  D3D12_FEATURE_DATA_FEATURE_LEVELS fl_data = {};
  fl_data.NumFeatureLevels = static_cast<UINT>(sizeof(kLevels) / sizeof(kLevels[0]));
  fl_data.pFeatureLevelsRequested = kLevels;
  if (SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &fl_data, sizeof(fl_data)))) {
    caps_.max_feature_level = fl_data.MaxSupportedFeatureLevel;
  }

  // Root Signature Version
  D3D12_FEATURE_DATA_ROOT_SIGNATURE rs_data = {D3D_ROOT_SIGNATURE_VERSION_1_1};
  if (SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &rs_data, sizeof(rs_data)))) {
    caps_.highest_root_signature_version = rs_data.HighestVersion;
  } else {
    caps_.highest_root_signature_version = D3D_ROOT_SIGNATURE_VERSION_1_0;
  }

  // Shader Model
  static constexpr D3D_SHADER_MODEL kModels[] = {
      D3D_SHADER_MODEL_6_8,
      D3D_SHADER_MODEL_6_7,
      D3D_SHADER_MODEL_6_6,
      D3D_SHADER_MODEL_6_5,
      D3D_SHADER_MODEL_6_4,
      D3D_SHADER_MODEL_6_3,
      D3D_SHADER_MODEL_6_2,
      D3D_SHADER_MODEL_6_1,
      D3D_SHADER_MODEL_6_0,
      D3D_SHADER_MODEL_5_1,
  };
  for (auto model : kModels) {
    D3D12_FEATURE_DATA_SHADER_MODEL sm_data = {model};
    if (SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm_data, sizeof(sm_data)))) {
      caps_.highest_shader_model = sm_data.HighestShaderModel;
      break;
    }
  }

  // D3D12 Options
  D3D12_FEATURE_DATA_D3D12_OPTIONS opt = {};
  if (SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &opt, sizeof(opt)))) {
    caps_.resource_binding_tier = opt.ResourceBindingTier;
  }

  // Enhanced Barriers (D3D12_OPTIONS12)
#ifdef D3D12_FEATURE_D3D12_OPTIONS12
  D3D12_FEATURE_DATA_D3D12_OPTIONS12 opt12 = {};
  if (SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &opt12, sizeof(opt12)))) {
    caps_.enhanced_barriers_supported = (opt12.EnhancedBarriersSupported == TRUE);
  }
#endif

  // GPU Upload Heaps / ReBAR (D3D12_OPTIONS16)
#ifdef D3D12_FEATURE_D3D12_OPTIONS16
  D3D12_FEATURE_DATA_D3D12_OPTIONS16 opt16 = {};
  if (SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS16, &opt16, sizeof(opt16)))) {
    caps_.gpu_upload_heaps_supported = (opt16.GPUUploadHeapSupported == TRUE);
  }
#endif

  // Tearing support on DXGI Factory
  Microsoft::WRL::ComPtr<IDXGIFactory5> factory5;
  if (SUCCEEDED(dxgi_factory_->QueryInterface(IID_PPV_ARGS(&factory5)))) {
    BOOL tearing = FALSE;
    if (SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing)))) {
      caps_.tearing_supported = (tearing == TRUE);
    }
  }
}

bool DeviceManager::Initialize(bool enable_debug_layer, bool enable_gpu_validation) {
  if (IsInitialized()) {
    return true;
  }

  UINT dxgi_flags = 0;
  if (enable_debug_layer) {
    Microsoft::WRL::ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
      debug->EnableDebugLayer();
      dxgi_flags |= DXGI_CREATE_FACTORY_DEBUG;
      if (enable_gpu_validation) {
        Microsoft::WRL::ComPtr<ID3D12Debug1> debug1;
        if (SUCCEEDED(debug.As(&debug1))) {
          debug1->SetEnableGPUBasedValidation(TRUE);
          REXLOG_INFO("[DeviceManager] D3D12 GPU-based validation enabled");
        }
      }
      REXLOG_INFO("[DeviceManager] D3D12 debug layer enabled");
    }
  }

  HRESULT hr = CreateDXGIFactory2(dxgi_flags, IID_PPV_ARGS(&dxgi_factory_));
  if (FAILED(hr)) {
    REXLOG_ERROR("[DeviceManager] CreateDXGIFactory2 failed: 0x{:08X}", static_cast<uint32_t>(hr));
    return false;
  }

  if (!SelectAdapter(dxgi_factory_.Get(), &adapter_)) {
    REXLOG_ERROR("[DeviceManager] Failed to find a compatible Direct3D 12 adapter");
    Shutdown();
    return false;
  }

  DXGI_ADAPTER_DESC1 desc = {};
  adapter_->GetDesc1(&desc);
  std::wstring ws(desc.Description);
  caps_.adapter_name = std::string(ws.begin(), ws.end());
  caps_.dedicated_video_memory = desc.DedicatedVideoMemory;

  REXLOG_INFO("[DeviceManager] Selected GPU: {} (VRAM: {:.1f} GB)",
              caps_.adapter_name, double(caps_.dedicated_video_memory) / (1024.0 * 1024.0 * 1024.0));

  hr = D3D12CreateDevice(adapter_.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_));
  if (FAILED(hr)) {
    REXLOG_ERROR("[DeviceManager] D3D12CreateDevice failed: 0x{:08X}", static_cast<uint32_t>(hr));
    Shutdown();
    return false;
  }

  QueryCapabilities();
  REXLOG_INFO("[DeviceManager] Feature level: 0x{:04X}, RootSig: {}, SM: 0x{:04X}, EnhancedBarriers: {}, GPUUploadHeaps: {}, Tearing: {}",
              static_cast<uint32_t>(caps_.max_feature_level),
              caps_.highest_root_signature_version == D3D_ROOT_SIGNATURE_VERSION_1_1 ? "1.1" : "1.0",
              static_cast<uint32_t>(caps_.highest_shader_model),
              caps_.enhanced_barriers_supported,
              caps_.gpu_upload_heaps_supported,
              caps_.tearing_supported);

  // Dedicated Direct Command Queue
  D3D12_COMMAND_QUEUE_DESC queue_desc = {};
  queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
  queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
  queue_desc.NodeMask = 0;

  hr = device_->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&direct_queue_));
  if (FAILED(hr)) {
    // If priority high failed on some drivers, fallback to normal priority
    queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    hr = device_->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&direct_queue_));
    if (FAILED(hr)) {
      REXLOG_ERROR("[DeviceManager] CreateCommandQueue failed: 0x{:08X}", static_cast<uint32_t>(hr));
      Shutdown();
      return false;
    }
  }

  direct_queue_->SetName(L"MCLA_Native_DirectQueue");
  return true;
}

}  // namespace mcla::native_gfx
