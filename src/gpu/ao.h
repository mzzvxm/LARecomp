#pragma once
// ===========================================================================
// MCLA GPU renderer -- screen-space ambient occlusion (feature-parity/AO-DESIGN.md)
// ===========================================================================
// The game has baked vertex occlusion in the world and a contact shadow under cars, and nothing that
// darkens creases, kerbs and wheel arches. This adds a screen-space term of its own: GTAO after
// Intel's XeGTAO (MIT) written as pixel shaders (hlsl/ao_*_ps.hlsl), applied as a weighted multiply
// on the scene colour.
//
// WHERE: at the game's mid-scene depth resolve (the first resolve per frame of the main view's depth,
// outside EndTiling, into 078B0000): after every opaque draw of the main view, before the soft
// particles, the rain, the fog quad and the post chain, so fog and particles cover the darkened scene
// as they cover everything else, and a temporal upscaler (temporal.h) sees the occlusion as part of
// the colour it accumulates. The depth is complete there and is read straight from the EDRAM depth
// target, guest sample 0 (the sample the game's own particle and fog reads see). The game resolves
// that depth in tiles (two: the rows 0-512 and 512-720, a leftover of the 360 drawing the frame once
// per tile), but the renderer draws the frame once, so the target is whole at the first resolve: the
// occlusion is worked out over the whole picture there (the destination texture's size, not the
// tile's), and the later tiles' resolves of the same picture are not triggers.
//
// WHAT: five passes into images of their own, then one into the main view's colour target:
//   depth    the linear view-space distance, R32_FLOAT, at the AO's size (the render size, or half of
//            it at LOW), and two coarser levels, R16_FLOAT
//   main     the GTAO visibility (R16_FLOAT), its noise changing every frame
//   denoise  a depth-aware 3x3 blur of it (R16_FLOAT): once when the occlusion has a history of its
//            own, twice when it has none; never left out
//   temporal the accumulation over frames: this frame's visibility averaged with the last frame's,
//            found again by the camera's own motion and kept only where it is the same surface
//            (see below); R16G16_FLOAT = (visibility, linear depth), the next frame's history
//   apply    rgb *= lerp(1, ao, strength * ambient_share * distance_fade) on every sample of the
//            colour target; ambient_share = La / (La + Ls * N.L) from the game's own constants (PS c26
//            the ambient, PS c39 the sun colour, VS c16 the direction the sunlight travels: the
//            direction to the sun is its opposite), N from the depth; mcla_gpu_ao_direct moves the
//            share towards 1 (the occlusion then darkens the sun-lit part too)
// The lens (the projection) is the previous frame's: the camera exists only at the game's motion blur
// draw, which comes after the trigger (OnAnchor keeps each frame's). A frame without a previous
// frame's camera has no occlusion.
//
// The pause menu's, the race pause's and the map's world (the same main view identities at 4x
// samples, drawn once a frame like the play view, resolved in three tiles) is handled the same way,
// with its own camera: its motion blur draw runs in every one of those frames with a valid camera
// (the census: 918 of 918), so the camera kept is the previous frame's of a pause/map view, never
// the play view's (the pause camera has another field of view, 53.75 degrees against 25). The first
// two frames of a pause spell have none (the second has no rate of change of the lens yet), and
// neither has a frame after a sweep of the lens (the camera's swing when the pause opens, the
// map): a change of the setting in the pause menu shows two frames later, without closing it. The
// trigger there is the same resolve (into 078B0000, the first of the frame); the host draws the 4x
// target once, so it is whole at the first tile.
//
// THE HISTORY (hlsl/ao_temporal_ps.hlsl): the sampling noise changes every frame and the
// occlusion averages it over frames, as the games' own do (mcla_gpu_ao_history: the frames, 12 by
// default; an exponential average once that many have been added up, the plain mean before). The
// game has no motion vectors for what moves by itself, so a pixel is found in the last frame from
// the camera alone, and the history is trusted only where it is the same surface: the plane test
// and the depth test of the shader, a clamp to this frame's neighbourhood against ghosts, the
// vehicle stencil IDs. What it cannot trust gets the wide blur of this frame in its place. The
// camera of the trigger's own frame is not known when it fires (the anchor draw comes later), so
// the motion into the last frame is either the frame's own view inverse read from the vertex
// constants of its draws (c12-c15: trusted only once it matched the anchor's camera in a frame the
// camera moved in) or the last step repeated (the constant velocity guess); FrameMotion. The
// history starts again after a cut (the temporal system's own rule on the camera), a gap of frames,
// the pause/map view switching, a new image size, and the debug view that overwrites it. One
// history serves both views: switching is a reset (the pause view's first frame has no occlusion
// anyway).
//
// The census (mcla_gpu_ao_census, off by default) is step 0 of the design: it logs the trigger, the
// light constants, the lens and, at its frames, a statistic of the linear depth read back from the
// GPU, so that a run can settle the design's CHECK items.
//
// Settings: mcla_gpu_ao (off, low, high; live; the pause menu's AMBIENT OCCLUSION row) and
// mcla_gpu_ao_intensity (light, default, strong, max; live; the AO INTENSITY row, grey while the
// occlusion is off). An intensity is four numbers: strength, radius (0.5 to 6 m), power (the
// visibility's exponent) and direct (how much the sun-lit part is darkened too). Off the menu,
// mcla_gpu_ao_strength, _radius, _power and _direct replace that one number of the chosen
// intensity when they are set to 0 or more (they default to -1: not set), so a test can move
// one of them and keep the rest; also _history (the frames averaged), _fade_end, _view, _sun_flip.
// The push block stays 128 bytes: power rides in shape.x, direct in the free lane extra.z.
//
// Threading: render thread only; Initialize and Shutdown on the lifecycle thread. The pipelines are
// made ahead on the host builder thread (host_precache.h).
// ===========================================================================

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <plume_render_interface.h>

