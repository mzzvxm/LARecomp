// MCLA GPU renderer -- contact shadows, the sun's visibility over a short distance (ao.h).
//
// The game has no shadow for the small things in the world (a kerb, a wall's foot, a car's sills, a
// lamp post on the pavement): its sun shadows are four cascades whose texels are 1 to 25 cm and
// 4-tap filtered, and at 6 h, 18 h and in rain none at all. This pass finds the short shadows from
// the picture's own depth: for each pixel that faces the sun, a ray toward the sun is marched in
// view space over a short length (the push block says how many world units and how many steps) and
// every step is looked up in the linear depth image the occlusion's depth pass made (the chain's
// level 0, R32_FLOAT, at the AO's size, which is the render size: a render scale of 3 traces
// 3840x2160 pixels). A step is hidden when the surface at its pixel is nearer to the camera than the
// step, and not by more than the thickness (a wall far in front of the ray hides nothing: it is
// another object that only overlaps on screen). The result is 1 (open) to 0 (all hidden), R16_FLOAT,
// like the occlusion's visibility.
//
// A shadow of the sun is sharp: the sun covers half a degree, so an edge 0.4 m from its caster is
// 4 mm wide. The first version was soft for four reasons, all changed here; every number below is in
// contact_trace.h with a standalone check (contact_trace_test.cpp), which also holds a C++ twin of
// this march that the check runs over synthetic scenes:
//   1. The ray's end fade was 1 - along^2, so a shadow was darkest at its caster and lost a quarter
//      of its darkness half way along the ray: a gradient, not a shadow. Now the shadow is full
//      over the first `fade` share of the ray (push shape.x, 0.6) and only the last part fades, so
//      that a shadow cut off by the ray's end does not end in a hard line.
//   2. The depth tolerance (a bias of 2 cm + 0.4% of the distance, and a ramp as wide as the bias)
//      was set for 1280x720 and was the same at every render scale: at 4K an edge was 5 to 30
//      pixels wide. The tolerance is now a few footprints (the world size of one pixel of the depth
//      image at that distance: 2 z tan(fov/2) / height) plus a few millimetres, and so is the lift
//      of the ray's start off its surface (4 footprints along the normal). That lift is what keeps a
//      lit surface from shadowing itself: along the view ray it is 4 footprints times root(1 + s^2)
//      for a surface of slope s (depth change per pixel step over the footprint), and the error of
//      reading the depth at the centre of the pixel the step falls in is at most half a pixel times
//      s footprints, so it is always covered (contact_trace_test.cpp runs planes at every angle).
//   3. The denoise was two plane-only bilateral passes (ao_denoise_ps), which blur a shadow edge
//      across one surface. The contact term has its own single pass now (ao_denoise_ps, flag bit
//      25: AoContactFilter) whose weights also fall with the difference of the visibility, so noise
//      goes and an edge stays.
//   4. The steps were equally spaced and the start offset noise moved the whole ray a step: the
//      near part of the ray, where the contact is, is sampled finer now (the distance grows as the
//      square of the step number) and the step's ramp covers the depth change of a step, so the far
//      steps do not speckle.
//
// What it is not: a shadow of anything that is not on screen, and nothing thinner than the step
// (the start of the ray is moved per pixel by interleaved gradient noise plus the frame's R2 shift;
// there is no history for it, so the shift is 0 (a fixed pattern) unless a temporal upscaler
// averages the frames).
//
// The apply pass (ao_apply_ps) puts it on the picture: only on the sun's share of the light, so it
// cannot darken what the game already shadows (the game's mask says that) or the ambient.
//
// Push block (ao_common.hlsli AoPush), this pass's fields:
//   slots.z    the linear depth image (bindless slot)
//   slots2.y   the frame number (the noise's shift); 0 while no temporal upscaler runs
//   sizes      the image's size twice (this pass's target and the depth image are the same size)
//   lens.xy    tan of the half field of view; lens.z the ray's length in world units, lens.w the
//              thickness (the farthest a surface may be in front of a step and still hide it)
//   shape.x    the share of the ray over which the shadow stays full before it fades out (0.6)
//   shape.y    the number of steps
//   light.xyz  the direction to the sun in view space (x right, y up, z forward; unit length)
//   extra.y    the distance beyond which nothing is computed (the caller's contact fade end)
//
// The constants below are contact_trace.h's: change them there first.

#include "ao_common.hlsli"

