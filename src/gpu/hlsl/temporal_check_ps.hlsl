// MCLA GPU renderer -- the temporal system's checks, the GPU half (temporal_mv.h, mcla_gpu_temporal_check).
//
// A 16 x 9 grid of sample pixels over the frame, each answered by two texels of a 32 x 9 float4 image
// that the host reads back and judges in double precision:
//   even texel   (depth, vehicle ID, vector x, vector y)  of the sample pixel p, from this frame's kept
//                depth, the anchor's stencil view and the kept vector image
//   odd texel    (depth of the previous frame's image at q, q inside the image, colour error with the
//                vector, colour error without), where
//                q = p + 0.5 + mv + (previous jitter - this jitter): the previous image shows the point
//                at its unjittered position plus the previous jitter, and the vectors are jitter free.
// The colour error is the mean over red, green and blue of |a - b| in 0..255 after a clamp and a gamma
// of 1 / 2.2 (the scene colour is HDR and linear); "with the vector" reads the previous colour at q
// bilinearly, "without" is the previous colour at the same pixel.

#include "host_pass_common.hlsli"

struct CheckPush {
  uint depth;       // this frame's kept depth (our R32_FLOAT image)
  uint stencil;     // the anchor's stencil view
  uint motion;      // our RG16_FLOAT vectors
  uint prev_depth;  // the previous frame's kept depth
  uint color;       // this frame's colour (our copy)
  uint prev_color;  // the previous frame's colour (our copy)
  float2 delta;     // previous jitter minus this frame's jitter, render pixels
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<CheckPush> g_push;
#else
ConstantBuffer<CheckPush> g_push : register(b0, space4);
#endif

float3 Encode(float3 color) {
  return pow(saturate(color), 1.0f / 2.2f);
}

// The previous colour at the continuous position `q` (texel centres at +0.5), edges clamped.
float3 Bilinear(uint slot, float2 q, int2 size) {
  const float2 p = q - 0.5f;
  const int2 i = int2(floor(p));
  const float2 f = p - float2(i);
  const int2 hi = size - 1;
  const float3 a = g_textures[slot].Load(int3(clamp(i, int2(0, 0), hi), 0)).rgb;
  const float3 b = g_textures[slot].Load(int3(clamp(i + int2(1, 0), int2(0, 0), hi), 0)).rgb;
  const float3 c = g_textures[slot].Load(int3(clamp(i + int2(0, 1), int2(0, 0), hi), 0)).rgb;
  const float3 d = g_textures[slot].Load(int3(clamp(i + int2(1, 1), int2(0, 0), hi), 0)).rgb;
  return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

float4 main(float4 position : SV_Position) : SV_Target {
  const uint2 cell = uint2(position.xy);
  const uint2 sample_at = uint2(cell.x >> 1, cell.y);  // 16 x 9
  uint width;
  uint height;
  g_textures[g_push.depth].GetDimensions(width, height);
  // Integer arithmetic: the host finds the same pixel.
  const int2 pixel = int2(((sample_at * 2u + 1u) * uint2(width, height)) / uint2(32u, 18u));
  const int3 at = int3(pixel, 0);
  const float2 mv = g_textures[g_push.motion].Load(at).xy;
  if ((cell.x & 1u) == 0u) {
    const float depth = g_textures[g_push.depth].Load(at).x;
    const uint raw = uint(g_textures[g_push.stencil & 0x7FFFFFFFu].Load(at).x * 255.0f + 0.5f);
    uint id = raw >> 2;
    if (id >= 24u) {
      id = 0u;
    }
    return float4(depth, float(id), mv);
  }
  const float2 q = float2(pixel) + 0.5f + mv + g_push.delta;
  const int2 size = int2(width, height);
  const int2 qi = int2(floor(q));
  if (any(qi < int2(0, 0)) || any(qi >= size)) {
    return float4(0.0f, 0.0f, -1.0f, -1.0f);
  }
  const float prev_depth = g_textures[g_push.prev_depth].Load(int3(qi, 0)).x;
  const float3 now = Encode(g_textures[g_push.color].Load(at).rgb);
  const float3 warped = Encode(Bilinear(g_push.prev_color, q, size));
  const float3 still = Encode(g_textures[g_push.prev_color].Load(at).rgb);
  const float warp_error = dot(abs(now - warped), float3(1.0f, 1.0f, 1.0f)) * (255.0f / 3.0f);
  const float still_error = dot(abs(now - still), float3(1.0f, 1.0f, 1.0f)) * (255.0f / 3.0f);
  return float4(prev_depth, 1.0f, warp_error, still_error);
}
