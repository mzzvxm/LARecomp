#pragma once
// ===========================================================================
// MCLA GPU renderer -- the EMITTERS of the ray traced bounce light: the lights that are not the world's own (neon, headlights, tail lamps, police bars, mod lamps) and light what a bounce ray hits
// (GI-BOUNCE-RT, perf/GI-BOUNCE.md; the shader side is hlsl/rt_emitters.hlsli and the library's CityHitEmitters in hlsl/city_hit_shade.hlsli; its C++ twin is city_hit_shade_rules.h)
// ===========================================================================
// WHAT BOUNCED BEFORE. The hit shading of the bounce light (the probe field's rays and the per-pixel pass; the reflections' city hits too) lit a hit by the sun, the sky, the street lamps and fill lights of
// the world frame's grid (one of them with a shadow ray), the glow of lit windows, neon SIGNS and lamp lenses, and the field's own light one frame old. A car's own lights were not in it: the per-pixel pass
// added the light manager's headlights without a shadow ray, and only while something else made the manager run (it is passive otherwise); the probes never had them; neon, tail lamps and police lights were
// nowhere. What the deferred light pass lights DIRECTLY (local_lights.h) is what this adds to the bounce, by the same laws and units, so that a wall lit by a car's neon is lit by it twice over: by the pass
// as the picture's own light, and by this as the light the wall sends back to the road and the car beside it.
//
// THE LIST. Once a frame (RtWorldFrame::OnTrigger) the lights of the light pass's classes are gathered by the pass's own functions (local_lights_sources.h: GatherNeonLights, GatherCarLights, GatherModLamps,
// SelectPassLights), one budget per class and tier (the same tier table as the pass: the rung and the level of the bounce light choose it: TierFor), at most kMaxEmitters (32). A light farther than
// kMaxDistance from the camera is left out BEFORE the budgets count it, as the pass leaves out the lights no pixel is inside the reach of (a car 300 m away takes no slot). Four GROUPS (kGroup*) each
// have a debug switch (mcla_gpu_gi_bounce_headlights / _neon / _rear / _other: perf/GI-BOUNCE.md), a group that is switched off is not gathered at all.
//   UNITS. The pass multiplies the law's term by night (the game's c63.w, not for the headlight law: its colour has its own night factor), the gain, the exposure and the height factor. The bounce light is in
//   the paint's units (the exposure is the frame's, p.units), so the colour of a light here is the Light's colour x night (the fill lights' night factor: what neon, signs and lit windows follow, row 47 y of
//   the world rows) x the pass's gain; the law (light_laws.h kHeadlight / kGameLamp / kOmni) is evaluated on the GPU from the same GpuLight row the pass reads, so a bounce and a direct term agree. The height
//   factor (a few per cent brighter above the street) is not applied: the bounce is soft.
//   MOVING LIGHTS. The probe field is fixed to the world and blends 33 updates (hysteresis 0.97): a light that moves at the speed of a car would leave a smear of its colour along its path that fades for
//   a second, and the new value would count 3 per cent. So the list is ordered STATIC FIRST (a light whose position changed by under kMovingSpeed m/s: a parked car, one at a stop) and the probes read only
//   the first `static count` of it (row 44 z of the world rows); the per-pixel pass and the reflections read all. A light seen for the first time is moving until a second sample says otherwise.
//   At HIGH (the field alone, no per-pixel pass) a moving car's lights therefore do not bounce through this path: the screen-space bounce light is what lights those (perf/GI-BOUNCE.md).
// THE LAYOUT. The emitters are GpuLight rows (light_frame.h: four float4 each, WORLD space, as the light pass reads them) in gRtLamps after the lamps' rows: row base + 4 i + k (k 0..3); the base, the count, the
// static count and the groups are row 44 of gRtWorld (rt_world_frame_rules.h kRowEmitters).
//
// CHECK (nothing here was run against the game):
//   E1  the night factor is the world frame's fill lights' (lamp_table.h EstimateNightFactor), a stand-in for the shaders' `c63.w` that the light pass votes for: where the two differ at dusk the bounce of the game
//       lamps' classes comes on early or late against their direct light (the headlight law is not affected).
//   E2  the numbers of the motion (1.5 m/s, 0.75 m/s, 6 samples) and of the reach (100 m) are values, not measurements: the neon car's static / moving split and the lights the far cascades miss are judged by eye.
//   E3  the cars' lights are the manager's last poll (one frame old at the trigger), the neon table's the hook's last 100 ms: a light's ray starts one frame's motion off its car.
//   E4  the lights the manager does not run for (it is passive unless a consumer asks: WantRt) are not in the list: with the manager off a car's head, rear and police lamps are missing, the neon and the mods' lamps are not.
//
// Pure: no renderer, no guest, no SDK. rt_emitters_rules_test.cpp holds it.
// ===========================================================================

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "car_lights.h"
#include "light_frame.h"
#include "local_lights_rules.h"
#include "local_lights_sources.h"
#include "mod_light_source.h"
#include "neon_light.h"

