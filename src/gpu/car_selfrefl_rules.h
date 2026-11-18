#pragma once
// ===========================================================================
// MCLA GPU renderer -- car self-reflections (G8b prototype): the rules that need no renderer
// ===========================================================================
// The test cvar's modes, the tuning numbers, the history matrix and the 112 bytes the car paint variant reads
// (shaderpack/variants/car_selfrefl.h), as pure functions on plain numbers, so that car_selfrefl_rules_test.cpp builds
// on its own. The renderer's half is car_selfrefl.h. This header includes nothing of the renderer.
// ===========================================================================

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "temporal_math.h"

namespace mcla::gpu::car_selfrefl {

// mcla_gpu_car_selfrefl_test: 0 the game's paint, 1 the reflection darkened where the car's own geometry blocks it, 2 the
// darkening and the car's own colour in the reflection.
enum class Mode : uint8_t { kOff = 0, kOcclusion = 1, kReflection = 2 };

constexpr Mode ModeOfTest(int value) {
  return value <= 0 ? Mode::kOff : (value == 1 ? Mode::kOcclusion : Mode::kReflection);
}

// The 112 bytes after the SharedConstants of a draw of the paint variant: g_CarReflM0..M3, g_CarReflA, g_CarReflB and
// g_CarReflC of shaderpack/variants/car_selfrefl.cpp, byte 608 on.
struct CarSelfReflConstants {
  // The world position of this frame, as the paint's vertex shader gives it, to the clip space of the frame the history
  // image was made from: the rows of a row-vector matrix (clip = p.x m[0..3] + p.y m[4..7] + p.z m[8..11] + m[12..15]),
  // the vehicle's own motion in it.
  float m[16];
  float a[4];     // x mode, y ray length (m), z thickness (m), w strength
  float b[4];     // x steps, y debug view, z colour gain, w the noise frame (not read since variant version 3: no per-pixel noise)
  uint32_t c[4];  // x the history image's bindless slot, y a linear clamp sampler's slot, zw reserved
};
static_assert(sizeof(CarSelfReflConstants) == 112);
static_assert(offsetof(CarSelfReflConstants, a) == 64 && offsetof(CarSelfReflConstants, b) == 80 &&
              offsetof(CarSelfReflConstants, c) == 96);

// The tuning cvars (mcla_gpu_car_selfrefl_*), already read.
struct Tuning {
  float length = 2.0f;      // the ray's length in the game's world units (about metres)
  float thickness = 0.3f;   // how far behind a surface a point still hits it (the weight falls from half of it to 0 at it)
  float strength = 1.0f;    // the most the hit takes of the reflection, 0 to 1
  int steps = 12;           // points on the ray, closer together near the shaded point (RayDistance)
  int view = 0;             // the debug view, 0 none
  float gain = 1.0f;        // times the exponent bias's, for the history's colour units (CHECK)
};

inline constexpr float kMinLength = 0.2f;
inline constexpr float kMaxLength = 6.0f;
inline constexpr float kMinThickness = 0.02f;
inline constexpr float kMaxThickness = 1.0f;
inline constexpr int kMinSteps = 4;
inline constexpr int kMaxSteps = 24;
inline constexpr int kMaxView = 4;
inline constexpr float kMaxGain = 16.0f;

inline Tuning Clamped(Tuning t) {
  t.length = std::clamp(t.length, kMinLength, kMaxLength);
  t.thickness = std::clamp(t.thickness, kMinThickness, kMaxThickness);
  t.strength = std::clamp(t.strength, 0.0f, 1.0f);
  t.steps = std::clamp(t.steps, kMinSteps, kMaxSteps);
  t.view = std::clamp(t.view, 0, kMaxView);
  t.gain = std::clamp(t.gain, 0.0f, kMaxGain);
  return t;
}

// What the history's colour is multiplied by to be in the paint's units. The history is the scene colour as the target
// holds it, which is the shaders' output times 2^bias (RT0's exponent bias: constants.cpp, the hardware multiplies the
// colour by it after the shader), and the paint's own colours (and the environment reads it mixes with) are the output.
// MCLA's main target has a bias of 4 (the paint's gInvColorExpBias.x is 0.0625). CHECK in a picture: if the self-reflection
// is too bright or too dark, mcla_gpu_car_selfrefl_gain moves it.
inline float ExpBiasGain(int32_t exp_bias) {
  return std::ldexp(1.0f, -std::clamp<int32_t>(exp_bias, -24, 24));
}

// The matrix a vertex of the draw is taken into the history frame's clip space with: the vehicle's own motion (this
// frame's world position to its world position one frame ago, vehicle_motion.h; null: the identity) and then the
// history frame's view-projection, row vectors, written row by row.
inline void HistoryMatrix(const temporal::Mat4& view_projection, const temporal::Mat4* motion, float out[16]) {
  const temporal::Mat4 m = motion ? temporal::Mul(*motion, view_projection) : view_projection;
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      out[r * 4 + c] = float(m.m[r][c]);
    }
  }
}

