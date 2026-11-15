#pragma once
// ===========================================================================
// MCLA GPU renderer -- car self-reflections (package G8b, a test prototype: debug-only cvars, off by default)
// ===========================================================================
// The player's car paint (xCarPaintCustomizable::PS_CarPaint) reflects the world through two environment reads and nothing of
// the car itself: no wing in the trunk lid, no mirror in the door (feature-parity/G8A-CAR-SELFREFL.md). This is the host
// half of the paint variant of shaderpack/variants/car_selfrefl.h: the variant marches a short ray along the reflected
// view vector through an image of the PREVIOUS frame (the car is drawn before the world, so it cannot see the current
// frame's depth), and this keeps that image and the numbers that take a point of this frame into it.
//
//   mcla_gpu_car_selfrefl_test   0 off (the default: nothing is selected, the game's paint draws, bit for bit)
//                                1 specular occlusion: the reflection is darkened where the ray passes just behind a
//                                  part of the car
//                                2 the same, and the car's own colour of the last frame is reflected there
//   mcla_gpu_car_selfrefl_view   a debug view in place of the paint's colour: 1 the hit weight, 2 the colour the ray
//                                found, 3 the baked vertex AO, 4 the state of the trace (colours in the variant's text)
//   mcla_gpu_car_selfrefl_length / _thickness / _strength / _steps / _gain    the tuning of the ray (car_selfrefl_rules.h)
//
// THE HISTORY: at the game's mid-scene depth copy (into 078B0000: the ambient occlusion's trigger, ao.h), after the AO's
// own apply, one pass reads the main view's colour and depth and stencil planes and writes an RGBA16F image at the
// picture's size: rgb the scene colour (the mean of the host samples, in the target's units: the shaders' output times
// 2^exp_bias), a the view-space distance of the pixels whose stencil ID says a vehicle (1 to 23; sample 0), 0 elsewhere
// (hlsl/car_history_ps.hlsl). That image is what the NEXT frame's paint draws read: it is the only copy of the frame's
// colour there is at that time (the game's own end-of-scene colour copy, 07FE0, is not alive at the car's first draw: the
// census of G8A found it in 0 of 600 frames, its depth copy in 600 of 600). The image exists only while the test cvar is
// on (it is given back after 2 s without it).
//
// THE NUMBERS: the history's own camera (the view-projection of the frame it was made in, read from the game's motion blur
// draw, the anchor, which comes after the trigger: ExtractCamera) and the vehicle's own motion (vehicle_motion.h: the game's
// c(69 + 4 ID) times the inverse of c69, taken at the last anchor and repeated for this frame as the ambient occlusion
// does) make the one matrix a draw of the paint is given (CarSelfReflConstants::m): this frame's world position to the
// history frame's clip space. The variant also tests that the shaded point finds itself in the history at the distance it
// has now; where it does not (a cut, a wrong guess of the motion, an occluder) the effect fades out.
//
// The depth is linearised with the previous frame's lens (the trigger comes before this frame's anchor, as the occlusion's
// does); a lens that changed by more than 5% between the two frames (the pause view's field of view, a sweep) leaves no
// history for the next frame. The play view only: the pause menu's and the map's world (4x samples) has none.
//
// COST (estimates, nothing here was run): the history pass is one full-screen pass reading the 2x multisampled colour, depth
// and stencil planes (0.1 to 0.2 ms at 4K on an RTX 5080), the ray is 12 steps of one RGBA16F load each on the car's pixels
// (about 7% of the picture in the K shots, 15% close behind it: 0.05 to 0.15 ms), the image 8 bytes a pixel (66 MB at 3x, 16 MB
// at 1080p). All of it only while the test cvar is on.
//
// CHECK: nothing here was built or run; the pictures and the log say:
//   1. units: the history colour times 2^-exp_bias (the main target's bias is 4, gInvColorExpBias.x = 1/16) is taken to be the
//      paint's own units, the ones its environment reads are in. If the reflected car is too dark or bright against the
//      paint, mcla_gpu_car_selfrefl_gain moves it and view 2 shows the colour alone.
//   2. the paint's interpolants iTexCoord1, iTexCoord2 and iTexCoord6 are the world position, the world normal and the view
//      vector towards the camera (read from xCarPaintCustomizable__VS_CarPaint.hlsl, not seen at run time): view 4 is
//      all red if they are not, and view 3 shows the baked AO.
//   3. the game's c8-c11 as a world to clip matrix whose w is the distance AoLinearDepth gives (the rules test checks the
//      arithmetic on a projection of that shape, not on the game's).
//   4. the paint draws have no alpha test or alpha to coverage (the census of G8A: variant 0, a2m 0), else they keep the
//      game's shader; and the stencil ID of the draw (4 ID + {0, 1}) is the ID the history's stencil holds for the same car.
//   5. the 720-byte shared block as a root constant buffer (the shadow filter's 624 works), the history image's layout
//      (SHADER_READ after DrawInto) when the next frame's paint draws read it, and the main depth target's stencil plane view.
//
// Threading: render thread only; Initialize and Shutdown on the lifecycle thread. The pipelines are made ahead on the host
// builder thread (host_precache.h).
// ===========================================================================

