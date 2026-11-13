// MCLA GPU renderer -- sun shafts, pass 2: the radial blur toward the sun (shafts.h).
//
// The scattering of the sky's light seen through the gaps of whatever stands in front of it, the
// screen-space way (Mitchell, "Light Shaft Rendering", GPU Gems 3, chapter 13): each texel marches
// from its own position toward the sun's position in the image and adds up the brightness of the sky
// image on the way, each sample weighing less than the one before (the decay). A texel whose way to
// the sun runs through sky is bright, one whose way is blocked by a roof, a tree or a wall is dark:
// the rays that stream from the sun through the gaps are the bright texels behind them.
//
//   density  the share of the distance to the sun one ray covers (0.9: nearly all of it); a sun that
//            is off screen shortens the ray to where it leaves the image, so no step is spent
//            outside it
//   decay    the weight of a step relative to the one before; the host raises it to the power
//            64 / steps, so the light a ray gathers does not depend on how many steps it has
//   radial   a sample far from the sun counts for less (1 / (1 + (d / R)^2), d in image heights),
//            so a bright sky that fills the picture lights the sun's surroundings, not the whole
//            screen
//
// The start of the ray is shifted by interleaved gradient noise (Jimenez 2014) so that the steps do
// not show as bands; the noise is a fixed pattern, plus a shift by the golden ratio each frame while
// a temporal upscaler runs (the host sends 0 otherwise), which the upscaler averages.

#include "shafts_common.hlsli"
#include "host_pass_samplers.hlsli"

// R, the distance in image heights at which a sample counts for half.
static const float kRadialRange = 1.5f;

float IgNoise(float2 p) {
  return frac(52.9829189f * frac(dot(p, float2(0.06711056f, 0.00583715f))));
}

float4 main(float4 position : SV_Position) : SV_Target {
  const float2 size = g_push.sizes.xy;
  const float2 uv = position.xy / size;
  const float2 sun = g_push.sun.xy;
  const uint steps = max(g_push.slots.w, 1u);
  const float2 to_sun = sun - uv;

  // How far along the way to the sun the ray goes: the density, or less when the image ends first.
  // (No vector ternaries: HLSL 2021 rejects them. The edge each axis runs into, and the distance to
  // it in units of the whole way; an axis that does not move reaches it never.)
  const float2 edge = step(0.0f, to_sun);
  const float2 safe_way = (edge * 2.0f - 1.0f) * max(abs(to_sun), 1e-6f);
  const float2 reach = (edge - uv) / safe_way;
  const float length_of_way = min(g_push.sun.z, min(reach.x, reach.y));
  const float2 step_uv = to_sun * (length_of_way / float(steps));

  const float noise = frac(IgNoise(position.xy) + g_push.tint.w * 0.6180339887f);
  const float aspect = size.x / size.y;
  const float inv_range2 = 1.0f / (kRadialRange * kRadialRange);

  float2 at = uv + step_uv * noise;
  float weight = 1.0f;
  float sum = 0.0f;
  [loop] for (uint i = 0u; i < steps; ++i) {
    at += step_uv;
    const float shine =
        g_textures[g_push.slots.y].SampleLevel(g_samplers[kSamplerLinearClamp], at, 0).x;
    const float2 q = (at - sun) * float2(aspect, 1.0f);
    sum += shine * weight / (1.0f + dot(q, q) * inv_range2);
    weight *= g_push.sun.w;
  }
  return float4(sum / float(steps), 0.0f, 0.0f, 0.0f);
}
