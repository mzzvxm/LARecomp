#pragma once
// ===========================================================================
// MCLA GPU renderer -- the volumetric light's setting: its words and what `auto` comes to
// ===========================================================================
// mcla_gpu_volumetrics (the pause menu's VOLUMETRIC LIGHT row on GRAPHICS > LIGHTING AND SHADOWS, live, saved):
//   off      the game's own picture: its fog quad and, in rain, its cone meshes under the lamps
//   medium   96 x 54 x 32 froxels, 12 lamps, 2 headlights, no history
//   high     128 x 72 x 48, 32 lamps, 4 headlights, history
//   ultra    160 x 90 x 64, 48 lamps, 4 headlights, history
//   auto     the default: the class's cell (gpu_class.h kEffects): OFF on Low (an integrated GPU), MEDIUM on Mid, HIGH on High
// Nothing is called low (the owner's rule of 2026-11-14): three real levels are medium / high / ultra. A text that is none of the words
// (a hand edited file) counts as `auto`.
//
// Pure inline functions; this header includes nothing of Plume or ReXGlue (volumetrics_level_test.cpp builds on its own).
// ===========================================================================

#include <string>
#include <string_view>

#include "gpu_class.h"
#include "volumetrics_types.h"

namespace mcla::gpu::volumetrics {

constexpr std::string_view Name(Level level) {
  switch (level) {
    case Level::kOff: return "off";
    case Level::kMedium: return "medium";
    case Level::kHigh: return "high";
    case Level::kUltra: return "ultra";
  }
  return "?";
}

constexpr bool On(Level level) { return level != Level::kOff; }

// The lower case of `text` (ASCII).
inline std::string Lower(std::string_view text) {
  std::string out(text);
  for (char& ch : out) {
    if (ch >= 'A' && ch <= 'Z') {
      ch = static_cast<char>(ch - 'A' + 'a');
    }
  }
  return out;
}

// "off", "medium", "high" or "ultra" exactly (lower case); false for anything else, `auto` included.
inline bool ParseLevel(std::string_view text, Level* level) {
  for (const Level candidate : {Level::kOff, Level::kMedium, Level::kHigh, Level::kUltra}) {
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
  bool unknown = false;    // the text is not a value of the setting
};

// The level the setting's text (any case) comes to on class `cls`.
inline Choice Resolve(gpu_class::GpuClass cls, std::string_view saved) {
  Choice choice;
  const std::string text = Lower(saved);
  if (ParseLevel(text, &choice.level)) {
    return choice;
  }
  choice.automatic = true;
  choice.unknown = text != "auto";
  const std::string cell = gpu_class::ResolveFor(cls, gpu_class::Effect::kVolumetrics, "auto");
  if (!ParseLevel(cell, &choice.level)) {
    choice.level = Level::kOff;  // the table holds no such word (the tests check that it does)
  }
  return choice;
}

// "high (auto on the high class)", "off (set)": for the log.
inline std::string Describe(gpu_class::GpuClass cls, const Choice& choice) {
  std::string text(Name(choice.level));
  if (choice.automatic) {
    text += choice.unknown ? " (not a value of the setting: taken as auto on the " : " (auto on the ";
    text += std::string(gpu_class::ClassName(cls)) + " class)";
  } else {
    text += " (set)";
  }
  return text;
}

}  // namespace mcla::gpu::volumetrics