#include <array>
#include <cstdint>
#include <memory>
#include <string>

#include <plume_render_interface.h>

#include "car_selfrefl_rules.h"
#include "constants.h"
#include "draw_state.h"
#include "edram.h"
#include "host_passes.h"
#include "records.h"
#include "resolve.h"
#include "temporal_camera.h"
#include "temporal_identities.h"
#include "vehicle_motion.h"

namespace mcla::gpu {

class PassTiming;

class CarSelfRefl {
 public:
  using Mode = car_selfrefl::Mode;

  // The destination of the game's mid-scene depth copy in the retail build (shafts.h, ao.h): another build gives another
  // address and then there is no history, never a wrong picture.
  static constexpr uint32_t kTriggerDestination = 0x078B0000;

  // True while mcla_gpu_car_selfrefl_test asks for the variant: Pipelines::ChoosePackedVariant asks it per draw, so a
  // change shows at the next frame. Reads the cvar only.
  static bool Selected();

  CarSelfRefl() = default;
  ~CarSelfRefl();
  CarSelfRefl(const CarSelfRefl&) = delete;
  CarSelfRefl& operator=(const CarSelfRefl&) = delete;

  // Makes the shaders and lists the pipelines with the host precache (before the Renderer starts it). Never fails the
  // renderer: without them the variant draws as the game's paint (mode 0 constants).
  void Initialize(HostPasses& passes, EdramTargets& edram, PassTiming& pass_timing);
  // GPU idle and the FrameRing drained, before HostPasses shuts down.
  void Shutdown();

  // The Scene opened a frame's first list: reads the cvars.
  void BeginFrame();
  // True while the draw hooks have anything to do.
  bool wants_draws() const { return mode_ != Mode::kOff; }

  // A prepared draw of the main view (temporal_identities.h MatchMainView): `stencil_ids` the IDs it writes into the
  // stencil (vehicle_motion.h StencilIdsWritten), for the vehicles' motion.
  void OnMainDraw(const temporal::MainMatch& match, uint32_t stencil_ids);
  // The game's motion blur draw: this frame's camera and the vehicles' motion; the history made this frame is theirs.
  void OnAnchor(const ConstantBanks::Banks& banks);
  // A resolve, after ResolveCache::Execute (and after the ambient occlusion's apply): the mid-scene depth copy is where the
  // history is made. True when passes were recorded (the caller's list state is gone).
  bool OnResolve(const ResolveRecord& record, const ResolveCache::Summary& summary, plume::RenderCommandList* list);
  // The scene is composed: the UI starts.
  void OnComposite() { composite_seen_ = true; }
  // A guest present: the frame boundary.
  void OnPresent();

