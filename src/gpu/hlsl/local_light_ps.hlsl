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
//   the shadow     (GB-H2) a light with views of the shadow atlas (its ShadowRef: GpuLight packed1) is multiplied by the ONE lookup's visibility (hlsl/light_shadow_lookup.hlsli, the lit shaders'
//                  lookup: LSL_MODE_SURFACE, the 4 x 4 footprint read at the receiver's own plane with a contact hardening kernel: HL-SHADOW-QUALITY): the frame's view table follows the lights in
//                  the buffer (LP_SHADOW_OFFSET), the atlas is a bindless depth plane read as one float (set 0), the views of a light are averaged, the layers of a view multiplied, the share
//                  blended by the light's weight. Read only for a pixel the light reaches, after the term is known to be over 0. The plane's slope needs the surface's GEOMETRIC normal, which the
//                  lookup takes from the depth plane's pixel quad (LpShadowNormal, the way the lit shaders take it from the derivatives of the world position): the G-buffer's normal is the
//                  shading one (normal-mapped) and would tilt the plane the wrong way at a grazing angle.
//
//   the ray traced visibility (RT-LIGHTS-D1, hlsl/rt_mask_read.hlsli): a light with the flag LL_FLAG_RT_VISIBILITY (and the table in the buffer: LP_FLAG_RT_MASK) reads its channel of the filtered images the ray
//                  pass wrote just before (at the mask's size: the occlusion's traced size over the table's step; a depth guided bilinear unless the mask is the picture's size) and multiplies it with the atlas's visibility (the product: two partial caster sets);
//                  a light the game lights is only ever TAKEN AWAY from (-term x (1 - V)), so with V = 1 the pixel is the game's. A lamp with a channel and no atlas views is looked at too.
//                  The table's view flag draws the visibility as a picture instead (mcla_gpu_rt_lights_view).
//
//   the street lamps (LAMP-SHADOWS-4, hlsl/lamp_shadow_lookup.hlsli): a light whose source is the street lamp names its entries in the COMBINED entry space (8 + the row of the lamps' table, which follows
//                  the headlights' in the buffer: LP_LAMP_*); the same one lookup runs over it with the edge fade, and a lamp the game lights has its shadowed share taken away (a negative colour: the blend is
//                  ADD), a lamp the pass owns is multiplied by the visibility. LP_OFF_DEBUG draws the shadow's own picture instead (mcla_gpu_lamp_shadows_view).
//
// The pixel shader runs once per pixel whatever the host's MSAA level: the planes and the depth are read at the host sample that is the guest's sample 0 and the result is added to every
// sample of the pixel (a one pixel fringe at silhouettes, as the AO's apply). LL_PASS_MS 1 reads Texture2DMS planes (host_pass_common.hlsli space 1), 0 plain textures (space 0).
//
// GB-NEON-BLEED: a neon light (LL_SOURCE_NEON: the underglow, no shadow views) on a VEHICLE pixel (RT3's vehicle flag) is multiplied by hlsl/local_light.hlsli LpContactVisibility: 5 reads of the
// main depth along the first 0.8 m of the pixel's way to the light (a vent's inner wall, a bumper lip, the cabin and the wheel's dish face the light through the car's own panel: N.L cannot tell).
// It runs inside LpOneLight, for each light of a draw, AFTER the law's term is known to be over 0 and BEFORE the shadow's lookup: a pixel no light of the draw reaches was discarded by the reach test
// before the planes (GB-NEON-COST above), and a light that adds nothing at a pixel is never asked, so the 5 reads are spent only on a vehicle pixel a neon light really lights.
//
// GB-NEON-CONTACT: the contact term has an A/B (push.info.w bit LP_PUSH_NO_CONTACT: mcla_gpu_neon_contact off) and a cap on its cost: a tube of three or four lights is one draw, and marching for each
// light was up to 20 depth reads a vehicle pixel. A pixel now marches for the two strongest lights of the draw only and each other light takes the visibility of the marched one nearest to it along
// the tube (LpCappedLights, hlsl/local_light.hlsli LpContactDonors): up to 10 reads. Draws of one or two lights are as before; LP_PUSH_FULL_CONTACT (the cost A/B) takes the cap off.
//
// Binding (local_lights.cpp): the bindless texture set at spaces 0 to 3 as every host pass, the lights' ByteAddressBuffer at (t0, space4), the push block at (b0, space5).

