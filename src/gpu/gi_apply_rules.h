#pragma once
// ===========================================================================
// MCLA GPU renderer -- the probe field's APPLY: the rules and the twin of the shader's maths (CITY-RT M3, package G3 of perf/CITY-RT-M3.md sections 4, 6, 9, 10)
// ===========================================================================
// WHAT G3 DOES. At the trigger, right after the ambient occlusion's apply and BEFORE the deferred light pass, one additive full screen draw puts the field's bounce light into the main
// colour:
//
//   E_surf  = GiLookup(P, N)                       the eight probes of the finest cascade that holds the pixel, Chebyshev x back face weights, blended into the coarser cascades (gi_frame.h)
//   E_net   = max(E_surf - E_base - band, 0)       what geometry sends over what the open ground the game's ambient stands for would (GiGroundBaseline), less a small dead band
//   gi      = albedo x CapRadiance(E_net x confidence) x strength x field share x lit weight x AO term      albedo = RT2.rgb squared (the TRUE albedo), lit weight = RT2.a
//   gi      = SoftClip(gi, room)                   the bounce light's CEILING: the light added may fill the pixel up to `cap` times the frame's light and no more
//   colour += gi                                   additive, never darkening
//
// SSGI's RULES, reused (ssgi_light.h): the CAP (a sample's radiance is soft clipped at `cap` x (ambient + sun), never under 0.5: here the net irradiance is), the CEILING (the same number),
// the VEHICLE share (a car's pixels take 0.35 of the strength, at most 0.8) and the SOURCE rejection (the lit weight: an emissive pixel, RT2.a = 0, gets nothing). Not reused: the GAIN LIMIT.
// It guards against amplifying the colour's own noise in a nearly black pixel (gi = colour x B / T with the colour estimated from the picture); here the albedo is the G-buffer's exact value and
// the irradiance is the field's smooth one, so there is no noise to amplify, and a limit relative to an ESTIMATE of the pixel's colour (the apply cannot read the colour it adds to) would cap
// a lamp's pool at night at a few times the ambient-lit colour. The strength, cap and the level's numbers are the BOUNCE LIGHT STRENGTH row's (ssgi_level.h): the owner's tuning stays valid.
//
// THE PIXEL'S OWN COLOUR is not read (the pass adds to the render target: it cannot sample it). The ceiling's room is therefore `cap x (ambient + sun) - luma(albedo) x T` with
// T = ambient + sun x N.L (the light the game's constants say falls on it, without the sun's shadow and without the lamps): a pixel a lamp already fills keeps more room than it has.
// CHECK C8 / G3a: the energy against the SSGI at the same strength on the canyon scene (perf/CITY-RT-M3.md section 10); a dead band or a ceiling from colour_post if it shows.
//
// THE AO'S ORDER. The occlusion's apply multiplies the pixel's colour by its factor first (the ambient's share of the light, contact shadows); this pass runs after it and multiplies its OWN
// term by the occlusion's visibility (the near mask: contact scale, which the probes' 2 m spacing cannot see): lerp(1, ao_seen, saturate(strength x fade)), ao_seen the denoised
// visibility (raised to the paint power on a vehicle's pixel), fade the occlusion's distance fade; the visibility image is the occlusion's own, so the sun share is NOT in it (the bounce is
// all indirect light). Then the light pass adds the lamps' direct light, which neither the occlusion nor the bounce multiplies (gi_frame.h kLightPass).
//
// THE SSGI SWITCH. Where the field stands the colour chain's gather is not built (SsgiShareFor = 0: the AO plans the plain occlusion, no colour capture, no ring), where it does not (invalid,
// warming up after a cut, off) SSGI runs as ever (share 1), and between them (the fade, 0.4 s in, 0.5 s out) both run, SSGI at (1 - fade) of its strength and the field at `fade`: a crossfade
// (the field's weight is min(fade, 1 - the share the occlusion actually ran SSGI with), FieldShare: they can never add up to more than one, whichever pass saw the frame first). The pixel's
// own confidence multiplies the field's part (a probe that is off, a lookup beyond the safe range): where it is under 1 the pixel gets less bounce, not SSGI's (the colour chain is a frame
// level decision). CHECK G3b: that is acceptable (the occlusion's reach is 9 m, the field's finest cascade 32 m: the pixels with confidence under 1 are inside walls, under the ground, past
// the safe range and in the first 0.4 s after a cut).
//
// THE SIZE. The lookup (16 taps and the maths) runs at the occlusion's TRACED size (half the picture's above 1080 lines: ao_size.h) into an RGBA16F image (rgb = E_net x confidence, a = the
// linear depth the texel was looked up at), and the apply, at the picture's size, takes the four nearest texels weighted by depth agreement and normal agreement (the AO's rule plus the normal)
// and multiplies by the albedo at the picture's size. At or under 1080 lines both are one pass. MSAA: the planes and the depth are read at the guest's sample 0 and the result added to
// every sample of the pixel (the light pass's rule, a one pixel fringe at silhouettes).
//
// THE DEBUG VIEWS (mcla_gpu_rt_gi_view, drawn over the picture instead of the add, the lookup done at the picture's size): 1 E_surf, 2 the confidence (red 0 to green 1), 3 the sky visibility,
// 5 the baseline E_base, 6 the leak test (the share of the eight probes' weight that the Chebyshev visibility test took away: green none, red all), 9 the bounce added (alone on black,
// four times as bright), 10 E_net (after the band and the cap), 11 the occlusion term. 4, 7 and 8 are the field owner's (gi_field.h: the probes as spheres).
//
// THIS FILE IS THE TWIN of hlsl/gi_apply_common.hlsli: gi_apply_test.cpp compiles the .hlsli as C++ and compares each function with the one here on random inputs. It includes nothing of Plume
// or ReXGlue. Threading: render thread only (plain data and pure functions).
//
// CHECK: nothing here was run against the game; the list is in the commit's report and in GiApply's header (gi_apply.h).
// ===========================================================================

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "gi_frame.h"
#include "ssgi_light.h"

