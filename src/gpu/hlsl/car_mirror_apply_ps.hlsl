// MCLA GPU renderer -- the player's side mirrors (car_mirror_rt.h, car_mirror_rt_rules.h): the apply pass.
//
// One draw per mirror: a full-screen triangle into the main view's colour target (RGBA16F with the target's sample count; the pass writes one value for every sample of a pixel), scissored to the
// mirror's rectangle, blended SRC_ALPHA / INV_SRC_ALPHA on the colour channels (alpha untouched: the pipeline's blend and write mask). The value is the texel of the trace's image that stands for
// the pixel (hlsl/car_mirror_rt_cs.hlsl: rgb what the mirror shows in the colour target's units, alpha the coverage of the glass times the strength). Where the trace ran pixel for pixel (divisor 1: the
// usual mirror) the image is the rectangle's pixels one for one and the pass reads one texel with Load, filtering nothing. Where the rectangle was bigger than the image's slot or the cap and was
// traced at 1 / divisor of its size each way (car_mirror_rt_rules.h FitDivisor: the driver's mirror of a cockpit at 4K), the pass reads the four texels round the pixel and blends them by their
// distance AND their alpha (a texel off the glass has no colour and must not darken the glass's edge); the glass's soft edge stays as soft as a texel. A pixel off the rectangle changes nothing.

#include "host_pass_common.hlsli"

struct MirrorApplyPush {
  uint4 image;  // x the image's bindless slot (a Texture2D<float4> in space 0), y, z the rectangle's first texel in the image, w the divisor (picture pixels to a texel each way; 0 or 1: one for one)
  uint4 rect;   // x0, y0, x1, y1: the rectangle on the picture, [x0, x1) x [y0, y1)
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<MirrorApplyPush> g_push;
#else
ConstantBuffer<MirrorApplyPush> g_push : register(b0, space4);
#endif

float4 main(float4 position : SV_Position) : SV_Target {
  const int2 p = int2(position.xy);
  const int2 lo = int2(g_push.rect.xy);
  const int2 hi = int2(g_push.rect.zw);
  if (any(p < lo) || any(p >= hi)) {
    return float4(0.0, 0.0, 0.0, 0.0);
  }
  const int divisor = int(max(g_push.image.w, 1u));
  if (divisor == 1) {
    return g_textures[g_push.image.x].Load(int3(p - lo + int2(g_push.image.yz), 0));
  }
  // the texels of the rectangle's slot of the image: ceil(size / divisor) each way; the pixel's place among them, texel centres at half integers
  const int2 texels = (hi - lo + (divisor - 1)) / divisor;
  const float2 at = (float2(p - lo) + 0.5) / float(divisor) - 0.5;
  const int2 t0 = int2(floor(at));
  const float2 f = at - float2(t0);
  float4 sum = float4(0.0, 0.0, 0.0, 0.0);  // rgb weighted by alpha, a the weighted alpha
  [unroll]
  for (int k = 0; k < 4; ++k) {
    const int2 step = int2(k & 1, k >> 1);
    const int2 t = clamp(t0 + step, int2(0, 0), texels - 1);
    const float4 texel = g_textures[g_push.image.x].Load(int3(t + int2(g_push.image.yz), 0));
    const float2 w2 = lerp(1.0 - f, f, float2(step));
    const float w = w2.x * w2.y * texel.a;
    sum.rgb += texel.rgb * w;
    sum.a += w;
  }
  return float4(sum.rgb / max(sum.a, 1e-6), sum.a);
}
