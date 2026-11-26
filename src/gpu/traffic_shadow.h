#pragma once
// ===========================================================================
// MCLA -- traffic and AI cars in the sun's shadow cascades (the TRAFFIC SHADOWS setting)
// ===========================================================================
// Owner, 2026-11-13: everything under a TRAFFIC car should be as dark as it is under the player's car.
//
// Why it is not [guest code, read from the recompiled functions; CHECK in a run]: Vehicle::Draw (0x82360518,
// the draw of every vehicle: world, garage, reflection) draws a vehicle into the four sun shadow cascades
// (render phases 0x20, 0x40, 0x80, 0x100; 0x1000 is the shadow collector that reads the atlas) only when
// the vehicle's byte +594 is set (0x82360668: `lbz r11,594(r30)`, `cmplwi cr6,r11,0`, `beq` over the call of
// sub_8235FA90, the routine that draws a car at the forced low LOD into the current phase). The constructor
// (0x8235AF90) writes the two bytes from its 4th argument (r6, the vehicle's slot):
//     +593 = (slot != -1)      the car takes part in the side views and the shadow phases at all
//     +594 = (slot ==  0)      the car casts into the sun shadow map
// so the player's car (slot 0) is the one vehicle the game ever draws into the cascades; traffic and the
// opponents (+593 = 1, +594 = 0) get only the game's soft quad decal under them. Nothing else in the draw
// reads +594 on the cascade path (sub_8235FA90 and everything it calls do not), the car materials have the
// cascade technique (VS_CarShadowDepthLocal, 27 car groups, no pixel shader), and sub_8235FA90 is already
// called for EVERY vehicle in the rain phase 0x10, so the routine copes with any car's model.
//
// CORRECTION (the first run, 2026-11-13: no car ever reached the selector): the routine above draws the player's
// car and the RACERS' cars only; traffic has its own renderer client and item draw, see "the traffic's own
// route" below. The hook at 0x8236066C stays for the opponents of a race.
//
// CORRECTION 2 (the second run, 2026-11-13: the traffic route was reached, 20 to 58 cars a frame passed every test, but
// the nearest was always 97 to 270 m away and none cast; the first `gates:` line, which showed every item with a negative
// LOD, was printed 0.6 s after the world's first frame and says nothing about the steady state). Two findings, both read
// from the recompiled code (see "the traffic's own route"):
//   - the distance was measured from the wrong eye: [0x82839704] is the CURRENT viewport, which the game swaps for every
//     view it renders (sub_8217C658 = SetCurrentViewport, called around each sun cascade by sub_823083F8 and put back at the
//     end of sub_823115E8), so in a cascade phase +112 is the cascade's own eye, about 100 m (and at the start 220 to
//     270 m) from the cars. The main view's viewport is [[0x8287E064] + 180 + 4 * buffer] (the game's own lookup in
//     sub_827902F0 and the one sub_823115E8 restores): both routes now measure from its eye.
//   - the phase the draw was told, 0x8000, is the car's LIGHTS (sub_82790B08 -> sub_8237AC90 loops over the car's light
//     points and submits their glows, AddGlow among them, with a per-light slot history), not its body: it would have
//     added glows four times a frame and drawn no shadow caster. The body is drawn by the 0x10000 / 0x100000 branch
//     (model vtbl+28, sub_827914B8, which reads the renderer's current phase [[0x8287E064]+356] and takes its plain path
//     for a phase that is neither: in a cascade). 0x10000 also recomputes the LOD and makes the car's own shadow decal;
//     0x100000 does neither, so the draw is told 0x100000, which returns for LOD 2 and for a negative LOD (no exemption)
//     and does not look at the visible byte.
//
// What the hook does (hooks/traffic_shadow.cpp, 0x8236066C): for a vehicle that the game would leave out
// of a cascade (r11 = 0 where the player's car has 1) it answers 1, when the car is one this file picks.
// Nothing is written into the vehicle, so the +594 readers elsewhere (the vehicle update, the colour
// pass) see what they always saw, and the setting is live: the next cascade draw obeys it.
//
// Which cars (Selector): the cars within `radius` metres of the camera and, of those, the nearest `cars`.
// The census of the cars comes from the cascade draw itself: in a frame's first cascade phase every
// candidate car passes the hook once, so the distances of that frame give the cutoff the NEXT frame casts
// by (a car casts when its distance is at most the cutoff, which sits halfway between the Nth and the
// (N+1)th nearest, or at the radius when there are no more than N). One frame late is invisible: cars move
// a few metres a second, and a car that is new has no history but a distance. Only cars the main view
// sees are candidates (the game's own visibility word +116, which sub_8235FA90 tests anyway: a car it
// would not draw must not take a place), so the cars behind the camera never use up the count.
//
// What it costs [I, to be measured: chain-trafficcascade.ps1]: a selected car draws in each cascade it
// can reach at the forced LOD (a body and four wheels at the low model, tens of draws, not the 210 of the
// player's car at full detail), with the guest's own CPU cost of a car draw on the render thread and the
// host's draw capture. ExtraDraws() is the rule of thumb the log line prints.
//
// What the renderer does with it (ao.cpp, ao_contact_ps.hlsl): the contact shadows' ground-under-a-car term
// (contact_trace.h UnderVehicle, the screen-space approximation made when only the player's car cast) stands
// aside for a car that now casts: the hook publishes the cutoff and the term leaves out the car whose shell
// is nearer than it (Publish / CastDistance, UnderCastWeight). Far cars, beyond the cutoff, keep it. The player's car casts whatever the
// setting says, so the distance the renderer uses is never under contact_trace.h kPlayerCarCastDistance (UnderCastDistance).
//
// Everything that decides something is here, pure (no ReXGlue, no renderer): traffic_shadow_test.cpp checks it.
// ===========================================================================

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string_view>
#include <type_traits>

