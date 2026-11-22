// MCLA GPU renderer -- the deferred local light pass (local_lights.h, perf/LIGHT-UNIFY.md 6.2, package GB-H1).
//
// Drawn once per light into the main view's colour target at the render size, scissored to the light's reach, in a pipeline whose blend is ADD (ONE, ONE) on the colour and writes no alpha:
// the pixel adds  albedo x lit weight x LightTerm(light, surface)  (hlsl/local_light.hlsli LpPixelLight) for every pixel the G-buffer holds a normal and an albedo for. The pass runs at the
// mid-scene depth copy after the ambient occlusion's apply (scene.cpp OnResolve), so the occlusion does not darken the light and the bounce light does not see it.
//
// GB-NEON-COST: a draw may carry up to LP_MAX_DRAW_LIGHTS consecutive lights (push.info.y: a car's neon tube, whose lights share one car, the scissor and the pixels: local_lights_rules.h
// PlanDraws): the pixel reads the planes and the depth once and adds the lights' terms (a light that adds nothing is left out of the sum, as its own draw would have been discarded), and the
// depth and the reach come FIRST: a pixel whose world position no light of the draw reaches (the sky, the far houses: most of a picture a close light's scissor covers) is discarded before the
// three planes are read (push.info.w bit LP_PUSH_LATE_REACH puts the old order back, the A/B). The laws are 0 at and beyond the range, so the picture does not change: local_lights_test.cpp.
//
//   the surface    RT1 the world normal (octahedral: GbRt1Decode), RT2 sqrt(albedo) and the lit weight, RT3 the smoothness and the class where this frame has it; a pixel with no normal, no
//                  albedo (RT2 all zero: a family that writes none yet), a lit weight of 0 (an emissive surface) or, with RT3, the class none or emissive takes nothing.
//   the position   the main depth plane (the stored depth, 1 near, 0 far) and the camera in the buffer's header: view distance by the projection's rows (as the AO's apply), then the world
//                  position by the camera's axes. The camera is the frame's own where the vertex constants hold it, else the last frame's (local_lights.cpp).
//   the light      the GpuLight rows of the draw's lights (push.info.x the first, info.y how many) in the buffer (g_lights): hlsl/light_laws.hlsli decodes them and holds the three laws.
//   the shadow     (GB-H2) a light with views of the shadow atlas (its ShadowRef: GpuLight packed1) is multiplied by the ONE lookup's visibility (hlsl/light_shadow_lookup.hlsli, the lit shaders' box
//                  kernel: LSL_MODE_SURFACE): the frame's view table follows the lights in the buffer (LP_SHADOW_OFFSET), the atlas is a bindless depth plane read as one float (set 0), the
//                  views of a light are averaged, the layers of a view multiplied, the share blended by the light's weight. Read only for a pixel the light reaches, after the term is known to be over 0.
//
// The pixel shader runs once per pixel whatever the host's MSAA level: the planes and the depth are read at the host sample that is the guest's sample 0 and the result is added to every
// sample of the pixel (a one pixel fringe at silhouettes, as the AO's apply). LL_PASS_MS 1 reads Texture2DMS planes (host_pass_common.hlsli space 1), 0 plain textures (space 0).
//
// GB-NEON-BLEED: a neon light (LL_SOURCE_NEON: the underglow, no shadow views) on a VEHICLE pixel (RT3's vehicle flag) is multiplied by hlsl/local_light.hlsli LpContactVisibility: 5 reads of the
// main depth along the first 0.8 m of the pixel's way to the light (a vent's inner wall, a bumper lip, the cabin and the wheel's dish face the light through the car's own panel: N.L cannot tell).
// It runs inside LpOneLight, for each light of a draw, AFTER the law's term is known to be over 0 and BEFORE the shadow's lookup: a pixel no light of the draw reaches was discarded by the reach test
// before the planes (GB-NEON-COST above), and a light that adds nothing at a pixel is never asked, so the 5 reads are spent only on a vehicle pixel a neon light really lights.
//
// Binding (local_lights.cpp): the bindless texture set at spaces 0 to 3 as every host pass, the lights' ByteAddressBuffer at (t0, space4), the push block at (b0, space5).

#include "host_pass_common.hlsli"
#include "gbuffer_common.hlsli"
#include "local_light.hlsli"
#include "light_shadow_lookup.hlsli"

#ifndef LL_PASS_MS
#define LL_PASS_MS 0
#endif

