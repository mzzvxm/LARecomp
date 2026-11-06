#pragma once
// ===========================================================================
// MCLA GPU renderer -- the groundwork of the temporal upscalers (F4 step 1a)
// ===========================================================================
// feature-parity/F4-DESIGN.md sections 0, 2, 3 and 6.1, as the census of the real game corrected
// them (feature-parity/CENSUS-FINDINGS.md section 3). One component, owned by the Renderer and
// called by the Scene, for the frames that have a 3D scene. At ratio 1 (the scene is drawn at the
// size it is shown at) it does:
//
//   Jitter.   A Halton(2,3) sub-pixel offset is added to SharedConstants::pos_offset, on the copy
//             of the shared constants a draw is about to upload (the memo keeps the unjittered
//             block), of the draws that go to the main view: the scene's colour and depth, found by
//             their exact EDRAM identity at 2x samples (temporal_identities.h), only before the
//             anchor and the composite marker, and only world draws (clipping and the viewport
//             transform on, not a rectangle list, a viewport as wide as the target). Everything
//             else keeps its position: the shadow maps, the reflections, the post chain, the UI.
//   Anchor.   The game's motion blur draw, found by its pinned shader pair and checked
//             structurally (its fetches 0 to 2, its colour target, c62). Its pixel shader constants
//             give the camera (temporal_camera.h), checked, and the cut state machine: a camera
//             cut, a change of size, a gap of more than 100 ms, a frame without an anchor before
//             it, or no usable previous matrix for the world gives `reset`.
//   State.    Per guest frame: whether the jitter is armed (the anchor was accepted in the frame
//             before), the jitter and its phase, the sizes, the camera, the anchor's bound
//             textures, the counts of draws; and the states of the run in the log.
//
// Step 1b (temporal_mv.h) adds to it, at the anchor, the passes that keep the scene's depth and make
// the motion vectors from this system's own camera history, the debug views and the checks; the
// backend steps read `frame()`, `previous_camera()` and `motion().kept()` below. It still runs no
// upscaler and changes no picture: everything is behind mcla_gpu_temporal.
//
// Development switch: mcla_gpu_temporal (off, read each frame). With it off no hook of the Scene
// does anything beyond one branch, no constant changes, no draw is added. On, it jitters, reads
// the camera and logs: "[gpu] temporal: ..." lines, one for each change of what the frames are
// (no 3D scene, 3D scene at 4x samples, anchor missing or refused, running) and a summary every
// few seconds.
//
// Threading: render thread only.
// ===========================================================================

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_set>

#include "constants.h"
#include "draw_decode.h"
#include "draw_state.h"
#include "edram.h"
#include "scale.h"
#include "shaders.h"
#include "temporal_camera.h"
#include "temporal_identities.h"
#include "temporal_mv.h"
#include "viewport.h"

namespace mcla::gpu {

class TemporalSystem {
 public:
  static constexpr double kReportSeconds = 5.0;
  // Phases of the sequence at ratio 1: 8 x ratio^2, rounded up (F4-DESIGN.md 3).
  static constexpr uint32_t kPhasesAtRatio1 = 8;
  // A longer gap between two anchors resets the history; the frame time is clamped to this.
  static constexpr double kMaxGapMs = 100.0;
  // Frames that were jittered, saw the main view, and had no accepted anchor, before the jitter is
  // switched off (until mcla_gpu_temporal is set off and on): a safety net, the census saw none.
  static constexpr uint32_t kDisarmAfterMisses = 3;
  // A new state of the frames is logged once it has lasted this many frames.
  static constexpr uint32_t kModeDebounce = 5;

