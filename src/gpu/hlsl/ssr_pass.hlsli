// MCLA GPU renderer -- screen-space reflections (ssr.h, GB-I1): what the three passes share that touches a resource: the push block, the reads of the guest's depth, the G-buffer's planes, the Hi-Z
// and the colour. The pure functions are in ssr_common.hlsli and ssr_trace.hlsli (src/gpu/ssr_test.cpp compiles those as C++); this file is HLSL only.
//
// DESCRIPTORS. Every resource is read through a bindless slot taken from the push block, and the slot of every read is the same for the whole draw (a value of the push, even where a branch on
// the level picks it): there is no NonUniformResourceIndex anywhere, so no pass needs the device's non uniform indexing. The Hi-Z and colour levels are therefore chains of branches, one
// slot each, and not an array indexed by the level.
//
// Compilations: SSR_MS 0 / 1 reads the guest's depth plane and the G-buffer's planes as plain textures (space 0) or as the host's multisampled ones (space 1, read at the guest's sample 0).

#ifndef MCLA_GPU_SSR_PASS_HLSLI
#define MCLA_GPU_SSR_PASS_HLSLI

#include "host_pass_common.hlsli"
#include "host_pass_samplers.hlsli"

#ifndef SSR_MS
#define SSR_MS 0
#endif
#define SSR_LOOP [loop]

#include "ssr_common.hlsli"

