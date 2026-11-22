#pragma once
// ===========================================================================
// MCLA GPU renderer -- what feeds the deferred local light pass besides the frame's own lights: the police light bars, the other cars' lamps and the mods' lamps (perf/LIGHT-UNIFY.md 11.2,
// package GB-H3; the sources are LU-7's: car_lights.h, mod_light_source.h)
// ===========================================================================
// local_lights_rules.h (GB-H1) takes the frame's surface lights and the lamp A/B's lamps. The game itself lights NOTHING of what is here, so the pass is the only light these ever give:
//
//   police       the light bars. A car's police lamps (kinds 7, 8, 9: car_lights.h Lamp::on and ::level are the game's own state of the lamp, the guest's `table x value` of the last 50 ms,
//                so the flash pattern is the game's: a lamp that is off in this frame adds no light, one that fades fades) make ONE light per car and kind at the middle of that kind's
//                lit lamps: red for Police0, white for Police1, blue for Police2 (a guess: CHECK C4). Kind 8 is a siren lamp only where it is not one of the headlights' neighbours: the night
//                probe read a Police1 pair 0.1 to 0.55 m from the headlights of the player's car and of every traffic car, on at 1.0 to 1.8 all night, which cannot be a light bar (kPoliceNearHeadlamp 0.8 m).
//   head lamps   the headlights of the cars the forward headlight stage does not light (the rig lights four cars at most, and not at all with HEADLIGHTS off): one spot light a car, the rig's
//                beam (38 / 12 degrees, 2 degrees down, the headlight law) at a shorter reach and half the strength: "small local lights". A car is covered, and not made here, when one of
//                the frame's own headlights sits at its lamps (kCoverMetres). GB-H2 retires the forward stage; this is the part of its job that the forward stage never did.
//   rear lamps   the brake and tail lamps (one red light at the middle of the lit ones), each lit side of the turn signals (orange), the reverse lamps (white). Small and short: a red wash on the
//                road behind a braking car and on the car that follows it.
//   mod lamps    the lamps of a mod's lights.bin that the game's grid is NOT lighting this frame (mod_light_source.h BuiltSets): the sets the camera is not over. They are outside the mod's
//                rectangle, where the shaders read the city's grid or none at all, so before this they were dark whatever the camera saw (LIGHT-UNIFY C14). The set the camera is over is the
//                grid's own (the pass adds nothing: the lamp would count twice); in the lamp A/B's pass mode that grid is built with its lamps off and the pass takes them through the A/B's own
//                path (HandedOverLamps), in its control mode nothing takes any lamp.
//
// BUDGETS PER CLASS, per tier. The pass is one scissored draw a light and a light the camera is inside the reach of is a full picture draw, so every class has its own cap, three tiers (the
// light shadows' level: the same words, medium / high / ultra: TierOfWord) and the pass's 32 over all of them. A class is cut by its score (the light's brightness over distance squared plus
// its plateau, the lamp picker's own: Score), after the lights that cannot be seen (no pixel of the picture is inside their reach) were left out, so that a car behind the camera takes none of
// the budget. Priority when the 32 are short: the frame's lights, the handed over ones, police, mods, neon, head lamps, rear lamps.
//
//   neon         (GB-NEON) a car's underglow: the game's own colour and pulse (neon_light.h reads them from the car's light object after the game's update: the 30 Hz hold, the day and night
//                constants and the fade are in it), as a few omni lights along the tube under the car, short range (6 / 7 / 8 m by tier), lighting the ground, the kerbs, the walls and the other cars
//                near it. The game's own underglow light is NOT taken away (CHECK N2 in neon_light.h: it may already light the road under the car, and the pass then adds a second pool).
//                The budget is cars x points of light_budget.h's NeonRow (the nearest neon cars, each a whole tube).
//
// THE LOOK is all [I]: the intensities, colours and reaches below are values to set by eye with the first run (CHECK LS1..LS5 in local_lights.h); the cars' own part is multiplied by
// mcla_gpu_local_lights_car_gain (debug) so that a run can tune it without a build. Every number is one line here.
//
// Pure: no renderer, no guest, no SDK. local_lights_sources_test.cpp holds it to the night probe's recorded points.
// ===========================================================================

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <vector>

#include "car_lights.h"
#include "light_budget.h"
#include "light_convert.h"
#include "light_frame.h"
#include "light_id.h"
#include "light_laws.h"
#include "light_points.h"
#include "local_lights_rules.h"
#include "mod_light_source.h"
#include "neon_light.h"