namespace mcla::gpu::gi_apply {

inline constexpr uint32_t kFormatVersion = 1;
inline constexpr uint32_t kNoSlot = gi::kNoSlot;

// ---------------------------------------------------------------------------
// the numbers (the .hlsli's #defines are these: gi_apply_test.cpp compares them)
// ---------------------------------------------------------------------------

inline constexpr float kSky = 60000.0f;                       // the depth chain's value for the sky (hlsl/ao_common.hlsli kAoSky)
inline constexpr float kCapFloor = ssgi_light::kCapFloor;     // the least a cap is (0.5, scene units)
inline constexpr float kKnee = ssgi_light::kKnee;
inline constexpr float kVehicleShare = ssgi_light::kVehicleShare;
inline constexpr float kVehicleStrengthMax = ssgi_light::kVehicleStrengthMax;
// The dead band under E_net, as a share of the open ground's radiance (albedo x (sun x sin(elevation) + ambient up)): the field's own reading of an open plane is within 1% of it on average and
// 3.4% at worst (gi_frame.h C6), so a band of 2% leaves an open field with next to nothing and costs a real wall's bounce next to nothing. COST PASS / G3a: a value, the open lawn run decides.
inline constexpr float kDeadBand = 0.02f;
// What a pixel's linear depth may differ from a texel's before the texel stops counting: the occlusion's rule (hlsl/ao_apply_ps.hlsl VisibilityAt).
inline constexpr float kDepthTolerance = 0.02f;
inline constexpr float kDepthToleranceFloor = 0.05f;
inline constexpr float kMinUpsampleWeight = 1.0e-4f;
// A pixel with more than this share of its neighbourhood marked as a vehicle takes the paint's visibility (ao_paint.h kVehicleFrom: over half, and all at 1).
inline constexpr float kPaintFrom = 0.5f;

// ---------------------------------------------------------------------------
// the views, the skips
// ---------------------------------------------------------------------------

// mcla_gpu_rt_gi_view's values this pass draws (gi_frame.h View for 1, 2, 3, 5, 6; 4, 7 and 8 are the field's own spheres).
inline constexpr uint32_t kViewIrradiance = uint32_t(gi::View::kIrradiance);       // 1
inline constexpr uint32_t kViewValidity = uint32_t(gi::View::kValidity);           // 2
inline constexpr uint32_t kViewSkyVisibility = uint32_t(gi::View::kSkyVisibility); // 3
inline constexpr uint32_t kViewBaseline = uint32_t(gi::View::kBaseline);           // 5
inline constexpr uint32_t kViewLeak = uint32_t(gi::View::kLeak);                   // 6
inline constexpr uint32_t kViewBounce = 9;                                         // the light the pass adds, alone on black, four times as bright
inline constexpr uint32_t kViewNet = 10;                                           // E_net x the confidence after the dead band and the cap
inline constexpr uint32_t kViewOcclusion = 11;                                     // the occlusion's term the bounce is multiplied by
inline constexpr uint32_t kViewMax = 11;
constexpr bool IsApplyView(uint32_t view) {
  return view == kViewIrradiance || view == kViewValidity || view == kViewSkyVisibility || view == kViewBaseline || view == kViewLeak || view == kViewBounce || view == kViewNet || view == kViewOcclusion;
}
// The views the field owner draws (gi_field_rules.h IsOwnView): the pass leaves them to it.
constexpr bool IsOwnerView(uint32_t view) { return view == uint32_t(gi::View::kProbes) || view == 7u || view == 8u; }
// The debug images are scaled by this so that 1.0 reads under the tone map's white (the occlusion's and the G-buffer's views do the same).
inline constexpr float kViewScale = 0.3f;

enum class Skip : uint32_t { kRecorded, kNoField, kNoStrength, kNoPlanes, kNoCamera, kNoTarget, kNoPipeline, kNoBuffer, kNoSun, kCount };
constexpr const char* SkipName(Skip why) {
  switch (why) {
    case Skip::kRecorded: return "recorded";
    case Skip::kNoField: return "no field";
    case Skip::kNoStrength: return "strength 0";
    case Skip::kNoPlanes: return "no G-buffer planes";
    case Skip::kNoCamera: return "no camera";
    case Skip::kNoTarget: return "no colour target";
    case Skip::kNoPipeline: return "no pipeline";
    case Skip::kNoBuffer: return "no buffer";
    case Skip::kNoSun: return "no light constants";
    case Skip::kCount: break;
  }
  return "?";
}

// ---------------------------------------------------------------------------
// the SSGI switch (the field's share of the frame)
// ---------------------------------------------------------------------------

// The share of its strength the screen-space bounce light runs with this frame: 1 where the field does not replace it (no field, not in the frame, fade 0, no safe range: ReplacesSsgi),
// (1 - fade) while the field fades in, 0 (the colour chain is not built) once it stands. The occlusion asks for it when it plans its frame (ao.cpp PlanGi) and scales its strength by it.
inline float SsgiShareFor(const gi::GiFrame& f) {
  if (!f.ReplacesSsgi()) return 1.0f;
  return gi::Saturate(1.0f - f.fade);
}
// True when the screen-space bounce light is not built at all this frame.
inline bool SsgiSkipped(float share) { return !(share > 0.0f); }

// The weight of the field's bounce in the frame, 0..1: its fade, but never more than what the screen-space bounce light left (1 - the share it ran with), so that the two cannot add up to
// more than one bounce whichever of them planned the frame first. 0 where there is no field in the frame. `ssgi_ran_share` is 0 when the screen-space bounce light did not run at all (the
// occlusion is off, the bounce light's row is off, the pause menu's world): the field then takes its whole fade.
inline float FieldShare(const gi::GiFrame& f, float ssgi_ran_share) {
  if (!f.FieldPresent()) return 0.0f;
  return std::min(gi::Saturate(f.fade), 1.0f - gi::Saturate(ssgi_ran_share));
}

// ---------------------------------------------------------------------------
// the frame's numbers beyond the GiFrame's own (the rows after the field's 20)
// ---------------------------------------------------------------------------

struct Frame {
  float right[3] = {1, 0, 0}, up[3] = {0, 1, 0}, forward[3] = {0, 0, 1}, position[3] = {0, 0, 0};  // the camera's axes and place, world space (the AO's trigger pose)
  float tan_x = 1.0f, tan_y = 1.0f;                                                                  // tan of the half field of view
  float lin[4] = {0, 0, 0, 0};                                                                       // the projection's depth rows P22, P23, P32, P33 (screen_frame.h)
  float render[2] = {0, 0};                                                                          // the picture's size
  float traced[2] = {0, 0};                                                                          // the occlusion's traced size (the lookup image's)
  float sun_dir[3] = {0, 1, 0};                                                                      // the direction TO the sun, world space, unit
  float ambient = 0.0f;                                                                              // the frame's ambient light, luma (PS c26)
  float sun = 0.0f;                                                                                  // ... and the sun's (PS c39)
  float cap = 2.0f;                                                                                  // the bounce light level's cap multiple (ssgi_level.h)
  float field_share = 0.0f;                                                                          // FieldShare
  float strength = 1.0f;                                                                             // the bounce light level's strength (GiFrame::strength)
  float ao_strength = 0.0f;                                                                          // the occlusion's look strength (AO INTENSITY): 0 = no occlusion term
  float ao_fade_end = 100.0f;                                                                        // the occlusion's distance fade's end, metres
  float paint_power = 1.0f;                                                                          // the paint's visibility power (ao_paint.h); 1 = off
  float dead_band = kDeadBand;
  float scene_units = 1.0f;
  float daylight_recovery = -1.0f;
};

// The buffer the shader reads: the field's 20 rows (gi_frame.h GpuGiFrame) and then these 9 rows of 16 bytes.
//   row 0  right xyz, tan_x             row 5  render w, h, traced w, h
//   row 1  up xyz, tan_y                row 6  sun direction xyz (toward the sun), ambient luma
//   row 2  forward xyz, field share     row 7  sun luma, cap, ao strength, ao fade end
//   row 3  position xyz, strength       row 8  dead band, paint power, scene units, daylight recovery
//   row 4  lin (P22, P23, P32, P33)
inline constexpr uint32_t kApplyRows = 9;
inline constexpr uint32_t kRowsOffsetBytes = gi::kGpuFrameBytes;  // 320
inline constexpr uint32_t kBufferBytes = kRowsOffsetBytes + kApplyRows * 16;  // 464
inline constexpr uint32_t kBufferAlloc = 512;
inline constexpr uint32_t kBufferWords = kBufferBytes / 4;
static_assert(kBufferBytes == 464 && kBufferBytes <= kBufferAlloc, "the buffer is the field's 20 rows and 9 more");

using Buffer = std::array<uint32_t, kBufferWords>;

inline Buffer MakeBuffer(const gi::GpuGiFrame& field, const Frame& fr) {
  Buffer b = {};
  std::memcpy(b.data(), field.words.data(), gi::kGpuFrameBytes);
  uint32_t* w = b.data() + gi::kGpuFrameRows * 4;
  const auto put = [&w](uint32_t row, float a, float b2, float c, float d) {
    w[row * 4 + 0] = gi::detail::Bits(a);
    w[row * 4 + 1] = gi::detail::Bits(b2);
    w[row * 4 + 2] = gi::detail::Bits(c);
    w[row * 4 + 3] = gi::detail::Bits(d);
  };
  put(0, fr.right[0], fr.right[1], fr.right[2], fr.tan_x);
  put(1, fr.up[0], fr.up[1], fr.up[2], fr.tan_y);
  put(2, fr.forward[0], fr.forward[1], fr.forward[2], fr.field_share);
  put(3, fr.position[0], fr.position[1], fr.position[2], fr.strength);
  put(4, fr.lin[0], fr.lin[1], fr.lin[2], fr.lin[3]);
  put(5, fr.render[0], fr.render[1], fr.traced[0], fr.traced[1]);
  put(6, fr.sun_dir[0], fr.sun_dir[1], fr.sun_dir[2], fr.ambient);
  put(7, fr.sun, fr.cap, fr.ao_strength, fr.ao_fade_end);
  put(8, fr.dead_band, fr.paint_power, fr.scene_units, fr.daylight_recovery);
  return b;
}

// The push block (hlsl/gi_apply_ps.hlsl GiaPush), 32 bytes, at (b0, space4) in the apply's own layout.
struct Push {
  uint32_t slots[4];  // x the main depth plane's bindless slot, y RT1's, z RT2's, w the occlusion's linear depth (level 0, traced size)
  uint32_t info[4];   // x the occlusion's visibility image's slot (kNoSlot: none), y the lookup image's slot (kNoSlot: the apply looks up itself), z the host sample that is the guest's sample 0, w flags | view << 8
                      // | the material plane's slot << 16 (gi_cabin_rules.h PackMaterialSlot; 0xFFFF none: the player's cabin term reads RT3 only with a slot)
};
static_assert(sizeof(Push) == 32, "the push block is 32 bytes");
inline constexpr uint32_t kPushSpace = 4;
inline constexpr uint32_t kFlagLowTraced = 1u;      // the traced size is half the picture's: the visibility image is read with the same upsample
inline constexpr uint32_t kFlagVisibility = 2u;     // info.x is a visibility image
inline constexpr uint32_t kFlagQuarterLookup = 4u;
inline constexpr uint32_t kFlagFastLookupApply = 8u;
constexpr bool UseQuarterLookup(bool enabled, bool half_traced, uint32_t render_height, bool field_only, bool special_view) {
  return enabled && half_traced && render_height >= 2160u && field_only && !special_view;
}
constexpr uint32_t QuarterLookupExtent(uint32_t n) { return n / 4u + (n % 4u != 0u); }
inline constexpr uint32_t kFlagPbr = 16u;  // Diffuse weight from RT3, whose slot is shared with cabin suppression.
inline constexpr uint32_t kViewShift = 8;
inline constexpr uint32_t kViewMask = 0xFFu << kViewShift;
constexpr uint32_t PackInfoW(uint32_t flags, uint32_t view) { return (flags & 0xFFu) | ((view << kViewShift) & kViewMask); }

// ---------------------------------------------------------------------------
// the camera (the twin of hlsl/gi_apply_common.hlsli GiaLinearDepth, GiaWorldPosition; local_lights_rules.h has the same two for its pass)
// ---------------------------------------------------------------------------

// The camera's axes and place from the game's view inverse (row vectors: row 0 right, row 1 up, row 2 BACK, row 3 position; the renderer's passes look along +z, so forward is minus row 2).
// False when an entry is not finite or an axis is not a unit vector.
inline bool CameraFromViewInverse(const float m[16], Frame* out) {
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 3; ++c) {
      if (!std::isfinite(m[r * 4 + c])) return false;
    }
  }
  for (int r = 0; r < 3; ++r) {
    const double length = std::sqrt(double(m[r * 4]) * m[r * 4] + double(m[r * 4 + 1]) * m[r * 4 + 1] + double(m[r * 4 + 2]) * m[r * 4 + 2]);
    if (!(length > 0.98 && length < 1.02)) return false;
  }
  for (int c = 0; c < 3; ++c) {
    out->right[c] = m[c];
    out->up[c] = m[4 + c];
    out->forward[c] = -m[8 + c];
    out->position[c] = m[12 + c];
  }
  return true;
}

