#ifndef MCLA_RTAO_REPROJECT_HLSLI
#define MCLA_RTAO_REPROJECT_HLSLI

// Guide-only reprojection shared by the temporal pass and the optional boost
// pre-pass. The previous normal travels with the length in the history image.
struct RtAoHistory {
  float open;
  float length;
  float cap;
};

RtAoHistory RtAoReproject(int2 p, RtDnPixel c) {
  RtAoHistory r = {0.0f, 0.0f, RtAoMotionCap(0.0f, float(RtDnW(25)))};
  if (!RtDnHasHistory() || r.cap <= 1.0f) return r;

  float4 m0, m1, m2;
  if (c.vehicle != 0u) {
    const uint id = c.vehicle - 1u;
    if (id == 0u || id > 23u || ((RtDnW(23) >> 8u) & (1u << id)) == 0u) return r;
    const uint table = (RtDnW(23) & 0xFFu) | (RtDnW(24) << 8u);
    if (g_textures[table].Load(int3(3, id, 0)).x <= 0.5f) return r;
    m0 = g_textures[table].Load(int3(0, id, 0));
    m1 = g_textures[table].Load(int3(1, id, 0));
    m2 = g_textures[table].Load(int3(2, id, 0));
  } else {
    m0 = float4(RtDnF(9), RtDnF(10), RtDnF(11), RtDnF(12));
    m1 = float4(RtDnF(13), RtDnF(14), RtDnF(15), RtDnF(16));
    m2 = float4(RtDnF(17), RtDnF(18), RtDnF(19), RtDnF(20));
  }
  const float4 position = float4(c.pos, 1.0f);
  const float3 prev = float3(dot(m0, position), dot(m1, position), dot(m2, position));
  const float2 lens = float2(RtDnF(7), RtDnF(8));
  if (!(prev.z > 1.0e-3f) || !RtDnIsNumber3(prev) || !all(lens > 0.0f)) return r;
  const float2 projected = (float2(0.5f, -0.5f) * prev.xy / (prev.z * lens) + 0.5f) * float2(RtDnSize()) - 0.5f;
  if (!all(projected > -1.0f) || !all(projected < float2(RtDnSize()))) return r;

  const float3 rotated = float3(dot(m0.xyz, c.n), dot(m1.xyz, c.n), dot(m2.xyz, c.n));
  if (!(dot(rotated, rotated) > 1.0e-6f) || !RtDnIsNumber3(rotated)) return r;
  const float3 prev_normal = normalize(rotated);
  r.cap = RtAoMotionCap(length(projected - float2(p)), float(RtDnW(25)));
  const int2 base = int2(floor(projected));
  const float2 f = frac(projected);
  float weights = 0.0f;
  float lengths = 0.0f;
  float opens = 0.0f;
  [unroll] for (int y = 0; y < 2; ++y) {
    [unroll] for (int x = 0; x < 2; ++x) {
      const int2 q = base + int2(x, y);
      if (any(q < 0) || any(q >= RtDnSize())) continue;
      const float depth = g_textures[RtDnHi(RtDnW(2))].Load(int3(q, 0)).x;
      if (!(depth > 0.0f) || !RtDnIsNumber(depth)) continue;
      const float4 h = g_textures[RtDnLo(RtDnW(2))].Load(int3(q, 0));
      if (!(h.a > 0.0f) || !RtDnIsNumber(h.a) || !RtDnIsNumber3(h.rgb)) continue;
      const bool same_vehicle = RtAoVehicle(depth) == (c.vehicle != 0u);
      if (!RtAoAcceptTap(depth, prev.z, same_vehicle, dot(prev_normal, GbOctDecode(h.gb)))) continue;
      const float plane = abs(dot(prev_normal, RtAoPosition(float2(q), depth, lens) - prev));
      const float bilinear = (x == 0 ? 1.0f - f.x : f.x) * (y == 0 ? 1.0f - f.y : f.y);
      const float w = bilinear * RtDnDepthWeight(plane, prev.z, RtDnDepthSigma(RT_DN_PROFILE_AMBIENT));
      weights += w;
      lengths += h.a * w;
      opens += h.r * w;
    }
  }
  r.length = RtAoHistoryLength(lengths, weights, r.cap);
  r.open = r.length > 0.0f ? opens / weights : 0.0f;
  return r;
}

#endif
