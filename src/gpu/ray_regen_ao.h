#pragma once

#include "host_passes.h"
#include "ray_regen.h"
#include "ray_regen_guides_rules.h"

namespace mcla::gpu {

// Builds the AO signal (1 = open) and AMD's shared guides in one dispatch.
// CHECK (RG-7): callers must supply the AO site's context. The SSR context
// dispatches later and cannot also hold this signal.
class RayRegenAoPass {
 public:
  void Initialize(Device&, FrameRing&, BindlessHeap&, HostPasses&);
  void Shutdown();

  struct Frame {
    plume::RenderCommandList* list = nullptr;
    const HostImage* signal = nullptr;
    const HostImage* depth = nullptr;
    const HostImage* normal = nullptr;
    const HostImage* id_share = nullptr;
    uint32_t gbuffer[3] = {};  // RT1 normals, RT2 sqrt albedo, RT3 material
    uint32_t render[2] = {};
    uint32_t vehicle_table_slot = 0xFFFFFFFFu;
    uint32_t vehicle_ids = 0;
    float tan_x = 0, tan_y = 0, prev_tan_x = 0, prev_tan_y = 0;
    float motion[12] = {};
    float jitter[2] = {};      // pixels at the traced size
  };
  bool Record(const Frame&);
  const HostImage* Dispatch(plume::RenderCommandList*, rayregen::RayRegen&, rayregen::DispatchFrame);
  void OnPresent();
  uint64_t HeldBytes() const;

 private:
  using Image = rayregen::guides::Image;
  plume::RenderPipeline* Pipeline();
  bool EnsureImages(uint32_t width, uint32_t height);
  void ReleaseImages();
  TemporalImage Tagged(Image image) const;

  Device* device_ = nullptr;
  FrameRing* frames_ = nullptr;
  BindlessHeap* heap_ = nullptr;
  HostPasses* passes_ = nullptr;
  plume::RenderDescriptorSetBuilder output_builder_;
  std::unique_ptr<plume::RenderPipelineLayout> layout_;
  std::unique_ptr<plume::RenderShader> shader_;
  std::unique_ptr<plume::RenderPipeline> pipeline_;
  std::unique_ptr<plume::RenderDescriptorSet> output_;
  std::array<HostImage, rayregen::guides::kImageCount> images_;
  uint32_t idle_frames_ = 0;
  bool listed_ = false;
  bool failed_ = false;
  plume::RenderCommandList* recorded_on_ = nullptr;
};

}  // namespace mcla::gpu