// The distance along the view direction of a stored depth (1 near, 0 far); kSky for the sky and for anything the linearisation cannot place.
inline float LinearDepth(float stored, const float lin[4]) {
  const float ndc = 1.0f - gi::Saturate(stored);
  const float num = lin[2] - ndc * lin[3];
  const float den = lin[0] - ndc * lin[1];
  const float t = num / den;
  return (stored > 0.0f && den != 0.0f && t > 0.0f && t < kSky) ? t : kSky;
}

// The world position of the centre (cx, cy) of a pixel of an image `w` x `h` at the view distance t.
inline gi::Vec3 WorldPosition(const Frame& fr, float cx, float cy, float w, float h, float t) {
  const float u = cx / w;
  const float v = cy / h;
  const float x = (u * 2.0f - 1.0f) * fr.tan_x * t;
  const float y = (1.0f - v * 2.0f) * fr.tan_y * t;
  return {fr.position[0] + fr.right[0] * x + fr.up[0] * y + fr.forward[0] * t, fr.position[1] + fr.right[1] * x + fr.up[1] * y + fr.forward[1] * t,
          fr.position[2] + fr.right[2] * x + fr.up[2] * y + fr.forward[2] * t};
}

// ---------------------------------------------------------------------------
// the pixel's maths (the twin of hlsl/gi_apply_common.hlsli)
// ---------------------------------------------------------------------------

