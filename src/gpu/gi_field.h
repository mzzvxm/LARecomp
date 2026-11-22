#pragma once
// ===========================================================================
// MCLA GPU renderer -- the probe field's owner (CITY-RT M3, package G1)
// ===========================================================================
// perf/CITY-RT-M3.md is the design; gi_frame.h (package G0) is the frozen interface; gi_field_rules.h is what this owner decides with no device (memory, the order of loss, the windows'
// toroidal moves, the dirty sets, cuts, the fade, the state word and the weight rule) and gi_field_rules_test.cpp checks it. This file is the Plume side:
//
//   MAKES          the three textures of the field in the layout gi_frame.h fixes, one of each for every cascade (the irradiance atlas RGBA16F, the moments atlas RG16F, the weight map
//                  R8_UNORM, all with storage usage and a bindless slot, so that the update writes them as storage images and the readers sample them), and the state buffer (one uint a
//                  probe: gi_field_rules.h THE STATE WORD)
//   KEEPS          the cascades' windows (FieldState: toroidal addressing; a camera that moves only exposes the coordinates that entered a window, and those are cleared on the GPU
//                  and marked dirty), the fade, the memory (counted against the OS budget before anything is made; when it is short the field steps down: per-pixel images, the fourth
//                  cascade, the third: gi_field_rules.h THE ORDER OF LOSS; it never fails the renderer)
//   PUBLISHES      the GiFrame (gi_frame.h) every reader takes and the 20 rows (GpuGiFrame) the shaders read, once the weights are made
//   DRAWS          the debug views (the probes as spheres over the picture: by validity, with the irradiance they hold, with their sky visibility)
//
// THE FRAME. The integration (G4) calls these in this order, all on the render thread, `frame_slot` the FrameRing's slot being recorded:
//   1. BeginFrame(inputs)        the frame's start, CPU only: the setting's layout, the fit, the textures (made here, between frames), the windows' move, the dirty sets, the fade. After it
//                                running() says whether a field exists this frame, and move() what the move exposed.
//   2. RecordMaintenance(list)   outside any render pass, at the trigger (the mid-scene depth copy where the AO runs), BEFORE the update: uploads the frame's rows, moves the textures to
//                                the storage layout and clears the exposed probes (hlsl/gi_field_reset_cs.hlsl). Afterwards update_ready() is true.
//   3.                           THE PROBE UPDATE (G2): traces and blends. It reads update_frame() and the cascades' dirty sets (scheduler()), writes the three textures' probe tiles (border
//                                included) and the probes' state words (state_buffer(): updates, inside, new), and calls scheduler().TakeDirty / NoteTraced for what it schedules. The
//                                textures and the state buffer are in the GENERAL layout (UAV) and have had a barrier; G2 puts a UAV barrier after its writes (RecordFinish adds the
//                                one that matters before the weights pass, so a barrier after the last pass is also enough).
//   4. RecordFinish(list)        the weights pass (hlsl/gi_field_weights_cs.hlsl: the one writer of the weight map), the textures to SHADER_READ, and the frame is published: frame() is
//                                valid until the frame's end.
//   5. RecordDebugView(...)      optional, over the picture, after the effects of the trigger (as gbuffer.h DrawView).
// A frame with no field (the setting is off, the world is the pause menu's, the safe range is 0) leaves 2 to 4 out: update_ready() and published are false, frame().valid is 0.
//
// THE MEMORY. The field's own resources are HeldBytes() (the integration lists them with VideoMemoryBudget::AddHolder: the OS counts them in the use it reports and the caches' trim cannot
// give them back). Before it makes anything the owner asks the room test (vram_room.h HasRoom: the use plus what the cell would add stays under the share the caches start releasing at)
// for the cell's TOTAL (what G2 and G3 / G5 are estimated to add is counted: gi_field_rules.h Memory), and steps down by the order of loss until a step fits; a step that did not fit
// waits (vram_room::RefusalBook) before it is asked again, and a smaller field is made bigger when the wait is over. A creation that fails is a refusal of its step too.
//
// WHO WRITES WHAT: the reset pass writes a cleared probe's tile, weight and state; G2 writes the probe tiles and the state words of the probes it traces; the weights pass writes the weight
// map from the state words. The tile's 1 texel border is the update's (gi_frame.h OctBorderSource: the reset clears it to 0).
//
// THREADING: render thread only; Initialize and Shutdown on the lifecycle thread. The compute pipelines are made on the host builder thread (host_precache.h) when the setting at the start
// uses them; a field switched on later makes them in the background and starts from the frame they are done.
//
// CHECK (not run on a device here): the typed storage image stores of RGBA16F, RG16F and R8_UNORM on both APIs (M3's C7; on Vulkan the formats are given to SPIR-V explicitly, which
// needs shaderStorageImageExtendedFormats), and the 5120 x 960 and 5120 x 1280 atlases; the buffer barriers between the passes; the view pass's layout (the bindless sets and one set of
// its own, with a push block of 96 bytes).
// ===========================================================================

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <plume_render_interface.h>

