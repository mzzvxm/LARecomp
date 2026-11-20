#pragma once
// ===========================================================================
// MCLA GPU renderer -- car headlights as lights: the rules that need no renderer
// ===========================================================================
// perf/LOCAL-SHADOWS-SURVEY.md finding 1 and section 5.1 (option P0): in the original game a car's headlights light nothing (a glow, a corona
// and an emissive lens). The renderer adds them as real spot lights to the lit shaders (the PSMultiLight family and the traffic body,
// shaderpack/variants/headlight.h). This header holds what needs no renderer, as pure functions on plain numbers, so that
// headlight_rig_test.cpp builds on its own:
//
//   - the look: the numbers of the beam (cone, reach, fall-off, colour), shared with the shader's helper (the variant's test checks that
//     the helper's `kHl...` constants are these);
//   - the CPU twin of the shader's term (LampTerm: cone, distance fall-off, a window to zero at the reach, a wrapped surface term, a low beam's
//     cut-off above the lamp, a fade near the lamp), and the projection of a world point into a local shadow view (ProjectToShadow);
//   - where a car's headlights are: the front lens draws' centres and the car's own axes (FrontLamp), whether it is night (NightFactor);
//   - the game's own light points (headlight_points.h): matched to the lamps the lens draws made (AssignPoints, ApplyPoints) they give the lamp's exact place,
//     the car's forward and whether the lights are on (the point's intensity); points no lens lamp is near make lamps of their own (LampsFromPoints);
//   - the Tracker: the cars seen in a frame become tracks that keep a key from frame to frame, are picked at most four at a time by
//     importance (the player's first) with a hysteresis, fade in and out, and are moved one frame ahead by their own velocity.
//
// Matrices are row vectors as in car_dump_rules.h (world = (p, 1) * M): the car entity's world matrix has the rows right, up, BACK and
// position (mc_engine/cinematic/car_registry.h): the car's forward is minus the third row. A car's DRAW does not carry that matrix in gWorld, though:
// gWorld (c0-c3) is the car's root translation only (rotation = the identity) and the car's turn is in the bone palette (gCarMtxBuffer, c64 and up;
// perf/CAR-RT-SPIKE.md: "the palette is world space minus the root translation"; the traffic cars' heading is in bone 0, 2026-11-19): a pose is made from a lens draw's BONE (PoseFromBone), its model space being the entity's
// (+z = the car's back). Reading gWorld's rows made every car face -z, whatever way it was driving (the headlights run of 2026-11-20, the light landed
// behind the player's car). This header includes nothing of the renderer.
// ===========================================================================

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "headlight_points.h"
#include "light_influence.h"
#include "local_shadow_types.h"