  // The kinds of main view draw: colour and depth, colour only (the pass "k_2_10_10_10_FLOAT@1024
  // pitch 1280 2x | no depth" of the census, under 22 draws a frame), depth only (the occlusion
  // draws, 256 a frame at dusk and night).
  enum Kind : uint32_t { kColorDepth, kColorOnly, kDepthOnly, kKinds };
  // Why a main view draw is not a world draw (a draw may have several).
  enum GateBit : uint32_t {
    kGateClipOff = 1u << 0,        // clipping disabled: a screen-space draw
    kGateNoViewport = 1u << 1,     // the viewport transform is off
    kGateRectList = 1u << 2,       // a rectangle list (the clears, the fog quad)
    kGateNarrow = 1u << 3,         // a viewport narrower than the target
  };
  static constexpr uint32_t kGateReasons = 4;
  // Why the history starts again (a bit each).
  enum ResetBit : uint32_t {
    kResetFirst = 1u << 0,       // no accepted anchor in the frame before (first, a menu, a pause)
    kResetCut = 1u << 1,         // the camera cut rule fired
    kResetSize = 1u << 2,        // the frame's size or the scene's scale changed
    kResetTime = 1u << 3,        // more than kMaxGapMs since the previous anchor
    kResetNoPrevious = 1u << 4,  // the world's previous matrix (c69) is not usable
  };
  static constexpr uint32_t kResetReasons = 5;
  // What the anchor test said of the frame.
  enum class AnchorStatus : uint8_t {
    kNotSeen,         // the frame has no anchor draw (or the system did not see it prepared)
    kAccepted,
    kNoMainView,      // an anchor draw but no main view draw before it
    kView4x,          // the main view is at 4x samples: the pause menu, the map
    kFetches,         // fetches 0 to 2 are not the colour, stencil and depth textures at one size
    kTarget,          // its colour target is not the half-size float image
    kFrameSize,       // temporal_camera.h Reject
    kConstants,
    kViewProjection,
    kViewInverse,
    kLens,
    kCount,
  };

  // What the Scene tells of a prepared draw, after its texture tables are filled and before its
  // constants are uploaded.
  struct DrawFacts {
    const DrawView* view = nullptr;
    const XenosState* state = nullptr;
    const ConstantBanks::Banks* banks = nullptr;  // the draw's own constants
    const ShaderStore::Entry* ps = nullptr;       // null: depth only
    const ViewportInfo* viewport = nullptr;
    Scale scale;                                  // of the draw's targets
    const EdramKey* color0 = nullptr;             // null: no colour output
    const EdramKey* depth = nullptr;              // null: no depth target
  };

  // The textures the anchor draw binds: what the motion vector pass reads (F4-DESIGN.md 2.3), as
  // the draw's own fetch constants and the table the shaders index (SharedConstants::texture_2d,
  // the bindless slot and the gamma bit).
  struct AnchorTextures {
    bool bound = false;
    uint32_t fetch[3][6] = {};  // the six dwords of fetch slots 0 (scene colour), 1 (stencil view
                                // of the depth, vehicle ID in x >> 2), 2 (depth, 1 near)
    uint32_t table[3] = {};
  };

  // Everything of the frame being recorded or, between EndFrame and the next BeginFrame, of the
  // last one.
  struct FrameState {
    uint64_t frame = 0;  // guest frames since the switch was turned on
    uint32_t slot = 0;   // the frame's upload slot

    // Jitter. `jitter` is what this frame's gated draws were moved by, in host pixels of the main
    // view, +y down (zero when not armed). The projection of the vertex shader epilogue shifts
    // the image by +x to the right and +y down by that much.
    bool armed = false;
    uint32_t jitter_index = 0;  // 0 .. phase_count - 1
    uint32_t phase_count = kPhasesAtRatio1;
    float jitter[2] = {};
    double ratio = 1.0;  // output size over render size: 1 until the scene is drawn smaller

    // The main view (2x samples, before the anchor).
    Scale scale = Scale::None();  // of the first main view draw's targets
    std::array<uint32_t, kKinds> main_draws{};
    std::array<uint32_t, kKinds> jittered{};
    std::array<uint32_t, kGateReasons> gate_failed{};  // by bit, in GateBit order
    uint32_t gate_rejected = 0;   // main view draws that were not world draws
    uint32_t forward_z_draws = 0;  // world draws whose depth range is not mirrored (census 3.6)
    uint32_t view4x_draws = 0;    // draws into the main view's 4x identity
    uint32_t late_draws = 0;      // main view draws after the anchor or the composite marker
    uint64_t stencil_ids = 0;     // bit n: a main view draw wrote the stencil ID n (value >> 2)
    bool composite_seen = false;

