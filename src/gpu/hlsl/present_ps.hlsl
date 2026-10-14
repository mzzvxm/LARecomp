// MCLA GPU renderer -- the guest's front buffer on the window (present.h).
//
// The console's display reads the front buffer through the fetch the Swap
// builds (its swizzle in the view: (Z, Y, X, 1) for MCLA), runs every pixel's
// 8-bit value through the gamma table the title gave the device (256 entries
// per channel, 10 bits; Xenia's apply_gamma_table.xesli indexes it the same
// way) and then scales to the output. Here: the table applied to each of the
// four texels a bilinear scale reads, then mixed -- the table before the
// scaler, as on the display -- into the letterboxed viewport the renderer set.
// A 1:1 present reads exactly one texel per pixel.

#include "host_pass_common.hlsli"

struct PresentPush {
  uint source;         // front buffer view, the Swap's swizzle applied
  uint ramp;           // 256x1 R32G32B32A32_FLOAT: the table / 1023, per channel
  uint source_width;
  uint source_height;
  float origin_x;      // back buffer pixel where the image starts
  float origin_y;
  float scale_x;       // source texels per back buffer pixel
  float scale_y;
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<PresentPush> g_push;
#else
ConstantBuffer<PresentPush> g_push : register(b0, space4);
#endif

float3 Ramp(float3 color) {
  // The display reads the 8-bit value: UNORM conversion, D3D10+ rules.
  const uint3 index = uint3(saturate(color) * 255.0f + 0.5f);
  return float3(g_textures[g_push.ramp].Load(int3(index.r, 0, 0)).r,
                g_textures[g_push.ramp].Load(int3(index.g, 0, 0)).g,
                g_textures[g_push.ramp].Load(int3(index.b, 0, 0)).b);
}

float3 Texel(int2 at) {
  const int2 limit = int2(g_push.source_width, g_push.source_height) - 1;
  return Ramp(g_textures[g_push.source].Load(int3(clamp(at, int2(0, 0), limit), 0)).rgb);
}

float4 main(float4 position : SV_Position) : SV_Target {
  const float2 p = (position.xy - float2(g_push.origin_x, g_push.origin_y)) *
                       float2(g_push.scale_x, g_push.scale_y) -
                   0.5f;
  const float2 cell = floor(p);
  const float2 f = p - cell;
  const int2 at = int2(cell);
  const float3 top = lerp(Texel(at), Texel(at + int2(1, 0)), f.x);
  const float3 bottom = lerp(Texel(at + int2(0, 1)), Texel(at + int2(1, 1)), f.x);
  return float4(lerp(top, bottom, f.y), 1.0f);
}