namespace mcla::gpu::headlight_rules {

// ---------------------------------------------------------------------------
// the look
// ---------------------------------------------------------------------------

inline constexpr size_t kMaxLights = 4;

// The shader's term of one light, in the game's light units (what a lamp's term is: its colour times how much of its disc the pixel sees;
// the grid lamps saturate at 1 inside their reach, and the night's ambient is about 0.1: a road under a street lamp shows about 60 to 80 of 255 where the
// ambient alone shows 20 to 25, a ratio of 8 to 9 in light).
//
// CALIBRATION (2026-11-20, HEADLIGHT-FIX-2). The first look (peak 2.4, half distance 9 m) was a street lamp's level at 5 m and almost nothing beyond 15 m:
// on a flat road (lamp 0.75 m up, the axis 2 degrees down, the road's surface term 0.47 to 0.53) its term was 0.96 at 5 m, 0.50 at 10 m, 0.28 at 15 m, 0.16 at
// 20 m, 0.06 at 30 m: below the ambient from 25 m on, so the beam was lost in the picture (2.8 % of a chase picture's pixels changed). An inverse square
// fall-off is too steep for a game's fixed exposure with a road lit at a grazing angle; a real low beam is aimed so that the road keeps its light out to
// 40 m. Now peak 2.0 and half distance 26 m: the road's term on the axis is
//     d (m)    3     5    10    15    20    25    30    40    50
//     term   1.15  1.01  0.82  0.65  0.50  0.37  0.26  0.12  0.05     (a wall facing the lamp: 1.9 at 5 m, 1.7 at 10 m, 1.06 at 20 m, 0.57 at 30 m)
// a street lamp's level at 5 m, half of it at 20 m (the edge of a lamp's pool, 4 to 5 times the ambient), a quarter at 30 m, out at 50 m: the near field is as
// before (0.96 -> 1.01 at 5 m), the road at 20 m is 3 times and at 30 m 4 times brighter. A pale wall right in front of the car (1.9 times its albedo) still
// clips, as a car's headlights do; a road never does (the lightest lane paint at 5 m: 0.7 x 1.0). headlight_rig_test.cpp holds these numbers;
// mcla_gpu_headlight_strength (debug) multiplies all of it.
inline constexpr float kPeak = 2.0f;              // the term on the beam's axis, close to the lamp (before the fall-off)
inline constexpr float kFalloffDistance = 26.0f;  // m: the distance at which the 1/(1 + (d/D)^2) fall-off is a half
inline constexpr float kSurfaceWrap = 0.45f;      // surface term = saturate(wrap + (1 - wrap) N.L): a road seen at a grazing angle keeps this share
inline constexpr float kCutLow = 0.10f;           // low beam: the sine of the angle a point is above the lamp (as seen from the lamp) where the cut-off starts
inline constexpr float kCutHigh = 0.38f;          // ... and where it is complete
inline constexpr float kNearStart = 0.8f;         // m: nothing is lit nearer to the lamp than this (the car's own nose, the singularity)
inline constexpr float kNearSpan = 1.5f;          // m: and the light is complete this much farther

// The beam as the rig makes it (the cone is a circular one: the frozen Headlight has one axis and two cosines).
inline constexpr float kOuterDegrees = 38.0f;     // half angle where the cone ends
inline constexpr float kInnerDegrees = 12.0f;     // half angle inside which it is full
inline constexpr float kDipDegrees = 2.0f;        // the axis points this far below the car's forward (a low beam)
inline constexpr float kRange = 70.0f;            // m: the reach; the term falls to 0 at it
inline constexpr float kNudge = 0.12f;             // m: the lamp is put this far in front of the lens
inline constexpr float kColor[3] = {1.0f, 0.93f, 0.78f};  // a warm white, before the strength and the fade

constexpr double kPi = 3.14159265358979323846;

// ---------------------------------------------------------------------------
// small vector helpers (float, 3 components)
// ---------------------------------------------------------------------------

inline float Dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline float Saturate(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }
// HLSL's smoothstep.
inline float SmoothStep(float a, float b, float x) {
  const float t = Saturate((x - a) / (b - a));
  return t * t * (3.0f - 2.0f * t);
}

// ---------------------------------------------------------------------------
// the shader's term, on the CPU
// ---------------------------------------------------------------------------

// One light as the shader reads it (the host's HeadlightConstants rows): position and reach, the axis and the cosine of the outer half angle,
// the colour (already times the strength and the fade) and the cosine of the inner half angle.
struct Lamp {
  float position[3] = {};
  float range = kRange;
  float direction[3] = {0.0f, 0.0f, 1.0f};
  float cos_outer = 0.7880f;
  float color[3] = {1.0f, 1.0f, 1.0f};
  float cos_inner = 0.9781f;
};

// The surface: a world point and its normal (unit, facing the viewer).
struct Surface {
  float position[3] = {};
  float normal[3] = {0.0f, 1.0f, 0.0f};
};

// The scalar part of one light's term at a point (without the colour), and 0 for a point outside the cone or the reach. `visibility` is the
// shadow lookup's answer (1 without a shadow). The order of the factors is the shader's.
inline float LampScale(const Lamp& lamp, const Surface& surface, float visibility = 1.0f) {
  const float to_light[3] = {lamp.position[0] - surface.position[0], lamp.position[1] - surface.position[1],
                             lamp.position[2] - surface.position[2]};
  const float dist2 = Dot3(to_light, to_light);
  const float range2 = lamp.range * lamp.range;
  if (dist2 >= range2 || dist2 < 1.0e-6f) {
    return 0.0f;
  }
  const float inverse = 1.0f / std::sqrt(dist2);
  const float l[3] = {to_light[0] * inverse, to_light[1] * inverse, to_light[2] * inverse};  // from the surface to the lamp
  const float cosine = -Dot3(l, lamp.direction);                                            // angle from the beam's axis
  if (cosine <= lamp.cos_outer) {
    return 0.0f;
  }
  const float cone = SmoothStep(lamp.cos_outer, lamp.cos_inner, cosine);
  const float dist = dist2 * inverse;
  const float falloff = 1.0f / (1.0f + dist2 * (1.0f / (kFalloffDistance * kFalloffDistance)));
  float window = Saturate(1.0f - dist2 / range2);
  window *= window;
  const float surface_term = Saturate(kSurfaceWrap + (1.0f - kSurfaceWrap) * Dot3(surface.normal, l));
  const float cutoff = 1.0f - SmoothStep(kCutLow, kCutHigh, -l[1]);
  const float near_fade = Saturate((dist - kNearStart) * (1.0f / kNearSpan));
  return kPeak * cone * falloff * window * surface_term * cutoff * near_fade * visibility;
}

// The sum over the first `count` lamps (the shader's loop), for a point; `visibility` per lamp may be null (all 1).
inline void LampSum(const Lamp* lamps, size_t count, const Surface& surface, const float* visibility, float out[3]) {
  out[0] = out[1] = out[2] = 0.0f;
  for (size_t i = 0; i < count && i < kMaxLights; ++i) {
    const float scale = LampScale(lamps[i], surface, visibility ? visibility[i] : 1.0f);
    for (int c = 0; c < 3; ++c) {
      out[c] += lamps[i].color[c] * scale;
    }
  }
}

// The box that holds every point any of the lamps reaches (the shader's early-out: a point outside it gets nothing without any lamp being
// looked at). False when there is no lamp.
inline bool ReachBox(const Lamp* lamps, size_t count, float lo[3], float hi[3]) {
  if (count == 0) {
    return false;
  }
  for (int c = 0; c < 3; ++c) {
    lo[c] = lamps[0].position[c] - lamps[0].range;
    hi[c] = lamps[0].position[c] + lamps[0].range;
  }
  for (size_t i = 1; i < count && i < kMaxLights; ++i) {
    for (int c = 0; c < 3; ++c) {
      lo[c] = std::min(lo[c], lamps[i].position[c] - lamps[i].range);
      hi[c] = std::max(hi[c], lamps[i].position[c] + lamps[i].range);
    }
  }
  return true;
}

// The cosines of the beam's half angles.
inline float CosOfDegrees(float degrees) { return float(std::cos(double(degrees) * kPi / 180.0)); }

// ---------------------------------------------------------------------------
// the shadow hook, on the CPU
// ---------------------------------------------------------------------------

// A world point through a local shadow view (local_shadow_types.h: world_to_shadow is row-major, out = M * (p, 1), then / w): the atlas uv
// in out_uv, the receiver's depth in *out_depth. False when the point is behind the view (w <= 0) or outside its atlas rectangle
// (atlas_rect = u0, v0, u1, v1) or its depth range [0, 1]: such a point is lit (the view says nothing about it).
inline bool ProjectToShadow(const larecomp::gpu::LocalShadowView& view, const float p[3], float out_uv[2], float* out_depth) {
  float c[4];
  for (int r = 0; r < 4; ++r) {
    const float* row = view.world_to_shadow + 4 * r;
    c[r] = row[0] * p[0] + row[1] * p[1] + row[2] * p[2] + row[3];
  }
  if (!(c[3] > 0.0f)) {
    return false;
  }
  const float u = c[0] / c[3], v = c[1] / c[3], z = c[2] / c[3];
  if (u < view.atlas_rect[0] || v < view.atlas_rect[1] || u > view.atlas_rect[2] || v > view.atlas_rect[3] || z < 0.0f || z > 1.0f) {
    return false;
  }
  out_uv[0] = u;
  out_uv[1] = v;
  *out_depth = z;
  return true;
}

// The compare of one tap: lit when the map's stored depth is not nearer than the receiver's (moved toward the lamp by the bias).
inline float ShadowTap(float stored, float receiver_depth, float bias) { return stored >= receiver_depth - bias ? 1.0f : 0.0f; }

// THE SOFT LOOKUP (the shader's HeadlightShadow; src/shaderpack/variants/headlight.cpp kHlShadowKernel is this number). The first version compared FOUR taps half a
// texel from the receiver's place, each tap one texel of a point sampled map: five levels, and every edge of a shadow a staircase of texels, hard and grainy (the owner's
// "hard-edged stripes", 2026-11-20). Now the answer is the share of a box `kShadowKernel` texels square, centred on the receiver's place in the map, whose texels are
// lit, each texel counted by the part of it the box covers (a texel the box covers by a third counts a third): the share changes smoothly as the receiver moves, and
// a straight edge goes from shadow to light over `kShadowKernel` texels (about 15 cm at 27 m from the lamps with a 1024 tile, twice that on the 512 tile of `high`).
// A box this wide covers 4 x 4 texels, 16 fetches; the four outer ones go first and when they agree (all lit or all dark: a point well inside a shadow or a lit
// area, which is most of the lit pixels) the other twelve are not taken: nothing thinner than the box is lost there, which the penumbra does not need.
inline constexpr float kShadowKernel = 3.0f;

// (tx, ty): the receiver's place in texels of the atlas (its uv over one texel's uv); lit_at(ix, iy): the compare of texel (ix, iy) of the atlas with the
// receiver's depth, 1 lit or 0 in shadow.
template <class LitAt>
inline float SoftShadow(float tx, float ty, LitAt&& lit_at) {
  const float lx = tx - 0.5f * kShadowKernel, ly = ty - 0.5f * kShadowKernel;
  const float bx = std::floor(lx), by = std::floor(ly);
  const float fx = lx - bx, fy = ly - by;
  const float wx[4] = {1.0f - fx, 1.0f, 1.0f, fx};
  const float wy[4] = {1.0f - fy, 1.0f, 1.0f, fy};
  const int ix = int(bx), iy = int(by);
  const float corners = lit_at(ix, iy) + lit_at(ix + 3, iy) + lit_at(ix, iy + 3) + lit_at(ix + 3, iy + 3);
  if (corners == 0.0f || corners == 4.0f) {
    return corners * 0.25f;
  }
  float lit = 0.0f;
  for (int j = 0; j < 4; ++j) {
    float row = 0.0f;
    for (int i = 0; i < 4; ++i) {
      row += wx[i] * lit_at(ix + i, iy + j);
    }
    lit += wy[j] * row;
  }
  return lit * (1.0f / 9.0f);
}

// The host's table from a light to its shadow view: view index, or kNoView. A view counts when it is valid and names a light of the set
// (light_index 0 .. kMaxLights - 1; 100 and up are the lamps', later); a light has the first valid view that names it.
inline constexpr uint32_t kNoView = 0xFFFFFFFFu;
inline std::array<uint32_t, kMaxLights> ShadowViewOfLight(const larecomp::gpu::LocalShadowFrame* frame, uint32_t light_count) {
  std::array<uint32_t, kMaxLights> of;
  of.fill(kNoView);
  if (!frame) {
    return of;
  }
  const uint32_t views = std::min<uint32_t>(frame->count, uint32_t(frame->views.size()));
  for (uint32_t v = 0; v < views; ++v) {
    const larecomp::gpu::LocalShadowView& view = frame->views[v];
    if (view.valid != 0 && view.light_index < light_count && view.light_index < kMaxLights && of[view.light_index] == kNoView) {
      of[view.light_index] = v;
    }
  }
  return of;
}

// A frame of shadow views made for one set of lights, read for a later one. A view names its light by its place in the set the producer saw
// (light_index); the set of the frame that reads the map can have another order or another count (a car came into the four, the tracker's hysteresis
// swapped two), so the views go by the car instead: `old_keys` is the car key of each light of the producer's set, `new_keys` of the set that reads. Each
// view goes to the first light of its car in the new set that no other view took (a car with two lamps has two views, one for each lamp, with the same
// matrix); a view whose car is not in the new set is dropped.
inline larecomp::gpu::LocalShadowFrame RemapShadowViews(const larecomp::gpu::LocalShadowFrame& frame, const uint32_t* old_keys, uint32_t old_count,
                                                        const uint32_t* new_keys, uint32_t new_count) {
  larecomp::gpu::LocalShadowFrame out;
  std::array<bool, kMaxLights> taken{};
  const uint32_t views = std::min<uint32_t>(frame.count, uint32_t(frame.views.size()));
  for (uint32_t v = 0; v < views; ++v) {
    const larecomp::gpu::LocalShadowView& view = frame.views[v];
    if (view.valid == 0 || view.light_index >= old_count) {
      continue;
    }
    const uint32_t key = old_keys[view.light_index];
    for (uint32_t i = 0; i < new_count && i < kMaxLights; ++i) {
      if (new_keys[i] == key && !taken[i]) {
        taken[i] = true;
        larecomp::gpu::LocalShadowView moved = view;
        moved.light_index = i;
        out.views[out.count++] = moved;
        break;
      }
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// the block the lit shaders read
// ---------------------------------------------------------------------------

inline constexpr uint32_t kNoSlot = 0xFFFFFFFFu;

// The constants the headlight variant reads, after the 608 byte SharedConstants (the variant's declarations: shaderpack/variants/headlight.cpp,
// c38 to c79 of the block; the SPIR-V side reads them at 608 + the offset). One block a draw: the 4 lights, the box they reach, the camera and the
// shadow views. 672 bytes.
struct HeadlightConstants {
  float header[4];             // x = the number of lights (0 to 4)
  float camera[4];             // xyz = the camera's world position
  float bounds_lo[4];          // the box every light reaches: xyz (w 0); a box with lo above hi holds no point
  float bounds_hi[4];
  float lights[12][4];         // per light: [3 i] position xyz + reach, [3 i + 1] axis xyz + cos outer, [3 i + 2] colour rgb + cos inner
  uint32_t shadow_of[4];       // per light: its shadow view, or kNoView
  uint32_t atlas[4];           // x = the atlas texture's bindless slot, y = its sampler's; kNoSlot: none (every light is lit)
  larecomp::gpu::LocalShadowView shadow[4];  // the views, as the producer made them (local_shadow_types.h)
};
static_assert(sizeof(HeadlightConstants) == 672, "the headlight block is 672 bytes (c38 to c79)");
static_assert(offsetof(HeadlightConstants, camera) == 16 && offsetof(HeadlightConstants, bounds_lo) == 32 && offsetof(HeadlightConstants, bounds_hi) == 48 &&
              offsetof(HeadlightConstants, lights) == 64 && offsetof(HeadlightConstants, shadow_of) == 256 &&
              offsetof(HeadlightConstants, atlas) == 272 && offsetof(HeadlightConstants, shadow) == 288);

// The block for a frame's lights (`lamps`, colours already times the strength and the fade), the camera's world position and the shadow views of the
// frame (null: none; `atlas_texture` / `atlas_sampler` the bindless slots of the atlas they are in).
inline HeadlightConstants MakeConstants(const Lamp* lamps, size_t count, const float camera[3], const larecomp::gpu::LocalShadowFrame* shadows,
                                        uint32_t atlas_texture, uint32_t atlas_sampler) {
  HeadlightConstants block{};
  count = std::min(count, kMaxLights);
  block.header[0] = float(count);
  for (int c = 0; c < 3; ++c) {
    block.camera[c] = camera[c];
  }
  float lo[3], hi[3];
  if (ReachBox(lamps, count, lo, hi)) {
    for (int c = 0; c < 3; ++c) {
      block.bounds_lo[c] = lo[c];
      block.bounds_hi[c] = hi[c];
    }
  } else {
    for (int c = 0; c < 3; ++c) {
      block.bounds_lo[c] = 1.0e30f;
      block.bounds_hi[c] = -1.0e30f;
    }
  }
  for (size_t i = 0; i < count; ++i) {
    const Lamp& lamp = lamps[i];
    float* a = block.lights[3 * i];
    float* b = block.lights[3 * i + 1];
    float* d = block.lights[3 * i + 2];
    for (int c = 0; c < 3; ++c) {
      a[c] = lamp.position[c];
      b[c] = lamp.direction[c];
      d[c] = lamp.color[c];
    }
    a[3] = lamp.range;
    b[3] = lamp.cos_outer;
    d[3] = lamp.cos_inner;
  }
  const std::array<uint32_t, kMaxLights> of = ShadowViewOfLight(shadows, uint32_t(count));
  bool any = false;
  for (size_t i = 0; i < kMaxLights; ++i) {
    block.shadow_of[i] = of[i];
    any |= of[i] != kNoView;
  }
  if (any && shadows) {
    block.atlas[0] = atlas_texture;
    block.atlas[1] = atlas_sampler;
    for (size_t v = 0; v < kMaxLights; ++v) {
      block.shadow[v] = shadows->views[v];
    }
  } else {
    block.atlas[0] = block.atlas[1] = kNoSlot;
  }
  block.atlas[2] = block.atlas[3] = 0;
  return block;
}

// ---------------------------------------------------------------------------
// the sun: is it night
// ---------------------------------------------------------------------------

// The sine of the sun's elevation from a vertex constant bank's c16, the direction the light TRAVELS (ao.cpp: noon overhead is y -0.87); the
// night's light points up (the moon, a dim light: elevation below the horizon). 0 when it is not a direction.
inline double SunSine(const double travel[3]) {
  const double length = std::sqrt(travel[0] * travel[0] + travel[1] * travel[1] + travel[2] * travel[2]);
  if (!std::isfinite(length) || length < 1e-6) {
    return 1.0;  // no light constants: treated as day
  }
  return -travel[1] / length;
}

// 1 at and below kNightFull degrees of elevation, 0 from kNightNone, smooth between (a dusk: 19 h is about 2.5 degrees with a grey sun).
inline constexpr double kNightFull = 3.0;
inline constexpr double kNightNone = 12.0;
inline double NightFactor(double sun_sine) {
  const double degrees = std::asin(std::clamp(sun_sine, -1.0, 1.0)) * 180.0 / kPi;
  const double t = std::clamp((degrees - kNightFull) / (kNightNone - kNightFull), 0.0, 1.0);
  return 1.0 - t * t * (3.0 - 2.0 * t);
}

// ---------------------------------------------------------------------------
// where a car's headlights are
// ---------------------------------------------------------------------------

// A car's own frame, from its world matrix (the entity's: right, up, back, position).
struct Pose {
  double position[3] = {};
  double right[3] = {1, 0, 0};
  double up[3] = {0, 1, 0};
  double back[3] = {0, 0, 1};  // minus forward
};

inline double Length3(const double v[3]) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }
inline double Dot3d(const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// The pose of a 4x4 (rows right, up, back, position); false when it is not a car's frame: a number that is not finite, an axis that is
// not about unit length, two axes that are not about at right angles.
inline bool PoseFromWorld(const double m[4][4], Pose* out) {
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 3; ++c) {
      if (!std::isfinite(m[r][c])) {
        return false;
      }
    }
  }
  Pose pose;
  double* axes[3] = {pose.right, pose.up, pose.back};
  for (int r = 0; r < 3; ++r) {
    const double length = Length3(m[r]);
    if (!(length > 0.5 && length < 2.0)) {
      return false;
    }
    for (int c = 0; c < 3; ++c) {
      axes[r][c] = m[r][c] / length;
    }
  }
  if (std::fabs(Dot3d(pose.right, pose.up)) > 0.2 || std::fabs(Dot3d(pose.up, pose.back)) > 0.2 || std::fabs(Dot3d(pose.right, pose.back)) > 0.2) {
    return false;
  }
  for (int c = 0; c < 3; ++c) {
    pose.position[c] = m[3][c];
  }
  *out = pose;
  return true;
}

// A car's frame from the bone matrix of one of its parts and the car's root. `palette` is car_dump::CarMatrices::palette: the row-vector form of the bone's three
// registers (the bone maps a model point p to rows 0-2 applied to p, plus row 3: row 0, 1 and 2 are the images of the model's x, y and z axes, in the world, row 3 the
// bone's offset from the root). The car's body and the parts fixed to it (the lamps' lenses) share the car's turn, and the model's +z is the car's BACK, as the
// entity matrix's third row is (the game's own light points' direction and the ambient scene verb's heading + 180 both say so). CHECK: that sign is read off those,
// not off a lens draw's bone: the census counts the lamps the game's points turn round (`forward turned by the points`). `root` is gWorld's
// translation (row 3 of c0-c3): the car's origin, which is how the lens draws of one car are told from another's. False when the rows are no car's frame.
inline bool PoseFromBone(const double palette[4][4], const double root[3], Pose* out) {
  double m[4][4];
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 4; ++c) {
      m[r][c] = palette[r][c];
    }
  }
  for (int c = 0; c < 3; ++c) {
    m[3][c] = root[c];
  }
  m[3][3] = 1.0;
  return PoseFromWorld(m, out);
}