#include "constants.h"
#include "edram.h"
#include "host_passes.h"
#include "records.h"
#include "resolve.h"
#include "temporal_camera.h"
#include "temporal_identities.h"

namespace mcla::gpu {

class BindlessHeap;
class Device;
class FrameRing;
class PassTiming;

class AmbientOcclusion {
 public:
  enum class Mode : uint8_t { kOff, kLow, kHigh };

  // The destination of the game's mid-scene depth resolve in the retail build (k_24_8_FLOAT,
  // 1280x720, the product the soft particles and the fog quad read), as the other identities of
  // temporal_identities.h: a different build gives another address and then there is no AO, never a
  // wrong picture.
  static constexpr uint32_t kTriggerDestination = 0x078B0000;

  // What the Scene tells of a prepared draw of the main view (temporal_identities.h MatchMainView).
  struct MainDraw {
    temporal::MainMatch match;
    bool writes_depth = false;  // the pipeline writes depth
  };

  // Why a depth resolve is or is not the trigger (one bit each for what failed).
  enum Refusal : uint32_t {
    kRefusedNotDepthCopy = 1u << 0,   // not a copy of an EDRAM depth target
    kRefusedNotMainDepth = 1u << 1,   // the source is not the main view's depth identity
    kRefusedEndTiling = 1u << 2,      // inside EndTiling (the end-of-scene copies)
    kRefusedNotFirst = 1u << 3,       // not the first qualifying resolve of the frame
    kRefusedAfterAnchor = 1u << 4,    // the anchor draw or the composite marker came first
    kRefusedDestination = 1u << 5,    // not into kTriggerDestination
    kRefusedClearsDepth = 1u << 6,    // the same call clears the depth after the copy
    kRefusedRectangle = 1u << 7,      // the rectangle does not start at the origin or leaves the picture
    kRefusedLaterTile = 1u << 8,      // another tile of the picture the trigger already covered whole
    kRefusedNotWholePicture = 1u << 9,  // a 4x view's destination is smaller than the main target
  };

