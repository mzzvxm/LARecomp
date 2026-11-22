#pragma once
// ===========================================================================
// MCLA GPU renderer -- the deferred local light pass: the rules (perf/LIGHT-UNIFY.md 6.2 and 11.2, package GB-H1)
// ===========================================================================
// WHAT THE PASS IS. At the game's mid-scene depth copy (the trigger), after the ambient occlusion's apply and before the sun shafts, the volumetric light and the game's own light volumes and
// glows, one scissored additive draw per light adds `albedo x lit weight x LightTerm(light, surface)` into the main colour, for every pixel the G-buffer holds a normal and an albedo for
// (hlsl/local_light_ps.hlsl). The surface comes from the G-buffer (RT1 the world normal, RT2 the albedo and the lit weight, RT3 the class where the stage writes it), the position from the
// main depth and the camera, the light from the frame's GpuLight rows (light_frame.h, the one description) and the law from hlsl/light_laws.hlsli (the twin of light_laws.h): the city's units,
// so that a lamp added here is the lamp the game's grid draws (THE LAMP A/B below).
//
// THIS FILE is everything of it that needs no renderer, and the unit test (local_lights_test.cpp) runs it: which lights the pass takes, the camera and the pixel's world position (the twin of
// hlsl/local_light.hlsli), the units (the game's night light intensity and its height factor), the scissor of a light, the votes that pick the frame's constants, the layout of the buffer
// the shader reads and of its push block. Pure: no renderer, no guest, no SDK.
//
// WHICH LIGHTS. mcla_gpu_local_lights_sources (a mask): bit 0 the frame's surface lights (LightFrame: the lights that may add surface light and that the game's own shaders do not already
// light with; today the headlight rig's, which ALSO have the forward headlight stage: with the forward stage on the two add, so the headlight route mcla_gpu_headlight_pass (headlight_pass_rules.h, GB-H2) takes the forward stage away when this source is wanted for them: `pass`), bit 1 a test lamp 8 m ahead of the camera and 4.5 m up. The lamps the
// A/B hands over (below) are in the pass whatever the mask says. The game's own lamps (kLightFlagGameLit) are never taken from the frame: its grid lights them. GB-H3's bits 2 to 5 (the police
// light bars, the head lamps of the cars the forward stage does not light, the cars' rear lamps, the mods' lamps outside their grid) and their budgets are local_lights_sources.h's; the
// selection of all of it is its SelectPassLights, which starts from the SelectLights below.
//
// THE LAMP A/B (mcla_gpu_local_lights_ab, restart): ONE lamp through the game's grid and through the pass, the same lamp in the same place on the same surfaces, so the two pictures differ
// only by what the two paths do with it. The route is the mod loader's second light grid (mod_lights.cpp): while the camera is over a mod's rectangle the shader globals point at the mod's
// grid ALONE (the city's is not used), so a lights.bin with ONE lamp and a rectangle around a toolkit scene's camera lights the scene with that lamp and the ambient, and nothing else.
//   0  the grid (the game's way): the lamp is in the grid, the pass takes nothing of it. The reference "A".
//   2  the pass: the grid is built with the lamp OFF (intensity 0, the on flag cleared: mod_lights.cpp WriteLight), and the lamp, as the manager's source publishes it (mod_light_source.h),
//      goes into the pass as a surface light. "B".
//   1  nowhere: the grid has it off and the pass does not take it. The control "C": A - C and B - C are the lamp's own light in each path, free of the ambient, which is the comparison.
// The picture must match within noise: the mean of the lamp's light in B over A inside 5 per cent and per pixel inside 10 per cent (LIGHT-UNIFY section 8, item 7; the test of the run
// plan measures both and the noise floor of two runs of the same path).
//
// THE UNITS (the one thing the G-buffer does not hold, CHECK C9 and C8): the game's lit shaders compute  colour = diffuse x (lamps + ambient) x H(y)  with
//   lamp  = rgb x saturate(I / d^2) x cone x N.L x ShaderGlobals.w                         (PS c63.w: the night light intensity; Light::color holds rgb x I; the headlight's colour holds its own night factor)
//   H(y)  = 1 + saturate((y - OcclusionInfo.x) x OcclusionInfo.y) x (OcclusionInfo.z - 1)  (PS c56; read in xCityDefault::PSMultiLight: r1.w = ramp x (c56.z - 1), then + 1, x the lit colour)
// so the pass multiplies the law's term by  night x H(y) x exposure x gain  where exposure is 2^bias of the main colour target (the target holds the shaders' output times it: the game's bias is
// 4, so 16: screen_rules.h ExposureOf, from ScreenCamera::exposure_bias) and gain is mcla_gpu_local_lights_gain (1: as read). The two constants of the shaders are read from the main view's draws
// (the frame's vote, as the AO picks the sun's) and are CHECK until the lamp A/B holds: a mismatch that depends on height is H, one that does not is night, the exposure or the gain (a ratio near
// 16 or 1/16 is the exposure applied twice or not at all).
// THE BAKED OCCLUSION (GB-H2-COVER, measured on the retail text): the lit shaders also multiply the SUM of their lights by the vertex colour's alpha (iColor1.w; the windows by its square), so the
// same headlight through the forward stage is darker than through the pass by that factor: GB-H2's A/B read pass over forward at 2.1 to 2.2 on the walls, larger where the base picture is darker.
// The night stage writes lit weight x saturate(occlusion) to RT2.a (gbuffer_night_albedo.h kGBufferNightOcclusionRows), and `lit` below carries it, so the pass is darkened as the game's lamps are.
// The screen space AO apply still darkens the forward light and not the pass's (the pass runs after it, by design: HP11).
// ===========================================================================

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "light_convert.h"
#include "light_frame.h"
#include "light_id.h"
#include "light_laws.h"
#include "lamp_shadow_frame.h"
#include "lamp_shadow_lookup.h"
#include "light_shadow_lookup.h"
#include "rt_lights_rules.h"

