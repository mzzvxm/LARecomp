#pragma once
// ===========================================================================
// MCLA GPU renderer -- the volumetric light's street lamps: the game's units, the pick and its hysteresis (perf/VOLUMETRICS-SURVEY.md sections 2.2, V7)
// ===========================================================================
// What this holds, with no guest, no renderer and no GPU in it (volumetrics_lamps_test.cpp checks all of it):
//   1. THE GAME'S LAMP LAW and the conversion of a record of the game's lamp list (vol_lamp_list.h GameLamp) into the volume's VolLamp.
//   2. THE CALIBRATION against the lit road (V7).
//   3. THE PICK: the top N lamps of the list by what they add to the view, with hysteresis and fades, so that nothing pops.
//
// 1. THE LAW. What the game's PSMultiLight shaders do for each of the four grid lamps of a pixel (read in the translated xCityRoad__PSMultiLight,
//    the constants in the recompiled sub_823033F0 that writes the light textures; [V] read, [I] inferred, CHECK = a run has to confirm):
//      term(lamp) = lamp colour (rgb 0..1) x saturate(I / d^2) x cone(angle) x N.L x ShaderGlobals.w            (road, props, cars alike)
//    `saturate(256 x T1.w / d^2)` with T1.w = I x 1/256 [V: sub_823033F0 stores intensity x 1.0 x 0.00390625 before packing; CHECK: the packed value],
//    so a lamp lights with its FULL colour out to d = sqrt(I) (a plateau 7.4 m wide for the common 55) and falls as I / d^2 beyond, which is the
//    inverse square law with the intensity as the numerator. The reach the mod loader quotes, sqrt(100 I), is where the term is 1 per cent.
//    The cone is a ramp in the cosine (sub_822FD2C8: half angles a = angle / 2 and s = spread / 2, in degrees): full at cos(a) and zero at cos(a + s) for
//    a spread of 0 or more (0 counts as 1), full at cos(a + s) and zero at cos(a) for a negative one. A Los Angeles street lamp (every one of the 6298 in the
//    city list: cone 125, spread -50.5, intensity 55 to 100, pointing down, warm white) is therefore full inside 37 degrees of its axis and gone at 62.5.
//    ShaderGlobals.w (PS c63.w) is the game's night light intensity (sm_fGlobalNightLightIntensity); the prop pass's own scale (the record's +112) is
//    its fade by distance and hour. CHECK: the record's scale already holds the night factor; if not, the volume's lamps are as bright by dusk as at midnight.
//
//    The volume's lamp term (hlsl/volumetrics_inject_cs.hlsl SpotLight) is  colour x w(d)^2 / (d^2 + near^2)  with the window w = saturate(1 - (d / range)^4):
//    the game's inverse square with a near radius where the game has its plateau. MakeVolLamp gives a lamp   colour = lamp colour x I x scale,
//    near^2 = kLampNearShare x I,  range = sqrt(100 I),  so that beyond the plateau the term IS the game's I / d^2 and the lamp's own core, where the game
//    clamps at 1, rises to 1 / kLampNearShare (1.62) instead of being flat: the largest ratio of the volume's term to the game's, anywhere, is 1.62 and the
//    smallest 0.62 (at the plateau's edge): kLampNearShare is the golden ratio's conjugate because that balances the two (tested: no other share does better).
//    The near radius travels in VolLamp::pad[0] (the frozen header leaves three padding floats; the shader reads the second float of the fourth row, 0 meaning
//    the shader's own 0.8 m), so the interface's layout is untouched.
//
// 2. THE CALIBRATION. Two parts, and only the second is a look.
//    (a) UNITS (hard): a volume lamp in the game's units means the same light as the road's: on the lamp's axis the volume's term at the plateau's edge,
//        in the units of the game's own term (1 there), is kLampNearShare-balanced as above, and the ratio between two lamps of different intensity is the
//        game's (I / d^2 beyond their plateaux). The test holds MakeVolLamp to the law above for every intensity the city has (20 to 100).
//    (b) GAIN (soft): what scatters toward the camera per metre is  sigma_t x p(theta) x E  where E is the irradiance and p the phase function (it sums to 1
//        over the sphere: 1 / 4pi for an even one). The game's colour units are a surface's radiance divided by its albedo (a lit road is colour x albedo, no
//        1/pi), so E = pi x the game's term, and the physical gain from the game's units to the volume's in-scatter is kPhysicalGain = pi. The shader's gain
//        (kLightGain 60, volumetrics_rules.h) is that times kVisibilityBoost = 19.1: the haze of a clear night (sigma_t 0.0012 per metre, which nobody
//        would see a lamp in) is shown as if it were 19 times thicker, the way every game shows a halo. HaloToPoolRatio says what that comes to: how bright
//        a column of air is beside the road it lights. The debug value is mcla_gpu_vol_lamp_strength (default 1 = the boost above); 0.052 (= pi / 60)
//        is the physical one, where the halo is the haze's real brightness and a lamp shows nothing in clear air. The run plan's calibration arms are
//        0.052, 0.25, 1, 4 at the low camera under one lamp (perf/VOL-INPUTS-RUNPLAN.md).
//
// 3. THE PICK. The list holds 64 lamps in a night frame (at most 83) and a level reads 12 / 32 / 48 (GridDesc::lamps). A lamp's score is what it adds to the
//    view: its light (colour x intensity x the game's fade) over its distance squared plus its plateau, times how much of its reach is inside the camera's
//    frustum (a lamp behind the camera still lights the air in front of it): 1 when its influence sphere touches the frustum, falling to 0 a whole radius
//    outside. Hysteresis: a lamp that was picked scores kKeepBonus times its score against the ones that were not, so that two equal lamps do not trade
//    places; fades: a lamp's weight (its colour's multiplier) rises over kFadeSeconds when it is picked and falls over the same when it is not, and a
//    lamp that falls out of the budget keeps a spare slot while it fades (a sixth of the budget is kept for that), so nothing pops. A cut (the camera
//    jumped, or a long gap) starts every picked lamp at 1.
// ===========================================================================

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "vol_lamp_list.h"
#include "volumetrics_rules.h"
#include "volumetrics_types.h"

