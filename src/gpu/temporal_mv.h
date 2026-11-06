#pragma once
// ===========================================================================
// MCLA GPU renderer -- the temporal system's passes: motion vectors, the images it keeps, the
// debug views and the checks (F4 step 1b)
// ===========================================================================
// feature-parity/F4-DESIGN.md 2.4, 6.1 and 9, as the census of the real game corrected them
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
//            camera: census R2.2, the first drives). With mcla_gpu_temporal_vehicles (debug) the
//            pixels of a vehicle ID whose c69+4k slot is usable get that vehicle's own matrix, and
//            the vehicles' rigid-transform log (temporal.h) says whether the slots mean that. IDs
//            without a usable matrix, IDs of 24 and above, and everything the game gives no ID
//            (far cars, pedestrians, particles, glass) keep the camera term and will ghost when
//            they move. After a history restart the image is zero.
//   ids      stencil >> 2 as a byte (R8_UNORM): only while the stencil debug view is on.
//
// They are the images a backend (FSR, DLSS) reads at the anchor, and the ones frame generation
// keeps for the present (kept()); the depth is double buffered (the previous frame's depth, for the
// reprojection check). Sizes and formats are in Kept.
//
// Development views (mcla_gpu_temporal_view: off, mv, depth, stencil): the chosen image over the
// whole picture in the present, letterboxed. Mid grey in the vector view is no motion; `range`
// (mcla_gpu_temporal_view_range, render pixels) is full colour.
//
// Checks (mcla_gpu_temporal_check = N, development): every N-th frame whose vectors have a previous
// frame, 16 x 9 sample pixels are gathered on the GPU (hlsl/temporal_check_ps.hlsl), read back
// without waiting, and judged on the host in double precision, two ways, in one log line
// "[gpu] temporal mv: check ...":
//   analytic     the vector of every sample pixel against the world point seen there (this frame's
//                view-projection inverse and the stored depth) projected by the previous frame's
//                view-projection: the plumbing (row vectors, y flip, the half pixel, jitter removal,
//                scale) apart from the shader. Expect well under 0.05 pixel for vectors under 16
//                pixels (the image is half precision).
//   reprojection the previous frame's depth and colour read at the pixel the vector says: the
//                depth must agree with the reprojected depth, and the colour error with the vectors
//                must be lower than with none when the camera moves (design 9.1).
// The history copy of the colour (a full-screen pass a frame, only around a check) is the cost.
//
// Threading: render thread only (the readbacks are judged by FrameRing retire actions, which run
// on it).
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
  // The check's grid and its image: two texels (even, odd) per sample, rows of 512 bytes.
  static constexpr uint32_t kCheckColumns = 16;
  static constexpr uint32_t kCheckRows = 9;
  static constexpr uint32_t kCheckImageWidth = 2 * kCheckColumns;
  static constexpr uint32_t kCheckRowBytes = kCheckImageWidth * 16;  // RGBA32F, a multiple of 256
  static constexpr uint32_t kReadbacks = kFramesInFlight + 2;
  static constexpr uint32_t kMaxCheckLines = 40;
  // The vector image is half precision: errors are judged on the vectors up to this many pixels.
  static constexpr double kSmallVector = 16.0;

  enum class View : uint8_t { kOff, kMotion, kDepth, kStencil };

  // What TemporalSystem tells of an accepted anchor.
  struct Frame {
    uint64_t frame = 0;
    // The anchor's fetches as bindless slots (the gamma bit is dropped by the shaders).
    uint32_t depth_slot = 0;
    uint32_t stencil_slot = 0;
    uint32_t color_slot = 0;
    uint32_t render_size[2] = {};  // the main view, host pixels
    uint32_t guest_size[2] = {};   // c62
    float jitter[2] = {};          // this frame's, render pixels, +y down (zero when not armed)
    float previous_jitter[2] = {};
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

  // Nothing is made until the first Record (the system is a development switch): this only keeps
  // what the passes need. `target_format`: the back buffer's.
  void Initialize(Device& device, FrameRing& frames, BindlessHeap& heap, HostPasses& passes,
                  PassTiming& pass_timing, plume::RenderFormat target_format);
  // GPU idle (every retire action run), before HostPasses and the BindlessHeap shut down.
  void Shutdown();
  // The switch went off: the images go (retired behind the frames that name them); the passes stay.
  void Release();

  // False once the passes could not be made (logged): TemporalSystem then records nothing.
  bool available() const { return initialized_ && !failed_; }

  // The anchor draw of a frame, `list` outside any render pass. Reads the development cvars.
  // Records the passes; false when nothing was recorded (the caller keeps its list state).
  bool Record(plume::RenderCommandList* list, const Frame& frame);

  // Inside the back buffer's render pass, after the present's draw: the debug view of the last
  // recorded frame, in `width` x `height` pixels. Nothing when the view is off or no frame is kept.
  void DrawDebug(plume::RenderCommandList* list, uint32_t width, uint32_t height);

  Kept kept() const;

  // The counters since the last call, for the temporal summary; false when nothing happened.
  struct Counters {
    uint64_t frames = 0;        // frames with vectors
    uint64_t zero_frames = 0;   // ... of them with no previous frame (vectors zero)
    uint64_t vehicle_draws = 0; // vehicle ID passes (debug)
    uint64_t checks = 0;        // reads judged
    uint64_t failed = 0;        // frames the images could not be made for
  };
  Counters TakeCounters();

  // The cvars, read once a frame (declared in temporal_mv.cpp).
  static View ReadView();

 private:
  // hlsl/temporal_mv_ps.hlsl MotionPush.
  struct MotionPush {
    uint32_t depth;
    uint32_t stencil;
    uint32_t mode;  // 0 every pixel, 1 the pixels of `id`, 2 zero
    uint32_t id;
    float jitter[2];
    float size[2];
    float to_render[2];
    float reserved[2];
    float col_x[4];
    float col_y[4];
    float col_w[4];
  };
  static_assert(sizeof(MotionPush) == 96);
  static constexpr uint32_t kMotionPushBytes = sizeof(MotionPush);

  struct CopyPush {  // hlsl/temporal_copy_ps.hlsl
    uint32_t source;
  };
  struct CheckPush {  // hlsl/temporal_check_ps.hlsl
    uint32_t depth;
    uint32_t stencil;
    uint32_t motion;
    uint32_t prev_depth;
    uint32_t color;
    uint32_t prev_color;
    float delta[2];
  };
  static_assert(sizeof(CheckPush) == HostPasses::kPushConstantBytes);
  struct DebugPush {  // hlsl/temporal_debug_ps.hlsl
    uint32_t source;
    uint32_t mode;
    float origin[2];
    float scale[2];
    float range;
    float reserved;
  };
  static_assert(sizeof(DebugPush) == HostPasses::kPushConstantBytes);

  // What a judged readback needs, copied out of the frame (the retire action runs later).
  struct CheckJob {
    uint64_t frame = 0;
    uint32_t render_size[2] = {};
    uint32_t guest_size[2] = {};
    double to_render[2] = {};
    double jitter[2] = {};
    temporal::Mat4 view_projection_inverse;  // this frame's
    temporal::Mat4 previous_view_projection;  // the frame before's
    double previous_near = 1.0;
    double previous_far = 2.0;
    uint32_t vehicle_slots = 0;  // IDs drawn with their own matrix (mask, bit k)
  };
  struct Readback {
    std::unique_ptr<plume::RenderBuffer> buffer;
    bool busy = false;
  };

  // The layout, shaders and pipelines, on first use. False (and failed_) when one cannot be made.
  bool Prepare();
  bool PrepareDebug();
  void ReadSettings();
  // The vector image: one draw over all of it, then one more per usable vehicle slot when asked.
  void RecordMotion(plume::RenderCommandList* list, const Frame& frame, bool has_previous);
  void BindMotion(plume::RenderCommandList* list) const;
  void RecordCheck(plume::RenderCommandList* list, const Frame& frame);
  bool EnsureReadbacks();
  void Process(uint32_t readback, const CheckJob& job);
  void Judge(const CheckJob& job, const float* rows);

  Device* device_ = nullptr;
  FrameRing* frames_ = nullptr;
  BindlessHeap* heap_ = nullptr;
  HostPasses* passes_ = nullptr;
  PassTiming* pass_timing_ = nullptr;
  plume::RenderFormat target_format_ = plume::RenderFormat::UNKNOWN;
  bool initialized_ = false;
  bool failed_ = false;

  // The settings of the frame being recorded, and what the log said of them last.
  View view_ = View::kOff;
  bool vehicles_ = false;
  uint32_t check_every_ = 0;
  View reported_view_ = View::kOff;
  bool reported_vehicles_ = false;
  uint32_t reported_check_ = 0;

  std::unique_ptr<plume::RenderPipelineLayout> motion_layout_;  // the host layout with 96 bytes of push
  std::unique_ptr<plume::RenderShader> motion_shader_;
  std::unique_ptr<plume::RenderShader> depth_shader_;
  std::unique_ptr<plume::RenderShader> color_shader_;
  std::unique_ptr<plume::RenderShader> ids_shader_;
  std::unique_ptr<plume::RenderShader> check_shader_;
  std::unique_ptr<plume::RenderPipeline> motion_pipeline_;
  std::unique_ptr<plume::RenderPipeline> depth_pipeline_;
  std::unique_ptr<plume::RenderPipeline> color_pipeline_;
  std::unique_ptr<plume::RenderPipeline> ids_pipeline_;
  std::unique_ptr<plume::RenderPipeline> check_pipeline_;
  std::unique_ptr<plume::RenderShader> debug_shader_;
  std::unique_ptr<plume::RenderPipeline> debug_pipeline_;
  bool debug_failed_ = false;

  // The images. [cur_] is this frame's; the other one holds the frame before's.
  HostImage depth_[2];
  HostImage color_[2];  // only around a check
  HostImage motion_;
  HostImage ids_;       // only with the stencil view
  HostImage check_;     // 32 x 9 RGBA32F
  uint32_t cur_ = 0;
  bool recorded_ = false;        // depth_[cur_] and motion_ hold a recorded frame
  uint64_t recorded_frame_ = 0;
  bool motion_zero_ = false;
  bool ids_recorded_ = false;    // ids_ holds the recorded frame's
  bool color_recorded_[2] = {};  // color_[i] holds the colour of frame color_frame_[i]
  uint64_t color_frame_[2] = {};
  uint64_t records_ = 0;         // frames recorded, the check's clock
  uint32_t logged_size_[2] = {};  // the render size the log last named

  std::array<Readback, kReadbacks> readbacks_;
  uint32_t next_readback_ = 0;
  bool readbacks_failed_ = false;
  uint32_t check_lines_ = 0;
  uint64_t generation_ = 0;  // a retire action of a shut down system does nothing

  Counters counters_;
};

}  // namespace mcla::gpu