// How much smaller than the render size the history image is, in each direction: 1 the picture's size, 2 half of it. The
// reflection of the car's own body is blurred by the paint's own noise, so the image is made at half size where that costs
// the look nothing the eye can see and the memory and the bandwidth are a quarter: for the occlusion-only test level (the
// specular occlusion only: a darkening, no colour) at any size, and for the full effect at 2560 pixels across and more (the render
// scale 2 of the game's 1280 and up, where the car's silhouette is wider than the image's texel by far). `forced` is the debug
// override mcla_gpu_car_selfrefl_history: 0 the rule, 1 the full size, 2 half. The image holds the vehicle's distance and
// colour only (see hlsl/car_history_ps.hlsl): at half size one texel stands for the 2x2 block's lower right pixel.
inline constexpr uint32_t kHalfHistoryFromWidth = 2560;
constexpr uint32_t HistoryDivisor(Mode mode, uint32_t render_width, int forced) {
  if (forced == 1 || forced == 2) {
    return uint32_t(forced);
  }
  return (mode == Mode::kOcclusion || render_width >= kHalfHistoryFromWidth) ? 2u : 1u;
}
// The image's edge for a render edge and a divisor (rounded up: the last texel covers a partial block).
constexpr uint32_t HistoryEdge(uint32_t render_edge, uint32_t divisor) {
  return divisor <= 1 ? render_edge : (render_edge + divisor - 1) / divisor;
}

// The constants of a draw that has no history: the game's paint (mode 0) and a slot that is never read.
inline CarSelfReflConstants OffConstants() {
  CarSelfReflConstants c = {};
  return c;
}

inline CarSelfReflConstants MakeConstants(Mode mode, const Tuning& tuning, const float matrix[16],
                                          uint32_t history_slot, uint32_t sampler_slot, float exp_gain,
                                          float noise_frame) {
  CarSelfReflConstants c = {};
  for (int i = 0; i < 16; ++i) {
    c.m[i] = matrix[i];
  }
  const Tuning t = Clamped(tuning);
  c.a[0] = float(uint8_t(mode));
  c.a[1] = t.length;
  c.a[2] = t.thickness;
  c.a[3] = t.strength;
  c.b[0] = float(t.steps);
  c.b[1] = float(t.view);
  c.b[2] = exp_gain * t.gain;
  c.b[3] = noise_frame;
  c.c[0] = history_slot;
  c.c[1] = sampler_slot;
  return c;
}

// The view distance a stored depth (1 near, 0 far) stands for under the projection `p` (row vectors, view space to clip,
// the camera looking down -z): ao_common.hlsli AoLinearDepth's formula, which car_history_ps.hlsl uses. For a perspective
// projection with p[2][3] = -1 and p[3][3] = 0 it is the w of the clip position, which the paint variant divides by.
inline double LinearDepth(double stored, const temporal::Mat4& p) {
  const double ndc = 1.0 - std::clamp(stored, 0.0, 1.0);
  const double den = p.m[2][2] - ndc * p.m[2][3];
  return den != 0.0 ? (p.m[3][2] - ndc * p.m[3][3]) / den : 0.0;
}

// ---------------------------------------------------------------------------------------------------------------------
// The ray of the paint variant, as plain functions
// ---------------------------------------------------------------------------------------------------------------------
// The arithmetic the variant's helper does for each ray point (shaderpack/variants/car_selfrefl.cpp: CarReflDistance,
// CarReflSlack, CarReflWindow, CarReflApproach, CarReflThin, CarReflSeen, CarReflApply), written here with the same numbers so that
// car_selfrefl_rules_test.cpp can check what the picture depends on: that the ray's points and weights do not jump from pixel to
// pixel (a dotted patch is a weight that does), and that a hit which may be the wrong face of a thin part cannot take the whole
// reflection. The shader keeps its own copy of these numbers (it cannot include this header): change one, change the other.
// The history is read as points, so "diff" below is how far BEHIND the vehicle pixel the ray point's view distance is (negative:
// in front of it, which is no hit).

