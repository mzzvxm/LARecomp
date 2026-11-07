// MCLA GPU renderer -- frame generation: the scene prepared for the interpolation (frame_generation.h).
//
// FSR's frame generation interpolates between the colour of the frame before and this frame's, both
// in one image format and at one size, the generation size. This pass makes that image: the frame's
// scene (the 8-bit hudless colour of the frame layers, or HDR stage 3's float scene) at the
// generation size, in the game's code values, as RGBA16F.
//
// The scene is read bilinearly at the sample positions the present pass uses for the real frame
// (present_ps.hlsl: (pixel + 0.5) * source texels per pixel - 0.5), so a generated frame shown at the
// size the real one is scaled to is scaled just as it is.
//
// FLAG_FLOAT_SCENE: the scene is the float one, whose code values may exceed 1 (the light the tone map
// clipped, HDR output). The interpolation (its optical flow keeps an 8-bit luma) is given a bounded
// image: each channel x is mapped to 0.5 * x up to 1 and 0.5 * (2 - 1/x) above it, which is exactly
// invertible (fg_compose_ps.hlsl) and stays below 1. The 8-bit scene never exceeds 1 and is left as it
// is.

#include "host_pass_common.hlsli"

struct PreparePush {
  uint scene;       // the scene's bindless slot
  uint flags;       // bit 0: the float scene (compress)
  uint src_width;   // the region of the scene that is the picture, texels
  uint src_height;
  uint out_width;   // the generation size
  uint out_height;
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<PreparePush> g_push;
#else
ConstantBuffer<PreparePush> g_push : register(b0, space4);
#endif

static const uint kFlagFloatScene = 1u;

float3 Texel(int2 at) {
  const int2 limit = int2(g_push.src_width, g_push.src_height) - 1;
  const float3 c = g_textures[g_push.scene].Load(int3(clamp(at, int2(0, 0), limit), 0)).rgb;
  // Finite and not negative: a NaN a float16 draw left in the scene is none.
  const uint3 bits = asuint(c) & 0x7FFFFFFFu;
  return float3((bits.x <= 0x7F800000u) ? max(c.x, 0.0f) : 0.0f,
                (bits.y <= 0x7F800000u) ? max(c.y, 0.0f) : 0.0f,
                (bits.z <= 0x7F800000u) ? max(c.z, 0.0f) : 0.0f);
}

float Compress(float x) {
  return 0.5f * (x <= 1.0f ? x : 2.0f - 1.0f / x);
}

float4 main(float4 position : SV_Position) : SV_Target {
  const float2 scale = float2(g_push.src_width, g_push.src_height) /
                       float2(g_push.out_width, g_push.out_height);
  const float2 p = position.xy * scale - 0.5f;
  const float2 cell = floor(p);
  const float2 f = p - cell;
  const int2 at = int2(cell);
  float3 color;
  if (all(f == 0.0f)) {
    color = Texel(at);  // 1:1: one texel, as the present pass does
  } else {
    const float3 top = lerp(Texel(at), Texel(at + int2(1, 0)), f.x);
    const float3 bottom = lerp(Texel(at + int2(0, 1)), Texel(at + int2(1, 1)), f.x);
    color = lerp(top, bottom, f.y);
  }
  if ((g_push.flags & kFlagFloatScene) != 0u) {
    color = float3(Compress(color.x), Compress(color.y), Compress(color.z));
  }
  return float4(color, 1.0f);
}