#include "host_pass_common.hlsli"
#include "gbuffer_common.hlsli"
#include "local_light.hlsli"
// the most entries a light may name: the headlights' table (8) and the street lamps' (20), one combined entry space (hlsl/lamp_shadow_lookup.hlsli)
#define LSL_MAX_VIEWS 28
#include "light_shadow_lookup.hlsli"
#include "lamp_shadow_lookup.hlsli"
#include "nan_guard.hlsli"
#include "rt_mask_read.hlsli"

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

// The atlas's size and the table's layers, read once from the table's header (main) and held for the seams below: a pixel that is lit by a shadowed light takes 16 taps of the atlas for each
// view (the 4 x 4 footprint), and asking the texture for its size each time would cost as much again.
static uint LpAtlasW = 0u;
static uint LpAtlasH = 0u;
static uint LpAtlasLayers = 0u;
static uint LpLampLayersLo = 0u;  // LAMP-SHADOWS-4: the layers of the lamps' table's entries 0..15 and 16..31
static uint LpLampLayersHi = 0u;
static uint LpKernelWord = 0u;

// The seams of hlsl/rt_mask_read.hlsli: the mask images (at the mask's size) and the occlusion's linear depth image (at the traced size) are heap textures (set 0, whatever LL_PASS_MS says: they are no multisampled planes).
float4 RtmLoadMask(uint slot, int2 p) {
  return g_textures[slot].Load(int3(p, 0));
}
float RtmLoadDepth(uint slot, int2 p) {
  return g_textures[slot].Load(int3(p, 0)).x;
}

RtmTable LpLoadRtTable() {
  const uint4 a = g_lights.Load4(LP_RT_OFFSET);          // images, flags, width, height
  const uint4 b = g_lights.Load4(LP_RT_OFFSET + 16u);    // the filtered images' slots, the linear depth image's slot
  const uint4 c = g_lights.Load4(LP_RT_OFFSET + 32u);    // the raw images' slots, the step (the traced pixels a mask texel spans)
  const uint4 d = g_lights.Load4(LP_RT_OFFSET + 48u);    // the depth guide's sigma, the occlusion's traced size
  RtmTable t;
  t.images = a.x;
  t.flags = a.y;
  t.width = a.z;
  t.height = a.w;
  t.slot0 = b.x;
  t.slot1 = b.y;
  t.slot2 = b.z;
  t.lin_slot = b.w;
  t.raw0 = c.x;
  t.raw1 = c.y;
  t.raw2 = c.z;
  t.step = c.w;
  t.depth_sigma = asfloat(d.x);
  t.traced_w = d.y;
  t.traced_h = d.z;
  return t;
}

float4 LoadPlane(uint slot, int2 p) {
#if LL_PASS_MS
  return g_textures_ms[slot].Load(p, g_push.info.z);
#else
  return g_textures[slot].Load(int3(p, 0));
#endif
}

// The stored depth of the main view's depth plane at the pixel (px, py), at the guest's sample 0 (0 outside the plane: Load returns zeros there, like the sky): the seam of hlsl/local_light.hlsli
// LpContactVisibility (GB-NEON-BLEED: it keeps its index inside the target), and the one read of a pixel's depth for the reach test (LpWorldAt) and the shadow normal's quad (LpShadowNormal).
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
  if (entry >= uint(LSL_LAMP_ENTRY_BASE)) {  // the combined entry space: the street lamps' table follows the headlights' (LAMP-SHADOWS-4)
    at = uint(LP_LAMP_VIEWS_OFFSET) + (entry - uint(LSL_LAMP_ENTRY_BASE)) * uint(LP_SHADOW_VIEW_BYTES);
  }
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
  if (entry >= uint(LSL_LAMP_ENTRY_BASE)) {
    uint e = entry - uint(LSL_LAMP_ENTRY_BASE);
    return LslLayerBits(e < 16u ? LpLampLayersLo : LpLampLayersHi, e & 15u);
  }
  return LslLayerBits(LpAtlasLayers, entry);
}
uint LslKernelWord() {
  return LpKernelWord;
}

// The stored depth of the main view's depth plane at the guest's sample 0, and from it the pixel's world position. False for the sky (stored depth 0, or a distance the linearisation cannot
// place), and for a pixel outside the plane: such a pixel takes nothing.
bool LpWorldAtDepth(LpGlobals g, int2 p, float far_depth, out float3 world) {
  world = float3(0.0f, 0.0f, 0.0f);
  const float stored = LpStoredDepth(p.x, p.y);
  if (!(stored > 0.0f)) {
    return false;  // the sky
  }
  const float t = LpLinearDepth(stored, g.lin);
  if (!(t < LP_SKY) || (far_depth > 0.0f && t > far_depth)) {
    return false;
  }
  world = LpWorldPosition(g, float(p.x) + 0.5f, float(p.y) + 0.5f, t);
  return true;
}

