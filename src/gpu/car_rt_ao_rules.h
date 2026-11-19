#pragma once
// ===========================================================================
// MCLA GPU renderer -- ray traced ambient occlusion and contact shadows around the player's car: the rules that need no renderer
// ===========================================================================
// The screen-space occlusion (ao.h) and the contact march (hlsl/ao_contact_ps.hlsl) work from the picture's depth, so they miss what the
// camera does not see: the far side of a wheel well, the body over a tyre, the road at the foot of a car whose underside hides it.
// On a device that can trace rays against the car's triangles (car_rt_scene.h: one top level structure of the player's car in the
// frame's VIEW space) a compute pass (hlsl/car_rt_ao_cs.hlsl, car_rt_ao.h) casts a few SHORT rays from every pixel near the car:
//
//   occlusion rays   N cosine weighted rays over the hemisphere of the pixel's surface, each as long as a random share of the reach
//                    (kDefaultReach, about 0.6 m), against the car's body and tyres. The visibility is 1 minus the share of rays that
//                    hit anything. The length is the trick: a ray of length L (1 - sqrt(u)) with u uniform is stopped by a blocker at
//                    distance d with probability (1 - d / L)^2, so the plain yes/no answers of N first-hit queries average to an
//                    occlusion that falls off with the blocker's distance (no nearest hit is needed, so the queries end at the first
//                    triangle they find), and a blocker at the reach counts for nothing. The stratified lengths and the rotated
//                    pattern are the variance control; the occlusion's own history and blur (ao.h) take the rest out. The
//                    result goes into the main pass as min(screen-space visibility, this visibility raised to the look's power).
//   sun ray          one ray toward the sun, spread over the sun's disc (kSunDiscTan), against the same parts: where it hits, the
//                    pixel is in the car's shadow. The contact march takes the darker of its own result, the car's shadow map's and
//                    this one (hlsl/ao_contact_ps.hlsl, car_shadow_map.h): the shadow of the wing on the lid, of the car on the road,
//                    exact, with no map resolution, no bias and no peter-panning.
//
// THE IMAGE (CarRtAo's, the size of the occlusion's traced image, R8G8B8A8_UNORM): r the occlusion's open share (1 = nothing in the
// reach), g the sun's open share (1 = lit), b 1 where the pixel was traced (else 0, with r and g 1), a 1. Every pixel is written every
// frame, so a consumer needs no rectangle: outside the car's region the answer is "open".
//
// WHERE: only for pixels near the car. The host gives the pass the car's box in view space (ViewBoxFor: the frame's world box through the
// camera, with the camera's z made forward as car_rt_scene.cpp makes it) and the pixel rectangle that can hold such pixels
// (ScreenRect: the box, grown by the reach, and its extrusion toward the sun); the shader tests each pixel's view position against the
// box (occlusion) and each sun ray against it (a slab test) and traces nothing else.
//
// THE PIXEL'S SURFACE. A road pixel is placed by the depth (view position, depth normal) and lifted off its surface by a few pixel
// footprints. A vehicle's pixel is first snapped onto the car's real triangles by a primary ray from the camera in a window around the
// depth (as hlsl/car_rt_refl_cs.hlsl does): the game's depth and the triangles differ by a centimetre or so, and a ray that starts under
// the surface would find the car's own panel at once. A hit there gives the exact point and the mesh's normal; a ray that later hits the
// panel it left (the same instance and its triangle, or a surface facing the way the point's normal does) counts as a miss (IsSelfHit).
//
// The numbers below are repeated by the shader (hlsl/car_rt_ao_cs.hlsl, ao_main_ps.hlsl, ao_contact_ps.hlsl); car_rt_ao_rules_test.cpp
// checks the arithmetic and, given the src/gpu folder, that the shaders repeat the flags. Change them here first.
//
// This header includes nothing of the renderer.
// ===========================================================================

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string_view>

#include "ao_stable.h"
#include "car_rt_instances_rules.h"
#include "car_rt_scene_rules.h"
#include "temporal_math.h"

