// MCLA GPU renderer -- ambient occlusion, pass 1: the linear depth (ao.h).
//
// The EDRAM depth target's guest sample 0 (Texture2DMS when AO_DEPTH_MS, else a plain texture),
// linearised with the lens into view-space distance, R32_FLOAT, at the AO's size. At half
// resolution (AoLow) each pixel stands for a 2x2 block of the depth plane: the one of its four
// samples nearest to the block's mean, so that a pixel never takes a depth that no sample of its
// block has (an average across an edge would).
//
// The lowest bit of each depth says whether the pixel is a vehicle (ao_common.hlsli, AoIsVehicle): the
// depth target's stencil plane (slots.z, 0xFFFFFFFF when there is none) holds a vehicle's ID in its upper
// six bits while the car is drawn. The occlusion's main pass uses the bit to keep a car's reach on the
// road short (ao_main_ps.hlsl).

#include "ao_common.hlsli"

// A vehicle's stencil ID is the stencil byte >> 2: 1 to 23 (the UI's draws use 24 and up), as in
// ao_temporal_ps.hlsl.
static const uint kFirstVehicleId = 1u;
static const uint kLastVehicleId = 23u;

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

float4 main(float4 position : SV_Position) : SV_Target {
  const int2 pixel = int2(position.xy);
  const int2 last = int2(g_push.sizes.zw) - 1;
  float t;
  int2 source;
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
      best = abs(d.y - mean);
    }
    if (abs(d.z - mean) < best) {
      t = d.z;
      source = p2;
      best = abs(d.z - mean);
    }
    if (abs(d.w - mean) < best) {
      t = d.w;
      source = p3;
    }
  } else {
    source = min(pixel, last);
    t = AoLinearDepth(AoLoadStored(source));
  }
  return float4(AoSetVehicleBit(t, IsVehiclePixel(source)), 0.0f, 0.0f, 0.0f);
}
