#pragma once
// ===========================================================================
// MCLA GPU renderer -- sun shafts
// ===========================================================================
// The game has no light shafts: a low sun behind trees and buildings lights the sky and the street
// and nothing in between. This adds them in screen space, the way most games of its time did
// (Mitchell, "Light Shaft Rendering", GPU Gems 3, chapter 13): the brightness of the sky is blurred
// radially toward the sun's position in the picture, so the gaps in front of the sun stream light
// across the scene, and the result is added to the scene colour as scattered sunlight.
//
// WHERE (the frame, see AO-DESIGN.md 2.1 and GFX-UPGRADES-DESIGN.md 6B.1): two points of the main view.
//   1. the game's mid-scene depth copy (phase 0x02000000, into 078B0000: the ambient occlusion's
//      point, ao.h): every opaque draw is in the colour target, the fog, particles and the post chain
//      are not. Here the sky's brightness is taken from the colour and the depth (one image at a quarter of the
//      picture's size, capped at a 480x270 box at 16:9: shafts_image.h; hlsl/shafts_sky_ps.hlsl). The depth of the main view is complete here and is read here. This
//      point is in every frame of the play view whatever the hour, the weather, the render scale or
//      the upscaler (the main view's depth is copied there in all of them), and whether the sun gives
//      shafts is decided by the sun alone (below): a noon, a night or a rain takes no sky and says
//      nothing.
//   2. the first copy of the main colour after the first point (the game's end-of-scene copy of the
//      picture, into 07FE0, two tiles of the picture: the first is the trigger), BEFORE the copy is
//      made: the fog quad and the particles are in the colour by then, so the shafts are added in
//      front of them (in-scattering lies between the camera and what it sees), and the copy, the
//      temporal upscaler, the bloom and the tone map all see them. The radial blur
//      (hlsl/shafts_blur_ps.hlsl) and the additive pass (hlsl/shafts_add_ps.hlsl) run here. It is
//      found by its source (colour 0, the main colour's identity at the depth's samples), its place
//      (after the first point, before the anchor draw) and by being a copy; it does NOT have to be
//      issued from inside EndTiling: the census shows the game's tile copies of the main depth as plain
//      Resolve calls (perf/out/AO.txt, caller 0x821790e4), and the first version of this pass, which
//      asked for a copy inside EndTiling, never found one (w1s-survey-h17_7-both).
// The second point is the preferred one, and the shafts_anchor.h policy keeps the pass alive if a build
// or a scene gives no such copy: after a few frames without it the shafts go in at the first point (the
// fog then covers them at distance), the copy is still watched, and the end of the scene is used again
// when it comes back. mcla_gpu_sun_shafts_early forces the first point (to compare the two).
//
// WHAT: per frame, when it is on and the sun is where shafts show (below):
//   sky     the quarter-resolution image of max(luma(colour) - threshold, 0)
//           where the depth is the far plane
//   blur    two eight-tap radial passes combining the exponentially weighted sky along the ray
//   add     the blurred image times the sun's colour and the selected strength, combined with the
//           volume composite whenever they share a target and anchor
// Nothing is recorded, and nothing costs, in a frame where the sun gives no shafts.
//
// THE SUN: the game has two (shafts_sun.h). The light's direction and colour are the game's own constants
// as the ambient occlusion reads them (VS c16 the direction the light travels, PS c39 the sun colour),
// taken from the draws of the main view before the first point. The sun the player sees is the sky's:
// PS c65 `SunDirection` of the sky shaders (the city's xSkyhat ps_main*, ps_sunonly, listed in
// shafts_sun.h; the direction toward the sun), told by the sky's own draws (NoteSkyDraw). The two differ: at 17.7 h the light is 18.7
// degrees above the horizon and the disc about 2 (ports/run/w1s-survey-h17_7-both), so a blur toward the
// light's place converged above the picture. The rays radiate from the disc: THE PLACE IN THE PICTURE
// comes from the sky's constant, and from the light when no sky draw has told it, when it is not a
// direction or when it is more than shafts_sun::kMaxSkyLightAngle from the light (not the sun in these
// axes). The camera is the previous frame's (it exists only at the game's motion blur draw, after both
// points), or the frame's own view from the vertex constants once those have matched the anchor's (as
// ao.h: vs_pose_trusted_). The strength is the product of
//   elevation  0 at and under the horizon, full from 2.5 degrees, falling to none at 45 degrees: the
//              light's elevation (asin of minus c16.y) says how much sunlight there is and when the day
//              is too high for shafts, and the fade in is of the lower of the light's and the sky's sun
//              (a disc at the horizon gives none): dawn and dusk, where the cascades are off
//              (the census: none at 6 h, 18 h and 19 h to night) and a screen-space shaft is the only
//              possible one; noon, 60 degrees, shows none and records nothing
//   wetness    VS c43.x (gReflectionParams.x, 0.095 dry, 1.17 in rain): none in rain, where the sky is
//              flat and bright and a threshold on it would light all of it
//   view       1 while the sun is on the screen, falling to 0 a screen and a half beyond its
//              edge, and 0 behind the camera: no pop at the edge (the play camera looks through
//              25 degrees vertically, so a dusk sun is often just above the picture)
// The pause menu's, the race pause's and the garage's world (the 4x view, menu_world.h) have the shafts too,
// where a sky draw of the frame told where the sun is (the garage is an interior: none is drawn): the camera
// of that kind of view is kept apart from the play view's, the frames whose lens is sweeping (the pause camera's
// swing when the menu opens) have none (a lens that is sweeping is worse than none), and they are always added at
// the first point (the mid-scene depth copy), not at the end of the scene, which was never looked at in the 4x
// frames and teaches the anchor policy nothing. The map has none: the sun's rays are not a thing seen from 9 km.
//
// Settings: mcla_gpu_sun_shafts (auto, off, light, default, strong, stronger, strongest: how strong the shafts are,
// x1 to x5 of the built-in strength, off being the game's own picture; the old low and high are read as
// default; live; the pause menu's SUN SHAFTS row; `auto` uses the ENHANCED preset value on every GPU)
// and the tuning cvars _intensity (a fine multiplier on top of the level),
// _steps (positive values select the legacy march for comparison), _density, _threshold, _sky_depth, _view,
// _early, _cap (false: work at the full fraction of the picture at any size; true by default)
// and _census, all debug-only.
//
// Threading: render thread only; Initialize and Shutdown on the lifecycle thread. The pipelines are
// made ahead on the host builder thread (host_precache.h) when the setting in force at the start uses
// them (host_plan.h); a setting switched on later makes them in the background
// (HostPrecache::AddLive) and the shafts are not drawn until they are made (HoldUntilBuilt).
// ===========================================================================

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <plume_render_interface.h>