bool LpWorldAt(LpGlobals g, int2 p, out float3 world) {
  return LpWorldAtDepth(g, p, 0.0f, world);
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

// A light's shadows at a pixel, and what is left of its term: for a light with views of the atlas, `add` times the one lookup's visibility (HL-SHADOW-QUALITY: at the surface's geometric normal, taken from the
// depth plane's pixel quad at p); then (RT-LIGHTS-D1) a light with a ray traced channel is multiplied by that visibility too, the product of the two partial ones, and a street lamp's (LAMP-SHADOWS-4) goes
// through the lamps' table. A light the GAME lights (a street lamp: its grid has added the term) is only ever TAKEN AWAY from: the colour is negative (the blend is ADD) and is the shadowed share. The
// w is 1 when the light adds anything and 0 when it does not (it is not in the sum: its own draw would have discarded the pixel); a debug picture (the rays' or the lamps' visibility) is returned with w = 1.
// The table's header says which bindless depth plane the views are in.
float4 LpShadowed(LlLight l, LpGlobals g, uint flags, int2 p, float3 world, float3 normal, float3 add) {
  // The ray traced visibility of this light (RT-LIGHTS-D1): its channel of the filtered images at this pixel; 1 for a light with none.
  const bool rt_ref = (flags & uint(LP_FLAG_RT_MASK)) != 0u && (l.flags & uint(LL_FLAG_RT_VISIBILITY)) != 0u;
  float rt_vis = 1.0f;
  if (rt_ref) {
    const RtmTable rt_table = LpLoadRtTable();
    const float t = LpLinearDepth(LpStoredDepth(p.x, p.y), g.lin);  // the view distance of the pixel (one more read of the depth, only for a light with a channel)
    rt_vis = RtmVisibility(rt_table, l.mask_channel, p, g.size.x, g.size.y, t, (rt_table.flags & uint(RTM_RAW_VIEW)) != 0u);
    if ((rt_table.flags & uint(RTM_VIEW)) != 0u) {
      return float4(float3(1.0f - rt_vis, rt_vis, 0.0f) * (g.exposure * 0.15f), 1.0f);   // the picture of the visibility instead of the light
    }
  }

  // A lamp the GAME lights with no source of visibility at all (no views of the atlas, no ray traced channel, or the channel's table is not in this frame's buffer): the grid has added its term, the pass
  // never does. TrimSelection keeps such a lamp out of the selection; this is the second line (a lamp that relied on a channel when the images did not come).
  if (l.source == uint(LL_SOURCE_LAMP) && (l.flags & uint(LL_FLAG_GAME_LIT)) != 0u && !rt_ref && !(l.shadow_kind == uint(LL_SHADOW_ATLAS) && l.view_count != 0u)) {
    return float4(0.0f, 0.0f, 0.0f, 0.0f);
  }

  // A STREET LAMP's light with views of the lamps' table (LAMP-SHADOWS-4): the one lookup plus the edge fade over the lamps' table, then, for a lamp the game lights (its grid already added the term)
  // the shadowed share is TAKEN AWAY (the blend is ADD: the pixel's colour is negative), for a lamp the pass owns the light is multiplied by the visibility. The pixel is read only after the term
  // is known to be over 0, as the headlights'. A game lamp with no table has nothing to take away.
  if (l.source == uint(LL_SOURCE_LAMP) && ((l.shadow_kind == uint(LL_SHADOW_ATLAS) && l.view_count != 0u) || rt_ref)) {
    const bool game_lit = (l.flags & uint(LL_FLAG_GAME_LIT)) != 0u;
    if (g_lights.Load(LP_OFF_DEBUG) == uint(LP_DEBUG_REMOVE)) {  // the units check: the whole term of a game lamp taken away, nothing added for an owned one
      if (!game_lit) {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
      }
      return float4(-add, 1.0f);
    }
    float lamp_vis = 1.0f;
    if (l.shadow_kind == uint(LL_SHADOW_ATLAS) && l.view_count != 0u) {
      const uint4 lamp_head = g_lights.Load4(LP_LAMP_OFF_COUNT);  // count, slot, width, height
      if ((flags & uint(LP_FLAG_LAMP_SHADOW)) == 0u || lamp_head.y == 0xFFFFFFFFu || lamp_head.z == 0u || lamp_head.w == 0u) {
        if (!rt_ref) {
          if (game_lit) {
            return float4(0.0f, 0.0f, 0.0f, 0.0f);
          }
          return float4(add, 1.0f);
        }
      } else {
        LpAtlasW = lamp_head.z;
        LpAtlasH = lamp_head.w;
        const uint2 lamp_layers = g_lights.Load2(LP_LAMP_OFF_LAYERS);
        LpLampLayersLo = lamp_layers.x;
        LpLampLayersHi = lamp_layers.y;
        LpKernelWord = 0u;  // HSQ-PCSS: the kernel settings are the headlights' (the table header's word); a street lamp keeps the default lookup, whichever light came before it in this pixel's loop
        const float3 lamp_normal = LpShadowNormal(g, p, world, normal);  // the geometric normal of the depth plane's quad, as the headlights' (the plane lookup needs it)
        lamp_vis = LslLampLit(l.shadow_kind, l.first_view, l.view_count, l.shadow_weight, l.pos.x, l.pos.y, l.pos.z, world.x, world.y, world.z, lamp_normal.x, lamp_normal.y, lamp_normal.z, lamp_head.y, uint(LSL_MODE_SURFACE));
        const uint debug = g_lights.Load(LP_OFF_DEBUG);
        if (debug == uint(LP_DEBUG_VISIBILITY)) {
          return float4(float3(1.0f - lamp_vis, lamp_vis, 0.0f) * (g.exposure * 0.15f), 1.0f);
        }
        if (debug == uint(LP_DEBUG_LAMPS)) {
          float f = float(l.first_view);
          float k = g.exposure * (0.05f + 0.25f * (1.0f - lamp_vis));
          return float4(frac(f * 0.37f) * k, frac(f * 0.61f + 0.3f) * k, frac(f * 0.83f + 0.6f) * k, 1.0f);
        }
      }
    }
    if (rt_ref) {
      lamp_vis = RtmCombine(lamp_vis, rt_vis);   // the product of the atlas's visibility and the ray's (each 1 where the lamp has none)
    }
    add = game_lit ? -add * (1.0f - lamp_vis) : add * lamp_vis;
    if (!any(add != 0.0f)) {
      return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }
    return float4(add, 1.0f);
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
  if (rt_ref) {
    // the ray traced visibility multiplies a light we own (a headlight, neon, a police or mod lamp); a light the game lights with no lamp source (never selected: TrimSelection) is taken away from
    add = ((l.flags & uint(LL_FLAG_GAME_LIT)) != 0u) ? -add * (1.0f - RtmCombine(1.0f, rt_vis)) : add * RtmCombine(1.0f, rt_vis);
  }
  if (!any(add != 0.0f)) {
    return float4(0.0f, 0.0f, 0.0f, 0.0f);
  }
  return float4(add, 1.0f);
}

// What one light adds at a pixel p: the term, then (GB-NEON-BLEED) for a neon light on a pixel that takes the contact term (`contact`: a vehicle pixel, the term on) the contact term, then the shadows'
// visibility (LpShadowed: the atlas, the ray traced channel, a street lamp's table). The w is 1 when the light adds anything (a light that does not is not in the sum: its own draw would have discarded the
// pixel); the colour is negative for a street lamp the game lights (its shadowed share taken away).
float4 LpOneLight(uint index, LpGlobals g, uint flags, int2 p, bool contact, float3 world, float3 normal, float3 albedo, float lit) {
  const LlLight l = LlUnpack(LpLoadLight(index));
  float3 add = NgFinite3(LpPixelLight(l, g, world, normal, albedo, lit));   // added to the scene: a NaN or an infinity is none (nan_guard.hlsli)
  if (!any(add > 0.0f)) {
    return float4(0.0f, 0.0f, 0.0f, 0.0f);
  }
  // GB-NEON-BLEED: a neon light on a vehicle pixel is shut out by the car's own panels (local_light.hlsli LpContactVisibility): the vent, the lip and the cabin it faces through the body get nothing.
  if (LpTakesContact(l, contact)) {
    add *= LpContactVisibility(g, world, normal, l.pos);
    if (!any(add > 0.0f)) {
      return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }
  }
  return LpShadowed(l, g, flags, p, world, normal, add);
}

// GB-NEON-CONTACT: the lights of a draw of three or four (a car's neon tube) at a pixel that takes the contact term, with the term's reads capped: every light's term first, then the depth is marched
// for the two strongest lights only and each other light takes the visibility of the marched one nearest to it along the tube (local_light.hlsli LpContactDonors: up to 10 depth reads a pixel where
// the lights marching for themselves made up to 20). Then, light by light as LpOneLight does: the term times the visibility (a light that is shut out is not in the sum), the shadow. The tube's lights
// are neon lights, which have no views of the atlas; the shadow step is kept so that the function is right for any light of a draw. Never called with a draw of one or two lights (those march for
// themselves, as before the cap) or with LP_PUSH_FULL_CONTACT.
float4 LpCappedLights(uint first, uint count, LpGlobals g, uint flags, int2 p, float3 world, float3 normal, float3 albedo, float lit) {
  LlLight l[4];
  float3 add[4];
  float strength[4];
  for (uint i = 0u; i < 4u; ++i) {
    add[i] = float3(0.0f, 0.0f, 0.0f);
    strength[i] = 0.0f;
    l[i] = LlUnpack(LpLoadLight(first + min(i, count - 1u)));
    if (i < count) {
      add[i] = NgFinite3(LpPixelLight(l[i], g, world, normal, albedo, lit));   // (nan_guard.hlsli, as LpOneLight)
      if (any(add[i] > 0.0f) && LpTakesContact(l[i], true)) {
        strength[i] = add[i].x + add[i].y + add[i].z;
      }
    }
  }
  const uint donors = LpContactDonors(strength[0], strength[1], strength[2], strength[3], count);
  float visibility[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  for (uint m = 0u; m < 4u; ++m) {
    if (((donors >> (4u * m)) & 7u) == (m | 4u)) {  // a marched light: its own donor
      visibility[m] = LpContactVisibility(g, world, normal, l[m].pos);
    }
  }
  for (uint k = 0u; k < 4u; ++k) {
    if (((donors >> (4u * k)) & 4u) != 0u) {
      visibility[k] = visibility[(donors >> (4u * k)) & 3u];
    }
  }
  float3 total = float3(0.0f, 0.0f, 0.0f);
  bool any_light = false;
  for (uint j = 0u; j < 4u; ++j) {
    if (j < count && any(add[j] > 0.0f)) {
      float3 one = add[j] * visibility[j];
      if (any(one > 0.0f)) {
        const float4 shadowed = LpShadowed(l[j], g, flags, p, world, normal, one);
        if (shadowed.w > 0.0f) {
          total += shadowed.xyz;
          any_light = true;
        }
      }
    }
  }
  return float4(total, any_light ? 1.0f : 0.0f);
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
    const float far_depth = float(g_push.info.w >> LP_PUSH_FAR_DEPTH_SHIFT) * (1.0f / 16.0f);
    if (!LpWorldAtDepth(g, p, far_depth, world)) {
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
    if (!LpWorldAt(g, p, world)) {
      discard;
    }
  }

  // The lights: each adds its term (and its shadow), the sum goes to the colour. No light adds anything: the pixel is discarded. GB-NEON-CONTACT: a vehicle pixel takes the contact term unless
  // LP_PUSH_NO_CONTACT (mcla_gpu_neon_contact off) says no, and with three or four lights in the draw (a tube) it marches for the strongest two only unless LP_PUSH_FULL_CONTACT says every light does.
  const float3 normal = GbRt1Decode(rt1);
  const float3 albedo = GbAlbedoDecode(rt2.xyz);
  const bool contact = vehicle && (g_push.info.w & uint(LP_PUSH_NO_CONTACT)) == 0u;
  const bool capped = contact && count > uint(LP_CONTACT_MARCHES) && (g_push.info.w & uint(LP_PUSH_FULL_CONTACT)) == 0u;
  float3 total = float3(0.0f, 0.0f, 0.0f);
  bool any_light = false;
  if (capped) {
    const float4 tube = LpCappedLights(first, count, g, flags, p, world, normal, albedo, rt2.w);
    total = tube.xyz;
    any_light = tube.w > 0.0f;
  } else {
    for (uint i = 0u; i < count; ++i) {
      const float4 one = LpOneLight(first + i, g, flags, p, contact, world, normal, albedo, rt2.w);
      if (one.w > 0.0f) {
        total += one.xyz;
        any_light = true;
      }
    }
  }
  if (!any_light) {
    discard;
  }
  return float4(total, 0.0f);
}