#include "frame.h"
#include "gi_field_rules.h"
#include "gi_frame.h"
#include "host_passes.h"
#include "vram_room.h"

namespace mcla::gpu {

class BindlessHeap;
class Device;
class PassTiming;

class GiField {
 public:
  static constexpr uint32_t kNoSlot = gi::kNoSlot;

  GiField() = default;
  ~GiField();
  GiField(const GiField&) = delete;
  GiField& operator=(const GiField&) = delete;

  // Makes the layouts and the shaders and, when `listed_at_start` (the setting is not off at the start), lists the pipelines with the host precache (before the Renderer starts it). Never
  // fails the renderer: without them there is no field (logged).
  void Initialize(Device& device, FrameRing& frames, BindlessHeap& heap, HostPasses& passes, PassTiming& pass_timing, bool listed_at_start);
  // GPU idle and the FrameRing drained, before HostPasses shuts down.
  void Shutdown();
  bool available() const { return ready_; }

  // What the frame's start needs from the rest of the renderer (the integration fills it; nothing here reads a setting).
  struct Inputs {
    gi::Rung rung = gi::Rung::kNone;     // how well the card traces rays (rt_tier.h; the glue is a cast)
    gi::Level level = gi::Level::kOff;   // mcla_gpu_rt_gi resolved (`auto` is the class table's)
    bool world_ok = false;               // the frame is the play view's: false for the pause menu's world, the garage, the map, a cutscene (the field idles and fades out)
    double camera[3] = {0, 0, 0};        // the play camera's position, world metres
    double time_s = 0.0;                 // the scene clock (the fade's and the retry's seconds)
    float safe_range_m = 0.0f;           // M1's SafeRange from the camera: 0 = no structures stand (no field)
    float strength = 1.0f;               // the BOUNCE LIGHT level's strength (ssgi_level.h)
    float bounce_weight = gi::kBounceWeightDefault;  // the probes' own feedback (G2's: published for the readers)
    float sun_rgb[3] = {0, 0, 0};        // the frame's colours the baseline needs (the car hit's frame rows: c39 with its exposure, c26 up, c57 down)
    float sun_sin_elevation = 0.0f;
    float ambient_up_rgb[3] = {0, 0, 0};
    float ambient_down_rgb[3] = {0, 0, 0};
    gi_field::CutKind cut = gi_field::CutKind::kNone;  // a cut the integration knows of: kTeleport (the garage exit, a race start, the Director's warp), kRelight (the Director's hour)
    uint64_t frame = 0;                  // the scene's frame serial
  };
  void BeginFrame(const Inputs& inputs);

  // True while a field exists this frame: textures made, the fit on, and the world is the play view's (or the field is fading out).
  bool running() const { return active_; }
  // The field may be updated (traced) this frame: the world is the play view's and the safe range is not 0.
  bool update_allowed() const { return usable_; }
  // What the move exposed this frame (BeginFrame), and the budget scale of the warm-up (gi_field_rules.h FieldState::BudgetScale).
  const gi_field::MoveReport& move() const { return move_; }
  float budget_scale() const { return state_.BudgetScale(); }

  // Records the maintenance (see THE FRAME) on `list`, outside any render pass. False when there is nothing to do (no field, or its pipeline is not made yet).
  bool RecordMaintenance(plume::RenderCommandList* list, uint32_t frame_slot);
  bool update_ready() const { return update_ready_; }
  // Records the weights pass and publishes the frame. False when RecordMaintenance did not run.
  bool RecordFinish(plume::RenderCommandList* list, uint32_t frame_slot);