// The depth tolerance, as a floor in metres plus footprints (the world size of one pixel of the
// depth image where the step is): a step counts as hidden once the surface at its pixel is nearer
// than the step by more than kEpsFloor + kEpsPixels footprints, fully so after kRampFloor +
// kRampPixels footprints more.
static const float kEpsFloor = 0.004f;
static const float kEpsPixels = 1.5f;
static const float kRampFloor = 0.004f;
static const float kRampPixels = 2.0f;
// Plus this share of the depth a step changes by along the sun's direction, so that the steps of a
// ray that is not parallel to the picture's plane run together instead of speckling.
static const float kRampStepShare = 0.5f;
// The lift of the ray's start off the surface it starts on, along the normal.
static const float kLiftFloor = 0.006f;
static const float kLiftPixels = 4.0f;
// A surface this nearly edge-on to the sun gets no contact shadow (its sun share is about 0 too).
static const float kSunFacingLow = 0.03f;
static const float kSunFacingHigh = 0.15f;
// The fade's start is kept below 1 (a fade of no width).
static const float kFadeStartMax = 0.98f;
// A step nearer to the camera than this is not looked up (behind the lens).
static const float kNearZ = 0.05f;

float4 main(float4 position : SV_Position) : SV_Target {
  const int2 pixel = int2(position.xy);
  const float2 size = g_push.sizes.zw;
  const int2 isize = int2(size);
  const uint depth_slot = g_push.slots.z;
  const float tc = AoLoadLinear(depth_slot, pixel);
  if (tc >= kAoSky * 0.5f || tc >= g_push.extra.y) {
    return float4(1.0f, 0.0f, 0.0f, 0.0f);
  }
  const float3 pc = AoViewPos(float2(pixel) + 0.5f, size, tc);
  float3 normal = AoNormalFromImage(depth_slot, pixel, isize, tc, pc);
  if (dot(normal, pc) > 0.0f) {
    normal = -normal;  // facing the camera
  }
  const float3 to_sun = g_push.light.xyz;
  // A surface that faces away from the sun gets none of its light: there is nothing to shadow, and
  // the apply pass's sun share is 0 there as well.
  const float facing = dot(normal, to_sun);
  if (facing <= 0.0f || dot(to_sun, to_sun) < 0.5f) {
    return float4(1.0f, 0.0f, 0.0f, 0.0f);
  }

  const float length_units = g_push.lens.z;
  const float thickness = max(g_push.lens.w, 1e-3f);
  const float fade_start = min(saturate(g_push.shape.x), kFadeStartMax);
  const uint steps = uint(max(g_push.shape.y, 1.0f));
  const float inv_steps = 1.0f / float(steps);
  // The world size of one pixel of the depth image at distance 1 (two tan of the half field of view
  // over the height): times the distance, a footprint.
  const float footprint_unit = 2.0f * g_push.lens.y / size.y;
  // Interleaved gradient noise plus the frame's R2 shift (ao_main_ps): the start of the ray inside
  // its first step.
  const float frame = float(g_push.slots2.y & 1023u);
  const float noise = frac(AoNoise(float2(pixel)) + frame * 0.75487766624669f);

  const float3 start = pc + normal * (kLiftFloor + kLiftPixels * footprint_unit * tc);
  float shadow = 0.0f;
  for (uint i = 0u; i < steps; ++i) {
    // The distance along the ray grows as the square of the step number: fine near the contact.
    const float t = (float(i) + noise) * inv_steps;
    const float along = t * t;
    const float3 p = start + to_sun * (along * length_units);
    if (p.z <= kNearZ) {
      break;
    }
    const float2 uv = float2(0.5f + 0.5f * p.x / (p.z * g_push.lens.x),
                             0.5f - 0.5f * p.y / (p.z * g_push.lens.y));
    if (any(uv < 0.0f) || any(uv >= 1.0f)) {
      break;
    }
    const float td = AoLoadLinear(depth_slot, min(int2(uv * size), isize - 1));
    // > 0: the surface at this pixel is in front of the step (it covers it from the camera).
    const float dz = p.z - td;
    const float footprint = footprint_unit * p.z;
    const float step_length = 2.0f * t * length_units * inv_steps;
    const float eps = kEpsFloor + kEpsPixels * footprint;
    const float ramp = kRampFloor + kRampPixels * footprint +
                       kRampStepShare * step_length * abs(to_sun.z);
    float hit = saturate((dz - eps) / ramp);
    hit *= 1.0f - saturate((dz - thickness) / thickness);
    hit *= 1.0f - smoothstep(fade_start, 1.0f, along);
    shadow = max(shadow, hit);
    if (shadow >= 0.99f) {
      break;
    }
  }
  shadow *= smoothstep(kSunFacingLow, kSunFacingHigh, facing);
  return float4(1.0f - shadow, 0.0f, 0.0f, 0.0f);
}
