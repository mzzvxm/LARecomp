// MCLA GPU renderer -- the player's side mirrors (car_mirror_rt.h, car_mirror_rt_rules.h): the apply pass.
//
// One draw per mirror: a full-screen triangle into the main view's colour target (RGBA16F with the target's sample count; the pass writes one value for every sample of a pixel), scissored to the
// mirror's rectangle, blended SRC_ALPHA / INV_SRC_ALPHA on the colour channels (alpha untouched: the pipeline's blend and write mask). The value is the texel of the trace's image that stands for
// the pixel (hlsl/car_mirror_rt_cs.hlsl: rgb what the mirror shows in the colour target's units, alpha the coverage of the glass times the strength): the image is the rectangle's pixels one for one,
// so the pass reads one texel with Load and filters nothing; a pixel off the rectangle changes nothing.

#include "host_pass_common.hlsli"

struct MirrorApplyPush {
  uint4 image;  // x the image's bindless slot (a Texture2D<float4> in space 0), y, z the rectangle's first texel in the image
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
  return g_textures[g_push.image.x].Load(int3(p - lo + int2(g_push.image.yz), 0));
}