namespace larecomp::gpu::local_lights {

// ---------------------------------------------------------------------------
// the numbers (hlsl/local_light.hlsli has the twins: LP_*)
// ---------------------------------------------------------------------------

inline constexpr uint32_t kMaxPassLights = kMaxSurfaceLights;  // 32: one draw each
inline constexpr uint32_t kHeaderBytes = 128;
inline constexpr uint32_t kLightsOffset = kHeaderBytes;
// GB-H2: after the lights' rows the shadow view table of the frame (light_shadow_lookup.h GpuShadowBuffer: a 32 byte header with the atlas's bindless slot and size and the layers, then eight of the
// 96 byte views), so that a light with a ShadowRef of the atlas is looked up through the one lookup (hlsl/light_shadow_lookup.hlsli): the pass reads it from the same buffer, no set of its own.
inline constexpr uint32_t kShadowOffset = kLightsOffset + uint32_t(sizeof(GpuLight)) * kMaxPassLights;
inline constexpr uint32_t kShadowBytes = uint32_t(sizeof(shadow_lookup::GpuShadowBuffer));
// LAMP-SHADOWS-4: after the headlights' table the street lamps' (lamp_shadow_frame.h GpuLampTable: a 32 byte header with the atlas's slot and size and the layers of up to 32 entries, then twenty
// of the 96 byte views): a lamp's light names its entries in the combined entry space (kLampEntryBase + the row) and is looked up through the same one lookup (hlsl/lamp_shadow_lookup.hlsli).
inline constexpr uint32_t kLampOffset = kShadowOffset + kShadowBytes;
inline constexpr uint32_t kLampBytes = uint32_t(sizeof(GpuLampTable));
// RT-LIGHTS-D1: after the lamps' table the ray traced visibility's table (rt_lights_rules.h GpuRtMaskTable: the filtered and the raw images' bindless slots, their size and the occlusion's linear depth
// image that guides the upsample): a light with the flag kLightFlagRtVisibility reads its ShadowRef::mask_channel from them (hlsl/rt_mask_read.hlsli).
inline constexpr uint32_t kRtOffset = kLampOffset + kLampBytes;
inline constexpr uint32_t kRtBytes = uint32_t(sizeof(rt_lights::GpuRtMaskTable));
inline constexpr uint32_t kBufferBytes = kRtOffset + kRtBytes;
inline constexpr uint32_t kBufferAlloc = (kBufferBytes + 255u) & ~255u;  // an upload buffer's size

// PassHeader::flags (LP_FLAG_*).
inline constexpr uint32_t kFlagMaterial = 1u << 0;    // RT3 holds this frame's class: the pass skips the classes that take no light
inline constexpr uint32_t kFlagOcclusion = 1u << 1;   // OcclusionInfo was read: the height factor is applied
inline constexpr uint32_t kFlagShadow = 1u << 2;      // GB-H2: the shadow view table is in the buffer and its atlas is there: a light with views of the atlas is shadowed
inline constexpr uint32_t kFlagLampShadow = 1u << 3;  // LAMP-SHADOWS-4: the street lamps' table is in the buffer and its atlas is there: a lamp's light with views is shadowed (or, for a lamp the game lights, its shadowed share taken away)
inline constexpr uint32_t kFlagRtMask = 1u << 4;      // RT-LIGHTS-D1: the ray traced visibility's table is in the buffer and its images are this frame's: a light with a channel reads it

// PassHeader::debug: what the pass draws for a street lamp's light instead of the light (mcla_gpu_lamp_shadows_view): 0 the light, 1 the shadow's own picture (red where the lamp's shadow takes
// light, green where it is lit), 2 the lamp's table row (a colour for each lamp, brighter in the shadow), 3 REMOVE: the lamp's whole term taken away whatever the shadow (a game lamp: -term; an owned
// lamp: nothing added): the UNITS CHECK, the pool must go to the ambient and no pixel may go negative.
inline constexpr uint32_t kDebugNone = 0, kDebugVisibility = 1, kDebugLamps = 2, kDebugRemove = 3;

// The sources mask of mcla_gpu_local_lights_sources. Bits 0 and 1 are GB-H1's; bits 2 to 5 are GB-H3's (local_lights_sources.h makes their lights and holds their budgets): the police light bars,
// the headlights of the cars the forward headlight stage does not light, the cars' brake / tail / turn / reverse lamps, and the mods' lamps the game's grid is not lighting.
inline constexpr uint32_t kSourceFrame = 1u << 0;
inline constexpr uint32_t kSourceTest = 1u << 1;
inline constexpr uint32_t kSourcePolice = 1u << 2;
inline constexpr uint32_t kSourceHeadLamps = 1u << 3;
inline constexpr uint32_t kSourceRearLamps = 1u << 4;
inline constexpr uint32_t kSourceMod = 1u << 5;
inline constexpr uint32_t kSourceMask = kSourceFrame | kSourceTest | kSourcePolice | kSourceHeadLamps | kSourceRearLamps | kSourceMod;
// The sources that need the cars' light points (the manager's car lights source).
inline constexpr uint32_t kSourceCars = kSourcePolice | kSourceHeadLamps | kSourceRearLamps;
// What SelectLights (below) reads: the frame's own lights and the test lamp.
inline constexpr uint32_t kSourceFrameAndTest = kSourceFrame | kSourceTest;

// Where a light of the pass came from, for the budgets and the log lines (GB-H3). kFrame: the frame's surface lights (the rig's headlights today); kHandedOver: the lamp A/B's mod lamps and the
// test lamp; the others are local_lights_sources.h's classes.
enum class SourceClass : uint8_t { kFrame = 0, kHandedOver, kPolice, kHeadLamps, kRearLamps, kMod };
inline constexpr uint32_t kSourceClassCount = 6;
inline constexpr const char* SourceClassName(SourceClass c) {
  switch (c) {
    case SourceClass::kFrame: return "frame";
    case SourceClass::kHandedOver: return "handed over";
    case SourceClass::kPolice: return "police";
    case SourceClass::kHeadLamps: return "head lamps";
    case SourceClass::kRearLamps: return "rear lamps";
    case SourceClass::kMod: return "mod lamps";
  }
  return "?";
}

// mcla_gpu_local_lights_ab.
enum class Ab : uint8_t { kGrid = 0, kNowhere = 1, kPass = 2 };
inline Ab AbOf(int value) { return value == 1 ? Ab::kNowhere : (value == 2 ? Ab::kPass : Ab::kGrid); }
// What the mod loader does with the lamps of a lights.bin in the grid it builds: with the pass or nowhere they are in the grid OFF (mod_lights.cpp).
inline bool AbWithholdsFromGrid(Ab ab) { return ab != Ab::kGrid; }

inline constexpr float kSky = 60000.0f;  // what the depth chain stores for the sky (hlsl/ao_common.hlsli kAoSky): the stored depth 0 here, the same bound
inline constexpr uint64_t kTestLampId = 0x7E57'1A3B'00C0'FFEEull;

// ---------------------------------------------------------------------------
// the buffer the pixel shader reads (a ByteAddressBuffer: 128 bytes of header, the GpuLight rows, then, GB-H2, the frame's shadow view table) and its push block
// ---------------------------------------------------------------------------

struct PassHeader {
  float right[3];
  float tan_x;
  float up[3];
  float tan_y;
  float forward[3];
  float gain;
  float position[3];
  float night;
  float lin[4];   // the projection's depth rows: t = (lin[2] - ndc * lin[3]) / (lin[0] - ndc * lin[1]), ndc = 1 - stored depth
  float size[4];  // the colour target's width and height in host pixels, and their inverses
  float occ[4];   // OcclusionInfo x, y, z; w 1 when it was read
  uint32_t count;
  uint32_t flags;
  float exposure;  // the main colour target's exponent bias as a factor, 2^bias (screen_frame.h ScreenCamera::exposure_bias)
  uint32_t debug;  // kDebug*
};
static_assert(sizeof(PassHeader) == kHeaderBytes, "the header is eight rows of 16 bytes");
static_assert(offsetof(PassHeader, right) == 0 && offsetof(PassHeader, up) == 16 && offsetof(PassHeader, forward) == 32 && offsetof(PassHeader, position) == 48 &&
                  offsetof(PassHeader, lin) == 64 && offsetof(PassHeader, size) == 80 && offsetof(PassHeader, occ) == 96 && offsetof(PassHeader, count) == 112 && offsetof(PassHeader, exposure) == 120 && offsetof(PassHeader, debug) == 124,
              "the header's rows (hlsl/local_light.hlsli LP_OFF_*)");

// The push block of one draw (hlsl LocalLightPush): the bindless slots of the main depth plane and of RT1 to RT3 (kNoLightSlot: not there), then the light's index, the flags, the host
// sample that is the guest's sample 0.
struct PassPush {
  uint32_t slots[4];
  uint32_t info[4];
};
static_assert(sizeof(PassPush) == 32, "the push block is two rows");

// ---------------------------------------------------------------------------
// the camera: the rows of a view inverse
// ---------------------------------------------------------------------------

// The camera's axes and place in world space: from the game's view inverse (row vectors: row 0 the camera's right, row 1 its up, row 2 its BACK, row 3 its position; the game's view space
// looks along -z, the renderer's passes look along +z, so forward is minus row 2). The same as volumetrics_rules.h VolCamera and ao_gbuffer.h Rows.
struct Rows {
  float right[3] = {1.0f, 0.0f, 0.0f};
  float up[3] = {0.0f, 1.0f, 0.0f};
  float forward[3] = {0.0f, 0.0f, 1.0f};
  float position[3] = {0.0f, 0.0f, 0.0f};
};

// False when an entry is not finite or one of the three axes is not a unit vector (the matrix is not a camera's: the same test as ao_gbuffer.h RowsFromViewInverse).
inline bool RowsFromViewInverse(const float m[16], Rows* out) {
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 3; ++c) {
      if (!std::isfinite(m[r * 4 + c])) {
        return false;
      }
    }
  }
  for (int r = 0; r < 3; ++r) {
    const double length = std::sqrt(double(m[r * 4]) * m[r * 4] + double(m[r * 4 + 1]) * m[r * 4 + 1] + double(m[r * 4 + 2]) * m[r * 4 + 2]);
    if (!(length > 0.98 && length < 1.02)) {
      return false;
    }
  }
  for (int c = 0; c < 3; ++c) {
    out->right[c] = m[c];
    out->up[c] = m[4 + c];
    out->forward[c] = -m[8 + c];
    out->position[c] = m[12 + c];
  }
  return true;
}

