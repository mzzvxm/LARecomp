// MCLA GPU renderer -- the device. See device.h.

#include "device.h"

#include <rex/logging.h>

namespace mcla::gpu {

Device::~Device() {
  Shutdown();
}

bool Device::Initialize(const Options& options) {
  if (device_) {
    return true;
  }
  // After nothing, before the interface: the debug layer has to be enabled
  // before the first D3D12 device of the process exists.
  const bool debug_layer = options.debug_layer && backend::EnableDebugLayer();
  if (debug_layer) {
    REXLOG_INFO("[gpu] {} debug layer enabled (mcla_gpu_debug_layer)", BackendName());
  }

  interface_ = backend::CreateInterface();
  if (!interface_) {
    REXLOG_ERROR("[gpu] Plume could not create the {} interface", BackendName());
    return false;
  }

  device_ = interface_->createDevice(options.preferred_adapter);
  if (!device_) {
    REXLOG_ERROR("[gpu] Plume could not create a {} device{}", BackendName(),
                 options.preferred_adapter.empty()
                     ? std::string()
                     : " on adapter '" + options.preferred_adapter + "'");
    interface_.reset();
    return false;
  }
  backend_info_ = backend::DescribeDevice(device_.get());
  debug_layer_ = debug_layer;
  if (debug_layer_) {
    backend::InstallDebugMessageSink(device_.get());
  }

  // The XenosRecomp shaders read their constant buffers through buffer device
  // addresses on Vulkan (shader_common.h: vk::RawBufferLoad off push-constant
  // addresses), and every shader of this renderer samples through the bindless
  // sets (descriptors.h), which need descriptor indexing. A device without
  // either cannot run a single draw, so it is refused here rather than at the
  // first one. D3D12 has both by definition at the feature level Plume asks for.
  if (kBackend == Backend::kVulkan) {
    const auto& caps = device_->getCapabilities();
    const char* missing = !caps.bufferDeviceAddress ? "buffer device address"
                          : !caps.descriptorIndexing ? "descriptor indexing"
                                                     : nullptr;
    if (missing) {
      REXLOG_ERROR("[gpu] {} on {} has no {}; this renderer needs it", backend_info_,
                   device_->getDescription().name, missing);
      device_.reset();
      interface_.reset();
      return false;
    }
  }

  queue_ = device_->createCommandQueue(plume::RenderCommandListType::DIRECT);
  if (backend::IsNullBacked(queue_.get())) {
    REXLOG_ERROR("[gpu] Plume could not create the direct queue on {}", backend_info_);
    queue_.reset();
    device_.reset();
    interface_.reset();
    return false;
  }

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
  return true;
}

void Device::Shutdown() {
  // With the debug layer on, what is still alive once every wrapper is gone is
  // a leak; the report lists it by name.
  backend::LiveObjectReport report;
  if (debug_layer_) {
    report.Capture(device_.get());
  }
  queue_.reset();
  device_.reset();
  interface_.reset();
  report.ReportAndRelease();
  debug_layer_ = false;
}

std::unique_ptr<plume::RenderTexture> Device::CreateTexture(const plume::RenderTextureDesc& desc,
                                                            const char* name) {
  if (!device_ || IsLost()) {
    return nullptr;
  }
  auto texture = device_->createTexture(desc);
  if (backend::IsNullBacked(texture.get())) {
    REXLOG_ERROR("[gpu] texture '{}' failed: {}x{}x{} mips={} fmt={} flags={:#x} samples={}",
                 name ? name : "?", desc.width, desc.height, desc.depth, desc.mipLevels,
                 static_cast<uint32_t>(desc.format), static_cast<uint32_t>(desc.flags),
                 static_cast<uint32_t>(desc.multisampling.sampleCount));
    ReportCreationFailure("texture", name);
    return nullptr;
  }
  if (name) {
    texture->setName(name);
  }
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
    REXLOG_ERROR("[gpu] texture view of '{}' failed: fmt={} dim={} mips={}", name ? name : "?",
                 static_cast<uint32_t>(desc.format), static_cast<uint32_t>(desc.dimension),
                 desc.mipLevels);
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
    REXLOG_ERROR("[gpu] sampler '{}' failed", name ? name : "?");
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
    REXLOG_ERROR(
        "[gpu] descriptor set '{}' failed: {} views + {} samplers (the shader-visible heaps are "
        "shared by every set; a full heap is the usual cause)",
        name ? name : "?", view_count, sampler_count);
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
    REXLOG_ERROR("[gpu] pipeline layout '{}' failed", name ? name : "?");
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
    REXLOG_ERROR("[gpu] shader '{}' has no bytecode", name ? name : "?");
    return nullptr;
  }
  auto shader = device_->createShader(data, size, entry_point, kHostShaderFormat);
  if (backend::IsNullBacked(shader.get())) {
    REXLOG_ERROR("[gpu] shader '{}' failed: {} bytes of {}", name ? name : "?", size,
                 kHostShaderFormat == plume::RenderShaderFormat::DXIL ? "DXIL" : "SPIR-V");
    ReportCreationFailure("shader", name);
    return nullptr;
  }
  if (name) {
    shader->setName(name);
  }
  return shader;
}