namespace mcla::gpu::rt_emitters {

namespace lg = ::larecomp::gpu;
namespace ll = ::larecomp::gpu::local_lights;
namespace nl = ::larecomp::gpu::neon_light;
namespace cars = ::larecomp::gpu::car_lights;
namespace mls = ::larecomp::gpu::mod_light_source;

// ---------------------------------------------------------------------------
// the groups and the limits
// ---------------------------------------------------------------------------

inline constexpr uint32_t kGroupHeadlights = 1u << 0;  // the frame's surface lights (the rig's headlights) and the head lamps of the cars the rig does not light
inline constexpr uint32_t kGroupNeon = 1u << 1;        // the cars' underglow
inline constexpr uint32_t kGroupRear = 1u << 2;        // brake, tail, turn signal and reverse lamps
inline constexpr uint32_t kGroupOther = 1u << 3;       // police light bars, the mods' lamps outside their grid
inline constexpr uint32_t kGroupAll = kGroupHeadlights | kGroupNeon | kGroupRear | kGroupOther;
inline constexpr uint32_t kGroupCount = 4;

inline constexpr const char* GroupName(uint32_t bit) {
  return bit == kGroupHeadlights ? "headlights" : (bit == kGroupNeon ? "neon" : (bit == kGroupRear ? "rear lamps" : (bit == kGroupOther ? "other" : "none")));
}
constexpr uint32_t GroupIndex(uint32_t bit) { return bit == kGroupHeadlights ? 0u : (bit == kGroupNeon ? 1u : (bit == kGroupRear ? 2u : 3u)); }

// The four debug switches as the group mask.
struct Switches {
  bool headlights = true;
  bool neon = true;
  bool rear = true;
  bool other = true;
};
constexpr uint32_t MaskOf(const Switches& s) {
  return (s.headlights ? kGroupHeadlights : 0u) | (s.neon ? kGroupNeon : 0u) | (s.rear ? kGroupRear : 0u) | (s.other ? kGroupOther : 0u);
}

inline constexpr uint32_t kMaxEmitters = ll::kMaxPassLights;  // 32: the pass's own limit
inline constexpr uint32_t kRowsEach = 4;                      // a GpuLight: four float4
inline constexpr uint32_t kWordsEach = kRowsEach * 4;
inline constexpr uint32_t kMaxEmitterRows = kMaxEmitters * kRowsEach;
static_assert(sizeof(lg::GpuLight) == kWordsEach * 4, "an emitter is one GpuLight row set");

// A light farther than this from the camera (m) is not gathered: a headlight reaches 70 m, a bounce ray 12 m from the pixel it starts at, and the field's far cascades are too coarse to show a car's
// light at all.
inline constexpr float kMaxDistance = 100.0f;
// A colour whose brightest channel is below this adds nothing a picture could show: it takes no slot (the dark half of a neon pulse, a lamp by day).
inline constexpr float kMinColour = 0.003f;
// The speed (m/s) over which a light is MOVING, and under which it must stay for kStillSamples samples in a row to be static again. A parked car's lights jitter by a few millimetres; a car at 1.5 m/s is
// walking pace.
inline constexpr float kMovingSpeed = 1.5f;
inline constexpr float kStillSpeed = 0.75f;
inline constexpr uint32_t kStillSamples = 6;
// Two samples of one light farther apart than this (s) are not a speed: the light was out of the list (a car out of range, a pause).
inline constexpr double kMaxSampleGap = 0.5;

// Match the emitter budget to the GI level.
constexpr ll::Tier TierFor(bool ultra, bool) { return ultra ? ll::Tier::kUltra : ll::Tier::kHigh; }

// ---------------------------------------------------------------------------
// which group a light is of
// ---------------------------------------------------------------------------

// The head lamps of the cars the rig lights and the ones it does not are both `kHeadlight` source lights; a rear lamp is a `kOther` source light of a car (car_key); the police and the mods have their own
// sources; anything else (the test lamp, a handed over lamp: a debug light) is `other`.
inline uint32_t GroupOf(const lg::Light& l) {
  switch (l.source) {
    case lg::LightSource::kHeadlight: return kGroupHeadlights;
    case lg::LightSource::kNeon: return kGroupNeon;
    case lg::LightSource::kOther: return l.car_key != 0 ? kGroupRear : kGroupOther;
    default: return kGroupOther;
  }
}

// ---------------------------------------------------------------------------
// moving or not
// ---------------------------------------------------------------------------

// The speed of every light of the list, by its id, from its place in two frames: Begin before a frame's notes, Note for each light, End after (a light not noted is forgotten). A light seen for the first time,
// after a gap, or without an id is MOVING (nobody knows yet); one that is slower than kStillSpeed for kStillSamples samples in a row is static; one faster than kMovingSpeed is moving at once.
class MotionTracker {
 public:
  void Begin() {
    for (Entry& e : entries_) {
      e.seen = false;
    }
  }
  bool Note(uint64_t id, const float pos[3], double now_s) {
    if (id == lg::kNoLightId || !std::isfinite(pos[0]) || !std::isfinite(pos[1]) || !std::isfinite(pos[2])) {
      return true;
    }
    for (Entry& e : entries_) {
      if (e.id != id) {
        continue;
      }
      e.seen = true;
      const double dt = now_s - e.time;
      bool moving = e.moving;
      if (!(dt > 1.0e-4) || dt > kMaxSampleGap) {
        moving = true;  // a gap (or a clock that did not advance): no speed to read
        e.still = 0;
      } else {
        const double dx = double(pos[0]) - double(e.pos[0]), dy = double(pos[1]) - double(e.pos[1]), dz = double(pos[2]) - double(e.pos[2]);
        const double speed = std::sqrt(dx * dx + dy * dy + dz * dz) / dt;
        if (speed > double(kMovingSpeed)) {
          moving = true;
          e.still = 0;
        } else if (speed < double(kStillSpeed)) {
          e.still = std::min(e.still + 1u, kStillSamples);
          if (e.still >= kStillSamples) {
            moving = false;
          }
        } else {
          e.still = 0;  // between the two: the state stays, the count of still samples starts again
        }
      }
      e.moving = moving;
      e.time = now_s;
      std::memcpy(e.pos, pos, sizeof(e.pos));
      return moving;
    }
    Entry e;
    e.id = id;
    e.time = now_s;
    std::memcpy(e.pos, pos, sizeof(e.pos));
    e.seen = true;
    entries_.push_back(e);
    return true;
  }
  void End() {
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [](const Entry& e) { return !e.seen; }), entries_.end());
  }
  void Clear() { entries_.clear(); }
  size_t size() const { return entries_.size(); }

 private:
  struct Entry {
    uint64_t id = 0;
    float pos[3] = {0.0f, 0.0f, 0.0f};
    double time = 0.0;
    bool moving = true;
    bool seen = false;
    uint32_t still = 0;
  };
  std::vector<Entry> entries_;
};