// A lens draw whose centre lies this near the car's root (on the ground plane) is no lamp of the car: a car's lamps are at its ends, 1.5 m and more from the
// root, and the draws that sit at the root are the sprites (glows, coronas) that have a world matrix of their own, or a lens mesh whose box could not be read
// (the centre is then the bone's origin).
inline constexpr double kLensMinFromRoot = 1.2;
inline bool LensOffRoot(const double root[3], const double lens[3]) {
  const double dx = lens[0] - root[0], dz = lens[2] - root[2];
  return std::sqrt(dx * dx + dz * dz) >= kLensMinFromRoot;
}

// The lamp of a car: where the beam starts and which way the car faces.
struct CarLamp {
  double position[3] = {};
  double forward[3] = {0, 0, 1};
  double up[3] = {0, 1, 0};
  uint32_t front = 0;  // lenses taken as the headlights
  uint32_t total = 0;  // lenses of the car seen
  double spread = 0.0; // the extent of the lenses along the forward axis (a car has lenses at both ends)
};

inline constexpr double kEndsApart = 0.9;   // m: lenses spread over more than this along the car have a front and a rear
inline constexpr double kSingleEndMin = 0.4;  // m: a single cluster counts as the front when its mean is this far ahead of the car's root

// The headlights of a car from the centres of its lens draws (world space) and its pose. A car's lens draws are all its lamps' (the head
// lamps, the tail lamps, the indicators ...): the ones on the front half along the car's forward are its headlights; the lamp is the mean
// of their centres, put kNudge in front. False when no lens is on the front half.
inline bool FrontLamp(const Pose& pose, const double (*lens)[3], size_t count, CarLamp* out) {
  if (count == 0) {
    return false;
  }
  double forward[3];
  for (int c = 0; c < 3; ++c) {
    forward[c] = -pose.back[c];
  }
  std::vector<double> along(count);
  double fmax = -1e30, fmin = 1e30, mean = 0.0;
  for (size_t i = 0; i < count; ++i) {
    const double offset[3] = {lens[i][0] - pose.position[0], lens[i][1] - pose.position[1], lens[i][2] - pose.position[2]};
    along[i] = Dot3d(offset, forward);
    fmax = std::max(fmax, along[i]);
    fmin = std::min(fmin, along[i]);
    mean += along[i];
  }
  mean /= double(count);
  const bool two_ends = fmax - fmin >= kEndsApart;
  const double threshold = two_ends ? 0.5 * (fmax + fmin) : kSingleEndMin;
  double sum[3] = {};
  uint32_t front = 0;
  for (size_t i = 0; i < count; ++i) {
    const bool is_front = two_ends ? along[i] >= threshold : mean >= threshold;
    if (!is_front) {
      continue;
    }
    for (int c = 0; c < 3; ++c) {
      sum[c] += lens[i][c];
    }
    ++front;
  }
  if (front == 0) {
    return false;
  }
  CarLamp lamp;
  for (int c = 0; c < 3; ++c) {
    lamp.position[c] = sum[c] / double(front) + kNudge * forward[c];
    lamp.forward[c] = forward[c];
    lamp.up[c] = pose.up[c];
  }
  lamp.front = front;
  lamp.total = uint32_t(count);
  lamp.spread = fmax - fmin;
  *out = lamp;
  return true;
}

