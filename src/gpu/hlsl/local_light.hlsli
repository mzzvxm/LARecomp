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
// GB-NEON-CONTACT (local_lights_rules.h kPushFullContact / kPushNoContact): bit 1 is the contact term's cost cap taken off (every light of a tube marches for itself), bit 2 the contact term itself
// (mcla_gpu_neon_contact off: a neon light is not shut out of the car it lights).
#define LP_MAX_DRAW_LIGHTS 4
#define LP_PUSH_LATE_REACH 1
#define LP_PUSH_FULL_CONTACT 2
#define LP_PUSH_NO_CONTACT 4

// what the depth chain stores for the sky (hlsl/ao_common.hlsli kAoSky)
#define LP_SKY 60000.0f

// the classes that take no light (gbuffer_common.hlsli GB_CLASS_*: 0 no data, 13 emissive)
#define LP_CLASS_NONE 0
#define LP_CLASS_EMISSIVE 13

// GB-NEON-BLEED (LpContactVisibility at the end of this file; local_lights_rules.h kContact*): the first LP_CONTACT_LENGTH metres of a vehicle pixel's way to a neon light are looked up in the
// picture's own depth. The numbers are metres of the view distance.
#define LP_CONTACT_STEPS 5
#define LP_CONTACT_LENGTH 0.8f   // the part of the way to the light that is looked at
#define LP_CONTACT_NORMAL 0.03f  // the way starts this far off the surface, along its normal
#define LP_CONTACT_BIAS 0.04f    // a surface in front of the way's point counts from this gap on ...
#define LP_CONTACT_RAMP 0.06f    // ... and is fully in the way this much past it
#define LP_CONTACT_THICK 0.7f    // a surface further in front than this is another object, not the shell the way runs behind
#define LP_CONTACT_FADE 0.25f    // and the shell's thickness fades out over this
#define LP_CONTACT_MARCHES 2     // a pixel of a draw of several lights marches for the two strongest only (LpContactDonors)

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

// ---- GB-NEON-BLEED: a neon light does not shine through the car that is lit -----------------------------------------------------------------

// The stored depth of the pixel (px, py) of the main depth plane at the guest's sample 0 (the including shader defines it; LpContactVisibility keeps the index inside the target).
float LpStoredDepth(int px, int py);

// Whether the light takes the contact term at a pixel: a NEON light (the cars' underglow: it has no shadow views, and it sits under a car, so it is inside one body and next to another) on a VEHICLE
// pixel (RT3's flags). The ground, the walls and the kerbs, and every other kind of light, are not asked (local_lights_rules.h TakesContact).
bool LpTakesContact(LlLight l, bool vehicle) {
  return vehicle && l.source == LL_SOURCE_NEON;
}

