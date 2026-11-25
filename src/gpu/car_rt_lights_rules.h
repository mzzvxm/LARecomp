#pragma once
// ===========================================================================
// MCLA GPU renderer -- the lamps of the cars in a reflection: a reflected car's lens glows as the game's own lamps say (LIGHT-UNIFY LU-8 = step U8)
// ===========================================================================
// WHAT WAS WRONG. A reflection ray that hits ANOTHER car's lens (the tail light of the Camaro beside the player, the headlights of the car behind) shades it by its material (car_rt_material_rules.h)
// and adds the lens's glow at night: `albedo (the illumination map) x emissive x kEmissiveGain x night`, where `emissive` was 1 for every lens of every car and `night` was a guess from the
// colour of the cars' draws' sun (kNightLow, kNightHigh), which read 0.97 at 14 h and at 23 h alike (RT-HIT-SHADING-3). So every lens glowed or none did, whatever the car's lamps were doing: a
// brake lamp, a lamp that is off, a turn signal in its dark half-period, the police's flash were all the same. The game knows: its light submit gives every lamp of every car a
// level (light_points.h, car_lights.h: 19 kinds a car, the intensity the guest multiplies, 2.0 = a headlight on, 0.0 by day).
//
// WHAT THIS DOES. The producer is car_lights.h (LU-7): once a frame the points of the last 100 ms become cars, each with its lamps (kind, family, place in the WORLD, level, on). This header is the
// consumer for ray traced hit shading:
//   LampSet      every lamp of every car, flat: the world place, the family, the level, whether it is on (MakeLampSet). Off lamps are in it: a lens whose lamp is off is DARK, not unknown.
//   LensGlow     the glow of one LENS instance of another car: the lamps that stand in the lens mesh's box (the box in the mesh's own space, grown by kLensMargin; the lamp's world place taken
//                into the mesh's space by the inverse of the draw's matrix: a lens is a mesh of its own in the car's kit, the lamp's light point is inside it) give the lens's scale, the
//                largest of the lamps' (GlowOf: the family's law of the lamp's level). A lens no lamp stands in (a car the hook did not see, a lamp kind the table leaves out, a mesh that is not at a
//                lamp) is NOT driven: it keeps the night factor's guess. A car whose lights the hook gave nothing for is the same. The join needs no car identity: the lamps of another car are
//                metres from this lens, so the box test tells them apart, for a racer (owned light object), a traffic car (none: joined by place by car_lights.h's own rule, which this does
//                not use) and a car placed from memory or from its model data alike.
//   the record   the instance's second row (car_rt_material_rules.h MakeRows): z the scale (float bits) as before, w kGlowDrivenBit when the scale is the lamps': the shader then does NOT
//                multiply it by the night factor (the lamps already are off by day: the level is the game's own). One bit: the rows' size is unchanged.
//
// THE LAW (GlowOf), the LOOK values to set by eye (CHECK on the first night run): glow = on + span x level for a lamp that is on, 0 for one that is off. A headlight, a hi-beam, a fog lamp, a
// reverse lamp, a police lamp and a turn signal are on or off and their glow is their level (a turn signal's flash is the game's own toggle). A TAIL lamp (kind 1: the lights that are on with
// the headlights) glows at kTailGlow. The BRAKE family (kinds 0 and 2) carries both the tail glow and the pedal in one scalar (LIGHT-UNIFY U7: the player's reads 0.075 with the pedal up, a racer's
// 0.5, traffic's 0.7 to 0.95 always), so it glows from kBrakeRest at a low level to 1 at a full one: the pedal shows as a brighter lamp (CHECK with a player who brakes: the census line says
// the level the lens read). By day every level is 0, so no lens glows by day (the night factor used to).
//
// THE SWITCH. mcla_gpu_car_rt_lamps (debug, true): false is the old rule (every lens at the night factor's guess); the car lights' source is then off (the hook notes nothing for the RT's use).
//
// COST (estimate, nothing was run). Host, render thread: the points' poll and Build (car_lights.h: about 20 microseconds for 15 cars' 400 points), then per lens instance of another car one 4x4
// inverse and a box test of each lamp (a lamp on a car 12 m away is rejected by its first axis): at most 8 cars x 12 lens parts x 256 lamps = 25 000 tests (about 0.1 ms), usually 4 x 8 x 100
// = 3 000 (about 10 microseconds). GPU: none; the shader reads the same row. The guest hook notes all 19 kinds while this is on (light_points.h: a few atomic stores a lamp a frame).
//
// RUN PLAN (D3D12 only until Plume's ray query; the exe is built by the main session): the SHOWCASE-RT night scene (ports/scripts/scenes/showcase-rt-night*): the Camaro's tail lights in
// the player's door, and a traffic car beside the player reflecting the player's lamps in its paint (car_rt_traffic_rules.h). Pairs: the default; mcla_gpu_car_rt_lamps false (the old rule); the
// Camaro with its brakes on (the scene's brake verb, or the player braking) against off; a day pair (nothing glows). The log, at the first night frame and every 300 triggers: "[car rt]
// materials ..." now ends with "lenses a frame: N glowing, N dark, N by the night factor (no lamp)", and "[car-lights] ..." lines name each car's levels (car_lights.h FormatCensus: the
// player's brake reading with the pedal up and down). Look at the Camaro's lenses in the player's door in the x6 exposure crop: lit red where the lamps are on, dark where off.
//
// CHECK (nothing here was run):
//   1. the lamps stand inside their lens mesh's box (kLensMargin 0.12 m): the census's "no lamp" count is the lenses the rule could not place. A taillight kit whose lens meshes are
//      far from the points (lamp points are the BULB, a lens mesh is the cover) would show up as a high "no lamp"; kLensMargin or the points' offset is then the number to change;
//   2. the brake family's reading with the pedal up and down (kBrakeRest, the span), and whether the tail kind (1) reads on a player's car at night;
//   3. which lens a lamp lights where one mesh holds several (a rear cluster of brake, tail, turn and reverse: the lens takes the largest, the illumination map says where the light is);
//   4. a car with no owner (traffic): the lens-to-lamp join by the box does not need its direction (kTrustOwnerlessDirection stays false);
//   5. the hook's cost with all kinds noted (the guest thread: GFX pass timing "guest" in a traffic scene, with and without mcla_gpu_car_rt_lamps).
//
// Pure: this header includes car_lights.h (and so light_points.h, light_frame.h), temporal_math.h and nothing of the renderer; car_rt_lights_rules_test.cpp builds on its own.
// ===========================================================================

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "car_lights.h"
#include "light_points.h"
#include "temporal_math.h"

