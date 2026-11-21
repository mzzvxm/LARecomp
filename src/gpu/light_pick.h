#pragma once
// ===========================================================================
// MCLA GPU renderer -- the one picker of the lights: which lights count, in what order, with what weight (perf/LIGHT-UNIFY.md section 3.3 and step U3, package LU-3)
// ===========================================================================
// WHAT IT IS. Two places pick lights for the picture today and a third and fourth are on their branches: the headlight rig's Tracker (four lights for the lit shaders), local_shadow_rules.h's
// ViewPicker (the cars that get shadow views), the volume's LampPicker (volumetrics_lamps.h) and the lamp shadows' Picker (feat/lamp-shadows). They rank by the same idea (a light is worth
// what it adds to the picture, nearer is more, a light that holds a place counts more than it is so that two equal ones do not trade) and differ in the numbers and the clock. This header
// is the rule written once, from the HL-EDGE-FADE rules (light_influence.h: how much of a beam is in the view, a distance fade, a car that left through the edge keeps its light while
// its beam is in the view), in SCENE-CLOCK SECONDS (a fade is the same time at 30, 60 and 144 FPS; the rig counted guest frames):
//
//   importance   1 / (1 + (distance / half_distance)^2)  x  the influence (a view: ReachWeight(reach), 0.35 with no reach, 1 in the view; no view: 0.35 for a light off the screen)
//   score        the importance (a player's light: 1e9, minus a tie-break), x 0.5^(age / linger) for a light the source has not reported for `age` seconds, x keep_bonus for a light that
//                held a place in the last pick
//   rank         by score, then by the lower id; the best `budget` are taken
//   fade         (the rig's lights) the weight of a picked light steps toward its target by rate x the scene clock's dt: the target is 1, less with the distance (DistanceFade) and, for a car
//                the game stopped drawing because it left through the picture's edge (a COAST), less as its beam leaves the view and over the end of the coast
//
// The MECHANISM is unified, the NUMBERS are today's: HeadlightTuning() is the rig's (keep 1.5, 0.12 a guest frame at 60 FPS = 7.2 a second, linger 6 frames = 0.1 s, drop 24 = 0.4 s,
// coast 90 = 1.5 s), ShadowCarTuning() is the shadow views' (keep 1.3, a car's views come and go together, no fade); LampTuning() is the lamps' (RESERVED: the volume's and the lamp
// shadows' pickers keep their own until the lamps move here: the constants are checked against volumetrics_lamps.h). At the rate the rig ran at (60 guest frames a second) the unified pick
// is the legacy pick: headlight_rig_test.cpp and local_shadow_rules_test.cpp run every check of the two old pickers through BOTH, and light_pick_test.cpp replays recorded and
// simulated inputs through both and compares every pick and fade.
//
// WHAT IT KEEPS. By id: whether the light held a place in the last pick (the hysteresis) and its fade. The SOURCE keeps what is not picking: the rig's Tracker matches the cars seen to
// tracks, gives each a velocity, moves a track the game stopped drawing on by it and says whether it left through the picture's edge; the picker is told all of it in the Entry. A light that
// is not in a frame's entries is forgotten: the caller hands in every light it still wants kept (the rig hands in every track; it drops one when its age passes `drop_s` and its fade is out).
//
// WHO OWNS IT. LightPicker (below) is what the LightManager holds: one Picker for each role (the rig's headlights, the cars with shadow views) and the one budget (light_budget.h). The rig's
// Tracker and the ViewPicker are ADAPTERS: they find and rank nothing themselves in the unified mode, they hand their entries to the manager's picker. A standalone Tracker or ViewPicker (a
// unit test) owns a picker of its own. mcla_gpu_light_pick (debug) = legacy runs the old code of both, which stays for one release as the A/B.
//
// Pure: plain numbers, no renderer, no guest. light_pick_test.cpp checks it.
// ===========================================================================

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