inline float Luma(gi::Vec3 c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }

inline gi::Vec3 CapRadiance(gi::Vec3 c, float cap) {
  const ssgi_light::Rgb r = ssgi_light::CapRadiance({c.x, c.y, c.z}, cap);
  return {r.r, r.g, r.b};
}

// The strength of a pixel whose share of vehicle neighbourhood is `vehicle`: the level's on the world, the car's on a vehicle (ssgi_light.h StrengthFor).
inline float StrengthFor(float strength, float vehicle) { return ssgi_light::StrengthFor(strength, vehicle); }

// What the field's irradiance holds over the open ground, per channel: E_net = max(E_surf - E_base - band x ground, 0), `ground` the radiance of the open plane (GroundBaseline at n = down).
inline gi::Vec3 NetIrradiance(const gi::GiFrame& f, gi::Vec3 e_surf, gi::Vec3 n, float dead_band) {
  const gi::Vec3 base = gi::GroundBaseline(f, n);
  const gi::Vec3 ground = gi::GroundBaseline(f, {0.0f, -1.0f, 0.0f});
  return {std::max(e_surf.x - base.x - dead_band * ground.x, 0.0f), std::max(e_surf.y - base.y - dead_band * ground.y, 0.0f), std::max(e_surf.z - base.z - dead_band * ground.z, 0.0f)};
}