namespace mcla::gpu::car_rt_lights {

namespace cl = larecomp::gpu::car_lights;
namespace lp = larecomp::gpu::light_points;

// ---------------------------------------------------------------------------
// the law
// ---------------------------------------------------------------------------

// The glow of a lamp that is on: on + span x level. Indexed by lp::Family (head, hibeam, fog, brake, tail, turn, reverse, police, other). LOOK values (CHECK).
struct GlowLaw {
  float on;
  float span;
};
inline constexpr float kTailGlow = 0.45f;     // a tail lamp (kind 1) at its full level
inline constexpr float kBrakeRest = 0.3f;     // the brake family's glow at a level of 0 while it is on (the pedal up, the lamps on with the headlights)
inline constexpr std::array<GlowLaw, lp::kFamilyCount> kGlowLaw = {{
    /* head    */ {0.0f, 1.0f},
    /* hibeam  */ {0.0f, 1.0f},
    /* fog     */ {0.0f, 1.0f},
    /* brake   */ {kBrakeRest, 1.0f - kBrakeRest},
    /* tail    */ {0.0f, kTailGlow},
    /* turn    */ {0.0f, 1.0f},
    /* reverse */ {0.0f, 1.0f},
    /* police  */ {0.0f, 1.0f},
    /* other   */ {0.0f, 0.0f},
}};

// A family a lens is lit by (the interior's and the unnamed kinds' lamps light no lens).
inline constexpr bool LensFamily(lp::Family f) { return f != lp::Family::kOther; }

// The glow scale of one lamp: 0 for one that is off (or has no law), else on + span x level, never over 1.
inline float GlowOf(lp::Family family, float level, bool on) {
  if (!on || !LensFamily(family)) return 0.0f;
  const GlowLaw& law = kGlowLaw[uint32_t(family)];
  return std::clamp(law.on + law.span * std::clamp(level, 0.0f, 1.0f), 0.0f, 1.0f);
}

// ---------------------------------------------------------------------------
// the lamps, flat
// ---------------------------------------------------------------------------

struct LampPoint {
  float world[3] = {};
  lp::Family family = lp::Family::kOther;
  uint8_t kind = 0;
  bool on = false;
  float level = 0.0f;  // 0..1 (light_points.h Level)
};

struct LampSet {
  std::vector<LampPoint> lamps;  // every lamp of every car the producer gave, on or off
  uint32_t cars = 0;
  uint32_t on = 0;               // ... of which are on
  bool Valid() const { return !lamps.empty(); }
  void Clear() {
    lamps.clear();
    cars = 0;
    on = 0;
  }
};

// The lamps of a frame of cars (car_lights.h Build): the ones a lens can be lit by (LensFamily).
inline void MakeLampSet(const cl::CarLightsFrame& frame, LampSet* out) {
  out->Clear();
  out->cars = frame.count;
  for (uint32_t c = 0; c < frame.count; ++c) {
    const cl::Car& car = frame.cars[c];
    for (uint32_t i = 0; i < car.lamp_count; ++i) {
      const cl::Lamp& lamp = car.lamps[i];
      if (!LensFamily(lamp.family)) continue;
      LampPoint p;
      for (int k = 0; k < 3; ++k) p.world[k] = lamp.world[k];
      p.family = lamp.family;
      p.kind = uint8_t(lamp.kind);
      p.on = lamp.on;
      p.level = lamp.level;
      out->lamps.push_back(p);
      out->on += lamp.on ? 1u : 0u;
    }
  }
}

// ---------------------------------------------------------------------------
// the lens
// ---------------------------------------------------------------------------

// A lamp this near the lens mesh's box (in the mesh's own space, metres) belongs to the lens. The lamp's light point is the bulb, inside the lens's housing; the housings' meshes are 0.2 to 0.6 m.
inline constexpr float kLensMargin = 0.12f;
// For a mesh whose box is not known: the lamp this near the mesh's origin (the instance's place) belongs to it.
inline constexpr float kOriginRadius = 0.5f;

// The walked parts' slots that are lenses (car_model_walk_rules.h: 11 headlight, 12 taillight, 13 taillight_b): which families can light them. A drawn lens (no part slot) takes any.
inline constexpr uint16_t kNoPart = 0xFFFFu;
inline constexpr uint32_t FamilyBit(lp::Family f) { return 1u << uint32_t(f); }
inline constexpr uint32_t kFrontFamilies = FamilyBit(lp::Family::kHead) | FamilyBit(lp::Family::kHiBeam) | FamilyBit(lp::Family::kFog) | FamilyBit(lp::Family::kTurn) | FamilyBit(lp::Family::kPolice);
inline constexpr uint32_t kRearFamilies = FamilyBit(lp::Family::kBrake) | FamilyBit(lp::Family::kTail) | FamilyBit(lp::Family::kTurn) | FamilyBit(lp::Family::kReverse) | FamilyBit(lp::Family::kPolice);
inline constexpr uint32_t kAnyFamilies = kFrontFamilies | kRearFamilies;
inline constexpr uint32_t FamiliesOfPart(uint16_t part) {
  switch (part) {
    case 11: return kFrontFamilies;
    case 12: case 13: return kRearFamilies;
    default: return kAnyFamilies;
  }
}

struct Glow {
  bool driven = false;      // a lamp stands in the lens: `scale` is the lamps' (else the night factor's guess stays)
  float scale = 0.0f;       // 0..1: the largest GlowOf of the lamps in the lens (0: they are all off)
  uint32_t matched = 0;     // lamps in the lens
  uint32_t lit = 0;         // ... of which are on
  float level = 0.0f;       // the largest level among the lamps that are on (for the census)
};

// A world point in the model space of a matrix `inverse` (row vectors, v' = v M: the inverse of the draw's model-to-world).
inline void ToModel(const temporal::Mat4& inverse, const float world[3], float out[3]) {
  for (int c = 0; c < 3; ++c) {
    out[c] = float(double(world[0]) * inverse.m[0][c] + double(world[1]) * inverse.m[1][c] + double(world[2]) * inverse.m[2][c] + inverse.m[3][c]);
  }
}

// The glow of a lens instance: `model_to_world` the draw's matrix, [box_min, box_max] the mesh's box in its own space (`has_box` false: the instance's origin with kOriginRadius), `part` the
// walked part's slot (kNoPart for a drawn car's draw), `lamps` the frame's. Without lamps, or with a matrix that has no inverse, nothing is driven.
inline Glow LensGlow(const temporal::Mat4& model_to_world, const float box_min[3], const float box_max[3], bool has_box, uint16_t part, const LampSet& lamps) {
  Glow glow;
  if (!lamps.Valid()) return glow;
  temporal::Mat4 inverse;
  if (!temporal::Inverse(model_to_world, &inverse)) return glow;
  const uint32_t families = FamiliesOfPart(part);
  for (const LampPoint& lamp : lamps.lamps) {
    if ((families & FamilyBit(lamp.family)) == 0) continue;
    float p[3];
    ToModel(inverse, lamp.world, p);
    bool inside = true;
    if (has_box) {
      for (int c = 0; c < 3 && inside; ++c) {
        inside = std::isfinite(p[c]) && p[c] >= box_min[c] - kLensMargin && p[c] <= box_max[c] + kLensMargin;
      }
    } else {
      inside = std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]) && p[0] * p[0] + p[1] * p[1] + p[2] * p[2] <= kOriginRadius * kOriginRadius;
    }
    if (!inside) continue;
    glow.driven = true;
    ++glow.matched;
    if (lamp.on) {
      ++glow.lit;
      glow.level = std::max(glow.level, lamp.level);
    }
    glow.scale = std::max(glow.scale, GlowOf(lamp.family, lamp.level, lamp.on));
  }
  return glow;
}

