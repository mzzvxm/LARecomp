#pragma once
// ===========================================================================
// MCLA GPU renderer -- the ray traced fallback of the screen-space reflections: the host's pure rules (CITY-RT package R4 of perf/CITY-RT-M2.md section 6 and 8)
// ===========================================================================
// ssr_rt.cpp asks here what the hybrid does in a frame (whether it runs, at which level and cell, how many rays it may spend, what the governor says), how the push block and the buffers are laid out
// and what the census says; ssr_rt_rules_test.cpp checks it all and compiles hlsl/ssr_rt_common.hlsli ITSELF as C++ against the twins below. The numbers of the push block, the buffers and the
// shaders' constants are the ones of hlsl/ssr_rt_common.hlsli (the test compares them).
//
// THE HYBRID (the design is perf/CITY-RT-M2.md R4; ssr.h has the effect's text). The screen-space reflections (ssr.h) see only what is in the picture. Where they fail -- the reflected thing is off the
// picture, behind a foreground object, or the ray points back at the camera (a shop window's mirror ray goes behind the camera) -- the pixel's reflection is the game's own (the road's planar view, the
// environment map), which is blurred and does not show the street. The fallback traces THE SAME GGX ray into the city's structures (the scene service's top level, city_rt_scene.h) and shades the hit
// with the city hit library (hlsl/city_hit_shade.hlsli ResolveHit), for the pixels the picture could not speak for, within a cap, and adds it to the trace image where the temporal and the apply
// passes take it unchanged. The passes of a frame (ssr.cpp OnTrigger):
//     1 trace      (graphics, hlsl/ssr_trace_ps.hlsl with SSR_RT 1) the trace and the confidence target (R8G8_UNORM): R packs the group and first ray's residual share, G holds mean confidence
//     2 select     (compute, hlsl/ssr_rt_select_cs.hlsl) the tile list: the candidates of the groups the level takes, with the confidence under the threshold, thinned to the cap, listed tile by tile
//     3 trace      (compute, hlsl/ssr_rt_trace_cs.hlsl) one thread a listed pixel: the same ray, the top level, the hit library; the result added into the trace image (a storage image)
//     4 temporal, apply   unchanged
//
// WHEN IT RUNS. Resolve() at the frame's start: REFLECTIONS ON SURFACES must be on (mcla_gpu_ssr high or ultra, or auto that comes to it), the ray tracing rung of the card (rt_tier.h) must not be
// none, and the debug setting mcla_gpu_ssr_rt (auto, off, high, ultra) must not be off. `auto` follows the effect's own level on a STRONG card and is off on a weak one (a weak card gets it by hand, with
// the Mid cell); a card with no ray tracing (tier none, the Low class) never runs it and the effect is exactly what it was (the same shaders, the same images, the same passes: the identity). Decide()
// at the trigger adds what the frame has: the material plane (not the stub), the pipelines, a top level in this frame. The reason is logged when it changes.
//
// THE CELLS (CITY-RT-M2 section 8; first guesses until the cost pass): classes, the ray cap a frame, the rays a pixel, the reach, and the GPU time the governor holds the passes to.
//     HIGH   glass, metal, water       0.25 M rays   1 ray   150 m   0.8 ms
//     ULTRA  + road, sidewalk, tunnels 1.0 M rays    2 rays  300 m   1.8 ms
//     MID    (a weak card by hand)  glass, metal, water   0.1 M rays   1 ray   60 m   1.0 ms     (the design's table says "off" for the Mid class; a hand-set row on a weak card gets this rather than nothing)
//
// THE GOVERNOR (the weak PC's safety net, ships with R4). The two passes' GPU time (two timestamps round them: ssr_rt.cpp) feeds an EWMA; above the cell's budget (at two decisions a quarter second apart: a spike is not a load) the cap drops one step (half) and holds
// for two seconds; once the time at the step below projected from this one (twice) is under 90% of the budget and the hold is over, it grows back one step. At most three steps (an eighth).
//
// Header only; includes the tables of rt_tier.h and the levels of ssr_rules.h / city_rt_rules.h, nothing of the renderer.
// ===========================================================================

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