  // What became of the frame's trigger (the census says it).
  enum class Outcome : uint8_t {
    kNoTrigger,      // no resolve of the frame was the trigger
    kIdle,           // the trigger, with the occlusion off and no depth statistic due
    kNoCamera,       // no previous frame's camera of this kind of view
    kLensUnknown,    // a pause/map view's second frame: the lens' rate of change is not known yet
    kLensChanging,   // a pause/map view whose lens was still sweeping
    kTooSmall,       // the picture is smaller than the passes are worth
    kNotRecorded,    // a pipeline or an image could not be made
    kRecorded,       // the passes were recorded
  };

  AmbientOcclusion() = default;
  ~AmbientOcclusion();
  AmbientOcclusion(const AmbientOcclusion&) = delete;
  AmbientOcclusion& operator=(const AmbientOcclusion&) = delete;

  // Makes the layout and the shaders and lists the pipelines with the host precache (before the
  // Renderer starts it). Never fails the renderer: without them there is no occlusion (logged).
  void Initialize(Device& device, FrameRing& frames, BindlessHeap& heap, HostPasses& passes,
                  EdramTargets& edram, PassTiming& pass_timing);
  // GPU idle and the FrameRing drained, before HostPasses and the BindlessHeap shut down.
  void Shutdown();

  // The Scene opened a frame's first list: reads the settings.
  void BeginFrame();
  // True while the draw hooks have anything to do (the occlusion is on, or the census).
  bool wants_draws() const { return mode_ != Mode::kOff || census_every_ != 0; }

  // A prepared draw into the main view's identities (the play view's or the pause/map view's), with
  // the draw's own banks, before the draw is recorded.
  void OnMainDraw(const MainDraw& draw, const ConstantBanks::Banks& banks);
  // The game's motion blur draw (in the play view and in the pause/map view alike): the camera of
  // this frame, which the next frame's trigger of the same kind of view uses.
  void OnAnchor(const ConstantBanks::Banks& banks);
  // A depth clear of the identity `key` (the main view's depth is cleared at the start of the main
  // view; its z says which end of the range is far).
  void OnDepthClear(const EdramKey& key, float z);
  // A resolve, after ResolveCache::Execute made its copy, with `list` the frame's open list
  // outside any render pass. `upscaler_running`: a temporal upscaler works on this frame (only
  // the log says it: the noise changes every frame whatever runs). True when passes were
  // recorded: the caller's list state is gone.
  bool OnResolve(const ResolveRecord& record, const ResolveCache::Summary& summary,
                 plume::RenderCommandList* list, bool upscaler_running);
  // The scene is composed: the UI starts.
  void OnComposite() { composite_seen_ = true; }
  // A guest present: the frame boundary. Prints the census frame.
  void OnPresent();

  // The video memory the images hold now.
  uint64_t HeldBytes() const;

 private:
  // The push block of every pass (hlsl/ao_common.hlsli AoPush), 128 bytes.
  struct Push {
    uint32_t slots[4];
    uint32_t slots2[4];
    float sizes[4];
    float lens[4];
    float lin[4];
    float shape[4];
    float light[4];
    float extra[4];
  };
  static_assert(sizeof(Push) == 128);

  // The pixel shaders, in the order host_shaders.h AmbientOcclusionPixelShader numbers them.
  enum Pass : uint32_t {
    kPassDepth,
    kPassDepthMs,
    kPassMip,
    kPassMain,
    kPassDenoise,
    kPassApply,
    kPassApplyMs,
    kPassTemporal,
    kPassTemporalMs,
    kPassCount,
  };

