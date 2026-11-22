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
//   the shadow     (GB-H2) a light with views of the shadow atlas (its ShadowRef: GpuLight packed1) is multiplied by the ONE lookup's visibility (hlsl/light_shadow_lookup.hlsli, the lit shaders'
//                  lookup: LSL_MODE_SURFACE, the 4 x 4 footprint read at the receiver's own plane with a contact hardening kernel: HL-SHADOW-QUALITY): the frame's view table follows the lights in
//                  the buffer (LP_SHADOW_OFFSET), the atlas is a bindless depth plane read as one float (set 0), the views of a light are averaged, the layers of a view multiplied, the share
//                  blended by the light's weight. Read only for a pixel the light reaches, after the term is known to be over 0. The plane's slope needs the surface's GEOMETRIC normal, which the
//                  lookup takes from the depth plane's pixel quad (LpShadowNormal, the way the lit shaders take it from the derivatives of the world position): the G-buffer's normal is the
//                  shading one (normal-mapped) and would tilt the plane the wrong way at a grazing angle.
//
// The pixel shader runs once per pixel whatever the host's MSAA level: the planes and the depth are read at the host sample that is the guest's sample 0 and the result is added to every
// sample of the pixel (a one pixel fringe at silhouettes, as the AO's apply). LL_PASS_MS 1 reads Texture2DMS planes (host_pass_common.hlsli space 1), 0 plain textures (space 0).
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
  uint4 info;   // x the light's index in the buffer, y reserved, z the host sample that is the guest's sample 0, w reserved
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<LocalLightPush> g_push;
#else
ConstantBuffer<LocalLightPush> g_push : register(b0, space5);
#endif

ByteAddressBuffer g_lights : register(t0, space4);

// The atlas's size and the table's layers, read once from the table's header (main) and held for the seams below: a pixel that is lit by a shadowed light takes 16 taps of the atlas for each
// view (the 4 x 4 footprint), and asking the texture for its size each time would cost as much again.
static uint LpAtlasW = 0u;
static uint LpAtlasH = 0u;
static uint LpAtlasLayers = 0u;
static uint LpKernelWord = 0u;

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
uint LslKernelWord() {
  return LpKernelWord;
}

// The stored depth at a pixel of the main view's depth plane, at the guest's sample 0 (0 outside the plane: Load returns zeros there, like the sky).
float LpStoredAt(int2 p) {
#if LL_PASS_MS
  return g_textures_ms[g_push.slots.x].Load(p, g_push.info.z).r;
#else
  return g_textures[g_push.slots.x].Load(int3(p, 0)).r;
#endif
}

// The world position of a pixel of the depth plane; false for the sky and for a pixel outside the plane.
bool LpWorldAt(LpGlobals g, int2 p, out float3 world) {
  world = float3(0.0f, 0.0f, 0.0f);
  const float stored = LpStoredAt(p);
  if (!(stored > 0.0f)) {
    return false;
  }
  const float t = LpLinearDepth(stored, g.lin);
  if (!(t < LP_SKY)) {
    return false;
  }
  world = LpWorldPosition(g, float(p.x) + 0.5f, float(p.y) + 0.5f, t);
  return true;
}

// The surface's GEOMETRIC unit normal at pixel p, turned to the camera: the lit shaders' rule (headlight.cpp HeadlightLight: cross(ddy(world), ddx(world)) of the pixel's 2 x 2 quad) taken
// from the depth plane: the quad's top left pixel, its right and its lower neighbour. `fallback` (the G-buffer's normal) where a quad pixel is the sky or the plane's edge, or the quad is
// degenerate. (local_lights_rules.h QuadNormal is the CPU twin.)
float3 LpShadowNormal(LpGlobals g, int2 p, float3 world, float3 fallback) {
  const int2 q = int2(p.x & ~1, p.y & ~1);
  float3 w00;
  float3 w10;
  float3 w01;
  if (!LpWorldAt(g, q, w00) || !LpWorldAt(g, q + int2(1, 0), w10) || !LpWorldAt(g, q + int2(0, 1), w01)) {
    return fallback;
  }
  float3 n = cross(w01 - w00, w10 - w00);
  const float len2 = dot(n, n);
  if (!(len2 > 1.0e-14f)) {
    return fallback;
  }
  n = n * rsqrt(len2);
  if (dot(n, g.position - world) < 0.0f) {
    n = -n;
  }
  return n;
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
  float3 add = LpPixelLight(l, g, world, normal, albedo, rt2.w);
  if (!any(add > 0.0f)) {
    discard;
  }

  // The shadow: only for a light that has views of the atlas and a pixel it lights. The table's header says which bindless depth plane the views are in.
  if ((flags & uint(LP_FLAG_SHADOW)) != 0u && l.shadow_kind == uint(LL_SHADOW_ATLAS) && l.view_count != 0u) {
    const uint4 head = g_lights.Load4(LP_SHADOW_OFF_COUNT);  // count, slot, width, height
    const uint atlas_slot = head.y;
    if (atlas_slot != 0xFFFFFFFFu && head.z != 0u && head.w != 0u) {
      LpAtlasW = head.z;
      LpAtlasH = head.w;
      LpAtlasLayers = g_lights.Load(LP_SHADOW_OFF_LAYERS);
      LpKernelWord = g_lights.Load(LP_SHADOW_OFF_KERNEL);
      const float3 shadow_normal = LpShadowNormal(g, p, world, normal);
      add *= LslLightLit(l.shadow_kind, l.first_view, l.view_count, l.shadow_weight, world.x, world.y, world.z, shadow_normal.x, shadow_normal.y, shadow_normal.z, atlas_slot, uint(LSL_MODE_SURFACE));
    }
  }
  if (!any(add > 0.0f)) {
    discard;
  }
  return float4(add, 0.0f);
}