// The lookup image's texel: E_net x the lookup's confidence (the field's own certainty about this pixel).
inline gi::Vec3 LookupNet(const gi::GiFrame& f, const gi::Sample& s, gi::Vec3 n, float dead_band) { return NetIrradiance(f, s.irradiance, n, dead_band) * s.confidence; }

// A flat, unobstructed plane has a directional sky share of (1 + n.y) / 2. A sustained deficit enables the daylight correction.
inline gi::Vec3 DaylightNet(const gi::GiFrame& f, const gi::Sample& s, gi::Vec3 n, const Frame& fr) {
  const gi::Vec3 net = NetIrradiance(f, s.irradiance, n, fr.dead_band);
  const float deficit = 0.5f * (1.0f + n.y) - s.sky_visibility;
  const float x = std::clamp((deficit - 0.15f) / 0.30f, 0.0f, 1.0f);
  const float shade = x * x * (3.0f - 2.0f * x);
  const float daylight = std::clamp((f.sun_sin_elevation - 0.10f) * 5.0f, 0.0f, 1.0f);
  const float weight = shade * daylight;
  if (fr.daylight_recovery < 0.0f || !(weight > 0.0f)) {
    return net * s.confidence;
  }
  const gi::Vec3 band = gi::GroundBaseline(f, {0.0f, -1.0f, 0.0f}) * fr.dead_band;
  const gi::Vec3 recovered = gi::NetIrradiance(s.irradiance, band) - net;
  const gi::Vec3 extra = net * (fr.scene_units - 1.0f) + recovered * (fr.scene_units * fr.daylight_recovery);
  return (net + extra * weight) * s.confidence;
}

