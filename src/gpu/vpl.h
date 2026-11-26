#pragma once

#include <array>
#include <memory>
#include <vector>

#include "frame.h"
#include "host_passes.h"
#include "local_lights_sources.h"
#include "resolve.h"
#include "vpl_rules.h"
#include "vol_lamp_list.h"

namespace mcla::gpu {

class AmbientOcclusion;
class BindlessHeap;
class CarFrame;
class CarRtScene;
class Device;
class FrameRing;
class GBuffer;
class GiApply;
class LightManager;
class PassTiming;
class RtSceneService;
class RtWorldFrame;
class ScreenShared;

class VirtualBounce {
 public:
  void Initialize(Device&, FrameRing&, BindlessHeap&, HostPasses&, EdramTargets&, PassTiming&,
                  LightManager&, CarRtScene&, CarFrame&, ScreenShared&, AmbientOcclusion&, GBuffer&, RtWorldFrame&, RtSceneService&);
  void Shutdown();
  void BeginFrame(uint64_t frame);
  struct Direct {
    vpl::DirectFrame frame{};
    plume::RenderBuffer* buffer = nullptr;
    uint32_t size = 0;
  };
  bool composite_enabled() const { return composite_enabled_; }
  bool hit_recorded() const { return hit_recorded_; }
  bool irradiance_recorded() const { return irradiance_recorded_; }
  bool composite_recorded() const { return composite_recorded_; }
  bool OnTrigger(const ResolveCache::Summary&, plume::RenderCommandList*, bool planes, uint32_t slot, float field_share, GiApply* gi = nullptr, const Direct* direct = nullptr);
  const HostImage* Capture() const { return recorded_ ? &irradiance_ : nullptr; }
  uint64_t HeldBytes() const;

 private:
  struct Slot {
    std::unique_ptr<plume::RenderBuffer> input;
    std::unique_ptr<plume::RenderDescriptorSet> set;
  };
  struct Programs {
    std::unique_ptr<plume::RenderPipeline> hit;
    std::unique_ptr<plume::RenderPipeline> hit_rt;
    std::unique_ptr<plume::RenderPipeline> irradiance;
    std::unique_ptr<plume::RenderPipeline> composite;
    std::unique_ptr<plume::RenderPipeline> combined_irradiance;
    std::unique_ptr<plume::RenderPipeline> combined_composite;
    plume::RenderFormat combined_format = plume::RenderFormat::UNKNOWN;
    plume::RenderFormat format = plume::RenderFormat::UNKNOWN;
  };

  bool Record(const ResolveCache::Summary&, plume::RenderCommandList*, bool planes, uint32_t index,
              float field_share, bool combined, GiApply*, const Direct*);
  bool EnsureResources(uint32_t slot);
  bool EnsurePrograms(plume::RenderFormat format, uint32_t samples, bool ray, bool beams, bool direct, bool combined);
  void ReleaseImages();
  void Gather(const float eye[3], float night, float field_share);
  void GatherWorld(const float eye[3], float night, float field_share);
  float VirtualShare(const larecomp::gpu::Light&, float field_share) const;
  uint32_t WriteBeams(vpl::Frame* input);
  uint32_t SourceStencil(const larecomp::gpu::Light&) const;
  void Draw(plume::RenderCommandList*, const Slot&, plume::RenderPipeline&, plume::RenderFramebuffer&,
            uint32_t width, uint32_t height, uint32_t clear_targets) const;

  Device* device_ = nullptr;
  FrameRing* frames_ = nullptr;
  BindlessHeap* heap_ = nullptr;
  HostPasses* passes_ = nullptr;
  EdramTargets* edram_ = nullptr;
  PassTiming* timing_ = nullptr;
  LightManager* lights_ = nullptr;
  CarRtScene* rt_ = nullptr;
  CarFrame* car_ = nullptr;
  ScreenShared* screen_ = nullptr;
  AmbientOcclusion* ao_ = nullptr;
  GBuffer* gbuffer_ = nullptr;
  RtWorldFrame* world_ = nullptr;
  RtSceneService* rt_service_ = nullptr;

  uint64_t frame_ = 0;
  uint64_t history_frame_ = 0;
  double history_time_ = 0.0;
  uint32_t generation_ = 0;
  uint32_t idle_ = 0;
  bool enabled_ = false;
  bool rt_requested_ = false;
  bool recorded_ = false;
  bool hit_recorded_ = false;
  bool irradiance_recorded_ = false;
  bool composite_recorded_ = false;
  bool composite_enabled_ = false;
  bool cockpit_ = false;
  std::array<bool, 2> logged_{};
  std::array<uint64_t, vpl::kMaxLights> history_ids_{};
  larecomp::gpu::local_lights::Candidates candidates_;
  std::vector<larecomp::gpu::Light> beams_;
  std::vector<larecomp::gpu::Light> world_beams_;
  std::vector<vol_lamps::GameLamp> street_lamps_;
  uint64_t street_sequence_ = 0;
  std::vector<larecomp::gpu::neon_light::NeonCar> neon_;

  HostImage irradiance_;
  HostImage direct_irradiance_;
  std::unique_ptr<plume::RenderFramebuffer> irradiance_mrt_;
  // Descriptors name these resources. Shutdown releases the sets first.
  std::unique_ptr<plume::RenderBuffer> history_;
  std::array<Slot, kFramesInFlight> slots_;
  plume::RenderDescriptorSetBuilder set_desc_;
  std::unique_ptr<plume::RenderPipelineLayout> layout_;
  std::unique_ptr<plume::RenderPipelineLayout> rt_layout_;
  std::array<Programs, 4> programs_;
};

}  // namespace mcla::gpu
