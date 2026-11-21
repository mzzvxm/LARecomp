#pragma once
// ===========================================================================
// MCLA GPU renderer -- the volumetric light's stability rules (VOL-STABLE, 2026-11-20): the C++ twins of the shader changes and the host rules that keep the
// volume from flickering in motion. See volumetrics_stable_test.cpp for the numbers that say why each one is there.
// ===========================================================================
// The owner's words (2026-11-20): "with the lampposts, the volumetrics isn't super stable in motion". The causes, from the code and from the simulation of the
// passes in the test (a camera driving past a lamp, frame by frame), most to least:
//
//   1. THE LAMP'S POOL IS POINT SAMPLED IN DEPTH (lamps; any small light). A voxel took ONE sample of the lamp's term at a random point of its slice, and the
//      term 1 / (d^2 + near^2) of a street lamp is a pool only 6 to 8 m wide (near^2 = 0.618 x the game's intensity, 55 to 100). A slice is 12.7 per cent of
//      its distance thick on high (9 per cent on ultra): 5 m at 40 m, 10 m at 80 m. Past about 40 m the pool is narrower than the slice that holds it, so the
//      sample sees the pool's core or its tail by chance: the halo pulses each time the camera moves a slice (every few metres of driving) and the random
//      jitter turns the pulse into noise that the history only averages over some frames. The fix is to average the term over the slice, exactly: along a ray
//      the term integrates in closed form (an arc tangent), so the voxel takes the slice's own average and its value no longer depends on where in the slice
//      a sample fell. LampSliceFactor is its twin and the inject's lamp loop its shader.
//   2. THE COMPOSITE TAKES ONE DEPTH FOR A PIXEL (every effect). With multisampling the composite read the depth of the guest's sample 0 and gave that light
//      to every sample, so at a depth edge (a lamp post against the street behind it, a roof against the sky) the light is the near or the far one by a hard
//      flip that follows sample 0, and the edge crawls in motion. The fix reads every sample's depth, looks the volume up at each and averages (one lookup
//      where the samples agree within half a slice): an anti-aliased edge. DepthsAgree is its twin.
//   3. THE HISTORY'S WEIGHT FOLLOWS THE FRAME COUNT AND THE CAMERA'S STEP OF THE FRAME (every effect). 0.9 at rest and down to 0.5 for a fast move, per frame:
//      the noise of a frame (the jitter of the samples is the same size in every frame) grows as the weight falls, so it pumps with the steering, and the
//      history is one time constant at 60 FPS and a quarter of it at 240. The fix is a time constant: the weight is exp(-dt / tau) with the scene clock's dt,
//      tau 0.16 s at rest and 0.06 s in a fast move, and the move is smoothed over about a tenth of a second so that it does not pump (HistoryWeightFor,
//      HistoryMotion).
//   4. THE LAMPS' FADE AND THE HAZE'S DRIFT RAN ON A 60 FPS CLOCK (lamps entering or leaving the pick; the wind). `elapsed_s` was the frames since the last volume
//      over 60: at 240 FPS a lamp faded in 88 ms, not 350. It is the scene clock's time now (the wall clock, or a take's frames times its step).
//   5. THE CAMERA OF THE VOLUME IS THE PREVIOUS FRAME'S WHEN THE VERTEX CONSTANTS ARE NOT TRUSTED (every effect; the first frame of a move, or after a frame
//      that mismatched): the volume is one frame behind the picture. VOL-STABLE-2: the census of the takes says `camera this frame's in 697 of 697 volumes` (every
//      take of the first run): the vertex pose is trusted all through a drive, so this is NOT what is left of the chase's flicker; where it is not trusted the volume now
//      takes the previous camera moved on by its own last step (ExtrapolateCamera), which is a frame closer than the previous one at any steady move.
//   6. THE HEADLIGHTS (headlights only). Two things: (a) the shadow edge. A voxel is up to a slice thick (5 to 10 m at 40 to 80 m) and a shadow's edge is a hard step
//      inside it, but the voxel took ONE jittered sample, so an edge voxel flipped between lit and shadowed by its jitter and the history only averaged it over a few
//      frames. Now a shadowed headlight takes kHeadlightShadowTaps samples, one in each third of the slice with the frame's one offset (StratifiedDepth), and averages
//      them. (b) A MOVING light against the history. The history is the volume at the previous camera, reprojected: a light that moved 0.2 m (12 m/s) or 1 m (60 m/s)
//      leaves its old beam in the history, and a weight of 0.9 keeps it for 8 frames: a trail of 1.8 m at 12 m/s and 8 m at 60 m/s behind the beam. The second pass lowered
//      the history's weight where the light's own term changed by more than a few per cent; VOL-HL-STEADY (section 4b) takes the history's beam to the light's place instead
//      (CompensateHeadlightHistory): the weight stays, the trail is under 0.3 m up to 60 m/s, and a parked car's beam is untouched.
//
//   VOL-HL-STEADY (2026-11-20, from the takes of the second pass, no game run): the headlights alone kept 78 and 90 per cent of the first version's flicker (dolly-heads, pan-heads)
//   and the second pass's rule was the suspect. It is not what those two scenes measure: the car drives 24 m ahead at the camera's own speed, so a world point sees the beam move
//   0.2 m a frame and the rule changes the history's weight by under a tenth in the beam's interior (97 per cent of the beam's light has a weight of 0.81 to 0.9; 2 to 7 per cent
//   under 0.8: CheckSceneBite in volumetrics_stable_test.cpp), and the same holds for the compensation. What those two takes measure is the 8-bit floor: dolly-heads' mean
//   second difference is 0.0085 of one level of the x32 picture, the beam's total light fluctuates 0.05 per cent frame to frame and its centroid 0.09 pixel, and the share of
//   the metric that is a band's edge moving one level is 89 to 92 per cent (dolly-heads) and 56 (pan-heads, where the picture's own depth edges, 37 per cent of the sum, are
//   the rest). The rule and the compensation are for a beam that moves against the camera: a passing car (scenes\vol-hl-pass.scene).
//
//   WHAT THE FIRST RUN'S NUMBERS WERE (VOL-STABLE-2, from the takes, no game run): the chase's and the headlight-only flicker that the first run kept were mostly the test
//   and the view, not the volume. (i) The chase scene's camera is not smooth: the pole's image in the PICTURE take moves 0.25 to 1.1 pixels a frame (mean 0.69) in the same
//   order in the first version's run and the steady one, a sub-pixel jitter that every sharp glow and edge answers with a second difference; the rigid dolly's steps are
//   exact to 0.01 pixel and its flicker is 0.07 per cent. A chase run's floor is the camera's own. (ii) The headlights alone light 1.8 levels in the x8 view: 97.5 per
//   cent of the lit pixels are the same in two frames and 95 per cent of what the metric sums is within one level (the picture is 8 bits: a band's edge that moves a level),
//   the metric's own floor. What is above one level (`ex` of vol_stable_metric.py) fell from 0.125 to 0.008 per cent: 6 per cent of the first version's, where the metric's
//   px said 57. The view 3 (x32) and a camera that moves exactly (scenes\vol-stable-pan.scene) measure what is left. (iii) What the volume still does, from the simulation of
//   the passes: a lamp narrower than a slice (a pool of 6 m past 40 m) still pulses 1.7 per cent of its light as the camera passes a slice, because a surface in the lamp's own
//   slice (the pole) gets the cumulative light by a ramp across the slice; only a second value per voxel (where in the slice the light is) would take that away.
//
// NOT CAUSES (read in the code): the lamp list popping (volumetrics_lamps.h: lamps are keyed by their place, fade over kFadeSeconds, keep their slot while they
// fade and win a tie by 1.3x: nothing pops, only the 60 FPS clock of 4 was wrong); the history starting again too often (kCutStepMetres 25 m and 0.7 rad are
// PER FRAME: only a teleport or a cut gets there, a drive at 100 m/s steps 1.7 m); the height layer (its floor is smoothed by 2 per cent a frame and it varies
// over 40 m); the froxel resolution (the volume is smooth in x and y: the light it holds changes over metres, a voxel is 0.25 to 0.5 m at 30 m).
//
// This header includes only volumetrics_rules.h (no guest, no renderer, no GPU). Change a function here, change the shader that twins it.
// ===========================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "volumetrics_rules.h"