#include "atmosphere.h"
#include "constants.h"
#include "edram.h"
#include "host_passes.h"
#include "host_plan.h"
#include "menu_world.h"
#include "noise_resolve.h"
#include "records.h"
#include "resolve.h"
#include "shafts_anchor.h"
#include "shafts_image.h"
#include "shafts_level.h"
#include "shafts_sun.h"
#include "temporal_camera.h"
#include "temporal_identities.h"

namespace mcla::gpu {

class PassTiming;

class SunShafts {
 public:
  // How strong the shafts are (the setting; shafts_level.h): off, light, default, strong, stronger, strongest.
  using Level = shafts_level::Level;

  // The destination of the game's mid-scene depth copy in the retail build, the ambient occlusion's
  // trigger too (ao.h kTriggerDestination): another build gives another address and then there are
  // no shafts, never a wrong picture.
  static constexpr uint32_t kTriggerDestination = 0x078B0000;

  SunShafts() = default;
  ~SunShafts();
  SunShafts(const SunShafts&) = delete;
  SunShafts& operator=(const SunShafts&) = delete;

  // Makes the shaders and lists the pipelines with the host precache (before the Renderer starts
  // it). Never fails the renderer: without them there are no shafts (logged).
  void Initialize(HostPasses& passes, EdramTargets& edram, PassTiming& pass_timing);
  // GPU idle and the FrameRing drained, before HostPasses shuts down.
  void Shutdown();

  // The Scene opened a frame's first list: reads the settings.
  void BeginFrame(gbuffer::PauseFxView menu_view, bool capture);
  const HostImage* CaptureLight() const { return captured_ ? &capture_ : nullptr; }
  bool CaptureEmpty() const;
  // True while the draw hooks have anything to do.
  bool wants_draws() const { return level_ != Level::kOff; }