// ---------------------------------------------------------------------------
// a pixel's world position (the twin of hlsl/local_light.hlsli LpLinearDepth, LpWorldPosition)
// ---------------------------------------------------------------------------

// The distance along the view direction of a stored depth (1 near, 0 far); kSky for the sky and for anything the linearisation cannot place (hlsl/ao_common.hlsli AoLinearDepth).
inline float LinearDepth(float stored, const float lin[4]) {
  const float ndc = 1.0f - std::clamp(stored, 0.0f, 1.0f);
  const float num = lin[2] - ndc * lin[3];
  const float den = lin[0] - ndc * lin[1];
  const float t = num / den;
  return (stored > 0.0f && den != 0.0f && t > 0.0f && t < kSky) ? t : kSky;
}

// The world position of the centre (centre_x, centre_y) of a pixel of an image `width` x `height` at the view distance t: position + right x X + up x Y + forward x t, with
// X = (2 u - 1) tan_x t and Y = (1 - 2 v) tan_y t (volumetrics_rules.h ViewToWorld; hlsl/ao_common.hlsli AoViewPos).
inline void WorldPosition(const Rows& rows, float tan_x, float tan_y, float centre_x, float centre_y, float width, float height, float t, float out[3]) {
  const float u = centre_x / width;
  const float v = centre_y / height;
  const float x = (u * 2.0f - 1.0f) * tan_x * t;
  const float y = (1.0f - v * 2.0f) * tan_y * t;
  for (int i = 0; i < 3; ++i) {
    out[i] = rows.position[i] + rows.right[i] * x + rows.up[i] * y + rows.forward[i] * t;
  }
}

// A world point in the camera's image: the pixel (x to the right, y down) and the view distance; false at or behind `near_eps` (the point is not in front of the camera).
inline bool ProjectToPixel(const Rows& rows, float tan_x, float tan_y, float width, float height, const float p[3], float near_eps, float* px, float* py, float* t) {
  const float d[3] = {p[0] - rows.position[0], p[1] - rows.position[1], p[2] - rows.position[2]};
  const float x = d[0] * rows.right[0] + d[1] * rows.right[1] + d[2] * rows.right[2];
  const float y = d[0] * rows.up[0] + d[1] * rows.up[1] + d[2] * rows.up[2];
  const float f = d[0] * rows.forward[0] + d[1] * rows.forward[1] + d[2] * rows.forward[2];
  if (!(f > near_eps)) {
    return false;
  }
  *px = 0.5f * (x / (f * tan_x) + 1.0f) * width;
  *py = 0.5f * (1.0f - y / (f * tan_y)) * height;
  *t = f;
  return true;
}

