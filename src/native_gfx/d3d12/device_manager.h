#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <rex/ui/d3d12/d3d12_api.h>
#include <dxgi1_6.h>

namespace mcla::native_gfx {

struct DeviceCapabilities {
  std::string adapter_name;
  uint64_t dedicated_video_memory = 0;
  D3D_FEATURE_LEVEL max_feature_level = D3D_FEATURE_LEVEL_11_0;
  D3D_ROOT_SIGNATURE_VERSION highest_root_signature_version = D3D_ROOT_SIGNATURE_VERSION_1_0;
  D3D_SHADER_MODEL highest_shader_model = D3D_SHADER_MODEL_5_1;
  D3D12_RESOURCE_BINDING_TIER resource_binding_tier = D3D12_RESOURCE_BINDING_TIER_1;
  bool enhanced_barriers_supported = false;
  bool gpu_upload_heaps_supported = false;
  bool tearing_supported = false;
  bool mesh_shaders_supported = false;
  bool sampler_feedback_supported = false;
};

class DeviceManager {
 public:
  static DeviceManager& Instance();

  bool Initialize(bool enable_debug_layer = false, bool enable_gpu_validation = false);
  void Shutdown();

  bool IsInitialized() const { return device_ != nullptr; }

  ID3D12Device* device() const { return device_.Get(); }
  ID3D12CommandQueue* direct_queue() const { return direct_queue_.Get(); }
  IDXGIFactory2* dxgi_factory() const { return dxgi_factory_.Get(); }
  IDXGIAdapter1* adapter() const { return adapter_.Get(); }

  const DeviceCapabilities& capabilities() const { return caps_; }

 private:
  DeviceManager() = default;
  ~DeviceManager();

  DeviceManager(const DeviceManager&) = delete;
  DeviceManager& operator=(const DeviceManager&) = delete;

  bool SelectAdapter(IDXGIFactory2* factory, IDXGIAdapter1** out_adapter);
  void QueryCapabilities();

  Microsoft::WRL::ComPtr<IDXGIFactory2> dxgi_factory_;
  Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter_;
  Microsoft::WRL::ComPtr<ID3D12Device> device_;
  Microsoft::WRL::ComPtr<ID3D12CommandQueue> direct_queue_;
  DeviceCapabilities caps_;
};

}  // namespace mcla::native_gfx