// The beam's axis: the car's forward turned kDipDegrees down toward minus up (unit).
inline void BeamAxisOf(const double forward[3], const double up[3], double out[3]) {
  const double dip = double(kDipDegrees) * kPi / 180.0;
  double v[3];
  for (int c = 0; c < 3; ++c) {
    v[c] = forward[c] * std::cos(dip) - up[c] * std::sin(dip);
  }
  const double length = Length3(v);
  for (int c = 0; c < 3; ++c) {
    out[c] = v[c] / (length > 0.0 ? length : 1.0);
  }
}
inline void BeamAxis(const CarLamp& lamp, float out[3]) {
  double axis[3];
  BeamAxisOf(lamp.forward, lamp.up, axis);
  for (int c = 0; c < 3; ++c) {
    out[c] = float(axis[c]);
  }
}

// ---------------------------------------------------------------------------
// the game's own light points
// ---------------------------------------------------------------------------

using GuestPoint = headlight_points::Point;

// How near a lens lamp a Headlight point must be to be that car's (m).
inline constexpr double kPointRadius = 3.0;

// The Headlight points that belong to one lamp.
struct PointMatch {
  uint32_t count = 0;
  double position[3] = {};   // the mean place
  double back[3] = {};       // the mean of the points' direction (the car's back axis), when they have one
  bool has_dir = false;
  float intensity = 0.0f;    // the strongest
  bool player = false;
  uint32_t vehicle = 0;
};

