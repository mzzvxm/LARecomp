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
//
// PRESENT_NO_RAMP (present_stretch_ps): the same bilinear stretch
// of an image the table was already applied to (FSR1's output, when its passes
// stopped short of the letterbox).
//
// PRESENT_HDR (present_hdr_ps, present_stretch_hdr_ps): the back buffer is an HDR10 swap
// chain. The picture is made exactly as above, in SDR code values, and its last step is
// hdr_output.hlsli's HdrFromSdr (or the test pattern) with the levels in the push constants.

#include "host_pass_common.hlsli"
#if PRESENT_HDR
#include "hdr_output.hlsli"
#endif

struct PresentPush {
  uint source;         // front buffer view, the Swap's swizzle applied
  uint ramp;           // 256x1 R32G32B32A32_FLOAT: the table / 1023, per channel
  uint source_width;
  uint source_height;
  float origin_x;      // back buffer pixel where the image starts
  float origin_y;
  float scale_x;       // source texels per back buffer pixel
  float scale_y;
  float hdr_white;     // PRESENT_HDR only (hdr.h HdrLevels): SDR white in nits
  float hdr_peak;
  float hdr_knee;
  uint hdr_flags;
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
  const float3 texel = g_textures[g_push.source].Load(int3(clamp(at, int2(0, 0), limit), 0)).rgb;
#if PRESENT_NO_RAMP
  return texel;
#else
  return Ramp(texel);
#endif
}

// The pixel's colour: the picture's SDR code values, or HDR10 code values of them.
float4 Finish(float3 color, float2 p) {
#if PRESENT_HDR
  HdrLevels levels;
  levels.white = g_push.hdr_white;
  levels.peak = g_push.hdr_peak;
  levels.knee = g_push.hdr_knee;
  levels.flags = g_push.hdr_flags;
  if ((levels.flags & 2u) != 0u) {
    // p is in source texels (less the half texel); the pattern covers the picture's area.
    return float4(HdrTestPattern((p + 0.5f) / float2(g_push.source_width, g_push.source_height),
                                 levels),
                  1.0f);
  }
  return float4(HdrFromSdr(color, levels), 1.0f);
#else
  return float4(color, 1.0f);
#endif
}

float4 main(float4 position : SV_Position) : SV_Target {
  const float2 p = (position.xy - float2(g_push.origin_x, g_push.origin_y)) *
                       float2(g_push.scale_x, g_push.scale_y) -
                   0.5f;
  const float2 cell = floor(p);
  const float2 f = p - cell;
  const int2 at = int2(cell);
  // At 1:1 (the image at its own size, on whole pixels) both weights are 0 and the other three
  // texels add nothing (lerp(a, b, 0) is a exactly): one texel and its three table reads instead
  // of four and twelve, the same result. Every pixel of the draw takes the same branch then.
  if (all(f == 0.0f)) {
    return Finish(Texel(at), p);
  }
  const float3 top = lerp(Texel(at), Texel(at + int2(1, 0)), f.x);
  const float3 bottom = lerp(Texel(at + int2(0, 1)), Texel(at + int2(1, 1)), f.x);
  return Finish(lerp(top, bottom, f.y), p);
}