  // What a reader takes: valid from RecordFinish to the frame's end (BeginFrame empties it: valid 0). The rows are the shaders' (gi_frame.h MakeGpuFrame of it).
  const gi::GiFrame& frame() const { return published_frame_; }
  const gi::GpuGiFrame& gpu_frame() const { return gpu_frame_; }
  // What the update (G2) takes between RecordMaintenance and RecordFinish: the same table with the products marked present.
  const gi::GiFrame& update_frame() const { return update_frame_; }
  // The CPU's dirty sets and the cascades' windows: the scheduler's (TakeDirty, NoteTraced, MarkWorldBox, RequestCut).
  gi_field::FieldState& scheduler() { return state_; }
  const gi_field::FieldState& scheduler() const { return state_; }
  const gi_field::Fit& fit() const { return fit_; }
  // Changes whenever the textures are made again (a new layout, a size, the memory): what was cached from them is dropped.
  uint32_t generation() const { return generation_; }

  // The textures and the buffer G2 writes. Null/empty while there is no field. The images are HostImages (the texture, its view, its bindless slot, its layout as this owner left it).
  const HostImage& irradiance() const { return irradiance_; }
  const HostImage& moments() const { return moments_; }
  const HostImage& weight() const { return weight_; }
  plume::RenderBuffer* state_buffer() const { return state_buffer_.get(); }
  uint64_t state_bytes() const { return state_bytes_; }

  // --- the debug views (mcla_gpu_rt_gi_view; gi_field_rules.h kViewProbes, kViewIrradianceSpheres, kViewSkySpheres) ---
  struct ViewTarget {
    plume::RenderFramebuffer* framebuffer = nullptr;  // the picture's colour target, in the colour write layout, with this framebuffer bound for the pass
    uint32_t width = 0;
    uint32_t height = 0;
    plume::RenderFormat format = plume::RenderFormat::UNKNOWN;
    uint32_t samples = 1;
  };
  struct ViewCamera {
    float position[3] = {0, 0, 0};
    float right[3] = {1, 0, 0};
    float up[3] = {0, 1, 0};
    float forward[3] = {0, 0, 1};
    float tan_x = 1.0f;  // tangent of the half field of view
    float tan_y = 1.0f;
  };
  struct ViewInputs {
    uint32_t view = 0;                    // gi_field_rules.h IsOwnView
    uint32_t cascade = 0;                 // the cascade whose probes are drawn
    ViewCamera camera;
    const HostImage* depth = nullptr;     // the occlusion's level 0 linear depth (R32_FLOAT), left readable: hides the spheres behind the geometry; null: not hidden
    float gain = 1.0f;                    // the irradiance spheres' gain
    float max_distance_m = gi_field::kViewMaxDistanceM;
  };
  // Draws the spheres over `target` (blended; a pixel with no sphere is left as it is). Records on `list` outside any render pass, the target's framebuffer bound by this call and unbound
  // after. False when nothing was drawn (not published, not an own view, the pipeline could not be made).
  bool RecordDebugView(plume::RenderCommandList* list, uint32_t frame_slot, const ViewTarget& target, const ViewInputs& inputs);

  // The video memory the field's own resources hold now (what vram_budget.h's AddHolder asks), and the census text.
  uint64_t HeldBytes() const;
  std::string CensusText() const;

 private:
  // The pipelines: the reset and the weights compute passes (the view's pipelines are per colour target format and sample count).
  enum Variant : uint32_t { kVReset = 0, kVWeights = 1, kVariantCount = 2 };
  static const char* VariantName(uint32_t variant);
  // The push block of the view pass (hlsl/gi_field_view_ps.hlsl GiFieldViewPush), 96 bytes.
  struct ViewPush {
    float pos_tan_x[4];
    float right_tan_y[4];
    float up_gain[4];
    float forward_max[4];
    uint32_t info[4];
    uint32_t size[4];
  };
  static_assert(sizeof(ViewPush) == gi_field::kViewPushBytes, "the view's push block is 96 bytes (hlsl/gi_field_view_ps.hlsl)");
  struct ViewPipeline {
    plume::RenderFormat format = plume::RenderFormat::UNKNOWN;
    uint32_t samples = 1;
    std::unique_ptr<plume::RenderPipeline> pipeline;
  };
  // Per frame slot: the frame's rows and the reset list (upload buffers the host writes each frame) and the two descriptor sets that name them with the textures and the state buffer.
  struct Slot {
    std::unique_ptr<plume::RenderBuffer> rows;
    std::unique_ptr<plume::RenderBuffer> list;
    std::unique_ptr<plume::RenderDescriptorSet> field_set;  // the compute passes'
    std::unique_ptr<plume::RenderDescriptorSet> view_set;   // the view pass's
    uint32_t sets_generation = 0;                           // the generation the two sets were made for
  };

