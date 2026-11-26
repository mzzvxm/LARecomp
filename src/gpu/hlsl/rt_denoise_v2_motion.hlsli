#ifndef MCLA_RT_DENOISE_V2_MOTION_HLSLI
#define MCLA_RT_DENOISE_V2_MOTION_HLSLI

struct Dn2Motion {
  float4 x, y, z;
  bool valid;
};

float3 Dn2Transform(Dn2Motion m, float3 value, float w) {
  const float4 p = float4(value, w);
  return float3(dot(m.x, p), dot(m.y, p), dot(m.z, p));
}

float3 Dn2Position(float2 pixel, float z, float2 lens) {
  const float2 uv = (pixel + 0.5f) / float2(RtDnSize());
  return float3((uv * 2.0f - 1.0f) * float2(lens.x, -lens.y) * z, z);
}

float2 Dn2Project(float3 p, float2 lens) {
  return (p.xy / (lens * max(p.z, 1.0e-4f)) * float2(0.5f, -0.5f) + 0.5f) * float2(RtDnSize()) - 0.5f;
}

bool Dn2InImage(float2 p) { return all(p > -1.0f) && all(p < float2(RtDnSize())); }

#endif