// ---------------------------------------------------------------------------
// the list
// ---------------------------------------------------------------------------

struct Emitter {
  lg::Light light;       // world space, the colour already times night and the gain
  uint32_t group = 0;    // kGroup*
  bool moving = true;
};

struct BuildInputs {
  const lg::LightFrame* frame = nullptr;                    // the light manager's frame (the rig's headlights); null: none
  const cars::CarLightsFrame* cars = nullptr;                // the manager's last poll of the cars' light points; null: none
  const std::vector<nl::NeonCar>* neon = nullptr;           // the neon table's recent cars; null: none
  mls::BuiltSets built;                                      // the mods' built sets (mod lamps outside their grid)
  const mls::Set* active_mod = nullptr;                      // the set the camera is over (its grid lights it)
  uint32_t groups = kGroupAll;                               // the switches
  ll::Tier tier = ll::Tier::kHigh;
  float camera[3] = {0.0f, 0.0f, 0.0f};                      // the camera's place in the game's world
  float night = 0.0f;                                        // the fill lights' night factor 0..1 (the schedule's): the game lamps' laws are times this
  float gain = 1.0f;                                         // mcla_gpu_local_lights_gain: the pass's own multiplier of every light
  float car_gain = 1.0f;                                     // mcla_gpu_local_lights_car_gain
  float neon_gain = 1.0f;                                    // mcla_gpu_neon_light_gain
  double now_s = 0.0;                                        // the scene clock, seconds (the motion's)
};