#include "city_rt_rules.h"
#include "city_texture_scope.h"
#include "gbuffer_types.h"
#include "rt_scene_rules.h"
#include "rt_tier.h"
#include "ssr_rules.h"

namespace mcla::gpu::ssr_rt {

using Level = ssr::Level;   // off, high, ultra: the same words as the effect's own
namespace gc = gpu_class;

// ---------------------------------------------------------------------------
// the class groups and the confidence target
// ---------------------------------------------------------------------------

inline constexpr uint32_t kGroupNone = 0;    // nothing was traced, or a class no level takes
inline constexpr uint32_t kGroupBright = 1;  // glass, metal, water
inline constexpr uint32_t kGroupGround = 2;  // road, wet road, sidewalk, the tiles the table flags (tunnels)
inline constexpr uint32_t kGroupCount = 3;
constexpr uint32_t GroupBit(uint32_t group) { return 1u << group; }

constexpr uint32_t GroupOf(gbuffer::SurfaceClass cls) {
  switch (cls) {
    case gbuffer::SurfaceClass::kGlass:
    case gbuffer::SurfaceClass::kMetal:
    case gbuffer::SurfaceClass::kWater:
      return kGroupBright;
    case gbuffer::SurfaceClass::kRoad:
    case gbuffer::SurfaceClass::kWetRoad:
    case gbuffer::SurfaceClass::kSidewalk:
    case gbuffer::SurfaceClass::kWorld:
      return kGroupGround;
    default:
      return kGroupNone;
  }
}
constexpr const char* GroupName(uint32_t group) {
  return group == kGroupBright ? "glass, metal, water" : (group == kGroupGround ? "road, sidewalk, tunnels" : "none");
}

// The confidence target is R8G8_UNORM at the SSR images' size: 2 bytes a pixel.
inline constexpr uint32_t kConfidenceBytes = 2;
constexpr uint64_t ConfidenceImageBytes(uint32_t w, uint32_t h) { return uint64_t(w) * h * kConfidenceBytes; }
// The share of the pixel's reflection that is the picture's under which it is a candidate.
inline constexpr float kConfidenceThreshold = 0.97f;
// A pixel whose smoothness ramp (hlsl SsrSmoothRamp) is under this is no candidate: the apply takes under a tenth of the term.
inline constexpr float kMinRamp = 0.1f;

constexpr bool Candidate(uint32_t group, float confidence, uint32_t group_mask, float threshold) {
  return group != kGroupNone && ((group_mask >> group) & 1u) != 0u && confidence < threshold;
}

// ---------------------------------------------------------------------------
// the levels and the cells
// ---------------------------------------------------------------------------

enum class Choice : uint8_t { kAuto, kOff, kHigh, kUltra };
constexpr Choice ParseChoice(std::string_view text) {
  if (text == "off") return Choice::kOff;
  if (text == "high") return Choice::kHigh;
  if (text == "ultra") return Choice::kUltra;
  return Choice::kAuto;   // `auto`, and a word that is none of them (a hand-edited file)
}

struct Cell {
  uint32_t group_mask = 0;   // GroupBit of every group traced
  uint32_t ray_cap = 0;      // rays a frame at the full step
  uint32_t rays = 0;         // rays a pixel (the trace's own: 1 at high, 2 at ultra)
  float reach = 0.0f;        // metres: the longest ray
  float budget_ms = 0.0f;    // the GPU time the governor holds the two passes to
};
inline constexpr Cell kHighCell = {GroupBit(kGroupBright) | GroupBit(kGroupGround), 250000, 1, 150.0f, 0.40f};
inline constexpr Cell kUltraCell = {GroupBit(kGroupBright) | GroupBit(kGroupGround), 1000000, 2, 300.0f, 0.50f};
inline constexpr Cell kMidCell = {GroupBit(kGroupBright), 100000, 1, 60.0f, 0.30f};

// The cell of a level on a card of this rung (the ray tracing tier gated by the class: rt_tier.h RtRung): a strong card has the level's own, a weak one the Mid cell whatever level it was given by hand.
constexpr Cell CellFor(Level level, gc::RtTier rung) {
  if (level == Level::kOff || rung == gc::RtTier::kNone) return Cell{};
  if (rung == gc::RtTier::kWeak) return kMidCell;
  return level == Level::kUltra ? kUltraCell : kHighCell;
}

// Size the list for one pixel per capped ray (4 bytes an entry: x | y << 16).
inline constexpr uint32_t kMaxListEntries = kUltraCell.ray_cap;  // one entry per ray when tracing one ray per pixel
inline constexpr uint64_t kListBytes = uint64_t(kMaxListEntries) * 4;
static_assert(kHighCell.ray_cap <= kMaxListEntries && kMidCell.ray_cap <= kMaxListEntries,
              "the list holds the largest cell's pixels");
inline constexpr uint32_t kThreads = 64;   // the trace pass's group
inline constexpr uint32_t kTile = 8;       // the select pass's tile
constexpr uint32_t GroupsFor(uint32_t capacity) { return (capacity + kThreads - 1) / kThreads; }
static_assert(GroupsFor(kMaxListEntries) <= 65535, "a dispatch's group count is under 65536");

// The most rays a frame may trace at governor step `step` (an eighth at most down): never fewer than kMinRays (a frame that traces a few is not worth its dispatches, but never a cap of nothing).
inline constexpr uint32_t kMinRays = 8192;
constexpr uint32_t RayCap(const Cell& cell, uint32_t step) {
  if (cell.ray_cap == 0) return 0;
  const uint32_t stepped = cell.ray_cap >> (step > 3 ? 3 : step);
  return stepped < kMinRays ? (cell.ray_cap < kMinRays ? cell.ray_cap : kMinRays) : stepped;
}
// The list's capacity in pixels is the capped ray count divided by rays per pixel. Single-ray mode samples one
// of the existing screen rays, so the compute shader still uses the screen's ray count to regenerate the direction.
constexpr uint32_t RaysUsed(const Cell& cell, uint32_t rays_walked, bool single_ray = false) {
  return single_ray ? std::min(1u, std::min(cell.rays, rays_walked)) : std::min(cell.rays, rays_walked);
}
constexpr uint32_t CapacityFor(const Cell& cell, uint32_t step, uint32_t rays) { return rays == 0 ? 0 : RayCap(cell, step) / rays; }
constexpr uint32_t Capacity(const Cell& cell, uint32_t step) { return CapacityFor(cell, step, cell.rays); }

constexpr bool CarHitsEnabled(const Cell& cell, bool enabled) {
  return enabled && (cell.group_mask & GroupBit(kGroupGround)) != 0;
}

// Ask for the car scene when road receivers can shade cars, including when the car reflection passes are off.
constexpr city_rt::Level CityLevelOf(Level level) { return level == Level::kUltra ? city_rt::Level::kUltra : (level == Level::kHigh ? city_rt::Level::kHigh : city_rt::Level::kOff); }
constexpr rt_scene::Needs NeedsFor(Level level, bool cars = false) {
  rt_scene::Needs n;
  n.cars = level != Level::kOff && cars;
  n.city = CityLevelOf(level);
  n.hit = CityLevelOf(level);
  n.textures = level == Level::kOff ? TextureScope::kNone : TextureScope::kAll;
  return n;
}

// ---------------------------------------------------------------------------
// the setting's decision (a frame's start)
// ---------------------------------------------------------------------------

enum class Why : uint8_t {
  kRuns,         // the fallback runs
  kSsrOff,       // REFLECTIONS ON SURFACES is off
  kNoRt,         // the card does not trace rays (tier none) or the class is Low
  kSetting,      // mcla_gpu_ssr_rt is off
  kWeakAuto,     // auto on a weak card: by hand only
  kStub,         // the frame has no material plane (the stub class stands in)
  kNoScene,      // no top level in this frame (nothing built yet, no city, no camera)
  kNoPipelines,  // a shader or a pipeline is not made yet (a frame never waits for a compile) or could not be
  kNoImages,     // the confidence image could not be made
  kCount,
};
constexpr const char* WhyName(Why why) {
  switch (why) {
    case Why::kRuns: return "runs";
    case Why::kSsrOff: return "REFLECTIONS ON SURFACES is off";
    case Why::kNoRt: return "this card or class does not trace rays";
    case Why::kSetting: return "mcla_gpu_ssr_rt is off";
    case Why::kWeakAuto: return "auto is off on a weak ray tracing card (mcla_gpu_ssr_rt high or ultra turns it on)";
    case Why::kStub: return "the frame has no material plane (RT3): the stub class stands in";
    case Why::kNoScene: return "no ray tracing top level in this frame";
    case Why::kNoPipelines: return "a pass's pipeline is not ready";
    case Why::kNoImages: return "the confidence image could not be made";
    case Why::kCount: break;
  }
  return "?";
}

struct LevelFacts {
  Level ssr = Level::kOff;                  // the effect's own level this frame (mcla_gpu_ssr resolved)
  Choice choice = Choice::kAuto;            // mcla_gpu_ssr_rt
  gc::RtTier rung = gc::RtTier::kNone;      // rt_tier.h: the ray tracing tier gated by the class
};
struct Resolved {
  Level level = Level::kOff;
  Cell cell{};
  Why why = Why::kSsrOff;
};
constexpr Resolved Resolve(const LevelFacts& f) {
  Resolved r;
  if (f.ssr == Level::kOff) {
    r.why = Why::kSsrOff;
    return r;
  }
  if (f.rung == gc::RtTier::kNone) {
    r.why = Why::kNoRt;
    return r;
  }
  if (f.choice == Choice::kOff) {
    r.why = Why::kSetting;
    return r;
  }
  Level level = f.ssr;                                // auto follows the effect's level ...
  if (f.choice == Choice::kHigh) level = Level::kHigh;
  if (f.choice == Choice::kUltra) level = Level::kUltra;
  if (f.choice == Choice::kAuto && f.rung != gc::RtTier::kStrong) {
    r.why = Why::kWeakAuto;                           // ... on a strong card only
    return r;
  }
  r.level = level;
  r.cell = CellFor(level, f.rung);
  r.why = Why::kRuns;
  return r;
}

// Physical ray-query support decides availability. Every supported
// device uses the selected level's same group mask, ray count and reach.
constexpr Resolved ResolveFixed(Level ssr, Choice choice, bool ray_queries) {
  return Resolve({ssr, choice, ray_queries ? gc::RtTier::kStrong : gc::RtTier::kNone});
}

// What the trigger knows beyond the setting.
struct FrameFacts {
  Resolved resolved;
  bool stub = false;
  bool pipelines_ready = false;   // the confidence trace, the select pass and the trace pass (for this MSAA level)
  bool scene_valid = false;       // CarRtView::scene_valid and its set
  bool images_ok = false;
};
constexpr Why DecideFrame(const FrameFacts& f) {
  if (f.resolved.why != Why::kRuns) return f.resolved.why;
  if (f.stub) return Why::kStub;
  if (!f.pipelines_ready) return Why::kNoPipelines;
  if (!f.images_ok) return Why::kNoImages;
  if (!f.scene_valid) return Why::kNoScene;
  return Why::kRuns;
}

// ---------------------------------------------------------------------------
// the push block and the buffers (hlsl/ssr_rt_common.hlsli)
// ---------------------------------------------------------------------------

namespace push {
inline constexpr uint32_t kConf = 4;         // SSR_W_RT_CONF
inline constexpr uint32_t kCap = 5;          // SSR_W_RT_CAP
inline constexpr uint32_t kReach = 18;       // SSR_W_RT_REACH
inline constexpr uint32_t kThreshold = 19;   // SSR_W_RT_THRESHOLD
inline constexpr uint32_t kFrame = 30;       // SSR_W_RT_FRAME
inline constexpr uint32_t kMaskShift = 16;
inline constexpr uint32_t kRaysShift = 20;
inline constexpr uint32_t kFlagsShift = 24;
inline constexpr uint32_t kFlagStats = 1;
inline constexpr uint32_t kFlagReset = 2;
inline constexpr uint32_t kFlagScreen = 4;
inline constexpr uint32_t kFlagSingleRay = 8;
inline constexpr uint32_t kFlagSkyMiss = 16;
inline constexpr uint32_t kFlagScreenProxy = 32;
inline constexpr uint32_t kFlagProbeMisses = 64;
inline constexpr uint32_t kFlagCars = 128;
}  // namespace push
// The conf word: the confidence image's slot (16 bits), the groups the level traces (4), the rays a pixel less one (2), the flags (8).
constexpr uint32_t PackConf(uint32_t slot, uint32_t group_mask, uint32_t rays, uint32_t flags) {
  return ssr::Slot16(slot) | ((group_mask & 15u) << push::kMaskShift) | (((rays < 1 ? 1u : (rays > 4 ? 4u : rays)) - 1u) << push::kRaysShift) | ((flags & 255u) << push::kFlagsShift);
}

namespace buf {
inline constexpr uint32_t kCtlTotal = 0;      // 3 words, a frame each in rotation
inline constexpr uint32_t kCtlListed = 3;     // 3 words
inline constexpr uint32_t kCtlWords = 8;
inline constexpr uint32_t kStatCal = 8;       // rt_hit_calibration.h kStatsWords words
inline constexpr uint32_t kStatCensus = 56;
inline constexpr uint32_t kCensusWords = 17;
inline constexpr uint32_t kStatWords = 73;
inline constexpr uint64_t kBytes = uint64_t(kStatWords) * 4;
// the census words (offsets from kStatCensus)
inline constexpr uint32_t kRays = 0;
inline constexpr uint32_t kCity = 1;
inline constexpr uint32_t kCar = 2;
inline constexpr uint32_t kMiss = 3;
inline constexpr uint32_t kNoData = 4;
inline constexpr uint32_t kScreen = 5;
inline constexpr uint32_t kShaded = 6;
inline constexpr uint32_t kFilled = 7;
inline constexpr uint32_t kMissProbed = 8;
inline constexpr uint32_t kMissBeyond = 9;
inline constexpr uint32_t kMissOpenUp = 10;
inline constexpr uint32_t kNoDataLod = 11;
inline constexpr uint32_t kNoDataDefault = 12;
inline constexpr uint32_t kNoDataMissing = 13;
inline constexpr uint32_t kProxyRecovered = 14;
inline constexpr uint32_t kEnvMiss = 15;
inline constexpr uint32_t kCarShaded = 16;
}  // namespace buf
static_assert(buf::kStatCensus + buf::kCensusWords <= buf::kStatWords && buf::kStatCal + 48 == buf::kStatCensus && buf::kCtlWords == buf::kStatCal, "the control buffer's regions follow one another");

// ---------------------------------------------------------------------------
// the numbers the shaders have (twins of hlsl/ssr_rt_common.hlsli; the test compiles that file and compares)
// ---------------------------------------------------------------------------

inline constexpr float kMinKeep = 0.25f;
inline constexpr uint32_t kTileStride = 7919;
inline constexpr float kFadeStart = 0.75f;
inline constexpr float kLiftFloor = 0.006f;
inline constexpr float kLiftPixels = 3.0f;
inline constexpr float kLiftCap = 0.4f;

// The share of the candidates the thinning keeps: the capacity over the candidates the last frame counted (none counted: all).
inline float Keep(uint32_t capacity, uint32_t previous_total) {
  const float share = float(capacity) / float(std::max<uint32_t>(previous_total, 1));
  return std::clamp(share, kMinKeep, 1.0f);
}
inline float Weight(float keep) { return 1.0f / std::max(keep, kMinKeep); }
constexpr uint32_t TileOf(uint32_t group_x, uint32_t group_y, uint32_t tiles_x, uint32_t tile_count, uint32_t rt_frame) {
  const uint32_t count = tile_count < 1 ? 1u : tile_count;
  return (group_y * tiles_x + group_x + (rt_frame % count) * kTileStride) % count;
}
inline float Lift(float t, float footprint_unit) { return std::min(kLiftFloor + kLiftPixels * footprint_unit * t, kLiftCap); }
inline float ReachFade(float t, float reach) {
  const float start = kFadeStart * reach;
  const float s = std::clamp((t - start) / std::max(reach - start, 1.0e-6f), 0.0f, 1.0f);
  return 1.0f - s * s * (3.0f - 2.0f * s);
}
inline float Footprint(float pixel_angle, float receiver_distance, float t, float cone_tan) { return pixel_angle * (receiver_distance + t) + 2.0f * cone_tan * t; }
// The term in the trace image after the fallback (one channel): the trace's own plus the share the picture could not speak for of the traced term.
inline float Blend(float trace, float confidence, float term, float weight) { return trace + term * ((1.0f - std::clamp(confidence, 0.0f, 1.0f)) * weight); }

// ---------------------------------------------------------------------------
// the governor
// ---------------------------------------------------------------------------

class Governor {
 public:
  static constexpr uint32_t kMaxStep = 3;          // an eighth of the cap
  static constexpr double kEwmaWeight = 0.08;      // a sample's weight in the average (a frame at 60 FPS: about a quarter of a second to follow a change)
  static constexpr double kHoldSeconds = 2.0;      // after a change nothing grows back for this long
  static constexpr double kDecideEvery = 0.25;     // seconds between decisions
  static constexpr uint32_t kMinSamples = 8;       // samples since the last change before it decides again
  static constexpr double kRegrowShare = 0.9;      // the cost at the step below, projected, must stay under this share of the budget
  static constexpr uint32_t kOverDecisions = 2;    // decisions in a row over the budget before the cap drops (a single spike decays within one: it is not a load)

