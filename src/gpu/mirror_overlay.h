#pragma once
// ===========================================================================
// MCLA GPU renderer -- the side mirrors' glass overlay (perf/MIRROR-VIEW.md, package MV4)
// ===========================================================================
// The shared rear view (mirror_view.h) holds a small picture of the street behind the player. The car's two side mirror glasses are
// drawn again at the end of the scene, into the main colour, with a pixel shader that reflects the eye's ray off the glass plane and reads that picture where the reflected ray goes: a planar mirror lookup
// (mirror_view_rules.h UvForMirrorPixel, exact for what is the assumed distance beyond the glass: mcla_gpu_mirror_view_t0, 20 m; the error for nearer things is the design's section 2.3 table).
//
//   WHEN     Scene::OnResolve, at the first copy of the main colour after the phase's end marker, right AFTER the HUD stamp (frame_order.h kMirrorGlass), from MirrorView::OverlayArmed: a whole picture, drawn this
//            frame or held (at most mirror_view::kMaxHeldMs old), in the play view. The decision is mirror_overlay_rules.h Decide: the setting (mirror_setting.h Current().overlay: the SIDE MIRRORS row's view, or the debug switch mcla_gpu_mirror_overlay), the picture, the glass and the projection.
//   WHAT     for each needed glass, one draw of the glass patch's own triangles (the host geometry its game draw read: CarFrame's Draw, as the car shadow map redraws the car), vertex stage
//            hlsl/mirror_overlay_vs.hlsl, pixel stage hlsl/mirror_overlay_ps.hlsl (the arithmetic is hlsl/mirror_overlay.hlsli), into the main colour with the main depth bound: the glass draw's own depth test
//            repeated with the equal case allowed (GREATER_EQUAL in the game's reverse-Z scene), the depth written never, the overlay pulled a few millimetres towards the eye along the view ray so that it lands on
//            the glass the game drew; blend SRC_ALPHA / INV_SRC_ALPHA on the colour channels (the alpha is the edge fade: the game's own glass shows where the lookup runs out of picture).
//   POSE     The lookup needs the camera the picture was drawn from (a held picture's camera has moved on) and the field it was built with. NoteViewDrawn, from the end marker of a phase that drew a fresh picture,
//            keeps the frame's camera (CarFrame's Frame::view) and the field of the phase's own camera (the begin marker's tag, MirrorView::PictureField: MV-J2; unknown = nothing is drawn).
//   PIPELINES one per vertex format (position kind, topology, stride: car_shadow_map_rules.h VariantKey), main colour sample count (1, 2, 4, 8) and depth test (mirror_overlay_rules.h Variant), listed with the
//            host precache at the start where the setting is on (the known strides, 1, 2 and 4 samples, the game's reverse-Z test) and made in the background otherwise or for a variant nobody listed (AddLive): a frame
//            whose draws need a pipeline that is not made yet draws no overlay, it never waits for a compile.
//   LOG      "[mirror overlay]": the first frame drawn (the pose, the field, the glass, the picture), a window every 1800 frames that armed (drawn, and why not by reason).
//
// CHECK: nothing of this has run on a device (no build in the package): the first run says whether the overlay lands on the glass (the depth test and the bias against the game's own glass draw: z fighting
// would show as a flicker of the old glass), whether the picture's orientation is right (the street behind, left and right the right way round), what the lookup does for a car 5 m and 10 m behind
// (the design's table predicts 6 and 2 degrees for the left mirror), and whether the end-of-scene copy still has the main depth with the glass in it.
//
// Threading: render thread only. Initialize and Shutdown on the lifecycle thread. The pipelines are made on the host builder thread.
// ===========================================================================

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <plume_render_interface.h>

#include "cabin_mask.h"
#include "car_frame.h"
#include "edram.h"
#include "host_passes.h"
#include "mirror_overlay_rules.h"

namespace mcla::gpu {

class BindlessHeap;
class Device;
class FrameRing;
class MirrorGlass;
class MirrorView;
class UploadRing;
class PassTiming;

class MirrorOverlay {
 public:
  MirrorOverlay();
  ~MirrorOverlay();
  MirrorOverlay(const MirrorOverlay&) = delete;
  MirrorOverlay& operator=(const MirrorOverlay&) = delete;