// Gives every Headlight point to the nearest of the `count` lamps (positions in `lamps`, world) within kPointRadius; `taken` (size of points) says which point
// found a lamp. Only the points of the Headlight kind are in the table.
inline std::vector<PointMatch> AssignPoints(const std::vector<GuestPoint>& points, const double (*lamps)[3], size_t count, std::vector<bool>* taken) {
  std::vector<PointMatch> matches(count);
  std::vector<double> dir_sum(count * 3, 0.0);
  taken->assign(points.size(), false);
  for (size_t p = 0; p < points.size(); ++p) {
    size_t best = count;
    double best_distance = kPointRadius;
    for (size_t l = 0; l < count; ++l) {
      const double d[3] = {double(points[p].pos[0]) - lamps[l][0], double(points[p].pos[1]) - lamps[l][1], double(points[p].pos[2]) - lamps[l][2]};
      const double distance = Length3(d);
      if (distance < best_distance) {
        best_distance = distance;
        best = l;
      }
    }
    if (best == count) {
      continue;
    }
    (*taken)[p] = true;
    PointMatch& m = matches[best];
    ++m.count;
    for (int c = 0; c < 3; ++c) {
      m.position[c] += double(points[p].pos[c]);
      dir_sum[best * 3 + size_t(c)] += double(points[p].dir[c]);
    }
    if (points[p].has_dir) {
      m.has_dir = true;
    }
    m.intensity = std::max(m.intensity, points[p].intensity);
    m.player = m.player || points[p].player;
    if (points[p].vehicle != 0) {
      m.vehicle = points[p].vehicle;
    }
  }
  for (size_t l = 0; l < count; ++l) {
    PointMatch& m = matches[l];
    if (m.count == 0) {
      continue;
    }
    for (int c = 0; c < 3; ++c) {
      m.position[c] /= double(m.count);
      m.back[c] = dir_sum[l * 3 + size_t(c)];
    }
    const double length = Length3(m.back);
    if (!m.has_dir || !(length > 0.5)) {
      m.has_dir = false;
    } else {
      for (int c = 0; c < 3; ++c) {
        m.back[c] /= length;
      }
    }
  }
  return matches;
}