namespace mcla::gpu::volumetrics {

using vol_lamps::GameLamp;

// ---------------------------------------------------------------------------
// the game's law, and the lamp it makes
// ---------------------------------------------------------------------------

// The game's term where its lamps' light saturates: I / d^2 at and beyond sqrt(I), 1 inside (the PSMultiLight's saturate(256 T1.w / d^2), T1.w = I / 256).
inline float GameLampTerm(float intensity, float dist) {
  if (!(intensity > 0.0f)) {
    return 0.0f;
  }
  const float d2 = dist * dist;
  return d2 <= intensity ? 1.0f : intensity / d2;
}
// The reach the mod loader quotes (mod_lights.cpp: "20 reaches sqrt(100 x 20) = 44.7 m"): where the game's term is 1 per cent.
inline constexpr float kReachShare = 100.0f;
inline float GameReach(float intensity) { return intensity > 0.0f ? std::sqrt(kReachShare * intensity) : 0.0f; }
// The range the volume's window gets: the reach, kept between these (a lamp of intensity 1 would reach 10 m, one of 400 would reach 200).
inline constexpr float kMinReach = 8.0f;
inline constexpr float kMaxReach = 120.0f;

// The game's cone as the cosines of its two edges: full at `inner`, zero at `outer` (sub_822FD2C8). A point light (type 0, or an angle of 360) has none.
inline void ConeCosines(float cone_deg, float spread_deg, float* cos_outer, float* cos_inner) {
  const float half = 0.5f * std::clamp(cone_deg, 0.0f, 360.0f);
  const float s = 0.5f * (spread_deg == 0.0f ? 1.0f : spread_deg);  // "0 means 1" (mod_lights.cpp)
  float inner = half;
  float outer = half;
  if (s >= 0.0f) {
    outer = half + s;
  } else {
    inner = half + s;
  }
  inner = std::clamp(inner, 0.0f, 179.0f);
  outer = std::clamp(outer, inner + 0.1f, 179.9f);
  const float to_rad = kPi / 180.0f;
  *cos_inner = std::cos(inner * to_rad);
  *cos_outer = std::cos(outer * to_rad);
}

// The near radius squared of a lamp's term, as a share of its intensity (the plateau's radius squared in the game): 0.618 balances the largest ratio
// of the volume's term to the game's (at the lamp's core, 1 / share) against the smallest (at the plateau's edge, 1 / (1 + share)).
inline constexpr float kLampNearShare = 0.6180340f;
// What the lamp's padding carries: the near radius squared (0: the shader's own, kLampNear squared).
inline float& LampNearSq(VolLamp& l) { return l.pad[0]; }
inline float LampNearSq(const VolLamp& l) { return l.pad[0]; }

// A record of the game's list as the volume's lamp, in the game's units: the colour carries its intensity and its fade, the scene units are the shader's
// (kLightGain). `units` is a multiplier (1: the calibrated one).
inline VolLamp MakeVolLamp(const GameLamp& g, float units = 1.0f) {
  VolLamp v{};
  const float intensity = std::max(g.intensity, 0.0f);
  for (int i = 0; i < 3; ++i) {
    v.pos[i] = g.pos[i];
    v.colour[i] = std::max(g.colour[i], 0.0f) * intensity * std::clamp(g.scale, 0.0f, 1.0f) * units;
  }
  v.range = std::clamp(GameReach(intensity), kMinReach, kMaxReach);
  // The axis: the way the light points (down, for a street lamp), unit length.
  const float len = std::sqrt(g.dir[0] * g.dir[0] + g.dir[1] * g.dir[1] + g.dir[2] * g.dir[2]);
  if (len > 1e-3f) {
    for (int i = 0; i < 3; ++i) {
      v.dir[i] = g.dir[i] / len;
    }
  } else {
    v.dir[0] = 0.0f;
    v.dir[1] = -1.0f;
    v.dir[2] = 0.0f;
  }
  if (g.type == 0 || g.cone_deg >= 359.0f || len <= 1e-3f) {
    v.cos_outer = -1.0f;  // a point light: no cone
    v.cos_inner = -1.0f;
  } else {
    ConeCosines(g.cone_deg, g.spread_deg, &v.cos_outer, &v.cos_inner);
  }
  v.shadow_view = -1;
  LampNearSq(v) = std::max(kLampNearShare * intensity, kLampNear * kLampNear);
  return v;
}

// The volume's term of a lamp at distance `dist` on its axis, in the units of the colour's brightest channel over the game's (the game's term is
// GameLampTerm(I, dist)): colour x w^2 / (d^2 + near^2), per unit of the colour's own brightest channel and fade.
inline float VolLampTerm(const VolLamp& v, float dist) {
  const float peak = std::max({v.colour[0], v.colour[1], v.colour[2]});
  return peak * LampFalloff(dist, v.range, LampNearSq(v));
}

// ---------------------------------------------------------------------------
// the calibration
// ---------------------------------------------------------------------------

// The physical gain from the game's colour units (a lit surface's radiance over its albedo) to what a medium scatters toward the camera.
inline constexpr float kPhysicalGain = kPi;
// What the shader's gain (kLightGain) adds to it: how many times thicker the haze looks than its extinction says.
inline constexpr float kVisibilityBoost = kLightGain / kPhysicalGain;

// How bright a column of air is beside the road it lights: the in-scatter radiance along `path_m` metres of air at `dist` from a lamp (on the axis,
// seen with the phase function's value `phase`) over the radiance of a road of `albedo` at the same place (the game's term, N.L = 1). `sigma` is the
// haze's extinction per metre, `strength` the debug multiplier (mcla_gpu_vol_lamp_strength).
inline float HaloToPoolRatio(const VolLamp& v, float game_intensity, float dist, float path_m, float sigma, float phase, float albedo, float strength = 1.0f) {
  const float game = GameLampTerm(game_intensity, dist);
  if (!(game > 0.0f) || !(albedo > 0.0f)) {
    return 0.0f;
  }
  const float peak = std::max({v.colour[0], v.colour[1], v.colour[2]});
  const float lamp_colour = peak / std::max(game_intensity, 1e-3f);  // the colour's brightest channel, before the intensity
  const float inscatter = sigma * kLightGain * strength * phase * LampFalloff(dist, v.range, LampNearSq(v)) * peak * path_m;
  const float road = albedo * lamp_colour * game;
  return inscatter / road;
}

// ---------------------------------------------------------------------------
// the pick
// ---------------------------------------------------------------------------

inline constexpr float kKeepBonus = 1.3f;       // a lamp that was picked scores this much more than a rival that was not
inline constexpr float kFadeSeconds = 0.35f;    // a lamp's weight goes 0 to 1 (or back) in this long
inline constexpr float kInfluenceMetres = 30.0f;  // the lit air that can be in view: the sphere of a lamp whose light reaches the frustum
inline constexpr float kCutMetres = 25.0f;      // a camera step over this is a cut (volumetrics.cpp kCutStepMetres)
inline constexpr float kSpareShare = 1.0f / 6.0f;

// How much of a lamp's influence sphere is in the camera's frustum: 1 when it touches it, falling to 0 a whole radius outside. The frustum is the
// camera's (tan_x, tan_y), a near plane at `near_m` and a far plane at `far_m`.
inline float FrustumFactor(const VolCamera& c, const float p[3], float radius, float near_m, float far_m) {
  const float d[3] = {p[0] - c.pos[0], p[1] - c.pos[1], p[2] - c.pos[2]};
  const float x = d[0] * c.right[0] + d[1] * c.right[1] + d[2] * c.right[2];
  const float y = d[0] * c.up[0] + d[1] * c.up[1] + d[2] * c.up[2];
  const float z = d[0] * c.forward[0] + d[1] * c.forward[1] + d[2] * c.forward[2];
  const float nx = 1.0f / std::sqrt(1.0f + c.tan_x * c.tan_x);
  const float ny = 1.0f / std::sqrt(1.0f + c.tan_y * c.tan_y);
  // signed distances to the six planes, positive inside
  const float s = std::min({(x + c.tan_x * z) * nx, (-x + c.tan_x * z) * nx, (y + c.tan_y * z) * ny, (-y + c.tan_y * z) * ny, z - near_m, far_m - z});
  if (s >= 0.0f) {
    return 1.0f;
  }
  return std::clamp(1.0f + s / std::max(radius, 1e-3f), 0.0f, 1.0f);
}

// What a lamp adds to the view (0: nothing: off, no light, outside).
inline float LampScore(const GameLamp& l, const VolCamera& cam, float near_m, float far_m) {
  const float peak = std::max({l.colour[0], l.colour[1], l.colour[2]});
  const float light = peak * std::max(l.intensity, 0.0f) * std::clamp(l.scale, 0.0f, 1.0f);
  if (!(light > 0.0f)) {
    return 0.0f;
  }
  const float d[3] = {l.pos[0] - cam.pos[0], l.pos[1] - cam.pos[1], l.pos[2] - cam.pos[2]};
  const float dist2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
  const float radius = std::min(GameReach(l.intensity), kInfluenceMetres);
  const float factor = FrustumFactor(cam, l.pos, radius, near_m, far_m);
  return light * factor / (dist2 + l.intensity);
}

// A lamp's identity from frame to frame: its place to an eighth of a metre (the list has no number, and street lamps do not move).
inline uint64_t LampKey(const GameLamp& l) {
  uint64_t h = 0x9E3779B97F4A7C15ull;
  for (int i = 0; i < 3; ++i) {
    const int64_t q = int64_t(std::floor(double(l.pos[i]) * 8.0));
    h ^= uint64_t(q) + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
  }
  return h;
}

struct PickParams {
  uint32_t budget = 0;    // the level's lamps (GridDesc::lamps), at most kMaxLamps
  float dt = 0.0f;        // seconds since the last pick; 0 on the first
  float near_m = 0.5f;    // the grid's near and far planes
  float far_m = 160.0f;
  bool reset = false;     // a cut: every picked lamp starts at full weight
  float units = 1.0f;     // the lamps' colour multiplier (1: the calibrated units)
};

struct PickResult {
  std::array<VolLamp, kMaxLamps> lamps{};
  uint32_t count = 0;
  // for the census
  uint32_t listed = 0;    // the records given
  uint32_t usable = 0;    // those that light anything (on, a colour, in reach of the view)
  uint32_t wanted = 0;    // picked
  uint32_t fading = 0;    // on their way out, in a spare slot
  uint32_t dropped = 0;   // out of the budget at once (more fading than spare slots)
  float best_score = 0.0f;
};

class LampPicker {
 public:
  // The slots kept for lamps that fade out: a sixth of the budget, at least one (none for a budget of one).
  static uint32_t SpareSlots(uint32_t budget) {
    if (budget < 2) {
      return 0;
    }
    return std::max(1u, uint32_t(float(budget) * kSpareShare));
  }