#include "light_budget.h"
#include "light_influence.h"

namespace mcla::gpu::light_pick {

// ---------------------------------------------------------------------------
// the clock
// ---------------------------------------------------------------------------

// The rate the rig's frame counts were tuned at: a guest frame at 60 FPS. A caller with no scene clock (a test) uses frame x kReferenceStep as the time, which is the legacy pick.
inline constexpr double kReferenceStep = 1.0 / 60.0;
// The longest step a fade takes at once: a stall of a second fades a light by a quarter second's worth, not by a second's (and never by less than nothing).
inline constexpr double kMaxStepSeconds = 0.25;
// Times are compared with this much slack, so that a limit written in whole frames at the reference rate (24 frames = 0.4 s) is the same limit in seconds.
inline constexpr double kTimeSlack = 1.0e-6;
// A player's light scores this much (every number the importance makes is below 1.3).
inline constexpr double kPlayerScore = 1.0e9;

// The time of a frame when the caller gave none.
inline constexpr double TimeOfFrame(uint64_t frame) { return double(frame) * kReferenceStep; }

// ---------------------------------------------------------------------------
// the tuning: today's numbers
// ---------------------------------------------------------------------------

struct Tuning {
  // the importance
  double half_distance = 25.0;       // m: the distance at which a light's importance is a half
  double off_screen = 0.35;          // the share of a light whose lamp is off the screen, when the frame's view is not known (the first version's rule)
  double keep_bonus = 1.5;           // a light that held a place scores this times more: a challenger must beat it by that much
  bool player_keeps_bonus = true;    // the player's light gets the bonus too (the rig does; the shadow views' pick does not)
  double linger_s = 0.1;             // seconds: the score of a light the source has not reported is halved this often (6 frames at 60 FPS)
  double coast_linger_s = 0.5;       // ... and of one that coasts (30 frames)
  // the fade (the rig's lights)
  double fade_in_per_s = 7.2;        // the weight's step up a second (0.12 a frame at 60 FPS)
  double fade_out_per_s = 7.2;       // ... and down
  double drop_s = 0.4;               // seconds without a sighting after which a light with no weight left is forgotten (24 frames)
  double fade_near_m = light_influence::kFadeNearMetres;  // a light fades out with its distance from the camera: in full up to here,
  double fade_far_m = light_influence::kFadeFarMetres;    // ... gone from here
  // the coast: a light the source stopped reporting because it left through the picture's edge goes on while its beam is in the view
  double coast_s = 1.5;              // the longest a light coasts (90 frames)
  double coast_taper_from = 0.6;     // the weight tapers off over the last part of the coast (from this share of coast_s to its end), not at a stroke
  double coast_reach_off = 0.01;     // a coasting light goes out as its beam leaves the view: its reach below this is out,
  double coast_reach_full = 0.06;    // ... above this in full, smooth between
};

// The rig's four lights (headlight_rig_rules.h TrackerTuning: the same numbers, the frame counts as seconds at 60 FPS).
inline constexpr Tuning HeadlightTuning() { return Tuning{}; }

// The cars that get shadow views (local_shadow_rules.h kViewKeepBonus, kViewHalfDistance, kViewOffScreen): the keep bonus is 1.3 and the player does not take it.
inline constexpr Tuning ShadowCarTuning() {
  Tuning t;
  t.keep_bonus = 1.3;
  t.player_keeps_bonus = false;
  return t;
}

// RESERVED: the street lamps' numbers as volumetrics_lamps.h and the lamp shadows have them (kKeepBonus 1.3, kFadeSeconds 0.35, spare slots 1/6 and 1/4), to be the lamps' row when
// the lamp pickers move here (LU-4, LU-5). Their importance is not this header's formula (a lamp's is light / (d^2 + I)): only the constants are held, light_pick_test.cpp checks them.
inline constexpr double kLampKeepBonus = 1.3;
inline constexpr double kLampFadeSeconds = 0.35;

// ---------------------------------------------------------------------------
// the setting
// ---------------------------------------------------------------------------

// mcla_gpu_light_pick (debug only): `unified` (the default) or `legacy` (the old code of the two pickers). A word that is none of them is `unified`.
enum class PickMode : uint8_t { kUnified, kLegacy };

inline PickMode ParseMode(std::string_view text) { return text == "legacy" ? PickMode::kLegacy : PickMode::kUnified; }
inline constexpr std::string_view ModeName(PickMode mode) { return mode == PickMode::kLegacy ? "legacy" : "unified"; }

// The mode a Tracker or a ViewPicker starts in (the unit tests flip it to run every check through both; a game run sets each one's mode from the setting every frame).
inline PickMode& DefaultMode() {
  static PickMode mode = PickMode::kUnified;
  return mode;
}

// ---------------------------------------------------------------------------
// one candidate, and what the pick says of it
// ---------------------------------------------------------------------------

struct Entry {
  uint64_t id = 0;               // identity across frames (the rig's track key; a car's key)
  bool player = false;
  bool observed = true;          // the source reported it in this frame
  double age_s = 0.0;            // scene-clock seconds since it was last reported (0 when observed)
  double distance = 0.0;         // m from the camera (or the reference the source ranks from)
  bool view_known = false;       // the frame's frustum was known: `reach` counts and the distance fade applies; else the first version's rules (`on_screen`, no coast)
  double reach = 1.0;            // light_influence.h BeamReach, 0 .. 1 (read only with view_known)
  bool on_screen = true;         // the first version's: whether the lamp is on the screen (read only without a view)
  bool coast_ok = false;         // the source says it left through the picture's edge: its light may go on without being reported
  bool lit = true;               // the source says its lights are on (a light that is off does not coast)
  double player_penalty = 0.0;   // taken off the player's score: a tie-break among players (the nearer first)
};

struct Out {
  bool picked = false;           // holds a place this frame (it is, or is kept as, one of the budget's best)
  bool coasting = false;         // the light goes on without being reported
  float fade = 0.0f;             // 0 .. 1: the weight (the rig's lights)
  double score = 0.0;            // what it was ranked by
};

struct Result {
  std::vector<Out> out;          // by the entries' order
  std::vector<uint32_t> ranked;  // the entries' indices, best first
  std::vector<uint32_t> shown;   // the indices of the lights that count this frame, best first: weight above 0 (fading pick) or picked (held pick), at most `budget`
};

// The score of one entry.
inline double Score(const Tuning& t, const Entry& e, bool holds) {
  double score;
  if (e.player) {
    score = kPlayerScore - e.player_penalty;
  } else {
    const double r = e.distance / t.half_distance;
    score = 1.0 / (1.0 + r * r);
    if (e.view_known) {
      score *= light_influence::ReachWeight(e.reach);
    } else if (!e.on_screen) {
      score *= t.off_screen;
    }
  }
  if (e.age_s > 0.0) {
    score *= std::pow(0.5, e.age_s / (e.coast_ok ? t.coast_linger_s : t.linger_s));
  }
  if (holds && (!e.player || t.player_keeps_bonus)) {
    score *= t.keep_bonus;
  }
  return score;
}

// ---------------------------------------------------------------------------
// the picker
// ---------------------------------------------------------------------------

class Picker {
 public:
  Picker() = default;
  explicit Picker(const Tuning& tuning) : tuning_(tuning) {}