  void Reset() { *this = Governor{}; }
  // A pass time (milliseconds of GPU time: select and trace together) measured for a frame that ran them.
  void Note(double ms) {
    if (!(ms >= 0.0) || !std::isfinite(ms)) return;
    ewma_ = have_ ? ewma_ + (ms - ewma_) * kEwmaWeight : ms;
    have_ = true;
    if (samples_ < 1000000) ++samples_;
  }
  // Decides at time `now` (seconds, any origin) against `budget_ms`. True when the step changed.
  bool Update(double now, double budget_ms) {
    if (!have_ || !(budget_ms > 0.0) || samples_ < kMinSamples || now - last_decide_ < kDecideEvery) return false;
    last_decide_ = now;
    over_ = ewma_ > budget_ms ? over_ + 1 : 0;
    if (over_ >= kOverDecisions && step_ < kMaxStep) {
      ++step_;
      Changed(now, 0.5);
      return true;
    }
    if (step_ > 0 && now - last_change_ >= kHoldSeconds && ewma_ * 2.0 < budget_ms * kRegrowShare) {
      --step_;
      Changed(now, 2.0);
      return true;
    }
    return false;
  }
  uint32_t step() const { return step_; }
  double ewma_ms() const { return have_ ? ewma_ : 0.0; }
  bool has_samples() const { return have_; }
  uint32_t changes() const { return changes_; }

