// MCLA GPU renderer -- contact shadows, the sun's visibility over a short distance (ao.h).
//
// The game has no shadow for the small things in the world (a kerb, a wall's foot, a car's sills, a
// lamp post on the pavement): its sun shadows are four cascades whose texels are 1 to 25 cm and
// 4-tap filtered, and at 6 h, 18 h and in rain none at all. This pass finds the short shadows from
// the picture's own depth: for each pixel that faces the sun, a ray toward the sun is marched in
// view space over a short length (the push block says how many world units and how many steps) and
// every step is looked up in the linear depth image the occlusion's depth pass made (the chain's
// level 0, R32_FLOAT, at the AO's size). A step is hidden when the surface at its pixel is nearer
// to the camera than the step by more than a small bias (a hit), and not by more than the
// thickness (a wall far in front of the ray hides nothing: it is another object that only
// overlaps on screen). The hit is soft at both limits, and fades along the ray (a shadow's end),
// so the result is 1 (open) to 0 (all hidden), R16_FLOAT, like the occlusion's visibility.
//
// What it is not: a shadow of anything that is not on screen, and nothing thinner than the step
// (the start of the ray is moved per pixel by interleaved gradient noise plus the frame's R2 shift,
// so the step quantisation is noise that the two bilateral passes (ao_denoise_ps, the occlusion's
// own) after this one take out; there is no history for it, so the shift is 0 (a fixed pattern)
// unless a temporal upscaler averages the frames).
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
//   shape.y    the number of steps
//   light.xyz  the direction to the sun in view space (x right, y up, z forward; unit length)
//   extra.y    the distance beyond which nothing is computed (the caller's contact fade end)

#include "ao_common.hlsli"

// The step's own bias along the view ray: a share of its depth plus a floor (the depth image's
// resolution and the slope of a surface seen at an angle).
static const float kBiasFloor = 0.02f;
static const float kBiasShare = 0.004f;
// The lift of the ray's start off the surface it starts on, along the normal.
static const float kNormalLiftFloor = 0.015f;
static const float kNormalLiftShare = 0.003f;
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
  const float3 normal = AoNormalFromImage(depth_slot, pixel, isize, tc, pc);
  const float3 to_sun = g_push.light.xyz;
  // A surface that faces away from the sun gets none of its light: there is nothing to shadow, and
  // the apply pass's sun share is 0 there as well.
  if (dot(normal, to_sun) <= 0.0f || dot(to_sun, to_sun) < 0.5f) {
    return float4(1.0f, 0.0f, 0.0f, 0.0f);
  }

  const float length_units = g_push.lens.z;
  const float thickness = max(g_push.lens.w, 1e-3f);
  const uint steps = uint(max(g_push.shape.y, 1.0f));
  // Interleaved gradient noise plus the frame's R2 shift (ao_main_ps): the start of the ray inside
  // its first step.
  const float frame = float(g_push.slots2.y & 1023u);
  const float noise = frac(AoNoise(float2(pixel)) + frame * 0.75487766624669f);

  const float3 start = pc + normal * (kNormalLiftFloor + kNormalLiftShare * tc);
  float shadow = 0.0f;
  for (uint i = 0u; i < steps; ++i) {
    const float along = (float(i) + noise) / float(steps);  // 0 to 1 of the ray
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
    const float bias = kBiasFloor + kBiasShare * p.z;
    float hit = saturate((dz - bias) / bias);
    hit *= 1.0f - saturate((dz - thickness) / thickness);
    shadow = max(shadow, hit * (1.0f - along * along));
    if (shadow >= 0.99f) {
      break;
    }
  }
  return float4(1.0f - shadow, 0.0f, 0.0f, 0.0f);
}
