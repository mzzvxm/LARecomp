#pragma once

#include <array>
#include <cstdint>
#include <memory>

#include "backend.h"
#include "frame.h"
#include "host_passes.h"
#include "ssr_rules.h"

namespace mcla::gpu {

class BindlessHeap;
class Device;

// Tile feedback is collected before RT modifies the screen signal. The
// ping-pong stores one tile per texel, independently of the denoiser.
class ReflectionClassifier {
 public:
  void Initialize(Device& device, FrameRing& frames, BindlessHeap& heap, HostPasses& passes,
                  const plume::RenderDescriptorSetBuilder& handoff);
  void Shutdown();
  void Release();
  bool Prepare(uint32_t width, uint32_t height, bool rt);
  void Reproject(plume::RenderCommandList& list, plume::RenderDescriptorSet& handoff, const ssr::Push& push);
  void Record(plume::RenderCommandList& list, plume::RenderDescriptorSet& handoff, const ssr::Push& push, bool rt);

  uint32_t HistorySlot() const { return tiles_[write_ ^ 1u].slot; }
  bool HistoryValid() const { return valid_; }
  uint32_t PlanSlot() const { return plan_.slot; }
  uint32_t Stride() const { return (width_ * height_ + 65534u) / 65535u; }
  plume::RenderBuffer* TileList() const { return tile_list_.get(); }
  uint64_t TileListBytes() const { return list_bytes_; }
  const backend::IndirectCompute* Dispatch() const { return indirect_.get(); }
  uint64_t HeldBytes() const { return uint64_t(width_) * height_ * 24u + list_bytes_; }

 private:
  std::unique_ptr<plume::RenderPipeline> MakePipeline(uint32_t variant);
  void Bind(plume::RenderCommandList& list, plume::RenderDescriptorSet& handoff, uint32_t variant);

  Device* device_ = nullptr;
  FrameRing* frames_ = nullptr;
  BindlessHeap* heap_ = nullptr;
  HostPasses* passes_ = nullptr;
  plume::RenderDescriptorSetBuilder output_builder_;
  std::unique_ptr<plume::RenderPipelineLayout> layout_;
  std::array<std::unique_ptr<plume::RenderShader>, 2> shaders_;
  std::array<std::unique_ptr<plume::RenderPipeline>, 2> pipelines_;
  bool listed_ = false;
  bool failed_ = false;
  std::array<std::unique_ptr<plume::RenderDescriptorSet>, kFramesInFlight> sets_;
  std::array<HostImage, 2> tiles_;
  HostImage plan_;
  std::unique_ptr<plume::RenderBuffer> tile_list_;
  std::unique_ptr<plume::RenderBuffer> unused_args_;
  std::unique_ptr<backend::IndirectCompute> indirect_;
  uint64_t list_bytes_ = 0;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint32_t write_ = 0;
  bool valid_ = false;
};

}  // namespace mcla::gpu