  // The pipelines: one per pass and kind of depth plane, and the apply pass per colour target's
  // sample count and per kind of output (the multiply, or a debug view's write).
  enum Variant : uint32_t {
    kVDepth = 0,
    kVDepthMs = 1,
    kVMip = 2,
    kVMain = 3,
    kVDenoise = 4,
    kVTemporal = 5,
    kVTemporalMs = 6,
    kVApplyBase = 8,  // + 4 * debug + log2(samples)
    kVariantCount = kVApplyBase + 8,
  };
  static uint32_t ApplyVariant(bool debug, uint32_t host_samples);
  static std::string VariantName(uint32_t variant);

  // The light constants of a draw, as the raw big-endian dwords of the registers: PS c26.xyz (the
  // ambient, gLightAmbient), PS c39.xyz (the sun colour, gLightColor), VS c16.xyz (gLightPosDir:
  // the direction the sunlight travels, measured at noon: y negative; the direction to the sun is
  // its opposite). Compared as bytes; decoded for the log and the shaders.
  struct LightRaw {
    uint32_t dwords[9] = {};
    bool operator==(const LightRaw& o) const;
  };
  struct LightSeen {
    LightRaw raw;
    uint32_t draws = 0;
  };
  static constexpr size_t kLightSlots = 8;

  // One resolve of the main view's depth, for the census.
  struct DepthResolveNote {
    uint32_t caller = 0;
    uint32_t flags = 0;
    EdramKey source;
    uint32_t dest_address = 0;
    uint32_t dest_format = 0;
    uint32_t dest_width = 0;
    uint32_t dest_height = 0;
    int32_t rect[4] = {};
    uint32_t host_sample = 0;
    uint32_t host_samples = 0;
    bool inside_end_tiling = false;
    bool clears_depth = false;
    uint32_t refused = 0;
    uint32_t draws_before = 0;
    bool anchor_before = false;
  };

  // The trigger test of AO-DESIGN.md 2.2: 0 = this resolve is the trigger. A 4x main depth (the
  // pause/map view) is judged by the same rules, except that its first tile may start anywhere
  // (the tile order is not known) and its destination must be the whole picture.
  uint32_t Judge(const ResolveRecord& record, const ResolveCache::Summary& summary) const;
  // The whole picture the resolve belongs to, in the source's guest pixels: the destination
  // texture's size, within the source target (the target's height is rounded up to rows of 32:
  // 736 for the picture's 720). The tile a resolve copies is a part of it.
  static void PictureSize(const ResolveCache::Summary& summary, uint32_t* width, uint32_t* height);
  // True when `summary` is another tile of the picture the trigger of this frame covered.
  bool IsLaterTile(const ResolveCache::Summary& summary) const;
  // The light constants most main view draws carried before the trigger (the last one's when none
  // repeats); false when no draw has been seen.
  bool PickLight(LightRaw* out, uint32_t* draws_with_it) const;
  static void DecodeLight(const LightRaw& raw, float ambient[3], float sun[3], float dir[3]);
  void Census();
  // The frame's view for the census: play, pause/map, both, or none.
  const char* ViewName() const;
  // Where the trigger's camera came from, and what became of the trigger, for the census.
  std::string TriggerText() const;