    // The anchor.
    uint32_t anchors = 0;         // anchor draws seen (a second one is ignored)
    bool anchor_seen = false;
    bool anchor_ok = false;       // accepted: `camera` and the fields below are valid
    AnchorStatus anchor_status = AnchorStatus::kNotSeen;
    temporal::Camera camera;
    uint32_t reset = 0;           // ResetBit: this frame starts the history again
    temporal::Cut cut;            // the cut test against the frame before (when there was one)
    float frame_time_ms = 1000.0f / 60.0f;  // between this anchor and the one before, 1 to 100
    uint32_t guest_size[2] = {};   // c62: the frame in guest pixels
    uint32_t render_size[2] = {};  // the main view in host pixels (guest size at the scene's scale)
    AnchorTextures anchor_textures;
  };

  // True while mcla_gpu_temporal was on at the start of the frame being recorded: the Scene's hooks
  // do nothing otherwise.
  bool active() const { return enabled_; }

  // The passes of step 1b (temporal_mv.h) need the device; nothing is made until the first anchor.
  void Initialize(Device& device, FrameRing& frames, BindlessHeap& heap, HostPasses& passes,
                  PassTiming& pass_timing, plume::RenderFormat target_format);
  // GPU idle, before HostPasses and the BindlessHeap shut down.
  void Shutdown();

  // The Renderer opened the frame's first list (frame slot `slot`). Reads the switch.
  void BeginFrame(uint32_t slot);
  // A prepared draw of the frame, after FillTextureTables and before UploadConstants: finds the
  // main view's draws and moves them (`shared` is the draw's own copy), finds the anchor and reads
  // the camera. True when this draw is the anchor, it was taken and its passes are due: the Scene
  // then ends its render pass, calls RecordAnchorPasses and forgets its list state.
  bool OnDraw(const DrawFacts& facts, SharedConstants& shared);
  // The anchor's passes (depth, motion vectors, the checks' copies) into `list`, outside any render
  // pass. False when nothing was recorded.
  bool RecordAnchorPasses(plume::RenderCommandList* list);
  // Inside the back buffer's render pass after the present's draw: the debug view
  // (mcla_gpu_temporal_view) of the last frame, when it had vectors.
  void DrawDebug(plume::RenderCommandList* list, uint32_t width, uint32_t height);
  // The composite marker: the UI starts.
  void OnComposite();
  // The guest's present: the frame ends, the state machine moves on, the log.
  void EndFrame();
  // The renderer shuts down: the last summary.
  void Finish();

  // The frame being recorded (valid fields as documented above), or the last one.
  const FrameState& frame() const { return frame_; }
  // The camera of the frame before when its anchor was accepted and this frame follows it
  // directly (null: the history starts again).
  const temporal::Camera* previous_camera() const { return ran_last_ ? &last_camera_ : nullptr; }
  // The depth and motion vector images kept for the last frame (temporal_mv.h Kept: sizes, formats).
  const TemporalMotion& motion() const { return motion_; }

 private:
  using Clock = std::chrono::steady_clock;

  // What the frames are, for the log.
  enum class Mode : uint8_t { kNoScene, kView4x, kNoAnchor, kRunning, kCount };

