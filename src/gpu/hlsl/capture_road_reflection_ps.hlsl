#include "ssr_pass.hlsli"
#include "reflhit.hlsli"

float2 main(float4 position : SV_Position) : SV_Target {
  const int2 p = int2(position.xy);
  const uint sources = SsrW(SSR_W_SRC);
  const float3 correction = NgFinite3(g_textures[SsrLo(sources)].Load(int3(p, 0)).rgb);
  const uint hit = g_utextures[SsrHi(sources)].Load(int3(p, 0)).x;
  const float3 planar = NgFinite3(SsrRoadViewAt(SsrW(SSR_W_ROAD_VIEW_APPLY), position.xy / float2(SsrSize())));
  // Reconstruct the combined lobe in the trace's scene-linear units, before the receiver BRDF.
  // The planar contribution uses the renderer's native estimate (the road view and game strength).
  const float strength = SsrF(SSR_W_FALLBACK);
  const float cap = SsrF(SSR_W_CAP);
  const float3 combined = float3(RoadReflectionLobe(correction.r, planar.r, strength, cap),
                                 RoadReflectionLobe(correction.g, planar.g, strength, cap),
                                 RoadReflectionLobe(correction.b, planar.b, strength, cap));
  const float energy = dot(combined, float3(0.2126f, 0.7152f, 0.0722f));
  return float2(min(energy, 65504.0f), float(ReflHitConfidence(hit)) / 63.0f);
}