// The occlusion's term the bounce is multiplied by (the apply's ao_factor with the ambient share at 1: the bounce is all indirect light): lerp(1, ao_seen, saturate(strength x fade)),
// ao_seen = ao, and on a vehicle's pixel (over half of its neighbourhood) ao raised to the paint power; fade = 1 up to half the fade's end, smoothstep to 0 at it.
inline float AoTerm(float ao, float vehicle, float tc, float strength, float fade_end, float paint_power) {
  const float on_car = gi::Saturate((vehicle - kPaintFrom) * 2.0f);
  float seen = ao;
  if (on_car > 0.0f) seen = ao + (std::pow(ao, paint_power) - ao) * on_car;
  const float half_end = 0.5f * fade_end;
  float t = half_end > 0.0f ? gi::Saturate((tc - half_end) / (fade_end - half_end)) : 1.0f;
  const float fade = 1.0f - t * t * (3.0f - 2.0f * t);
  return 1.0f + (seen - 1.0f) * gi::Saturate(strength * fade);
}

struct PixelIn {
  gi::Vec3 albedo;          // RT2.rgb squared
  float lit = 1.0f;         // RT2.a (0..1)
  gi::Vec3 e_net;           // the lookup's E_net x confidence
  float strength = 1.0f;    // the level's
  float field_share = 1.0f; // FieldShare
  float vehicle = 0.0f;     // the share of the pixel's neighbourhood the depth pass marked as a vehicle
  float ao_term = 1.0f;     // AoTerm
  float ambient = 0.0f;     // the frame's lights, luma
  float sun = 0.0f;
  float n_dot_l = 0.0f;
  float cap = 2.0f;
};