struct LocalLightPush {
  uint4 slots;  // x the main depth plane's bindless slot, y RT1's, z RT2's, w RT3's (0xFFFFFFFF: not there)
  uint4 info;   // x the first light's index in the buffer, y the number of lights of the draw (0 is 1; at most LP_MAX_DRAW_LIGHTS), z the host sample that is the guest's sample 0, w flags (LP_PUSH_*)
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<LocalLightPush> g_push;
#else
ConstantBuffer<LocalLightPush> g_push : register(b0, space5);
#endif

ByteAddressBuffer g_lights : register(t0, space4);

// The atlas's size and the table's layers, read once from the table's header (main) and held for the seams below: a pixel that is lit by a shadowed light takes 4 to 16 taps of the atlas, and
// asking the texture for its size each time would cost as much again.
static uint LpAtlasW = 0u;
static uint LpAtlasH = 0u;
static uint LpAtlasLayers = 0u;

float4 LoadPlane(uint slot, int2 p) {
#if LL_PASS_MS
  return g_textures_ms[slot].Load(p, g_push.info.z);
#else
  return g_textures[slot].Load(int3(p, 0));
#endif
}

// The seam of hlsl/local_light.hlsli LpContactVisibility (GB-NEON-BLEED): the stored depth of the main depth plane at a pixel, at the guest's sample 0 like the pixel's own. The caller has kept the
// pixel inside the target.
float LpStoredDepth(int px, int py) {
#if LL_PASS_MS
  return g_textures_ms[g_push.slots.x].Load(int2(px, py), g_push.info.z).r;
#else
  return g_textures[g_push.slots.x].Load(int3(px, py, 0)).r;
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

// The seams of hlsl/light_shadow_lookup.hlsli: the atlas is the heap's depth plane at `slot` (set 0, whatever LL_PASS_MS says: it is no multisampled plane), a tap is Load with the index clamped
// into the atlas (the lit shaders' sampler clamps at its edge), the views are the table's rows.
float LslTexel(uint slot, int ix, int iy) {
  return g_textures[slot].Load(int3(clamp(ix, 0, int(LpAtlasW) - 1), clamp(iy, 0, int(LpAtlasH) - 1), 0)).x;
}
uint LslAtlasWidth(uint slot) {
  return LpAtlasW;
}
uint LslAtlasHeight(uint slot) {
  return LpAtlasH;
}
LslView LslLoadView(uint entry) {
  uint at = uint(LP_SHADOW_VIEWS_OFFSET) + entry * uint(LP_SHADOW_VIEW_BYTES);
  LslView v;
  v.m0 = asfloat(g_lights.Load4(at));
  v.m1 = asfloat(g_lights.Load4(at + 16u));
  v.m2 = asfloat(g_lights.Load4(at + 32u));
  v.m3 = asfloat(g_lights.Load4(at + 48u));
  v.rect = asfloat(g_lights.Load4(at + 64u));
  uint4 w = g_lights.Load4(at + 80u);
  v.bias = asfloat(w.x);
  v.texel = asfloat(w.y);
  v.light_index = w.z;
  v.valid = w.w;
  return v;
}
uint LslLayerOf(uint entry) {
  return LslLayerBits(LpAtlasLayers, entry);
}

// The stored depth of the main view's depth plane at the guest's sample 0, and from it the pixel's world position. False for the sky (stored depth 0, or a distance the linearisation cannot
// place): such a pixel takes nothing.
bool LpWorldOf(int2 p, LpGlobals g, out float3 world) {
  world = float3(0.0f, 0.0f, 0.0f);
#if LL_PASS_MS
  const float stored = g_textures_ms[g_push.slots.x].Load(p, g_push.info.z).r;
#else
  const float stored = g_textures[g_push.slots.x].Load(int3(p, 0)).r;
#endif
  if (!(stored > 0.0f)) {
    return false;  // the sky
  }
  const float t = LpLinearDepth(stored, g.lin);
  if (!(t < LP_SKY)) {
    return false;
  }
  world = LpWorldPosition(g, float(p.x) + 0.5f, float(p.y) + 0.5f, t);
  return true;
}

// Whether any of the `count` lights from row `first` reaches `world` (hlsl/local_light.hlsli LpReaches): the rows' first float4, the place and the range.
bool LpAnyReaches(uint first, uint count, float3 world) {
  for (uint i = 0u; i < count; ++i) {
    const float4 row0 = asfloat(g_lights.Load4(LP_LIGHTS_OFFSET + (first + i) * LL_LIGHT_BYTES));
    if (LpReaches(float3(row0.x, row0.y, row0.z), row0.w, world)) {
      return true;
    }
  }
  return false;
}

// What one light adds at a pixel: the term, then (GB-NEON-BLEED) for a neon light on a vehicle pixel the contact term, then, for a light with views of the atlas and a pixel it lights, the shadow's
// visibility. The w is 1 when the light adds anything (a light that does not is not in the sum: its own draw would have discarded the pixel). The table's header says which bindless depth plane the
// views are in.
float4 LpOneLight(uint index, LpGlobals g, uint flags, bool vehicle, float3 world, float3 normal, float3 albedo, float lit) {
  const LlLight l = LlUnpack(LpLoadLight(index));
  float3 add = LpPixelLight(l, g, world, normal, albedo, lit);
  if (!any(add > 0.0f)) {
    return float4(0.0f, 0.0f, 0.0f, 0.0f);
  }
  // GB-NEON-BLEED: a neon light on a vehicle pixel is shut out by the car's own panels (local_light.hlsli LpContactVisibility): the vent, the lip and the cabin it faces through the body get nothing.
  if (LpTakesContact(l, vehicle)) {
    add *= LpContactVisibility(g, world, normal, l.pos);
    if (!any(add > 0.0f)) {
      return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }
  }
  if ((flags & uint(LP_FLAG_SHADOW)) != 0u && l.shadow_kind == uint(LL_SHADOW_ATLAS) && l.view_count != 0u) {
    const uint4 head = g_lights.Load4(LP_SHADOW_OFF_COUNT);  // count, slot, width, height
    const uint atlas_slot = head.y;
    if (atlas_slot != 0xFFFFFFFFu && head.z != 0u && head.w != 0u) {
      LpAtlasW = head.z;
      LpAtlasH = head.w;
      LpAtlasLayers = g_lights.Load(LP_SHADOW_OFF_LAYERS);
      add *= LslLightLit(l.shadow_kind, l.first_view, l.view_count, l.shadow_weight, world.x, world.y, world.z, atlas_slot, uint(LSL_MODE_SURFACE));
    }
  }
  if (!any(add > 0.0f)) {
    return float4(0.0f, 0.0f, 0.0f, 0.0f);
  }
  return float4(add, 1.0f);
}

float4 main(float4 position : SV_Position) : SV_Target {
  const int2 p = int2(position.xy);
  const uint first = g_push.info.x;
  const uint count = clamp(g_push.info.y, 1u, uint(LP_MAX_DRAW_LIGHTS));
  const LpGlobals g = LpLoadGlobals();

  // The position and the reach first (GB-NEON-COST): the depth is one read, and a pixel no light of the draw reaches takes nothing whatever the planes hold (the laws are 0 at and beyond the
  // range), so the three planes are left unread for it. LP_PUSH_LATE_REACH is the old order: planes, then the position, the reach only inside the laws.
  const bool reach_first = (g_push.info.w & uint(LP_PUSH_LATE_REACH)) == 0u;
  float3 world = float3(0.0f, 0.0f, 0.0f);
  if (reach_first) {
    if (!LpWorldOf(p, g, world)) {
      discard;
    }
    if (!LpAnyReaches(first, count, world)) {
      discard;
    }
  }

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
  bool vehicle = false;  // GB-NEON-BLEED: RT3's vehicle flag (a frame without RT3 says no)
  if ((flags & LP_FLAG_MATERIAL) != 0u && g_push.slots.w != 0xFFFFFFFFu) {
    const float2 rt3 = LoadPlane(g_push.slots.w, p).xy;
    const uint surface_class = GbRt3Class(rt3);
    if (surface_class == uint(LP_CLASS_NONE) || surface_class == uint(LP_CLASS_EMISSIVE)) {
      discard;
    }
    vehicle = (GbRt3Flags(rt3) & uint(GB_FLAG_VEHICLE)) != 0u;
  }

  // The position, in the old order: after the planes.
  if (!reach_first) {
    if (!LpWorldOf(p, g, world)) {
      discard;
    }
  }

  // The lights: each adds its term (and its shadow), the sum goes to the colour. No light adds anything: the pixel is discarded.
  const float3 normal = GbRt1Decode(rt1);
  const float3 albedo = GbAlbedoDecode(rt2.xyz);
  float3 total = float3(0.0f, 0.0f, 0.0f);
  bool any_light = false;
  for (uint i = 0u; i < count; ++i) {
    const float4 one = LpOneLight(first + i, g, flags, vehicle, world, normal, albedo, rt2.w);
    if (one.w > 0.0f) {
      total += one.xyz;
      any_light = true;
    }
  }
  if (!any_light) {
    discard;
  }
  return float4(total, 0.0f);
}