  // A prepared draw of the main view (play or pause/map, temporal_identities.h MatchMainView), with
  // the draw's own banks and its shaders' identities (the sky's draws tell where the sun is).
  void OnMainDraw(const temporal::MainMatch& match, const ConstantBanks::Banks& banks,
                  uint64_t vs_identity, uint64_t ps_identity);
  // The game's motion blur draw: this frame's camera, which the next frame's shafts use.
  void OnAnchor(const ConstantBanks::Banks& banks);
  // A resolve, BEFORE ResolveCache::Execute makes its copy, with `list` the frame's open list
  // outside any render pass: the first copy of the main colour after the first point is the second
  // point (shafts_anchor.h). True when the pending shafts should composite at this copy.
  bool BeforeResolve(const ResolveRecord& record, plume::RenderCommandList* list);
  bool AtMidScene() const { return pending_.valid && anchor_ == shafts_anchor::Anchor::kMidScene; }
  const HostImage* PrepareComposite(plume::RenderCommandList* list);
  bool MergeInput(const HostImage& shine, AtmosphereShafts& out) const;
  bool Composite(plume::RenderCommandList* list, const HostImage& shine, bool merged);
  // A resolve, after Execute: the mid-scene depth copy is the first point. `upscaler_running`: a
  // temporal upscaler works on this frame (the noise then shifts every frame, which it averages).
  bool OnResolve(const ResolveRecord& record, const ResolveCache::Summary& summary,
                 plume::RenderCommandList* list, bool upscaler_running);
  // The scene is composed: the UI starts.
  void OnComposite() { composite_seen_ = true; }
  // A guest present: the frame boundary.
  void OnPresent();
  void ResetPresetHistory() { hist_valid_ = false; }

  // The world of the frame that is ending (menu_world.h), by the frame's own camera, and the level in force, for
  // the scene's one line per change of world. Read before OnPresent, which forgets the frame.
  menu_world::World FrameWorld() const;
  std::string LevelName() const;

  // The video memory the images hold now.
  uint64_t HeldBytes() const;

 private:
  // The push block of every pass (hlsl/shafts_common.hlsli ShaftsPush), 64 bytes: the host pass
  // layout's push range.
  struct Push {
    uint32_t slots[4];
    float sizes[4];
    float sun[4];
    float tint[4];
  };
  static_assert(sizeof(Push) == 64 && sizeof(Push) == HostPasses::kPushConstantBytes);

  // The pixel shaders, in the order host_shaders.h SunShaftsPixelShader numbers them.
  enum Pass : uint32_t {
    kPassSky,
    kPassSkyMs,
    kPassBlur,
    kPassAdd,
    kPassTemporal,  // NOISE-RESOLVE: the blurred image averaged over frames while no temporal upscaler runs
    kPassLegacyBlur,
    kPassBlurAccumulate,
    kPassCount,
  };
  // The pipelines: the sky pass per kind of planes (plain or multisampled), the blur, and the add
  // pass per colour target's sample count and kind of output (the addition, or a debug view's write).
  enum Variant : uint32_t {
    kVSky = 0,
    kVSkyMs = 1,
    kVBlur = 2,
    kVTemporal = 3,
    kVLegacyBlur = 4,
    kVBlurAccumulate = 5,
    kVAddBase = 6,  // + 4 * debug + log2(samples)
    kVariantCount = kVAddBase + 8,
  };
  static uint32_t AddVariant(bool debug, uint32_t host_samples);
  static std::string VariantName(uint32_t variant);

  // The light constants of a draw, as the raw dwords of the registers: VS c16.xyz (the direction the
  // sunlight travels), PS c39.xyz (the sun colour) and VS c43.x (gReflectionParams.x, the wetness).
  // Compared as bytes; the commonest over the draws before the first point is the frame's.
  struct Seen {
    uint32_t dwords[7] = {};
    uint32_t draws = 0;
  };
  static constexpr size_t kSeenSlots = 8;

  // What the first point worked out for the second.
  struct Pending {
    bool valid = false;
    EdramKey colour;               // the main colour's identity of the frame
    uint32_t host_samples = 0;
    uint32_t render[2] = {};       // the picture in host pixels
    uint32_t image[2] = {};        // the sky and shaft images' size (shafts_image.h: half the picture, capped)
    float footprint[2] = {2.0f, 2.0f};  // the picture's pixels one texel of the sky image spans
    float sun_uv[2] = {};
    bool sun_from_sky = false;     // the sun's place came from the sky's constant (else the light's)
    float tint[3] = {};            // the light's colour times its strength
    uint32_t legacy_steps = 0;
    Level level = Level::kOff;     // the strength it was planned at
    float density = 0.9f;
    float noise_shift = 0.0f;
    bool history = false;          // NOISE-RESOLVE: no temporal upscaler averages the frames, so the noise moves every frame and the blurred image is averaged over frames (RecordTemporal)
    bool history_gap = false;      // NOISE-FOLLOW: it would be, but a temporal upscaler runs this frame: the history's images that are held stay (noise_resolve.h HistoryLifetime)
    uint32_t view = 0;
  };

