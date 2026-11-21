#pragma once
// ===========================================================================
// MCLA GPU renderer -- the probe field's PER-PIXEL DETAIL PASS: the rules and the twin of the shader's maths (CITY-RT M3, package G5 of perf/CITY-RT-M3.md sections 4.3, 5, 6, 10)
// ===========================================================================
// WHAT IT IS. At ULTRA on a strong ray tracing card (gi_frame.h PixelPassAllowed: the extended layout) the probe field (2 m probes, a 2 s memory) gets a detail layer: one cosine weighted ray a pixel
// at the occlusion's TRACED size (half the picture's above 1080 lines), from the G-buffer's position and normal, 12 m long, into the city and the cars. What it adds that the field cannot carry:
// contact scale below the probes' spacing, the cars as occluders, and the HEADLIGHTS' bounce (they move 36 m/s against a 2 s field: the light manager's surface lights are added at the hit, no shadow ray).
//
//   THE RAY     a pixel with a normal, an albedo, a lit weight and a depth (the apply's own conditions, gi_apply_rules.h) that is not a vehicle's: the direction is cosine weighted about the normal,
//               from a per-pixel offset that steps along the R2 low discrepancy sequence each frame (so successive frames sample new directions and the temporal stage converges to the integral), and it
//               goes at most kReachM (THE 12 m RULE) from the surface. Mask: the city (0x80) and the cars' parts (0x7F).
//   THE HIT     the city library's GI mode (hlsl/city_hit_shade.hlsli CityHitShadeHit, the SAME call the probes' rays make, so the two estimates are of one thing): the hit's average colour times the
//               frame's ambient, the sun by N.L with a shadow ray, ONE of the nearest lamps by the world frame's grid with a shadow ray (R7), the glow of what glows, the headlights of the frame's
//               surface lights (CITY_HIT_AMBIENT: no shadow ray), and the field one frame old as the second bounce (bounce weight, as the probes'). A car's hit is a neutral diffuse (kCarAlbedo) in the
//               frame's light: an occluder, not yet a painted emitter. The picture's colour (ResolveHit's screen term) is NOT used: this pass runs at the apply, before the light pass and before colour_post.
//   A MISS      a ray that finds nothing within the reach carries the FIELD's irradiance at the pixel (kMissShare): the long range is the field's, so a pixel with nothing near it is the field's value
//               exactly (the pixel term is the field's where it has nothing to say) and a pixel with geometry near it is a mix of the near hits and the field. The estimate's expectation counts the
//               near geometry twice at most by p_hit x p_miss of it (kMissShare lowers it): CHECK G5-b.
//   THE DENOISER the shared RtDenoise (rt_denoise.h, its DIFFUSE profile over the PLAIN guides): the temporal stage (the last frame's result by the camera's motion, a variance box, a length) then up to three
//               a-trous iterations (steps 1, 2, 4; a fixed 6 pixel radius that shortens as the history grows). Guides: depth, view normal (roughness 1), hit distance and share.
//   THE MIX     the apply (gi_apply_rules.h) takes E = lerp(E_field, E_pixel, w) with w = Weight(the history's length): 0 on a fresh or disoccluded pixel, 1 from kFullLength frames on. A pixel the pass
//               did not trace (a vehicle, no albedo, the sky, no field) has no guide (depth 0) and keeps the field's value.
//   THE RAW MODE (G6, CITY-RT G6: registered) in a frame DLSS Ray Reconstruction takes (Facts::raw: temporal.h RawModes().RtGi()): the denoiser is skipped (Plan::denoise false: RR is the denoiser,
//               filtering twice is blur), the apply reads the 1 spp SIGNAL image itself and mixes it into the field's irradiance at the weight kRawWeight (1) wherever the pass traced (the history length the
//               filter's weight is made of does not exist: RR keeps the history), and the hit distance image (RawHitDistance: 0 not traced, kMissDistance for a miss, the ray's length for a hit) goes to the guide
//               pass as the LAST choice of its specular hit distance input (GiPixel::raw_output() hands both out; rr_guides_rules.h WithBounce). rr::kConsumers' kRtGi has raw_mode_built = true (AUTO does not
//               trust RR with it before the RR-6 A/B) and GiApply::BeginFrame calls NoteConsumer. The switch mcla_gpu_dlss_rr_rt_gi is the A/B: 0 ours, 1 raw with the hit distance, 2 raw without it.
//   IDENTITY    nothing is made, recorded or bound unless the extended layout stands (rules below): HIGH, OFF, weak cards and every card without the field are bit for bit as before.
//
// THIS FILE is the specification and the twin of hlsl/gi_pixel_common.hlsli (gi_pixel_test.cpp compiles the .hlsli as C++ and compares each function with the one here). It includes nothing of Plume or
// ReXGlue. Threading: render thread only (plain data and pure functions).
//
// CHECK (nothing here was run against the game):
//   G5-a  the reach: the doc names "the 12 m rule" without defining it; it is read here as the ray's length (a hit farther than 12 m is the field's). If it meant another number, kReachM is it.
//   G5-b  the miss term: the field's value stands in for a ray that finds nothing; in a canyon the opposite wall (20 m) is the field's alone, near walls and the road are the rays'. The overshoot where both
//         are near is bounded (p_hit x p_miss of the near light); the canyon scene's A/B against the field alone decides kMissShare.
//   G5-c  the picture's colour (ResolveHit's screen term) is not read: this pass is before colour_post and the light pass; the previous frame's colour_post would need the camera's motion. NEEDS: if the
//         owner wants "seen colour where it is seen" the apply's pass order has to move the pixel pass after CapturePost (one frame of lag for the bounce) or read history_colour_slot.
//   G5-d  a car's hit is a grey (kCarAlbedo) lit by the frame's ambient and the sun without a shadow ray; no paint colour, no glow of its lamps (the car scene's instance table is the way in).
//   G5-e  the headlights' units: LightFrame colours are shader output units (the paint's, no exposure), so the term is added at units 1 like the field's; the deferred light pass multiplies by night,
//         height and gain calibrations (local_lights_rules.h): a mismatch that depends on the hour is those.
//   G5-f  the timestamps: two a frame slot around the trace and the denoise (GpuTiming's rule); without them the governor stays at the full size.
// ===========================================================================

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

