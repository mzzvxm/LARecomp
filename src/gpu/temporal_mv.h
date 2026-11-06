#pragma once
// ===========================================================================
// MCLA GPU renderer -- the temporal system's passes: the depth and the motion vectors it keeps
// ===========================================================================
// feature-parity/F4-DESIGN.md 2.4 and 6.1, as the census of the real game corrected them
// (feature-parity/CENSUS-FINDINGS.md section 3 and Round 2). TemporalSystem (temporal.h) finds the
// anchor draw and calls Record() for it, before the anchor's own draw is recorded, with the list
// outside any render pass. The passes (all full-screen triangles, the renderer's host pass layout
// of host_passes.h, hlsl/temporal_*.hlsl, DXIL and SPIR-V):
//
//   depth    the scene's depth from the product the anchor's fetch 2 reads, kept: R32_FLOAT, render
//            size, 1 near / 0 far as the guest stores it, jittered like the scene.
//   motion   RG16_FLOAT, render size, previous minus current position of what each pixel shows, in
//            render pixels, +y down, this frame's jitter taken out. Camera motion only, for every
//            pixel, from this renderer's OWN view-projection history (c69 is not the previous
//            camera: census R2.2, the first drives). What moves by itself (vehicles, pedestrians,
//            particles, glass) keeps the camera term and can ghost when it moves: the game's own
//            per-vehicle matrices are not used. After a history restart the image is zero.
//
// They are the images a backend (FSR, DLSS) reads at the anchor, and the ones frame generation
// keeps for the present (kept()); the depth is double buffered (the previous frame's depth stays
// for what needs it). Sizes and formats are in Kept.
//
// Threading: render thread only.
// ===========================================================================

#include <array>
#include <cstdint>
#include <memory>
#include <string>

#include <plume_render_interface.h>

#include "frame.h"
#include "host_passes.h"
#include "temporal_camera.h"

namespace mcla::gpu {

class BindlessHeap;
class Device;
class PassTiming;

class TemporalMotion {
 public:
  // What TemporalSystem tells of an accepted anchor.
  struct Frame {
    uint64_t frame = 0;
    // The anchor's depth fetch as a bindless slot (the gamma bit is dropped by the shaders).
    uint32_t depth_slot = 0;
    uint32_t render_size[2] = {};  // the main view, host pixels
    uint32_t guest_size[2] = {};   // c62
    float jitter[2] = {};          // this frame's, render pixels, +y down (zero when not armed)
    const temporal::Camera* camera = nullptr;    // this frame's (valid)
    const temporal::Camera* previous = nullptr;  // the frame before's; null: the history starts again
  };

  // What the system keeps of the last frame it recorded.
  struct Kept {
    bool valid = false;
    uint64_t frame = 0;
    // R32_FLOAT, render size, 1 near / 0 far, SHADER_READ. Null when not valid.
    const HostImage* depth = nullptr;
    // RG16_FLOAT, render size, previous minus current, render pixels, +y down, jitter removed,
    // SHADER_READ.
    const HostImage* motion = nullptr;
    bool motion_is_zero = false;  // the history started again in this frame
  };

  TemporalMotion() = default;
  ~TemporalMotion();
  TemporalMotion(const TemporalMotion&) = delete;
  TemporalMotion& operator=(const TemporalMotion&) = delete;

  // Nothing is made until the first Record: this only keeps what the passes need.
  void Initialize(Device& device, BindlessHeap& heap, HostPasses& passes, PassTiming& pass_timing);
  // GPU idle (every retire action run), before HostPasses and the BindlessHeap shut down.
  void Shutdown();
  // The switch went off: the images go (retired behind the frames that name them); the passes stay.
  void Release();
  // The video memory the images hold now: the depth pair and the vectors.
  uint64_t HeldBytes() const;

  // False once the passes could not be made (logged): TemporalSystem then records nothing.
  bool available() const { return initialized_ && !failed_; }

  // The anchor draw of a frame, `list` outside any render pass. Records the passes; false when
  // nothing was recorded (the caller keeps its list state).
  bool Record(plume::RenderCommandList* list, const Frame& frame);

  Kept kept() const;

 private:
  // hlsl/temporal_mv_ps.hlsl MotionPush. CHECK: the same offsets there (depth 0, mode 4, jitter 8,
  // size 16, to_render 24, columns 32, 48 and 64) under both the D3D and the Vulkan packing.
  struct MotionPush {
    uint32_t depth;
    uint32_t mode;  // 0 every pixel, 2 zero
    float jitter[2];
    float size[2];
    float to_render[2];
    float col_x[4];
    float col_y[4];
    float col_w[4];
  };
  static_assert(sizeof(MotionPush) == 80);
  static constexpr uint32_t kMotionPushBytes = sizeof(MotionPush);

  struct CopyPush {  // hlsl/temporal_copy_ps.hlsl
    uint32_t source;
  };

  // The layout, shaders and pipelines, on first use. False (and failed_) when one cannot be made.
  bool Prepare();
  // The vector image: one draw over all of it.
  void RecordMotion(plume::RenderCommandList* list, const Frame& frame, bool has_previous);
  void BindMotion(plume::RenderCommandList* list) const;

  Device* device_ = nullptr;
  BindlessHeap* heap_ = nullptr;
  HostPasses* passes_ = nullptr;
  PassTiming* pass_timing_ = nullptr;
  bool initialized_ = false;
  bool failed_ = false;

  std::unique_ptr<plume::RenderPipelineLayout> motion_layout_;  // the host layout with 80 bytes of push
  std::unique_ptr<plume::RenderShader> motion_shader_;
  std::unique_ptr<plume::RenderShader> depth_shader_;
  std::unique_ptr<plume::RenderPipeline> motion_pipeline_;
  std::unique_ptr<plume::RenderPipeline> depth_pipeline_;

  // The images. [cur_] is this frame's; the other one holds the frame before's.
  HostImage depth_[2];
  HostImage motion_;
  uint32_t cur_ = 0;
  bool recorded_ = false;        // depth_[cur_] and motion_ hold a recorded frame
  uint64_t recorded_frame_ = 0;
  bool motion_zero_ = false;
  uint32_t logged_size_[2] = {};  // the render size the log last named
};

}  // namespace mcla::gpu