// How much of its full light a point's intensity is (0: off).
inline double OnOf(float intensity) {
  return intensity <= headlight_points::kOffIntensity ? 0.0 : std::min(1.0, double(intensity) / double(headlight_points::kFullIntensity));
}

// A lens lamp corrected by its points: the place is the points' mean (nudged forward), the forward is minus the points' back axis when they have one (and
// the car's own up stays), and `*on` the lights' state. `*disagrees` is set when the points' forward is turned more than 90 degrees from the lens
// lamp's (the pose's convention or the front/rear split is wrong, or the car is reversing: the points win, and the census says so).
inline void ApplyPoints(const PointMatch& match, CarLamp* lamp, double* on, bool* disagrees) {
  *disagrees = false;
  if (match.has_dir) {
    const double forward[3] = {-match.back[0], -match.back[1], -match.back[2]};
    *disagrees = Dot3d(forward, lamp->forward) < 0.0;
    for (int c = 0; c < 3; ++c) {
      lamp->forward[c] = forward[c];
    }
  }
  for (int c = 0; c < 3; ++c) {
    lamp->position[c] = match.position[c] + kNudge * lamp->forward[c];
  }
  *on = OnOf(match.intensity);
}

// Lamps from the points that no lens lamp took: the owned ones (a racer's, the player's: they have a direction), grouped by vehicle. The forward is minus the
// back axis, the up the world's. `taken` is AssignPoints'.
struct PointLamp {
  CarLamp lamp;
  bool player = false;
  double on = 0.0;
  uint32_t vehicle = 0;
};
inline std::vector<PointLamp> LampsFromPoints(const std::vector<GuestPoint>& points, const std::vector<bool>& taken) {
  std::vector<PointLamp> out;
  std::vector<std::array<double, 4>> sums;  // per lamp: the position sum and the count
  std::vector<std::array<double, 3>> backs;
  for (size_t p = 0; p < points.size(); ++p) {
    if (p < taken.size() && taken[p]) continue;
    const GuestPoint& point = points[p];
    if (point.vehicle == 0 || !point.has_dir) continue;
    size_t at = out.size();
    for (size_t i = 0; i < out.size(); ++i) {
      if (out[i].vehicle == point.vehicle) {
        at = i;
        break;
      }
    }
    if (at == out.size()) {
      PointLamp fresh;
      fresh.vehicle = point.vehicle;
      out.push_back(fresh);
      sums.push_back({0.0, 0.0, 0.0, 0.0});
      backs.push_back({0.0, 0.0, 0.0});
    }
    for (int c = 0; c < 3; ++c) {
      sums[at][size_t(c)] += double(point.pos[c]);
      backs[at][size_t(c)] += double(point.dir[c]);
    }
    sums[at][3] += 1.0;
    out[at].player = out[at].player || point.player;
    out[at].on = std::max(out[at].on, OnOf(point.intensity));
  }
  std::vector<PointLamp> kept;
  for (size_t i = 0; i < out.size(); ++i) {
    const double length = Length3(backs[i].data());
    if (!(length > 0.5)) continue;
    PointLamp& l = out[i];
    for (int c = 0; c < 3; ++c) {
      l.lamp.forward[c] = -backs[i][size_t(c)] / length;
      l.lamp.up[c] = c == 1 ? 1.0 : 0.0;
    }
    for (int c = 0; c < 3; ++c) {
      l.lamp.position[c] = sums[i][size_t(c)] / sums[i][3] + kNudge * l.lamp.forward[c];
    }
    l.lamp.front = uint32_t(sums[i][3]);
    l.lamp.total = l.lamp.front;
    kept.push_back(l);
  }
  return kept;
}

// ---------------------------------------------------------------------------
// the tracker: keys, the pick of at most four, hysteresis, fades, one frame ahead
// ---------------------------------------------------------------------------

// A car seen in a frame whose headlights are known.
struct Candidate {
  CarLamp lamp;
  bool player = false;
  double camera_distance = 0.0;  // m, the lamp to the camera
  bool on_screen = true;         // the first version's: whether the LAMP is on the screen. Only read when EndFrame is given no view (the reach below replaces it)
  // The lights' state when the game's own light points said (headlight_points.h): `guest` true, `on` 0 to 1 (the point's intensity over the full one). Else
  // the caller's rule (the sun) decides and these are unused.
  bool guest = false;
  double on = 1.0;
};

// What a frame hands to the lit shaders for one light.
struct Tracked {
  uint32_t key = 0;
  bool player = false;
  double position[3] = {};  // one frame ahead by the track's own velocity
  double forward[3] = {0, 0, 1};
  double up[3] = {0, 1, 0};
  float fade = 0.0f;        // 0 .. 1: fades in over a few frames, out over as many once the car is gone or not picked
  bool guest = false;       // the game's own light points said whether the lights are on: `on` is that (0 to 1)
  double on = 1.0;
  bool coasting = false;    // the game no longer draws the car (it left the frame) and its light goes on, moved on by its velocity, while its beam is in the view
  double reach = 1.0;       // how much of its beam is in the view (light_influence.h BeamReach); 1 when the tracker had no view
};

struct TrackerTuning {
  double match_radius = 6.0;        // m: a car is the same car when its lamp is this near where the track's was (moved by its velocity)
  double max_step = 5.0;            // m a frame: a faster velocity is not believed (a teleport)
  double score_distance = 25.0;     // m: the distance at which a car's score is a half
  double off_screen = 0.35;         // the share of the score of a car whose lamp is not on the screen (the first version's rule, with no view)
  double keep_bonus = 1.5;          // a picked track's score is this times more: a challenger must beat it by that much
  double linger = 6.0;              // frames: a track not seen has its score halved this often
  uint32_t drop_after = 24;         // frames without a sighting before a track is forgotten
  float fade_in = 0.12f;            // a frame's fade step up
  float fade_out = 0.12f;           // ... and down

