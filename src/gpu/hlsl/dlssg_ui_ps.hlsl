// MCLA GPU renderer -- DLSS frame generation: the UI layer in the back buffer's terms
// (dlss_frame_generation.h).
//
// Streamline's frame generation is given, besides the back buffer, the picture without the UI (the
// present pass run again over the frame layers' hudless image) and a UI buffer: premultiplied colour
// and coverage, "0 where there is no UI", at the back buffer's size and in the back buffer's colour
// space. The UI layer of the frame layers (frame_layers.h: what the UI phase drew into a cleared
// target, colour premultiplied by its coverage, coverage in alpha) is laid into the window as the
// present pass lays the picture (the same letterbox, bilinear), and its colour goes through what the
// display does to the back buffer's pixels: the game's gamma table, and with an HDR10 back buffer
// the output step of hdr_output.hlsli (PRESENT_HDR). The table is applied to the colour already
// multiplied by the coverage, which is exact where the UI is opaque and where it is absent, and
// near enough between (a faded edge): the buffers are hints to the interpolation here (the
// recomposition option stays off).
//
// A taps with no coverage have no colour (the table maps 0 to its first entry, which need not be 0).

#include "host_pass_common.hlsli"
#if PRESENT_HDR
#include "hdr_output.hlsli"
#endif

struct UiPush {
  uint ui;             // the UI layer, bindless slot
  uint ramp;           // 256x1 R32G32B32A32_FLOAT: the table / 1023, per channel
  uint src_width;      // the layer's texels that the picture covers
  uint src_height;
  float origin_x;      // back buffer pixel where the picture starts (the letterbox)
  float origin_y;
  float scale_x;       // source texels per back buffer pixel
  float scale_y;
  float hdr_white;     // PRESENT_HDR only (hdr.h HdrLevels)
  float hdr_peak;
  float hdr_knee;
  uint hdr_flags;
  uint gap;            // the layer's first columns and rows that hold no picture (Frame::gap): read at `gap`
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<UiPush> g_push;
#else
ConstantBuffer<UiPush> g_push : register(b0, space4);
#endif

float3 RampAt(uint3 index) {
  return float3(g_textures[g_push.ramp].Load(int3(index.r, 0, 0)).r,
                g_textures[g_push.ramp].Load(int3(index.g, 0, 0)).g,
                g_textures[g_push.ramp].Load(int3(index.b, 0, 0)).b);
}

// One layer texel as the back buffer would hold it: colour through the table (and the HDR10 output
// step), coverage as it is.
float4 UiTexel(int2 at) {
  const int2 limit = int2(g_push.src_width, g_push.src_height) - 1;
  // As the compose and the frame generation passes read the layers (frame_layers.h, Frame::gap).
  const int2 first = int2(int(g_push.gap), int(g_push.gap));
  const float4 texel = g_textures[g_push.ui].Load(int3(clamp(at, first, limit), 0));
  if (texel.a <= 0.0f) {
    return float4(0.0f, 0.0f, 0.0f, 0.0f);
  }
  float3 color = RampAt(uint3(saturate(texel.rgb) * 255.0f + 0.5f));
#if PRESENT_HDR
  HdrLevels levels;
  levels.white = g_push.hdr_white;
  levels.peak = g_push.hdr_peak;
  levels.knee = g_push.hdr_knee;
  levels.flags = g_push.hdr_flags;
  color = HdrFromSdr(color, levels);
#endif
  return float4(color, texel.a);
}

float4 main(float4 position : SV_Position) : SV_Target {
  // Source texels at this pixel; outside the letterbox there is no picture, so no UI.
  const float2 at_source = (position.xy - float2(g_push.origin_x, g_push.origin_y)) *
                           float2(g_push.scale_x, g_push.scale_y);
  if (any(at_source < 0.0f) || at_source.x >= float(g_push.src_width) ||
      at_source.y >= float(g_push.src_height)) {
    return float4(0.0f, 0.0f, 0.0f, 0.0f);
  }
  const float2 p = at_source - 0.5f;
  const float2 cell = floor(p);
  const float2 f = p - cell;
  const int2 at = int2(cell);
  if (all(f == 0.0f)) {
    return UiTexel(at);
  }
  const float4 top = lerp(UiTexel(at), UiTexel(at + int2(1, 0)), f.x);
  const float4 bottom = lerp(UiTexel(at + int2(0, 1)), UiTexel(at + int2(1, 1)), f.x);
  return lerp(top, bottom, f.y);
}
