// MCLA GPU renderer -- the shared screen products' debug view (screen_shared.h, mcla_gpu_screen_view).
//
// Drawn into the main view's colour target at the mid-scene depth copy, in a pipeline that REPLACES the colour (no blend): a product of the shared frame as a picture. The images
// are at the traced size, the target at the picture's; each pixel reads the texel it falls in.
//
//   view 1  the linear depth, level 0: a grey (near bright, a logarithmic scale), black for the sky
//   view 2  the Hi-Z level mcla_gpu_screen_view_level: the same grey; the level's texels are visible as blocks
//   view 3  the Hi-Z check: the level against the linear depth of each pixel it covers. RED where the level is deeper than the pixel (cannot be: it is a min), otherwise green,
//           brighter where the min is nearer than the pixel (an edge); level 0 is all dark green (the same depth)
//   view 4  the normal image (0.5 + 0.5 n), black where the frame has none
//   view 5  the vehicle mark of level 0: white on a vehicle's pixels, grey elsewhere on geometry, black for the sky
//   view 6  colour_post level mcla_gpu_screen_view_level (slots.y is that level's image), each texel stretched over the pixels it covers, black where the frame has none
//   view 7  the history's colour (slots.y: the last frame's colour_post), the same

#include "host_pass_common.hlsli"
#include "gbuffer_common.hlsli"
#include "screen_hiz.hlsli"

struct ScreenViewPush {
  uint4 slots;  // x the linear depth level 0's bindless slot, y the Hi-Z level's slot (the colour image's for the views 6 and 7; 0xFFFFFFFF: none), z the normal image's slot (0xFFFFFFFF: none), w the view
  uint4 sizes;  // xy the pass's target size (the picture), zw the traced size (the size of level 0)
  uint4 level;  // x the level shown, yz that level's size
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<ScreenViewPush> g_push;
#else
ConstantBuffer<ScreenViewPush> g_push : register(b0, space4);
#endif

static const uint kNone = 0xFFFFFFFFu;
static const float kSkyLimit = 59999.0f;

float Load(uint slot, int2 p) {
  return g_textures[slot].Load(int3(p, 0)).x;
}

// The grey of a distance: 1 at the camera, 0 at 1000 m, logarithmic; the sky is black.
float3 DepthGrey(float d) {
  if (d >= kSkyLimit) {
    return float3(0.0f, 0.0f, 0.0f);
  }
  return (1.0f - saturate(log2(1.0f + d) / 10.0f)).xxx;
}

float4 main(float4 position : SV_Position) : SV_Target {
  const float2 uv = position.xy / float2(g_push.sizes.xy);
  const int2 traced = int2(g_push.sizes.zw);
  // The pixel of level 0 this target pixel falls in.
  const int2 p0 = clamp(int2(uv * float2(traced)), int2(0, 0), traced - 1);
  const uint view = g_push.slots.w;
  float3 colour = float3(0.0f, 0.0f, 0.0f);
  if (view == 1u) {
    colour = DepthGrey(Load(g_push.slots.x, p0));
  } else if (view == 2u) {
    if (g_push.slots.y != kNone) {
      const int2 size = int2(g_push.level.yz);
      colour = DepthGrey(Load(g_push.slots.y, clamp(int2(uv * float2(size)), int2(0, 0), size - 1)));
    }
  } else if (view == 3u) {
    if (g_push.slots.y != kNone) {
      const uint level = g_push.level.x;
      const int2 size = int2(g_push.level.yz);
      const float d0 = ScreenHizStrip(Load(g_push.slots.x, p0));
      const float h = Load(g_push.slots.y, clamp(p0 >> int(level), int2(0, 0), size - 1));
      if (h > d0 * 1.00001f + 1.0e-5f) {
        colour = float3(1.0f, 0.0f, 0.0f);
      } else {
        colour = float3(0.0f, 0.25f + 0.75f * saturate((d0 - h) / max(d0, 0.01f) * 8.0f), 0.0f);
      }
    }
  } else if (view == 4u) {
    if (g_push.slots.z != kNone) {
      // (the normal image has the traced size, so the pixel of level 0 is its texel)
      colour = GbOctDecode(g_textures[g_push.slots.z].Load(int3(p0, 0)).xy) * 0.5f + 0.5f;
    }
  } else if (view == 5u) {
    const float d = Load(g_push.slots.x, p0);
    if (d < kSkyLimit) {
      colour = (asuint(d) & 1u) != 0u ? float3(1.0f, 1.0f, 1.0f) : float3(0.2f, 0.2f, 0.2f);
    }
  } else if (view == 6u || view == 7u) {
    if (g_push.slots.y != kNone) {
      // (the target is the main view's colour: the capture holds the same units, so it shows as the game drew it before the tone map)
      const int2 size = int2(g_push.level.yz);
      colour = g_textures[g_push.slots.y].Load(int3(clamp(int2(uv * float2(size)), int2(0, 0), size - 1), 0)).rgb;
    }
  }
  return float4(colour, 1.0f);
}