// The light the pass adds to the pixel (gi above): albedo x CapRadiance(E_net) x strength x field share x lit x AO term, then SoftClip to the room under the ceiling.
inline gi::Vec3 BounceAdd(const PixelIn& in) {
  const float cap_luma = ssgi_light::RadianceCap(in.cap, in.ambient, in.sun);
  const gi::Vec3 e = CapRadiance(in.e_net, cap_luma);
  const float s = StrengthFor(in.strength, in.vehicle);
  gi::Vec3 add = in.albedo * e * (s * in.field_share * gi::Saturate(in.lit) * in.ao_term);
  const float t = std::max(in.ambient + in.sun * in.n_dot_l, 1.0e-3f);
  const float own = Luma(in.albedo) * t;
  const float room = std::max(cap_luma - own, 0.0f);
  const float added = Luma(add);
  if (added > 1.0e-6f) add = add * (ssgi_light::SoftClip(added, room) / added);
  return add;
}

// The upsample's weight of one of the four nearest traced texels: the bilinear weight x the depth agreement (the occlusion's rule) x the normal agreement (the cosine to the fourth).
inline float DepthWeight(float tc, float td) {
  const float dz = std::fabs(td - tc) / std::max(kDepthTolerance * tc, kDepthToleranceFloor);
  return std::max(std::exp(-dz * dz), kMinUpsampleWeight);
}
inline float NormalWeight(gi::Vec3 n, gi::Vec3 m) {
  const float d = gi::Saturate(gi::Dot(n, m));
  const float d2 = d * d;
  return d2 * d2;
}