  const Tuning& tuning() const { return tuning_; }
  void SetTuning(const Tuning& tuning) { tuning_ = tuning; }

  // FADING PICK (the rig's lights). `dt_s`: the scene clock's step since the last call. Every light the caller still wants kept is in `in`, picked or not; `budget` is how many lights hold a
  // place (the rig's four). Per entry: it holds a place when it is the best `budget` of those the source reports, or a light that coasts, or one that held a place and still has weight (it
  // fades out in its slot). The weights step toward their targets by rate x dt. `result->shown` is the lights with weight, best first, at most `budget`.
  void PickFading(double dt_s, std::span<const Entry> in, size_t budget, Result* result) {
    const size_t n = in.size();
    const double dt = std::clamp(dt_s, 0.0, kMaxStepSeconds);
    result->out.assign(n, Out{});
    result->shown.clear();
    scratch_.resize(n);
    for (size_t i = 0; i < n; ++i) {
      scratch_[i] = Find(in[i].id);
      result->out[i].score = Score(tuning_, in[i], scratch_[i].picked);
      // A reported light does not coast; any other keeps its flag from the last pick unless the loop below sets it (the old rig's behaviour: Tracked::coasting stays set while a light that
      // coasted fades out in its slot).
      result->out[i].coasting = in[i].observed ? false : scratch_[i].coasting;
    }
    Rank(in, result);
    // Who holds a place: a light reported in this frame, a light that coasts, a light that held a place and still has weight.
    size_t picked = 0;
    for (const uint32_t i : result->ranked) {
      const Entry& e = in[i];
      Out& o = result->out[i];
      if (picked < budget && e.observed) {
        o.picked = true;
        ++picked;
      } else if (picked < budget && CanCoast(e, scratch_[i])) {
        o.picked = true;
        o.coasting = true;
        ++picked;
      } else if (picked < budget && scratch_[i].picked && scratch_[i].fade > 0.0f) {
        o.picked = false;
        ++picked;
      }
    }
    // The weights, and who counts.
    const float step_up = float(tuning_.fade_in_per_s * dt);
    const float step_down = float(tuning_.fade_out_per_s * dt);
    for (const uint32_t i : result->ranked) {
      const Entry& e = in[i];
      Out& o = result->out[i];
      float target = 0.0f;
      if (o.picked) {
        double goal = 1.0;
        if (e.view_known) {
          goal = light_influence::DistanceFade(e.distance, tuning_.fade_near_m, tuning_.fade_far_m);
          if (o.coasting) {
            goal *= light_influence::SmoothStep(tuning_.coast_reach_off, tuning_.coast_reach_full, e.reach);
            goal *= 1.0 - light_influence::SmoothStep(tuning_.coast_taper_from * tuning_.coast_s, tuning_.coast_s, e.age_s);
          }
        }
        target = float(goal);
      }
      float fade = scratch_[i].fade;
      if (target > fade) {
        fade = std::min(target, fade + step_up);
      } else {
        fade = std::max(target, fade - step_down);
      }
      o.fade = fade;
      if (fade > 0.0f && result->shown.size() < budget) {
        result->shown.push_back(i);
      }
    }
    // The state of the next pick: every entry's, nothing else.
    states_.clear();
    for (size_t i = 0; i < n; ++i) {
      states_.push_back(State{in[i].id, result->out[i].picked, result->out[i].fade, result->out[i].coasting});
    }
  }

