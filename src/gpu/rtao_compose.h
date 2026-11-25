#pragma once

#include <array>

#include "host_passes.h"

namespace mcla::gpu {

class RtAoCompose {
 public:
  void Initialize(Device&, FrameRing&, BindlessHeap&, HostPasses&);
  void Shutdown();
  bool available() const;

  struct Frame {
    plume::RenderCommandList* list;
    const HostImage* filtered;
    const HostImage* depth_guide;
    const HostImage* depth_full;
    uint32_t normal_slot;  // Reserved soft input. The compose weights use depth and vehicle IDs.
    uint32_t screen_term_slot, bounce_slot;
    uint32_t traced[2], plan_ao[2];
    float power, floor_;
    bool blend_screen;
    int view;
  };
  uint32_t Record(const Frame&);

  // Optional trace inputs for stencil-exact upsampling and raw debug views.
  // Screen and bounce textures arrive readable by compute as well as graphics.
  struct Guides {
    const HostImage* raw = nullptr;
    const HostImage* id_share = nullptr;
    uint32_t stencil_slot = 0xFFFFFFFFu;
    uint32_t stencil_sample = 0;
    bool stencil_ms = false;
    uint32_t render[2] = {};
    uint32_t car_term_slot = 0xFFFFFFFFu;
  };
  uint32_t Record(const Frame&, const Guides&);
  // The planner can make the image and its set before choosing the fallback.
  bool EnsureImage(uint32_t width, uint32_t height, bool bounce);
  void ReleaseImage();
  uint64_t HeldBytes() const;

 private:
  std::unique_ptr<plume::RenderPipeline> MakePipeline(bool bounce) const;
  plume::RenderPipeline* Pipeline(bool bounce) const;

  Device* device_ = nullptr;
  FrameRing* frames_ = nullptr;
  BindlessHeap* heap_ = nullptr;
  HostPasses* passes_ = nullptr;
  plume::RenderDescriptorSetBuilder output_builder_;
  std::unique_ptr<plume::RenderPipelineLayout> layout_;
  std::array<std::unique_ptr<plume::RenderShader>, 2> shaders_;
  mutable std::array<std::unique_ptr<plume::RenderPipeline>, 2> pipelines_;
  mutable std::array<bool, 2> listed_{};
  mutable std::array<bool, 2> failed_{};
  std::unique_ptr<plume::RenderDescriptorSet> output_;
  HostImage image_;
};

}  // namespace mcla::gpu