  // HL-EDGE-FADE (owner 2026-11-20: traffic cars' headlights turn off near the frame's edge). With a view (EndFrame's third argument) a car is worth what its BEAM adds to
  // the picture, not where its lamp is, and a car the game stops drawing because it left the frame keeps its light while the beam is in the view.
  double beam_range = kRange;                  // m: the beam the reach is made for (the rig's setting)
  double beam_outer_degrees = kOuterDegrees;   // ... and its half angle
  double exit_ndc = 0.5;                       // a car whose last sighting was this far out (a share of the half picture; 1 is the edge) left through the picture's edge
  uint32_t coast_frames = 90;                  // the longest a car the game stopped drawing keeps its light (1.5 s at 60 FPS: a frame is a guest present)
  double coast_linger = 30.0;                  // frames: the score of a car that coasts is halved this often (a car in view comes first)
  double coast_damping = 0.98;                 // the velocity of a car that coasts, a frame's share of it
  double coast_reach_off = 0.01;               // a car that coasts goes out as its beam leaves the view: its reach below this is out,
  double coast_reach_full = 0.06;              // ... above this in full, smooth between (the fade steps follow)
  double coast_taper_from = 0.6;               // and the light tapers off over the last part of the coast (from this share of coast_frames to its end), not at a stroke
  double fade_near = light_influence::kFadeNearMetres;  // the light of a car fades out with its distance from the camera: in full up to here,
  double fade_far = light_influence::kFadeFarMetres;    // ... gone from here
};

class Tracker {
 public:
  Tracker() = default;
  explicit Tracker(const TrackerTuning& tuning) : tuning_(tuning) {}

  // The beam the reach is made for (the rig's settings: they change live).
  void SetBeam(double range, double outer_degrees) {
    tuning_.beam_range = range;
    tuning_.beam_outer_degrees = outer_degrees;
  }

  // A car seen in the frame being collected.
  void Observe(const Candidate& candidate) { seen_.push_back(candidate); }

