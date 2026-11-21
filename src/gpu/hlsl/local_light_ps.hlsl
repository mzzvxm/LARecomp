// MCLA GPU renderer -- the deferred local light pass (local_lights.h, perf/LIGHT-UNIFY.md 6.2, package GB-H1).
//
// Drawn once per light into the main view's colour target at the render size, scissored to the light's reach, in a pipeline whose blend is ADD (ONE, ONE) on the colour and writes no alpha:
// the pixel adds  albedo x lit weight x LightTerm(light, surface)  (hlsl/local_light.hlsli LpPixelLight) for every pixel the G-buffer holds a normal and an albedo for. The pass runs at the
// mid-scene depth copy after the ambient occlusion's apply (scene.cpp OnResolve), so the occlusion does not darken the light and the bounce light does not see it.
//
//   the surface    RT1 the world normal (octahedral: GbRt1Decode), RT2 sqrt(albedo) and the lit weight, RT3 the smoothness and the class where this frame has it; a pixel with no normal, no
//                  albedo (RT2 all zero: a family that writes none yet), a lit weight of 0 (an emissive surface) or, with RT3, the class none or emissive takes nothing.
//   the position   the main depth plane (the stored depth, 1 near, 0 far) and the camera in the buffer's header: view distance by the projection's rows (as the AO's apply), then the world
//                  position by the camera's axes. The camera is the frame's own where the vertex constants hold it, else the last frame's (local_lights.cpp).
//   the light      the GpuLight row of the draw's light (push.info.x) in the buffer (g_lights): hlsl/light_laws.hlsli decodes it and holds the three laws.
//
// The pixel shader runs once per pixel whatever the host's MSAA level: the planes and the depth are read at the host sample that is the guest's sample 0 and the result is added to every
// sample of the pixel (a one pixel fringe at silhouettes, as the AO's apply). LL_PASS_MS 1 reads Texture2DMS planes (host_pass_common.hlsli space 1), 0 plain textures (space 0).
//
// Binding (local_lights.cpp): the bindless texture set at spaces 0 to 3 as every host pass, the lights' ByteAddressBuffer at (t0, space4), the push block at (b0, space5).

#include "host_pass_common.hlsli"
#include "gbuffer_common.hlsli"
#include "local_light.hlsli"

#ifndef LL_PASS_MS
#define LL_PASS_MS 0
#endif

struct LocalLightPush {
  uint4 slots;  // x the main depth plane's bindless slot, y RT1's, z RT2's, w RT3's (0xFFFFFFFF: not there)
  uint4 info;   // x the light's index in the buffer, y reserved, z the host sample that is the guest's sample 0, w reserved
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<LocalLightPush> g_push;
#else
ConstantBuffer<LocalLightPush> g_push : register(b0, space5);
#endif

ByteAddressBuffer g_lights : register(t0, space4);

float4 LoadPlane(uint slot, int2 p) {
#if LL_PASS_MS
  return g_textures_ms[slot].Load(p, g_push.info.z);
#else
  return g_textures[slot].Load(int3(p, 0));
#endif
}

LpGlobals LpLoadGlobals() {
  LpGlobals g;
  float4 a = asfloat(g_lights.Load4(LP_OFF_RIGHT));
  float4 b = asfloat(g_lights.Load4(LP_OFF_UP));
  float4 c = asfloat(g_lights.Load4(LP_OFF_FORWARD));
  float4 d = asfloat(g_lights.Load4(LP_OFF_POSITION));
  g.right = float3(a.x, a.y, a.z);
  g.tan_x = a.w;
  g.up = float3(b.x, b.y, b.z);
  g.tan_y = b.w;
  g.forward = float3(c.x, c.y, c.z);
  g.gain = c.w;
  g.position = float3(d.x, d.y, d.z);
  g.night = d.w;
  g.exposure = asfloat(g_lights.Load(LP_OFF_EXPOSURE));
  g.lin = asfloat(g_lights.Load4(LP_OFF_LIN));
  g.size = asfloat(g_lights.Load4(LP_OFF_SIZE));
  g.occ = asfloat(g_lights.Load4(LP_OFF_OCC));
  return g;
}

LlGpuLight LpLoadLight(uint index) {
  uint at = LP_LIGHTS_OFFSET + index * LL_LIGHT_BYTES;
  LlGpuLight g;
  g.row0 = asfloat(g_lights.Load4(at));
  g.row1 = asfloat(g_lights.Load4(at + 16));
  g.row2 = asfloat(g_lights.Load4(at + 32));
  uint4 w = g_lights.Load4(at + 48);
  g.packed0 = w.x;
  g.law_a = asfloat(w.y);
  g.packed1 = w.z;
  g.shadow_weight = asfloat(w.w);
  return g;
}

float4 main(float4 position : SV_Position) : SV_Target {
  const int2 p = int2(position.xy);

  // The surface: no normal, no albedo or no light to take, nothing to add.
  const float2 rt1 = LoadPlane(g_push.slots.y, p).xy;
  if (!GbRt1Valid(rt1)) {
    discard;
  }
  const float4 rt2 = LoadPlane(g_push.slots.z, p);
  if (!any(rt2 > 0.0f) || !(rt2.w > 0.0f)) {
    discard;
  }
  const uint flags = g_lights.Load(LP_OFF_COUNT + 4);
  if ((flags & LP_FLAG_MATERIAL) != 0u && g_push.slots.w != 0xFFFFFFFFu) {
    const uint surface_class = GbRt3Class(LoadPlane(g_push.slots.w, p).xy);
    if (surface_class == uint(LP_CLASS_NONE) || surface_class == uint(LP_CLASS_EMISSIVE)) {
      discard;
    }
  }

  // The position: the stored depth of the main view's depth plane at the guest's sample 0.
#if LL_PASS_MS
  const float stored = g_textures_ms[g_push.slots.x].Load(p, g_push.info.z).r;
#else
  const float stored = g_textures[g_push.slots.x].Load(int3(p, 0)).r;
#endif
  if (!(stored > 0.0f)) {
    discard;  // the sky
  }
  const LpGlobals g = LpLoadGlobals();
  const float t = LpLinearDepth(stored, g.lin);
  if (!(t < LP_SKY)) {
    discard;
  }
  const float3 world = LpWorldPosition(g, float(p.x) + 0.5f, float(p.y) + 0.5f, t);

  // The light.
  const LlLight l = LlUnpack(LpLoadLight(g_push.info.x));
  const float3 normal = GbRt1Decode(rt1);
  const float3 albedo = GbAlbedoDecode(rt2.xyz);
  const float3 add = LpPixelLight(l, g, world, normal, albedo, rt2.w);
  if (!any(add > 0.0f)) {
    discard;
  }
  return float4(add, 0.0f);
}
