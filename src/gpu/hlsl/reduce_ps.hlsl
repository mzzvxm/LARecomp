// MCLA GPU renderer -- a resolve product reduced to a smaller resolution
// scale (resolve.h, ResolveCache::ReducedSlot).
//
// A pass at a smaller scale than a product was made at samples the guest's
// image at its own density: each texel of the copy is the mean of the
// product's ratio x ratio block over it, or -- for a product a depth source
// made -- the block's centre texel (a mean of depths is no depth), as the
// encode into the resolve memory takes it (resolve_memory_cs.hlsl).

#include "host_pass_common.hlsli"

struct ReducePush {
  uint source;  // the product, every level, RGBA (a bindless 2D slot)
  uint ratio;   // product texels per copy texel, each way
  uint centre;  // 1: the block's centre texel instead of its mean
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<ReducePush> g_push;
#else
ConstantBuffer<ReducePush> g_push : register(b0, space4);
#endif

float4 main(float4 position : SV_Position) : SV_Target {
  const int r = int(g_push.ratio);
  const int2 origin = int2(position.xy) * r;
  if (g_push.centre != 0u) {
    return g_textures[g_push.source].Load(int3(origin + (r >> 1), 0));
  }
  float4 sum = 0.0f;
  for (int y = 0; y < r; ++y) {
    for (int x = 0; x < r; ++x) {
      sum += g_textures[g_push.source].Load(int3(origin + int2(x, y), 0));
    }
  }
  return sum / float(r * r);
}