// The share of the eight probes' weight (trilinear x validity x the smooth back face term) that the Chebyshev visibility test took away at p with normal n, in the finest cascade that holds p (the
// leak test's number): 0 no probe is hidden from the point by geometry between them, 1 all of them are. The back face term is left out on purpose: it takes the weight of every probe that is behind
// the surface, wall or no wall, so it is high everywhere. 0 where no cascade holds the point or no probe is on.
template <class Source>
float GuardShare(const gi::GiFrame& f, const Source& src, gi::Vec3 p, gi::Vec3 n) {
  for (uint32_t c = 0; c < f.cascade_count && c < gi::kMaxCascades; ++c) {
    const gi::CascadeFrame& k = f.cascades[c];
    if (!(gi::CascadeBlend(k, p) > 0.0f)) continue;
    const float offset = gi::kNormalOffsetSpacings * k.spacing_m;
    const gi::Vec3 q = {p.x + n.x * offset, p.y + n.y * offset, p.z + n.z * offset};
    const float inv = 1.0f / k.spacing_m;
    const float gx = std::min(std::max(q.x * inv, float(k.origin.x)), float(k.origin.x) + float(k.dims.x - 1));
    const float gy = std::min(std::max(q.y * inv, float(k.origin.y)), float(k.origin.y) + float(k.dims.y - 1));
    const float gz = std::min(std::max(q.z * inv, float(k.origin.z)), float(k.origin.z) + float(k.dims.z - 1));
    const int32_t bx = std::min(int32_t(std::floor(gx)), k.origin.x + int32_t(k.dims.x) - 2);
    const int32_t by = std::min(int32_t(std::floor(gy)), k.origin.y + int32_t(k.dims.y) - 2);
    const int32_t bz = std::min(int32_t(std::floor(gz)), k.origin.z + int32_t(k.dims.z) - 2);
    const float fx = gx - float(bx), fy = gy - float(by), fz = gz - float(bz);
    float sum_w = 0.0f, sum_b = 0.0f;
    for (uint32_t i = 0; i < 8; ++i) {
      const uint32_t ox = i & 1u, oy = (i >> 1) & 1u, oz = (i >> 2) & 1u;
      const int32_t cx = bx + int32_t(ox), cy = by + int32_t(oy), cz = bz + int32_t(oz);
      const float wt = (ox ? fx : 1.0f - fx) * (oy ? fy : 1.0f - fy) * (oz ? fz : 1.0f - fz);
      const uint32_t sx = gi::Wrap(cx, k.dims.x), sy = gi::Wrap(cy, k.dims.y), sz = gi::Wrap(cz, k.dims.z);
      const float wv = src.LoadWeight(sx + k.dims.x * sy, k.weight_y + sz);
      if (!(wt * wv > 0.0f)) continue;
      const gi::Vec3 d = {float(cx) * k.spacing_m - q.x, float(cy) * k.spacing_m - q.y, float(cz) * k.spacing_m - q.z};
      const float dist = std::sqrt(std::max(gi::Dot(d, d), 1.0e-8f));
      const gi::Vec3 to_probe = d * (1.0f / dist);
      float bf = std::max((gi::Dot(to_probe, n) + 1.0f) * 0.5f, gi::kBackFaceFloor);
      bf = bf * bf;
      const gi::Vec2 moments_uv = gi::ProbeUv(k, f.atlas_width, f.atlas_height, sx, sy, sz, gi::OctEncode({-to_probe.x, -to_probe.y, -to_probe.z}));
      const gi::Vec2 m = src.SampleMoments(moments_uv.x, moments_uv.y);
      float wc = 1.0f;
      if (dist > m.x) {
        const float variance = std::fabs(m.y - m.x * m.x);
        const float diff = dist - m.x;
        const float cheb = variance / (variance + diff * diff);
        wc = std::max(cheb * cheb * cheb, gi::kChebyshevFloor);
      }
      sum_w += wt * wv * bf * wc;
      sum_b += wt * wv * bf;
    }
    return sum_b > 0.0f ? 1.0f - gi::Saturate(sum_w / sum_b) : 0.0f;
  }
  return 0.0f;
}

// ---------------------------------------------------------------------------
// the static checks
// ---------------------------------------------------------------------------

static_assert(sizeof(Push) == 32 && kPushSpace == 4, "the apply's push block");
static_assert(PackInfoW(kFlagLowTraced | kFlagVisibility, kViewBounce) == (3u | (9u << 8)) && (PackInfoW(0xFFFFu, 0x1FFu) & ~(0xFFu | kViewMask)) == 0, "the flags and the view share the last word");
static_assert(IsApplyView(1) && IsApplyView(2) && IsApplyView(3) && IsApplyView(5) && IsApplyView(6) && IsApplyView(9) && IsApplyView(10) && IsApplyView(11) && !IsApplyView(0) && !IsApplyView(4) &&
                  !IsApplyView(7) && !IsApplyView(8) && !IsApplyView(12),
              "the views this pass draws");
static_assert(IsOwnerView(4) && IsOwnerView(7) && IsOwnerView(8) && !IsOwnerView(5), "the views the field owner draws");
static_assert(kViewMax == 11, "the last view");

}  // namespace mcla::gpu::gi_apply
