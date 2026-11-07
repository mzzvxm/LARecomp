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
// WHAT: four passes into images of their own, then one into the main view's colour target:
//   depth    the linear view-space distance, R32_FLOAT, at the AO's size (the render size, or half of
//            it at LOW), and two coarser levels, R16_FLOAT
//   main     the GTAO visibility (R16_FLOAT)
//   denoise  a depth-aware blur of it (R16_FLOAT)
//   apply    rgb *= lerp(1, ao, strength * ambient_share * distance_fade) on every sample of the
//            colour target; ambient_share = La / (La + Ls * N.L) from the game's own constants (PS c26
//            the ambient, PS c39 the sun colour, VS c16 the direction the sunlight travels: the
//            direction to the sun is its opposite), N from the depth
// The lens (the projection) is the previous frame's: the camera exists only at the game's motion blur
// draw, which comes after the trigger (OnAnchor keeps each frame's). A frame without a previous
// frame's camera has no occlusion. The pause menu's and the map's world (4x samples) has none.
//
// The census (mcla_gpu_ao_census, off by default) is step 0 of the design: it logs the trigger, the
// light constants, the lens and, at its frames, a statistic of the linear depth read back from the
// GPU, so that a run can settle the design's CHECK items.
//
// Settings: mcla_gpu_ao (off, low, high; live; the pause menu's AMBIENT OCCLUSION row), and off the
// menu mcla_gpu_ao_strength, _radius, _fade_end, _view, _sun_flip.
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

  // A prepared draw into the main view's identities (not the pause view's), with the draw's own
  // banks, before the draw is recorded.
  void OnMainDraw(const MainDraw& draw, const ConstantBanks::Banks& banks);
  // The game's motion blur draw: the camera of this frame, which the next frame's trigger uses.
  void OnAnchor(const ConstantBanks::Banks& banks);
  // A depth clear of the identity `key` (the main view's depth is cleared at the start of the main
  // view; its z says which end of the range is far).
  void OnDepthClear(const EdramKey& key, float z);
  // A resolve, after ResolveCache::Execute made its copy, with `list` the frame's open list
  // outside any render pass. `upscaler_running`: a temporal upscaler works on this frame (the
  // noise then rotates). True when passes were recorded: the caller's list state is gone.
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

  // The trigger test of AO-DESIGN.md 2.2: 0 = this resolve is the trigger.
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
  uint32_t view4x_draws_ = 0;
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

  // The camera: this frame's (read at the anchor) and the frame before's.
  temporal::Camera camera_;
  bool have_camera_ = false;
  temporal::Camera previous_;
  bool have_previous_ = false;
  uint64_t previous_frame_ = 0;
  temporal::Reject reject_ = temporal::Reject::kNone;
  uint32_t rejects_logged_ = 0;
  // The previous frame's lens against this frame's (CHECK 7): relative change of the vertical field
  // of view, the aspect, near and far, then the distance the camera moved and the cosine between
  // the two forward vectors.
  bool lens_delta_valid_ = false;
  double lens_delta_[6] = {};

  // The census.
  uint32_t census_frames_logged_ = 0;
  uint32_t census_anomalies_logged_ = 0;
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
  HostImage depth_[3];  // the linear depth chain: the AO's size, then 1/2 and 1/4 of it
  HostImage raw_;       // the main pass's visibility
  HostImage denoised_;  // the denoise's
  uint32_t images_made_for_[2] = {};
  bool images_failed_logged_ = false;
  // The census probe's readback.
  std::unique_ptr<plume::RenderBuffer> probe_buffer_;
  uint64_t probe_buffer_bytes_ = 0;
  bool probe_busy_ = false;
  uint64_t generation_ = 1;  // bumped when the images go: a probe in flight is then ignored

  // What the log has said once.
  bool no_lens_logged_ = false;
  bool refused_target_logged_ = false;
  uint32_t last_mode_logged_ = ~0u;
  uint64_t runs_ = 0;
  uint64_t skipped_no_lens_ = 0;
};

}  // namespace mcla::gpu