// ---------------------------------------------------------------------------
// the units
// ---------------------------------------------------------------------------

struct Units {
  float night = 1.0f;                       // ShaderGlobals.w (PS c63.w): the game's night light intensity; the game lamps' laws multiply by it, the headlight's does not
  float gain = 1.0f;                        // mcla_gpu_local_lights_gain
  float exposure = 1.0f;                    // 2^bias of the main colour target (ScreenCamera::exposure_bias): the target holds the shaders' output times it; 1 where no draw told it
  float occ[4] = {0.0f, 0.0f, 1.0f, 0.0f};  // OcclusionInfo x, y, z (PS c56), w 1 when read
};

// The game's height factor on the lit colour: 1 where OcclusionInfo is not known.
inline float OcclusionFactor(const float occ[4], float world_y) {
  if (!(occ[3] > 0.0f)) {
    return 1.0f;
  }
  const float ramp = std::clamp((world_y - occ[0]) * occ[1], 0.0f, 1.0f);
  return 1.0f + ramp * (occ[2] - 1.0f);
}

// The light one light adds to one pixel, in the main colour's units: albedo x lit weight x LightTerm x the units (the twin of hlsl LpPixelLight). `lit` is RT2's alpha, 0..1.
inline void PixelLight(const Light& l, const Units& u, const float world[3], const float normal[3], const float albedo[3], float lit, float out[3]) {
  light_laws::LightSurface s;
  for (int i = 0; i < 3; ++i) {
    s.position[i] = world[i];
    s.normal[i] = normal[i];
  }
  const float scale = light_laws::LightScale(l, s, 1.0f);
  const float night = l.law == LightLaw::kHeadlight ? 1.0f : u.night;
  const float factor = scale * lit * night * u.gain * u.exposure * OcclusionFactor(u.occ, world[1]);
  for (int i = 0; i < 3; ++i) {
    out[i] = albedo[i] * l.color[i] * factor;
  }
}

// The surface's GEOMETRIC unit normal at a pixel for the shadow lookup, turned to the camera: the lit shaders' rule (headlight.cpp HeadlightLight: cross(ddy(world), ddx(world)) over the pixel's
// 2 x 2 quad) taken from the depth plane: w00 the world position of the quad's top left pixel, w10 its right neighbour's, w01 its lower neighbour's (hlsl/local_light_ps.hlsl LpShadowNormal is
// the twin). `valid` is false where one of the three is the sky or outside the plane: then, as for a degenerate quad, `fallback` is the answer.
inline void QuadNormal(const float w00[3], const float w10[3], const float w01[3], bool valid, const float world[3], const float camera[3], const float fallback[3], float out[3]) {
  if (!valid) {
    for (int i = 0; i < 3; ++i) {
      out[i] = fallback[i];
    }
    return;
  }
  const float dx[3] = {w10[0] - w00[0], w10[1] - w00[1], w10[2] - w00[2]};
  const float dy[3] = {w01[0] - w00[0], w01[1] - w00[1], w01[2] - w00[2]};
  float n[3] = {dy[1] * dx[2] - dy[2] * dx[1], dy[2] * dx[0] - dy[0] * dx[2], dy[0] * dx[1] - dy[1] * dx[0]};  // cross(dy, dx)
  const float len2 = n[0] * n[0] + n[1] * n[1] + n[2] * n[2];
  if (!(len2 > 1.0e-14f)) {
    for (int i = 0; i < 3; ++i) {
      out[i] = fallback[i];
    }
    return;
  }
  const float inv = 1.0f / std::sqrt(len2);
  const float to_camera = n[0] * (camera[0] - world[0]) + n[1] * (camera[1] - world[1]) + n[2] * (camera[2] - world[2]);
  const float sign = to_camera < 0.0f ? -inv : inv;
  for (int i = 0; i < 3; ++i) {
    out[i] = n[i] * sign;
  }
}

// GB-H2: the pass's whole per-pixel path, the twin of hlsl/local_light_ps.hlsl main after the surface and the depth are read: the light's term (PixelLight), then, for a light that has views of the
// atlas and a frame whose table is in the buffer (kFlagShadow), the ONE lookup's visibility (shadow_lookup::LightLit, the lit shaders' lookup: Mode::kSurface, the receiver's plane from the unit
// normal `shadow_normal`, which is the surface's geometric one: QuadNormal; null: `normal`) multiplied in. `depth_at(ix, iy)` is the atlas's stored depth of a texel, the index clamped into the
// atlas. Returns whether the pixel adds anything (the shader discards otherwise).
template <class Depth>
inline bool PixelLightShadowed(const Light& l, const Units& u, const ShadowFrame* shadow, bool table, const float world[3], const float normal[3], const float albedo[3], float lit, Depth&& depth_at,
                               float out[3], const float* shadow_normal = nullptr, bool rt_ref = false, float rt_visibility = 1.0f) {
  PixelLight(l, u, world, normal, albedo, lit, out);
  if (!(out[0] > 0.0f || out[1] > 0.0f || out[2] > 0.0f)) {
    return false;
  }
  float visibility = 1.0f;
  const bool atlas = table && shadow != nullptr && l.shadow.kind == ShadowKind::kAtlas && l.shadow.view_count != 0;
  if (atlas) {
    visibility = shadow_lookup::LightLit(*shadow, l.shadow, world, shadow_normal != nullptr ? shadow_normal : normal, shadow_lookup::Mode::kSurface, depth_at);
  }
  if (rt_ref && (l.flags & kLightFlagRtVisibility) != 0) {
    // RT-LIGHTS-D1: the ray's visibility is multiplied in (the product with the atlas's: rt_lights::Combine), a light the game lights is only taken away from
    const float term[3] = {out[0], out[1], out[2]};
    rt_lights::Apply((l.flags & kLightFlagGameLit) != 0, term, rt_lights::Combine(visibility, rt_visibility), out);
    return out[0] != 0.0f || out[1] != 0.0f || out[2] != 0.0f;
  }
  if (atlas) {
    for (int i = 0; i < 3; ++i) {
      out[i] *= visibility;
    }
  }
  return out[0] > 0.0f || out[1] > 0.0f || out[2] > 0.0f;
}