  void Reset() {
    tracks_.clear();
    started_ = false;
  }
  size_t tracked() const { return tracks_.size(); }

  void Update(const std::vector<GameLamp>& list, const VolCamera& camera, const PickParams& params, PickResult* out) {
    *out = PickResult{};
    out->listed = uint32_t(list.size());
    const uint32_t budget = std::min(params.budget, kMaxLamps);
    if (budget == 0) {
      tracks_.clear();
      return;
    }
    const uint32_t wanted_cap = budget - SpareSlots(budget);
    const bool instant = params.reset || !started_ || !(params.dt > 0.0f);
    const float step = instant ? 1.0f : std::min(params.dt, 0.25f) / kFadeSeconds;

    // The candidates: every lamp that lights something, once per place.
    struct Candidate {
      uint64_t key;
      GameLamp lamp;
      float score;
      float ranked;
      bool tracked;
    };
    std::vector<Candidate> candidates;
    candidates.reserve(list.size());
    std::unordered_map<uint64_t, size_t> by_key;  // the place in `candidates` while it is being built (it is sorted afterwards)
    for (const GameLamp& l : list) {
      if (!vol_lamps::Plausible(l)) {
        continue;
      }
      const float score = LampScore(l, camera, params.near_m, params.far_m);
      if (!(score > 0.0f)) {
        continue;
      }
      const uint64_t key = LampKey(l);
      const auto found = by_key.find(key);
      if (found != by_key.end()) {
        Candidate& c = candidates[found->second];
        if (score > c.score) {
          c.lamp = l;
          c.score = score;
        }
        continue;
      }
      by_key.emplace(key, candidates.size());
      candidates.push_back({key, l, score, 0.0f, false});
    }
    out->usable = uint32_t(candidates.size());
    std::unordered_map<uint64_t, GameLamp> now;  // the list's lamps by place, for the tracks that are leaving
    for (const Candidate& c : candidates) {
      now.emplace(c.key, c.lamp);
    }

    std::unordered_map<uint64_t, size_t> track_of;
    for (size_t i = 0; i < tracks_.size(); ++i) {
      track_of.emplace(tracks_[i].key, i);
    }
    for (Candidate& c : candidates) {
      c.tracked = track_of.count(c.key) != 0 && tracks_[track_of[c.key]].wanted;
      c.ranked = c.score * (c.tracked ? kKeepBonus : 1.0f);
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
      return a.ranked != b.ranked ? a.ranked > b.ranked : a.key < b.key;
    });
    const size_t wanted = std::min<size_t>(candidates.size(), wanted_cap);
    out->best_score = candidates.empty() ? 0.0f : candidates.front().score;

