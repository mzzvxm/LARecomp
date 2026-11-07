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
//            render pixels, +y down, this frame's jitter taken out. The camera's motion, from this
//            renderer's OWN view-projection history (c69 is not the previous camera: census R2.2 and
//            GHOSTING-DESIGN.md 9.3), for every pixel; a pixel that shows a vehicle (the stencil ID
//            the game's draws write) takes the vehicle's own: the camera's and the game's
//            per-vehicle matrix in one 4x4, from the table of vehicle_motion.h, drawn into a small
//            image first (RecordVehicleTable). What moves by itself without an ID (pedestrians,
//            particles, glass) keeps the camera term and can ghost when it moves. After a history
//            restart the image is zero.
//   mask     R8_UNORM, render size, when the frame drew a car's shadow decal: the reactive input of
//            the upscalers (vehicle_motion_mask.h), made from the Scene's second draws of those
//            decals.
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
#include "vehicle_motion.h"
#include "vehicle_motion_mask.h"

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
    // The anchor's stencil view (fetch 1: the vehicle ID in its stencil byte >> 2): its slot and the
    // six dwords of its fetch constant, from which the component that holds the stencil is found.
    uint32_t stencil_slot = 0;
    uint32_t stencil_fetch[6] = {};
    uint32_t render_size[2] = {};  // the main view, host pixels
    uint32_t guest_size[2] = {};   // c62
    float jitter[2] = {};          // this frame's, render pixels, +y down (zero when not armed)
    const temporal::Camera* camera = nullptr;    // this frame's (valid)
    const temporal::Camera* previous = nullptr;  // the frame before's; null: the history starts again
    // The vehicles' matrices of this frame (vehicle_motion.h), made when `previous` is there: the
    // usable rows are the bits of `vehicles.valid`.
    temporal::VehicleTable vehicles;
    temporal::VehicleStats vehicle_stats;  // what making the table found (for the summary)
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
    // R8_UNORM, render size, SHADER_READ: the shadow decals' mask of this frame, null when the frame
    // drew none (the upscalers then get no mask).
    const HostImage* mask = nullptr;
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

  // The decals' mask: the Scene draws the decals into it and asks for its pipeline key (scene.cpp).
  ShadowMask& shadow_mask() { return mask_; }

  // The component (0 to 3) of the stencil view that holds the stencil byte, from the six dwords of its
  // fetch constant: the host swizzle the texture cache gives the view (the fetch constant's over
  // k_8_8_8_8's) names, for each component, the product's channel it reads, and the stencil is the
  // product's red. 4 when no component reads red (or the constant is no texture): no ID.
  // `host_swizzle` (optional) gets the swizzle, for the log.
  static uint32_t StencilChannel(const uint32_t fetch[6], uint32_t* host_swizzle = nullptr);
  // What the passes take for the view: the slot (the gamma bit dropped) in bits 0 to 27 and the
  // component in bits 28 to 30 (hlsl/temporal_common.hlsli).
  static uint32_t StencilRef(uint32_t slot, uint32_t channel) {
    return (slot & 0x0FFFFFFFu) | ((channel & 7u) << 28);
  }

  // The vehicle table's counts and the shadow decals' mask for the summary; empty when neither was
  // ever used.
  std::string Summary() const;

 private:
  // hlsl/temporal_mv_ps.hlsl MotionPush. CHECK: the same offsets there (depth 0, stencil 4, mode 8,
  // table 12, jitter 16, size 24, to_render 32, guard 40, columns 48, 64 and 80) under both the D3D
  // and the Vulkan packing.
  struct MotionPush {
    uint32_t depth;
    uint32_t stencil;  // StencilRef of the anchor's stencil view
    uint32_t mode;     // 0 every pixel, 2 zero
    uint32_t table;    // the vehicle table's slot, kNoTable: none
    float jitter[2];
    float size[2];
    float to_render[2];
    float guard;       // render pixels: the most a vehicle's vector may be away from the camera's
    float reserved;
    float col_x[4];
    float col_y[4];
    float col_w[4];
  };
  static_assert(sizeof(MotionPush) == 96);
  static constexpr uint32_t kMotionPushBytes = sizeof(MotionPush);
  static constexpr uint32_t kNoTable = 0xFFFFFFFFu;
  // A vehicle's vector is taken only when it is within this share of the frame's short side of the
  // camera's: a car placed or teleported in the frame holds garbage (GHOSTING-DESIGN.md 3.1, 9.3).
  static constexpr float kGuardShare = 0.25f;

  struct TablePush {  // hlsl/temporal_vehicle_table_ps.hlsl: one row of the vehicle table
    float col_x[4];
    float col_y[4];
    float col_w[4];
  };
  static_assert(sizeof(TablePush) == 48);

  struct CopyPush {  // hlsl/temporal_copy_ps.hlsl
    uint32_t source;
  };

  // The layout, shaders and pipelines, on first use. False (and failed_) when one cannot be made.
  bool Prepare();
  // The vehicle table: one draw of 4 x 1 pixels a usable vehicle into its row of table_. False when
  // there is none (the vector pass then gives every pixel the camera's motion).
  bool RecordVehicleTable(plume::RenderCommandList* list, const Frame& frame);
  // The vector image: one draw over all of it. `stencil` is StencilRef of the frame's view, `table`
  // the vehicle table's slot or kNoTable.
  void RecordMotion(plume::RenderCommandList* list, const Frame& frame, bool has_previous,
                    uint32_t stencil, uint32_t table);
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
  std::unique_ptr<plume::RenderShader> table_shader_;
  std::unique_ptr<plume::RenderPipeline> motion_pipeline_;
  std::unique_ptr<plume::RenderPipeline> depth_pipeline_;
  std::unique_ptr<plume::RenderPipeline> table_pipeline_;

  // The images. [cur_] is this frame's; the other one holds the frame before's.
  HostImage depth_[2];
  HostImage motion_;
  HostImage table_;  // the vehicle table: R32G32B32A32_FLOAT, 4 x 24, a row a stencil ID
  ShadowMask mask_;  // the shadow decals' mask (vehicle_motion_mask.h)
  uint32_t cur_ = 0;
  bool recorded_ = false;        // depth_[cur_] and motion_ hold a recorded frame
  uint64_t recorded_frame_ = 0;
  bool motion_zero_ = false;
  uint32_t logged_size_[2] = {};  // the render size the log last named
  bool mask_recorded_ = false;    // the frame's mask was made (kept().mask)

  // For Summary().
  struct VehicleCounts {
    uint64_t frames = 0;     // frames the passes ran in with a previous frame
    uint64_t with_ids = 0;   // ... of them, with vehicle IDs written before the anchor
    uint64_t ids = 0;        // IDs asked, taken and left out, summed over the frames
    uint64_t taken = 0;
    uint64_t not_sane = 0;
    uint64_t not_rigid = 0;
    uint64_t not_finite = 0;
    uint64_t no_world = 0;   // frames whose c69 was not usable
    uint64_t tables = 0;     // frames with a table drawn
    uint32_t max_ids = 0;
    bool no_stencil_logged = false;
  };
  VehicleCounts counts_;
  uint32_t logged_swizzle_ = 0xFFFFFFFFu;  // the stencil view's host swizzle the log last named
};

}  // namespace mcla::gpu
