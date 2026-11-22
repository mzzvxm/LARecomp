#pragma once
// ===========================================================================
// MCLA GPU renderer -- a mod's lamps (lights.bin) as the light manager's source (perf/LIGHT-UNIFY.md package LU-7 = step U7; LU-1 light_convert.h LightFromModLight)
// ===========================================================================
// A mod that adds ground outside the city's light rectangle ships <exe>/models/<mod>/lights.bin; mod_lights.cpp builds a second MCLA light grid from it, and the shader globals point at that grid
// while the camera is over the mod's rectangle (and at the city's otherwise). The grid lights the SURFACES by the game's own shaders; nothing else knows the lamps: not the volume, not
// the shadows, not the RT. This is the feed that tells them: mod_lights.cpp PUBLISHES the set of the grid the camera is over (the lamps as the game's grid got them: the intensity already times
// the `mod_light_intensity` setting, the record's own on flag) when the grid changes, and the manager takes the current set, as a `Set` it only reads, from the render thread.
//
// `Candidates` is the manager's half: the lamps that matter near the camera, as Lights (LightFromModLight: source kMod, the game's law, id from the mod's key and the record's own id,
// the roles of a game-lit lamp: volume, shadow and RT, no surface ADD because the grid already lights the surface), at most `max_out` of them, the nearest by the lamp picker's own score.
// A lamp outside the city's rectangle is not in the game's per-frame list either (the list is the city's), so these are the only lamps there: LIGHT-UNIFY C14.
//
// ALL THE MODS' SETS (GB-H3, LIGHT-UNIFY C14). The set above is the grid the camera is OVER: a lamp of a mod the camera is not over (it is outside that mod's rectangle: the city's grid, or none
// where the city has none, is what the shaders read there) is lit by NOTHING, however near the camera it is. The deferred local light pass (local_lights_sources.h) can light such a lamp
// without any grid, so mod_lights.cpp also publishes every set it built (`PublishBuilt`, when the city's lights load) and the pass takes the lamps of the sets the camera is not over:
// the lamps the game is not lighting. The set the camera is over stays the game's grid's (the pass takes it only in the lamp A/B's pass mode, where the grid is built with it off).
//
// Header only, pure (no guest, no renderer, no SDK): car_lights_test.cpp checks it.
// ===========================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "light_convert.h"
#include "light_id.h"
#include "light_laws.h"

namespace larecomp::gpu::mod_light_source {

// One lamp of a mod's lights.bin in host byte order (the members LightFromModLight reads: pos, colour, dir, intensity, cone_deg, spread_deg, id, type).
struct Record {
  float pos[3] = {};
  float colour[3] = {1.0f, 1.0f, 1.0f};  // rgb 0..1
  float dir[3] = {0.0f, -1.0f, 0.0f};
  float intensity = 0.0f;                // the game's units, already times the mod_light_intensity setting
  float cone_deg = 360.0f;               // 360: omni
  float spread_deg = 0.0f;
  uint32_t id = 0;                       // the record's own id (lights.bin)
  uint8_t type = 0;                      // 0 omni, 1 spot
  uint8_t on = 1;
};

// The lamps of one mod's grid.
struct Set {
  std::string mod;                       // the mod's folder name
  uint32_t mod_key = 0;                  // light_id.h ModKey(mod)
  float rect[4] = {};                    // the grid's rectangle on the ground: min x, min z, max x, max z
  float intensity_mult = 1.0f;           // what `mod_light_intensity` was when the grid was built (the records have it applied)
  std::vector<Record> lamps;
};

// The set of the grid the camera is over, or null (over the city, or `mod_lights` off): published by mod_lights.cpp's SelectGrid, read by the manager. The generation changes with every
// publish, so a reader can tell that the set it holds is the old one.
using SetPtr = std::shared_ptr<const Set>;
using BuiltSets = std::shared_ptr<const std::vector<SetPtr>>;

class Feed {
 public:
  void Publish(std::shared_ptr<const Set> set) {
    std::lock_guard<std::mutex> guard(lock_);
    set_ = std::move(set);
    ++generation_;
  }
  std::shared_ptr<const Set> Current(uint64_t* generation = nullptr) const {
    std::lock_guard<std::mutex> guard(lock_);
    if (generation != nullptr) *generation = generation_;
    return set_;
  }

  // Every set the mod loader built (the same objects Publish hands out, so a reader tells the one the camera is over by pointer): replaced whole when the city's lights load; empty when
  // there is no mod grid or `mod_lights` is off.
  void PublishBuilt(std::vector<SetPtr> sets) {
    auto list = std::make_shared<const std::vector<SetPtr>>(std::move(sets));
    std::lock_guard<std::mutex> guard(lock_);
    built_ = std::move(list);
  }
  BuiltSets Built() const {
    std::lock_guard<std::mutex> guard(lock_);
    return built_;
  }

 private:
  mutable std::mutex lock_;
  std::shared_ptr<const Set> set_;
  BuiltSets built_;
  uint64_t generation_ = 0;
};

inline Feed& Global() {
  static Feed feed;
  return feed;
}

// The lamp picker's score for a lamp at distance d (volumetrics_lamps.h: light / (d^2 + I)): what the nearest K are taken by.
inline float Score(const Record& r, const float camera[3]) {
  const float dx = r.pos[0] - camera[0], dy = r.pos[1] - camera[1], dz = r.pos[2] - camera[2];
  const float d2 = dx * dx + dy * dy + dz * dz;
  const float brightness = std::max({r.colour[0], r.colour[1], r.colour[2], 0.0f}) * std::max(r.intensity, 0.0f);
  return brightness / (d2 + std::max(r.intensity, 1.0f));
}

// The lamps of `set` that are on, within their own reach (the game's sqrt(100 I) kept in 8..120 m) plus `margin` of the camera, the best `max_out` by Score as Lights, best first.
inline void Candidates(const Set& set, const float camera[3], uint32_t max_out, std::vector<Light>* out, float margin = 20.0f) {
  struct Scored {
    float score;
    const Record* record;
  };
  std::vector<Scored> picked;
  for (const Record& r : set.lamps) {
    if (r.on == 0 || r.intensity <= 0.0f) continue;
    const float dx = r.pos[0] - camera[0], dy = r.pos[1] - camera[1], dz = r.pos[2] - camera[2];
    const float reach = light_laws::LampRange(r.intensity) + margin;
    if (dx * dx + dy * dy + dz * dz > reach * reach) continue;
    picked.push_back({Score(r, camera), &r});
  }
  const size_t keep = std::min<size_t>(picked.size(), max_out);
  std::partial_sort(picked.begin(), picked.begin() + std::ptrdiff_t(keep), picked.end(), [](const Scored& a, const Scored& b) {
    if (a.score != b.score) return a.score > b.score;
    return a.record->id < b.record->id;  // a tie is told by the record's own id, never by the vector's order
  });
  for (size_t i = 0; i < keep; ++i) out->push_back(LightFromModLight(*picked[i].record, set.mod_key));
}

}  // namespace larecomp::gpu::mod_light_source
