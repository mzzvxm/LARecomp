#pragma once

#include <array>
#include <memory>
#include <string>

#include "ao.h"
#include "city_rt_shadow_rules.h"
#include "frame.h"
#include "gi_pixel_rules.h"
#include "host_passes.h"
#include "rtao_rules.h"

#if __has_include("rtao_setting.h")
#include "rtao_setting.h"
#else
// STUB (R3 owns): the standalone trace branch keeps the setting gate shut.
namespace mcla::gpu::rtao_setting {
struct Decision {
  bool rt = false;
  gpu_class::RtTier rung = gpu_class::RtTier::kNone;
};
inline Decision DecideFor(std::string_view) { return {}; }
inline void NotePassBuilt(bool) {}
}
#endif

namespace mcla::gpu {

class CarRtScene;
class CityRtScene;
class RtSceneService;
class RtAoDenoise;
class RtAoCompose;
struct CarRtView;

class RtAo {
 public:
  static constexpr uint32_t kNoSlot = 0xFFFFFFFFu;
  RtAo();
  ~RtAo();
  RtAo(const RtAo&) = delete;
  RtAo& operator=(const RtAo&) = delete;

  void Initialize(Device&, FrameRing&, BindlessHeap&, HostPasses&, PassTiming&, CarRtScene&, CityRtScene*, RtSceneService&, bool listed_at_start);
  void Shutdown();
  bool available() const;
  void BeginFrame(const rtao_setting::Decision&, bool play_view);
  bool wanted() const { return wanted_; }
  void SetRawMode(bool raw) { raw_ = raw; }
  bool raw_frame() const { return plan_.raw; }

  struct Inputs {
    const HostImage* depth = nullptr;
    uint32_t normal_slot = kNoSlot;
    uint32_t stencil_slot = kNoSlot;
    uint32_t stencil_sample = 0;
    bool stencil_ms = false;
    uint32_t render[2] = {};
    float tan_x = 0, tan_y = 0, fade_end = 160.0f;
    float look_radius = 3.0f, look_power = 3.6f, look_floor = 0.1f, car_reach = 0.4f;
    uint64_t noise_index = 0;
    uint32_t noise_code = 0;
    uint64_t frame = 0;
    bool interior = false;
  };

  rtao::Plan PlanFrame(const Inputs&, const CarRtView& top_level, bool bounce_light_on);
  bool RecordTrace(plume::RenderCommandList*, const Inputs&);
  struct FilterInputs {
    bool ours = true;
    AmbientOcclusion::FrameCameraOut camera;
    AmbientOcclusion::VehicleTableOut vehicles;
    bool cut = false;
  };
  bool RecordFilter(plume::RenderCommandList*, const FilterInputs&);
  struct ComposeInputs {
    uint32_t plan_ao[2] = {};
    uint32_t screen_term_slot = kNoSlot;
    uint32_t bounce_slot = kNoSlot;
    float power = 3.6f, floor = 0.1f;
    int view = 0;
  };
  uint32_t RecordCompose(plume::RenderCommandList*, const ComposeInputs&);
  void OnPresent();
  uint64_t HeldBytes() const;
  std::string CensusText() const;

 private:
  friend class AmbientOcclusion;
  bool PipelineReady(bool ms);
  bool EnsureImages(uint32_t width, uint32_t height);
  void ReleaseImages();
  void FinishTraceTiming(plume::RenderCommandList*);
  void ReadStats(uint64_t frame, uint64_t mesh_epoch, bool probe);
  void NoteFallback(uint64_t frame, rtao::Fallback);

  Device* device_ = nullptr;
  FrameRing* frames_ = nullptr;
  BindlessHeap* heap_ = nullptr;
  HostPasses* passes_ = nullptr;
  PassTiming* timing_ = nullptr;
  CityRtScene* city_ = nullptr;
  RtSceneService* service_ = nullptr;
  const CarRtView* top_level_ = nullptr;
  std::unique_ptr<RtAoDenoise> denoise_;
  std::unique_ptr<RtAoCompose> compose_;
  plume::RenderDescriptorSetBuilder output_builder_;
  std::unique_ptr<plume::RenderPipelineLayout> layout_;
  std::unique_ptr<plume::RenderShader> shader_[2];
  std::unique_ptr<plume::RenderPipeline> pipeline_[2];
  std::unique_ptr<plume::RenderDescriptorSet> output_[2];
  std::unique_ptr<plume::RenderBuffer> stats_, zeros_, readback_;
  std::array<std::unique_ptr<plume::RenderQueryPool>, kFramesInFlight> stamps_;
  plume::RenderQueryPool* pending_timing_ = nullptr;

  HostImage signal_, depth_[2], normal_, id_share_;
  const HostImage* filtered_ = nullptr;
  const HostImage* depth_full_ = nullptr;
  uint32_t normal_slot_ = kNoSlot, parity_ = 0;
  uint32_t previous_size_[2] = {};
  float tan_[2] = {}, previous_tan_[2] = {};
  uint64_t frame_ = 0, previous_frame_ = 0;
  bool previous_ours_ = false, recorded_ = false;
  bool listed_ = false, failed_[2] = {};
  bool wanted_ = false, selected_ = false, play_view_ = false, raw_ = false;
  bool blend_car_ = false, cabin_screen_ = false, interior_ = false, cabin_closed_ = false;
  bool readback_busy_ = false, probe_due_ = false;
  uint64_t player_mesh_key_ = 0, mesh_epoch_ = 0, last_probe_ = 0;
  rtao::Screen screen_ = rtao::Screen::kAuto;
  rtao::Interior interior_policy_ = rtao::Interior::kAuto;
  rtao::Budget budget_ = {};
  rtao::Plan plan_;
  city_rt_shadow::AlphaMode alpha_ = city_rt_shadow::AlphaMode::kStochastic;
  float reach_override_ = 0, range_override_ = 0;
  uint32_t rays_ = 0, scale_ = 0, check_ = 0;
  int view_ = 0;
  gi_pixel::Governor governor_;
  bool govern_ = false;
  uint64_t passes_run_ = 0, closed_probes_ = 0, open_probes_ = 0, idle_ = 0;
  uint64_t next_sample_ = 0;
  std::array<uint64_t, 7> fallback_{};
  std::array<uint64_t, 7> counters_{};
  std::array<uint32_t, 6> last_state_{};
};

}  // namespace mcla::gpu