// ---------------------------------------------------------------------------
// the record's bit, the census
// ---------------------------------------------------------------------------

// Word w of an instance's second row (car_rt_material_rules.h MakeRows, hlsl/car_rt_hit_shade.hlsli kGlowDrivenBit): bit 0 set: the scale in word z is the lamps' and the night factor does not
// multiply it.
inline constexpr uint32_t kGlowDrivenBit = 1u;

// What the shader multiplies a lens's scale by with the frame's night factor `night`: the factor for an undriven one, 1 for a driven one.
inline float NightFor(bool driven, float night) { return driven ? 1.0f : night; }

// What a frame's lenses were, for the log.
struct LensStats {
  uint64_t frames = 0;
  uint64_t lenses = 0;        // lens instances of other cars asked about (class light, emissive)
  uint64_t glowing = 0;       // driven, scale over 0
  uint64_t dark = 0;          // driven, scale 0 (their lamps are off)
  uint64_t undriven = 0;      // no lamp in the lens: the night factor's guess
  uint64_t lamps = 0;         // lamp points in the frames' sets
  uint64_t lamps_on = 0;
  uint64_t cars = 0;
  float level_max = 0.0f;     // the largest level a lit lens read
  void Note(const Glow& g) {
    ++lenses;
    if (!g.driven) {
      ++undriven;
    } else if (g.scale > 0.0f) {
      ++glowing;
      level_max = std::max(level_max, g.level);
    } else {
      ++dark;
    }
  }
  void NoteFrame(const LampSet& set) {
    ++frames;
    lamps += set.lamps.size();
    lamps_on += set.on;
    cars += set.cars;
  }
};

inline std::string Text(const LensStats& s) {
  const double n = double(std::max<uint64_t>(s.frames, 1));
  char text[320];
  std::snprintf(text, sizeof(text), "lenses of other cars a frame: %.1f glowing, %.1f dark (their lamps are off), %.1f by the night factor (no lamp in the lens); the hook's lamps a frame: %.1f of %.1f cars, %.1f on; largest level read on a lit lens %.2f",
                double(s.glowing) / n, double(s.dark) / n, double(s.undriven) / n, double(s.lamps) / n, double(s.cars) / n, double(s.lamps_on) / n, double(s.level_max));
  return text;
}

}  // namespace mcla::gpu::car_rt_lights