  // The sun of a frame: where it is in the picture and how strongly it shines.
  struct Sun {
    bool valid = false;
    double elevation = 0.0;        // degrees, the light's (c16)
    double wetness = 0.0;
    double to_sun[3] = {};         // the light's sun in view space, x right, y up, z forward
    double light_ndc[2] = {};      // ... and where the light puts it in the picture
    float light_uv[2] = {};
    // The sky's own sun constant (PS c65 of the sky draws): what became of it, and where it puts the sun.
    shafts_sun::SkyState sky_state = shafts_sun::SkyState::kNotSeen;
    double sky_elevation = 0.0;    // degrees
    double sky_light_angle = 0.0;  // degrees between the sky's sun and the light's
    double sky_to_sun[3] = {};     // view space (set when the sky's constant is a direction)
    double sky_ndc[2] = {};
    float sky_uv[2] = {};
    uint64_t sky_age = 0;          // frames since a sky draw told it (0: this frame)
    bool from_sky = false;         // the sun's place below is the sky's
    double ndc[2] = {};            // the place used
    float uv[2] = {};
    double fade_elevation = 0.0;
    double fade_wet = 0.0;
    double fade_view = 0.0;
    float colour[3] = {};          // the sun's colour, the largest channel 1
    float strength = 0.0f;         // the product of the fades
    bool vertex_pose = false;      // the rotation came from the vertex constants
  };
  bool ComputeSun(Sun* out) const;
  // A draw of the main view whose pixel shader is one of the sky's (shafts_sun.h): its PS c65 is the sun disc's
  // direction. A draw with the dome's vertex shader and another pixel shader is counted and logged, and tells nothing.
  void NoteSkyDraw(uint64_t vs_identity, uint64_t ps_identity, const ConstantBanks::Banks& banks);
  // What the log says about the sky's sun constant, a few times and no more: first used, further from the
  // light than it may be, never told.
  void LogSkySun(const Sun& sun);
  static std::string SkyText(const Sun& sun);

  // Why a frame had no shafts (the census says it).
  enum class Outcome : uint8_t {
    kNoTrigger,   // no resolve of the frame was the first point
    kNoCamera,    // no previous frame's camera
    kNoLight,     // no usable light constants
    kGated,       // the fades gave nothing: no pass recorded
    kWorld,       // the world has none: the map, or an interior with no sky drawn (menu_world.h ShaftsRun)
    kTooSmall,    // the picture is smaller than the passes are worth
    kNotRecorded, // a pipeline or an image could not be made
    kSkyOnly,     // the sky was taken, the second point never came (nothing was added)
    kRecorded,    // all three passes
  };
  static const char* OutcomeName(Outcome outcome);

  bool IsFirstPoint(const ResolveRecord& record, const ResolveCache::Summary& summary) const;
  // The EDRAM identity of a resolve's colour 0 source (as FrameLayers::ResolveSourceKey).
  static bool ColourSourceKey(const ResolveRecord& record, EdramKey* key);
  // The resolves between the first point and the anchor, kept in short to say what they were when no
  // copy of the main colour came among them (`key`: the identity of a colour 0 source, else null).
  void NoteResolve(const ResolveRecord& record, const EdramKey* key);
  void LogMissingCopy();
  // The frame's end: the policy, the interval's tally and its log line.
  void EndSkyFrame();
  void LogInterval();
  static void PictureSize(const ResolveCache::Summary& summary, uint32_t* width, uint32_t* height);
  EdramTargets::Target* ColourTarget(const EdramKey& key, plume::RenderCommandList* list);