  bool MakeLayoutsAndShaders();
  std::unique_ptr<plume::RenderPipeline> MakePipeline(uint32_t variant);
  plume::RenderPipeline* Pipeline(uint32_t variant);
  bool PipelinesReady();
  plume::RenderPipeline* ViewPipelineFor(plume::RenderFormat format, uint32_t samples);
  bool EnsureSlot(uint32_t frame_slot);
  void RetireSets();

  // The fit and the textures.
  gi_field::Fit ChooseFitNow(gi::Layout wanted, bool* refused_something);
  bool MakeResources(const gi_field::Fit& fit, const double camera[3]);
  void ReleaseResources(const char* why);
  bool HasRoomFor(uint64_t total_bytes) const;
  uint64_t NowMs() const;

  void Log(const char* what);
  void LogInterval();

  // --- the GPU side ---
  Device* device_ = nullptr;
  FrameRing* frames_ = nullptr;
  BindlessHeap* heap_ = nullptr;
  HostPasses* passes_ = nullptr;
  PassTiming* pass_timing_ = nullptr;
  bool ready_ = false;
  bool listed_ = false;
  bool shaders_failed_ = false;
  std::array<bool, kVariantCount> pipeline_failed_ = {};
  plume::RenderDescriptorSetBuilder field_set_builder_;
  plume::RenderDescriptorSetBuilder view_set_builder_;
  uint32_t field_rows_index_ = 0, field_list_index_ = 0, field_irradiance_index_ = 0, field_moments_index_ = 0, field_weight_index_ = 0, field_state_index_ = 0;
  uint32_t view_rows_index_ = 0, view_state_index_ = 0;
  std::unique_ptr<plume::RenderPipelineLayout> compute_layout_;
  std::unique_ptr<plume::RenderPipelineLayout> view_layout_;
  std::unique_ptr<plume::RenderShader> reset_shader_;
  std::unique_ptr<plume::RenderShader> weights_shader_;
  std::unique_ptr<plume::RenderShader> view_shader_;
  std::array<std::unique_ptr<plume::RenderPipeline>, kVariantCount> pipelines_;
  std::vector<ViewPipeline> view_pipelines_;
  bool view_failed_ = false;
  std::array<Slot, kFramesInFlight> slots_;

  // The field's resources.
  HostImage irradiance_;
  HostImage moments_;
  HostImage weight_;
  std::unique_ptr<plume::RenderBuffer> state_buffer_;
  uint64_t state_bytes_ = 0;
  bool resources_made_ = false;
  uint32_t generation_ = 0;

  // --- the field ---
  gi_field::FieldState state_;
  gi_field::Fit fit_;
  gi_field::FadeState fade_;
  gi_field::MoveReport move_;
  gi::GiFrame update_frame_;
  gi::GiFrame published_frame_;
  gi::GpuGiFrame gpu_frame_;
  Inputs inputs_;
  bool active_ = false;
  bool usable_ = false;
  bool update_ready_ = false;
  bool published_ = false;
  // what was refused for want of room, and until when (vram_room.h): the steps above the fit that is made are asked again after their wait
  struct StepKey {
    gi::Layout layout = gi::Layout::kNone;
    gi_field::Step step = gi_field::Step::kNone;
    bool operator==(const StepKey& o) const { return layout == o.layout && step == o.step; }
  };
  vram_room::RefusalBook<StepKey> refusals_;

  // --- time and counts ---
  uint64_t frame_count_ = 0;
  double last_time_s_ = 0.0;
  bool have_time_ = false;
  uint32_t off_frames_ = 0;
  uint32_t idle_frames_ = 0;               // frames the world has had no field for (usable_ false)
  uint64_t last_none_log_ = 0;
  float safe_range_kept_ = 0.0f;           // the safe range of the last frame that was usable: the fade out of an idle field keeps it
  gi::Level shown_level_ = gi::Level::kOff;  // ... and its level
  uint64_t next_refit_frame_ = 0;
  uint32_t last_logged_fit_ = ~0u;
  uint64_t interval_start_frame_ = 0;
  uint64_t moves_ = 0, cuts_ = 0, relights_ = 0, exposed_ = 0, published_frames_ = 0, maintenance_runs_ = 0, reset_runs_ = 0, view_runs_ = 0, refits_ = 0;
  uint64_t resources_failed_ = 0;
};

}  // namespace mcla::gpu
