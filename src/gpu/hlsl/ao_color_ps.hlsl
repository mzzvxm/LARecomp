// MCLA GPU renderer -- screen-space global illumination, the colour chain and its debug views (ao.h).
//
// The visibility bitmask pass (ao_main_ps.hlsl, AO_GI=1) takes the radiance of what blocks a pixel's
// hemisphere from the scene colour as it is at the game's mid-scene depth copy: every opaque draw of
// the main view is in, the fog, the particles and the post chain are not. This file makes that colour
// readable the way the depth chain is, in four compilations (AO_COLOR_PASS, AO_COLOR_MS):
//
//   0  ao_color_ps / ao_color_ms_ps   level 0: the main view's colour target (Texture2D, or
//                                     Texture2DMS when the game's MSAA is on) to an RGBA16F image at
//                                     the AO's size. One texel is the mean of the pixels it covers
//                                     (2x2 when the passes run at half the picture's size) and of
//                                     every host sample of each, after each sample was cleaned and
//                                     capped (below). The sky is in it as any other colour: the main
//                                     pass never reads a sample whose depth says sky.
//   1  ao_color_mip_ps                a coarser level: the mean of the 2x2 texels below it. Run twice,
//                                     level 0 to 1 and 1 to 2, as the depth chain's levels.
//   2  ao_color_view_ps               a debug picture (ao.h mcla_gpu_ssgi_test_view): an RGBA16F image
//                                     of the chain or the main pass's bounce, put into the colour
//                                     target in place of the picture (the pipeline's blend replaces the
//                                     rgb). Never made for a player.
//
// Cleaning and the cap (pass 0): a sample is a radiance only if it is a finite non-negative colour, and
// no sample is brighter than the cap the push block gives (a multiple of the frame's ambient + sun,
// ao.cpp: a street lamp, neon, the sun's disc and a specular glint are sources of the game's own light,
// not surfaces that bounce it, and one of them would light its whole neighbourhood white). The cap is
// on luma and keeps the hue: the colour is scaled down, never clipped per channel.
//
// Push block (ao_common.hlsli AoPush), the fields of these passes:
//   pass 0   slots.x the colour plane's bindless slot, slots.y its host samples, sizes.xy this image's
//            size, sizes.zw the colour plane's size (the picture), shape.x the cap (luma), flags: low
//   pass 1   slots.z the level below's slot, sizes.xy this level's size, sizes.zw the level below's
//   pass 2   slots.z the image shown, sizes.xy the colour target's size, flags: bits 0-7 the view (1 the
//            image's rgb as it is, 2 the image's yzw (the bounce), 3 the image's x as a grey, 4 the
//            bounce cleaned by a 5x5 mean), shape.x the display gain (the other views write the value
//            times 0.3 so that 1.0 reads below the tone map's white; the gain multiplies that)

#include "ao_common.hlsli"

#ifndef AO_COLOR_PASS
#define AO_COLOR_PASS 0
#endif
#ifndef AO_COLOR_MS
#define AO_COLOR_MS 0
#endif

#if AO_COLOR_PASS == 2
#include "host_pass_samplers.hlsli"
#endif

static const float3 kLuma = float3(0.2126f, 0.7152f, 0.0722f);
// What a half float can carry with room.
static const float kColourMax = 64.0f;
static const float kViewScale = 0.3f;

#if AO_COLOR_PASS == 0

// The sample `s` of the pixel `p`, finite, non-negative, and not brighter than the cap.
float3 TakeColour(int2 p, uint s) {
#if AO_COLOR_MS
  float3 colour = g_textures_ms[g_push.slots.x].Load(p, s).rgb;
#else
  float3 colour = g_textures[g_push.slots.x].Load(int3(p, 0)).rgb;
#endif
  colour = min(max(colour, 0.0f), kColourMax);  // max(NaN, 0) is 0
  const float luma = dot(colour, kLuma);
  const float cap = max(g_push.shape.x, 1e-3f);
  return luma > cap ? colour * (cap / luma) : colour;
}

float4 main(float4 position : SV_Position) : SV_Target {
  const bool low = AoLow();
  const int2 base = int2(position.xy) * (low ? 2 : 1);
  const int2 last = int2(g_push.sizes.zw) - 1;
  const int across = low ? 2 : 1;
#if AO_COLOR_MS
  const uint samples = max(g_push.slots.y, 1u);
#else
  const uint samples = 1u;
#endif
  float3 sum = float3(0.0f, 0.0f, 0.0f);
  for (int j = 0; j < across; ++j) {
    for (int i = 0; i < across; ++i) {
      const int2 p = min(base + int2(i, j), last);
      for (uint s = 0u; s < samples; ++s) {
        sum += TakeColour(p, s);
      }
    }
  }
  return float4(sum / float(uint(across * across) * samples), 1.0f);
}

#elif AO_COLOR_PASS == 1

float4 main(float4 position : SV_Position) : SV_Target {
  const int2 pixel = int2(position.xy);
  const int2 last = int2(g_push.sizes.zw) - 1;
  const uint slot = g_push.slots.z;
  float3 sum = g_textures[slot].Load(int3(min(pixel * 2 + int2(0, 0), last), 0)).rgb;
  sum += g_textures[slot].Load(int3(min(pixel * 2 + int2(1, 0), last), 0)).rgb;
  sum += g_textures[slot].Load(int3(min(pixel * 2 + int2(0, 1), last), 0)).rgb;
  sum += g_textures[slot].Load(int3(min(pixel * 2 + int2(1, 1), last), 0)).rgb;
  return float4(sum * 0.25f, 1.0f);
}

#else  // AO_COLOR_PASS == 2

float4 main(float4 position : SV_Position) : SV_Target {
  const uint view = AoView();
  const float2 uv = position.xy / g_push.sizes.xy;
  const uint slot = g_push.slots.z;
  float4 texel = g_textures[slot].SampleLevel(g_samplers[kSamplerLinearClamp], uv, 0.0f);
  float3 shown = texel.rgb;
  if (view == 2u) {
    shown = texel.yzw;
  } else if (view == 3u) {
    shown = texel.xxx;
  } else if (view == 4u) {
    // The bounce, cleaned by a 5x5 mean (of the image's own texels, nothing cleverer: a preview of
    // what a denoise will make of it, not the denoise).
    float3 sum = float3(0.0f, 0.0f, 0.0f);
    for (int j = -2; j <= 2; ++j) {
      for (int i = -2; i <= 2; ++i) {
        sum += g_textures[slot].SampleLevel(g_samplers[kSamplerLinearClamp],
                                            uv + float2(i, j) / g_push.sizes.zw, 0.0f).yzw;
      }
    }
    shown = sum / 25.0f;
  }
  const float gain = g_push.shape.x > 0.0f ? g_push.shape.x : 1.0f;
  return float4(max(shown, 0.0f) * (kViewScale * gain), 1.0f);
}

#endif
