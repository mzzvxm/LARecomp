#ifndef MCLA_GPU_SSR_CLASSIFY_HLSLI
#define MCLA_GPU_SSR_CLASSIFY_HLSLI

#include "ssr_classify_rules.hlsli"

// Frame fields appended to reflection_handoff::Frame.
uint SsrClassifyFlags() { return g_handoff.Load(128u); }
static const uint kSsrClassifyEnabled = 1u;
static const uint kSsrClassifyHistory = 2u;
static const uint kSsrClassifySparse = 4u;

float4 SsrEmptyTile() { return float4(0.0f, 0.0f, 0.0f, float(kSsrTileMixed)); }

// Filtered hit and miss counts, variance and packed age/mode. Reproject
// the tile representative once in compute. The trace reads that result.
float4 SsrTileReproject(int2 tile) {
  const float4 empty = SsrEmptyTile();
  const int2 size = SsrSize();
  const int2 centre = min(tile * int(kSsrTileSize) + 4, size - 1);
  const float z = SsrHizAt(SsrHz(), centre.x, centre.y);
  if (!(z < kSsrSky * 0.5f)) return empty;
  const float3 view = SsrViewPos(float(centre.x) + 0.5f, float(centre.y) + 0.5f,
      float(size.x), float(size.y), z, SsrF(SSR_W_TAN_X), SsrF(SSR_W_TAN_Y));
  const float4 position = float4(view, 1.0f);
  const float3 previous = float3(dot(asfloat(g_handoff.Load4(80u)), position),
      dot(asfloat(g_handoff.Load4(96u)), position), dot(asfloat(g_handoff.Load4(112u)), position));
  const float3 lens = asfloat(g_handoff.Load3(48u));
  if (!(previous.z > lens.z)) return empty;
  const float2 uv = 0.5f + float2(previous.x, -previous.y) / (2.0f * previous.z * lens.xy);
  if (!(all(uv >= 0.0f) && all(uv < 1.0f))) return empty;
  const int2 pixel = min(int2(uv * float2(size)), size - 1);
  const float prev_z = g_textures[g_handoff.Load(24u)].Load(int3(pixel, 0)).x;
  if (!(abs(prev_z - previous.z) <= max(0.12f, 0.045f * previous.z))) return empty;
  return g_textures[g_handoff.Load(132u)].Load(int3(pixel / int(kSsrTileSize), 0));
}

float4 SsrTileHistory(int2 tile) {
  if ((SsrClassifyFlags() & kSsrClassifyHistory) == 0u) return SsrEmptyTile();
  return g_textures[g_handoff.Load(148u)].Load(int3(tile, 0));
}

#endif