  // --- the passes (ao.cpp) ---
  struct Plan {
    Mode mode = Mode::kHigh;
    bool probe = false;            // read the coarsest level back for the census
    bool apply = true;             // the apply pass runs (false: the census's probe only)
    bool upscaler = false;
    uint32_t render[2] = {};       // the main view, host pixels
    uint32_t ao[2] = {};           // the AO's size
    uint32_t level[2][2] = {};     // the coarser levels' sizes
    uint32_t view = 0;
  };
  // Records the passes for the trigger `summary` (whose source target is the depth). True when
  // anything was recorded.
  bool Run(const ResolveCache::Summary& summary, plume::RenderCommandList* list, const Plan& plan);
  bool EnsureImages(const Plan& plan);
  void ReleaseImages();
  plume::RenderPipeline* Pipeline(uint32_t variant);
  std::unique_ptr<plume::RenderPipeline> MakePipeline(uint32_t variant);
  void ListPrecache();
  void Bind(plume::RenderCommandList* list) const;
  // A full-screen triangle into `image` (all of it) with `pipeline` and `push`.
  void DrawPass(plume::RenderCommandList* list, HostImage& image,
                const plume::RenderPipeline* pipeline, const Push& push);
  // The apply pass into the main view's colour target; false when it could not be made.
  bool RecordApply(plume::RenderCommandList* list, const ResolveCache::Summary& summary,
                   const Plan& plan, const Push& push);
  void FillPush(Push* push, const ResolveCache::Summary& summary, const Plan& plan,
                const LightRaw* light) const;
  // The look (strength, radius, power, direct): mcla_gpu_ao_intensity's, with the testing cvars
  // that are set on top of it, read when the passes are recorded; logged when it changes.
  struct Look {
    float strength = 1.0f;
    float radius = 3.0f;
    float power = 3.6f;
    float direct = 0.375f;
  };
  void UpdateLook();

  // --- the history (hlsl/ao_temporal_ps.hlsl) ---
  // Why a frame's occlusion had no history to lean on (ChooseHistory), or kUsed.
  enum class HistoryWhy : uint8_t {
    kUsed,
    kFirst,  // none yet, or given back with the images, or dropped by a debug view
    kGap,    // not the frame after the one that made it
    kView,   // the other kind of view (play against pause/map)
    kSize,   // another image size
    kCut,    // the camera jumped or turned (the temporal system's cut rule)
    kCap,    // mcla_gpu_ao_history is 1
    kCount,
  };
  // Where the camera's motion into the last frame came from.
  enum class Pose : uint8_t { kStill, kPredicted, kVertexBank };
  struct HistoryUse {
    bool on = false;
    HistoryWhy why = HistoryWhy::kFirst;
    Pose pose = Pose::kStill;
    uint32_t age = 0;        // pictures already averaged into the history, less the first
    uint32_t cap = 12;       // the frames the average spans at most
    temporal::Mat4 motion;   // this frame's game view space into the last frame's (row vectors)
  };
  static const char* HistoryWhyName(HistoryWhy why);
  static const char* PoseName(Pose pose);
  // Decides the frame's history: its use, and the matrix that moves this frame's view space into
  // the history's.
  HistoryUse ChooseHistory(const Plan& plan) const;
  // The motion of the trigger's frame's camera into the previous frame's (true unless the
  // matrix is not finite); *cut says that the frame's own camera jumped (vertex bank only).
  bool FrameMotion(temporal::Mat4* motion, Pose* source, bool* cut) const;
  // Counts what ChooseHistory decided (the census says it) and logs the first use.
  void NoteHistory(const HistoryUse& use);
  // The census's depth statistic: a copy of the coarsest level into a readback buffer, and its
  // processing when the GPU has run the copy.
  void RecordProbe(plume::RenderCommandList* list, const Plan& plan);
  void ProcessProbe(uint64_t frame, uint64_t generation, uint32_t width, uint32_t height,
                    uint32_t row_texels, uint32_t level_shift);

  // Settings, read at the frame's start.
  Mode mode_ = Mode::kOff;
  uint32_t census_every_ = 0;
  uint32_t off_frames_ = 0;

