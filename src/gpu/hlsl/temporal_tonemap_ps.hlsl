// MCLA GPU renderer -- the reversible tone map around the temporal upscalers (temporal_tonemap.h).
//
// The scene's colour is linear and scene referred (the 7e3 targets reach about 32). A sub-pixel
// detail 20 to 30 times brighter than its surroundings (a sun glint on a roof, a lamp, a sign) lands
// on a pixel in some jitter phases and not in others, and an upscaler that averages the phases in
// that range lets the average blink. Compressed first, the glint is at most one step brighter than
// what is around it, and the average no longer swings.
//
//   compress (TONEMAP_EXPAND 0)   c' = c / (1 + max(c.r, c.g, c.b))
//   expand   (TONEMAP_EXPAND 1)   c  = c' / max(1 - max(c'.r, c'.g, c'.b), kMinHeadroom)
//
// The weight is the largest channel, not the luminance: every channel of a pixel is scaled by the
// same factor, so the hue is kept and the expansion undoes the compression exactly; the result is in
// [0, 1) whatever the colour, which is what an upscaler told its input is not HDR expects (a
// luminance weight lets a saturated blue or red channel stay far above 1). kMinHeadroom keeps the
// expansion finite when an upscaler overshoots to 1 or past it (ringing): the largest value it makes
// is 63, about twice what the game's targets can hold.
//
// One texel for texel read of the source slot into an image of the same size; the alpha is kept.

#include "host_pass_common.hlsli"

struct TonemapPush {
  uint source;  // the bindless slot of the image read (a gamma bit 31 is dropped)
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<TonemapPush> g_push;
#else
ConstantBuffer<TonemapPush> g_push : register(b0, space4);
#endif

// 1 over the largest factor the expansion applies.
static const float kMinHeadroom = 1.0f / 64.0f;
// The largest half float: Inf in the source (a broken specular) must not become Inf / Inf.
static const float kLargest = 65504.0f;

// A channel that is not a number or below zero is zero (every comparison with NaN is false).
float Sane(float v) {
  return v >= 0.0f ? min(v, kLargest) : 0.0f;
}

float4 main(float4 position : SV_Position) : SV_Target {
  const int3 at = int3(int2(position.xy), 0);
  const float4 texel = g_textures[g_push.source & 0x7FFFFFFFu].Load(at);
  const float3 color = float3(Sane(texel.r), Sane(texel.g), Sane(texel.b));
  const float peak = max(color.r, max(color.g, color.b));
#if TONEMAP_EXPAND
  return float4(color / max(1.0f - peak, kMinHeadroom), texel.a);
#else
  return float4(color / (1.0f + peak), texel.a);
#endif
}