namespace larecomp::gpu::local_lights {

namespace cars = ::larecomp::gpu::car_lights;
namespace pts = ::larecomp::gpu::light_points;
namespace mls = ::larecomp::gpu::mod_light_source;
namespace nl = ::larecomp::gpu::neon_light;
namespace lb = ::mcla::gpu::light_budget;

// ---------------------------------------------------------------------------
// the tier and its budgets
// ---------------------------------------------------------------------------

// The light shadows' level words (the pause menu's LIGHT SHADOWS: off | medium | high | ultra) as the pass's three tiers: `off` and `medium` the smallest (the pass is a debug feature and
// has no setting of its own: Q3 of LIGHT-UNIFY says LIGHT SHADOWS covers headlights and lamps).
enum class Tier : uint8_t { kMedium = 0, kHigh = 1, kUltra = 2 };
inline constexpr Tier TierOfWord(std::string_view word) { return word == "ultra" ? Tier::kUltra : (word == "high" ? Tier::kHigh : Tier::kMedium); }
inline constexpr const char* TierName(Tier t) { return t == Tier::kUltra ? "ultra" : (t == Tier::kHigh ? "high" : "medium"); }

// The most lights of each class a tier lets the pass draw (before the pass's 32). `frame` (GB-H2) is the frame's own surface lights: the headlights of the cars the rig lights, which are the only
// ones that can have a shadow (the replay's views are for them) and the dearest per pixel (a 16 tap lookup where the taps disagree): the rig's four at every tier (light_budget.h
// kMaxHeadlights; the number of cars that have views is the level's, 2 / 2 / 3, and is the replay's, not the pass's). The classes below it are cheaper per light (no lookup).
// GB-NEON: `neon` is the lights of the cars' underglow, light_budget.h's NeonRow (the nearest `neon_cars` cars with `neon_points` lights along each tube, never half a car).
struct ClassBudget {
  uint32_t police = 0;
  uint32_t head = 0;
  uint32_t rear = 0;
  uint32_t mod = 0;
  uint32_t frame = 4;
  uint32_t neon = 0;
  uint32_t neon_cars = 0;
  uint32_t neon_points = 0;
};
// The tier as the one budget table's level (light_budget.h).
inline constexpr lb::Level LevelOf(Tier tier) {
  return tier == Tier::kUltra ? lb::Level::kUltra : (tier == Tier::kHigh ? lb::Level::kHigh : lb::Level::kMedium);
}
inline constexpr ClassBudget BudgetOf(Tier tier) {
  ClassBudget b;
  switch (tier) {
    case Tier::kUltra: b = {16, 8, 12, 12, 4}; break;
    case Tier::kHigh: b = {10, 5, 8, 8, 4}; break;
    default: b = {6, 3, 4, 4, 4}; break;
  }
  const lb::NeonRow& neon = lb::NeonRowOf(LevelOf(tier));
  b.neon_cars = neon.cars;
  b.neon_points = neon.points;
  b.neon = neon.cars * neon.points;
  return b;
}
inline constexpr uint32_t CapOf(const ClassBudget& b, SourceClass c) {
  switch (c) {
    case SourceClass::kPolice: return b.police;
    case SourceClass::kHeadLamps: return b.head;
    case SourceClass::kRearLamps: return b.rear;
    case SourceClass::kMod: return b.mod;
    case SourceClass::kFrame: return b.frame;
    case SourceClass::kNeon: return b.neon;
    default: return kMaxPassLights;
  }
}

// ---------------------------------------------------------------------------
// the look [I]
// ---------------------------------------------------------------------------

struct Look {
  float rgb[3];
  float intensity;  // the game's lamp units: the plateau's radius squared and the colour's multiplier (a street lamp is 55)
};

// Police: Police0 red, Police1 white, Police2 blue. CHECK LS1: the colours, which kind is which side, the flash. I 12: a plateau 3.5 m round the lamp, 0.5 at 5 m, 0.12 at 10 m.
inline constexpr Look PoliceLook(uint32_t kind) {
  switch (kind) {
    case pts::kKindPolice0: return {{1.0f, 0.03f, 0.02f}, 12.0f};
    case pts::kKindPolice2: return {{0.04f, 0.12f, 1.0f}, 12.0f};
    default: return {{1.0f, 0.92f, 0.82f}, 8.0f};
  }
}
inline constexpr float PoliceReach(Tier t) { return t == Tier::kUltra ? 28.0f : (t == Tier::kHigh ? 20.0f : 14.0f); }

// A kind 8 lamp this near (m) to one of its car's headlight lamps is the headlight's neighbour the probe saw on every car (0.1 to 0.55 m), not a siren; a roof's bar is metres from them.
inline constexpr float kPoliceNearHeadlamp = 0.8f;

// Rear lamps. CHECK LS2: brake against tail (one scalar of the car times the kind's constant: the player's brake lamps read 0.075 with nothing pressed, a traffic car's 0.9).
inline constexpr Look kBrakeTailLook = {{1.0f, 0.05f, 0.03f}, 2.5f};
inline constexpr Look kTurnLook = {{1.0f, 0.45f, 0.05f}, 2.0f};
inline constexpr Look kReverseLook = {{1.0f, 0.95f, 0.85f}, 3.0f};
inline constexpr float RearReach(Tier t) { return t == Tier::kUltra ? 16.0f : (t == Tier::kHigh ? 13.0f : 10.0f); }

// Head lamps: the rig's beam, smaller. CHECK LS3: the strength and the reach against the forward headlight (they must read as the same lamp, not a second kind).
inline constexpr float kHeadConeOuterDeg = 38.0f;  // headlight_rig_rules.h kOuterDegrees
inline constexpr float kHeadConeInnerDeg = 12.0f;  // kInnerDegrees
inline constexpr float kHeadDipDeg = 2.0f;         // kDipDegrees
inline constexpr float kHeadNudge = 0.12f;         // kNudge: the light is this far in front of the lens
inline constexpr float kHeadColor[3] = {1.0f, 0.93f, 0.78f};
inline constexpr float kHeadStrength = 0.5f;
inline constexpr float HeadReach(Tier t) { return t == Tier::kUltra ? 42.0f : (t == Tier::kHigh ? 32.0f : 22.0f); }
// A car whose headlights the frame already has (a frame light of the headlight law this near to the middle of its lit head lamps) is covered by the forward stage.
inline constexpr float kCoverMetres = 3.0f;

// Neon (GB-NEON). The colour is the game's (neon_light.h): ExtNeonColor x intensity x pulse x fade, which is the "rgb" of the game's lamp law, so a light here is rgb x saturate(I / d^2) like a
// street lamp with its rgb. The plateau I is a share of the reach squared (kNeonCutShare: where the reach cuts the light it is that share of the colour, as the other classes' lights do: police
// 6 %, rear lamps 2.5 %); a tube of n points shares one game point's strength between them (kNeonTubeGain: the whole tube is as bright as one point of the game's own light at its middle). CHECK
// N2 / N3 (neon_light.h) and the run plan's N runs: the gain (mcla_gpu_neon_light_gain, debug) is the live lever, these are the starting looks.
inline constexpr float NeonReach(Tier t) { return t == Tier::kUltra ? 8.0f : (t == Tier::kHigh ? 7.0f : 6.0f); }
inline constexpr float kNeonCutShare = 0.06f;
inline constexpr float kNeonTubeGain = 1.0f;
// A neon whose brightest channel is below this adds nothing a picture could show (the dark half of a pulse at night): it takes no budget and no G-buffer.
inline constexpr float kNeonMinColour = 0.003f;
// A neon car farther from the camera than this is not lit (its light's reach is a few metres: the picture around it is far away and small, and the pass's draw is a scissored one a light).
inline constexpr float kNeonSeenMetres = 40.0f;

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------

namespace detail {

constexpr float kDegToRad = light_laws::kPi / 180.0f;

inline float Dist2(const float a[3], const float b[3]) {
  const float d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
  return d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
}

// The lit lamps of a group: the running sums of the positions (the light goes at their middle) and the strongest level.
struct Group {
  float sum[3] = {0.0f, 0.0f, 0.0f};
  float level = 0.0f;
  uint32_t n = 0;
  void Add(const cars::Lamp& lamp) {
    for (int i = 0; i < 3; ++i) {
      sum[i] += lamp.world[i];
    }
    level = std::max(level, lamp.level);
    ++n;
  }
  void Centre(float out[3]) const {
    for (int i = 0; i < 3; ++i) {
      out[i] = n != 0 ? sum[i] / float(n) : 0.0f;
    }
  }
};

inline uint32_t CarId(const cars::Car& car) { return car.vehicle != 0 ? car.vehicle : car.object; }

// The group codes of the lights a car makes (ids: PointId(car, kGroupBase | code)): a code per light, never a lamp's own (kind << 8 | index), which is under 0x1300.
inline constexpr uint32_t kGroupBase = 0x10000u;
inline constexpr uint32_t kGroupHead = 0x10, kGroupBrakeTail = 0x20, kGroupTurnL = 0x21, kGroupTurnR = 0x22, kGroupTurnC = 0x23, kGroupReverse = 0x24, kGroupPolice = 0x30;  // + the kind - 7
inline uint64_t GroupId(const cars::Car& car, uint32_t code) { return light_id::PointId(CarId(car), kGroupBase | code); }

// One omni light of the cars' classes: colour x level x I (the game's lamp: plateau I, the distance law of the game's grid), out to `reach`, surface light only, the car's key and flag.
inline Light MakeCarOmni(const cars::Car& car, uint32_t code, const Group& g, const Look& look, float reach, LightSource source, float gain) {
  float centre[3];
  g.Centre(centre);
  Light l = MakeOmniLight(source, centre, look.rgb, g.level * look.intensity * gain, look.intensity, std::min(reach, light_laws::kMaxReach), GroupId(car, code));
  l.roles = kRoleMaskSurface;
  l.car_key = car.lights.car_key;
  l.flags = car.player ? kLightFlagPlayer : 0u;
  return l;
}

}  // namespace detail

// ---------------------------------------------------------------------------
// the picker's score
// ---------------------------------------------------------------------------

// The lamp picker's own score (volumetrics_lamps.h: light over distance squared plus the plateau): what a class is cut by. A light's colour holds the level already.
inline float Score(const Light& l, const float camera[3]) {
  const float brightness = std::max({l.color[0], l.color[1], l.color[2], 0.0f});
  return brightness / (detail::Dist2(l.pos, camera) + std::max(l.law_a, 1.0f));
}

// Best first, ties by id (never by the order the table gave them), at most `keep`; `importance` records the score.
inline void RankByScore(std::vector<Light>* lights, const float camera[3], size_t keep = 64) {
  for (Light& l : *lights) {
    l.importance = Score(l, camera);
  }
  std::sort(lights->begin(), lights->end(), [](const Light& a, const Light& b) {
    if (a.importance != b.importance) return a.importance > b.importance;
    return a.id < b.id;
  });
  if (lights->size() > keep) {
    lights->resize(keep);
  }
}

// ---------------------------------------------------------------------------
// the cars' lights
// ---------------------------------------------------------------------------

struct Candidates {
  std::vector<Light> police;
  std::vector<Light> head;
  std::vector<Light> rear;
  std::vector<Light> mod;
  std::vector<Light> neon;  // GB-NEON: whole cars' tubes, the nearest car first (the budget takes whole cars)
  void Clear() {
    police.clear();
    head.clear();
    rear.clear();
    mod.clear();
    neon.clear();
  }
  const std::vector<Light>& Of(SourceClass c) const {
    switch (c) {
      case SourceClass::kPolice: return police;
      case SourceClass::kHeadLamps: return head;
      case SourceClass::kRearLamps: return rear;
      case SourceClass::kNeon: return neon;
      default: return mod;
    }
  }
};

// Whether a lamp of kind 7 to 9 is a siren (see the top: kind 8 beside a headlight lamp is not).
inline bool IsSirenLamp(const cars::Car& car, const cars::Lamp& lamp) {
  if (lamp.family != pts::Family::kPolice) {
    return false;
  }
  if (lamp.kind != pts::kKindPolice1) {
    return true;
  }
  for (uint32_t i = 0; i < car.lamp_count; ++i) {
    const cars::Lamp& other = car.lamps[i];
    if ((other.family == pts::Family::kHead || other.family == pts::Family::kHiBeam) && detail::Dist2(other.world, lamp.world) < kPoliceNearHeadlamp * kPoliceNearHeadlamp) {
      return false;
    }
  }
  return true;
}

// The police lights of one car: one per lit kind (a kind's lit lamps share one light at their middle).
inline void MakePoliceLights(const cars::Car& car, Tier tier, float gain, std::vector<Light>* out) {
  std::array<detail::Group, 3> groups{};  // kind 7, 8, 9
  for (uint32_t i = 0; i < car.lamp_count; ++i) {
    const cars::Lamp& lamp = car.lamps[i];
    if (!lamp.on || !IsSirenLamp(car, lamp)) {
      continue;
    }
    groups[lamp.kind - pts::kKindPolice0].Add(lamp);
  }
  for (uint32_t k = 0; k < 3; ++k) {
    if (groups[k].n == 0) {
      continue;
    }
    const uint32_t kind = pts::kKindPolice0 + k;
    out->push_back(detail::MakeCarOmni(car, detail::kGroupPolice + k, groups[k], PoliceLook(kind), PoliceReach(tier), LightSource::kPolice, gain));
  }
}

// The rear lamps of one car: the brake and tail lamps together, each side of the turn signals, the reverse lamps.
inline void MakeRearLights(const cars::Car& car, Tier tier, float gain, std::vector<Light>* out) {
  detail::Group brake_tail, turn_left, turn_right, turn_centre, reverse;
  for (uint32_t i = 0; i < car.lamp_count; ++i) {
    const cars::Lamp& lamp = car.lamps[i];
    if (!lamp.on) {
      continue;
    }
    switch (lamp.family) {
      case pts::Family::kBrake:
      case pts::Family::kTail: brake_tail.Add(lamp); break;
      case pts::Family::kTurn: (lamp.side == cars::Side::kLeft ? turn_left : (lamp.side == cars::Side::kRight ? turn_right : turn_centre)).Add(lamp); break;
      case pts::Family::kReverse: reverse.Add(lamp); break;
      default: break;
    }
  }
  const float reach = RearReach(tier);
  const auto push = [&](uint32_t code, const detail::Group& g, const Look& look) {
    if (g.n != 0) {
      out->push_back(detail::MakeCarOmni(car, code, g, look, reach, LightSource::kOther, gain));
    }
  };
  push(detail::kGroupBrakeTail, brake_tail, kBrakeTailLook);
  push(detail::kGroupTurnL, turn_left, kTurnLook);
  push(detail::kGroupTurnR, turn_right, kTurnLook);
  push(detail::kGroupTurnC, turn_centre, kTurnLook);
  push(detail::kGroupReverse, reverse, kReverseLook);
}

// The middle of a car's lit head lamps and the way they shine (the mean of the lamps' own directions): false when none is lit or no direction is known (a car whose frame is not known).
inline bool HeadLampAim(const cars::Car& car, float centre[3], float dir[3], float* level) {
  detail::Group g;
  float sum_dir[3] = {0.0f, 0.0f, 0.0f};
  for (uint32_t i = 0; i < car.lamp_count; ++i) {
    const cars::Lamp& lamp = car.lamps[i];
    if (!lamp.on || (lamp.family != pts::Family::kHead && lamp.family != pts::Family::kHiBeam)) {
      continue;
    }
    g.Add(lamp);
    for (int k = 0; k < 3; ++k) {
      sum_dir[k] += lamp.emit[k];
    }
  }
  const float len = std::sqrt(sum_dir[0] * sum_dir[0] + sum_dir[1] * sum_dir[1] + sum_dir[2] * sum_dir[2]);
  if (g.n == 0 || !(len > 1.0e-3f)) {
    return false;
  }
  g.Centre(centre);
  for (int k = 0; k < 3; ++k) {
    dir[k] = sum_dir[k] / len;
  }
  *level = g.level;
  return true;
}

// Whether the frame already has the headlight of a car whose lit head lamps are at `centre`: a light of the headlight law and source within kCoverMetres of it (the rig's light is the middle of
// the pair, 0.12 m in front of the lens).
inline bool HeadlightCovered(const LightFrame& frame, const float centre[3]) {
  for (uint32_t i = 0; i < frame.count && i < kMaxLights; ++i) {
    const Light& l = frame.lights[i];
    if (l.id == kNoLightId || l.source != LightSource::kHeadlight || l.law != LightLaw::kHeadlight || !(l.range > 0.0f)) {
      continue;
    }
    if (detail::Dist2(l.pos, centre) <= kCoverMetres * kCoverMetres) {
      return true;
    }
  }
  return false;
}

// The head lamp light of one car the frame does not cover: the rig's beam (a spot of the headlight law: 38 / 12 degrees, 2 degrees down, the nudge in front of the lens) at the tier's reach and
// half the strength. Nothing for a car with no lit head lamp, no known direction or a headlight in the frame.
inline void MakeHeadLight(const cars::Car& car, const LightFrame& frame, Tier tier, float gain, std::vector<Light>* out) {
  float centre[3], dir[3], level = 0.0f;
  if (!HeadLampAim(car, centre, dir, &level) || HeadlightCovered(frame, centre)) {
    return;
  }
  dir[1] -= std::tan(kHeadDipDeg * detail::kDegToRad);
  const float len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
  Light l;
  for (int i = 0; i < 3; ++i) {
    l.dir[i] = dir[i] / len;
    l.pos[i] = centre[i] + l.dir[i] * kHeadNudge;
    l.color[i] = kHeadColor[i] * level * kHeadStrength * gain;
  }
  l.range = HeadReach(tier);
  l.cos_outer = std::cos(kHeadConeOuterDeg * detail::kDegToRad);
  l.cos_inner = std::cos(kHeadConeInnerDeg * detail::kDegToRad);
  l.law_a = light_laws::kHeadlightFalloffDistance;
  l.kind = LightKind::kSpot;
  l.source = LightSource::kHeadlight;
  l.law = LightLaw::kHeadlight;
  l.roles = kRoleMaskSurface;
  l.car_key = car.lights.car_key;
  l.flags = car.player ? kLightFlagPlayer : 0u;
  l.id = detail::GroupId(car, detail::kGroupHead);
  l.shadow.reach = l.range;
  out->push_back(l);
}

struct CarSourceInputs {
  const cars::CarLightsFrame* cars = nullptr;  // the manager's last poll (null: no cars)
  const LightFrame* frame = nullptr;           // the frame's lights, for the headlights the forward stage has (null: none)
  uint32_t sources = 0;                        // the mask: kSourcePolice, kSourceHeadLamps, kSourceRearLamps
  Tier tier = Tier::kMedium;
  float gain = 1.0f;                           // mcla_gpu_local_lights_car_gain
  float camera[3] = {0.0f, 0.0f, 0.0f};
};

// The police, head lamp and rear lamp lights of every car, each class ranked by score (best first), appended to `out`.
inline void GatherCarLights(const CarSourceInputs& in, Candidates* out) {
  if (in.cars == nullptr || (in.sources & kSourceCars) == 0) {
    return;
  }
  static const LightFrame kEmptyFrame{};
  const LightFrame& frame = in.frame != nullptr ? *in.frame : kEmptyFrame;
  const float gain = std::max(in.gain, 0.0f);
  for (uint32_t c = 0; c < in.cars->count && c < kMaxCars; ++c) {
    const cars::Car& car = in.cars->cars[c];
    if ((in.sources & kSourcePolice) != 0) {
      MakePoliceLights(car, in.tier, gain, &out->police);
    }
    if ((in.sources & kSourceHeadLamps) != 0) {
      MakeHeadLight(car, frame, in.tier, gain, &out->head);
    }
    if ((in.sources & kSourceRearLamps) != 0) {
      MakeRearLights(car, in.tier, gain, &out->rear);
    }
  }
  RankByScore(&out->police, in.camera);
  RankByScore(&out->head, in.camera);
  RankByScore(&out->rear, in.camera);
}

// ---------------------------------------------------------------------------
// the cars' underglow (GB-NEON)
// ---------------------------------------------------------------------------

// The omni light i of n along a car's tube, at `pos`: the game's colour, the plateau from the reach, the tube's strength shared by its n points, the car's key and flag, surface light only (no
// shadow views: the underglow is a glow under the car, not a lamp that casts).
inline Light MakeNeonLight(const nl::NeonCar& car, uint32_t i, uint32_t n, const float pos[3], Tier tier, float gain) {
  const float reach = NeonReach(tier);
  const float plateau = kNeonCutShare * reach * reach;
  Light l = MakeOmniLight(LightSource::kNeon, pos, car.colour, plateau * std::max(gain, 0.0f) * kNeonTubeGain / float(std::max<uint32_t>(n, 1u)), plateau, reach,
                          light_id::NeonCarId(car.object, i));
  l.roles = kRoleMaskSurface;
  l.car_key = cars::PointCarKey(car.object);
  l.flags = car.player ? kLightFlagPlayer : 0u;
  return l;
}

// The lights of one placed tube: `points` of them at the middles of equal parts of the tube from A to B (one at the middle when the tube is a point: kPointTube).
inline void MakeNeonTubeLights(const nl::NeonCar& car, const nl::Tube& tube, uint32_t points, Tier tier, float gain, std::vector<Light>* out) {
  const uint32_t n = tube.length < nl::kPointTube ? 1u : std::clamp<uint32_t>(points, 1u, nl::kTubePoints);
  for (uint32_t i = 0; i < n; ++i) {
    const float t = (float(i) + 0.5f) / float(n);
    float pos[3];
    for (int k = 0; k < 3; ++k) {
      pos[k] = tube.a[k] + (tube.b[k] - tube.a[k]) * t;
    }
    out->push_back(MakeNeonLight(car, i, n, pos, tier, gain));
  }
}

struct NeonSourceInputs {
  const std::vector<nl::NeonCar>* cars = nullptr;  // the table's recent cars (null: none)
  Tier tier = Tier::kMedium;
  float gain = 1.0f;                               // mcla_gpu_neon_light_gain
  float camera[3] = {0.0f, 0.0f, 0.0f};
  // Whether a light can be seen: some pixel of the picture is inside its reach (the pass's ScissorOfLight is not empty). Empty: every light can. The budget takes the nearest cars whose tube has
  // a light that can be seen, so a car behind the camera takes none of it (the header's rule for every class: a car behind the camera takes none of the budget).
  std::function<bool(const Light&)> can_be_seen;
};

// What a gather saw, for the log: the cars the table gave, the ones with their neon off, too dim, not placeable (N1), too far (`distant`), out of the picture (`unseen`), lit and cut by the budget; how the
// last placed tube was read.
struct NeonReport {
  uint32_t seen = 0;
  uint32_t off = 0;
  uint32_t dim = 0;
  uint32_t unplaced = 0;
  uint32_t distant = 0;
  uint32_t unseen = 0;
  uint32_t lit = 0;
  uint32_t cut = 0;
  nl::Space space = nl::Space::kNone;
};

// The underglow lights of the nearest lit neon cars, whole tubes, nearest car first, appended to `out` (`report` may be null). A car with its neon off or dim, whose tube the four points do not
// place (ResolveTube), beyond kNeonSeenMetres, or (when `in.can_be_seen` is set) with no light of its tube in the picture, makes none and takes no budget.
inline void GatherNeonLights(const NeonSourceInputs& in, std::vector<Light>* out, NeonReport* report = nullptr) {
  NeonReport local;
  NeonReport& r = report != nullptr ? *report : local;
  r = NeonReport{};
  if (in.cars == nullptr) {
    return;
  }
  struct Placed {
    float distance;
    const nl::NeonCar* car;
    nl::Tube tube;
  };
  std::vector<Placed> placed;
  for (const nl::NeonCar& car : *in.cars) {
    ++r.seen;
    if (!car.on) {
      ++r.off;
      continue;
    }
    if (!(nl::Brightest(car.colour) >= kNeonMinColour)) {
      ++r.dim;
      continue;
    }
    nl::Tube tube;
    if (!nl::ResolveTube(car, &tube)) {
      ++r.unplaced;
      continue;
    }
    const float mid[3] = {0.5f * (tube.a[0] + tube.b[0]), 0.5f * (tube.a[1] + tube.b[1]), 0.5f * (tube.a[2] + tube.b[2])};
    const float distance = nl::detail::Dist3(mid, in.camera);
    if (!(distance <= kNeonSeenMetres)) {
      ++r.distant;
      continue;
    }
    placed.push_back({distance, &car, tube});
  }
  std::sort(placed.begin(), placed.end(), [](const Placed& a, const Placed& b) {
    if (a.distance != b.distance) return a.distance < b.distance;
    return a.car->object < b.car->object;
  });
  const ClassBudget budget = BudgetOf(in.tier);
  std::vector<Light> tube_lights;
  for (const Placed& p : placed) {
    if (r.lit >= budget.neon_cars) {
      ++r.cut;
      continue;
    }
    tube_lights.clear();
    MakeNeonTubeLights(*p.car, p.tube, budget.neon_points, in.tier, in.gain, &tube_lights);
    if (in.can_be_seen && std::none_of(tube_lights.begin(), tube_lights.end(), [&](const Light& l) { return in.can_be_seen(l); })) {
      ++r.unseen;  // behind the camera or out of the picture: no budget for it, the next car may have it
      continue;
    }
    ++r.lit;
    r.space = p.tube.space;
    out->insert(out->end(), tube_lights.begin(), tube_lights.end());
  }
}

// Whether the gather would light at least one car with these inputs: some car with its neon on and bright, placed, within kNeonSeenMetres, one of whose lights can be seen (`in.can_be_seen`,
// or any light when it is empty). It IS the gather's rule (the tier's car budget is at least one car), so the planes the light pass asks for are made exactly when it will have a neon light to
// draw: the G-buffer's planes cost about 0.22 ms at 1080p on D3D12 and a lit car 100 m away, or behind the camera, has nothing in the picture to use them for (NEONALL-REVIEW M2).
inline bool AnyNeonLightSeen(const NeonSourceInputs& in) {
  std::vector<Light> lights;
  NeonReport report;
  GatherNeonLights(in, &lights, &report);
  return report.lit != 0;
}

// The same question against a camera's picture (the pass's own visibility test: the scissor of the light's reach is not empty), for the frame's start: `view` is the last frame's camera, widened
// (ProbeViewOf).
inline bool NeonReachesView(const std::vector<nl::NeonCar>& cars, Tier tier, float gain, const ProbeView& view) {
  NeonSourceInputs in;
  in.cars = &cars;
  in.tier = tier;
  in.gain = gain;
  for (int i = 0; i < 3; ++i) {
    in.camera[i] = view.rows.position[i];
  }
  in.can_be_seen = [&view](const Light& l) { return !ScissorOfLight(l, view.rows, view.tan_x, view.tan_y, view.width, view.height).Empty(); };
  return AnyNeonLightSeen(in);
}

// ---------------------------------------------------------------------------
// the mods' lamps
// ---------------------------------------------------------------------------

// The lamp A/B's own path (pass mode): the lamps of the set the camera is over, allowed to add surface light (the grid was built with them off). Nothing in the other two modes.
inline void HandedOverLamps(Ab ab, const mls::Set* active, const float camera[3], std::vector<Light>* out) {
  if (ab != Ab::kPass || active == nullptr) {
    return;
  }
  std::vector<Light> lamps;
  mls::Candidates(*active, camera, kMaxPassLights, &lamps);
  for (const Light& lamp : lamps) {
    out->push_back(AsPassLight(lamp));
  }
}

// How far past a lamp's own reach the camera may be for the pass to still take it (m). The sets here are the ones the camera is NOT over, which is more than 100 m from the lamps (a mod's
// rectangle is its lamps' extent plus 100 m and a lamp reaches sqrt(100 I) = 74 m at I 55): the lamp's pool on a wall in the distance is what a camera outside the rectangle sees, so the
// manager's 20 m (a lamp's own light must reach the camera's surroundings) is too short here. Seen or not is the scissor's question, after this (the pass's own `unseen`).
inline constexpr float kModMargin = 200.0f;

// The lamps of the sets the camera is NOT over (their grid is not the one the shaders read): within their reach and kModMargin of the camera, the best by score, as pass lights. `active` is the
// set the camera is over (compared by pointer: the feed hands out the objects it built), null over the city. None in the A/B's control mode (the lamps are written off in every grid and
// nothing may light them).
inline void GatherModLamps(const mls::BuiltSets& built, const mls::Set* active, Ab ab, const float camera[3], std::vector<Light>* out) {
  if (!built || ab == Ab::kNowhere) {
    return;
  }
  std::vector<Light> lamps;
  for (const mls::SetPtr& set : *built) {
    if (!set || set.get() == active) {
      continue;
    }
    mls::Candidates(*set, camera, kMaxPassLights, &lamps, kModMargin);
  }
  for (const Light& lamp : lamps) {
    out->push_back(AsPassLight(lamp));
  }
  RankByScore(out, camera);
}

// ---------------------------------------------------------------------------
// the selection
// ---------------------------------------------------------------------------

// SelectLights' lights (the frame's surface lights up to the frame class's cap, the handed over ones), then the classes in priority order police, mods, neon, head lamps, rear lamps: each, when its bit is in `sources`, up to its tier
// cap of what `cand` holds (already ranked and already without the lights that cannot be seen), and up to the pass's 32 over all. Without candidates this is SelectLights.
inline Selection SelectPassLights(const LightFrame& frame, uint32_t sources, const Light* extra, uint32_t extra_count, const Candidates& cand, Tier tier) {
  const ClassBudget budget = BudgetOf(tier);
  Selection s = SelectLights(frame, sources, extra, extra_count, budget.frame);
  const auto append = [&](uint32_t bit, SourceClass cls) {
    if ((sources & bit) == 0) {
      return;
    }
    const uint32_t cap = CapOf(budget, cls);
    uint32_t taken = 0;
    for (const Light& l : cand.Of(cls)) {
      if (taken >= cap) {
        ++s.cut_class[size_t(cls)];
        continue;
      }
      if (s.count >= kMaxPassLights) {
        ++s.dropped;
        ++s.cut_class[size_t(cls)];
        continue;
      }
      s.lights[s.count++] = l;
      ++taken;
      ++s.from_class[size_t(cls)];
    }
  };
  append(kSourcePolice, SourceClass::kPolice);
  append(kSourceMod, SourceClass::kMod);
  append(kSourceNeon, SourceClass::kNeon);
  append(kSourceHeadLamps, SourceClass::kHeadLamps);
  append(kSourceRearLamps, SourceClass::kRearLamps);
  return s;
}

}  // namespace larecomp::gpu::local_lights