#include "gfx_preset.h"

namespace mcla::gpu::traffic_shadow {

enum class Mode : uint8_t {
  kOff,   // the game's own picture: only the player's car casts into the sun shadow map
  kNear,  // the nearest few cars within a short radius
  kAll,   // every car within a long radius, up to a larger count
};

constexpr std::string_view ModeName(Mode mode) {
  switch (mode) {
    case Mode::kNear:
      return "near";
    case Mode::kAll:
      return "all";
    default:
      return "off";
  }
}

// The setting's text after `auto` has been resolved (gpu_class.h): near and all; anything else is off.
inline Mode ParseMode(std::string_view resolved) {
  if (resolved == "near") {
    return Mode::kNear;
  }
  if (resolved == "all") {
    return Mode::kAll;
  }
  return Mode::kOff;
}

// Legacy auto follows the Enhanced preset on all devices.
inline Mode ModeFor(std::string_view setting_text) {
  constexpr auto enhanced = gfx_preset::CellAt(gfx_preset::MustFindRow("mcla_gpu_traffic_shadows"),
                                               gfx_preset::Preset::kEnhanced);
  return ParseMode(setting_text == "auto" ? enhanced : setting_text);
}

// --- the render phases --------------------------------------------------------------------------------------

// The sun shadow cascades, nearest first [guest: sub_823120C8 / the renderer's phase list; the order is CHECK].
inline constexpr uint32_t kCascadePhases[4] = {0x20u, 0x40u, 0x80u, 0x100u};

// 1 to 4 for a cascade phase, 0 for anything else (the collector 0x1000 included: a car is not added to it).
constexpr int CascadeIndex(uint32_t phase) {
  for (int i = 0; i < 4; ++i) {
    if (phase == kCascadePhases[i]) {
      return i + 1;
    }
  }
  return 0;
}

// Receiver far edges in metres. Nearer receivers share larger cascades, so
// there is no lower distance cutoff. A car length pads the far edge.
inline constexpr std::array<float, 4> kCascadeFar = {15.0f, 31.0f, 80.0f, 160.0f};
inline constexpr float kCarLengthPadding = 6.0f;
inline constexpr float kOpponentLengthPadding = 12.0f;

inline bool BeyondCascadeFar(int cascade, float distance, float length, float far_scale = 1.0f) {
  return cascade >= 1 && cascade <= int(kCascadeFar.size()) && std::isfinite(distance) &&
         std::isfinite(length) && length > 0.0f && std::isfinite(far_scale) && far_scale > 0.0f &&
         distance > kCascadeFar[cascade - 1] * std::max(1.0f, far_scale) + length;
}

// The U and V columns of the guest's world-to-atlas matrix. The other
// columns carry depth and its scale.
using AtlasMatrixUV = std::array<std::array<float, 2>, 4>;

struct CascadeReach {
  std::array<float, 3> u{}, v{};
  float offset_u = 0.0f, offset_v = 0.0f;
  float scale_u = 0.0f, scale_v = 0.0f;
  bool valid = false;

