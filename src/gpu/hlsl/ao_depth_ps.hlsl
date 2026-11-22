// MCLA GPU renderer -- ambient occlusion, pass 1: the linear depth (ao.h).
//
// The EDRAM depth target's guest sample 0 (Texture2DMS when AO_DEPTH_MS, else a plain texture),
// linearised with the lens into view-space distance, R32_FLOAT, at the AO's size. At half
// resolution (AoLow) each pixel stands for a 2x2 block of the depth plane: where the four samples
// are one surface (their spread under kHalfSurfaceShare of the depth, the share the apply's upsample
// calls the same surface) the MEAN, which is the surface's depth at the block's centre where the pixel's
// position is; across an edge the one of the four nearest to the block's mean, so that a pixel never
// takes a depth that no sample of its block has (an average across an edge would).
// The mean matters on a surface seen at a grazing angle (a road, a wall along the street): its depth
// changes by centimetres to metres from one row of the plane to the next, and the sample nearest to the
// mean is always one of the corners, half a pixel off the block's centre. The surface then comes out
// as a staircase in the half size image, its normals and horizons are wrong and the occlusion is
// darker by a few hundredths on every grazing plane (measured on the software rasteriser, flat road and
// wall: 0.04 darker on average against the full size image, 0.003 with the mean).
//
// The lowest bit of each depth says whether the pixel is a vehicle (ao_common.hlsli, AoIsVehicle): the
// depth target's stencil plane (slots.z, 0xFFFFFFFF when there is none) holds a vehicle's ID in its upper
// six bits while the car is drawn. The occlusion's main pass uses the bit to keep a car's reach on the
// road short (ao_main_ps.hlsl).
//
// With the G-buffer's normals on (slots2.w not 0; GB-E, ao_normal_ps.hlsl, ao_gbuffer_normal.hlsli) the two bits above the mark also say which of the block's four samples a
// half size pixel stands for (0 top left, 1 top right, 2 bottom left, 3 bottom right: the sample whose depth and stencil the pixel took, the one nearest the block's mean),
// so that the normal pass takes the G-buffer's texel of the same surface. Seven units in the last place of the depth at the most; with slots2.w 0 the depth is what it was.

#include "ao_common.hlsli"
#include "ao_gbuffer_normal.hlsli"
#if AO_HIZ0
#include "screen_hiz.hlsli"
#endif

// A vehicle's stencil ID is the stencil byte >> 2: 1 to 23 (the UI's draws use 24 and up), as in
// ao_temporal_ps.hlsl.
static const uint kFirstVehicleId = 1u;
static const uint kLastVehicleId = 23u;

// The block's four samples are one surface when their spread (largest minus smallest linear depth) is under this share of the mean
// depth plus 2 cm (ao_size.h kHalfSurfaceShare, kHalfSurfaceFloor): a depth step the apply's upsample (ao_apply_ps.hlsl VisibilityAt,
// 2% of the depth, 5 cm) takes for the same surface is nearly not an edge here either, and a grazing plane's own slope stays under it for
// the distances the occlusion works at (a road seen from 1.4 m up: to 105 m in a picture of 2160 rows, to 70 m in 1440; the occlusion has
// faded out from 80 m).
static const float kHalfSurfaceShare = 0.04f;
static const float kHalfSurfaceFloor = 0.02f;

// Whether the depth target's pixel `p` (the guest's sample 0) belongs to a vehicle.
bool IsVehiclePixel(int2 p) {
  const uint slot = g_push.slots.z;
  if (slot == 0xFFFFFFFFu) {
    return false;
  }
#if AO_DEPTH_MS
  const uint id = StencilOf(g_utextures_ms[slot].Load(p, g_push.slots.y)) >> 2;
#else
  const uint id = StencilOf(g_utextures[slot].Load(int3(p, 0))) >> 2;
#endif
  return id >= kFirstVehicleId && id <= kLastVehicleId;
}

// The texel the pass writes (the depth with the vehicle mark and, with the G-buffer's normals, the source of the half size block's sample): one function for the plain pass and for the fused one (AO_HIZ0).
float AoDepthTexel(float2 position_xy) {
  const int2 pixel = int2(position_xy);
  const int2 last = int2(g_push.sizes.zw) - 1;
  float t;
  int2 source;
  uint source_index = 0u;
  if (AoLow()) {
    const int2 p0 = min(pixel * 2 + int2(0, 0), last);
    const int2 p1 = min(pixel * 2 + int2(1, 0), last);
    const int2 p2 = min(pixel * 2 + int2(0, 1), last);
    const int2 p3 = min(pixel * 2 + int2(1, 1), last);
    float4 d;
    d.x = AoLinearDepth(AoLoadStored(p0));
    d.y = AoLinearDepth(AoLoadStored(p1));
    d.z = AoLinearDepth(AoLoadStored(p2));
    d.w = AoLinearDepth(AoLoadStored(p3));
    const float mean = (d.x + d.y + d.z + d.w) * 0.25f;
    t = d.x;
    source = p0;
    float best = abs(d.x - mean);
    if (abs(d.y - mean) < best) {
      t = d.y;
      source = p1;
      source_index = 1u;
      best = abs(d.y - mean);
    }
    if (abs(d.z - mean) < best) {
      t = d.z;
      source = p2;
      source_index = 2u;
      best = abs(d.z - mean);
    }
    if (abs(d.w - mean) < best) {
      t = d.w;
      source = p3;
      source_index = 3u;
    }
    // One surface: its depth at the block's centre (the vehicle mark and the stencil stay the chosen sample's).
    const float spread = max(max(d.x, d.y), max(d.z, d.w)) - min(min(d.x, d.y), min(d.z, d.w));
    if (spread <= kHalfSurfaceShare * mean + kHalfSurfaceFloor) {
      t = mean;
    }
  } else {
    source = min(pixel, last);
    t = AoLinearDepth(AoLoadStored(source));
  }
  const float marked = AoSetVehicleBit(t, IsVehiclePixel(source));
  return g_push.slots2.w != 0u ? AoGbSetSource(marked, source_index) : marked;
}

// AO_HIZ0 (SS-3, screen_shared.h): the same pass also writes the Hi-Z's level 0 into a second target (SV_Target1, R32_FLOAT, the same size): the texel it writes with the marks off
// (screen_hiz.hlsli ScreenHizStrip), which is what the Hi-Z's own level 0 pass made of this image. The first target is exactly what it was: the same function.
#if AO_HIZ0
struct DepthHizOut {
  float4 depth : SV_Target0;
  float4 hiz : SV_Target1;
};
DepthHizOut main(float4 position : SV_Position) {
  const float stored = AoDepthTexel(position.xy);
  DepthHizOut o;
  o.depth = float4(stored, 0.0f, 0.0f, 0.0f);
  o.hiz = float4(ScreenHizStrip(stored), 0.0f, 0.0f, 0.0f);
  return o;
}
#else
float4 main(float4 position : SV_Position) : SV_Target {
  return float4(AoDepthTexel(position.xy), 0.0f, 0.0f, 0.0f);
}
#endif