  // The frame.
  uint64_t frame_ = 0;
  bool anchor_seen_ = false;
  bool composite_seen_ = false;
  bool play_view_ = false;            // the frame had main view draws at the play samples
  bool pause_view_ = false;           // ... at the pause/map samples (4x)
  uint32_t pause_spell_ = 0;          // consecutive pause/map frames up to this one
  uint32_t qualifying_resolves_ = 0;  // resolves that passed every test but "the first"
  // The picture the frame's first qualifying resolve covered, and how many resolves of its other
  // tiles followed (they are not triggers, and not counted as qualifying).
  EdramKey trigger_source_;
  uint32_t trigger_destination_ = 0;
  uint32_t trigger_format_ = 0;
  uint32_t trigger_size_[2] = {};
  int32_t trigger_rect_[4] = {};
  uint32_t later_tiles_ = 0;
  uint32_t draws_before_ = 0;         // main view draws before the first qualifying resolve
  uint32_t color_draws_before_ = 0;
  uint32_t draws_after_ = 0;          // ... after it, before the anchor
  uint32_t depth_writes_after_ = 0;   // ... of them, drawing into the main depth with depth writes
  uint32_t draws_at_anchor_ = 0;      // ... after the anchor (the UI's depth-only draw, 1x)
  uint32_t view4x_draws_ = 0;         // main view draws at the pause/map samples, whenever
  std::array<LightSeen, kLightSlots> lights_{};
  uint32_t light_count_ = 0;
  uint32_t light_overflow_ = 0;       // draws with a value past the table's room
  uint32_t light_draws_ = 0;
  LightRaw first_light_;
  LightRaw last_light_;
  float depth_clear_z_ = -1.0f;
  uint32_t depth_clears_ = 0;
  std::vector<DepthResolveNote> depth_resolves_;
  uint32_t other_resolves_ = 0;
  bool frame_had_main_view_ = false;
  bool ran_this_frame_ = false;
  bool applied_this_frame_ = false;   // ... and the apply pass drew into the colour target

  // The camera: this frame's (read at the anchor) and the frame before's, with the kind of view
  // each came from (a pause/map frame's camera is never used for a play frame or the other way).
  temporal::Camera camera_;
  bool have_camera_ = false;
  temporal::Camera previous_;
  bool have_previous_ = false;
  bool previous_pause_ = false;
  uint64_t previous_frame_ = 0;
  // The trigger's own lens (the camera of the frame before it), kept for the census: previous_
  // is replaced by this frame's camera at the anchor.
  temporal::Camera trigger_camera_;
  bool trigger_camera_valid_ = false;
  bool trigger_pause_ = false;        // the frame's trigger was a pause/map view's
  Outcome outcome_ = Outcome::kNoTrigger;
  // How much the lens changed from the camera before the previous to the previous one (the same
  // kind of view, consecutive frames): the largest relative change of fov, aspect, near and far.
  // A pause/map view whose lens is sweeping (the pause camera's swing, the map), or whose rate of
  // change is not known yet (its second frame), has no occlusion: the previous frame's lens may
  // then be too far from this frame's.
  bool lens_step_valid_ = false;
  double lens_step_ = 0.0;
  double trigger_lens_step_ = 0.0;    // lens_step_ as the frame's trigger saw it
  temporal::Reject reject_ = temporal::Reject::kNone;
  uint32_t rejects_logged_ = 0;
  // The previous frame's lens against this frame's (CHECK 7): relative change of the vertical field
  // of view, the aspect, near and far, then the distance the camera moved and the cosine between
  // the two forward vectors.
  bool lens_delta_valid_ = false;
  double lens_delta_[6] = {};
  // CHECK 7, log only: whether the main view's vertex bank holds the camera, so that a frame's own
  // lens could be had before the anchor. VS c12-c15 of the last colour draw before the trigger
  // against the view inverse the anchor reads (PS c12-c15), per kind of view.
  float vs_view_inverse_[16] = {};
  uint32_t vs_view_inverse_draws_ = 0;
  bool vs_compare_valid_ = false;
  double vs_compare_diff_ = 0.0;
  uint32_t vs_compared_[2] = {};      // frames compared (play, pause/map)
  uint32_t vs_equal_[2] = {};         // ... of them equal

  // The census.
  uint32_t census_frames_logged_ = 0;
  uint32_t census_anomalies_logged_ = 0;
  uint32_t census_pause_logged_ = 0;
  uint32_t probes_done_ = 0;
  uint32_t probe_frames_ = 0;