  // The 112 bytes a draw of the paint variant reads (Scene::UploadCarSelfReflConstants): the game's paint (mode 0) when
  // there is no history for this frame. `upscaler_running`: a temporal upscaler works on the frame (the noise then moves
  // every frame, which its history averages out).
  void FillConstants(const XenosState& state, bool upscaler_running, car_selfrefl::CarSelfReflConstants* out);

  // The video memory the image holds now.
  uint64_t HeldBytes() const;

 private:
  // The push block of the pass (hlsl/car_history_ps.hlsl CarHistoryPush), 64 bytes: the host pass layout's push range.
  struct Push {
    uint32_t slots[4];   // depth plane, colour plane, stencil plane (0xFFFFFFFF: none), host samples
    uint32_t slots2[4];  // the host sample that is the guest's sample 0
    float sizes[4];      // the picture in host pixels
    float lin[4];        // the projection's rows P22, P23, P32, P33 (ao_common.hlsli AoLinearDepth)
  };
  static_assert(sizeof(Push) == 64 && sizeof(Push) == HostPasses::kPushConstantBytes);

  enum Pass : uint32_t { kPassPlain, kPassMs, kPassCount };

  bool IsTrigger(const ResolveRecord& record, const ResolveCache::Summary& summary) const;
  EdramTargets::Target* ColourTarget(const EdramKey& key, plume::RenderCommandList* list);
  bool RecordHistory(const ResolveCache::Summary& summary, plume::RenderCommandList* list);
  plume::RenderPipeline* Pipeline(uint32_t pass);
  std::unique_ptr<plume::RenderPipeline> MakePipeline(uint32_t pass);
  void ListPrecache();
  void ReleaseImage();
  void Census();

  // Settings, read at the frame's start.
  Mode mode_ = Mode::kOff;
  car_selfrefl::Tuning tuning_;
  uint32_t off_frames_ = 0;

  // The frame.
  uint64_t frame_ = 0;
  bool anchor_seen_ = false;
  bool composite_seen_ = false;
  bool trigger_seen_ = false;
  bool play_view_ = false;
  bool pause_view_ = false;
  uint32_t stencil_ids_ = 0;      // the vehicle IDs the main view's draws wrote before the anchor
  bool history_pending_ = false;  // the pass was recorded this frame; the anchor says whose camera it goes with

  // The camera of the last anchor (the previous frame's, for this frame's trigger) and the lens the history was linearised with.
  temporal::Camera previous_;
  bool have_previous_ = false;
  uint64_t previous_frame_ = 0;
  temporal::Camera lens_used_;

  // The history: made at the trigger of frame `history_frame_`, valid for the draws of the frame after it.
  bool history_valid_ = false;
  uint64_t history_frame_ = 0;
  temporal::Camera history_camera_;
  // The vehicles' own motion of the history frame's anchor, repeated for the next frame.
  temporal::VehicleMotion motion_;
  uint64_t motion_frame_ = 0;

  // The GPU side.
  HostPasses* passes_ = nullptr;
  EdramTargets* edram_ = nullptr;
  PassTiming* pass_timing_ = nullptr;
  bool ready_ = false;  // the shaders exist
  std::array<std::unique_ptr<plume::RenderShader>, kPassCount> shaders_;
  std::array<std::unique_ptr<plume::RenderPipeline>, kPassCount> pipelines_;
  std::array<bool, kPassCount> pipeline_failed_{};
  HostImage history_;  // R16G16B16A16_FLOAT, the picture's size

  // What the log has said once, and the totals.
  uint32_t last_mode_logged_ = ~0u;
  bool no_stencil_logged_ = false;
  bool failed_logged_ = false;
  bool bias_logged_ = false;
  uint32_t image_for_[2] = {};
  uint64_t passes_run_ = 0;
  uint64_t triggers_ = 0;
  uint64_t no_camera_ = 0;
  uint64_t lens_changed_ = 0;
  uint64_t paint_draws_ = 0;
  uint64_t paint_with_history_ = 0;
  uint64_t paint_no_id_ = 0;
  uint64_t paint_motion_ = 0;
};

}  // namespace mcla::gpu
