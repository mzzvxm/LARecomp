// MCLA GPU renderer -- the shared screen products' colour capture (screen_shared.h CapturePost): colour_post and its mip chain.
//
// Three compilations (SCREEN_COLOUR_PASS, SCREEN_COLOUR_MS), each a full-screen triangle into an RGBA16_FLOAT image:
//
//   0 / 0  screen_colour_ps       level 0 from the main view's colour target as a plain texture
//   0 / 1  screen_colour_ms_ps    level 0 from the colour target as a multisampled texture (the game's MSAA)
//   1      screen_colour_mip_ps   a level above 0: the mean of the 2x2 texels of the level below
//
// Level 0. A texel is the mean over the pixels it covers (one at the picture's size, 2x2 at half size: slots.z says which, 1 or 2) of the pixel's mean over its host samples, in the target's own
// units (the shaders' output times 2^exp_bias), a NaN counted as black and the result held to what a half float keeps. At the picture's size the arithmetic is EXACTLY the car history pass's
// (hlsl/car_history_ps.hlsl: the same loop over the samples in the same order, the same division, the same clamp), so that the car history may read this image in place of the planes without a
// byte changing (car_selfrefl.h, the source setting); the 2x2 case adds the four pixels' means and takes a quarter. The sky is a colour like any other; nothing here reads the depth.
//
// Push block (screen_shared.cpp ColourPush), 32 bytes: slots.x the colour plane's bindless slot (the level below's for the mip pass), slots.y its host samples (the pass reads Texture2DMS
// only when the compilation says so), slots.z the ratio of the source to this image (1 or 2); sizes.xy this image's size, sizes.zw the source's.

#include "host_pass_common.hlsli"

#ifndef SCREEN_COLOUR_PASS
#define SCREEN_COLOUR_PASS 0
#endif
#ifndef SCREEN_COLOUR_MS
#define SCREEN_COLOUR_MS 0
#endif

struct ScreenColourPush {
  uint4 slots;
  uint4 sizes;
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<ScreenColourPush> g_push;
#else
ConstantBuffer<ScreenColourPush> g_push : register(b0, space4);
#endif

// The largest colour a half float keeps with room (the car history's kMaxColour).
static const float kMaxColour = 60000.0f;

#if SCREEN_COLOUR_PASS == 0

float4 main(float4 position : SV_Position) : SV_Target {
  const int ratio = int(max(g_push.slots.z, 1u));
  const int2 base = int2(position.xy) * ratio;
  const int2 last = int2(g_push.sizes.zw) - 1;
#if SCREEN_COLOUR_MS
  const uint samples = max(g_push.slots.y, 1u);
#endif
  float3 total = float3(0.0f, 0.0f, 0.0f);
  for (int j = 0; j < ratio; ++j) {
    for (int i = 0; i < ratio; ++i) {
      const int2 p = min(base + int2(i, j), last);
#if SCREEN_COLOUR_MS
      float3 sum = float3(0.0f, 0.0f, 0.0f);
      [loop] for (uint s = 0u; s < samples; ++s) {
        sum += g_textures_ms[g_push.slots.x].Load(p, s).rgb;
      }
      total += sum / float(samples);
#else
      total += g_textures[g_push.slots.x].Load(int3(p, 0)).rgb;
#endif
    }
  }
  const float3 colour = total / float(ratio * ratio);
  return float4(min(max(colour, 0.0f), kMaxColour), 1.0f);
}

#else  // SCREEN_COLOUR_PASS == 1

float4 main(float4 position : SV_Position) : SV_Target {
  const int2 pixel = int2(position.xy);
  const int2 last = int2(g_push.sizes.zw) - 1;
  const uint slot = g_push.slots.x;
  float3 sum = g_textures[slot].Load(int3(min(pixel * 2 + int2(0, 0), last), 0)).rgb;
  sum += g_textures[slot].Load(int3(min(pixel * 2 + int2(1, 0), last), 0)).rgb;
  sum += g_textures[slot].Load(int3(min(pixel * 2 + int2(0, 1), last), 0)).rgb;
  sum += g_textures[slot].Load(int3(min(pixel * 2 + int2(1, 1), last), 0)).rgb;
  return float4(sum * 0.25f, 1.0f);
}

#endif