 private:
  void Changed(double now, double scale) {
    last_change_ = now;
    ewma_ *= scale;   // the cost follows the rays: the average is carried over to the new step, not waited for
    samples_ = 0;
    over_ = 0;
    ++changes_;
  }
  uint32_t step_ = 0;
  double ewma_ = 0.0;
  bool have_ = false;
  uint32_t samples_ = 0;
  uint32_t over_ = 0;
  double last_change_ = -1.0e9;
  double last_decide_ = -1.0e9;
  uint32_t changes_ = 0;
};

// ---------------------------------------------------------------------------
// the counters and the census
// ---------------------------------------------------------------------------

// The 32 bit counters the GPU never clears: the interval's words are the difference to the last readback, modulo 2^32.
inline void Delta(const uint32_t* now, const uint32_t* last, uint32_t words, uint32_t* out) {
  for (uint32_t i = 0; i < words; ++i) out[i] = now[i] - last[i];
}

// What one frame's census line says (the readback of a stats frame: the counts are that frame's alone).
struct FrameLine {
  uint64_t frame = 0;
  Level level = Level::kOff;
  uint32_t group_mask = 0;
  float reach = 0.0f;
  uint32_t capacity = 0;      // the list's capacity in pixels at the governor's step
  uint32_t step = 0;
  uint32_t rays_per_pixel = 0;
  uint32_t candidates = 0;    // counted before the cap (the stats frame's own)
  uint32_t listed = 0;
  float keep = 1.0f;
  uint32_t rays = 0, city = 0, car = 0, miss = 0, no_data = 0, screen = 0, shaded = 0, filled = 0;
  double ms = -1.0;           // the passes' GPU time (the average), negative: no timing
  float budget_ms = 0.0f;
};
// "[ssr rt] frame 1200: level ultra, groups glass+metal+water+road+sidewalk+tunnels, reach 300 m, capacity 500000 px at step 0 (2 rays), candidates 120400 listed 120400 keep 1.00, rays 240800 city 91000 car 2100 miss
// 147700 no data 0 screen 30000 shaded 61000 filled 83000, 0.42 ms (budget 1.80)". The census line a chain reads.
inline std::string FormatFrameLine(const FrameLine& f) {
  std::string groups;
  for (uint32_t g = 1; g < kGroupCount; ++g) {
    if ((f.group_mask & GroupBit(g)) == 0) continue;
    if (!groups.empty()) groups += "+";
    groups += g == kGroupBright ? "glass+metal+water" : "road+sidewalk+tunnels";
  }
  if (groups.empty()) groups = "none";
  char text[640];
  char timing[64];
  if (f.ms >= 0.0) {
    std::snprintf(timing, sizeof(timing), "%.2f ms (budget %.2f)", f.ms, double(f.budget_ms));
  } else {
    std::snprintf(timing, sizeof(timing), "no timing (budget %.2f)", double(f.budget_ms));
  }
  std::snprintf(text, sizeof(text),
                "[ssr rt] frame %llu: level %s, groups %s, reach %.0f m, capacity %u px at step %u (%u ray%s), candidates %u listed %u keep %.2f, rays %u city %u car %u miss %u no data %u screen %u shaded %u filled %u, %s",
                static_cast<unsigned long long>(f.frame), std::string(ssr::LevelName(f.level)).c_str(), groups.c_str(), double(f.reach), f.capacity, f.step, f.rays_per_pixel, f.rays_per_pixel == 1 ? "" : "s",
                f.candidates, f.listed, double(f.keep), f.rays, f.city, f.car, f.miss, f.no_data, f.screen, f.shaded, f.filled, timing);
  return text;
}

// The totals the census text (the renderer's "; ssr rt: ..." part) carries.
struct Totals {
  uint64_t frames_run = 0;
  uint64_t frames_not_run = 0;    // frames the fallback was wanted and did not run (the reason is the last one)
  uint64_t stats_frames = 0;
  uint64_t candidates = 0, listed = 0, rays = 0, city = 0, car = 0, miss = 0, no_data = 0, screen = 0, shaded = 0, filled = 0;
  uint32_t step = 0;
  uint32_t step_changes = 0;
  double ms = -1.0;
  Why last_why = Why::kSsrOff;
};
inline std::string FormatTotals(const Totals& t) {
  if (t.frames_run == 0 && t.frames_not_run == 0) return {};
  char text[512];
  char timing[48];
  if (t.ms >= 0.0) {
    std::snprintf(timing, sizeof(timing), "%.2f ms", t.ms);
  } else {
    std::snprintf(timing, sizeof(timing), "no timing");
  }
  std::snprintf(text, sizeof(text),
                "; ssr rt: %llu frames ran, %llu wanted and did not (last: %s), %llu stats frames (candidates %llu, rays %llu, city hits %llu, car hits %llu, misses %llu, no data %llu, screen %llu, shaded %llu, filled %llu), governor step %u (%u changes), %s",
                static_cast<unsigned long long>(t.frames_run), static_cast<unsigned long long>(t.frames_not_run), WhyName(t.last_why), static_cast<unsigned long long>(t.stats_frames),
                static_cast<unsigned long long>(t.candidates), static_cast<unsigned long long>(t.rays), static_cast<unsigned long long>(t.city), static_cast<unsigned long long>(t.car),
                static_cast<unsigned long long>(t.miss), static_cast<unsigned long long>(t.no_data), static_cast<unsigned long long>(t.screen), static_cast<unsigned long long>(t.shaded),
                static_cast<unsigned long long>(t.filled), t.step, t.step_changes, timing);
  return text;
}

// The memory the fallback holds: the confidence image at the SSR size, the list and the control words (the trace image's storage flags cost nothing more).
constexpr uint64_t HeldBytes(uint32_t w, uint32_t h) { return ConfidenceImageBytes(w, h) + kListBytes + buf::kBytes; }

}  // namespace mcla::gpu::ssr_rt