  struct Window {
    uint64_t frames = 0;
    uint64_t armed = 0;
    uint64_t modes[size_t(Mode::kCount)] = {};
    uint64_t anchors = 0;
    uint64_t second_anchors = 0;
    uint64_t anchor_status[size_t(AnchorStatus::kCount)] = {};
    uint64_t main_draws[kKinds] = {};
    uint64_t jittered[kKinds] = {};
    uint64_t gate_failed[kGateReasons] = {};
    uint64_t gate_rejected = 0;
    uint64_t forward_z_draws = 0;
    uint64_t view4x_draws = 0;
    uint64_t late_draws = 0;
    uint64_t resets = 0;
    uint64_t reset_reasons[kResetReasons] = {};
    uint64_t cut_distance = 0;  // camera cuts by clause (a cut may have several)
    uint64_t cut_turn = 0;
    uint64_t cut_fov = 0;
    uint64_t history_checks = 0;  // c69 against the previous frame's own view-projection
    uint64_t history_flip = 0;
    uint64_t history_noflip = 0;
    uint64_t pixel_to_world_agrees = 0;
    uint64_t vehicle_matrices = 0;  // usable previous matrices, summed over the accepted frames
    uint64_t rigid_checked = 0;     // vehicle slots measured as c(69+4k) * inverse(c69)
    uint64_t rigid_ok = 0;          // ... of them a rigid transform within 1e-3
    uint64_t rigid_moving = 0;      // ... of them that moved more than 1e-3 units
    uint64_t mv_missed = 0;         // anchors taken whose passes could not be recorded
    uint64_t misses = 0;            // jittered main view frames with no accepted anchor
    // The last accepted camera, for the line.
    bool have_camera = false;
    temporal::Camera camera;
    uint32_t render_size[2] = {};
  };

  void SetEnabled(bool enabled);
  void OnMainView(const DrawFacts& facts, const temporal::MainMatch& match, SharedConstants& shared);
  bool OnAnchor(const DrawFacts& facts, const SharedConstants& shared);
  // The vehicle slots as rigid transforms (temporal_camera.h Rigid): the counts and the first lines.
  void NoteRigid(const temporal::Camera& camera, uint64_t frame);
  static uint32_t EvaluateGate(const XenosState& state, const DrawRecord& record,
                               const ViewportInfo& viewport, Scale scale);
  void NoteStencil(const XenosState& state);
  // A main view draw that is not a world draw: the first few distinct ones are logged.
  void NoteStill(const DrawFacts& facts, uint32_t failed);
  // Structure of the anchor: its fetches and its target. Fills `*width`, `*height` (the fetches'
  // size) and `*why` on a refusal.
  AnchorStatus CheckAnchor(const DrawFacts& facts, uint32_t* width, uint32_t* height,
                           std::string* why) const;
  static Mode ModeOf(const FrameState& frame);
  void NoteMode(Mode mode, const FrameState& frame);
  void Report(const char* why);
  void ResetRun();

  bool enabled_ = false;
  bool frame_open_ = false;  // BeginFrame ran and no present has ended the frame yet
  uint64_t frame_number_ = 0;
  FrameState frame_;

  // The run: what the frames before this one left.
  bool ran_last_ = false;    // the previous frame's anchor was accepted
  temporal::Camera last_camera_;
  // The camera's last step (position now minus position before), what the cut test predicts the
  // next one from; unknown at the first step of a run.
  double last_step_[3] = {};
  bool last_step_known_ = false;
  float last_jitter_[2] = {};  // the jitter of the frame before (zero when it was not armed)
  Scale last_scale_ = Scale::None();
  Clock::time_point last_anchor_time_{};
  uint32_t jitter_index_ = 0;  // advances in frames whose anchor was accepted
  uint32_t misses_ = 0;
  bool disarmed_ = false;

  // Step 1b: the passes and what they keep (temporal_mv.h), and what the anchor told them.
  TemporalMotion motion_;
  TemporalMotion::Frame pending_;
  bool pending_ready_ = false;

  // The log.
  Window window_;
  Clock::time_point window_start_{};
  Mode shown_mode_ = Mode::kCount;    // kCount: none shown yet
  Mode pending_mode_ = Mode::kCount;
  uint32_t pending_frames_ = 0;
  std::array<bool, size_t(AnchorStatus::kCount)> status_logged_{};
  uint32_t reset_lines_ = 0;
  uint32_t rigid_lines_ = 0;
  std::unordered_set<uint64_t> still_logged_;  // main view draws kept still, by shaders and reason
  double logged_lens_[3] = {};                 // field of view (rad), near, far of the last line
  uint32_t lens_lines_ = 0;
};

}  // namespace mcla::gpu