struct Census {
  uint32_t groups = 0;                          // the switches' mask this frame
  uint32_t candidates = 0;                      // lights the gathers made, before the filters and the budgets
  uint32_t selected = 0;                        // lights the budgets took (all of them go in the list: the filters ran before)
  uint32_t dropped_far = 0;                     // lights beyond kMaxDistance (before the budgets)
  uint32_t dropped_dark = 0;                    // with no colour left after the night factor and the gain, or with a number in it that is not one (before the budgets)
  uint32_t by_group[kGroupCount] = {0, 0, 0, 0};
  uint32_t still = 0;                           // static emitters (the probes' share)
  uint32_t moving = 0;
};

struct BuildResult {
  std::vector<Emitter> list;  // static first, then moving; each in the selection's order
  Census census;
  uint32_t static_count() const { return census.still; }
};

namespace detail {
inline float Dist2(const float a[3], const float b[3]) {
  const float d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
  return d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
}
inline bool Far(const lg::Light& l, const float camera[3]) { return !(Dist2(l.pos, camera) <= kMaxDistance * kMaxDistance); }
// Every number the GPU's law reads is a number: one NaN in a light would make the luminance of a hit's whole sum a NaN and the library would drop all the emitters' light there.
inline bool Finite(const lg::Light& l) {
  for (int i = 0; i < 3; ++i) {
    if (!std::isfinite(l.pos[i]) || !std::isfinite(l.dir[i]) || !std::isfinite(l.color[i])) {
      return false;
    }
  }
  return std::isfinite(l.range) && std::isfinite(l.cos_outer) && std::isfinite(l.cos_inner) && std::isfinite(l.law_a);
}
// What a light must pass BEFORE it may take a slot of its class's budget (the review's lesson: a budget counted before the reach and the visibility is spent on lights that add nothing): near enough to
// matter, made of numbers, and some colour left once the game lamps' night factor and the pass's gain are in it. Folds the factor into the light's colour. The counts say why a light went.
struct Filter {
  const float* camera;
  float night;  // 0..1
  float gain;   // >= 0
  Census* census;
  bool Keep(lg::Light* l) const {
    if (Far(*l, camera)) {
      ++census->dropped_far;
      return false;
    }
    if (!Finite(*l)) {
      ++census->dropped_dark;
      return false;
    }
    const float scale = gain * (l->law == lg::LightLaw::kHeadlight ? 1.0f : night);
    float brightest = 0.0f;
    for (int k = 0; k < 3; ++k) {
      const float v = l->color[k] * scale;
      l->color[k] = std::isfinite(v) && v > 0.0f ? v : 0.0f;
      brightest = std::max(brightest, l->color[k]);
    }
    if (!(brightest >= kMinColour) || !(l->range > 0.0f)) {
      ++census->dropped_dark;
      return false;
    }
    return true;
  }
  void Keep(std::vector<lg::Light>* v) const {
    size_t kept = 0;
    for (lg::Light& l : *v) {
      if (Keep(&l)) {
        (*v)[kept++] = l;
      }
    }
    v->resize(kept);
  }
};
}  // namespace detail

// The light pass's sources the groups stand for.
constexpr uint32_t SourcesOf(uint32_t groups) {
  uint32_t s = 0;
  if ((groups & kGroupHeadlights) != 0) s |= ll::kSourceFrame | ll::kSourceHeadLamps;
  if ((groups & kGroupNeon) != 0) s |= ll::kSourceNeon;
  if ((groups & kGroupRear) != 0) s |= ll::kSourceRearLamps;
  if ((groups & kGroupOther) != 0) s |= ll::kSourcePolice | ll::kSourceMod;
  return s;
}