// How much of a neon light's way to a vehicle pixel is open: 1 open, 0 shut (local_lights_rules.h ContactVisibility). N.L cannot tell a surface that faces the light from one that the light
// can reach: a vent, a bumper lip, the inside of a cabin, the wheel's dish face the light through the car's own panel (the light is not shadowed: the underglow is a glow, not a lamp that casts),
// and the far side of a body is no problem for N.L, its notches are. The picture's depth does tell: a point on the way to the light that lies BEHIND a visible surface (by 4 cm to 70 cm: a
// shell, not a distant wall) is inside something. The way starts 3 cm off the surface along its normal and runs LP_CONTACT_LENGTH metres (or to the light) in LP_CONTACT_STEPS steps; the
// strongest step is the answer, soft in the gap (no pops: a car's panel is a few centimetres to a few decimetres thick). A step off the picture or behind the camera ends the look (the light
// counts as reaching). The cost is LP_CONTACT_STEPS depth reads for a vehicle pixel the light already lights; nothing for any other pixel.
float LpContactVisibility(LpGlobals g, float3 world, float3 normal, float3 light_pos) {
  float3 to_light = light_pos - world;
  float len2 = dot(to_light, to_light);
  if (!(len2 > 1.0e-6f)) {
    return 1.0f;
  }
  float len = sqrt(len2);
  float3 dir = to_light * (1.0f / len);
  float3 origin = world + normal * LP_CONTACT_NORMAL;
  float reach = len < LP_CONTACT_LENGTH ? len : LP_CONTACT_LENGTH;
  float shut = 0.0f;
  for (int i = 0; i < LP_CONTACT_STEPS; ++i) {
    float s = reach * (float(i) + 1.0f) / float(LP_CONTACT_STEPS);
    float3 d = origin + dir * s - g.position;
    float f = dot(d, g.forward);
    if (!(f > 0.05f)) {
      break;
    }
    float px = 0.5f * (dot(d, g.right) / (f * g.tan_x) + 1.0f) * g.size.x;
    float py = 0.5f * (1.0f - dot(d, g.up) / (f * g.tan_y)) * g.size.y;
    if (!(px >= 0.0f && py >= 0.0f && px < g.size.x && py < g.size.y)) {
      break;
    }
    float t = LpLinearDepth(LpStoredDepth(int(px), int(py)), g.lin);
    float gap = f - t;
    float w = saturate((gap - LP_CONTACT_BIAS) / LP_CONTACT_RAMP) * (1.0f - saturate((gap - LP_CONTACT_THICK) / LP_CONTACT_FADE));
    shut = w > shut ? w : shut;
  }
  return 1.0f - shut;
}

// GB-NEON-CONTACT: the cost cap of the contact term. A tube of up to four lights a car is one draw and each light of it marched for itself (LP_CONTACT_STEPS reads each: up to 20 depth reads a pixel).
// The lights of a tube lie in a row, a metre or so apart, so the ways from a pixel to two neighbours go the same way and ask the same depth: a pixel marches for its LP_CONTACT_MARCHES strongest
// lights only (strength: the size of the light's term at the pixel; the lowest index on a tie), and each other light that adds something takes the visibility of the marched light that is nearest to it
// along the tube (the lowest index distance; the stronger on a tie). The word packs 4 bits for each light i: bits 0..1 the index of the light whose visibility light i takes (itself when it marches),
// bit 2 set when it takes one at all (clear: the light adds nothing here, or takes no contact). `s0..s3` are the strengths, 0 for a light that adds nothing or takes no contact; `count` the lights of
// the draw (the rows after the first are zero). With one or two lights over 0 every one marches for itself: the cap changes the pictures of tubes of three and four lights only.
// (local_lights_rules.h ContactDonors is the C++ twin; the measurements that chose this rule are in local_lights_test.cpp ContactCapChecks.)
uint LpContactDonors(float s0, float s1, float s2, float s3, uint count) {
  float s[4] = {s0, s1, s2, s3};
  int m1 = 0;  // the strongest and the second strongest (valid indices even before any is found: no index of -1 is ever read)
  int m2 = 0;
  int n = 0;
  int n2 = 0;
  for (int i = 0; i < 4; ++i) {
    if (uint(i) < count && s[i] > 0.0f) {
      ++n;
      if (n == 1 || s[i] > s[m1]) {
        m1 = i;
      }
    }
  }
  for (int j = 0; j < 4; ++j) {
    if (uint(j) < count && s[j] > 0.0f && j != m1) {
      ++n2;
      if (n2 == 1 || s[j] > s[m2]) {
        m2 = j;
      }
    }
  }
  uint word = 0u;
  for (int k = 0; k < 4; ++k) {
    if (uint(k) < count && s[k] > 0.0f) {
      int donor = k;
      if (k != m1 && k != m2) {
        donor = abs(k - m1) <= abs(k - m2) ? m1 : m2;
      }
      word |= (uint(donor) | 4u) << (4u * uint(k));
    }
  }
  return word;
}

#endif  // LARECOMP_LOCAL_LIGHT_HLSLI