  // HELD PICK (the cars that get shadow views): the best `take` of the entries hold a place; there is no weight (a car's views come and go together). The entries that were picked in the
  // last call score `keep_bonus` times more. `result->shown` = the picked indices, best first.
  void PickHeld(std::span<const Entry> in, size_t take, Result* result) {
    const size_t n = in.size();
    result->out.assign(n, Out{});
    result->shown.clear();
    scratch_.resize(n);
    for (size_t i = 0; i < n; ++i) {
      scratch_[i] = Find(in[i].id);
      result->out[i].score = Score(tuning_, in[i], scratch_[i].picked);
    }
    Rank(in, result);
    take = std::min(take, n);
    states_.clear();
    for (size_t k = 0; k < take; ++k) {
      const uint32_t i = result->ranked[k];
      result->out[i].picked = true;
      result->out[i].fade = 1.0f;
      result->shown.push_back(i);
      states_.push_back(State{in[i].id, true, 1.0f, false});
    }
  }

  // The held pick's state: forget it (a new night is a new start), ask who holds a place, set it from outside (a switch of the mode hands over what the other path held).
  void Forget() { states_.clear(); }
  bool Holds(uint64_t id) const {
    for (const State& s : states_) {
      if (s.id == id) {
        return s.picked;
      }
    }
    return false;
  }
  size_t held_count() const {
    size_t count = 0;
    for (const State& s : states_) {
      count += s.picked ? 1u : 0u;
    }
    return count;
  }
  // The state of the fading pick for one light (seeding it from a path that kept it elsewhere).
  void Seed(uint64_t id, bool picked, float fade, bool coasting) {
    for (State& s : states_) {
      if (s.id == id) {
        s = State{id, picked, fade, coasting};
        return;
      }
    }
    states_.push_back(State{id, picked, fade, coasting});
  }
  size_t state_count() const { return states_.size(); }

