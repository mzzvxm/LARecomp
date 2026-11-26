#include "ssr_pass.hlsli"
#include "rt_denoise_common.hlsli"

#define RT_DN_PROFILE RT_DN_PROFILE_SPECULAR

// Retain the SSR material, wetness and projection words. The last motion row
// uses the four depth-linearization words, which this pass never reads.
uint Dn2W(uint i) { return SsrW(i); }
int2 RtDnSize() { return SsrSize(); }
float2 Dn2Lens() { return float2(SsrF(SSR_W_TAN_X), SsrF(SSR_W_TAN_Y)); }
float2 Dn2PreviousLens() { return float2(SsrF(SSR_W_PREV_TAN_X), SsrF(SSR_W_PREV_TAN_Y)); }
float4 RtDnSignal(int2 p) { return g_textures[SsrLo(SsrW(SSR_W_SRC))].Load(int3(p, 0)); }

#include "rt_denoise_v2_motion.hlsli"

RtDnPixel RtDnLoad(int2 p) {
  RtDnPixel c = (RtDnPixel)0;
  const float4 raw = RtDnSignal(p);
  const float z = SsrHizAt(SsrHz(), p.x, p.y);
  if (!(z > 0.0f && z < kSsrSky * 0.5f) || raw.w == 0.0f) return c;
  const float2 lens = Dn2Lens();
  const int2 size = RtDnSize();
  const int2 texel = min(p * 2, SsrRender() - 1);
  const SsrSurface surface = SsrSurfaceAt(texel, false);
  const float2 confidence = g_textures[SsrLo(SsrW(SSR_W_COL01))].Load(int3(p, 0)).xy;
  c.ok = true;
  c.z = z;
  c.pos = Dn2Position(float2(p), z, lens);
  c.n = SsrDepthNormal(SsrHz(), p.x, p.y, size.x, size.y, z, c.pos, lens.x, lens.y);
  c.colour = raw.rgb;
  c.rough = 1.0f - saturate(SsrWetSmooth(surface.cls, surface.smoothness, SsrF(SSR_W_WET)));
  c.hit = SsrHitDistance(raw.w);
  c.share = confidence.x > 0.0f ? 1.0f - saturate(confidence.y) : 0.0f;
  const uint hit_id = SsrHitVehicleId(raw.w);
  const uint id = SsrHitIsVehicle(raw.w) ? (hit_id == 0u ? 31u : hit_id) : 0u;
  c.dn2_surface = (id << 5u) | min(surface.cls, 31u);
  return c;
}

// Receiver material and the reflected vehicle ID must agree independently.
// Keep the unknown vehicle marker separate from a world-space hit.
uint Dn2SurfaceId(RtDnPixel c, int2 p) {
  return c.dn2_surface;
}

Dn2Motion Dn2ReceiverMotion(uint surface) {
  Dn2Motion m;
  m.x = float4(SsrF(20), SsrF(21), SsrF(22), SsrF(23));
  m.y = float4(SsrF(24), SsrF(25), SsrF(26), SsrF(27));
  m.z = float4(SsrF(14), SsrF(15), SsrF(16), SsrF(17));
  m.valid = true;
  return m;
}

float2 Dn2ReflectionPixel(Dn2Motion receiver, RtDnPixel c, uint surface, float hit, float2 lens, float2 receiver_pixel, out float reliability) {
  const uint id = surface >> 5u;
  const float3 virtual_point = SsrVirtualPoint(c.pos, hit);
  const float3 previous = Dn2Transform(receiver, virtual_point, 1.0f);
  reliability = 0.0f;
  if (previous.z <= 1.0e-3f) return float2(-2.0f, -2.0f);
  float2 image = Dn2Project(previous, lens);
  reliability = 1.0f;
  if (id != 0u) {
    const uint table = SsrHi(SsrW(SSR_W_COL01));
    reliability = kSsrVehicleNoRowWeight;
    if (id <= 23u && SsrHasSlot(table) && g_textures[table].Load(int3(3, id, 0)).x > 0.5f) {
      const float4 v0 = g_textures[table].Load(int3(0, id, 0));
      const float4 v1 = g_textures[table].Load(int3(1, id, 0));
      const float4 v2 = g_textures[table].Load(int3(2, id, 0));
      const SsrPrevious reflected = SsrVehicleImage(receiver.x, receiver.y, receiver.z, v0, v1, v2, c.pos, c.n, hit, lens.x, lens.y);
      if (!reflected.ok) return float2(-2.0f, -2.0f);
      image = float2(reflected.u, reflected.v) * float2(RtDnSize()) - 0.5f;
      reliability = kSsrVehicleRowWeight;
    }
  }
  return lerp(receiver_pixel, image, RtDnVirtualShare(c.rough, hit));
}

float Dn2HistoryCap(float pixels) {
  const float weight = clamp(SsrF(SSR_W_HISTORY_WEIGHT), 0.0f, 0.98f);
  return min(1.0f / (1.0f - weight), lerp(24.0f, 6.0f, saturate(pixels / 16.0f)));
}

float4 Dn2Composite(int2 p, float3 colour, float depth, float samples) { return float4(colour, depth); }
