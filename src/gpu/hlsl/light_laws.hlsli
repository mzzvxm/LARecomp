// MCLA GPU renderer -- the laws of a light in HLSL: THE TWIN of src/gpu/light_laws.h and light_frame.h's GpuLight (perf/LIGHT-UNIFY.md 2.2, 2.3, package U1).
//
// For every pass that reads the frame's lights from the GpuLight buffer (the deferred light pass GB-H1, a shadow or volume consumer that wants the same numbers): the decoding of a
// GpuLight row into a light, and the three laws (kHeadlight, kGameLamp, kOmni) as one function, LlLightScale. src/gpu/light_frame_test.cpp compiles THIS FILE as C++ (through a
// small shim of the HLSL vector types, the way gbuffer_rules_test.cpp does gbuffer_common.hlsli), checks every LL_ define against light_frame.h / light_laws.h and every function against
// its C++ twin on random lights and surfaces, so the two cannot drift. That is why the text keeps to a subset both languages read: scalar and vector arithmetic, .x .y .z .w, no
// swizzles of more than one component, no intrinsics but abs, sqrt, saturate, dot, min, max, no `static const`.
//
// THE BUFFER (GpuLightBuffer): a 16 byte header (uint count, frame, generation, reserved) then kMaxLights rows of 64 bytes (LL_BUFFER_HEADER_BYTES, LL_LIGHT_BYTES), the lights in SLOT
// order (the frame's `order[role]` lists are on the CPU: a pass that wants the best K takes the slots from there, or reads them all and tests the role bit). A row past `count` is
// zero: a light of no range. The rows are read as a StructuredBuffer<LlGpuLight> bound from `GpuLightHandles::lights_offset + LL_BUFFER_HEADER_BYTES` (element i is the light of
// slot i), the header's `count` through a ByteAddressBuffer at `lights_offset` (a pass that loops over all LL_MAX_LIGHTS rows does not need it). CHECK in LU-2: that the bindless
// view of the host's upload ring accepts a first element 16 bytes in (else the manager writes the header into a second small buffer).
//
// THE TEXT must not use the names of the game's registers (r0..r31, c0..c255, p0, ps, a0, aL): every name here starts with Ll or LL_.

#ifndef LARECOMP_LIGHT_LAWS_HLSLI
#define LARECOMP_LIGHT_LAWS_HLSLI

// ---- the numbers (light_frame.h, light_laws.h) -------------------------------------------------------------------------------------------

#define LL_MAX_LIGHTS 64
#define LL_BUFFER_HEADER_BYTES 16
#define LL_LIGHT_BYTES 64

#define LL_KIND_SPOT 0
#define LL_KIND_POINT 1

#define LL_SOURCE_HEADLIGHT 0
#define LL_SOURCE_LAMP 1
#define LL_SOURCE_NEON 2
#define LL_SOURCE_POLICE 3
#define LL_SOURCE_MOD 4
#define LL_SOURCE_OTHER 5

#define LL_LAW_HEADLIGHT 0
#define LL_LAW_GAME_LAMP 1
#define LL_LAW_OMNI 2

#define LL_SHADOW_NONE 0
#define LL_SHADOW_ATLAS 1
#define LL_SHADOW_RT_MASK 2

#define LL_ROLE_SURFACE 1
#define LL_ROLE_VOLUME 2
#define LL_ROLE_SHADOW 4
#define LL_ROLE_RT 8

#define LL_FLAG_PLAYER 1
#define LL_FLAG_LIGHTS_GEOMETRY 2
#define LL_FLAG_GAME_LIT 4

// packed0: kind | source | law | role mask | zero | flags;  packed1: first_view | view_count | shadow kind | mask channel
#define LL_KIND_SHIFT 0
#define LL_KIND_MASK 3
#define LL_SOURCE_SHIFT 2
#define LL_SOURCE_MASK 7
#define LL_LAW_SHIFT 5
#define LL_LAW_MASK 3
#define LL_ROLES_SHIFT 7
#define LL_ROLES_MASK 15
#define LL_FLAGS_SHIFT 16
#define LL_FLAGS_MASK 65535
#define LL_FIRST_VIEW_SHIFT 0
#define LL_VIEW_COUNT_SHIFT 8
#define LL_SHADOW_KIND_SHIFT 16
#define LL_MASK_CHANNEL_SHIFT 24

// the headlight's law (light_laws.h kHeadlight*)
#define LL_HEADLIGHT_PEAK 2.0f
#define LL_HEADLIGHT_FALLOFF_DISTANCE 26.0f
#define LL_SURFACE_WRAP 0.45f
#define LL_CUT_LOW 0.10f
#define LL_CUT_HIGH 0.38f
#define LL_NEAR_START 0.8f
#define LL_NEAR_SPAN 1.5f

// ---- the light ----------------------------------------------------------------------------------------------------------------------------

// One row of the buffer as the GPU reads it (GpuLight: four float4; the last has two uints and two floats).
struct LlGpuLight {
  float4 row0;  // pos.xyz, range
  float4 row1;  // dir.xyz, cos_outer
  float4 row2;  // color.xyz, cos_inner
  uint packed0;
  float law_a;
  uint packed1;
  float shadow_weight;
};

struct LlLight {
  float3 pos;
  float range;
  float3 dir;
  float cos_outer;
  float3 color;
  float cos_inner;
  float law_a;
  float shadow_weight;
  uint kind;
  uint source;
  uint law;
  uint roles;
  uint flags;
  uint first_view;
  uint view_count;
  uint shadow_kind;
  uint mask_channel;
};