// LAMP-SHADOWS-4: the pass's per-pixel path for a STREET LAMP's light, the twin of the lamp branch of hlsl/local_light_ps.hlsl main. The light's term is PixelLight's. A lamp whose ShadowRef names
// entries of the lamps' table and a frame whose table is in the buffer (kFlagLampShadow) is looked up (shadow_lookup::LampLit: the ONE lookup plus the edge fade, Mode::kSurface), and then
//   a lamp the GAME LIGHTS (kLightFlagGameLit: its grid already added the term)  ->  the shadowed share is taken away:  out = -term x (1 - visibility)   (the blend is ADD: a negative colour)
//   a lamp the pass OWNS (the flag clear: the grid has it off)                   ->  out = term x visibility
// A game lamp with no table (the frame has none, the table's atlas is gone) has nothing to take away. `debug` (kDebug*) draws the shadow's picture instead. Returns whether the pixel adds (or
// takes) anything (the shader discards otherwise). `depth_at(ix, iy)` is the atlas's stored depth of a texel, the index clamped into the atlas. `shadow_normal` is the surface's GEOMETRIC unit
// normal for the lookup's plane test (QuadNormal, as the headlights'; null: `normal`).
// RT-LIGHTS-D1: a lamp with the flag kLightFlagRtVisibility and a ray traced table in the buffer (`rt_ref`: kFlagRtMask) has the ray's visibility `rt_visibility` (1 where its ray was free) multiplied into
// the atlas's (rt_lights::Combine: the product of two partial caster sets); a lamp with ONLY the ray (no atlas views) is looked at too, and a lamp the game lights is still only ever taken away from:
// with every visibility 1 nothing is drawn, as if the pass did not know the lamp.
template <class Depth>
inline bool PixelLampShadowed(const Light& l, const Units& u, const LampShadowFrame* lamps, bool table, uint32_t debug, const float world[3], const float normal[3], const float albedo[3],
                              float lit, Depth&& depth_at, float out[3], const float* shadow_normal = nullptr, bool rt_ref = false, float rt_visibility = 1.0f) {
  PixelLight(l, u, world, normal, albedo, lit, out);
  if (!(out[0] > 0.0f || out[1] > 0.0f || out[2] > 0.0f)) {
    return false;
  }
  const bool game_lit = (l.flags & kLightFlagGameLit) != 0;
  const bool atlas_ref = l.source == LightSource::kLamp && l.shadow.kind == ShadowKind::kAtlas && l.shadow.view_count != 0;
  const bool rt_here = rt_ref && l.source == LightSource::kLamp && (l.flags & kLightFlagRtVisibility) != 0;
  if (l.source != LightSource::kLamp) {
    return true;
  }
  if (!atlas_ref && !rt_here) {
    return !game_lit;   // RT-LIGHTS-D1: a lamp the game lights with no source of visibility is never added on top of the grid's (the shader discards it); an owned lamp is added as it was
  }
  if (debug == kDebugRemove) {
    for (int i = 0; i < 3; ++i) {
      out[i] = game_lit ? -out[i] : 0.0f;
    }
    return game_lit;
  }
  float atlas_visibility = 1.0f;
  if (atlas_ref) {
    if (!table || lamps == nullptr) {
      if (!rt_here) {
        return !game_lit;
      }
    } else {
      atlas_visibility = shadow_lookup::LampLit(*lamps, l.shadow, l.pos, world, shadow_normal != nullptr ? shadow_normal : normal, shadow_lookup::Mode::kSurface, depth_at);
      if (debug == kDebugVisibility) {
        const float k = u.exposure * 0.15f;
        out[0] = (1.0f - atlas_visibility) * k;
        out[1] = atlas_visibility * k;
        out[2] = 0.0f;
        return true;
      }
      if (debug == kDebugLamps) {
        const float f = float(l.shadow.first_view);
        const float k = u.exposure * (0.05f + 0.25f * (1.0f - atlas_visibility));
        const auto fract = [](float v) { return v - std::floor(v); };
        out[0] = fract(f * 0.37f) * k;
        out[1] = fract(f * 0.61f + 0.3f) * k;
        out[2] = fract(f * 0.83f + 0.6f) * k;
        return true;
      }
    }
  }
  const float visibility = rt_here ? rt_lights::Combine(atlas_visibility, rt_visibility) : atlas_visibility;
  for (int i = 0; i < 3; ++i) {
    out[i] = game_lit ? -out[i] * (1.0f - visibility) : out[i] * visibility;
  }
  return out[0] != 0.0f || out[1] != 0.0f || out[2] != 0.0f;
}

// ---------------------------------------------------------------------------
// the frame's constants, picked by vote
// ---------------------------------------------------------------------------

// The registers the units come from are the game's shader globals: they hold one value for the frame, and a draw whose shader does not use them leaves what an earlier one set (the AO's rule
// for the sun's constants: "the value most colour draws carry is the frame's"). So every colour draw of the main view before the trigger votes with the four floats it carries (c63.w, c56.xyz,
// as raw bits: a NaN is a value too) and the frame's units are the most common one.
class UnitsVote {
 public:
  static constexpr uint32_t kSlots = 8;
  struct Entry {
    std::array<uint32_t, 4> bits{};
    uint32_t votes = 0;
  };
  void Reset() {
    count_ = 0;
    overflow_ = 0;
    draws_ = 0;
  }
  void Add(const std::array<uint32_t, 4>& bits) {
    ++draws_;
    for (uint32_t i = 0; i < count_; ++i) {
      if (entries_[i].bits == bits) {
        ++entries_[i].votes;
        return;
      }
    }
    if (count_ < kSlots) {
      entries_[count_].bits = bits;
      entries_[count_].votes = 1;
      ++count_;
    } else {
      ++overflow_;
    }
  }
  bool Best(std::array<uint32_t, 4>* bits) const {
    if (count_ == 0) {
      return false;
    }
    uint32_t best = 0;
    for (uint32_t i = 1; i < count_; ++i) {
      if (entries_[i].votes > entries_[best].votes) {
        best = i;  // the first of equals wins: a tie is told by the order the values came in
      }
    }
    *bits = entries_[best].bits;
    return true;
  }
  uint32_t draws() const { return draws_; }
  uint32_t distinct() const { return count_; }
  uint32_t overflow() const { return overflow_; }

