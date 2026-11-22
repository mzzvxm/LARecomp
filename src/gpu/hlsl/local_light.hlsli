// MCLA GPU renderer -- the deferred local light pass's per-pixel maths: THE TWIN of src/gpu/local_lights_rules.h (perf/LIGHT-UNIFY.md 6.2, package GB-H1).
//
// hlsl/local_light_ps.hlsl reads the frame's buffer (a 128 byte header, then the GpuLight rows: LP_OFF_*, LP_LIGHTS_OFFSET) and the G-buffer's planes and calls these. Everything that is a
// function of numbers only is here, so src/gpu/local_lights_test.cpp compiles THIS FILE as C++ (through a small shim of the HLSL vector types, the way light_frame_test.cpp does for
// light_laws.hlsli, which this file includes) and compares it with the C++ twins on random cameras, pixels, lights and surfaces. That is why the text keeps to the subset both languages read:
// scalars, float3 / float4, .x .y .z .w, no swizzles of more than one component, no intrinsics but abs, sqrt, saturate, dot, min, max, no `static const`.
//
// THE TEXT must not use the names of the game's registers (r0..r31, c0..c255, p0, ps, a0, aL): every name here starts with Lp or LP_.

#ifndef LARECOMP_LOCAL_LIGHT_HLSLI
#define LARECOMP_LOCAL_LIGHT_HLSLI

#include "light_laws.hlsli"

// ---- the buffer (local_lights_rules.h PassHeader, kLightsOffset) -----------------------------------------------------------------------------

#define LP_HEADER_BYTES 128
#define LP_LIGHTS_OFFSET 128
#define LP_MAX_LIGHTS 32

#define LP_OFF_RIGHT 0     // xyz the camera's right, w tan_x
#define LP_OFF_UP 16       // xyz up, w tan_y
#define LP_OFF_FORWARD 32  // xyz forward, w the gain
#define LP_OFF_POSITION 48 // xyz the camera's place, w the night light intensity (PS c63.w)
#define LP_OFF_LIN 64      // the projection's depth rows
#define LP_OFF_SIZE 80     // the target's width and height, their inverses
#define LP_OFF_OCC 96      // OcclusionInfo (PS c56) x, y, z, w 1 when known
#define LP_OFF_COUNT 112   // uint count, flags
#define LP_OFF_EXPOSURE 120 // float 2^bias of the main colour target

#define LP_FLAG_MATERIAL 1    // RT3 holds this frame's class
#define LP_FLAG_OCCLUSION 2   // the height factor is applied
#define LP_FLAG_SHADOW 4      // the shadow view table is in the buffer and its atlas is there (GB-H2)

// ---- the shadow view table (local_lights_rules.h kShadowOffset; light_shadow_lookup.h GpuShadowBuffer) -------------------------------------------
// After the lights' rows: a 32 byte header (uint count, atlas bindless slot, atlas width, atlas height, layers (two bits an entry), tile small, tile large, reserved), then eight 96 byte views
// (LocalShadowView: 16 floats of world_to_shadow, the atlas rectangle, depth bias, texel size, the rig's light index, valid). hlsl/light_shadow_lookup.hlsli reads them through the seams the
// pixel shader defines (hlsl/local_light_ps.hlsl).

#define LP_SHADOW_OFFSET 2176       // = LP_LIGHTS_OFFSET + 64 * LP_MAX_LIGHTS
#define LP_SHADOW_HEADER_BYTES 32
#define LP_SHADOW_VIEWS_OFFSET 2208 // = LP_SHADOW_OFFSET + LP_SHADOW_HEADER_BYTES
#define LP_SHADOW_VIEW_BYTES 96
#define LP_SHADOW_OFF_COUNT 2176    // the table's header: uint count, then slot, width, height (four uints: one Load4)
#define LP_SHADOW_OFF_SLOT 2180     // the atlas's bindless slot (a uint)
#define LP_SHADOW_OFF_SIZE 2184     // the atlas's width and height in texels (two uints)
#define LP_SHADOW_OFF_LAYERS 2192   // two bits for each entry: 0 single, 1 static, 2 live

