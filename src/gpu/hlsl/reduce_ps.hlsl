// MCLA GPU renderer -- a resolve product copied to a reader at another
// resolution scale (resolve.h, ResolveCache::ReducedSlot).
//
// A pass at a smaller scale than a product was made at samples the guest's
// image at its own density: each texel of the copy is the mean of the
// product's block over it -- by area when the ratio is fractional, each
// product texel weighted by how much of it the block covers -- or, for a
// product a depth source made, the texel that holds the block's centre (a
// mean of depths is no depth), as the encode into the resolve memory takes it
// (resolve_memory_cs.hlsl). The centre also serves a depth source enlarged
// for a pass at a larger scale: the nearest texel, never a blend of depths.

#include "host_pass_common.hlsli"

struct ReducePush {
  uint source;  // the product, every level, RGBA (a bindless 2D slot)
  uint num;     // num / den product texels per copy texel, each way: the
  uint den;     // product's Scale::n over the copy's
  uint centre;  // 1: the texel at the block's centre instead of its mean
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<ReducePush> g_push;
#else
ConstantBuffer<ReducePush> g_push : register(b0, space4);
#endif

float4 main(float4 position : SV_Position) : SV_Target {
  const uint2 p = uint2(position.xy);
  if (g_push.centre != 0u) {
    // floor((p + 0.5) * num / den), in integers.
    return g_textures[g_push.source].Load(int3((p * 2u + 1u) * g_push.num / (2u * g_push.den), 0));
  }
  if (g_push.num % g_push.den == 0u) {
    // A whole ratio: the plain mean of the r x r block.
    const int r = int(g_push.num / g_push.den);
    const int2 origin = int2(p) * r;
    float4 sum = 0.0f;
    for (int y = 0; y < r; ++y) {
      for (int x = 0; x < r; ++x) {
        sum += g_textures[g_push.source].Load(int3(origin + int2(x, y), 0));
      }
    }
    return sum / float(r * r);
  }
  // A fractional ratio: the block [p * q, (p + 1) * q) by area. The last
  // block may reach past the product by the rounding of the two sizes: the
  // product's edge texel stands in for what lies beyond it.
  uint width, height, levels;
  g_textures[g_push.source].GetDimensions(0, width, height, levels);
  const float q = float(g_push.num) / float(g_push.den);
  const float2 b0 = float2(p) * q;
  const float2 b1 = b0 + q;
  const int2 t0 = int2(floor(b0));
  const int2 t1 = int2(ceil(b1));
  const int2 last = int2(width, height) - 1;
  float4 sum = 0.0f;
  for (int y = t0.y; y < t1.y; ++y) {
    const float wy = min(float(y + 1), b1.y) - max(float(y), b0.y);
    for (int x = t0.x; x < t1.x; ++x) {
      const float wx = min(float(x + 1), b1.x) - max(float(x), b0.x);
      sum += g_textures[g_push.source].Load(int3(min(int2(x, y), last), 0)) * (wx * wy);
    }
  }
  return sum / (q * q);
}