#include "gi_frame.h"
#include "light_frame.h"
#include "rt_denoise_rules.h"

namespace mcla::gpu::gi_pixel {

inline constexpr uint32_t kFormatVersion = 1;
inline constexpr uint32_t kNoSlot = gi::kNoSlot;

// ---------------------------------------------------------------------------
// the numbers (the .hlsli's #defines are these: gi_pixel_test.cpp compares them)
// ---------------------------------------------------------------------------

inline constexpr float kReachM = 12.0f;             // THE 12 m RULE (CHECK G5-a): the ray's length; farther is the field's
inline constexpr float kMaxRadiance = 32.0f;        // a ray carries at most this (the probes' record's, gi_update_rules.h kMaxRadiance): a firefly's guard (neon, a sun glint)
inline constexpr float kCarAlbedo = 0.12f;          // a car's hit: a neutral diffuse (CHECK G5-d)
inline constexpr float kMissShare = 1.0f;           // the share of the field's irradiance a ray with no hit carries (CHECK G5-b)
inline constexpr float kHeadlightGain = 1.0f;       // the surface lights' term at a hit (CHECK G5-e)
inline constexpr uint32_t kLightsMax = 6;           // the surface lights the pass reads at a hit (two headlamps of three cars)
inline constexpr float kFullLength = 4.0f;          // the denoiser's history length (frames) at which the pixel term replaces the field in full
inline constexpr float kMaxWeight = 1.0f;           // the most the pixel term replaces of the field (1: all)
inline constexpr float kRawWeight = 1.0f;           // a RAW frame's weight of the 1 spp pixel term where the pass traced (Ray Reconstruction is the history and the filter: nothing to wait for)
inline constexpr float kMissDistance = 4096.0f;     // the raw mode's hit distance of a ray that found nothing (dlss_rr_rules.h kRawMissDistance, ssr_rules.h)
inline constexpr float kCosineFloor = 0.02f;        // a ray is kept at least this far (cosine) above the surface's tangent plane
inline constexpr float kR2A1 = 0.7548776662466927f; // the R2 sequence's two steps (Roberts): the per-pixel offset advances by them each frame
inline constexpr float kR2A2 = 0.5698402909980532f;
inline constexpr float kTwoPi = 6.283185307179586f;

// ---------------------------------------------------------------------------
// the cell: what each (ray tracing rung, level) runs, and what it is estimated to cost (COST PASS: every number is an estimate x2 uncertain)
// ---------------------------------------------------------------------------

struct Cell {
  bool runs = false;
  uint32_t rays_per_pixel = 0;       // 1: one cosine ray a pixel at the pass's size
  float reach_m = 0.0f;
  uint32_t denoise_iterations = 0;   // a-trous iterations at most (steps 1, 2, 4)
  uint32_t lights = 0;               // surface lights read at a hit
  float cap_ms = 0.0f;               // the governor's cap on the pass (trace + denoise) at the traced 1080 lines; est. 5080: trace 1.0 to 1.5, denoise 0.3 to 0.6
  float est_ms_lo = 0.0f, est_ms_hi = 0.0f;
};

// The cell of a (rung, level): only the strong rung at ULTRA runs the pass (gi_frame.h LayoutFor / PixelPassAllowed); every other cell is "none" (the identity).
constexpr Cell CellFor(gi::Rung rung, gi::Level level) {
  const gi::Layout layout = gi::LayoutFor(rung, level);
  const gi::Budget& budget = gi::BudgetOf(layout);
  if (!gi::PixelPassAllowed(rung, layout) || budget.pixel_rays_per_pixel == 0) return Cell{};
  return Cell{true, budget.pixel_rays_per_pixel, kReachM, rt_denoise::kMaxIterations, kLightsMax, 2.2f, 1.3f, 2.1f};
}

// What the pass holds in video memory at a size: the signal, the two depth guides (ping-pong: the temporal stage reads the last frame's), the view normal, the hit image, and RtDenoise's own four
// RGBA16F images (two spatial, two history). The result is the doc's "per-pixel images" part of the extended layout's 190 MiB (a 1080 line traced size: 2.07 M pixels).
constexpr uint64_t BytesPerPixel() { return 8 + 4 + 4 + 8 + 4 + 4 * rt_denoise::kBytesPerPixel; }   // 60
constexpr uint64_t ImageBytes(uint32_t width, uint32_t height) { return uint64_t(width) * height * BytesPerPixel(); }

// ---------------------------------------------------------------------------
// the size and the governor
// ---------------------------------------------------------------------------

// The pass runs at the occlusion's traced size divided by this: 1 (the traced size) or 2 (a quarter of the pixels, the doc's "quarter of it"; the apply reads it with a depth and normal weighted
// bilinear). The governor's step is the divisor.
inline constexpr uint32_t kDivisorFull = 1;
inline constexpr uint32_t kDivisorReduced = 2;
constexpr uint32_t DivisorOf(bool reduced) { return reduced ? kDivisorReduced : kDivisorFull; }
constexpr uint32_t PixelExtent(uint32_t traced, uint32_t divisor) { return divisor <= 1 ? traced : (traced + divisor - 1) / divisor; }
// The traced texel a pixel of the pass stands for (its top left one), clamped into the traced image.
constexpr uint32_t TracedTexel(uint32_t pixel, uint32_t divisor, uint32_t traced) { return std::min(pixel * (divisor == 0 ? 1u : divisor), traced > 0 ? traced - 1 : 0u); }

// The step governor: an EWMA of the pass's measured milliseconds against the cell's cap. It goes to the reduced size after kDownFrames frames over kDownFactor x the cap, and back after kUpFrames frames in
// which four times the reduced cost (what the full size would cost) is under kUpFactor x the cap. Hysteresis: a pass near the cap does not flicker between the two.
class Governor {
 public:
  static constexpr float kAlpha = 0.1f;
  static constexpr float kDownFactor = 1.15f;
  static constexpr float kUpFactor = 0.9f;
  static constexpr uint32_t kDownFrames = 30;
  static constexpr uint32_t kUpFrames = 150;
  void Reset() { *this = Governor{}; }
  void AddSample(float ms, float cap_ms) {
    if (!(ms >= 0.0f) || !(cap_ms > 0.0f)) return;
    ewma_ = have_ ? ewma_ + kAlpha * (ms - ewma_) : ms;
    have_ = true;
    if (!reduced_) {
      over_ = ewma_ > kDownFactor * cap_ms ? over_ + 1 : 0;
      if (over_ >= kDownFrames) {
        reduced_ = true;
        over_ = under_ = 0;
        have_ = false;   // the next samples are the reduced size's: the average starts again
      }
    } else {
      under_ = 4.0f * ewma_ < kUpFactor * cap_ms ? under_ + 1 : 0;
      if (under_ >= kUpFrames) {
        reduced_ = false;
        over_ = under_ = 0;
        have_ = false;
      }
    }
  }
  bool reduced() const { return reduced_; }
  uint32_t divisor() const { return DivisorOf(reduced_); }
  float ewma() const { return ewma_; }

