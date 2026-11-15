#pragma once
// ===========================================================================
// MCLA GPU renderer -- the bounce light's strength levels
// ===========================================================================
// The player's row BOUNCE LIGHT STRENGTH (mcla_gpu_ssgi_intensity) is how strong the bounce light looks; the row
// BOUNCE LIGHT (mcla_gpu_ssgi: off, high, ultra, auto) is still how finely it is drawn and whether it runs at all.
// A level is four numbers (ssgi_light.h says what each does):
//   strength  the gain on the bounce (1 = all that the surroundings send back, by the picture's own colours)
//   limit     the most the bounce may add to a pixel, in multiples of the pixel's own colour (a guard, not the look)
//   reach     how far the gather looks, in metres: the occlusion's own samples go to 1 to 3 m, a ring of samples goes
//             out to this
//   cap       the brightest a sample may carry into the bounce, and the ceiling the bounce may fill a pixel up to, in
//             multiples of the frame's ambient + sun light
//
//   level       strength  limit  reach  cap   what it is
//   light         1.0      2.0     5 m   1.5   the old look at its gentlest, with the ring that lets it reach
//   default       2.0      4.5     9 m   2.0   at or over the most the old cvars gave (owner, 2026-11-13: clamp 4,
//                                            radius x3 = 9 m, strength 2 "looked believable, still subtle"): the
//                                            strongest look the maths keeps clean, since the ceiling (the cap's light)
//                                            and the soft limit hold a nearly black pixel and a lamp's neighbour
//   strong        3.0      6.0    11 m   2.5
//   stronger      4.5      8.0    13 m   3.0
//   strongest     7.0     10.2    16 m   3.5   clearly past the maths' comfort: more noise in the dark, shadows lifted
//                                            as far as the ceiling lets them
// The cap grows slowly on purpose: it is what keeps neon, lamps and the sun-lit sky from whitening what stands beside
// them (a sample's radiance is cut to it, and a pixel is filled only up to it), so even STRONGEST stops at 3.5 times a
// white wall in full sun. `default` is the setting's default. A text that is none of the words (a hand-edited file) is `default`.
//
// The debug cvars mcla_gpu_ssgi_strength / _clamp / _reach / _test_clamp stay as fine overrides of one number each
// (-1 = not set: the level's; 0 or more replaces it), and mcla_gpu_ssgi_far_steps overrides the ring's steps. They are
// debug-only, so a change of theirs is never saved.
//
// Everything is a pure inline function; ssgi_test.cpp builds on its own and checks every level.
// ===========================================================================

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace mcla::gpu::ssgi_level {

enum class Level : uint8_t { kLight, kDefault, kStrong, kStronger, kStrongest };
inline constexpr int kLevelCount = 5;
inline constexpr Level kDefaultLevel = Level::kDefault;

struct Preset {
  float strength;
  float limit;
  float reach;
  float cap;
};

inline constexpr std::array<Preset, kLevelCount> kPresets = {{
    {1.0f, 2.0f, 5.0f, 1.5f},     // light
    {2.0f, 4.5f, 9.0f, 2.0f},     // default
    {3.0f, 6.0f, 11.0f, 2.5f},    // strong
    {4.5f, 8.0f, 13.0f, 3.0f},    // stronger
    {7.0f, 10.2f, 16.0f, 3.5f},   // strongest
}};

// What a number may be (the override cvars' ranges; ssgi_light.h's pack codes hold the limit and the cap).
inline constexpr float kStrengthMax = 12.0f;
inline constexpr float kLimitMin = 0.05f;
inline constexpr float kLimitMax = 10.2f;
inline constexpr float kReachMax = 16.0f;
inline constexpr float kCapMin = 0.25f;
inline constexpr float kCapMax = 15.75f;

constexpr std::string_view Name(Level level) {
  switch (level) {
    case Level::kLight: return "light";
    case Level::kDefault: return "default";
    case Level::kStrong: return "strong";
    case Level::kStronger: return "stronger";
    case Level::kStrongest: return "strongest";
  }
  return "?";
}

constexpr const Preset& PresetOf(Level level) {
  return kPresets[static_cast<size_t>(level)];
}

inline std::string Lower(std::string_view text) {
  std::string out(text);
  for (char& ch : out) {
    if (ch >= 'A' && ch <= 'Z') {
      ch = static_cast<char>(ch - 'A' + 'a');
    }
  }
  return out;
}

// "light", "default", "strong", "stronger" or "strongest" exactly (lower case already); false for anything else.
inline bool Parse(std::string_view text, Level* level) {
  for (int i = 0; i < kLevelCount; ++i) {
    const Level candidate = static_cast<Level>(i);
    if (text == Name(candidate)) {
      *level = candidate;
      return true;
    }
  }
  return false;
}

// What the setting's text (any case) comes to: the level it names, else `default`. `known` says which.
inline Level Resolve(std::string_view text, bool* known = nullptr) {
  Level level = kDefaultLevel;
  const bool ok = Parse(Lower(text), &level);
  if (known) {
    *known = ok;
  }
  return ok ? level : kDefaultLevel;
}

// The debug cvars' values: below 0 is "not set".
struct Overrides {
  double strength = -1.0;
  double limit = -1.0;
  double reach = -1.0;
  double cap = -1.0;
  int far_steps = -1;
};

struct Effective {
  float strength;
  float limit;
  float reach;
  float cap;
  int far_steps;       // -1: by the quality (ssgi_light.h FarSteps)
  bool overridden;     // any number came from a debug cvar
};

// The numbers in force: the level's, each one replaced by its debug cvar when that is set (0 or more), clamped.
inline Effective Choose(Level level, const Overrides& ov) {
  const Preset& p = PresetOf(level);
  Effective e{p.strength, p.limit, p.reach, p.cap, -1, false};
  if (ov.strength >= 0.0) {
    e.strength = static_cast<float>(std::min(ov.strength, static_cast<double>(kStrengthMax)));
    e.overridden = true;
  }
  if (ov.limit >= 0.0) {
    e.limit = static_cast<float>(std::clamp(ov.limit, static_cast<double>(kLimitMin), static_cast<double>(kLimitMax)));
    e.overridden = true;
  }
  if (ov.reach >= 0.0) {
    e.reach = static_cast<float>(std::min(ov.reach, static_cast<double>(kReachMax)));
    e.overridden = true;
  }
  if (ov.cap >= 0.0) {
    e.cap = static_cast<float>(std::clamp(ov.cap, static_cast<double>(kCapMin), static_cast<double>(kCapMax)));
    e.overridden = true;
  }
  if (ov.far_steps >= 0) {
    e.far_steps = ov.far_steps;
    e.overridden = true;
  }
  return e;
}

}  // namespace mcla::gpu::ssgi_level
