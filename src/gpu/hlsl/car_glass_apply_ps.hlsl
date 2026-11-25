// MCLA GPU renderer -- the player's car windows reflecting the city (car_rt_glass_refl.h, car_glass_rules.h): the additive pass.
//
// A full-screen triangle into the main view's colour target (RGBA16F with the target's sample count; the pass writes one value for every sample of a pixel), the blend ADDING it to what the game and
// the effects before drew, alpha untouched (the pipeline's blend: ONE, ONE, the colour channels only). The term is the glass image the trace pass made this frame (hlsl/car_rt_refl_glass.hlsli: rgb the
// colour to add, alpha 1 where a pane is) read at the pixel's place with the four nearest texels:
//   - the image is the history's size (the picture over the divisor, 1 or 2): texel i stands for the render pixel floor((i + 0.5) divisor), as VisibleAt in car_rt_refl_cs.hlsl;
//   - the taps are kept inside the rectangle the trace wrote (the texels outside it hold the frames before's), and a pixel outside the rectangle adds nothing;
//   - the four taps are weighted by their bilinear shares times their alpha: the colour is the mean over the panes' texels (no black from the texels beside the glass), and the term fades in over
//     the first half of the coverage, so that a pixel whose neighbourhood is less than half pane adds nothing (the window's edge: no halo on the pillar).
// The colour is in the target's own units, as the history, so no exposure is applied.

#include "host_pass_common.hlsli"
#include "nan_guard.hlsli"

struct GlassApplyPush {
  uint4 image;  // x the glass image's bindless slot (a Texture2D<float4> in space 0), y, z its size in texels, w the divisor (1 or 2)
  uint4 rect;   // x0, y0, x1, y1: the texels the trace wrote this frame, [x0, x1) x [y0, y1)
  float4 sizes;  // x, y the picture's size in pixels
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<GlassApplyPush> g_push;
#else
ConstantBuffer<GlassApplyPush> g_push : register(b0, space4);
#endif

static const float kGlassApplyCoverageFrom = 0.5;  // the share of the four taps that are pane under which the pixel adds nothing, and from which the term fades in (car_glass_rules.h kApplyCoverageFrom)

float4 main(float4 position : SV_Position) : SV_Target {
  const int2 p = int2(position.xy);
  if (p.x >= int(g_push.sizes.x) || p.y >= int(g_push.sizes.y)) {
    return float4(0.0, 0.0, 0.0, 0.0);
  }
  const float divisor = float(max(g_push.image.w, 1u));
  const int2 lo = int2(g_push.rect.xy);
  const int2 hi = int2(g_push.rect.zw) - 1;
  // the texel this pixel is in: nothing outside the rectangle of the trace
  const int2 own = int2(floor((float2(p) + 0.5) / divisor));
  if (any(own < lo) || any(own > hi)) {
    return float4(0.0, 0.0, 0.0, 0.0);
  }
  // the pixel's place in texels of the image (texel i is centred at i * divisor + floor(divisor / 2) + 0.5)
  const float2 f = (float2(p) - floor(divisor * 0.5)) / divisor;
  const int2 base = int2(floor(f));
  const float2 fr = f - float2(base);
  float4 sum = float4(0.0, 0.0, 0.0, 0.0);
  [unroll]
  for (int k = 0; k < 4; ++k) {
    const int2 offset = int2(k & 1, k >> 1);
    const int2 texel = clamp(base + offset, lo, hi);
    const float w = (offset.x != 0 ? fr.x : 1.0 - fr.x) * (offset.y != 0 ? fr.y : 1.0 - fr.y);
    sum += w * NgFinite4(g_textures[g_push.image.x].Load(int3(texel, 0)));   // (nan_guard.hlsli: a NaN or an infinity texel is none; w * NaN would spoil the whole sum)
  }
  const float coverage = sum.a;
  if (coverage < kGlassApplyCoverageFrom) {
    return float4(0.0, 0.0, 0.0, 0.0);
  }
  const float3 colour = sum.rgb / coverage;
  return float4(colour * saturate((coverage - kGlassApplyCoverageFrom) / (1.0 - kGlassApplyCoverageFrom)), 0.0);
}