  // --- the passes (shafts.cpp) ---
  bool RecordSky(const ResolveCache::Summary& summary, plume::RenderCommandList* list,
                 const Pending& plan);
  // `shine` is the image the add pass reads: the blurred one, or its average over frames (RecordTemporal).
  bool RecordAdd(plume::RenderCommandList* list, EdramTargets::Target& colour,
                 const Pending& plan, const HostImage& shine);
  void RecordCapture(plume::RenderCommandList* list, const Pending& plan, const HostImage& shine);
  // NOISE-RESOLVE: the blurred image averaged with the last frame's result (hlsl/shafts_temporal_ps.hlsl); the image holding the result, or null (no pipeline or images yet: the add pass takes the blurred one).
  const HostImage* RecordTemporal(plume::RenderCommandList* list, const Pending& plan);
  bool EnsureImages(const Pending& plan);
  void ReleaseImages();
  plume::RenderPipeline* Pipeline(uint32_t variant);
  std::unique_ptr<plume::RenderPipeline> MakePipeline(uint32_t variant);
  // Makes the shaders once (Initialize when the shafts are on at the start, else BeginFrame at the switch);
  // false when one could not be made (said in the log, not tried again).
  bool MakeShaders();
  // The pipelines of the pass, in the order they are made (the likeliest first).
  std::vector<uint32_t> Variants() const;
  // The start: lists them with the host precache and says so (and why) for its log line.
  void ListPrecache(const host_plan::Choice& choice);
  // Every frame while the shafts are on: lists the pipelines on a worker when the start did not (the
  // setting was switched on since), and turns the shafts off for this frame until they are all made.
  void HoldUntilBuilt(Level* level);
  void Census();

  // Settings, read at the frame's start.
  Level level_ = Level::kOff;
  uint32_t census_every_ = 0;
  uint32_t off_frames_ = 0;

  // The frame.
  uint64_t frame_ = 0;
  bool anchor_seen_ = false;
  bool composite_seen_ = false;
  bool first_colour_seen_ = false;   // the frame's first colour draw of the main view was noted (the sky dome)
  bool play_view_ = false;           // the frame had main view draws at the play samples
  bool pause_view_ = false;          // ... at the pause/map samples (4x)
  bool first_point_seen_ = false;    // the first point's resolve came (whatever became of it)
  uint32_t main_draws_ = 0;          // play view colour draws counted before the first point
  std::array<Seen, kSeenSlots> seen_{};
  uint32_t seen_count_ = 0;
  uint32_t seen_overflow_ = 0;
  Seen last_seen_;
  // VS c12-c15 of the last colour draw before the first point, as the registers' dwords (decoded when
  // used: this is kept for every draw).
  uint32_t vs_view_inverse_[16] = {};
  uint32_t vs_view_inverse_draws_ = 0;
  Pending pending_;                  // the plan of the frame's shafts; valid until they are added
  Sun sun_;                          // the frame's, for the census
  Outcome outcome_ = Outcome::kNoTrigger;
  bool recorded_this_frame_ = false;
  // Where the shafts of this frame go and what became of them (a frame in which the sky was taken).
  bool sky_taken_ = false;           // the sky image was recorded at the first point
  shafts_anchor::Anchor anchor_ = shafts_anchor::Anchor::kEndOfScene;
  bool copy_seen_ = false;           // a copy of the main colour came after the first point, before the anchor
  bool composited_ = false;          // the blur and the add pass were recorded
  // The resolves seen between the first point and the anchor (what each was, for the log of a miss).
  struct SeenResolve {
    uint32_t caller = 0;
    uint32_t flags = 0;              // ResolveRecord::flags
    uint32_t resolve_flags = 0;
    bool keyed = false;              // a colour 0 source whose identity was read
    EdramKey key;
  };
  static constexpr size_t kSeenResolves = 8;
  std::array<SeenResolve, kSeenResolves> seen_resolves_{};
  uint32_t seen_resolve_count_ = 0;  // all of them, kept or not

  // The camera: the previous frame's (the lens and the view of the sun's position), and whether the
  // vertex constants have been shown to hold a frame's own view (ao.h).
  temporal::Camera previous_;
  gbuffer::PauseFxView menu_view_ = gbuffer::PauseFxView::kNone;
  bool have_previous_ = false;
  bool previous_pause_ = false;  // ... and it came from a pause/map view's frame (4x samples)
  uint64_t previous_frame_ = 0;
  bool vs_pose_trusted_ = false;
  // How much the lens changed between the two cameras before the trigger (the same kind of view,
  // consecutive frames): the pause camera's swing and the map's zoom move it fast, and a lens that is
  // sweeping gives no shafts for those frames (menu_world.h LensSteady).
  bool lens_step_valid_ = false;
  double lens_step_ = 0.0;
  menu_world::World trigger_world_ = menu_world::World::kNone;  // the world the first point was in
  bool first_world_run_logged_ = false;