// The frame's emitters. `tracker` (may be null: everything is moving) is told every light of the list once. The order is deterministic: the same inputs give the same list.
inline void Build(const BuildInputs& in, MotionTracker* tracker, BuildResult* out) {
  *out = BuildResult{};
  Census& c = out->census;
  c.groups = in.groups & kGroupAll;
  if (tracker != nullptr) {
    tracker->Begin();
  }
  const uint32_t sources = SourcesOf(c.groups);
  if (sources != 0) {
    ll::Candidates cand;
    if ((sources & ll::kSourceNeon) != 0 && in.neon != nullptr) {
      ll::NeonSourceInputs neon_in;
      neon_in.cars = in.neon;
      neon_in.tier = in.tier;
      neon_in.gain = in.neon_gain;
      for (int i = 0; i < 3; ++i) {
        neon_in.camera[i] = in.camera[i];
      }
      ll::GatherNeonLights(neon_in, &cand.neon);
    }
    if ((sources & ll::kSourceCars) != 0 && in.cars != nullptr) {
      ll::CarSourceInputs cars_in;
      cars_in.cars = in.cars;
      cars_in.frame = in.frame;
      cars_in.sources = sources & ll::kSourceCars;
      cars_in.tier = in.tier;
      cars_in.gain = in.car_gain;
      for (int i = 0; i < 3; ++i) {
        cars_in.camera[i] = in.camera[i];
      }
      ll::GatherCarLights(cars_in, &cand);
    }
    if ((sources & ll::kSourceMod) != 0 && in.built) {
      ll::GatherModLamps(in.built, in.active_mod, ll::Ab::kGrid, in.camera, &cand.mod);
    }
    c.candidates = uint32_t(cand.police.size() + cand.head.size() + cand.rear.size() + cand.mod.size() + cand.neon.size());
    // BEFORE the budgets: the lights too far to matter, with a number in them that is not one, or with no colour left take no slot (the pass's rule for the lights no pixel is inside the reach of; the night
    // factor and the gain are folded into the colours here, so that by day the game lamps are gone before they are counted)
    detail::Filter filter{in.camera, std::clamp(std::isfinite(in.night) ? in.night : 0.0f, 0.0f, 1.0f), std::max(std::isfinite(in.gain) ? in.gain : 0.0f, 0.0f), &c};
    filter.Keep(&cand.police);
    filter.Keep(&cand.head);
    filter.Keep(&cand.rear);
    filter.Keep(&cand.mod);
    filter.Keep(&cand.neon);
    // the frame's lights (the rig's headlights: picked by the view's frustum, not by distance) the same way, on a copy in which the ones that fail are holes
    lg::LightFrame filtered;
    if (in.frame != nullptr) {
      filtered = *in.frame;
      for (uint32_t i = 0; i < filtered.count && i < lg::kMaxLights; ++i) {
        if (filtered.lights[i].id != lg::kNoLightId && !filter.Keep(&filtered.lights[i])) {
          filtered.lights[i].id = lg::kNoLightId;
        }
      }
    }
    const ll::Selection sel = ll::SelectPassLights(filtered, sources, nullptr, 0, cand, in.tier);
    c.selected = sel.count;
    std::vector<Emitter> still, moving;
    for (uint32_t i = 0; i < sel.count && i < kMaxEmitters; ++i) {
      Emitter e;
      e.light = sel.lights[i];
      e.group = GroupOf(e.light);
      e.moving = tracker != nullptr ? tracker->Note(e.light.id, e.light.pos, in.now_s) : true;
      (e.moving ? moving : still).push_back(e);
    }
    out->list = std::move(still);
    c.still = uint32_t(out->list.size());
    out->list.insert(out->list.end(), moving.begin(), moving.end());
    c.moving = uint32_t(moving.size());
    for (const Emitter& e : out->list) {
      ++c.by_group[GroupIndex(e.group)];
    }
  }
  if (tracker != nullptr) {
    tracker->End();
  }
}

// The emitters as the words gRtLamps holds: kWordsEach words each (the GpuLight rows, world space). A light's flags are the light pass's own (the frame's flags the GPU twin carries).
inline void PackWords(const std::vector<Emitter>& list, std::vector<uint32_t>* words) {
  for (const Emitter& e : list) {
    const lg::GpuLight g = lg::PackGpuLight(e.light);
    const size_t at = words->size();
    words->resize(at + kWordsEach);
    std::memcpy(words->data() + at, &g, sizeof(g));
  }
}

inline std::string CensusText(const Census& c) {
  char text[384];
  std::snprintf(text, sizeof(text), "emitters %u (static %u, moving %u): headlights %u, neon %u, rear lamps %u, other %u; %u gathered, %u taken by the budgets, %u too far, %u dark; groups %u%u%u%u (headlights, neon, rear, other)",
                c.still + c.moving, c.still, c.moving, c.by_group[0], c.by_group[1], c.by_group[2], c.by_group[3], c.candidates, c.selected, c.dropped_far, c.dropped_dark, (c.groups & kGroupHeadlights) != 0 ? 1u : 0u,
                (c.groups & kGroupNeon) != 0 ? 1u : 0u, (c.groups & kGroupRear) != 0 ? 1u : 0u, (c.groups & kGroupOther) != 0 ? 1u : 0u);
  return text;
}

}  // namespace mcla::gpu::rt_emitters