 private:
  std::array<Entry, kSlots> entries_{};
  uint32_t count_ = 0;
  uint32_t overflow_ = 0;
  uint32_t draws_ = 0;
};

inline float BitsToFloat(uint32_t bits) {
  float f;
  std::memcpy(&f, &bits, sizeof(f));
  return f;
}
inline uint32_t FloatToBits(float f) {
  uint32_t bits;
  std::memcpy(&bits, &f, sizeof(bits));
  return bits;
}

// The units a vote's winner says: c63.w the night light intensity (finite, 0 to 8: the game's is 0 to 1), OcclusionInfo only where its three numbers are finite and its scale is positive.
// `gain` is the setting's.
inline Units UnitsFromBits(const std::array<uint32_t, 4>& bits, float gain) {
  Units u;
  u.gain = gain;
  const float night = BitsToFloat(bits[0]);
  u.night = (std::isfinite(night) && night >= 0.0f && night <= 8.0f) ? night : 1.0f;
  const float x = BitsToFloat(bits[1]);
  const float y = BitsToFloat(bits[2]);
  const float z = BitsToFloat(bits[3]);
  if (std::isfinite(x) && std::isfinite(y) && std::isfinite(z) && y > 0.0f && std::fabs(z) < 64.0f && std::fabs(x) < 1.0e6f) {
    u.occ[0] = x;
    u.occ[1] = y;
    u.occ[2] = z;
    u.occ[3] = 1.0f;
  }
  return u;
}

// ---------------------------------------------------------------------------
// which lights
// ---------------------------------------------------------------------------

// Whether the pass adds a light of the frame: it may add surface light, the game's own shaders do not already light with it (a street lamp: its grid does), it is on and it has colour.
inline bool TakesFrameLight(const Light& l) {
  if (l.id == kNoLightId) {
    return false;
  }
  if ((l.roles & kRoleMaskSurface) == 0 || (l.flags & kLightFlagGameLit) != 0) {
    return false;
  }
  if (!(l.range > 0.0f) || !(std::max({l.color[0], l.color[1], l.color[2]}) > 0.0f)) {
    return false;
  }
  return true;
}

// A game lamp the pass lights with in place of the grid (the A/B): the same light, now allowed to add surface light and no longer lit by the game.
inline Light AsPassLight(const Light& l) {
  Light out = l;
  out.flags &= ~kLightFlagGameLit;
  out.roles = uint8_t(out.roles | kRoleMaskSurface);
  return out;
}

// The test lamp: an LA street lamp (I 55, the game's cone 125 / -50.5, warm white) 8 m ahead of the camera and 4.5 m up, pointing down (the volume's test lamp: mcla_gpu_vol_test 1).
inline Light MakeTestLamp(const Rows& rows) {
  float pos[3];
  for (int i = 0; i < 3; ++i) {
    pos[i] = rows.position[i] + rows.forward[i] * 8.0f;
  }
  pos[1] += 4.5f;
  const float colour[3] = {1.0f, 0.82f, 0.55f};
  const float dir[3] = {0.0f, -1.0f, 0.0f};
  Light l = MakeGameLampLight(pos, colour, dir, 55.0f, 125.0f, -50.5f, 1.0f, 1, LightSource::kOther, kTestLampId);
  return AsPassLight(l);
}

struct Selection {
  std::array<Light, kMaxPassLights> lights{};
  uint32_t count = 0;
  uint32_t from_frame = 0;   // taken from the frame
  uint32_t from_extra = 0;   // handed over (the A/B lamps, the test lamp)
  uint32_t dropped = 0;      // wanted and over the budget
  // GB-H3 (local_lights_sources.h SelectPassLights): the lights taken from each class (SourceClass) and the wanted ones it cut for its class budget or the pass's 32.
  std::array<uint32_t, kSourceClassCount> from_class{};
  std::array<uint32_t, kSourceClassCount> cut_class{};
};

// The lights of the pass: the frame's surface lights in the frame's own ranking (the surface role's, best first; the slots in order where the frame has no ranking) when
// `sources` has kSourceFrame, at most `frame_cap` of them (GB-H2: the frame class's budget, local_lights_sources.h BudgetOf; the headlights' four), then `extra` (the A/B's lamps and the test
// lamp), at most kMaxPassLights over all. A frame light over the cap is counted in cut_class[kFrame].
inline Selection SelectLights(const LightFrame& frame, uint32_t sources, const Light* extra, uint32_t extra_count, uint32_t frame_cap = kMaxPassLights) {
  Selection s;
  const auto add = [&](const Light& l, bool from_frame) {
    if (s.count >= kMaxPassLights) {
      ++s.dropped;
      return;
    }
    s.lights[s.count++] = l;
    ++(from_frame ? s.from_frame : s.from_extra);
  };
  const auto add_frame = [&](const Light& l) {
    if (s.from_frame >= frame_cap) {
      ++s.cut_class[size_t(SourceClass::kFrame)];
      return;
    }
    add(l, true);
  };
  if ((sources & kSourceFrame) != 0) {
    const uint32_t ranked = std::min<uint32_t>(frame.RoleCount(LightRole::kSurface), kMaxLights);
    if (ranked != 0) {
      for (uint32_t r = 0; r < ranked; ++r) {
        const Light& l = frame.Ranked(LightRole::kSurface, r);
        if (TakesFrameLight(l)) {
          add_frame(l);
        }
      }
    } else {
      for (uint32_t i = 0; i < frame.count && i < kMaxLights; ++i) {
        if (TakesFrameLight(frame.lights[i])) {
          add_frame(frame.lights[i]);
        }
      }
    }
  }
  for (uint32_t i = 0; i < extra_count; ++i) {
    add(extra[i], false);
  }
  s.from_class[size_t(SourceClass::kFrame)] = s.from_frame;
  s.from_class[size_t(SourceClass::kHandedOver)] = s.from_extra;
  return s;
}

