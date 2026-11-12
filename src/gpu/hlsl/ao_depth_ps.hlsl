// MCLA GPU renderer -- ambient occlusion, pass 1: the linear depth (ao.h).
//
// The EDRAM depth target's guest sample 0 (Texture2DMS when AO_DEPTH_MS, else a plain texture),
// linearised with the lens into view-space distance, R32_FLOAT, at the AO's size. At half
// resolution (AoLow) each pixel stands for a 2x2 block of the depth plane: the one of its four
// samples nearest to the block's mean, so that a pixel never takes a depth that no sample of its
// block has (an average across an edge would).

#include "ao_common.hlsli"

float4 main(float4 position : SV_Position) : SV_Target {
  const int2 pixel = int2(position.xy);
  const int2 last = int2(g_push.sizes.zw) - 1;
  float t;
  if (AoLow()) {
    float4 d;
    d.x = AoLinearDepth(AoLoadStored(min(pixel * 2 + int2(0, 0), last)));
    d.y = AoLinearDepth(AoLoadStored(min(pixel * 2 + int2(1, 0), last)));
    d.z = AoLinearDepth(AoLoadStored(min(pixel * 2 + int2(0, 1), last)));
    d.w = AoLinearDepth(AoLoadStored(min(pixel * 2 + int2(1, 1), last)));
    const float mean = (d.x + d.y + d.z + d.w) * 0.25f;
    t = d.x;
    float best = abs(d.x - mean);
    if (abs(d.y - mean) < best) {
      t = d.y;
      best = abs(d.y - mean);
    }
    if (abs(d.z - mean) < best) {
      t = d.z;
      best = abs(d.z - mean);
    }
    if (abs(d.w - mean) < best) {
      t = d.w;
    }
  } else {
    t = AoLinearDepth(AoLoadStored(min(pixel, last)));
  }
  return float4(t, 0.0f, 0.0f, 0.0f);
}