namespace mcla::gpu::volumetrics {

// ---------------------------------------------------------------------------
// the flags the host gives the passes (the shaders' own constants say the same: the rules test reads them)
// ---------------------------------------------------------------------------

// The inject's push (VolPush::slots.w): bit 0: the lamps' term is averaged over each slice (1 / the first version's point sample) and sampled at the voxel's centre in x
// and y, not at a jittered place (VOL-STABLE-2: the term is smooth across a voxel, so the jitter was noise and nothing else); bit 1: the headlights' rules (the stratified
// shadow taps and the history's weight under a light's motion).
inline constexpr uint32_t kInjectFlagSliceLamps = 1u;
inline constexpr uint32_t kInjectFlagSteadyHeads = 2u;
// The composite's push (VolApplyPush::slots.w): bits 0 to 7 the host sample that is the guest's sample 0, bit 8: every sample's depth is used (0: sample 0 only),
// bits 9 and 10 the debug view (0 the picture, 1 the volume's light alone, 2 the same times 8, 3 the same times 32).
inline constexpr uint32_t kApplyFlagAllSamples = 0x100u;
inline constexpr uint32_t kApplyViewShift = 9u;
inline constexpr uint32_t kApplyViewMask = 3u;
inline constexpr float kApplyViewGain = 8.0f;       // the debug view 2's multiplier
inline constexpr float kApplyViewGainHigh = 32.0f;  // the debug view 3's: the headlights alone are 1.8 levels in view 2, and the 8 bit steps of the picture are the floor of the metric
inline constexpr float ApplyViewGain(uint32_t view) { return view == 2u ? kApplyViewGain : (view == 3u ? kApplyViewGainHigh : 1.0f); }

inline constexpr uint32_t ApplyWord(uint32_t depth_sample, bool all_samples, uint32_t view) {
  return (depth_sample & 0xFFu) | (all_samples ? kApplyFlagAllSamples : 0u) | ((view & kApplyViewMask) << kApplyViewShift);
}

// ---------------------------------------------------------------------------
// 1. the lamp's term averaged over a slice
// ---------------------------------------------------------------------------

// The average over the ray's stretch [s0, s1] (metres from the camera along the unit ray) of 1 / (d(s)^2 + near^2), d(s) being the distance from the ray's
// point at s to the lamp. With s_c the ray's closest approach to the lamp and dmin its distance, d(s)^2 = (s - s_c)^2 + dmin^2, and with h^2 = dmin^2 + near^2
//   integral of ds / ((s - s_c)^2 + h^2) = (atan((s1 - s_c) / h) - atan((s0 - s_c) / h)) / h.
// A stretch with no length is the point value 1 / (d^2 + near^2).
inline float SliceAverageKernel(float s0, float s1, float s_c, float dmin_sq, float near_sq) {
  const float h_sq = dmin_sq + near_sq;
  const float h = std::sqrt(h_sq);
  const float len = s1 - s0;
  if (!(len > 1e-6f)) {
    const float d = 0.5f * (s0 + s1) - s_c;
    return 1.0f / (d * d + h_sq);
  }
  // atan(a) - atan(b) = atan2(a - b, 1 + a b): exact where the two arc tangents are nearly equal (a thin slice far from the closest approach)
  const float a = (s1 - s_c) / h;
  const float b = (s0 - s_c) / h;
  return std::atan2(len / h, 1.0f + a * b) / (h * len);
}

// What a lamp scatters toward the camera through the stretch [s0, s1] of the unit ray `ray` from `cam`, per unit of its colour x strength (the shader multiplies the
// colour, the strength and sigma x kLightGain): the twin of the lamp's SpotLight with the 1 / (d^2 + near^2) term averaged over the stretch. The cone, the window and the
// phase function are smooth over a stretch and are taken at its point nearest the lamp (the kernel's own peak), so that they move with the lamp.
inline float LampStretchFactor(const VolLamp& lamp, const float cam[3], const float ray[3], float s0, float s1, float g) {
  const float m[3] = {lamp.pos[0] - cam[0], lamp.pos[1] - cam[1], lamp.pos[2] - cam[2]};
  const float s_c = m[0] * ray[0] + m[1] * ray[1] + m[2] * ray[2];
  const float mm = m[0] * m[0] + m[1] * m[1] + m[2] * m[2];
  const float dmin_sq = std::max(mm - s_c * s_c, 0.0f);
  const float s_near = std::clamp(s_c, s0, s1);
  const float p[3] = {cam[0] + ray[0] * s_near, cam[1] + ray[1] * s_near, cam[2] + ray[2] * s_near};
  const float delta[3] = {p[0] - lamp.pos[0], p[1] - lamp.pos[1], p[2] - lamp.pos[2]};
  const float dist_sq = delta[0] * delta[0] + delta[1] * delta[1] + delta[2] * delta[2];
  if (!(dist_sq < lamp.range * lamp.range)) {
    return 0.0f;
  }
  const float dist = std::sqrt(dist_sq);
  const float inv = 1.0f / std::max(dist, 1e-4f);
  const float travel[3] = {delta[0] * inv, delta[1] * inv, delta[2] * inv};
  const float spot = SpotFactor(travel[0] * lamp.dir[0] + travel[1] * lamp.dir[1] + travel[2] * lamp.dir[2], lamp.cos_outer, lamp.cos_inner);
  if (!(spot > 0.0f)) {
    return 0.0f;
  }
  const float r = dist / lamp.range;
  const float w = std::clamp(1.0f - r * r * r * r, 0.0f, 1.0f);
  const float near_sq = LampNearSqOrDefault(lamp.pad[0]);
  const float kernel = SliceAverageKernel(s0, s1, s_c, dmin_sq, near_sq);
  const float phase = HenyeyGreenstein(-(travel[0] * ray[0] + travel[1] * ray[1] + travel[2] * ray[2]), g);
  return w * w * kernel * spot * phase;
}

// How many pieces a slice's stretch is cut into so that the cone, which the stretch factor takes at one point, does not change much inside a piece: a piece is at most
// `kPieceReach` times the pool's width h = sqrt(dmin^2 + near^2) long (the pool and the cone are about as wide as that), up to kMaxPieces.
inline constexpr float kPieceReach = 0.6f;
inline constexpr uint32_t kMaxPieces = 3;
inline uint32_t StretchPieces(float length, float h) {
  return std::clamp(uint32_t(std::ceil(length / std::max(kPieceReach * h, 1e-3f))), 1u, kMaxPieces);
}

// The slice's factor: the stretch cut into StretchPieces pieces, each averaged exactly and weighted by its length.
inline float LampSliceFactor(const VolLamp& lamp, const float cam[3], const float ray[3], float s0, float s1, float g) {
  const float m[3] = {lamp.pos[0] - cam[0], lamp.pos[1] - cam[1], lamp.pos[2] - cam[2]};
  const float s_c = m[0] * ray[0] + m[1] * ray[1] + m[2] * ray[2];
  const float mm = m[0] * m[0] + m[1] * m[1] + m[2] * m[2];
  // out of reach of the whole stretch: out of reach of every piece of it (the shader's cheap exit)
  const float s_near = std::clamp(s_c, s0, s1);
  const float e[3] = {cam[0] + ray[0] * s_near - lamp.pos[0], cam[1] + ray[1] * s_near - lamp.pos[1], cam[2] + ray[2] * s_near - lamp.pos[2]};
  if (!(e[0] * e[0] + e[1] * e[1] + e[2] * e[2] < lamp.range * lamp.range)) {
    return 0.0f;
  }
  const float h = std::sqrt(std::max(mm - s_c * s_c, 0.0f) + LampNearSqOrDefault(lamp.pad[0]));
  const float len = s1 - s0;
  const uint32_t pieces = StretchPieces(len, h);
  if (pieces == 1) {
    return LampStretchFactor(lamp, cam, ray, s0, s1, g);
  }
  float total = 0.0f;
  for (uint32_t i = 0; i < pieces; ++i) {
    const float a = s0 + len * float(i) / float(pieces);
    const float b = s0 + len * float(i + 1) / float(pieces);
    total += LampStretchFactor(lamp, cam, ray, a, b, g);
  }
  return total / float(pieces);
}

// The first version's lamp term at the point `p` seen along the unit ray `ray` (the twin of the inject's SpotLight for a lamp, no shadow): what one sample of
// a voxel gave. Kept for the A/B switch and for the tests that hold the slice average to it.
inline float LampPointFactor(const VolLamp& lamp, const float p[3], const float ray[3], float g) {
  const float delta[3] = {p[0] - lamp.pos[0], p[1] - lamp.pos[1], p[2] - lamp.pos[2]};
  const float dist_sq = delta[0] * delta[0] + delta[1] * delta[1] + delta[2] * delta[2];
  if (!(dist_sq < lamp.range * lamp.range)) {
    return 0.0f;
  }
  const float dist = std::sqrt(dist_sq);
  const float inv = 1.0f / std::max(dist, 1e-4f);
  const float travel[3] = {delta[0] * inv, delta[1] * inv, delta[2] * inv};
  const float spot = SpotFactor(travel[0] * lamp.dir[0] + travel[1] * lamp.dir[1] + travel[2] * lamp.dir[2], lamp.cos_outer, lamp.cos_inner);
  if (!(spot > 0.0f)) {
    return 0.0f;
  }
  return spot * LampFalloff(dist, lamp.range, lamp.pad[0]) * HenyeyGreenstein(-(travel[0] * ray[0] + travel[1] * ray[1] + travel[2] * ray[2]), g);
}

// The slice's bounds along a voxel's ray: the grid's depths z0 and z1 (metres along the view's z) times the length of the ray vector that has a forward part of 1.
inline void SliceBoundsAlongRay(const GridDesc& g, float far_m, uint32_t k, float ray_length, float* s0, float* s1) {
  *s0 = SliceBoundary(g.near_m, far_m, g.nz, float(k)) * ray_length;
  *s1 = SliceBoundary(g.near_m, far_m, g.nz, float(k) + 1.0f) * ray_length;
}

// ---------------------------------------------------------------------------
// 2. the composite's depths
// ---------------------------------------------------------------------------

// How far apart (as a share of the nearest) the depths of a pixel's samples may be and still be looked up once, at their mean: half a slice's thickness, where the
// volume is as good as linear.
inline float DepthAgreementTolerance(uint32_t nz, float ln_ratio) { return 0.5f * (std::exp(ln_ratio / float(nz)) - 1.0f); }

// Whether the `n` view depths t[] agree: the farthest is within `rel_tol` of the nearest.
inline bool DepthsAgree(const float* t, uint32_t n, float rel_tol) {
  float lo = 1e30f;
  float hi = 0.0f;
  for (uint32_t i = 0; i < n; ++i) {
    lo = std::min(lo, t[i]);
    hi = std::max(hi, t[i]);
  }
  return hi - lo <= rel_tol * lo;
}

// The light a pixel gets from `n` samples whose depths are t[] given `lookup(t)` for one depth: the one lookup at the mean where the depths agree, else the mean of
// the lookups (the box filter of an anti-aliased edge, what a resolve of per-sample shading gives).
template <class Lookup>
inline float AverageOverSamples(const float* t, uint32_t n, float rel_tol, Lookup&& lookup) {
  if (n <= 1) {
    return lookup(t[0]);
  }
  if (DepthsAgree(t, n, rel_tol)) {
    float mean = 0.0f;
    for (uint32_t i = 0; i < n; ++i) {
      mean += t[i];
    }
    return lookup(mean / float(n));
  }
  float sum = 0.0f;
  for (uint32_t i = 0; i < n; ++i) {
    sum += lookup(t[i]);
  }
  return sum / float(n);
}

// ---------------------------------------------------------------------------
// 3. the history's weight: a time constant, and a motion that does not pump
// ---------------------------------------------------------------------------

inline constexpr float kHistoryTauStill = 0.158f;   // s: exp(-1/60 / tau) = 0.9, the first version's weight at rest at 60 FPS
inline constexpr float kHistoryTauMoving = 0.058f;  // s: 0.75 at 60 FPS (the first version went down to 0.5)
inline constexpr float kHistoryMaxWeight = 0.98f;   // at a high frame rate: 0.98 is a history of about 50 frames
inline constexpr float kHistoryMinWeight = 0.5f;
inline constexpr float kHistoryMotionSeconds = 0.12f;  // the move is smoothed over this long
inline constexpr float kMotionFullSpeed = 180.0f;   // m/s: the first version's step / 3 m a frame at 60 FPS
inline constexpr float kMotionFullTurn = 6.0f;      // rad/s: its turn / 0.1 rad a frame at 60 FPS
inline constexpr float kMinFrameSeconds = 1.0f / 480.0f;
inline constexpr float kMaxFrameSeconds = 0.1f;

// The frame's time step as the weights use it (a gap beyond a tenth of a second is a hitch or a menu: the history is not trusted for longer than that).
inline float ClampFrameSeconds(float dt) { return std::clamp(dt, kMinFrameSeconds, kMaxFrameSeconds); }

// The move of the camera as 0 (still) to 1 (a fast move) from the metres and radians of the last frame's step in `dt` seconds.
inline float RawMotion(float step_m, float turn_rad, float dt) {
  const float d = ClampFrameSeconds(dt);
  return std::clamp(step_m / d / kMotionFullSpeed + turn_rad / d / kMotionFullTurn, 0.0f, 1.0f);
}

// The smoothed move: an exponential average over kHistoryMotionSeconds, so that a bump or a flick of the wheel does not change the history's weight for one frame.
inline float HistoryMotion(float previous, float raw, float dt) {
  const float k = 1.0f - std::exp(-ClampFrameSeconds(dt) / kHistoryMotionSeconds);
  return previous + (raw - previous) * k;
}

// The weight the history keeps: exp(-dt / tau), tau between kHistoryTauStill and kHistoryTauMoving by the (smoothed) move. 0 where the grid keeps no history
// or there is none to read.
inline float HistoryWeightFor(const GridDesc& g, bool valid, float dt, float motion) {
  if (!g.temporal || !valid) {
    return 0.0f;
  }
  const float tau = kHistoryTauStill + (kHistoryTauMoving - kHistoryTauStill) * std::clamp(motion, 0.0f, 1.0f);
  return std::clamp(std::exp(-ClampFrameSeconds(dt) / tau), kHistoryMinWeight, kHistoryMaxWeight);
}


// ---------------------------------------------------------------------------
// 4. the headlights: stratified shadow taps, and the history under a light's motion
// ---------------------------------------------------------------------------

// A shadowed headlight takes this many taps of its shadow map along the voxel's slice, one in each part of it (the frame's one offset `u` in 0..1 places them).
inline constexpr uint32_t kHeadlightShadowTaps = 3;

// The depth of tap `i` of `n`: the stretch [z0, z1] cut into n equal parts, the tap at the fraction `u` of its part. With u uniform every tap is uniform in its part and
// the n of them cover the slice: the edge of a shadow inside the slice takes a share of the taps by where it is, where one tap took all or none of the slice.
inline float StratifiedDepth(float z0, float z1, uint32_t i, uint32_t n, float u) { return z0 + (float(i) + u) / float(std::max(n, 1u)) * (z1 - z0); }

// What a headlight scatters toward the camera at `p` seen along the unit ray `ray`, per unit of its colour and strength, WITHOUT its shadow: the low beam's shape, the cone,
// the distance law and the phase function (the twin of the inject's SpotLight for a headlight with the shadow lookup left out).
inline float HeadlightTermUnshadowed(const VolLamp& l, const float p[3], const float ray[3], float g) {
  const float delta[3] = {p[0] - l.pos[0], p[1] - l.pos[1], p[2] - l.pos[2]};
  const float dist_sq = delta[0] * delta[0] + delta[1] * delta[1] + delta[2] * delta[2];
  if (!(dist_sq < l.range * l.range)) {
    return 0.0f;
  }
  const float dist = std::sqrt(dist_sq);
  const float inv = 1.0f / std::max(dist, 1e-4f);
  const float travel[3] = {delta[0] * inv, delta[1] * inv, delta[2] * inv};
  const float spot = SpotFactor(travel[0] * l.dir[0] + travel[1] * l.dir[1] + travel[2] * l.dir[2], l.cos_outer, l.cos_inner);
  if (!(spot > 0.0f)) {
    return 0.0f;
  }
  const float shape = HeadlightCutoff(travel[1]) * HeadlightNearFade(dist);
  if (!(shape > 0.0f)) {
    return 0.0f;
  }
  return spot * shape * LampFalloff(dist, l.range, l.pad[0]) * HenyeyGreenstein(-(travel[0] * ray[0] + travel[1] * ray[1] + travel[2] * ray[2]), g);
}

// ---------------------------------------------------------------------------
// 4b. a moving light against the history (VOL-HL-STEADY): the history's beam taken to where the light is now
// ---------------------------------------------------------------------------
//
// The history is the volume at the previous camera, so a light that moves leaves its old beam in it (1.8 m of trail at 12 m/s, 8 m at 60 m/s with a weight of 0.9). The
// second pass of VOL-STABLE answered with a rule that lowered the history's weight wherever the beam's own term had changed by a few per cent: it took the trail away
// and the smoothing with it (the headlights alone flickered at 78 to 90 per cent of the first version's, where the first pass had 47). The history was only wrong by how
// much the BEAM moved, and the beam is analytic: the history's headlight part is (about) colour x term(then) x visibility, so the volume takes its own old beam and makes it
// colour x term(NOW) x the same visibility, by the ratio of the two terms, which the inject can evaluate at the voxel (both sets of lights are in the constants). The history
// then averages the visibility (a shadow, the haze, the jitter) and follows the light exactly, so the weight stays where the time constant puts it.
//   old' = old + head_old x (ratio - 1),   head_old = clamp(old - base, 0, 1.25 x term(then) x sigma x gain),   ratio = luma(term(now)) / luma(term(then))
// `base` is this frame's light of everything but the headlights at the voxel (the haze, the sun, the lamps): the part of the old light the beam did not make. The bound on
// head_old (a visibility is at most 1) keeps the medium's own noise out of the correction where the beam is faint.
// Where the light does not exist yet at the voxel (ratio over kHeadCompFullRatio: the beam's leading edge, a light that came on) the old beam has nothing to be scaled from, and
// the weight falls to 0 by kHeadCompMaxRatio, but only where the beam is a real part of the voxel's light (kHeadCompSignificance of the rest): a fringe a hundredth of the haze
// is not worth a frame of noise. A light that went out has ratio 0: the old beam is taken out in one frame.
// What it does not do: the visibility is the old one. A shadow's edge that moves with the light (a pole's shadow sweeping a wall) trails by the history's length as it did
// before the rule; the weight's own time constant is all that bounds it.

inline constexpr float kHeadCompFullRatio = 2.0f;       // up to this ratio of the new term to the old one the history keeps its weight
inline constexpr float kHeadCompMaxRatio = 4.0f;        // from this ratio (and over) the history is dropped; the correction's own ratio is capped here
inline constexpr float kHeadCompOldBound = 1.25f;       // the old beam's part is at most this times the old term (a visibility is at most 1; the rest is the medium's difference)
inline constexpr float kHeadCompSignificance = 0.1f;    // a beam under this share of the voxel's other light does not lower the weight

inline float HeadlightLuma(const float c[3]) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }

// The ratio of the headlights' term now to the one the history was made with, from their luminances: 1 where neither lights the voxel, 0 where the light went, huge where it is new.
inline float HeadlightRatio(float luma_now, float luma_then) {
  if (!(luma_now > 0.0f)) {
    return luma_then > 0.0f ? 0.0f : 1.0f;
  }
  return luma_now / std::max(luma_then, 1e-20f);
}

// The weight's fall for a ratio: 1 up to kHeadCompFullRatio, 0 from kHeadCompMaxRatio.
inline float HeadCompWeightScale(float ratio) { return std::clamp((kHeadCompMaxRatio - ratio) / (kHeadCompMaxRatio - kHeadCompFullRatio), 0.0f, 1.0f); }

// The history's light at a voxel `old_rgb` taken to the light's place now (the twin of the inject's compensation): `base` this frame's light of everything but the headlights,
// `term_now` / `term_then` the headlights' unshadowed term summed over the set of this frame and of the volume the history holds (colour included, no sigma and no gain), `sigma_now`
// and `sigma_old` the extinctions, `gain` the scene units' multiplier (kLightGain times the strength). Writes the corrected light and returns the weight's multiplier (0 to 1).
inline float CompensateHeadlightHistory(const float old_rgb[3], const float base[3], const float term_now[3], const float term_then[3], float sigma_now, float sigma_old, float gain,
                                        float out_rgb[3]) {
  const float luma_now = HeadlightLuma(term_now);
  const float luma_then = HeadlightLuma(term_then);
  const float ratio = HeadlightRatio(luma_now, luma_then);
  const float used = std::min(ratio, kHeadCompMaxRatio);
  for (int c = 0; c < 3; ++c) {
    const float bound = term_then[c] * (kHeadCompOldBound * sigma_old * gain);
    const float head_old = std::clamp(old_rgb[c] - base[c], 0.0f, std::max(bound, 0.0f));
    out_rgb[c] = old_rgb[c] + head_old * (used - 1.0f);
  }
  const float fall = 1.0f - HeadCompWeightScale(ratio);
  const float beam = std::max(luma_now * sigma_now, luma_then * sigma_old) * gain;
  const float significance = std::clamp(beam / std::max(kHeadCompSignificance * HeadlightLuma(base), 1e-6f), 0.0f, 1.0f);
  return 1.0f - significance * fall;
}