  // Makes the layout and the shaders and, where the setting is on at the start, lists the pipelines with the host precache (before the Renderer starts it). Never fails the renderer: without them there is
  // no overlay.
  void Initialize(Device& device, FrameRing& frames, BindlessHeap& heap, HostPasses& passes, EdramTargets& edram, PassTiming& pass_timing);
  // GPU idle and the FrameRing drained, before HostPasses shuts down.
  void Shutdown();

  // Scene::BeginFrame: reads the overlay setting (mirror_setting.h Current().overlay) and the assumed distance, and writes the census line.
  void BeginFrame();
  bool wanted() const { return on_; }

  // The end marker of a phase that drew a FRESH picture (MirrorView::EndPhase true): the camera the picture was drawn from, with the field of THAT phase's camera (the begin marker's tag, MV-J2:
  // MirrorView::PictureField; `field_known` false = no camera had been built when the phase began: the overlay then draws nothing). `frame` is CarFrame's frame of this guest frame.
  void NoteViewDrawn(const car_frame::Frame& frame, uint64_t guest_frame, bool field_known, float field_h, float field_v, uint32_t cabin_offset);
  void OnPresent();

  struct Inputs {
    const car_frame::Frame* frame = nullptr;  // this frame's camera and the player's draws
    MirrorGlass* glass = nullptr;             // the finder's pair
    const MirrorView* view = nullptr;         // the picture
    bool play_view = true;
    UploadRing* upload = nullptr;
    uint32_t slot = 0;
    float jitter[2] = {0.0f, 0.0f};           // the frame's sub-pixel jitter in render pixels, +y down (TemporalSystem::FrameJitter), as the draws were moved by
  };
  // At the first copy of the main colour after the phase's end marker, after the stamp. `colour` and `depth` are the main view's targets as the copy found them (null: they were not there). True when the glass
  // was drawn: the caller's list state is gone.
  bool Record(plume::RenderCommandList* list, const Inputs& in, EdramTargets::Target* colour, EdramTargets::Target* depth);

 private:
  CabinMask cabin_mask_;
  bool cabin_drawn_ = false;
  bool cabin_pose_ = false;
  bool cabin_logged_ = false;
  bool MakeShaders();
  void ListBase(bool at_start, const std::string& note);
  static uint64_t PrecacheKey(uint32_t variant);
  std::string VariantName(uint32_t variant) const;
  std::unique_ptr<plume::RenderPipeline> MakePipeline(uint32_t variant);
  // The pipeline of `variant` if it is made; else null with *pending set (it is listed or being made), or null (it failed).
  plume::RenderPipeline* Pipeline(uint32_t variant, bool* pending);
  void FlushListing();
  void Count(mirror_overlay::Why why);
  void Census(bool force);
  void LogFirst(const Inputs& in, uint32_t drawn);

  Device* device_ = nullptr;
  FrameRing* frames_ = nullptr;
  BindlessHeap* heap_ = nullptr;
  HostPasses* passes_ = nullptr;
  EdramTargets* edram_ = nullptr;
  PassTiming* pass_timing_ = nullptr;
  std::unique_ptr<plume::RenderPipelineLayout> layout_;  // the host pass layout's sets and a 128 byte push block for both stages, with an input layout
  std::unique_ptr<plume::RenderShader> vertex_shader_;
  std::unique_ptr<plume::RenderShader> pixel_shader_;
  bool ready_ = false;
  bool shaders_failed_ = false;
  bool base_listed_ = false;
  std::unordered_map<uint32_t, std::unique_ptr<plume::RenderPipeline>> pipelines_;
  std::unordered_set<uint32_t> listed_;
  std::unordered_set<uint32_t> failed_;
  std::vector<uint32_t> to_list_;

  // The settings of the frame.
  bool on_ = false;
  double distance_ = mirror_view::kAssumedDistance;
  uint64_t frame_ = 0;

  // The camera the picture was drawn from.
  mirror_overlay::ViewPose pose_;
  bool field_known_ = false;
  float field_h_ = 0.0f;
  float field_v_ = 0.0f;

  // The census.
  uint64_t why_[size_t(mirror_overlay::Why::kCount)] = {};
  uint64_t drawn_glasses_ = 0;
  uint64_t census_frame_ = 0;
  uint64_t census_total_ = 0;
  bool first_logged_ = false;
  bool pending_logged_ = false;
  bool projection_logged_ = false;
};

}  // namespace mcla::gpu