    // The new tracks: the picked ones first (by score), then the ones fading out (by weight), within the budget.
    std::vector<Track> next;
    next.reserve(budget);
    std::unordered_map<uint64_t, bool> picked;
    for (size_t i = 0; i < wanted; ++i) {
      const Candidate& c = candidates[i];
      Track t;
      t.key = c.key;
      t.lamp = c.lamp;
      t.wanted = true;
      const auto old = track_of.find(c.key);
      t.weight = old != track_of.end() ? std::min(1.0f, tracks_[old->second].weight + step) : (instant ? 1.0f : std::min(1.0f, step));
      picked.emplace(c.key, true);
      next.push_back(t);
    }
    std::vector<Track> leaving;
    for (const Track& old : tracks_) {
      if (picked.count(old.key) != 0) {
        continue;
      }
      Track t = old;
      // Its data is the list's if it is still there, else the last it had.
      const auto still = now.find(old.key);
      if (still != now.end()) {
        t.lamp = still->second;
      }
      t.wanted = false;
      t.weight = old.weight - step;
      if (t.weight > 0.0f) {
        leaving.push_back(t);
      }
    }
    std::sort(leaving.begin(), leaving.end(), [](const Track& a, const Track& b) { return a.weight != b.weight ? a.weight > b.weight : a.key < b.key; });
    for (const Track& t : leaving) {
      if (next.size() < budget) {
        next.push_back(t);
      } else {
        ++out->dropped;
      }
    }
    tracks_ = std::move(next);
    started_ = true;

    for (const Track& t : tracks_) {
      VolLamp v = MakeVolLamp(t.lamp, params.units);
      for (int i = 0; i < 3; ++i) {
        v.colour[i] *= t.weight;
      }
      out->lamps[out->count++] = v;
      if (t.wanted) {
        ++out->wanted;
      } else {
        ++out->fading;
      }
    }
  }

 private:
  struct Track {
    uint64_t key = 0;
    GameLamp lamp;
    float weight = 0.0f;
    bool wanted = false;
  };
  std::vector<Track> tracks_;
  bool started_ = false;
};

}  // namespace mcla::gpu::volumetrics
