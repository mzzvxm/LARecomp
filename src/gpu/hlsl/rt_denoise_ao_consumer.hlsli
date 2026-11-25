#ifndef MCLA_RT_DENOISE_AO_CONSUMER_HLSLI
#define MCLA_RT_DENOISE_AO_CONSUMER_HLSLI

#include "host_pass_common.hlsli"
#include "gbuffer_common.hlsli"
#include "rt_denoise_common.hlsli"
#include "rtao_denoise_math.hlsli"

#define RT_DN_PROFILE RT_DN_PROFILE_AMBIENT

// Same 32-word block as the plain consumer. The history keeps octahedral
// normals in gb. The spatial result carries the current judged flag and ray count.
struct RtDnPush { uint4 r[8]; };
#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<RtDnPush> g_push;
#else
ConstantBuffer<RtDnPush> g_push : register(b0, space4);
#endif

uint RtDnW(uint i) { return g_push.r[i >> 2u][i & 3u]; }
float RtDnF(uint i) { return asfloat(RtDnW(i)); }
uint RtDnLo(uint word) { return word & 0xFFFFu; }
uint RtDnHi(uint word) { return word >> 16u; }

int2 RtDnSize() { return int2(RtDnLo(RtDnW(3)), RtDnHi(RtDnW(3))); }
uint RtDnIteration() { return RtDnW(4) & 15u; }
bool RtDnHasHistory() { return (RtDnW(4) & (1u << 8u)) != 0u; }
bool RtAoSpatialResolve() { return RtDnW(27) != 0u; }
float RtDnPixelAngle() { return 2.0f * RtDnF(6) / float(RtDnSize().y); }
bool RtAoVehicle(float depth) { return (asuint(depth) & 1u) != 0u; }
float RtDnDepthAt(int2 p) { return g_textures[RtDnHi(RtDnW(0))].Load(int3(p, 0)).x; }
float2 RtAoIdShare(int2 p) { return g_textures[RtDnHi(RtDnW(1))].Load(int3(p, 0)).xy; }
float4 RtAoRawAt(int2 p) { return g_textures[RtDnW(26)].Load(int3(p, 0)); }
float4 RtDnSignal(int2 p) { return g_textures[RtDnLo(RtDnW(0))].Load(int3(p, 0)); }
float3 RtAoPosition(float2 pixel, float depth, float2 lens) {
  const float2 ndc = (pixel + 0.5f) / float2(RtDnSize()) * 2.0f - 1.0f;
  return float3(ndc * float2(lens.x, -lens.y) * depth, depth);
}

float4 RtDnColourAt(int2 p) {
  const float open = RtDnSignal(p).x;
  return float4(open, open, open, RtAoIdShare(p).y >= kRtDnMinShare ? 1.0f : 0.0f);
}

RtDnPixel RtDnLoad(int2 p) {
  RtDnPixel c = (RtDnPixel)0;
  const float2 guide = RtAoIdShare(p);
  const float depth = RtDnDepthAt(p);
  c.z = depth;
  c.share = guide.y;
  c.rough = 1.0f;
  c.hit = 0.0f;
  c.vehicle = RtAoVehicle(depth) ? uint(guide.x) + 1u : 0u;
  c.ok = depth > 0.0f && c.share >= kRtDnMinShare;
  if (c.ok) {
    const float4 s = RtDnSignal(p);
    c.colour = s.xxx;
    c.length = s.a;
    c.pos = RtAoPosition(float2(p), depth, float2(RtDnF(5), RtDnF(6)));
    c.n = g_textures[RtDnLo(RtDnW(1))].Load(int3(p, 0)).xyz;
    // An invalid normal cannot seed a history texel.
    c.ok = dot(c.n, c.n) > 1.0e-6f;
  }
  return c;
}

#endif