  // The sky's sun constant, told by the sky's draws and kept from frame to frame (the hour moves slowly):
  // the direction toward the sun, the frame and the shader that told it, how many sky draws told it this
  // frame and in all, and what the log has said.
  float sky_dir_[3] = {};
  bool sky_have_ = false;
  uint64_t sky_frame_ = 0;
  uint64_t sky_ps_ = 0;
  uint32_t sky_draws_ = 0;
  uint64_t sky_draws_total_ = 0;
  // The shaders of the last frame's first colour draw of the main view (the dome), and the dome draws whose pixel
  // shader the table does not list: what the log says when no sky draw ever told the sun.
  uint64_t first_colour_vs_ = 0;
  uint64_t first_colour_ps_ = 0;
  uint64_t sky_unknown_draws_ = 0;
  uint64_t sky_unknown_ps_ = 0;
  uint32_t sky_unknown_logged_ = 0;
  uint32_t sky_missing_frames_ = 0;  // frames in a row with a sun to place and no sky constant
  uint32_t sky_missing_logged_ = 0;
  uint32_t sky_far_logged_ = 0;
  bool sky_used_logged_ = false;

  // The GPU side.
  HostPasses* passes_ = nullptr;
  EdramTargets* edram_ = nullptr;
  PassTiming* pass_timing_ = nullptr;
  bool ready_ = false;  // the shaders exist (made only when the sun shafts are on: MakeShaders)
  bool shaders_failed_ = false;
  std::array<std::unique_ptr<plume::RenderShader>, kPassCount> shaders_;
  std::array<std::unique_ptr<plume::RenderPipeline>, kVariantCount> pipelines_;
  std::array<bool, kVariantCount> pipeline_failed_{};
  bool listed_ = false;  // the pipelines are with the host precache (at the start, or since the switch)
  bool built_ = false;   // and all of them are made (HoldUntilBuilt asks no more)
  HostImage sky_;    // R16_FLOAT, quarter the picture (capped, shafts_image.h): the sky's brightness
  HostImage shaft_;  // R16_FLOAT, the blurred image
  HostImage blur_segment_;
  HostImage capture_;
  std::unique_ptr<plume::RenderPipeline> capture_pipeline_;
  bool capture_wanted_ = false;
  bool capture_off_ = false;
  bool captured_ = false;
  // NOISE-RESOLVE (noise_resolve.h): the blurred image's history while no temporal upscaler runs, two R16_FLOAT images of the same size that take turns (the add pass reads the one written last), and what
  // the next frame needs to know of it: that it exists, which frame wrote it, where the sun was, how long a history it holds.
  HostImage hist_[2];
  uint32_t hist_index_ = 0;
  bool hist_valid_ = false;
  uint64_t hist_frame_ = 0;
  bool hist_pause_ = false;
  float hist_sun_[2] = {};
  uint32_t hist_frames_ = 0;
  uint32_t hist_legacy_steps_ = 0;
  noise_resolve::HistoryLifetime hist_life_;   // how long the two images stay while an upscaler runs (noise_resolve::kHistoryKeepFrames)
  uint64_t hist_used_ = 0;       // frames that averaged over it
  bool hist_logged_ = false;
  uint32_t unused_frames_ = 0;       // frames since a pass was recorded, for giving the images back
  uint32_t images_made_for_[2] = {};
  bool images_failed_logged_ = false;

  // The anchor policy (shafts_anchor.h), the interval's tally and what the log has said once.
  shafts_anchor::Policy policy_;
  shafts_anchor::Tally interval_;
  uint32_t last_level_logged_ = ~0u;
  uint32_t missing_copy_logged_ = 0;
  bool first_run_logged_ = false;
  uint32_t census_frames_logged_ = 0;
  // The totals: composites by anchor, sky frames lost, the frames the sun gave no shafts.
  uint64_t runs_ = 0;
  uint64_t runs_end_ = 0;
  uint64_t runs_mid_ = 0;
  uint64_t runs_world_ = 0;  // passes in the menu worlds (their own count: not the anchor policy's)
  uint64_t triggers_ = 0;
  uint64_t skipped_no_camera_ = 0;
  uint64_t gated_ = 0;
  uint64_t lost_ = 0;
  uint32_t census_end_logged_ = 0;
};

}  // namespace mcla::gpu