  // The GPU side.
  Device* device_ = nullptr;
  FrameRing* frames_ = nullptr;
  BindlessHeap* heap_ = nullptr;
  HostPasses* passes_ = nullptr;
  EdramTargets* edram_ = nullptr;
  PassTiming* pass_timing_ = nullptr;
  bool ready_ = false;  // the layout and the shaders exist
  std::unique_ptr<plume::RenderPipelineLayout> layout_;
  std::array<std::unique_ptr<plume::RenderShader>, kPassCount> shaders_;
  std::array<std::unique_ptr<plume::RenderPipeline>, kVariantCount> pipelines_;
  std::array<bool, kVariantCount> pipeline_failed_{};
  Look look_;
  Look look_logged_;
  uint32_t look_logged_index_ = ~0u;
  bool look_logged_overridden_ = false;
  HostImage depth_[3];  // the linear depth chain: the AO's size, then 1/2 and 1/4 of it
  HostImage raw_;       // the main pass's visibility
  HostImage denoised_;  // the denoise's
  // The history: (visibility, linear depth) of the last frame that ran the passes, R16G16_FLOAT,
  // two images that take turns (a pass does not read the image it writes); history_[history_index_]
  // is the one written last.
  HostImage history_[2];
  uint32_t history_index_ = 0;
  bool history_valid_ = false;    // it holds a picture the next frame may lean on
  bool history_cut_ = false;      // ... and was dropped by a cut (for the census)
  uint64_t history_frame_ = 0;    // the frame that wrote it
  bool history_pause_ = false;    // ... which kind of view it was
  uint32_t history_size_[2] = {}; // ... at which image size
  float history_tan_[2] = {};     // ... with which lens (tan of the half field of view, x and y)
  uint32_t history_age_ = 0;      // pictures averaged into it, less the first
  // The camera's motion from the frame before the previous to the previous one, measured at the
  // previous frame's anchor draw (game view space to game view space, row vectors): the guess of
  // the step the next frame takes (FrameMotion), and the position change of that step for the
  // cut rule.
  temporal::Mat4 last_motion_;
  bool last_motion_valid_ = false;
  uint64_t last_motion_frame_ = 0;
  double last_step_[3] = {};
  bool have_last_step_ = false;
  // The vertex constants c12-c15 held this frame's view inverse in the last frame the camera moved
  // in (OnAnchor compares them with the anchor's): the trigger may then use them as the frame's own
  // camera.
  bool vs_pose_trusted_ = false;
  // The census: what the last frame's trigger did with the history, and the totals.
  HistoryWhy history_why_ = HistoryWhy::kFirst;
  Pose history_pose_ = Pose::kStill;
  uint64_t history_used_ = 0;
  uint64_t history_resets_[size_t(HistoryWhy::kCount)] = {};
  uint64_t pose_frames_[3] = {};
  bool history_logged_ = false;
  uint32_t images_made_for_[2] = {};
  bool images_failed_logged_ = false;
  // The census probe's readback.
  std::unique_ptr<plume::RenderBuffer> probe_buffer_;
  uint64_t probe_buffer_bytes_ = 0;
  bool probe_busy_ = false;
  uint64_t generation_ = 1;  // bumped when the images go: a probe in flight is then ignored

  // What the log has said once.
  bool no_lens_logged_ = false;
  bool no_lens_pause_logged_ = false;
  bool lens_step_logged_ = false;
  bool pause_destination_logged_ = false;
  bool refused_target_logged_ = false;
  uint32_t last_mode_logged_ = ~0u;
  uint64_t runs_ = 0;
  uint64_t skipped_no_lens_ = 0;
  uint64_t pause_runs_ = 0;                // occlusion passes recorded in pause/map frames
  uint64_t pause_skipped_no_lens_ = 0;     // pause/map triggers without a camera of their kind
  uint64_t pause_skipped_lens_ = 0;        // ... held back for the lens (unknown or sweeping)
};

}  // namespace mcla::gpu