std::unique_ptr<plume::RenderQueryPool> Device::CreateQueryPool(uint32_t query_count,
                                                                const char* name) {
  if (!device_ || IsLost()) {
    return nullptr;
  }
  auto pool = device_->createQueryPool(query_count);
  if (backend::IsNullBacked(pool.get())) {
    REXLOG_ERROR("[gpu] query pool '{}' of {} timestamps failed", name ? name : "?",
                 query_count);
    ReportCreationFailure("query pool", name);
    return nullptr;
  }
  return pool;
}

std::unique_ptr<plume::RenderFramebuffer> Device::CreateFramebuffer(
    const plume::RenderFramebufferDesc& desc, const char* name) {
  if (!device_ || IsLost()) {
    return nullptr;
  }
  auto framebuffer = device_->createFramebuffer(desc);
  if (backend::IsNullBacked(framebuffer.get())) {
    REXLOG_ERROR("[gpu] framebuffer '{}' of {} colour targets{} failed", name ? name : "?",
                 desc.colorAttachmentsCount, desc.depthAttachment ? " and depth" : "");
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
  auto buffer = device_->createBuffer(desc);
  if (backend::IsNullBacked(buffer.get())) {
    REXLOG_ERROR("[gpu] buffer '{}' failed: {} bytes heap={} flags={:#x}", name ? name : "?",
                 desc.size, static_cast<uint32_t>(desc.heapType),
                 static_cast<uint32_t>(desc.flags));
    ReportCreationFailure("buffer", name);
    return nullptr;
  }
  if (name) {
    buffer->setName(name);
  }
  return buffer;
}

std::unique_ptr<plume::RenderPipeline> Device::CreateGraphicsPipeline(
    const plume::RenderGraphicsPipelineDesc& desc, const char* name) {
  if (!device_ || IsLost()) {
    return nullptr;
  }
  auto pipeline = device_->createGraphicsPipeline(desc);
  if (backend::IsNullBacked(pipeline.get())) {
    REXLOG_ERROR("[gpu] graphics pipeline '{}' failed", name ? name : "?");
    ReportCreationFailure("graphics pipeline", name);
    return nullptr;
  }
  if (name) {
    pipeline->setName(name);
  }
  return pipeline;
}

bool Device::CheckLost(const char* context) {
  if (IsLost()) {
    return true;
  }
  if (!device_) {
    return false;
  }
  const int64_t reason = backend::DeviceRemovedReason(device_.get());
  if (reason == 0) {
    return false;
  }
  if (!lost_.exchange(true, std::memory_order_acq_rel)) {
    // 0x887A0005 removed/reset, 0x887A0006 hung (TDR), 0x887A0007 internal
    // error, 0x887A0020 driver internal error.
    REXLOG_ERROR("[gpu] device lost, noticed at {}: reason {:#010x}", context ? context : "?",
                 static_cast<uint32_t>(reason));
  }
  return true;
}

void Device::ReportCreationFailure(const char* what, const char* name) {
  if (CheckLost(what)) {
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
  return device_ ? backend::QueryVideoMemory(device_.get()) : backend::VideoMemory{};
}

}  // namespace mcla::gpu
