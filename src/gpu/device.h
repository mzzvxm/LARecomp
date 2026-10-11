#pragma once
// ===========================================================================
// MCLA GPU renderer -- the device
// ===========================================================================
// Owns the Plume interface, the device and the one DIRECT queue every frame is
// submitted on. Nothing here is shared with the ReXGlue SDK: in mcla_gpu mode
// the SDK has no graphics system (gpu_plugin empty) and no D3D12Provider is
// created, so this is the only device in the process. Same ownership as
// reblue (gpu/device.cpp CreateHostDevice) and reeot (gpu/device.cpp).
//
// Creation goes through the Create* members below, never straight through
// device()->create*: Plume returns a live wrapper around a null native object
// when the backend call fails, and binding one faults later somewhere else.
// Each failure is logged with what was being created and checked against
// device loss, which is latched -- once lost, every later creation and every
// fence wait would fail or hang, so callers stop at IsLost().
//
// Threading: Initialize/Shutdown on the thread that owns the renderer's
// lifecycle; queue() submission only from the render thread. Create* is called
// from the render thread today. reblue and reeot also create pipelines from
// precache worker threads; whether every Plume create path is safe for that is
// to be checked per backend when the precache workers land, not assumed here.
// ===========================================================================

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include <plume_render_interface.h>

#include "backend.h"

namespace mcla::gpu {

class Device {
 public:
  struct Options {
    // Passed to RenderInterface::createDevice; empty picks Plume's default.
    std::string preferred_adapter;
    bool debug_layer = false;
  };

  Device() = default;
  ~Device();
  Device(const Device&) = delete;
  Device& operator=(const Device&) = delete;

  bool Initialize(const Options& options);
  // Releases the queue, the device and the interface, in that order. Everything
  // created from this device must already be gone.
  void Shutdown();

  plume::RenderDevice* device() const { return device_.get(); }
  plume::RenderCommandQueue* queue() const { return queue_.get(); }
  const plume::RenderDeviceCapabilities& capabilities() const { return device_->getCapabilities(); }
  const plume::RenderDeviceDescription& description() const { return device_->getDescription(); }
  const std::string& backend_info() const { return backend_info_; }

  // Validated creation. Null on failure, never a null-backed wrapper. `name`
  // becomes the debug name (RenderDoc, PIX, validation messages) where Plume
  // has one for the object, and names the object in the failure log always.
  std::unique_ptr<plume::RenderTexture> CreateTexture(const plume::RenderTextureDesc& desc,
                                                      const char* name);
  std::unique_ptr<plume::RenderTextureView> CreateTextureView(
      const plume::RenderTexture& texture, const plume::RenderTextureViewDesc& desc,
      const char* name);
  std::unique_ptr<plume::RenderBuffer> CreateBuffer(const plume::RenderBufferDesc& desc,
                                                    const char* name);
  std::unique_ptr<plume::RenderSampler> CreateSampler(const plume::RenderSamplerDesc& desc,
                                                      const char* name);
  // `view_count`/`sampler_count`: descriptors the builder describes, checked
  // against what the backend allocated (backend::DescriptorSetAllocated).
  std::unique_ptr<plume::RenderDescriptorSet> CreateDescriptorSet(
      const plume::RenderDescriptorSetBuilder& builder, uint32_t view_count,
      uint32_t sampler_count, const char* name);
  std::unique_ptr<plume::RenderPipelineLayout> CreatePipelineLayout(
      const plume::RenderPipelineLayoutBuilder& builder, const char* name);
  // A shader blob in kHostShaderFormat: the host shaders built with the
  // renderer, and the guest shaders of the pack (shaders.h).
  std::unique_ptr<plume::RenderShader> CreateShader(const void* data, uint64_t size,
                                                    const char* entry_point, const char* name);
  std::unique_ptr<plume::RenderPipeline> CreateGraphicsPipeline(
      const plume::RenderGraphicsPipelineDesc& desc, const char* name);
  std::unique_ptr<plume::RenderQueryPool> CreateQueryPool(uint32_t query_count, const char* name);

  // True once device loss has been seen. Latched.
  bool IsLost() const { return lost_.load(std::memory_order_acquire); }
  // Asks the backend whether the device is gone; logs and latches the first
  // time it is. `context` names the operation that noticed. Returns IsLost().
  bool CheckLost(const char* context);

  backend::VideoMemory QueryVideoMemory() const;

 private:
  void ReportCreationFailure(const char* what, const char* name);

  // Declaration order is destruction order in reverse: queue, device, interface.
  std::unique_ptr<plume::RenderInterface> interface_;
  std::unique_ptr<plume::RenderDevice> device_;
  std::unique_ptr<plume::RenderCommandQueue> queue_;
  std::string backend_info_;
  bool debug_layer_ = false;
  std::atomic<bool> lost_{false};
};

}  // namespace mcla::gpu