LlLight LlUnpack(LlGpuLight g) {
  LlLight l;
  l.pos = float3(g.row0.x, g.row0.y, g.row0.z);
  l.range = g.row0.w;
  l.dir = float3(g.row1.x, g.row1.y, g.row1.z);
  l.cos_outer = g.row1.w;
  l.color = float3(g.row2.x, g.row2.y, g.row2.z);
  l.cos_inner = g.row2.w;
  l.law_a = g.law_a;
  l.shadow_weight = g.shadow_weight;
  l.kind = (g.packed0 >> LL_KIND_SHIFT) & LL_KIND_MASK;
  l.source = (g.packed0 >> LL_SOURCE_SHIFT) & LL_SOURCE_MASK;
  l.law = (g.packed0 >> LL_LAW_SHIFT) & LL_LAW_MASK;
  l.roles = (g.packed0 >> LL_ROLES_SHIFT) & LL_ROLES_MASK;
  l.flags = (g.packed0 >> LL_FLAGS_SHIFT) & LL_FLAGS_MASK;
  l.first_view = (g.packed1 >> LL_FIRST_VIEW_SHIFT) & 255;
  l.view_count = (g.packed1 >> LL_VIEW_COUNT_SHIFT) & 255;
  l.shadow_kind = (g.packed1 >> LL_SHADOW_KIND_SHIFT) & 255;
  l.mask_channel = (g.packed1 >> LL_MASK_CHANNEL_SHIFT) & 255;
  return l;
}

// Whether the light may take a role (a LL_ROLE_* bit), and whether the game's own shaders already light with it.
bool LlHasRole(LlLight l, uint role_bit) {
  return (l.roles & role_bit) != 0;
}
bool LlIsGameLit(LlLight l) {
  return (l.flags & LL_FLAG_GAME_LIT) != 0;
}

// ---- the laws -----------------------------------------------------------------------------------------------------------------------------

// HLSL's smoothstep, written out (the C++ twin has no intrinsic).
float LlSmoothStep(float a, float b, float x) {
  float t = saturate((x - a) / (b - a));
  return t * t * (3.0f - 2.0f * t);
}

// kHeadlight: the scalar part of the term at a surface (position `sp`, unit normal `sn` facing the viewer), without the colour: light_laws.h HeadlightScale.
float LlHeadlightScale(LlLight l, float3 sp, float3 sn, float visibility) {
  float3 to_light = l.pos - sp;
  float dist2 = dot(to_light, to_light);
  float range2 = l.range * l.range;
  if (dist2 >= range2 || dist2 < 1.0e-6f) {
    return 0.0f;
  }
  float inverse = 1.0f / sqrt(dist2);
  float3 lv = to_light * inverse;  // from the surface to the lamp
  float cosine = -dot(lv, l.dir);  // the angle from the beam's axis
  if (cosine <= l.cos_outer) {
    return 0.0f;
  }
  float cone = LlSmoothStep(l.cos_outer, l.cos_inner, cosine);
  float dist = dist2 * inverse;
  float d_fall = l.law_a > 0.0f ? l.law_a : LL_HEADLIGHT_FALLOFF_DISTANCE;
  float falloff = 1.0f / (1.0f + dist2 * (1.0f / (d_fall * d_fall)));
  float window = saturate(1.0f - dist2 / range2);
  window = window * window;
  float surface_term = saturate(LL_SURFACE_WRAP + (1.0f - LL_SURFACE_WRAP) * dot(sn, lv));
  float cutoff = 1.0f - LlSmoothStep(LL_CUT_LOW, LL_CUT_HIGH, -lv.y);
  float near_fade = saturate((dist - LL_NEAR_START) * (1.0f / LL_NEAR_SPAN));
  return LL_HEADLIGHT_PEAK * cone * falloff * window * surface_term * cutoff * near_fade * visibility;
}

// kGameLamp and kOmni: the game's term (the plateau law), light_laws.h GameLampScale. law_a is the plateau's radius squared (a lamp's intensity I); an omni light, or a cone of
// cos_outer <= -0.999, has no cone.
float LlGameLampScale(LlLight l, float3 sp, float3 sn, float visibility) {
  float3 to_light = l.pos - sp;
  float dist2 = dot(to_light, to_light);
  float range2 = l.range * l.range;
  if (!(l.law_a > 0.0f) || dist2 >= range2 || dist2 < 1.0e-6f) {
    return 0.0f;
  }
  float inverse = 1.0f / sqrt(dist2);
  float3 lv = to_light * inverse;
  float cone = 1.0f;
  if (l.law == LL_LAW_GAME_LAMP && l.cos_outer > -0.999f) {
    float cosine = -dot(lv, l.dir);
    if (cosine <= l.cos_outer) {
      return 0.0f;
    }
    cone = LlSmoothStep(l.cos_outer, l.cos_inner, cosine);
  }
  float distance_term = dist2 <= l.law_a ? 1.0f / l.law_a : 1.0f / dist2;
  float n_dot_l = saturate(dot(sn, lv));
  return distance_term * cone * n_dot_l * visibility;
}

// The scalar part of one light's term by its law (colour excluded), and the light a surface gets: colour x that.
float LlLightScale(LlLight l, float3 sp, float3 sn, float visibility) {
  if (l.law == LL_LAW_HEADLIGHT) {
    return LlHeadlightScale(l, sp, sn, visibility);
  }
  return LlGameLampScale(l, sp, sn, visibility);
}

float3 LlLightTerm(LlLight l, float3 sp, float3 sn, float visibility) {
  float scale = LlLightScale(l, sp, sn, visibility);
  return l.color * scale;
}

#endif  // LARECOMP_LIGHT_LAWS_HLSLI