  // The frame is over (guest frame `frame`, increasing by one a frame): matches the cars seen to the tracks, scores them, picks the lights for the next
  // frame (at most kMaxLights, the player's first, by key order among equals) and returns how many are in `out`.
  //
  // `view`: the frame's camera (light_influence.h), or null when this frame has none (no lens draw gave it, the setting is off): with it a car's score is its distance
  // times how much of its BEAM is in the view (ReachWeight), a car is lit by distance (DistanceFade), and a picked car that the game no longer draws because it left through the
  // picture's edge keeps its light, moved on by its own velocity, for up to coast_frames while its beam reaches the view. Without it the first version's rules: the lamp's place
  // on the screen (Candidate::on_screen) and a fade out of 8 frames as soon as the car is not seen.
  size_t EndFrame(uint64_t frame, std::array<Tracked, kMaxLights>* out, const light_influence::ViewFrustum* view = nullptr) {
    const bool influence = view != nullptr && view->valid;
    // 1. Match: the player first, then by distance to the camera; each track takes one car.
    std::vector<size_t> order(seen_.size());
    for (size_t i = 0; i < order.size(); ++i) {
      order[i] = i;
    }
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
      if (seen_[a].player != seen_[b].player) {
        return seen_[a].player;
      }
      return seen_[a].camera_distance < seen_[b].camera_distance;
    });
    std::vector<bool> taken(tracks_.size(), false);
    for (const size_t index : order) {
      const Candidate& car = seen_[index];
      size_t best = tracks_.size();
      double best_distance = tuning_.match_radius;
      for (size_t t = 0; t < tracks_.size(); ++t) {
        if (taken[t] || tracks_[t].player != car.player) {
          continue;
        }
        const double predicted[3] = {tracks_[t].position[0] + tracks_[t].velocity[0], tracks_[t].position[1] + tracks_[t].velocity[1],
                                     tracks_[t].position[2] + tracks_[t].velocity[2]};
        const double d[3] = {car.lamp.position[0] - predicted[0], car.lamp.position[1] - predicted[1], car.lamp.position[2] - predicted[2]};
        const double distance = Length3(d);
        if (distance < best_distance) {
          best_distance = distance;
          best = t;
        }
      }
      if (best == tracks_.size()) {
        Track fresh;
        fresh.key = next_key_++;
        fresh.player = car.player;
        tracks_.push_back(fresh);
        taken.push_back(true);
        best = tracks_.size() - 1;
        Update(&tracks_[best], car, frame, false);
      } else {
        taken[best] = true;
        Update(&tracks_[best], car, frame, tracks_[best].last_seen + 1 == frame);
      }
    }
    seen_.clear();
    // A track seen in this frame: how far out on the picture its lamp is (a car that goes out of the game's sight from there left through the edge). A track not seen moves on
    // by its velocity (its position is where the car is thought to be at the frame that just ended); a car that coasts slows a little each frame.
    for (Track& track : tracks_) {
      if (track.last_seen == frame) {
        track.unseen = 0;
        track.coast_ok = false;
        track.coasting = false;
        track.edge = influence && light_influence::NdcExtent(*view, track.position) >= tuning_.exit_ndc;
        continue;
      }
      ++track.unseen;
      for (int c = 0; c < 3; ++c) {
        track.position[c] += track.velocity[c];
      }
      if (influence && track.unseen == 1) {
        // The first frame the game did not draw it: did it leave through the edge (its last sighting was out on the picture, or it is thought to be out there now)?
        track.coast_ok = track.edge || light_influence::NdcExtent(*view, track.position) >= tuning_.exit_ndc;
      }
      if (track.coast_ok) {
        for (int c = 0; c < 3; ++c) {
          track.velocity[c] *= tuning_.coast_damping;
        }
      }
    }

    // 2. Score every track (a track not seen decays with its age) and pick.
    const double outer_radians = tuning_.beam_outer_degrees * kPi / 180.0;
    for (Track& t : tracks_) {
      if (influence) {
        t.distance = Distance3(t.position, view->eye);
        double axis[3];
        BeamAxisOf(t.forward, t.up, axis);
        t.reach = light_influence::BeamReach(*view, t.position, axis, tuning_.beam_range, outer_radians);
      } else {
        t.reach = 1.0;
      }
      const uint64_t age = frame - t.last_seen;
      double score;
      if (t.player) {
        score = 1.0e9;
      } else {
        score = 1.0 / (1.0 + (t.distance / tuning_.score_distance) * (t.distance / tuning_.score_distance));
        if (influence) {
          score *= light_influence::ReachWeight(t.reach);
        } else if (!t.on_screen) {
          score *= tuning_.off_screen;
        }
      }
      if (age > 0) {
        score *= std::pow(0.5, double(age) / (t.coast_ok ? tuning_.coast_linger : tuning_.linger));
      }
      if (t.picked) {
        score *= tuning_.keep_bonus;
      }
      t.score = score;
    }
    std::vector<size_t> ranked(tracks_.size());
    for (size_t t = 0; t < tracks_.size(); ++t) {
      ranked[t] = t;
    }
    std::stable_sort(ranked.begin(), ranked.end(), [&](size_t a, size_t b) {
      if (tracks_[a].score != tracks_[b].score) {
        return tracks_[a].score > tracks_[b].score;
      }
      return tracks_[a].key < tracks_[b].key;
    });
    for (Track& t : tracks_) {
      t.picked_now = false;
    }
    size_t picked = 0;
    for (const size_t t : ranked) {
      Track& track = tracks_[t];
      if (picked < kMaxLights && frame == track.last_seen) {  // a car seen this frame is newly picked, or kept
        track.picked_now = true;
        ++picked;
      } else if (picked < kMaxLights && CanCoast(track, influence)) {  // a car picked before that left the frame keeps its light while its beam is in the view
        track.picked_now = true;
        track.coasting = true;
        ++picked;
      } else if (picked < kMaxLights && track.picked && track.fade > 0.0f) {  // a picked one that is gone fades out in its slot
        track.picked_now = false;
        ++picked;
      }
    }

    // 3. Fades, and the output: every track with a fade left, the strongest four (the player's first). The target is 1 for a picked car, less with a distance beyond fade_near
    // and, for a car that coasts, less as its beam leaves the view (both are continuous, and the steps below follow them: no pop).
    std::vector<size_t> shown;
    for (const size_t t : ranked) {
      Track& track = tracks_[t];
      float target = 0.0f;
      if (track.picked_now) {
        double goal = 1.0;
        if (influence) {
          goal = light_influence::DistanceFade(track.distance, tuning_.fade_near, tuning_.fade_far);
          if (track.coasting) {
            goal *= light_influence::SmoothStep(tuning_.coast_reach_off, tuning_.coast_reach_full, track.reach);
            goal *= 1.0 - light_influence::SmoothStep(tuning_.coast_taper_from * double(tuning_.coast_frames), double(tuning_.coast_frames), double(track.unseen));
          }
        }
        target = float(goal);
      }
      if (target > track.fade) {
        track.fade = std::min(target, track.fade + tuning_.fade_in);
      } else {
        track.fade = std::max(target, track.fade - tuning_.fade_out);
      }
      track.picked = track.picked_now;
      if (track.fade > 0.0f) {
        shown.push_back(t);
      }
    }
    size_t count = 0;
    coasting_ = 0;
    for (const size_t t : shown) {
      if (count >= kMaxLights) {
        break;
      }
      const Track& track = tracks_[t];
      Tracked& light = (*out)[count++];
      light.key = track.key;
      light.player = track.player;
      for (int c = 0; c < 3; ++c) {
        light.position[c] = track.position[c] + track.velocity[c];
        light.forward[c] = track.forward[c];
        light.up[c] = track.up[c];
      }
      light.fade = track.fade;
      light.guest = track.guest;
      light.on = track.on;
      light.coasting = track.coasting;
      light.reach = track.reach;
      coasting_ += track.coasting ? 1u : 0u;
    }
    tracks_.erase(std::remove_if(tracks_.begin(), tracks_.end(),
                                 [&](const Track& t) { return frame - t.last_seen > tuning_.drop_after && t.fade <= 0.0f; }),
                  tracks_.end());
    return count;
  }

  void Clear() {
    tracks_.clear();
    seen_.clear();
    coasting_ = 0;
  }
  size_t track_count() const { return tracks_.size(); }
  // Lights of the last EndFrame that coast (for the census).
  uint32_t coasting_count() const { return coasting_; }

 private:
  struct Track {
    uint32_t key = 0;
    bool player = false;
    double position[3] = {};  // where the car is thought to be at the frame that just ended (the last sighting, moved on by the velocity)
    double observed[3] = {};  // the last sighting
    double velocity[3] = {};  // per frame
    double forward[3] = {0, 0, 1};
    double up[3] = {0, 1, 0};
    double distance = 0.0;
    bool on_screen = true;
    bool guest = false;
    double on = 1.0;
    uint64_t last_seen = 0;
    float fade = 0.0f;
    bool picked = false;      // was picked in the frame before
    bool picked_now = false;
    double score = 0.0;
    // the view's rules
    uint32_t unseen = 0;      // frames in a row without a sighting
    bool edge = false;        // the last sighting was out on the picture (NdcExtent >= exit_ndc)
    bool coast_ok = false;    // the car left through the edge: its light may go on without the game drawing it
    bool coasting = false;    // it does, in this frame
    double reach = 1.0;
  };

  static double Distance3(const double a[3], const double b[3]) {
    const double d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
    return Length3(d);
  }

  // A picked car the game stopped drawing: its light goes on while it left through the picture's edge, the car was lit, its beam is in the view and not too long has passed.
  bool CanCoast(const Track& t, bool influence) const {
    return influence && t.coast_ok && t.picked && t.unseen >= 1 && t.unseen <= tuning_.coast_frames && (!t.guest || t.on > 0.0) &&
           t.reach >= tuning_.coast_reach_off;
  }

  void Update(Track* t, const Candidate& car, uint64_t frame, bool consecutive) {
    double velocity[3] = {};
    if (consecutive) {
      for (int c = 0; c < 3; ++c) {
        velocity[c] = car.lamp.position[c] - t->observed[c];
      }
      const double speed = Length3(velocity);
      if (!(speed <= tuning_.max_step)) {
        velocity[0] = velocity[1] = velocity[2] = 0.0;
      } else {
        // Smoothed: the lamp's own jitter (a lens box that changes between frames) must not become a velocity.
        for (int c = 0; c < 3; ++c) {
          velocity[c] = 0.5 * velocity[c] + 0.5 * t->velocity[c];
        }
      }
    }
    for (int c = 0; c < 3; ++c) {
      t->position[c] = car.lamp.position[c];
      t->observed[c] = car.lamp.position[c];
      t->velocity[c] = velocity[c];
      t->forward[c] = car.lamp.forward[c];
      t->up[c] = car.lamp.up[c];
    }
    t->distance = car.camera_distance;
    t->on_screen = car.on_screen;
    t->guest = car.guest;
    t->on = car.on;
    t->last_seen = frame;
  }

  TrackerTuning tuning_;
  std::vector<Track> tracks_;
  std::vector<Candidate> seen_;
  uint32_t next_key_ = 1;
  uint32_t coasting_ = 0;
};

}  // namespace mcla::gpu::headlight_rules
