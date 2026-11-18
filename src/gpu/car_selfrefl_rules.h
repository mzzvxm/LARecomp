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
  float b[4];     // x steps, y debug view, z colour gain, w the share of the points that take the whole hit (the noise frame before variant version 4)
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
  float full = 0.25f;       // the share of the ray's points that, all inside a part, take the whole hit (FullPoints)
};

inline constexpr float kMinLength = 0.2f;
inline constexpr float kMaxLength = 6.0f;
inline constexpr float kMinThickness = 0.02f;
inline constexpr float kMaxThickness = 1.0f;
inline constexpr int kMinSteps = 4;
inline constexpr int kMaxSteps = 24;
inline constexpr int kMaxView = 4;
inline constexpr float kMaxGain = 16.0f;
inline constexpr float kMinFull = 0.05f;
inline constexpr float kMaxFull = 1.0f;

inline Tuning Clamped(Tuning t) {
  t.length = std::clamp(t.length, kMinLength, kMaxLength);
  t.thickness = std::clamp(t.thickness, kMinThickness, kMaxThickness);
  t.strength = std::clamp(t.strength, 0.0f, 1.0f);
  t.steps = std::clamp(t.steps, kMinSteps, kMaxSteps);
  t.view = std::clamp(t.view, 0, kMaxView);
  t.gain = std::clamp(t.gain, 0.0f, kMaxGain);
  t.full = std::clamp(t.full, kMinFull, kMaxFull);
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
                                          uint32_t history_slot, uint32_t sampler_slot, float exp_gain) {
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
  c.b[3] = t.full;
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
// The arithmetic the variant's helper does for the ray (shaderpack/variants/car_selfrefl.cpp: CarReflDistance, CarReflSlack,
// CarReflWindowShape, CarReflWindow, CarReflWindowIntegral, CarReflApproach, CarReflInWindow, CarReflHasOutline, CarReflMayCover,
// CarReflSpan, CarReflSegmentOf, CarReflFade, CarReflSeen, CarReflApply),
// written here with the same numbers so that car_selfrefl_rules_test.cpp can check what the picture depends on: that the weight
// follows the picture continuously (a patch whose neighbouring pixels differ by steps is a band), that a part narrower than the
// distance between two points counts in proportion to how much of the ray it takes and not all or nothing, and that a hit which
// may be the wrong face of a thin part cannot take the whole reflection. The shader keeps its own copy of these numbers (it cannot
// include this header): change one, change the other.
//
// The history is read as points, so "diff" below is how far BEHIND the vehicle pixel the ray point's view distance is (negative: in
// front of it, which is no hit). The ray is a chain of segments between consecutive points (the first starts at the shaded point,
// diff 0). What a segment takes is its COVER: the share of its length that lies inside the window of diff, by the line from the
// diff of its first point to the diff of its second (an exact integral, WindowIntegral), so a thin shell between two points
// counts for the part of the segment it takes and the hit does not jump when the ray's point steps over it.
//
// Where the ends of a segment disagree about the history's surface (one is in the window of a vehicle pixel, the other not, because
// the part's outline or a step in the picture's depth lies between them) the cover is not a line in diff: the outline's place is found
// by three halvings (kOutlineSteps: three more reads of the history, only for the segments that have such an outline in them) and the
// segment takes the share of its length that is on the window's side of it, so a point that enters or leaves a part's outline moves
// the weight by the length of the outline's way across the segment, which is continuous, and a part narrower than the segment
// takes the share it is wide. The ray's hit is the sum of the covers (times the fade with the distance, the hidden-face factor and
// the screen edge), and the whole hit takes `full` of the ray's points (3 of 12): a ray that goes through a part for three points'
// length takes all of it, and a sweep of the outline across a segment is a ramp, not a step (the "heaviest point" of the previous
// version, with its thin-part floor, jumped by 0.4 to 0.7 from pixel to pixel). There is no point to switch between: the hit's
// colour, face and distance are the covers' weighted means.

// HLSL's smoothstep.
inline float SmoothStep(float edge0, float edge1, float x) {
  const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

inline constexpr float kRayStartMin = 0.04f;  // m: no sample is nearer the shaded point than this
inline constexpr float kDark = 0.85f;         // the most of the reflection a hit takes away (the occlusion)
inline constexpr float kBackFloor = 0.5f;     // the weight of a hit the ray reached from behind the surface, against 1 from in front
inline constexpr float kShade = 0.4f;         // the paint's own colour in shade, for the face the camera does not see
inline constexpr int kOutlineSteps = 3;       // the halvings that find a part's outline in a segment: 1/8 of the segment
inline constexpr float kFadeFrom = 0.3f;      // the share of the ray's length from which the weight of what it finds falls...
inline constexpr float kFadeTo = 0.9f;        // ...to nothing at this one

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

// The window of a ray (made with the shaded point's view distance `depth`): the weight of "just behind a vehicle pixel", as a function
// of diff. 0 up to the bias (the game's depth noise and the slack of a slanted surface), a short ramp to 1, 1 up to half the thickness,
// a fall to 0 at the thickness (a ray that is far behind a thin shell has gone past it, not hit it). The two ramps never overlap (the
// bias is held under half the thickness less the ramp), so the window is the difference of two smoothsteps and has an exact integral.
struct WindowShape {
  float bias;       // the rise starts here
  float ramp;       // and ends this much later
  float fall;       // the fall starts here (half the thickness) and ends at the thickness
  float thickness;
};

inline WindowShape MakeWindowShape(float depth, float thickness, float slack) {
  const float ramp = std::min(0.02f + 0.004f * depth, 0.25f * thickness);
  const float bias = std::min(0.01f + 0.004f * depth + slack, 0.5f * thickness - ramp);
  return {bias, ramp, 0.5f * thickness, thickness};
}

inline float WindowWeight(float diff, const WindowShape& w) {
  return SmoothStep(w.bias, w.bias + w.ramp, diff) - SmoothStep(w.fall, w.thickness, diff);
}

// The integral of a smoothstep(edge0, edge1) from minus infinity to x: 0 below the ramp, a smooth curve in it, then the
// ramp's half plus the way past it.
inline float SmoothStepIntegral(float edge0, float edge1, float x) {
  const float width = edge1 - edge0;
  const float u = std::clamp((x - edge0) / width, 0.0f, 1.0f);
  return width * (u * u * u - 0.5f * u * u * u * u) + std::max(x - edge1, 0.0f);
}

// The integral of the window from minus infinity to diff (a constant past the thickness: the window's area).
inline float WindowIntegral(float diff, const WindowShape& w) {
  return SmoothStepIntegral(w.bias, w.bias + w.ramp, diff) - SmoothStepIntegral(w.fall, w.thickness, diff);
}

// 0..1: the mean of the window along a segment whose diff goes in a line from `diff0` to `diff1`: the share of the segment that is
// inside the window (the two integrals are of one function: the ray has one window).
inline float SegmentCover(float diff0, float diff1, const WindowShape& w) {
  const float d = diff1 - diff0;
  if (std::fabs(d) < 1e-4f) {
    return WindowWeight(0.5f * (diff0 + diff1), w);
  }
  return std::clamp((WindowIntegral(diff1, w) - WindowIntegral(diff0, w)) / d, 0.0f, 1.0f);
}

// 0..1: how much a ray point has come nearer to the vehicle pixel than the previous point was, when that one was behind it
// (previous_diff > 0; pass a negative one for a point in front of the surface or on no vehicle), from nothing to a tenth of the
// thickness, per kApproachSpan of the ray's length (`dt` is the distance between the two points, so a segment cut in two by the search
// for an outline has the same face as the whole one). A ray that is coming out of the back of a surface towards its front reaches it
// from behind: the face the picture does not hold (face = 1 - this). A ray that goes deeper behind the surface went in at the front: 0.
inline constexpr float kApproachSpan = 0.15f;  // m
inline float Approach(float previous_diff, float diff, float dt, float thickness) {
  return previous_diff > 0.0f ? SmoothStep(0.0f, 0.1f * thickness, (previous_diff - diff) * (kApproachSpan / std::max(dt, 1e-3f))) : 0.0f;
}

// One end of a segment: is it on a vehicle, how far behind that pixel it is, the pixel's view distance, and its distance along the ray.
struct SegmentEnd {
  bool on = false;
  float diff = 0.0f;
  float stored = 0.0f;  // the vehicle pixel's view distance (0 where there is none)
  float t = 0.0f;
};

// The end is in the window of a vehicle pixel (the window is open: diff is past the bias and short of the thickness): the side of an
// outline the ray's hit is on.
inline bool InWindow(const SegmentEnd& e, const WindowShape& w) { return e.on && e.diff > w.bias && e.diff < w.thickness; }

// The ends disagree about the surface in a way a line in diff does not say: one is in a vehicle's window and the other not, and
// either the other is on no vehicle or the vehicle's distance steps by more than half the thickness between them (a part in front of a
// door). A segment like that has an outline in it, and the caller looks for it (kOutlineSteps halvings).
inline bool HasOutline(const SegmentEnd& a, const SegmentEnd& b, const WindowShape& w) {
  return InWindow(a, w) != InWindow(b, w) && (!(a.on && b.on) || std::fabs(a.stored - b.stored) > 0.5f * w.thickness);
}

// Can a segment between two points on a vehicle take anything: does the line in diff between them pass through the open window? (A
// shortcut: where it cannot, the cover is 0 and the integral need not be made.)
inline bool MayCover(const SegmentEnd& a, const SegmentEnd& b, const WindowShape& w) {
  return a.on && b.on && std::max(a.diff, b.diff) > w.bias && std::min(a.diff, b.diff) < w.thickness;
}

// The share of a segment from the end that is in the window to the outline, after the halvings: the outline is taken in the middle of
// what is left between the last point known to be in the window and the last known not to be.
inline float OutlineShare(float t_in_end, float t_inner, float t_outer, float t_other_end) {
  const float whole = std::fabs(t_other_end - t_in_end);
  return whole > 0.0f ? std::clamp(std::fabs(0.5f * (t_inner + t_outer) - t_in_end) / whole, 0.0f, 1.0f) : 0.0f;
}

// What a segment takes of the ray: its cover (0..1), its face (1: the ray meets the visible side of what it finds, 0: it comes back
// out of the hidden side) and the share of its colour that is the second end's (the ends' windows weigh the colours of the two points).
struct Segment {
  float cover = 0.0f;
  float face = 1.0f;
  float colour_share = 1.0f;
};

// The segment from `a` to `b` (a nearer the shaded point) when both ends are on a vehicle: the mean of the window along the line in diff,
// by ONE window for the whole ray (the shaded point's depth: the ray stays within a metre or so of it, and the bias moves 4 mm a metre).
// A segment with an end on no vehicle takes nothing (the caller has looked for its outline if it has one in the window).
inline Segment SegmentOf(const SegmentEnd& a, const SegmentEnd& b, const WindowShape& w) {
  Segment s;
  if (a.on && b.on) {
    s.cover = SegmentCover(a.diff, b.diff, w);
    s.face = 1.0f - Approach(a.diff, b.diff, b.t - a.t, w.thickness);
    const float wa = WindowWeight(a.diff, w);
    const float wb = WindowWeight(b.diff, w);
    s.colour_share = wa + wb > 1e-4f ? wb / (wa + wb) : 0.5f;
  }
  return s;
}

// 1 up to kFadeFrom of the ray's length, 0 from kFadeTo: what the ray finds far away takes little of the reflection (the distance
// along the ray is the segment's middle as a share of the ray's length).
inline float RayFade(float share) { return 1.0f - SmoothStep(kFadeFrom, kFadeTo, share); }

// The sum of the covers (after the fade, the hidden-face factor and the screen edge) that takes the whole hit: `full` (the tuning's
// share, 0.25 by default) of the ray's points, at least one. A larger one makes every hit lighter and each point's step smaller.
inline float FullPoints(float full, int steps) { return std::max(full * float(steps), 1.0f); }

// What the segments add up to: `covers` is the sum of cover x fade x edge, `seen` the same times the face (so the hidden face's
// share is known), `middle` the same times the segment's middle distance, `colour` the same times the colour.
struct HitSum {
  float covers = 0.0f;
  float seen = 0.0f;
  float middle = 0.0f;
  float colour = 0.0f;  // one channel; the shader does the three
  void Add(const Segment& s, float a_t, float b_t, float ray_length, float edge, float a_colour, float b_colour) {
    const float mid = 0.5f * (a_t + b_t);
    const float c = std::clamp(s.cover, 0.0f, 1.0f) * RayFade(mid / ray_length) * edge;
    covers += c;
    seen += c * s.face;
    middle += c * mid;
    colour += c * (a_colour + (b_colour - a_colour) * s.colour_share);
  }
  // The covers' sum, the hidden face counting for kBackFloor.
  float Occupied() const { return covers * kBackFloor + seen * (1.0f - kBackFloor); }
  float Weight(float full, int steps) const { return std::clamp(Occupied() / FullPoints(full, steps), 0.0f, 1.0f); }
  float Face() const { return covers > 0.0f ? seen / covers : 1.0f; }
  float Distance(float ray_length) const { return covers > 0.0f ? middle / (covers * ray_length) : 0.0f; }
  float Colour() const { return covers > 0.0f ? colour / covers : 0.0f; }
};

// The reflection `env` with an occlusion of weight `weight` (the mode 1), and the colour a hit shows in the reflection mode: the hit
// pixel's own colour for the face the camera sees, the paint's own colour in shade for a hidden one.
inline float Darkened(float env, float weight) { return env * (1.0f - kDark * std::clamp(weight, 0.0f, 1.0f)); }
inline float SeenColour(float own, float hit_colour, float face) { return own * kShade + (hit_colour - own * kShade) * face; }

}  // namespace mcla::gpu::car_selfrefl