// ---------------------------------------------------------------------------
// 5. the camera, when the vertex constants are not trusted
// ---------------------------------------------------------------------------

// The camera one frame on from two consecutive ones, `older` then `newer`: the position moved on by the last step, each axis by its last change and made a frame again (unit,
// at right angles). The previous camera is a frame behind the picture at any move; this one is behind only by the change of the move's speed. False (leave the camera as
// it is) for a step that is a cut or an axis that turned half way round.
inline bool ExtrapolateCamera(const VolCamera& older, const VolCamera& newer, VolCamera* out) {
  float step2 = 0.0f;
  for (int i = 0; i < 3; ++i) {
    const float d = newer.pos[i] - older.pos[i];
    step2 += d * d;
  }
  if (!(step2 <= 25.0f * 25.0f) || !std::isfinite(step2)) {
    return false;
  }
  const float cosine = newer.forward[0] * older.forward[0] + newer.forward[1] * older.forward[1] + newer.forward[2] * older.forward[2];
  if (!(cosine > 0.9f)) {
    return false;
  }
  VolCamera c = newer;
  float f[3];
  float r[3];
  for (int i = 0; i < 3; ++i) {
    c.pos[i] = newer.pos[i] + (newer.pos[i] - older.pos[i]);
    f[i] = newer.forward[i] + (newer.forward[i] - older.forward[i]);
    r[i] = newer.right[i] + (newer.right[i] - older.right[i]);
  }
  auto normalise = [](float v[3]) {
    const float n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (!(n > 1e-6f)) {
      return false;
    }
    for (int i = 0; i < 3; ++i) {
      v[i] /= n;
    }
    return true;
  };
  if (!normalise(f)) {
    return false;
  }
  // right made square to forward, then the up axis as right x forward (the camera's: right x up = backward)
  const float rf = r[0] * f[0] + r[1] * f[1] + r[2] * f[2];
  for (int i = 0; i < 3; ++i) {
    r[i] -= rf * f[i];
  }
  if (!normalise(r)) {
    return false;
  }
  const float u[3] = {r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]};
  for (int i = 0; i < 3; ++i) {
    c.forward[i] = f[i];
    c.right[i] = r[i];
    c.up[i] = u[i];
  }
  *out = c;
  return true;
}

}  // namespace mcla::gpu::volumetrics