// RT-LIGHTS-D1: what the pass does with a selection of lights and the ray traced channels of the frame, in the pass's own terms.
//   AnnotateSelection  every selected light whose id holds a channel gets the flag kLightFlagRtVisibility and ShadowRef::mask_channel (its atlas kind and views are NOT touched: the product reads both);
//                      a light that holds none loses the flag. Returns how many lights hold a channel.
//   TrimSelection      a light the GAME lights that has no source of visibility at all (no atlas views, no channel) is taken out: the pass would ADD its term on top of the grid's. A lamp that was handed to
//                      the pass only to get a ray, and was not picked for a channel, goes this way; so does any game's lamp whose provider is gone. Nothing else is touched. Returns how many were taken out.
// Both keep the selection's order (the draws' order) and its counters.
inline uint32_t AnnotateSelection(const rt_lights::Assignment& channels, Selection* selection) {
  uint32_t held = 0;
  for (uint32_t i = 0; i < selection->count && i < kMaxPassLights; ++i) {
    Light& l = selection->lights[i];
    const uint32_t channel = rt_lights::ChannelFor(channels, l.id);
    if (channel != rt_lights::kNoChannel) {
      l.flags |= kLightFlagRtVisibility;
      l.shadow.mask_channel = uint8_t(channel);
      ++held;
    } else {
      l.flags &= ~kLightFlagRtVisibility;
    }
  }
  return held;
}

inline bool HasVisibilitySource(const Light& l) {
  return (l.shadow.kind == ShadowKind::kAtlas && l.shadow.view_count != 0) || (l.flags & kLightFlagRtVisibility) != 0;
}

inline uint32_t TrimSelection(Selection* selection) {
  uint32_t kept = 0;
  uint32_t removed = 0;
  Selection out = *selection;
  for (uint32_t i = 0; i < selection->count && i < kMaxPassLights; ++i) {
    const Light& l = selection->lights[i];
    if ((l.flags & kLightFlagGameLit) != 0 && !HasVisibilitySource(l)) {
      ++removed;
      // the counters: a game's lamp comes to the pass as a handed over light (the lamp shadows' lamps, a lamp given for its ray)
      if (out.from_extra > 0) {
        --out.from_extra;
      }
      if (out.from_class[size_t(SourceClass::kHandedOver)] > 0) {
        --out.from_class[size_t(SourceClass::kHandedOver)];
      }
      continue;
    }
    out.lights[kept++] = l;
  }
  for (uint32_t i = kept; i < kMaxPassLights; ++i) {
    out.lights[i] = Light{};
  }
  out.count = kept;
  *selection = out;
  return removed;
}

// ---------------------------------------------------------------------------
// the scissor of a light
// ---------------------------------------------------------------------------

// A sphere that holds the part of the light's reach that can be lit: the ball of `range` round the light, or, for a narrow cone (a half angle under 45 degrees), the smaller sphere through
// the apex and the rim of the cone's base at `range`: radius range / (2 cos^2) on the axis (the whole truncated cone is inside it).
inline void BoundingSphere(const Light& l, float centre[3], float* radius) {
  float r = l.range;
  float along = 0.0f;
  if (l.kind == LightKind::kSpot && l.cos_outer > 0.7072f) {
    r = l.range / (2.0f * l.cos_outer * l.cos_outer);
    along = r;
  }
  for (int i = 0; i < 3; ++i) {
    centre[i] = l.pos[i] + l.dir[i] * along;
  }
  *radius = r;
}

struct PixelRect {
  int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // [x0, x1) x [y0, y1) in pixels, y down
  bool Empty() const { return x1 <= x0 || y1 <= y0; }
  uint64_t Area() const { return Empty() ? 0ull : uint64_t(x1 - x0) * uint64_t(y1 - y0); }
};

inline constexpr float kScissorNear = 0.05f;   // a corner nearer than this (or behind the camera) makes the light's rectangle the whole picture
inline constexpr int32_t kScissorMargin = 2;   // pixels: the jitter, the previous frame's lens and a rounding

// The pixels a light can reach: the box round its sphere, in the camera's image. The whole picture when some corner of the box is not in front of the camera (the camera is inside it or at its
// edge); empty when every corner is behind the camera (the light's reach is all behind the picture) or the box is off the picture. Conservative by construction (a box holds the sphere;
// the margin takes what is left).
inline PixelRect ScissorOfLight(const Light& l, const Rows& rows, float tan_x, float tan_y, uint32_t width, uint32_t height) {
  PixelRect full;
  full.x1 = int32_t(width);
  full.y1 = int32_t(height);
  float c[3];
  float r;
  BoundingSphere(l, c, &r);
  float min_x = 1.0e30f, min_y = 1.0e30f, max_x = -1.0e30f, max_y = -1.0e30f;
  int behind = 0;
  for (int corner = 0; corner < 8; ++corner) {
    const float p[3] = {c[0] + ((corner & 1) ? r : -r), c[1] + ((corner & 2) ? r : -r), c[2] + ((corner & 4) ? r : -r)};
    float px, py, t;
    if (!ProjectToPixel(rows, tan_x, tan_y, float(width), float(height), p, kScissorNear, &px, &py, &t)) {
      ++behind;
      continue;
    }
    min_x = std::min(min_x, px);
    max_x = std::max(max_x, px);
    min_y = std::min(min_y, py);
    max_y = std::max(max_y, py);
  }
  if (behind == 8) {
    return PixelRect{};  // the whole box is behind the camera
  }
  if (behind != 0) {
    return full;
  }
  PixelRect rect;
  const auto clamp_i = [](float v, int32_t hi) { return int32_t(std::clamp(v, 0.0f, float(hi))); };
  rect.x0 = clamp_i(std::floor(min_x) - float(kScissorMargin), int32_t(width));
  rect.y0 = clamp_i(std::floor(min_y) - float(kScissorMargin), int32_t(height));
  rect.x1 = clamp_i(std::ceil(max_x) + float(kScissorMargin), int32_t(width));
  rect.y1 = clamp_i(std::ceil(max_y) + float(kScissorMargin), int32_t(height));
  return rect;
}

// ---------------------------------------------------------------------------
// the buffer
// ---------------------------------------------------------------------------

