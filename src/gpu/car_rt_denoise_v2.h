#pragma once

#include <array>
#include <span>
#include <vector>

#include "car_rt_denoise_v2_rules.h"
#include "car_rt_paint_tiles.h"
#include "frame.h"
#include "host_passes.h"

namespace mcla::gpu {

struct CarRtView;
class BindlessHeap;
class Device;

// Both car signals use the trace's pixel grid and share the transient guides
// and spatial scratch. The two signals keep independent temporal histories.
class CarRtDenoiseV2 {
 public:
  struct Frame {
    uint64_t number = 0;
    uint64_t paint_signature = 0;
    uint64_t glass_signature = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t render_width = 0;
    uint32_t render_height = 0;
    uint32_t divisor = 1;
    float tan_x = 0.0f;
    float tan_y = 0.0f;
    float cone = 0.0f;
    bool reset = true;
    bool report = false;
  };

  void Initialize(Device& device, FrameRing& frames, BindlessHeap& heap, HostPasses& passes);
  void Precache(bool startup);
  void Shutdown();
  void Release();
  uint64_t HeldBytes() const;

  bool Prepare(const Frame& frame, const CarRtView& view, bool paint, bool glass);
  bool PreparePaint(HostImage& trace);
  bool PrepareGlass(HostImage& trace, car_rt_refl::PaintRect rect);
  HostImage& Guide() { return guide_; }
  void RecordPaint(plume::RenderCommandList* list, HostImage& trace, const HostImage& aux, const car_rt_refl::PaintRects& regions);
  void RecordGlass(plume::RenderCommandList* list, HostImage& trace, car_rt_refl::PaintRect rect);

 private:
  struct MotionInstance {
    uint64_t mesh = 0;
    uint32_t mask = 0;
    temporal::Mat4 view;
    std::array<double, 3> origin{};
  };

  struct Push {
    uint32_t slots[4];          // input, guides, previous radiance, previous features
    uint32_t extent[4];         // trace width/height, render width/height
    int32_t rect[4];            // current history bounds in trace coordinates, exclusive end
    int32_t previous_rect[4];   // previous history bounds, exclusive end
    int32_t dispatch_rect[4];
    float lens[4];              // current and previous tan(half FOV), xy per pair
    uint32_t config[4];         // pass, divisor, glass/history flags, paint aux slot
    float trace[4];             // cone tangent, number of motion rows, reserved, reserved
  };
  static_assert(sizeof(Push) == 128);

  struct Output {
    std::unique_ptr<plume::RenderDescriptorSet> set;
    uint64_t image = 0;
    uint64_t features = 0;
  };

  struct History {
    HostImage values[2];
    HostImage features[2];
    Output history_set[2];
    Output trace_set;
    Output scratch_set;
    car_rt_refl::PaintCoverage coverage[2];
    car_rt_refl::PaintRect rect;
    Frame frame;
    uint32_t index = 0;
    bool valid = false;
  };

  struct MotionSlot {
    std::unique_ptr<plume::RenderBuffer> buffer;
    std::unique_ptr<plume::RenderDescriptorSet> set;
  };

  std::unique_ptr<plume::RenderPipeline> MakePipeline();
  bool UploadMotion(const CarRtView& view, bool previous);
  bool EnsureHistory(History& history, HostImage& trace, uint32_t width, uint32_t height);
  bool EnsureSet(Output& output, const HostImage& image, const HostImage* features);
  void ReleaseHistory(History& history);
  void RetireSet(Output& output);
  void Record(plume::RenderCommandList* list, History& history, HostImage& trace, uint32_t aux,
              car_rt_refl::PaintRect rect, std::span<const car_rt_refl::PaintRect> regions, bool glass);

  Device* device_ = nullptr;
  FrameRing* frames_ = nullptr;
  BindlessHeap* heap_ = nullptr;
  HostPasses* passes_ = nullptr;
  plume::RenderDescriptorSetBuilder output_builder_;
  plume::RenderDescriptorSetBuilder motion_builder_;
  std::unique_ptr<plume::RenderPipelineLayout> layout_;
  std::unique_ptr<plume::RenderShader> shader_;
  std::unique_ptr<plume::RenderPipeline> pipeline_;
  bool listed_ = false;
  bool failed_ = false;

  Frame frame_;
  std::vector<MotionInstance> previous_;
  std::vector<MotionInstance> current_;
  std::array<MotionSlot, kFramesInFlight> motion_;
  uint32_t instance_count_ = 0;
  HostImage guide_;
  HostImage scratch_;
  History paint_;
  History glass_;
};

}  // namespace mcla::gpu