struct SsrPush {
  uint4 r[8];   // 32 words (ssr_rules.h push::)
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<SsrPush> g_push;
#else
ConstantBuffer<SsrPush> g_push : register(b0, space4);
#endif

uint SsrW(uint i) {
  return g_push.r[i >> 2u][i & 3u];
}
float SsrF(uint i) {
  return asfloat(SsrW(i));
}

// ---- the frame's numbers ---------------------------------------------------------------------------------------------------------------------------------------------------------------
uint SsrSample0() { return SsrW(SSR_W_FLAGS) & 15u; }
uint SsrHizLevels() { return (SsrW(SSR_W_FLAGS) >> 4u) & 15u; }
uint SsrColourLevels() { return (SsrW(SSR_W_FLAGS) >> 8u) & 15u; }
bool SsrFlag(uint flag) { return ((SsrW(SSR_W_FLAGS) >> SSR_FLAG_SHIFT) & flag) != 0u; }
uint SsrRays() { return ((SsrW(SSR_W_FLAGS) >> SSR_RAYS_SHIFT) & 3u) + 1u; }
uint SsrView() { return (SsrW(SSR_W_FLAGS) >> SSR_VIEW_SHIFT) & 15u; }
uint SsrMaxIter() { return SsrW(SSR_W_FLAGS) >> SSR_ITER_SHIFT; }
int2 SsrRender() { return int2(int(SsrLo(SsrW(SSR_W_RENDER))), int(SsrHi(SsrW(SSR_W_RENDER)))); }
// The SSR images' size: half of the picture, rounded up.
int2 SsrSize() { return (SsrRender() + 1) / 2; }
// The Hi-Z level that has that size (SSR_FLAG_HZ: 1 when the traced size is the picture's), and level 0's size.
uint SsrHz() { return SsrFlag(SSR_FLAG_HZ) ? 1u : 0u; }
int2 SsrHiz0Size() { return SsrFlag(SSR_FLAG_HZ) ? SsrRender() : SsrSize(); }

// ---- the guest's depth and the G-buffer's planes (at the picture's pixels) ---------------------------------------------------------------------------------------------------------------
float SsrStoredAt(int2 p) {
  const uint slot = SsrLo(SsrW(SSR_W_DEPTH_RT1));
#if SSR_MS
  return g_textures_ms[slot].Load(p, SsrSample0()).r;
#else
  return g_textures[slot].Load(int3(p, 0)).r;
#endif
}
float SsrLinearAt(int2 p) {
  return SsrLinearDepth(SsrStoredAt(p), SsrF(SSR_W_LIN_X), SsrF(SSR_W_LIN_Y), SsrF(SSR_W_LIN_Z), SsrF(SSR_W_LIN_W));
}
float2 SsrRt1At(int2 p) {
  const uint slot = SsrHi(SsrW(SSR_W_DEPTH_RT1));
#if SSR_MS
  return g_textures_ms[slot].Load(p, SsrSample0()).xy;
#else
  return g_textures[slot].Load(int3(p, 0)).xy;
#endif
}
float4 SsrRt2At(int2 p) {
  const uint slot = SsrLo(SsrW(SSR_W_RT2_RT3));
  if (!SsrHasSlot(slot)) {
    return float4(0.0f, 0.0f, 0.0f, 0.0f);
  }
#if SSR_MS
  return g_textures_ms[slot].Load(p, SsrSample0());
#else
  return g_textures[slot].Load(int3(p, 0));
#endif
}
float2 SsrRt3At(int2 p) {
  const uint slot = SsrHi(SsrW(SSR_W_RT2_RT3));
#if SSR_MS
  return g_textures_ms[slot].Load(p, SsrSample0()).xy;
#else
  return g_textures[slot].Load(int3(p, 0)).xy;
#endif
}

// ---- the Hi-Z (the trace's hook) ---------------------------------------------------------------------------------------------------------------------------------------------------------
float SsrHizAt(uint level, int cx, int cy) {
  if (level == 0u) {
    return g_textures[SsrLo(SsrW(SSR_W_HIZ01))].Load(int3(cx, cy, 0)).x;
  }
  if (level == 1u) {
    return g_textures[SsrHi(SsrW(SSR_W_HIZ01))].Load(int3(cx, cy, 0)).x;
  }
  if (level == 2u) {
    return g_textures[SsrLo(SsrW(SSR_W_HIZ23))].Load(int3(cx, cy, 0)).x;
  }
  if (level == 3u) {
    return g_textures[SsrHi(SsrW(SSR_W_HIZ23))].Load(int3(cx, cy, 0)).x;
  }
  if (level == 4u) {
    return g_textures[SsrLo(SsrW(SSR_W_HIZ45))].Load(int3(cx, cy, 0)).x;
  }
  return g_textures[SsrHi(SsrW(SSR_W_HIZ45))].Load(int3(cx, cy, 0)).x;
}

// The depth chain's level 0 (the vehicle mark is its lowest bit: ao_common.hlsli AoIsVehicle) at a texel of the Hi-Z's level 0.
bool SsrVehicleAt(int cx, int cy) {
  const uint slot = SsrLo(SsrW(SSR_W_LIN0));
  if (!SsrHasSlot(slot)) {
    return false;
  }
  return (asuint(g_textures[slot].Load(int3(cx, cy, 0)).x) & 1u) != 0u;
}

// GB-I2: the vehicle (stencil ID 1 to 23: the byte >> 2 the main view's draws write) on a texel of the Hi-Z's level 0, 0 where there is none or the stencil plane is not in the push. Where level 0 is
// the picture's size the texel is the pixel; where it is half, the texel stands for a 2x2 block of the depth plane and the vehicle is the first of its four samples that is one (the AO's rule,
// ao_temporal_ps.hlsl AoVehicleId: on a silhouette the block's first sample can be the other surface).
uint SsrStencilId(uint slot, int2 p) {
#if SSR_MS
  const uint id = StencilOf(g_utextures_ms[slot].Load(p, SsrSample0())) >> 2;
#else
  const uint id = StencilOf(g_utextures[slot].Load(int3(p, 0))) >> 2;
#endif
  return (id >= 1u && id <= 23u) ? id : 0u;
}
uint SsrVehicleIdAt(int cx, int cy) {
  const uint slot = SsrHi(SsrW(SSR_W_STENCIL));
  if (!SsrHasSlot(slot)) {
    return 0u;
  }
  if (SsrFlag(SSR_FLAG_HZ)) {
    return SsrStencilId(slot, int2(cx, cy));
  }
  const int2 last = SsrRender() - 1;
  for (int k = 0; k < 4; ++k) {
    const uint id = SsrStencilId(slot, min(int2(cx * 2 + (k & 1), cy * 2 + (k >> 1)), last));
    if (id != 0u) {
      return id;
    }
  }
  return 0u;
}

// GB-I2: the game's road reflection view at a place of the picture. The view is the whole screen drawn with the roads' own mirrored geometry into a 256x256 target at the guest's size (an
// integer multiple above it, side_views.h), whose top left corner is the picture's top left: `word` is SSR_W_ROAD_VIEW (slot, host samples, side), `uv` 0..1 over the picture. Bilinear by hand (the
// samples of a multisampled target are averaged, as the resolve that makes the game's own texture does), the picture's own colour units. 0 where the word has no slot.
float3 SsrRoadViewTexel(uint slot, uint samples, int side, int2 p) {
  const int2 q = clamp(p, int2(0, 0), int2(side - 1, side - 1));
  if (samples <= 1u) {
    return g_textures[slot].Load(int3(q, 0)).rgb;
  }
  float3 sum = float3(0.0f, 0.0f, 0.0f);
  [loop] for (uint s = 0u; s < samples; ++s) {
    sum += g_textures_ms[slot].Load(q, s).rgb;
  }
  return sum * (1.0f / float(samples));
}
float3 SsrRoadViewAt(uint word, float2 uv) {
  const uint slot = SsrRoadViewSlot(word);
  const int side = int(SsrRoadViewSide(word));
  if (!SsrHasSlot(slot) || side <= 0) {
    return float3(0.0f, 0.0f, 0.0f);
  }
  const uint samples = SsrRoadViewSamples(word);
  const float2 pos = uv * float(side) - 0.5f;
  const int2 p0 = int2(floor(pos));
  const float2 f = pos - float2(p0);
  const float3 a = SsrRoadViewTexel(slot, samples, side, p0);
  const float3 b = SsrRoadViewTexel(slot, samples, side, p0 + int2(1, 0));
  const float3 c = SsrRoadViewTexel(slot, samples, side, p0 + int2(0, 1));
  const float3 d = SsrRoadViewTexel(slot, samples, side, p0 + int2(1, 1));
  return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

// ---- the colour (colour_post and its mips) ---------------------------------------------------------------------------------------------------------------------------------------------
float3 SsrColourLevel(uint level, float2 uv) {
  if (level == 0u) {
    return g_textures[SsrLo(SsrW(SSR_W_COL01))].SampleLevel(g_samplers[kSamplerLinearClamp], uv, 0.0f).rgb;
  }
  if (level == 1u) {
    return g_textures[SsrHi(SsrW(SSR_W_COL01))].SampleLevel(g_samplers[kSamplerLinearClamp], uv, 0.0f).rgb;
  }
  if (level == 2u) {
    return g_textures[SsrLo(SsrW(SSR_W_COL23))].SampleLevel(g_samplers[kSamplerLinearClamp], uv, 0.0f).rgb;
  }
  return g_textures[SsrHi(SsrW(SSR_W_COL23))].SampleLevel(g_samplers[kSamplerLinearClamp], uv, 0.0f).rgb;
}
// The colour at `lod` (a level of the chain, blended between the two it falls between), 0 where the chain is not there.
float3 SsrColour(float2 uv, float lod) {
  const uint levels = max(SsrColourLevels(), 1u);
  const float l = clamp(lod, 0.0f, float(levels - 1u));
  const uint l0 = uint(floor(l));
  const float t = l - float(l0);
  const float3 a = SsrColourLevel(l0, uv);
  if (t < 0.02f || l0 + 1u >= levels) {
    return a;
  }
  return lerp(a, SsrColourLevel(l0 + 1u, uv), t);
}

// ---- the surface of a pixel --------------------------------------------------------------------------------------------------------------------------------------------------------------
struct SsrSurface {
  uint cls;
  uint flags;
  float smoothness;   // before the wetness
  float world_up;     // the world normal's y where RT1 holds one (-2 where not)
};
// The class, flags and smoothness of the pixel `p` (the picture's): RT3's, or the stub's (no RT3 in the frame: a road wherever the world normal points up and the depth chain's mark is no vehicle;
// `vehicle` is that mark, read by the caller at the texel that belongs to the pixel).
SsrSurface SsrSurfaceAt(int2 p, bool vehicle) {
  SsrSurface s;
  s.cls = uint(GB_CLASS_NONE);
  s.flags = 0u;
  s.smoothness = 0.0f;
  s.world_up = -2.0f;
  if (SsrFlag(SSR_FLAG_STUB)) {
    const float2 rt1 = SsrRt1At(p);
    if (GbRt1Valid(rt1) && !vehicle) {
      const float3 nw = GbRt1Decode(rt1);
      s.world_up = nw.y;
      s.cls = SsrStubClass(nw.y);
      s.flags = s.cls == uint(GB_CLASS_NONE) ? 0u : uint(GB_FLAG_SSR);
      s.smoothness = SsrF(SSR_W_STUB_SMOOTH);
    }
    return s;
  }
  const float2 rt3 = SsrRt3At(p);
  s.cls = GbRt3Class(rt3);
  s.flags = GbRt3Flags(rt3);
  s.smoothness = GbRt3Smoothness(rt3);
  return s;
}

// The view-space normal of the pixel `p` at view position `pc`: RT1's world normal turned into view space with the camera's axes (the frame's own: SSR_FLAG_CAMERA), facing the camera; w 0 where
// there is none (no data, a back face, no camera).
float4 SsrViewNormalAt(int2 p, float3 pc) {
  if (!SsrFlag(SSR_FLAG_CAMERA)) {
    return float4(0.0f, 0.0f, 0.0f, 0.0f);
  }
  const float3 right = float3(SsrF(SSR_W_RIGHT), SsrF(SSR_W_RIGHT + 1u), SsrF(SSR_W_RIGHT + 2u));
  const float3 up = float3(SsrF(SSR_W_UP), SsrF(SSR_W_UP + 1u), SsrF(SSR_W_UP + 2u));
  return AoGbViewNormal(SsrRt1At(p), pc, right, up, cross(up, right));
}

// The depth's own normal at texel (cx, cy) of the Hi-Z level `level` (size w x h): the neighbour on each axis whose depth is nearer to the pixel's, as ao_common.hlsli AoNormalFromImage; the
// fallback where RT1 has none.
float3 SsrDepthNormal(uint level, int cx, int cy, int w, int h, float zc, float3 pc, float tan_x, float tan_y) {
  const int xl = max(cx - 1, 0);
  const int xr = min(cx + 1, w - 1);
  const int yt = max(cy - 1, 0);
  const int yb = min(cy + 1, h - 1);
  const float zl = SsrHizAt(level, xl, cy);
  const float zr = SsrHizAt(level, xr, cy);
  const float zt = SsrHizAt(level, cx, yt);
  const float zb = SsrHizAt(level, cx, yb);
  const float fw = float(w);
  const float fh = float(h);
  const float3 pl = SsrViewPos(float(xl) + 0.5f, float(cy) + 0.5f, fw, fh, zl, tan_x, tan_y);
  const float3 pr = SsrViewPos(float(xr) + 0.5f, float(cy) + 0.5f, fw, fh, zr, tan_x, tan_y);
  const float3 pt = SsrViewPos(float(cx) + 0.5f, float(yt) + 0.5f, fw, fh, zt, tan_x, tan_y);
  const float3 pb = SsrViewPos(float(cx) + 0.5f, float(yb) + 0.5f, fw, fh, zb, tan_x, tan_y);
  const float3 hh = abs(zl - zc) < abs(zr - zc) ? pc - pl : pr - pc;
  const float3 vv = abs(zt - zc) < abs(zb - zc) ? pt - pc : pc - pb;
  const float3 n = cross(vv, hh);
  const float len = length(n);
  float3 result = len > 1.0e-20f ? n / len : float3(0.0f, 0.0f, -1.0f);
  if (dot(result, pc) > 0.0f) {
    result = -result;
  }
  return result;
}

#endif  // MCLA_GPU_SSR_PASS_HLSLI