namespace mcla::gpu::car_rt_ao {

// ---------------------------------------------------------------------------------------------------------------------
// The setting
// ---------------------------------------------------------------------------------------------------------------------
// mcla_gpu_ao_rt (debug only): auto (the default: both terms, where the device can trace the car and the effect they belong to is on) | off |
// ao (the occlusion term only) | contact (the sun ray only). A word that is none of these counts as auto, as the graphics settings' do.
enum class Setting : uint8_t { kAuto, kOff, kOcclusion, kContact };
inline Setting SettingOf(std::string_view text) {
  if (text == "off") return Setting::kOff;
  if (text == "ao") return Setting::kOcclusion;
  if (text == "contact") return Setting::kContact;
  return Setting::kAuto;
}

struct Terms {
  bool occlusion = false;
  bool contact = false;
  bool any() const { return occlusion || contact; }
};
// What the pass computes in a frame. `device_supports`: the device can trace the car (CarRtSupported) and the pass's layout and shader exist.
// `occlusion_on`: the occlusion is on in this frame (a level other than off, and its own pipelines built). The bounce light's pass takes the traced
// term too (hlsl/ao_main_ps.hlsl THE TRACED TERM: the same rule on its visibility channel), so the occlusion term does not depend on it.
// `contact_on`: the contact shadows are active in this frame (on, and not gated off by a low sun or rain).
inline Terms Wanted(Setting setting, bool device_supports, bool occlusion_on, bool contact_on) {
  Terms t;
  if (!device_supports || setting == Setting::kOff) {
    return t;
  }
  t.occlusion = occlusion_on && setting != Setting::kContact;
  t.contact = contact_on && setting != Setting::kOcclusion;
  return t;
}

// ---------------------------------------------------------------------------------------------------------------------
// The rays
// ---------------------------------------------------------------------------------------------------------------------
// Occlusion rays a pixel: the occlusion's level (high: 2, ultra: 4). The occlusion's own history averages 12 frames of them.
inline constexpr uint32_t kRaysHigh = 2;
inline constexpr uint32_t kRaysUltra = 4;
inline constexpr uint32_t kMaxRays = 4;
inline uint32_t OcclusionRays(bool ultra) { return ultra ? kRaysUltra : kRaysHigh; }

// The reach of the occlusion rays in metres (mcla_gpu_ao_rt_length): what a car blocks within it, and no more, so the road gets the dark
// line at the tyre and the foot of the car and no halo (the screen-space term's car reach, ao.h, is 1.2 m).
inline constexpr float kDefaultReach = 0.6f;
inline constexpr float kMinReach = 0.2f;
inline constexpr float kMaxReach = 2.0f;
inline float ClampedReach(float metres) {
  return std::isfinite(metres) ? std::clamp(metres, kMinReach, kMaxReach) : kDefaultReach;
}
// The sun ray's reach: the car's shadow on the road is its height over the tangent of the sun's elevation (1.4 m at 12 degrees, the lowest
// sun the contact shadows run in, is 6.6 m), and the box test ends a ray that cannot reach the car at once.
inline constexpr float kSunReach = 8.0f;
// The tangent of the sun disc's angular radius, a little over the real 0.0047 so that a single ray a pixel has a softer, steadier edge.
inline constexpr float kSunDiscTan = 0.007f;
// A surface this nearly edge-on to the sun gets no ray (its sun share is about 0 too: contact_trace.h kSunFacingLow).
inline constexpr float kSunFacingLow = 0.03f;
// The least cosine a ray keeps above its pixel's tangent plane: the cosine weighted pattern has few rays under 5 degrees, and the depth's normal
// is a few degrees off the triangle's, so the rays that would run under the surface are lifted.
inline constexpr float kMinAbove = 0.08f;
// A hit on the instance the primary ray found, facing within this cosine of the point's normal (32 degrees), is the panel itself.
inline constexpr float kSelfCos = 0.85f;
// The parts the rays hit: the body and the tyres of the player's car (hlsl/car_rt_common.hlsli CAR_RT_MASK_*); glass lets the light through, the
// driver is not in the structures. The primary ray finds the same parts.
inline constexpr uint32_t kRayParts = car_rt_rules::kMaskBody | car_rt_rules::kMaskTyres;
// With the other cars in the top level (car_rt_instances.h: every instance of another car has the mask kMaskOthers, 0x10, whatever the part), the
// rays see them too, so the road between two cars is darkened by both: the player's body and tyres plus the others' parts, 0x15. NOT 0xFF, which
// would add the player's own glass (0x02) and driver (0x08) that this pass leaves out, and NOT 0x0F, which is the player's car alone. The primary
// ray that snaps a vehicle's pixel onto its triangles takes the same mask, so another car's pixel is snapped onto its own triangles as the
// player's is (a ray that starts under its own panel finds the panel).
inline constexpr uint32_t kRayPartsWithOthers = kRayParts | car_rt_rules::kMaskOthers;

// How far a ray's origin is lifted off its surface along the normal (metres). A pixel snapped onto the triangles by the primary ray is exactly on
// them (the reflection pass lifts 4 mm + 2 mm a metre); a pixel placed by the depth alone is within a few footprints of its surface.
inline float SnappedLift(float t) { return 0.004f + 0.002f * t; }
inline constexpr float kDepthLiftFloor = 0.006f;
inline constexpr float kDepthLiftPixels = 3.0f;
inline float DepthLift(float t, float footprint_unit) { return kDepthLiftFloor + kDepthLiftPixels * footprint_unit * t; }

// ---------------------------------------------------------------------------------------------------------------------
// The pattern (the twin of hlsl/car_rt_ao_cs.hlsl's)
// ---------------------------------------------------------------------------------------------------------------------
struct Vec3 {
  float x = 0.0f, y = 0.0f, z = 0.0f;
};

inline constexpr float kTwoPi = 6.28318530717959f;
inline constexpr float kGolden = 0.6180339887f;  // ao_stable.h kStepGolden

// A pixel's noise (interleaved gradient noise, Jimenez 2014: the occlusion's own, ao_common.hlsli AoNoise) and the frame's shift.
inline float PixelNoise(float x, float y) {
  const float f = 0.06711056f * x + 0.00583715f * y;
  const float g = f - std::floor(f);
  const float h = 52.9829189f * g;
  return h - std::floor(h);
}

// Ray `ray` of `rays` over the cosine weighted hemisphere, in the surface's own frame (z along the normal): the radius squared is stratified
// by the ray's number (an offset `noise_y` into its stratum) and the angle turns by the golden angle a ray from `noise_x`.
inline Vec3 CosineLocal(uint32_t ray, uint32_t rays, float noise_x, float noise_y) {
  const float r2 = (float(ray) + std::clamp(noise_y, 0.0f, 0.9999f)) / float(std::max(rays, 1u));
  const float angle = kTwoPi * (noise_x + kGolden * float(ray));
  const float r = std::sqrt(r2);
  return {r * std::cos(angle), r * std::sin(angle), std::sqrt(std::max(1.0f - r2, 0.0f))};
}

// The stratum a ray's length comes from: not the direction's, so that the long rays are not the grazing ones, and turned by `rotation` (0 to
// rays - 1, from the pixel's noise) so that over the pixels and the frames every direction meets every length with the same chance: the estimate
// has no bias from the pairing.
inline uint32_t LengthStratum(uint32_t ray, uint32_t rays, uint32_t rotation) {
  return (ray * 3u + 1u + rotation) % std::max(rays, 1u);
}
// The share u of the ray's length, 0 to 1, from its stratum and an offset `jitter` into it.
inline float LengthShare(uint32_t ray, uint32_t rays, uint32_t rotation, float jitter) {
  return (float(LengthStratum(ray, rays, rotation)) + std::clamp(jitter, 0.0f, 0.9999f)) / float(std::max(rays, 1u));
}
// The rotation of the pairing from the pixel's noise (the shader's).
inline uint32_t LengthRotation(float noise_x, float noise_y, uint32_t rays) {
  const float f = noise_x * 13.7f + noise_y * 5.3f;
  const float g = f - std::floor(f);
  return std::min(uint32_t(g * float(rays)), std::max(rays, 1u) - 1u);
}
// The ray's length: P(length >= d) = (1 - d / reach)^2 for u uniform.
inline float RayLength(float reach, float u) { return reach * (1.0f - std::sqrt(std::clamp(u, 0.0f, 1.0f))); }
// The weight a blocker at distance d has on the expected occlusion (the probability a ray is long enough to reach it).
inline float BlockerWeight(float d, float reach) {
  const float a = std::clamp(1.0f - d / std::max(reach, 1e-6f), 0.0f, 1.0f);
  return a * a;
}

// A direction in the cone of the sun disc around `to_sun` (unit): `u1`, `u2` uniform, the disc's radius `tan_radius`.
inline Vec3 SunDirection(const Vec3& to_sun, float u1, float u2, float tan_radius) {
  // an orthonormal frame around to_sun (Duff et al. 2017)
  const float sign = to_sun.z >= 0.0f ? 1.0f : -1.0f;
  const float a = -1.0f / (sign + to_sun.z);
  const float b = to_sun.x * to_sun.y * a;
  const Vec3 t{1.0f + sign * to_sun.x * to_sun.x * a, sign * b, -sign * to_sun.x};
  const Vec3 s{b, sign + to_sun.y * to_sun.y * a, -to_sun.y};
  const float r = std::sqrt(std::clamp(u1, 0.0f, 1.0f)) * tan_radius;
  const float angle = kTwoPi * u2;
  const float dx = r * std::cos(angle);
  const float dy = r * std::sin(angle);
  Vec3 d{to_sun.x + t.x * dx + s.x * dy, to_sun.y + t.y * dx + s.y * dy, to_sun.z + t.z * dx + s.z * dy};
  const float length = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
  return {d.x / length, d.y / length, d.z / length};
}

// ---------------------------------------------------------------------------------------------------------------------
// The image's codes (R8G8B8A8_UNORM)
// ---------------------------------------------------------------------------------------------------------------------
struct Texel {
  float open = 1.0f;  // r: the occlusion's open share
  float sun = 1.0f;   // g: the sun's open share
  bool traced = false;  // b
};
inline uint8_t Encode8(float x) { return uint8_t(std::clamp(x, 0.0f, 1.0f) * 255.0f + 0.5f); }
inline float Decode8(uint8_t b) { return float(b) / 255.0f; }

// ---------------------------------------------------------------------------------------------------------------------
// The occlusion term in the main pass
// ---------------------------------------------------------------------------------------------------------------------
// The visibility the main pass writes where the frame has a traced term (hlsl/ao_main_ps.hlsl AoTracedVisibility, the same rule in the plain pass
// and in the bounce light's, on the channel the denoise and the history read): the darker of the screen-space visibility (the look's power and floor
// already applied) and the traced open share shaped the same way. It never lightens what the screen-space term found, and a pixel the rays found
// open keeps it as it was.
inline float CombineVisibility(float screen_visibility, float traced_open, float power, float floor) {
  return std::min(screen_visibility, std::max(std::pow(traced_open, power), floor));
}

// ---------------------------------------------------------------------------------------------------------------------
// The flags the consumers read (the shaders repeat them: car_rt_ao_rules_test.cpp checks the text)
// ---------------------------------------------------------------------------------------------------------------------
// The main pass's flags word (slots2.z): bit 31, the plain pass only (the bounce light's pass reads slots.x for its colour chain); slots.x
// is then the image's bindless slot. The contact march's flags: bit 31 the same, slots.x the image's slot; bit 25 the debug view of this term
// alone (the march writes the image's g channel: mcla_gpu_contact_shadows_view 6). Neither bit is read by those passes otherwise.
inline constexpr uint32_t kMainFlag = 1u << 31;
inline constexpr uint32_t kContactFlag = 1u << 31;
inline constexpr uint32_t kContactViewFlag = 1u << 25;

// The traced pass's own push flags (slots.w of hlsl/car_rt_ao_cs.hlsl CarRtAoPush): the terms it computes.
inline constexpr uint32_t kPushOcclusion = 1u << 0;
inline constexpr uint32_t kPushSun = 1u << 1;
inline constexpr uint32_t kPushRaysShift = 8;  // bits 8-11: the occlusion rays a pixel
inline constexpr uint32_t kPushBoxesShift = 12;  // bits 12-15: the other cars' boxes in the car RT table (noise.w the first row), 0 to kMaxOtherBoxes

// ---------------------------------------------------------------------------------------------------------------------
// The car's region
// ---------------------------------------------------------------------------------------------------------------------
// A box in the frame's view space (x right, y up, z forward): car_rt_instances_rules.h's, so that the other cars' boxes from the scene are the same type.
using Box = car_rt_instances::ViewBox;

// The boxes of the region: the player's car's and at most this many of the other cars' (car_rt_instances_rules.h kMaxCars).
inline constexpr uint32_t kMaxOtherBoxes = car_rt_instances::kMaxCars;
inline constexpr uint32_t kMaxBoxes = 1 + kMaxOtherBoxes;
static_assert(kMaxOtherBoxes <= 15, "the other cars' boxes are counted in four bits of the push flags");

// The radius of the stand-in box when the frame has no box of the car (car_frame.h: "a sphere of a few metres around player_world's row 3").
inline constexpr double kFallbackRadius = 3.2;
// What the world box of the car is grown by before it is made a view box: the boxes are of the meshes read so far, and a mirror or a wing
// may not be in them.
inline constexpr float kBoxMargin = 0.15f;

// The player's car's box in the view space the top level structure is in. `view` is the frame's world-to-view matrix (row vectors,
// car_frame::Frame::view); the camera's z is forward in the occlusion's view space and the matrix may carry the game's own (looking down -z):
// which one is found as car_rt_scene.cpp finds it (ViewZSign of the car's position: it is in front of the camera), and with no answer
// (the car within half a metre of the camera's plane) there is no box. `has_bounds` / `world_min` / `world_max` are the frame's world box
// of the car; without them a cube of kFallbackRadius round `position`. The result is grown by `margin` on every side.
inline bool ViewBoxFor(const temporal::Mat4& view, const double position[3], bool has_bounds, const double world_min[3],
                       const double world_max[3], float margin, Box* out) {
  const int sign = car_rt_rules::ViewZSign(view, position);
  if (sign == 0) {
    return false;
  }
  const bool flip_z = sign < 0;
  double lo[3], hi[3];
  for (int c = 0; c < 3; ++c) {
    if (has_bounds) {
      lo[c] = world_min[c];
      hi[c] = world_max[c];
    } else {
      lo[c] = position[c] - kFallbackRadius;
      hi[c] = position[c] + kFallbackRadius;
    }
  }
  double vmin[3] = {1e300, 1e300, 1e300};
  double vmax[3] = {-1e300, -1e300, -1e300};
  for (int corner = 0; corner < 8; ++corner) {
    const double p[3] = {(corner & 1) ? hi[0] : lo[0], (corner & 2) ? hi[1] : lo[1], (corner & 4) ? hi[2] : lo[2]};
    for (int c = 0; c < 3; ++c) {
      double v = p[0] * view.m[0][c] + p[1] * view.m[1][c] + p[2] * view.m[2][c] + view.m[3][c];
      if (c == 2 && flip_z) {
        v = -v;
      }
      if (!std::isfinite(v)) {
        return false;
      }
      vmin[c] = std::min(vmin[c], v);
      vmax[c] = std::max(vmax[c], v);
    }
  }
  for (int c = 0; c < 3; ++c) {
    out->min[c] = float(vmin[c]) - margin;
    out->max[c] = float(vmax[c]) + margin;
  }
  out->valid = true;
  return true;
}

// A pixel rectangle [x0, x1) x [y0, y1).
struct Rect {
  uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  bool empty() const { return x1 <= x0 || y1 <= y0; }
  uint32_t width() const { return x1 > x0 ? x1 - x0 : 0; }
  uint32_t height() const { return y1 > y0 ? y1 - y0 : 0; }
  uint64_t area() const { return uint64_t(width()) * height(); }
};

// The nearest a corner may be to the camera before the projection is given up on (the rectangle is then the whole image).
inline constexpr float kNearZ = 0.1f;

// The pixels of an image `width` x `height` that can hold a point of `box` grown by `reach` on every side, or, with `sun_reach` > 0, a
// point that is in the box's shadow: the box moved by up to `sun_reach` away from the sun (-to_sun). The hull of the projected corners of
// the two boxes. A corner at or behind kNearZ: the whole image. `tan_x`, `tan_y`: tan of the half field of view.
inline Rect ScreenRect(const Box& box, float reach, const float to_sun[3], float sun_reach, float tan_x, float tan_y, uint32_t width,
                       uint32_t height) {
  const Rect whole{0, 0, width, height};
  if (!box.valid || width == 0 || height == 0 || !(tan_x > 0.0f) || !(tan_y > 0.0f)) {
    return Rect{};
  }
  float lo_x = 1e30f, lo_y = 1e30f, hi_x = -1e30f, hi_y = -1e30f;
  const int copies = sun_reach > 0.0f ? 2 : 1;
  for (int copy = 0; copy < copies; ++copy) {
    for (int corner = 0; corner < 8; ++corner) {
      float p[3];
      for (int c = 0; c < 3; ++c) {
        p[c] = ((corner >> c) & 1) ? box.max[c] + reach : box.min[c] - reach;
        if (copy == 1) {
          p[c] -= to_sun[c] * sun_reach;
        }
      }
      if (!(p[2] > kNearZ)) {
        return whole;
      }
      const float nx = p[0] / (p[2] * tan_x);
      const float ny = p[1] / (p[2] * tan_y);
      const float px = (nx * 0.5f + 0.5f) * float(width);
      const float py = (0.5f - ny * 0.5f) * float(height);
      lo_x = std::min(lo_x, px);
      hi_x = std::max(hi_x, px);
      lo_y = std::min(lo_y, py);
      hi_y = std::max(hi_y, py);
    }
  }
  const float fw = float(width), fh = float(height);
  const float x0 = std::clamp(std::floor(lo_x) - 1.0f, 0.0f, fw);
  const float y0 = std::clamp(std::floor(lo_y) - 1.0f, 0.0f, fh);
  const float x1 = std::clamp(std::ceil(hi_x) + 1.0f, 0.0f, fw);
  const float y1 = std::clamp(std::ceil(hi_y) + 1.0f, 0.0f, fh);
  return Rect{uint32_t(x0), uint32_t(y0), uint32_t(x1), uint32_t(y1)};
}

// The slab test of a ray against a box (the shader's, for the sun ray): the ray's parameters of its entry and its exit, true when it meets the
// box in [0, t_max]. `box` grown by `grow`. A component of the direction that is 0 counts as parallel.
inline bool RayMeetsBox(const float origin[3], const float direction[3], const Box& box, float grow, float t_max, float* t_enter,
                        float* t_exit) {
  float lo = 0.0f, hi = t_max;
  for (int c = 0; c < 3; ++c) {
    const float bmin = box.min[c] - grow;
    const float bmax = box.max[c] + grow;
    if (std::fabs(direction[c]) < 1e-12f) {
      if (origin[c] < bmin || origin[c] > bmax) {
        return false;
      }
      continue;
    }
    const float inv = 1.0f / direction[c];
    float a = (bmin - origin[c]) * inv;
    float b = (bmax - origin[c]) * inv;
    if (a > b) {
      std::swap(a, b);
    }
    lo = std::max(lo, a);
    hi = std::min(hi, b);
    if (lo > hi) {
      return false;
    }
  }
  if (t_enter) *t_enter = lo;
  if (t_exit) *t_exit = hi;
  return true;
}

// Whether a pixel's view position can have an occlusion ray reach the car: it is in the box grown by the reach.
inline bool NearBox(const float p[3], const Box& box, float reach) {
  for (int c = 0; c < 3; ++c) {
    if (p[c] < box.min[c] - reach || p[c] > box.max[c] + reach) {
      return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// The region: the player's car and the cars beside it
// ---------------------------------------------------------------------------------------------------------------------
// The pass traces where the player's car or another car of the top level is: its REGION is the boxes (the player's from CarFrame's box through the
// camera, ViewBoxFor, each other car's from the scene, CarRtView::boxes), not one box round all of them: cars of a race are spread over 14 metres of
// road and a box round them would trace all the road between and, with a car behind the camera, the whole picture. A pixel is traced if it is near
// ANY box (the occlusion rays) or its sun ray meets ANY (the sun ray); the pass's rectangle is the hull of the boxes' rectangles.

// The other cars' boxes are grown by this when the shader reads them (the scene's boxes are the transformed boxes of the meshes listed; the
// player's is grown by kBoxMargin in ViewBoxFor): the same number.
inline constexpr float kOtherBoxMargin = kBoxMargin;

inline Box GrownBox(const Box& box, float margin) {
  Box out = box;
  for (int c = 0; c < 3; ++c) {
    out.min[c] -= margin;
    out.max[c] += margin;
  }
  return out;
}

// A box (grown by `reach`, and by the sun's extrusion `sun_reach` toward -to_sun, 0: none) lying wholly at or behind the camera's plane has no pixel
// of the picture: a car behind a chase camera. (ScreenRect would make the whole image of a box that reaches the plane.)
inline bool BoxBehindCamera(const Box& box, float reach, float sun_reach) {
  return box.max[2] + reach + std::max(sun_reach, 0.0f) <= kNearZ;
}

// The pixels that can hold a point near any of the boxes (grown by `reach`) or in the shadow the sun's rays carry from them (`sun_reach`): the hull
// of the boxes' rectangles (ScreenRect), a box that is not valid or is wholly behind the camera left out. Empty when no box has pixels.
inline Rect RegionRect(const Box* boxes, uint32_t count, float reach, const float to_sun[3], float sun_reach, float tan_x, float tan_y,
                       uint32_t width, uint32_t height) {
  Rect hull;
  bool any = false;
  for (uint32_t i = 0; i < count; ++i) {
    if (!boxes[i].valid || BoxBehindCamera(boxes[i], reach, sun_reach)) {
      continue;
    }
    const Rect r = ScreenRect(boxes[i], reach, to_sun, sun_reach, tan_x, tan_y, width, height);
    if (r.empty()) {
      continue;
    }
    if (!any) {
      hull = r;
      any = true;
    } else {
      hull.x0 = std::min(hull.x0, r.x0);
      hull.y0 = std::min(hull.y0, r.y0);
      hull.x1 = std::max(hull.x1, r.x1);
      hull.y1 = std::max(hull.y1, r.y1);
    }
  }
  return hull;
}

// The shader's region tests (hlsl/car_rt_ao_cs.hlsl TracePixel): the pixel is near any box, and the sun ray meets any box (`t_exit` the farthest
// exit over the boxes it meets: the ray's length).
inline bool NearAnyBox(const float p[3], const Box* boxes, uint32_t count, float reach) {
  for (uint32_t i = 0; i < count; ++i) {
    if (boxes[i].valid && NearBox(p, boxes[i], reach)) {
      return true;
    }
  }
  return false;
}
inline bool SunMeetsAnyBox(const float origin[3], const float to_sun[3], const Box* boxes, uint32_t count, float grow, float t_max, float* t_exit) {
  bool met = false;
  float farthest = 0.0f;
  for (uint32_t i = 0; i < count; ++i) {
    float t_leave = 0.0f;
    if (boxes[i].valid && RayMeetsBox(origin, to_sun, boxes[i], grow, t_max, nullptr, &t_leave)) {
      met = true;
      farthest = std::max(farthest, t_leave);
    }
  }
  if (t_exit) {
    *t_exit = farthest;
  }
  return met;
}

// ---------------------------------------------------------------------------------------------------------------------
// The cost, for the log line
// ---------------------------------------------------------------------------------------------------------------------
// An estimate of the rays a frame casts (not a measurement): the pixels of the rectangle times the share of them near the car (a chase
// camera sees the car and the road round it in a fifth to a half of the rectangle), the occlusion rays and the sun ray, and the primary
// ray of the car's own pixels (about a third of what is near).
inline double EstimatedRays(uint64_t rect_pixels, uint32_t rays, bool sun, double near_share = 0.35) {
  const double traced = double(rect_pixels) * near_share;
  return traced * (double(rays) + (sun ? 1.0 : 0.0) + 0.33);
}

}  // namespace mcla::gpu::car_rt_ao