 private:
  struct State {
    uint64_t id = 0;
    bool picked = false;
    float fade = 0.0f;
    bool coasting = false;
  };

  State Find(uint64_t id) const {
    for (const State& s : states_) {
      if (s.id == id) {
        return s;
      }
    }
    return State{id, false, 0.0f, false};
  }

  // By score, then the lower id.
  static void Rank(std::span<const Entry> in, Result* result) {
    result->ranked.resize(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
      result->ranked[i] = uint32_t(i);
    }
    const std::vector<Out>& out = result->out;
    std::stable_sort(result->ranked.begin(), result->ranked.end(), [&](uint32_t a, uint32_t b) {
      if (out[a].score != out[b].score) {
        return out[a].score > out[b].score;
      }
      return in[a].id < in[b].id;
    });
  }

  // A light that held a place and that the source stopped reporting keeps its light while it left through the picture's edge, was lit, its beam is in the view and not too long has passed.
  bool CanCoast(const Entry& e, const State& s) const {
    return e.view_known && e.coast_ok && s.picked && !e.observed && e.age_s <= tuning_.coast_s + kTimeSlack && e.lit && e.reach >= tuning_.coast_reach_off;
  }

  Tuning tuning_;
  std::vector<State> states_;
  std::vector<State> scratch_;
};

// ---------------------------------------------------------------------------
// what the LightManager holds
// ---------------------------------------------------------------------------

struct LightPicker {
  Picker headlights{HeadlightTuning()};  // the rig's lights for the lit shaders: a fading pick
  Picker shadow_cars{ShadowCarTuning()}; // the cars that get shadow views: a held pick
  // RESERVED: Picker lamps (the street lamps: volume and shadows) when the lamp pickers move here.

  // THE BUDGET (light_budget.h): each consumer notes the level of the setting it resolved to, once a frame; the budget is the one function of all of them.
  struct Levels {
    bool headlights_on = false;
    light_budget::Level shadows = light_budget::Level::kOff;
    int views_per_car = 0;  // the debug setting mcla_gpu_headlight_shadow_views
    light_budget::Level volume = light_budget::Level::kOff;
  };
  Levels levels;
  light_budget::Table table{light_budget::kThreeLevels};  // the shadow level table in use (local_shadow_rules.h kLevelTable)
  light_budget::LightBudget budget;

  void NoteHeadlights(bool on) {
    levels.headlights_on = on;
    Refresh();
  }
  void NoteShadows(light_budget::Level level, int views_per_car, light_budget::Table shadow_table) {
    levels.shadows = level;
    levels.views_per_car = views_per_car;
    table = shadow_table;
    Refresh();
  }
  void NoteVolume(light_budget::Level level) {
    levels.volume = level;
    Refresh();
  }
  void Refresh() { budget = light_budget::BudgetFor(levels.headlights_on, levels.shadows, levels.views_per_car, levels.volume, table); }
};

}  // namespace mcla::gpu::light_pick
