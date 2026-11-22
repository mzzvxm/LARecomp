// MCLA GPU renderer -- the shared screen products: one level of the Hi-Z chain (screen_shared.h, hlsl/screen_hiz.hlsli).
//
// A full-screen triangle into an R32_FLOAT image of the level's size. Mode 0 makes level 0: the depth chain's level 0 with its marks cleared. Mode 1 makes a level above it:
// the MIN of the 2x2 texels of the level below, an odd edge clamped (the last column or row is read twice), so that no texel is dropped. One pass a level, run in order.

#include "host_pass_common.hlsli"
#include "screen_hiz.hlsli"

struct HizPush {
  uint4 slots;  // x the source image's bindless slot (the depth chain's level 0, or the level below), y the mode (0: level 0, 1: a level above it)
  uint4 sizes;  // xy the source's size in texels, zw this level's size
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<HizPush> g_push;
#else
ConstantBuffer<HizPush> g_push : register(b0, space4);
#endif

float Load(uint slot, int2 p) {
  return g_textures[slot].Load(int3(p, 0)).x;
}

float4 main(float4 position : SV_Position) : SV_Target {
  const int2 pixel = int2(position.xy);
  const int2 last = int2(g_push.sizes.xy) - 1;
  const uint slot = g_push.slots.x;
  float depth;
  if (g_push.slots.y == 0u) {
    depth = ScreenHizStrip(Load(slot, min(pixel, last)));
  } else {
    float4 d;
    d.x = Load(slot, min(pixel * 2 + int2(0, 0), last));
    d.y = Load(slot, min(pixel * 2 + int2(1, 0), last));
    d.z = Load(slot, min(pixel * 2 + int2(0, 1), last));
    d.w = Load(slot, min(pixel * 2 + int2(1, 1), last));
    depth = ScreenHizMin4(d);
  }
  return float4(depth, 0.0f, 0.0f, 0.0f);
}
