// MCLA GPU renderer -- sun shafts, pass 1: the sky's brightness (shafts.h).
//
// Run at the game's mid-scene depth copy, when the scene's opaque draws are all in the colour
// target and the fog, the particles and the post chain are not. One texel of the image (R16_FLOAT)
// averages four pixels spread over its footprint (shafts_image.h), and for each the mean over the
// host samples of the target (a
// multisampled scene: Texture2DMS, every sample its own depth and colour, so an edge between the sky
// and a roof is as smooth as the picture's):
//
//   sky ? clamp(luma(colour) - threshold, 0, limit) : 0
//
// A sample is sky where its stored depth is at or under the limit the push gives (the sky is drawn at
// the far end: 0). Only a sky shines: everything else is what the rays of the next pass are blocked
// by. The colour is clamped to what a half float keeps and a NaN counts as black (max(NaN, 0) is 0).

#include "shafts_common.hlsli"

#ifndef SHAFTS_MS
#define SHAFTS_MS 0
#endif

static const float3 kLuma = float3(0.2126f, 0.7152f, 0.0722f);

// The brightness of the sample `s` of the pixel `p` (the plain texture has the one).
float SampleShine(int2 p, uint s) {
#if SHAFTS_MS
  const float depth = g_textures_ms[g_push.slots.x].Load(p, s).r;
#else
  const float depth = g_textures[g_push.slots.x].Load(int3(p, 0)).r;
#endif
  if (depth > g_push.sun.x) {
    return 0.0f;
  }
#if SHAFTS_MS
  float3 colour = g_textures_ms[g_push.slots.y].Load(p, s).rgb;
#else
  float3 colour = g_textures[g_push.slots.y].Load(int3(p, 0)).rgb;
#endif
  colour = min(max(colour, 0.0f), 64.0f);
  return clamp(dot(colour, kLuma) - g_push.tint.w, 0.0f, g_push.sun.y);
}

float4 main(float4 position : SV_Position) : SV_Target {
  // The texel's tap k of an axis is at the quarter (k = 0) and three quarters (k = 1) of its footprint
  // (shafts_image.h SkyTap): with footprint 2 the pixels 2 * texel and 2 * texel + 1, the old 2x2 block.
  const float2 texel = floor(position.xy);
  const float2 footprint = g_push.tint.xy;
  const int2 last = int2(g_push.sizes.zw) - 1;
#if SHAFTS_MS
  const uint samples = max(g_push.slots.z, 1u);
#else
  const uint samples = 1u;
#endif
  float sum = 0.0f;
  [unroll] for (int j = 0; j < 2; ++j) {
    [unroll] for (int i = 0; i < 2; ++i) {
      const int2 p = min(int2(floor((texel + float2(0.25f + 0.5f * float(i), 0.25f + 0.5f * float(j))) * footprint)), last);
      [loop] for (uint s = 0u; s < samples; ++s) {
        sum += SampleShine(p, s);
      }
    }
  }
  return float4(sum / float(4u * samples), 0.0f, 0.0f, 0.0f);
}