  bool MissesTile(const float (&at)[3], float length) const {
    if (!valid || !std::isfinite(length) || !(length > 0.0f)) return false;
    // Project the car's bounding sphere. Its entire silhouette must miss
    // the tile, even for sun rays pointing back towards the camera.
    const float x = at[0] * u[0] + at[1] * u[1] + at[2] * u[2] + offset_u;
    const float y = at[0] * v[0] + at[1] * v[1] + at[2] * v[2] + offset_v;
    constexpr float kAtlasSlop = 1.0f / 1280.0f;
    return std::isfinite(x) && std::isfinite(y) &&
           (std::abs(x) > 0.25f + scale_u * length + kAtlasSlop ||
            std::abs(y) > 0.25f + scale_v * length + kAtlasSlop);
  }
};

inline CascadeReach MakeCascadeReach(int cascade, const AtlasMatrixUV& matrix, const float (&eye)[3]) {
  if (cascade < 1 || cascade > int(kCascadeFar.size())) return {};
  // The guest resolves to (0,0), (0,640), (640,0), (640,640).
  const float centre_u = (cascade - 1) & 2 ? 0.75f : 0.25f;
  const float centre_v = (cascade - 1) & 1 ? 0.75f : 0.25f;
  CascadeReach reach;
  for (int i = 0; i < 3; ++i) {
    reach.u[i] = matrix[i][0];
    reach.v[i] = matrix[i][1];
  }
  reach.scale_u = std::hypot(reach.u[0], reach.u[1], reach.u[2]);
  reach.scale_v = std::hypot(reach.v[0], reach.v[1], reach.v[2]);
  if (!(reach.scale_u > 0.0f && std::isfinite(reach.scale_u) &&
        reach.scale_v > 0.0f && std::isfinite(reach.scale_v))) return {};
  float axis_dot = 0.0f;
  for (int i = 0; i < 3; ++i) axis_dot += (reach.u[i] / reach.scale_u) * (reach.v[i] / reach.scale_v);
  if (!(std::abs(axis_dot) < 0.01f)) return {};
  const float eye_u = eye[0] * reach.u[0] + eye[1] * reach.u[1] + eye[2] * reach.u[2] + matrix[3][0];
  const float eye_v = eye[0] * reach.v[0] + eye[1] * reach.v[1] + eye[2] * reach.v[2] + matrix[3][1];
  // The view eye lies inside the fitted cone and must map into this tile.
  if (!(std::abs(eye_u - centre_u) <= 0.25f && std::abs(eye_v - centre_v) <= 0.25f)) return {};
  reach.offset_u = matrix[3][0] - centre_u;
  reach.offset_v = matrix[3][1] - centre_v;
  reach.valid = true;
  return reach;
}

// The current viewport is needed for fallback and for the census that compares the two eyes.
constexpr bool NeedsCurrentEye(bool have_main, bool census, bool skip_unused) {
  return !have_main || census || !skip_unused;
}

// --- which cars ---------------------------------------------------------------------------------------------

inline constexpr float kNearRadius = 40.0f;  // m: `near` reaches this far from the camera ...
inline constexpr uint32_t kNearCars = 4;     // ... and takes at most this many cars
inline constexpr float kAllRadius = 150.0f;  // m: `all`
inline constexpr uint32_t kAllCars = 16;
inline constexpr uint32_t kMaxCars = 24;     // the most any setting may take (the census table is bigger: 32)

struct Limits {
  float radius = 0.0f;     // m from the camera; 0 or less: no car casts
  uint32_t cars = 0;       // the most cars that cast at once
  uint32_t cascades = 4;   // the cars are drawn into the first this many cascades (4: all)
};

// The limits of a mode with the debug overrides applied (0 or less keeps the mode's own value).
inline Limits LimitsFor(Mode mode, double radius_override, int cars_override, int cascades_override) {
  Limits limits;
  if (mode == Mode::kOff) {
    return limits;
  }
  limits.radius = mode == Mode::kNear ? kNearRadius : kAllRadius;
  limits.cars = mode == Mode::kNear ? kNearCars : kAllCars;
  if (radius_override > 0.0) {
    limits.radius = float(std::min(radius_override, 1000.0));
  }
  if (cars_override > 0) {
    limits.cars = uint32_t(std::min<int>(cars_override, int(kMaxCars)));
  }
  if (cascades_override >= 1 && cascades_override <= 4) {
    limits.cascades = uint32_t(cascades_override);
  }
  return limits;
}

// The guest's bytes of a vehicle that decide whether it is a candidate (offsets in hooks/traffic_shadow.cpp).
struct VehicleFlags {
  uint8_t casts_in_game = 0;  // +594: the game draws it into the cascades already (the player's car)
  uint8_t side_views = 0;     // +593: the vehicle takes part in the shadow phases at all
  uint8_t hidden = 0;         // +6478: the game's own "do not draw" byte, tested in the same condition
  uint8_t disabled = 0;       // +6467: sub_8235FA90 draws nothing for it
  uint8_t ghost = 0;          // +123: likewise
  uint16_t visible = 0;       // +116: seen by the main view (the visibility pass of this or the last frame)
  uint16_t enabled = 0;       // +120: likewise
};

// A car this file may add: the game leaves it out (+594 clear) although it is a shadow-casting kind
// (+593 set), and sub_8235FA90 would draw it (the tests it makes at its start, so that a car it would
// skip does not take one of the places).
constexpr bool Eligible(const VehicleFlags& f) {
  return f.casts_in_game == 0 && f.side_views != 0 && f.hidden == 0 && f.disabled == 0 && f.ghost == 0 &&
         f.visible != 0 && f.enabled != 0;
}

// The flags that failed, one bit each, for the gate counters (traffic_shadow_gates.h); 0 is Eligible().
enum VehicleReject : uint32_t {
  kRejectSideViews = 1u << 0,
  kRejectHidden = 1u << 1,
  kRejectDisabled = 1u << 2,
  kRejectGhost = 1u << 3,
  kRejectNotVisible = 1u << 4,
  kRejectNotEnabled = 1u << 5,
};

constexpr uint32_t VehicleRejectBits(const VehicleFlags& f) {
  return (f.side_views == 0 ? kRejectSideViews : 0u) | (f.hidden != 0 ? kRejectHidden : 0u) |
         (f.disabled != 0 ? kRejectDisabled : 0u) | (f.ghost != 0 ? kRejectGhost : 0u) |
         (f.visible == 0 ? kRejectNotVisible : 0u) | (f.enabled == 0 ? kRejectNotEnabled : 0u);
}

// --- the traffic's own route: mcAmbientCarRenderClass ---------------------------------------------------------
//
// [guest, read from default.xex's data and the recompiled code 2026-11-13] Traffic is NOT drawn by Vehicle::Draw
// (sub_82360518 above): that routine is the item draw of the renderer's client in slot 7 (mcCarModelClass, mask
// 0x12E183F3: the player's and the racers' cars; the shadow collector 0x1000 is not in its mask either). Traffic
// cars are the items of slot 9, mcAmbientCarRenderClass: its object's vtable is 0x820D8C0C, its phase mask
// 0x01118002 (0x2 0x8000 0x10000 0x100000 and the mirror's 0x1000000) has no sun cascade, so the phase loop
// (sub_822E6408, 0x822E6870: `and r10,r11,r30` with r11 = the client's mask, r30 = the phase) never calls it in
// one, and its item draw (sub_82793360: r3 = the item, r4 = the phase) would draw nothing in a phase it does not
// know. That is why the hook in Vehicle::Draw never saw a traffic car (in free roam it sees the player's car only,
// with +594 set, and returns at its first test).
//
// The route: the phase loop's mask test is widened by the cascade phase for this one client (nothing is written
// into the client), and the item draw is told the phase is kAmbientBodyPhase for the cars this file picks. That
// phase's branch of the item draw is the body draw (the model's vtbl+28, sub_827914B8, with the item's own LOD and
// the per-buffer data [[item+24] + (buffer+2)*4]); the model's draw reads the renderer's current phase
// ([[0x8287E064]+356], the cascade's own), so it takes the plain path with the technique the cascade picks (every
// ambient car shader group has the cascade technique, VS_ShadowDepthLocal, from the shader pack's report). The branch
// is the game's own colour-pass draw minus what the LOD phase (0x10000) adds, and what it overwrites it overwrites the
// same way in the car's own pass later in the frame [I: read from the code, never seen running]: the state pushes it
// makes (sub_8217FED0 / sub_8217F768, ids 20 24 23 25) are popped by the branch itself (sub_82491668), item +512..+520
// are set to 0.0, +576 (the object's lighting, sub_822F57A0) is recomputed and +40 is restamped from a global
// ([0x827D7500+44]) every draw. The hook saves and restores nothing because it changes r4 (a register) only.
//
// The LOD and the other fields the draw tests are kept by the item, not by a phase: item +28 (LOD) and +556 bit 0x20
// (near) by the LOD update sub_827932D8 (item vtbl+44; sub_82793360 calls it in phase 0x10000 for the main view and in
// 0x1000000 for the mirror, so a cascade sees the last frame's: the cascades come before 0x10000 in the loop), item
// +32 = the view depth + radius that update got from the main viewport's frustum test (sub_8217C1D8; a negative LOD
// is "outside the frustum", the occlusion query said no, or deeper than 500 m), +556 bit 0x80 (the lights object at
// +528 exists; built in the item's update sub_82792E00 through sub_82792A28), +557 + buffer (bit 4: lights, by
// sub_826E6D90 in the car's update sub_826E8EC8; it gates the phases 0x2 and 0x8000 only and is NOT tested here).
// AmbientRejectBits repeats the body phase's own tests so that a car the draw would not draw does not take a place.
//
// Where a cascade measures from: the game keeps the viewport it is rendering in [0x82839704] (the CURRENT viewport; the
// sun cascades swap their own in), and the main view's one for the render buffer at [renderer + 180 + 4 * buffer]
// (renderer = [0x8287E064], buffer = [0x827E5640]); a viewport's +112 is its eye (three floats), as sub_82793360 and
// sub_827914B8 read it. In a cascade the census measures from the main viewport's eye, and compares it with the item's
// own view depth (+32) to say whether the position source is right.

inline constexpr uint32_t kRendererGlobal = 0x8287E064u;           // [guest] u32: the renderer object (rear_view.cpp's kRendererPtr)
inline constexpr uint32_t kRendererMainViewports = 180u;            // [guest] renderer + this + 4 * buffer: the main viewport of the render buffer
inline constexpr uint32_t kCurrentViewportGlobal = 0x82839704u;     // [guest] u32: the viewport being rendered in
inline constexpr uint32_t kViewportEye = 112u;                      // [guest] a viewport's eye: three floats

constexpr uint32_t MainViewportSlot(uint32_t buffer) { return kRendererMainViewports + 4u * buffer; }

inline constexpr uint32_t kCascadeMask = 0x20u | 0x40u | 0x80u | 0x100u;
inline constexpr uint32_t kAmbientClientVtable = 0x820D8C0Cu;  // the client's object: [client] == this
// [guest] The branch of the item draw that draws the car's body without side effects: sub_82793360 with r4 = 0x100000
// returns for LOD 2 and a negative LOD, checks the item and the car, makes the shader-variable pushes it pops again, sets
// the object's lighting and calls the model's draw (vtbl+28). 0x8000 (the first version) is the lights, 0x10000 recomputes
// the LOD and creates the shadow decal, 0x1000000 is the mirror's and recomputes the LOD for the mirror.
inline constexpr uint32_t kAmbientBodyPhase = 0x100000u;
inline constexpr uint32_t kAmbientCoarsestLod = 2;               // [guest] the body phase returns for this LOD
inline constexpr uint32_t kAmbientBufferGlobal = 0x827E5640u;    // [guest] u32: the render buffer: which copy of the item's state the render thread reads

// The phase loop's mask test for the traffic client: its own mask, plus the phase when it is a cascade and the
// setting is on.
constexpr uint32_t WidenedClientMask(uint32_t mask, uint32_t phase, bool on) {
  return (on && CascadeIndex(phase) != 0) ? (mask | phase) : mask;
}

// What the item draw reads of an item (offsets in hooks/traffic_shadow.cpp) for the tests the body phase makes before
// the model's draw.
struct AmbientFlags {
  uint8_t enabled = 0;       // item +36: 0 = the draw returns
  uint8_t car_flags = 0;     // [item+16] +500: bit 0x40 must be set, bit 0x20 (a child) and bit 0x80 clear
  int32_t model_count = 0;   // [[item+24]+32]: the draw wants at least 3
  int32_t lod = 0;           // item +28: negative = outside the main view (or too far), 2 = the coarsest: neither is drawn
};

enum AmbientReject : uint32_t {
  kAmbientRejectEnabled = 1u << 0,
  kAmbientRejectCarFlags = 1u << 1,
  kAmbientRejectModels = 1u << 2,
  kAmbientRejectLod = 1u << 3,        // negative
  kAmbientRejectLodCoarse = 1u << 4,  // the coarsest: the body phase returns for it
};

constexpr uint32_t AmbientRejectBits(const AmbientFlags& f) {
  const bool car_ok = (f.car_flags & 0x40u) != 0 && (f.car_flags & 0x20u) == 0 && (f.car_flags & 0x80u) == 0;
  return (f.enabled == 0 ? kAmbientRejectEnabled : 0u) | (car_ok ? 0u : kAmbientRejectCarFlags) |
         (f.model_count < 3 ? kAmbientRejectModels : 0u) | (f.lod < 0 ? kAmbientRejectLod : 0u) |
         (f.lod == int32_t(kAmbientCoarsestLod) ? kAmbientRejectLodCoarse : 0u);
}

constexpr bool AmbientEligible(const AmbientFlags& f) { return AmbientRejectBits(f) == 0; }

// The item's own measure of the same thing, against ours [I: that plane 0 of the frustum test is the near plane, so that
// item +32 is the depth along the view axis plus the car's radius; the game compares it with 500 m and with the model's
// LOD distances, so it is a depth, but which plane is read from the vector code only]. A car in the main view's
// frustum is at least its depth from the eye and, in any field of view the game has, at most about twice that
// (a 120 degree view: 1 / cos 60); an eye or a position that is wrong shows as a distance outside that band.
enum class DepthVerdict : uint8_t {
  kUnknown,           // no usable depth (not finite, or the car is behind the near plane)
  kConsistent,        // the distance is the depth of a car in the view, within the band
  kCloserThanDepth,   // the distance is shorter than the depth: the eye or the position is wrong
  kFarBeyondView,     // the distance is much longer than the depth allows: the eye or the position is wrong
};

constexpr const char* DepthVerdictName(DepthVerdict v) {
  switch (v) {
    case DepthVerdict::kConsistent: return "consistent";
    case DepthVerdict::kCloserThanDepth: return "closer than the depth";
    case DepthVerdict::kFarBeyondView: return "beyond the view";
    default: return "unknown";
  }
}

inline DepthVerdict CheckAgainstDepth(float distance, float depth_plus_radius, float radius) {
  if (!std::isfinite(distance) || !std::isfinite(depth_plus_radius) || !std::isfinite(radius)) {
    return DepthVerdict::kUnknown;
  }
  const float depth = depth_plus_radius - std::max(radius, 0.0f);  // the centre's depth along the view axis
  if (!(depth > 0.5f)) {
    return DepthVerdict::kUnknown;  // at or behind the eye's plane: no band to compare with
  }
  if (distance + 1.0f < depth) {
    return DepthVerdict::kCloserThanDepth;
  }
  if (distance > 2.5f * depth + 2.0f * std::max(radius, 0.0f) + 5.0f) {
    return DepthVerdict::kFarBeyondView;
  }
  return DepthVerdict::kConsistent;
}

// The distance bands of the census (metres from the main viewport's eye): <20, <40 (the `near` radius), <80, <160, more.
inline constexpr int kDistanceBands = 5;
constexpr int DistanceBand(float distance) {
  if (!(distance >= 0.0f)) {
    return kDistanceBands - 1;  // NaN and negative: counted with the far ones
  }
  return distance < 20.0f ? 0 : distance < 40.0f ? 1 : distance < 80.0f ? 2 : distance < 160.0f ? 3 : 4;
}
constexpr const char* DistanceBandName(int band) {
  switch (band) {
    case 0: return "<20 m";
    case 1: return "<40 m";
    case 2: return "<80 m";
    case 3: return "<160 m";
    default: return ">=160 m";
  }
}

// The car's world matrix, as sub_82794EE8 finds it for a car that is not a child: on the render thread (the
// thread flag [[r13]+72] set) at ((buffer + 4) rotated left by 6) & ~63 from the car, else at +128; its
// translation is the row at +48. `buffer` is [kAmbientBufferGlobal]; anything above 3 is not a buffer index.
constexpr bool AmbientBufferValid(uint32_t buffer) { return buffer <= 3u; }
constexpr uint32_t AmbientMatrixOffset(bool render_copy, uint32_t buffer) {
  const uint32_t n = buffer + 4u;
  return render_copy ? (((n << 6) | (n >> 26)) & 0xFFFFFFC0u) : 128u;
}
constexpr uint32_t kAmbientTranslation = 48u;

// A position read out of guest memory that is a position: three finite numbers, none beyond a million metres.
inline bool PlausiblePosition(const float p[3]) {
  for (int i = 0; i < 3; ++i) {
    if (!std::isfinite(p[i]) || std::fabs(p[i]) > 1.0e6f) {
      return false;
    }
  }
  return true;
}

inline float Distance(const float camera[3], const float position[3]) {
  const float dx = camera[0] - position[0];
  const float dy = camera[1] - position[1];
  const float dz = camera[2] - position[2];
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// The distance up to which a car casts, from the distances of the frame's candidates (any order; NaN counts as
// far away): the radius when no more than `cars` of them are within reach, else halfway between the Nth and
// the (N+1)th nearest (or the radius if that is nearer): the nearest N cast and the rest do not, whatever
// their order of arrival.
inline float CutoffFor(const float* distances, uint32_t count, const Limits& limits, bool select = false) {
  if (!(limits.radius > 0.0f) || limits.cars == 0) {
    return 0.0f;
  }
  constexpr uint32_t kCapacity = 32;
  float sorted[kCapacity];
  uint32_t n = 0;
  for (uint32_t i = 0; i < count && n < kCapacity; ++i) {
    const float d = distances[i];
    // Only the cars within the radius compete for the places.
    if (std::isfinite(d) && d <= limits.radius) {
      sorted[n++] = d;
    }
  }
  if (n <= limits.cars) {
    return limits.radius;
  }
  float last_caster;
  if (select) {
    float* first_rejected = sorted + limits.cars;
    std::nth_element(sorted, first_rejected, sorted + n);
    last_caster = *std::max_element(sorted, first_rejected);
  } else {
    std::sort(sorted, sorted + n);
    last_caster = sorted[limits.cars - 1];
  }
  const float cut = 0.5f * (last_caster + sorted[limits.cars]);
  return std::min(limits.radius, cut);
}

// What the frame did, for the log.
struct FrameStats {
  uint32_t candidates = 0;       // eligible cars the first cascade phase saw
  uint32_t casting = 0;          // of them the ones that cast
  float nearest = 0.0f;          // m, the nearest candidate (0 with none)
  float farthest_cast = 0.0f;    // m, the farthest car that cast (0 with none)
  float cutoff = 0.0f;           // m, the cutoff those cars were chosen by
  uint64_t frame = 0;            // frames counted since the start (boundaries seen)
};

// The census and the decision. Single threaded (the guest's render thread, the one that draws the phases).
// Decide() is called once per candidate car in every cascade phase of a frame, nearest cascade first; a frame
// ends (and the next cutoff is made) when a phase comes that is not above the last one's, or when the
// frame's census phase sees a car it has seen already (a frame that has one phase only).
class Selector {
 public:
  static constexpr uint32_t kCapacity = 32;

  // Whether the car (any id that is stable and different per car: its guest address) casts in `phase` now.
  // `distance` is its distance from the camera in metres. A phase that is not a cascade does not cast.
  template <typename UseSelection = std::false_type>
  bool Decide(uint32_t id, uint32_t phase, float distance, const Limits& limits, UseSelection use_selection = {}) {
    const int cascade = CascadeIndex(phase);
    if (cascade == 0) {
      return false;
    }
    if (phase < last_phase_ || (phase == census_phase_ && Seen(id))) {
      EndFrame(limits, use_selection());
    }
    last_phase_ = phase;
    if (census_phase_ == 0) {
      census_phase_ = phase;
    }
    const float reach = cutoff_ < 0.0f ? limits.radius : std::min(cutoff_, limits.radius);
    const bool cast = limits.radius > 0.0f && limits.cars > 0 && cascade <= int(limits.cascades) &&
                      std::isfinite(distance) && distance <= reach;
    if (phase == census_phase_) {
      if (count_ < kCapacity) {
        ids_[count_] = id;
        distances_[count_] = distance;
        ++count_;
      }
      ++candidates_;
      nearest_ = candidates_ == 1 ? distance : std::min(nearest_, distance);
      if (cast) {
        ++casting_;
        farthest_cast_ = std::max(farthest_cast_, distance);
      }
    }
    return cast;
  }

  // Closes the frame in progress (the next one casts by its census). Public for the caller that knows a frame
  // ended (a mode change or the game leaving the world: everything is forgotten).
  void EndFrame(const Limits& limits, bool select = false) {
    cutoff_ = CutoffFor(distances_, count_, limits, select);
    stats_.candidates = candidates_;
    stats_.casting = casting_;
    stats_.nearest = candidates_ ? nearest_ : 0.0f;
    stats_.farthest_cast = farthest_cast_;
    stats_.cutoff = cutoff_;
    ++stats_.frame;
    count_ = 0;
    candidates_ = 0;
    casting_ = 0;
    nearest_ = 0.0f;
    farthest_cast_ = 0.0f;
    census_phase_ = 0;
    last_phase_ = 0;
  }

  // Forget everything (the setting went off, or came on after a pause).
  void Reset() { *this = Selector(); }

  // The cutoff in force for this frame, in metres: negative before the first frame has been counted.
  float cutoff() const { return cutoff_; }
  const FrameStats& last() const { return stats_; }

 private:
  bool Seen(uint32_t id) const {
    for (uint32_t i = 0; i < count_; ++i) {
      if (ids_[i] == id) {
        return true;
      }
    }
    return false;
  }

  uint32_t ids_[kCapacity] = {};
  float distances_[kCapacity] = {};
  uint32_t count_ = 0;
  uint32_t candidates_ = 0;
  uint32_t casting_ = 0;
  float nearest_ = 0.0f;
  float farthest_cast_ = 0.0f;
  uint32_t last_phase_ = 0;
  uint32_t census_phase_ = 0;  // the phase whose cars are counted: the first one of the frame
  float cutoff_ = -1.0f;
  FrameStats stats_;
};

// --- the cost rule of thumb ---------------------------------------------------------------------------------

// Cascade draws a casting car adds a frame: every cascade it is drawn into times the draws of the car at the
// forced low LOD. kDrawsPerCarPerCascade is a first guess [I] for the body and wheels at LOD 1 (the player's
// car has about 210 draws in the main view at full detail, the whole cascade pass about 1,340 for the world and
// the player's car): the chain counts the real number (cascade draws a frame with the setting off and on,
// divided by the cars that cast).
inline constexpr uint32_t kDrawsPerCarPerCascade = 40;
constexpr uint32_t ExtraDraws(uint32_t casting_cars, uint32_t cascades) {
  return casting_cars * cascades * kDrawsPerCarPerCascade;
}

// --- what the renderer is told ------------------------------------------------------------------------------

// The cutoff, published once a frame by the hook (render thread) and read when the contact pass is recorded.
// 0 while nothing is published or the last publication is older than kStaleNs (the game's cascades stopped:
// night, rain, a menu; the hook stopped: the setting is off): the screen-space term then treats every car as
// one that does not cast.
inline constexpr int64_t kStaleNs = 300'000'000;

struct Published {
  std::atomic<float> cutoff{0.0f};
  std::atomic<int64_t> stamp_ns{0};
};

inline Published& State() {
  static Published state;
  return state;
}

inline int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

inline void Publish(float cutoff_m, int64_t now_ns) {
  Published& s = State();
  s.cutoff.store(cutoff_m > 0.0f && std::isfinite(cutoff_m) ? cutoff_m : 0.0f, std::memory_order_relaxed);
  s.stamp_ns.store(now_ns, std::memory_order_release);
}

// Pure form of CastDistance: the published cutoff if it is fresh, else 0.
constexpr float FreshCutoff(float cutoff_m, int64_t stamp_ns, int64_t now_ns) {
  return (stamp_ns != 0 && now_ns - stamp_ns <= kStaleNs && now_ns >= stamp_ns) ? cutoff_m : 0.0f;
}

// The distance (metres from the camera) within which cars cast into the sun shadow map this frame; 0 for none.
inline float CastDistance() {
  const Published& s = State();
  const int64_t stamp = s.stamp_ns.load(std::memory_order_acquire);
  return FreshCutoff(s.cutoff.load(std::memory_order_relaxed), stamp, NowNs());
}

}  // namespace mcla::gpu::traffic_shadow
