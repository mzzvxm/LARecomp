#pragma once
// ===========================================================================
// MCLA GPU renderer -- the sun shafts' strength levels
// ===========================================================================
// The player's setting mcla_gpu_sun_shafts is how strong the shafts are, not how finely they are drawn:
//   off        no shafts: the game's own picture
//   light      the built-in strength x1
//   default    x2 (the look the owner judged "great" on 2026-11-13; the setting's usual value)
//   strong     x3
//   strongest  x4
//   auto       the default of the setting: DEFAULT on the Mid and High GPU classes, OFF on the Low class
//              (an integrated GPU, a handheld or a software adapter keeps the game's own picture;
//              gpu_class.h's table, kEffects)
// The multiplier is the one mcla_gpu_sun_shafts_intensity was while the shafts were tuned; that debug cvar
// stays, a fine multiplier on top of the level (1 = the level as it is).
//
// The old values: the setting used to be auto, off, low and high, a quality (32 or 64 ray steps). An older
// larecomp.toml that holds `low` or `high` is read as `default`, the nearest meaning (both gave the same
// strength; only the steps differed, and those now come from the GPU class, gpu_class.h SunShaftSteps).
// A text that is none of the values (a hand-edited file) counts as `auto`, as it always did.
//
// The ray steps are not a player's choice: the class picks them (Mid 32, High 64); the debug cvar
// mcla_gpu_sun_shafts_steps overrides them for a comparison (Steps below).
//
// Everything is a pure inline function and this header includes nothing of Plume or ReXGlue:
// shafts_level_test.cpp builds on its own and checks every text on every class.
// ===========================================================================

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

#include "gpu_class.h"

namespace mcla::gpu::shafts_level {

enum class Level : uint8_t { kOff, kLight, kDefault, kStrong, kStrongest };
inline constexpr int kLevelCount = 5;

// The word of the setting for a level (lower case), for the cvar's value and the logs.
constexpr std::string_view Name(Level level) {
  switch (level) {
    case Level::kOff: return "off";
    case Level::kLight: return "light";
    case Level::kDefault: return "default";
    case Level::kStrong: return "strong";
    case Level::kStrongest: return "strongest";
  }
  return "?";
}

// The multiplier of the built-in strength (the old intensity value): 0, 1, 2, 3, 4.
constexpr float Multiplier(Level level) {
  switch (level) {
    case Level::kOff: return 0.0f;
    case Level::kLight: return 1.0f;
    case Level::kDefault: return 2.0f;
    case Level::kStrong: return 3.0f;
    case Level::kStrongest: return 4.0f;
  }
  return 0.0f;
}

constexpr bool On(Level level) {
  return level != Level::kOff;
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

// "off", "light", "default", "strong" or "strongest" exactly (the text is lower case already); false for
// anything else, `auto` and the old values included.
inline bool ParseLevel(std::string_view text, Level* level) {
  for (int i = 0; i < kLevelCount; ++i) {
    const Level candidate = static_cast<Level>(i);
    if (text == Name(candidate)) {
      *level = candidate;
      return true;
    }
  }
  return false;
}

// What one saved text comes to.
struct Choice {
  Level level = Level::kOff;
  bool automatic = false;  // `auto` (or a text that is no value, taken as `auto`): the class's cell decided
  bool unknown = false;    // the text is not a value of the setting (a hand-edited file); taken as `auto`
  bool legacy = false;     // an old value (low, high): read as `default`
};

// The level the setting's text (any case) comes to on class `cls`.
inline Choice Resolve(gpu_class::GpuClass cls, std::string_view saved) {
  Choice choice;
  const std::string text = Lower(saved);
  if (text == "low" || text == "high") {
    choice.level = Level::kDefault;
    choice.legacy = true;
    return choice;
  }
  if (ParseLevel(text, &choice.level)) {
    return choice;
  }
  choice.automatic = true;
  choice.unknown = text != "auto";
  const std::string cell = gpu_class::ResolveFor(cls, gpu_class::Effect::kSunShafts, "auto");
  if (!ParseLevel(cell, &choice.level)) {
    choice.level = Level::kOff;  // the table holds no such word (the tests check that it does)
  }
  return choice;
}

// The ray steps in force: the class's (gpu_class.h SunShaftSteps), or the debug cvar's when it is above 0
// (kept between kMinSteps and kMaxSteps: the blur's loop takes any count, the decay is scaled to match).
inline constexpr int kMinSteps = 8;
inline constexpr int kMaxSteps = 128;
constexpr uint32_t Steps(gpu_class::GpuClass cls, int forced) {
  if (forced <= 0) {
    return gpu_class::SunShaftSteps(cls);
  }
  return static_cast<uint32_t>(forced < kMinSteps ? kMinSteps : (forced > kMaxSteps ? kMaxSteps : forced));
}

// "default x2", "off": a level for the log's words.
inline std::string Describe(Level level) {
  std::string text(Name(level));
  if (On(level)) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), " x%g", static_cast<double>(Multiplier(level)));
    text += buf;
  }
  return text;
}

}  // namespace mcla::gpu::shafts_level