// HLSL's smoothstep.
inline float SmoothStep(float edge0, float edge1, float x) {
  const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

inline constexpr float kRayStartMin = 0.04f;  // m: no sample is nearer the shaded point than this
inline constexpr float kDark = 0.85f;         // the most of the reflection a hit takes away (the occlusion)
inline constexpr float kBackFloor = 0.5f;     // the weight of a hit the ray reached from behind the surface, against 1 from in front
inline constexpr float kThinFloor = 0.6f;     // the weight of a hit made by one sample or small ones, against 1 for a wide one
inline constexpr float kShade = 0.4f;         // the paint's own colour in shade, for the face the camera does not see

// The distance along the ray of sample `index` (0 to steps - 1) of `steps`: the samples are closer together near the shaded
// point (the spacing grows with the distance: t = length (u^1.5), u = (index + 1) / steps), and the same for every pixel and frame.
inline float RayDistance(int index, int steps, float length) {
  const float u = float(index + 1) / float(steps);
  return std::max(length * u * std::sqrt(u), kRayStartMin);
}

// How far off the stored distance of a slanted surface can be, in metres (added to the bias of the hit test): the history holds the
// distance at a texel's centre, up to a texel from where a ray point projects, and a surface seen with N.V = nv changes its distance
// by about sqrt(1 - nv^2) / nv metres per metre across the picture. `focal` is the projection's scale (the length of the history
// matrix's y column, 1 / tan(half the vertical field of view)); `history_height` the image's height in texels.
inline float SurfaceSlack(float depth, float history_height, float focal, float nv) {
  const float texel = depth * 2.0f / (history_height * std::max(focal, 0.001f));
  const float slope = std::min(std::sqrt(std::max(1.0f - nv * nv, 0.0f)) / std::max(nv, 0.1f), 8.0f);
  return std::min(1.5f * texel * slope, 0.05f);
}

// 0..1: how much a ray point `diff` metres behind a vehicle pixel (view distance `depth`) is "just behind" it. 0 up to the bias
// (the game's depth noise and the slack), a short ramp to 1, 1 up to half the thickness, a fall to 0 at the thickness: a ray that
// is far behind a thin shell has gone past it, not hit it.
inline float WindowWeight(float diff, float depth, float thickness, float slack) {
  const float bias = 0.01f + 0.004f * depth + slack;
  const float ramp = std::min(0.02f + 0.004f * depth, 0.25f * thickness);
  return SmoothStep(bias, bias + ramp, diff) * (1.0f - SmoothStep(0.5f * thickness, thickness, diff));
}

// 0..1: how much a ray point has come nearer to the vehicle pixel than the previous point was, when that one was behind it
// (previous_diff > 0; pass a negative one for a point in front of the surface or on no vehicle), from nothing to a tenth of the
// thickness. A ray that is coming out of the back of a surface towards its front reaches it from behind: the face the picture
// does not hold (face = 1 - this). A ray that goes deeper behind the surface went in at the front: 0.
inline float Approach(float previous_diff, float diff, float thickness) {
  return previous_diff > 0.0f ? SmoothStep(0.0f, 0.1f * thickness, previous_diff - diff) : 0.0f;
}

// The weight of the sum of the samples' window weights: one full sample is a thin part or the edge of one that the samples may have
// caught or missed (kThinFloor to 0.73), two samples and a half and more are a part the ray goes along (1).
inline float ThinFactor(float sum) { return kThinFloor + (1.0f - kThinFloor) * std::clamp((sum - 0.5f) / 1.5f, 0.0f, 1.0f); }

// The weight a sample of window weight `window` at share `share` of the ray's length (0..1) has as the hit, `face` as above.
inline float PointWeight(float window, float share, float face) {
  return window * (1.0f - SmoothStep(0.55f, 1.0f, share)) * (kBackFloor + (1.0f - kBackFloor) * face);
}

// The reflection `env` with an occlusion of weight `weight` (the mode 1), and the colour a hit shows in the reflection mode: the hit
// pixel's own colour for the face the camera sees, the paint's own colour in shade for a hidden one.
inline float Darkened(float env, float weight) { return env * (1.0f - kDark * std::clamp(weight, 0.0f, 1.0f)); }
inline float SeenColour(float own, float hit_colour, float face) { return own * kShade + (hit_colour - own * kShade) * face; }

}  // namespace mcla::gpu::car_selfrefl
