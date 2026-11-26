#include "rt_denoise_v2_motion.hlsli"

uint Dn2W(uint i) { return RtDnW(i); }
float2 Dn2Lens() { return float2(RtDnF(5), RtDnF(6)); }
float2 Dn2PreviousLens() { return float2(RtDnF(7), RtDnF(8)); }

uint Dn2SurfaceId(RtDnPixel c, int2 p) {
#if RT_DN_PROFILE == RT_DN_PROFILE_AMBIENT
  return c.vehicle;
#elif defined(RT_DN_SHADOW)
  return asuint(RtDnDepthAt(p)) & 1u;
#else
  return 0;
#endif
}

Dn2Motion Dn2ReceiverMotion(uint surface) {
  Dn2Motion m;
  m.valid = true;
#ifdef RT_DN_SHADOW
  // The linear depth marks car receivers but carries no per-car motion.
  if (surface != 0u) return (Dn2Motion)0;
#endif
#if RT_DN_PROFILE == RT_DN_PROFILE_AMBIENT
  if (surface != 0u) {
    const uint id = surface - 1u;
    const uint table = (RtDnW(23) & 255u) | (RtDnW(24) << 8u);
    if (id == 0u || id > 23u || ((RtDnW(23) >> 8u) & (1u << id)) == 0u ||
        g_textures[table].Load(int3(3, id, 0)).x <= 0.5f) return (Dn2Motion)0;
    m.x = g_textures[table].Load(int3(0, id, 0));
    m.y = g_textures[table].Load(int3(1, id, 0));
    m.z = g_textures[table].Load(int3(2, id, 0));
    return m;
  }
#endif
  m.x = float4(RtDnF(9), RtDnF(10), RtDnF(11), RtDnF(12));
  m.y = float4(RtDnF(13), RtDnF(14), RtDnF(15), RtDnF(16));
  m.z = float4(RtDnF(17), RtDnF(18), RtDnF(19), RtDnF(20));
  return m;
}

float Dn2HistoryCap(float pixels) {
#if RT_DN_PROFILE == RT_DN_PROFILE_AMBIENT
  return RtAoMotionCap(pixels, float(RtDnW(25)));
#elif defined(RT_DN_SHADOW)
  return lerp(12.0f, 4.0f, saturate(pixels / 8.0f));
#elif defined(RT_DN_VIS)
  return lerp(20.0f, 6.0f, saturate(pixels / 12.0f));
#else
  return lerp(24.0f, 8.0f, saturate(pixels / 16.0f));
#endif
}

float4 Dn2Composite(int2 p, float3 colour, float depth, float samples) {
#if RT_DN_PROFILE == RT_DN_PROFILE_AMBIENT
  return float4(saturate(colour.x), RtAoRawAt(p).gb, samples);
#else
  return float4(colour, samples);
#endif
}