// GB-NEON-COST (the draw's push block, local_lights_rules.h kMaxDrawLights / kPushLateReach): one draw lights a pixel with up to LP_MAX_DRAW_LIGHTS consecutive rows of the buffer (a car's neon tube:
// its points are the lights of one car), summed in the shader; push.info.w bit 0 puts the reach test back after the planes are read (the A/B of the early test).
#define LP_MAX_DRAW_LIGHTS 4
#define LP_PUSH_LATE_REACH 1

// what the depth chain stores for the sky (hlsl/ao_common.hlsli kAoSky)
#define LP_SKY 60000.0f

// the classes that take no light (gbuffer_common.hlsli GB_CLASS_*: 0 no data, 13 emissive)
#define LP_CLASS_NONE 0
#define LP_CLASS_EMISSIVE 13

struct LpGlobals {
  float3 right;
  float tan_x;
  float3 up;
  float tan_y;
  float3 forward;
  float gain;
  float3 position;
  float night;
  float exposure;
  float4 lin;
  float4 size;
  float4 occ;
};

// ---- the pixel's world position ---------------------------------------------------------------------------------------------------------------

// The distance along the view direction of a stored depth (1 near, 0 far): hlsl/ao_common.hlsli AoLinearDepth with the projection's rows in `lin`; LP_SKY for the sky.
float LpLinearDepth(float stored, float4 lin) {
  float ndc = 1.0f - saturate(stored);
  float num = lin.z - ndc * lin.w;
  float den = lin.x - ndc * lin.y;
  float t = num / den;
  return (stored > 0.0f && den != 0.0f && t > 0.0f && t < LP_SKY) ? t : LP_SKY;
}

// The world position of the pixel centre (centre_x, centre_y) at the view distance t (local_lights_rules.h WorldPosition).
float3 LpWorldPosition(LpGlobals g, float centre_x, float centre_y, float t) {
  float u = centre_x / g.size.x;
  float v = centre_y / g.size.y;
  float x = (u * 2.0f - 1.0f) * g.tan_x * t;
  float y = (1.0f - v * 2.0f) * g.tan_y * t;
  return g.position + g.right * x + g.up * y + g.forward * t;
}

// ---- the units and the light ------------------------------------------------------------------------------------------------------------------

// The game's height factor on the lit colour (local_lights_rules.h OcclusionFactor): 1 + saturate((y - OcclusionInfo.x) * OcclusionInfo.y) * (OcclusionInfo.z - 1); 1 when unknown.
float LpOcclusion(float4 occ, float world_y) {
  if (!(occ.w > 0.0f)) {
    return 1.0f;
  }
  float ramp = saturate((world_y - occ.x) * occ.y);
  return 1.0f + ramp * (occ.z - 1.0f);
}

// Whether a light of centre `pos` and `range` can add anything at `world` (local_lights_rules.h Reaches): the three laws of hlsl/light_laws.hlsli all return 0 at or past the range
// (dist2 >= range2), so a pixel that fails this test takes nothing from the light whatever its surface is, and the pixel shader asks it BEFORE it reads the G-buffer's planes. Written as the
// negation of the laws' own test so that a range that is not a number (the laws go on with it) is not left out here either: the pixel then goes the whole way, as it always did.
bool LpReaches(float3 pos, float range, float3 world) {
  float3 d = pos - world;
  return !(dot(d, d) >= range * range);
}

// The light one light adds to one pixel (local_lights_rules.h PixelLight): albedo x lit weight x the law's term x the night light intensity (the game lamps' laws only: a headlight's colour has
// its own) x the main colour target's exposure (2^bias) x the gain x the height factor.
float3 LpPixelLight(LlLight l, LpGlobals g, float3 world, float3 normal, float3 albedo, float lit) {
  float scale = LlLightScale(l, world, normal, 1.0f);
  float night = (l.law == LL_LAW_HEADLIGHT) ? 1.0f : g.night;
  float factor = scale * lit * night * g.gain * g.exposure * LpOcclusion(g.occ, world.y);
  return albedo * l.color * factor;
}

#endif  // LARECOMP_LOCAL_LIGHT_HLSLI