struct PassInputs {
  Rows rows;
  float tan_x = 1.0f, tan_y = 1.0f;
  float lin[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  uint32_t width = 0, height = 0;
  Units units;
  uint32_t flags = 0;  // kFlag*
  // GB-H2: the frame's shadow views and atlas as of this trigger (the replay's maps of THIS frame, recorded just before the pass), null: none. kFlagShadow is set by BuildBuffer, not by the caller.
  const ShadowFrame* shadow = nullptr;
  // LAMP-SHADOWS-4: the street lamps' views of this trigger (lamp_shadows.h: recorded just before the pass), null: none. kFlagLampShadow is set by BuildBuffer, not by the caller.
  const LampShadowFrame* lamp_shadow = nullptr;
  uint32_t debug = 0;  // kDebug*
  // RT-LIGHTS-D1: the ray traced visibility's images of this trigger (rt_lights.h: traced just before the pass), null: none. kFlagRtMask is set by BuildBuffer, not by the caller.
  const rt_lights::MaskFrame* rt_mask = nullptr;
};

inline void FillHeader(PassHeader* h, const PassInputs& in, uint32_t count) {
  std::memset(h, 0, sizeof(*h));
  for (int i = 0; i < 3; ++i) {
    h->right[i] = in.rows.right[i];
    h->up[i] = in.rows.up[i];
    h->forward[i] = in.rows.forward[i];
    h->position[i] = in.rows.position[i];
  }
  h->tan_x = in.tan_x;
  h->tan_y = in.tan_y;
  h->gain = in.units.gain;
  h->exposure = in.units.exposure;
  h->night = in.units.night;
  for (int i = 0; i < 4; ++i) {
    h->lin[i] = in.lin[i];
    h->occ[i] = in.units.occ[i];
  }
  h->size[0] = float(in.width);
  h->size[1] = float(in.height);
  h->size[2] = in.width != 0 ? 1.0f / float(in.width) : 0.0f;
  h->size[3] = in.height != 0 ? 1.0f / float(in.height) : 0.0f;
  h->count = count;
  h->flags = in.flags | ((in.units.occ[3] > 0.0f) ? kFlagOcclusion : 0u);
  h->debug = in.debug;
}

// Whether the shadow table is worth writing and reading: the frame has an atlas the pass can name (a valid atlas with a bindless slot) and at least one selected light has views in it.
// A light whose views are not in the table (first_view past the count) is lit by the shader, so a stale ref never reads past the table.
inline bool ShadowTableNeeded(const Selection& selection, const ShadowFrame* shadow) {
  if (shadow == nullptr || !shadow->atlas.valid || shadow->atlas.slot == kNoLightSlot || shadow->count == 0) {
    return false;
  }
  for (uint32_t i = 0; i < selection.count && i < kMaxPassLights; ++i) {
    const ShadowRef& ref = selection.lights[i].shadow;
    if (ref.kind == ShadowKind::kAtlas && ref.view_count != 0 && uint32_t(ref.first_view) < shadow->count) {
      return true;
    }
  }
  return false;
}

// LAMP-SHADOWS-4: whether the lamps' table is worth writing and reading: the frame has a valid lamp atlas the pass can name and at least one selected street lamp names entries of it (a ref past the
// table's count is lit by the shader, so a stale ref never reads past it).
inline bool LampTableNeeded(const Selection& selection, const LampShadowFrame* lamps) {
  if (lamps == nullptr || !lamps->atlas.valid || lamps->atlas.slot == kNoLightSlot || lamps->count == 0) {
    return false;
  }
  for (uint32_t i = 0; i < selection.count && i < kMaxPassLights; ++i) {
    const Light& l = selection.lights[i];
    const ShadowRef& ref = l.shadow;
    if (l.source == LightSource::kLamp && ref.kind == ShadowKind::kAtlas && ref.view_count != 0 && uint32_t(ref.first_view) >= kLampEntryBase &&
        uint32_t(ref.first_view) - kLampEntryBase < lamps->count) {
      return true;
    }
  }
  return false;
}

// RT-LIGHTS-D1: whether the ray traced table is worth writing and reading: the frame has valid mask images and at least one selected light reads a channel of them.
inline bool RtTableNeeded(const Selection& selection, const rt_lights::MaskFrame* rt_mask) {
  if (rt_mask == nullptr || !rt_lights::MaskTableValid(rt_lights::BuildMaskTable(*rt_mask))) {
    return false;
  }
  for (uint32_t i = 0; i < selection.count && i < kMaxPassLights; ++i) {
    const Light& l = selection.lights[i];
    if ((l.flags & kLightFlagRtVisibility) != 0 && uint32_t(l.shadow.mask_channel) < rt_mask->images * rt_lights::kChannelsPerImage) {
      return true;
    }
  }
  return false;
}

// Writes the whole buffer (kBufferAlloc bytes: the header, the lights' rows, zero past the count, then the shadow view table when a selected light reads it: kFlagShadow, then the street lamps' table when a selected lamp does: kFlagLampShadow, then the ray traced visibility's table when a selected light reads a channel: kFlagRtMask).
inline void BuildBuffer(const PassInputs& in, const Selection& selection, uint8_t* out) {
  std::memset(out, 0, kBufferAlloc);
  PassInputs with = in;
  const bool shadowed = ShadowTableNeeded(selection, in.shadow);
  const bool lamps = LampTableNeeded(selection, in.lamp_shadow);
  const bool rt = RtTableNeeded(selection, in.rt_mask);
  with.flags = in.flags | (shadowed ? kFlagShadow : 0u) | (lamps ? kFlagLampShadow : 0u) | (rt ? kFlagRtMask : 0u);
  PassHeader header;
  FillHeader(&header, with, selection.count);
  std::memcpy(out, &header, sizeof(header));
  for (uint32_t i = 0; i < selection.count && i < kMaxPassLights; ++i) {
    const GpuLight row = PackGpuLight(selection.lights[i]);
    std::memcpy(out + kLightsOffset + size_t(i) * sizeof(GpuLight), &row, sizeof(row));
  }
  if (shadowed) {
    shadow_lookup::GpuShadowBuffer table;
    shadow_lookup::BuildGpuShadowBuffer(*in.shadow, &table);
    std::memcpy(out + kShadowOffset, &table, sizeof(table));
  }
  if (lamps) {
    GpuLampTable table;
    BuildGpuLampTable(*in.lamp_shadow, &table);
    std::memcpy(out + kLampOffset, &table, sizeof(table));
  }
  if (rt) {
    const rt_lights::GpuRtMaskTable table = rt_lights::BuildMaskTable(*in.rt_mask);
    std::memcpy(out + kRtOffset, &table, sizeof(table));
  }
}

}  // namespace larecomp::gpu::local_lights