 private:
  float ewma_ = 0.0f;
  bool have_ = false;
  bool reduced_ = false;
  uint32_t over_ = 0, under_ = 0;
};

// ---------------------------------------------------------------------------
// the plan: whether the pass runs this frame, and what it does
// ---------------------------------------------------------------------------

// The pass is a CONSUMER of the DENOISER row (CITY-RT G6: rr::kConsumers kRtGi; GiApply::BeginFrame says it every frame) while the frame's layout has a per-pixel pass: the extended layout (a strong ray
// tracing card at ULTRA). The caller adds the rest (the field runs, the pass is made). The pass's own cell is the same word (CellFor).
constexpr bool ConsumerWanted(gi::Layout layout) { return gi::BudgetOf(layout).pixel_rays_per_pixel != 0; }

struct Facts {
  gi::Layout layout = gi::Layout::kNone;   // the field's frame's layout
  bool field_present = false;              // the field's frame has its three textures
  int debug = -1;                          // mcla_gpu_rt_gi_pixel: -1 as the layout says, 0 off, 1 on where the layout allows
  bool scene_ok = false;                   // the car scene's top level of this frame stands (CarRtScene::View)
  bool world_ok = false;                   // the world frame's rows are valid and the light is known
  bool hit_data_ok = false;                // the city hit data's pool has a committed block
  bool planes_ok = false;                  // the G-buffer's normal and albedo planes, the main depth, lin_depth, the camera
  bool pipelines_ready = false;
  bool resources_ok = true;
  bool raw = false;                        // G6: Ray Reconstruction takes the frame and cleans the 1 spp signal itself (GiApply: TemporalSystem::RawModes().RtGi(), false with the plan not RR)
};
enum class Why : uint32_t { kRuns, kNotWanted, kDebugOff, kNoField, kNoScene, kNoWorld, kNoHitData, kNoPlanes, kPipelines, kResources, kCount };
constexpr const char* WhyName(Why why) {
  switch (why) {
    case Why::kRuns: return "runs";
    case Why::kNotWanted: return "the layout has no per-pixel pass";
    case Why::kDebugOff: return "mcla_gpu_rt_gi_pixel is 0";
    case Why::kNoField: return "no field in the frame";
    case Why::kNoScene: return "no top level this frame";
    case Why::kNoWorld: return "no world rows or light";
    case Why::kNoHitData: return "the hit data's pool is empty";
    case Why::kNoPlanes: return "no G-buffer planes, depth, camera or light";
    case Why::kPipelines: return "the pipelines are still being made";
    case Why::kResources: return "a resource could not be made";
    case Why::kCount: break;
  }
  return "?";
}
struct Plan {
  Why why = Why::kNotWanted;
  bool denoise = false;   // RtDenoise's temporal and spatial stages follow the trace (not in a raw frame: Ray Reconstruction is the denoiser)
  bool raw = false;
  bool runs() const { return why == Why::kRuns; }
};
// The first reason that applies is the answer. The layout decides first (identity: no pass, nothing made), the debug switch second (0 turns it off, 1 and -1 leave the layout's say).
// Whether a frame the pass ran is the NEXT frame's history for the denoiser: a plain frame is (RtDenoise wrote its history image in it), a raw one is not (Ray Reconstruction is the denoiser there; the image
// the next plain frame would read is old), so the frame after a raw one starts the denoiser again.
constexpr bool KeepsHistory(bool raw) { return !raw; }
constexpr Plan Decide(const Facts& f) {
  Plan p;
  if (gi::BudgetOf(f.layout).pixel_rays_per_pixel == 0) return p;
  if (f.debug == 0) { p.why = Why::kDebugOff; return p; }
  if (!f.field_present) { p.why = Why::kNoField; return p; }
  if (!f.scene_ok) { p.why = Why::kNoScene; return p; }
  if (!f.world_ok) { p.why = Why::kNoWorld; return p; }
  if (!f.hit_data_ok) { p.why = Why::kNoHitData; return p; }
  if (!f.planes_ok) { p.why = Why::kNoPlanes; return p; }
  if (!f.pipelines_ready) { p.why = Why::kPipelines; return p; }
  if (!f.resources_ok) { p.why = Why::kResources; return p; }
  p.why = Why::kRuns;
  p.raw = f.raw;
  p.denoise = !f.raw;
  return p;
}

// ---------------------------------------------------------------------------
// the ray (the twin of hlsl/gi_pixel_common.hlsli)
// ---------------------------------------------------------------------------

inline uint32_t Hash(uint32_t x) {   // lowbias32 (Wellons), the probes' hash
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}
inline float Unit(uint32_t h) { return float(h >> 8) * (1.0f / 16777216.0f); }
inline float Frac(float v) { return v - std::floor(v); }
// The pixel's two numbers in 0..1 at a frame: a per-pixel offset (a hash of the pixel alone) advanced by the R2 sequence each frame.
inline gi::Vec2 Noise(uint32_t x, uint32_t y, uint32_t frame) {
  const uint32_t h = Hash(x * 0x9E3779B1u + y * 0x85EBCA6Bu + 0x27d4eb2fu);
  const float a = Unit(h), b = Unit(Hash(h ^ 0x68E31DA4u));
  return {Frac(a + float(frame & 0xFFFFu) * kR2A1), Frac(b + float(frame & 0xFFFFu) * kR2A2)};
}
// The orthonormal frame about a unit vector (Duff et al. 2017: the library's CityHitMakeBasis).
struct Basis {
  gi::Vec3 t, b;
};
inline Basis MakeBasis(gi::Vec3 n) {
  const float s = n.z >= 0.0f ? 1.0f : -1.0f;
  const float a = -1.0f / (s + n.z);
  const float c = n.x * n.y * a;
  return {{1.0f + s * n.x * n.x * a, s * c, -s * n.x}, {c, s + n.y * n.y * a, -n.y}};
}
// A cosine weighted direction about the unit normal n from two numbers in 0..1 (Malley's: the disc lifted to the hemisphere), kept at least kCosineFloor above the tangent plane.
inline gi::Vec3 CosineDirection(gi::Vec3 n, float u1, float u2) {
  const float r = std::sqrt(std::min(u1, 0.9999f));
  const float phi = kTwoPi * u2;
  const float x = r * std::cos(phi), y = r * std::sin(phi);
  const float z = std::sqrt(std::max(1.0f - x * x - y * y, 0.0f));
  const Basis bs = MakeBasis(n);
  gi::Vec3 d = bs.t * x + bs.b * y + n * z;
  const float above = gi::Dot(d, n);
  if (above < kCosineFloor) d = d + n * (kCosineFloor - above);
  const float l2 = gi::Dot(d, d);
  return d * (1.0f / std::sqrt(l2));
}

inline float ClampChannel(float v) { return v >= 0.0f ? std::min(v, kMaxRadiance) : 0.0f; }   // as the probes' record: a NaN and a negative are 0
inline gi::Vec3 ClampRadiance(gi::Vec3 c) { return {ClampChannel(c.x), ClampChannel(c.y), ClampChannel(c.z)}; }
// The radiance a ray with no hit within the reach carries (the field's irradiance at the pixel).
inline gi::Vec3 MissRadiance(gi::Vec3 e_field) { return ClampRadiance(e_field * kMissShare); }
// A car's hit: a neutral diffuse in the frame's ambient and the sun's N.L (CHECK G5-d).
inline gi::Vec3 CarRadiance(gi::Vec3 ambient, gi::Vec3 sun, float n_dot_l) { return ClampRadiance((ambient + sun * gi::Saturate(n_dot_l)) * kCarAlbedo); }
// The raw mode's hit distance (dlss_rr_rules.h RawHitDistance's rule): 0 where nothing was traced, kMissDistance where the ray found nothing, the ray's length where one hit.
constexpr float HitDistance(bool traced, bool hit, float length) {
  if (!traced) return 0.0f;
  if (!hit) return kMissDistance;
  return length > 0.0f ? (length < kMissDistance ? length : kMissDistance) : 0.0f;
}
// The ray's origin is lifted off the surface along its normal (the library's CityHitLift: kCityHitLiftFloor + kCityHitLiftPerMetre x the view distance, at most kCityHitLiftCap).
inline float OriginLift(float view_distance) { return std::min(0.006f + 0.003f * view_distance, 0.4f); }
// The ray cone's width where the ray starts (the pixel's width in metres at its distance): the cut-outs' alpha reads at the level of it.
inline float PixelFootprint(float tan_y, float picture_height, float distance) { return picture_height > 0.0f ? 2.0f * tan_y / picture_height * distance : 0.0f; }

// ---------------------------------------------------------------------------
// the mix (what the apply does with the denoised image)
// ---------------------------------------------------------------------------

// The pixel term's weight against the field's, 0..1: the denoiser's history length (a fresh pixel 1: 0; kFullLength: all), the least a pixel that is not signal has (a depth guide of 0: 0).
inline float Weight(float length, float depth_guide) {
  if (!(depth_guide > 0.0f)) return 0.0f;
  return gi::Saturate((length - 1.0f) / (kFullLength - 1.0f)) * kMaxWeight;
}
// A RAW frame's weight (hlsl GipRawWeight): the whole of kRawWeight where the pass traced the pixel (its depth guide holds a distance), 0 where it did not (a vehicle, no albedo, the sky: the field's value).
inline float RawWeight(float depth_guide) { return depth_guide > 0.0f ? kRawWeight : 0.0f; }
inline constexpr float kMinTapWeight = 1.0e-4f;   // the apply's upsample's floor (gi_apply_rules.h kMinUpsampleWeight)
// One tap of a reduced image's read (the apply, divisor 2): bilinear x depth agreement x normal agreement, floored; 0 for a tap the pass did not trace (a depth guide of 0).
inline float TapWeight(float bilinear, float tap_depth, float depth_weight, float normal_weight) {
  return tap_depth > 0.0f ? bilinear * std::max(depth_weight * normal_weight, kMinTapWeight) : 0.0f;
}
inline gi::Vec3 Mix(gi::Vec3 e_field, gi::Vec3 e_pixel, float w) {
  if (!(w > 0.0f)) return e_field;
  return {e_field.x + (e_pixel.x - e_field.x) * w, e_field.y + (e_pixel.y - e_field.y) * w, e_field.z + (e_pixel.z - e_field.z) * w};
}

// ---------------------------------------------------------------------------
// the surface lights (the headlights' term at a hit)
// ---------------------------------------------------------------------------

// The frame's surface lights the pass reads: the surface ranking's first lights that are not the game's own lamps (those add light in the game's shaders: kLightFlagGameLit) and have a range, at most
// `max`, best first; written as GpuLight rows. Returns the count.
inline uint32_t PickLights(const larecomp::gpu::LightFrame& frame, uint32_t max, larecomp::gpu::GpuLight* out) {
  using namespace larecomp::gpu;
  uint32_t n = 0;
  const uint32_t wanted = frame.role_count[uint32_t(LightRole::kSurface)];
  for (uint32_t i = 0; i < wanted && i < larecomp::gpu::kMaxLights && n < max; ++i) {
    const uint32_t slot = frame.order[uint32_t(LightRole::kSurface)][i];
    if (slot >= frame.count) continue;
    const Light& l = frame.lights[slot];
    if (l.id == kNoLightId || !(l.range > 0.0f) || (l.roles & kRoleMaskSurface) == 0 || (l.flags & kLightFlagGameLit) != 0) continue;
    out[n++] = PackGpuLight(l);
  }
  return n;
}

// ---------------------------------------------------------------------------
// the passes' layout (the compute layout, the rows, the push, the images)
// ---------------------------------------------------------------------------

// The trace's layout: the heap's texture set twice (set 0 Texture2D, set 1 Texture2DMS: the planes at the host's MSAA level), the samplers, the pass set, the ray tracing set, the push block.
inline constexpr uint32_t kTextureSet = 0, kTextureMsSet = 1, kSamplerSet = 2, kPassSet = 3, kSceneSet = 4, kPushSpace = 5;
// The pass set: t0 the rows, u1 the signal, u2 the depth guide, u3 the view normal guide, u4 the hit image, u5 the counters (D3D12 sizes a set by this count).
inline constexpr uint32_t kBindRows = 0, kBindSignal = 1, kBindDepth = 2, kBindNormal = 3, kBindHit = 4, kBindStats = 5;
inline constexpr uint32_t kSetViews = 6;
inline constexpr uint32_t kGroup = 8;   // numthreads(8, 8, 1)
constexpr uint32_t Groups(uint32_t extent) { return (extent + kGroup - 1) / kGroup; }

// The rows the trace reads (a ByteAddressBuffer, 16 bytes a row): the apply's buffer first (the field's 20 rows then its 9, gi_apply_rules.h MakeBuffer, 464 bytes), then these:
//   row 0  uint   pixel image width, height, divisor, traced-to-picture scale (1 or 2)
//   row 1  uint   frame serial, flags (kFlag*), the surface lights in the rows, the sample count of the planes (informational)
//   row 2  float  the ray's reach (m), the probes' bounce weight, the light gain, the pixel's angle (rad)
//   row 3..  the surface lights as GpuLight rows (four rows each)
inline constexpr uint32_t kRowsOffsetBytes = 464;                 // gi_apply::kBufferBytes
inline constexpr uint32_t kPixelRows = 3 + 4 * kLightsMax;
inline constexpr uint32_t kBufferBytes = kRowsOffsetBytes + kPixelRows * 16;   // 464 + 27 x 16 = 896
inline constexpr uint32_t kBufferAlloc = 1024;
static_assert(kBufferBytes == 896 && kBufferBytes <= kBufferAlloc, "the rows: the apply's buffer and the pixel pass's own");
inline constexpr uint32_t kFlagStats = 1u;   // a census frame (the counters are added to)
inline constexpr uint32_t kFlagMs = 2u;      // informational: the planes are multisampled (the shader variant says it)

struct Push {
  uint32_t slots[4];   // x the main depth plane, y RT1 (world normal), z RT2 (albedo, lit weight), w the occlusion's linear depth (traced size, the vehicle mark)
  uint32_t info[4];    // x 0, y 0, z the host sample that is the guest's sample 0, w 0
};
static_assert(sizeof(Push) == 32, "the push block is 32 bytes");

// The counters (u5): words
inline constexpr uint32_t kStatPixels = 0;     // pixels traced
inline constexpr uint32_t kStatHits = 1;       // ... whose ray hit the city
inline constexpr uint32_t kStatCarHits = 2;    // ... a car
inline constexpr uint32_t kStatMisses = 3;     // ... found nothing within the reach
inline constexpr uint32_t kStatSkipped = 4;    // pixels left alone (no normal, albedo, depth, or a vehicle)
inline constexpr uint32_t kStatNoData = 5;     // hits the library could not shade (no data in the pool)
inline constexpr uint32_t kStatWords = 8;
inline constexpr uint32_t kStatBytes = kStatWords * 4;

// The images' formats: the signal RGBA16F (the sample's radiance in rgb), the depth guide R32F (the view distance along the view axis, 0: not signal), the view normal RGBA16F (facing the
// camera, w the roughness: 1), the hit image RG16F (x the hit distance as HitDistance, y the share the filter may replace: 1 for a pixel the pass traced).
inline constexpr float kRoughness = 1.0f;

// The census's text, the first line and the interval (what the chain reads: the lines start with '[gi pixel]').
inline std::string FirstLine(uint32_t frame, uint32_t width, uint32_t height, uint32_t divisor, uint32_t lights, float reach) {
  char buf[400];
  std::snprintf(buf, sizeof(buf), "frame %u: the per-pixel pass runs: %ux%u pixels (traced size / %u), 1 ray a pixel, reach %.1f m, %u surface light(s), RtDenoise diffuse profile", frame, width, height, divisor, double(reach),
                lights);
  return buf;
}

// The log line of a change of the raw state (a frame DLSS Ray Reconstruction takes or gives back).
inline std::string RawLine(bool raw) {
  return raw ? "raw: Ray Reconstruction takes the frame, so RtDenoise does not run: the 1 spp signal goes to the colour as it is (weight 1 where traced) and the ray's hit distance goes to the guide pass"
             : "plain: Ray Reconstruction does not take the frame, RtDenoise filters the signal again (its history starts again)";
}

// ---------------------------------------------------------------------------
// the denoiser's push block (the PLAIN consumer's words, rt_denoise_rules.h plain::) and the camera's motion
// ---------------------------------------------------------------------------

struct DenoiseView {
  uint32_t signal_slot = 0, depth_slot = 0, normal_slot = 0, hit_slot = 0, history_depth_slot = 0;   // bindless slots, each under 65535
  uint32_t width = 0, height = 0;
  float tan_x = 1.0f, tan_y = 1.0f, prev_tan_x = 1.0f, prev_tan_y = 1.0f;
  float motion[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};   // the 3 x 4 from this frame's view space into the last frame's
  float length = 1.0f;                                       // the history length the spatial pass assumes
};
inline std::array<uint32_t, rt_denoise::kPushWords> MakePush(const DenoiseView& v) {
  namespace p = rt_denoise::plain;
  std::array<uint32_t, rt_denoise::kPushWords> w = {};
  const auto bits = [](float f) { return std::bit_cast<uint32_t>(f); };
  w[p::kSignal] = (v.signal_slot & 0xFFFFu) | (v.depth_slot << 16);
  w[p::kGuides] = (v.normal_slot & 0xFFFFu) | (v.hit_slot << 16);
  w[p::kHistory] = 0 | (v.history_depth_slot << 16);   // the class patches the low 16 bits (the history image's slot)
  w[p::kSize] = (v.width & 0xFFFFu) | (v.height << 16);
  w[p::kParams] = 0;                                   // the class patches the iteration and the has-history bit
  w[p::kTanX] = bits(v.tan_x);
  w[p::kTanY] = bits(v.tan_y);
  w[p::kPrevTanX] = bits(v.prev_tan_x);
  w[p::kPrevTanY] = bits(v.prev_tan_y);
  for (uint32_t i = 0; i < 12; ++i) w[p::kMotion + i] = bits(v.motion[i]);
  w[p::kLength] = bits(v.length);
  return w;
}

struct CameraPose {
  double position[3] = {0, 0, 0}, right[3] = {1, 0, 0}, up[3] = {0, 1, 0}, forward[3] = {0, 0, 1};   // world space; the renderer's view looks along +z
};
// The 3 x 4 (rows of four) taking a point of the CURRENT view space (x right, y up, z forward, metres) into the PREVIOUS one: world = position + x right + y up + z forward, then the previous pose's axes.
inline void MotionRows(const CameraPose& cur, const CameraPose& prev, float out[12]) {
  const double* axes_prev[3] = {prev.right, prev.up, prev.forward};
  const double* axes_cur[3] = {cur.right, cur.up, cur.forward};
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      double dot = 0.0;
      for (int k = 0; k < 3; ++k) dot += axes_prev[r][k] * axes_cur[c][k];
      out[r * 4 + c] = float(dot);
    }
    double t = 0.0;
    for (int k = 0; k < 3; ++k) t += axes_prev[r][k] * (cur.position[k] - prev.position[k]);
    out[r * 4 + 3] = float(t);
  }
}
// The history length the spatial pass assumes after `frames` frames of history (the kernel shrinks as it grows, rt_denoise_rules.h LengthScale).
constexpr float AssumedLength(uint32_t frames) { return float(std::min(frames + 1u, uint32_t(rt_denoise::kMaxLength))); }

static_assert(kReachM == 12.0f && kLightsMax == 6 && kLightsMax * 4 + 3 == kPixelRows, "the cell's numbers");
static_assert(!CellFor(gi::Rung::kNone, gi::Level::kUltra).runs && !CellFor(gi::Rung::kWeak, gi::Level::kUltra).runs && !CellFor(gi::Rung::kStrong, gi::Level::kHigh).runs && !CellFor(gi::Rung::kStrong, gi::Level::kOff).runs &&
                  CellFor(gi::Rung::kStrong, gi::Level::kUltra).runs,
              "only the strong rung at ULTRA runs the per-pixel pass: every other cell is the identity");
static_assert(kBufferAlloc >= kBufferBytes && kSetViews == 6, "the rows and the set");

}  // namespace mcla::gpu::gi_pixel
