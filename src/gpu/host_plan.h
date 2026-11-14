#pragma once
// ===========================================================================
// MCLA GPU renderer -- which host passes a start needs pipelines for
// ===========================================================================
// The renderer's own screen passes that the graphics settings switch (ambient occlusion, contact
// shadows, sun shafts, bounce light) make a dozen pipelines each, the car self-reflections' history pass two, and a pipeline
// is a driver compile of 20 to 150 ms. They are made ahead of use (host_precache.h) -- but only the
// ones the settings in force will use: a pass that is off at the start is not compiled for nothing on a
// weak PC, and one that is switched on later is made in the background before its first frame
// (HostPrecache::AddLive), so neither the first frame of play nor a switch of the setting waits for a
// compile.
//
// This file is the rule that says what is on, as pure inline functions that include nothing of Plume or
// ReXGlue (host_plan_test.cpp builds on its own and checks every class against every value):
//   Resolve   one setting's text -> its level, with `auto` taken from the class (gpu_class.h's table);
//             the owners' own ReadMode functions call it, so the draws and the precache cannot
//             disagree about what is on
//   Decide    the five settings -> the plan of a start
//   Describe  the words of the start's log line: what was decided and why
//   Ambient   which of the occlusion's pipeline groups a level of the occlusion, of the contact
//             shadows and of the bounce light need together (the first two share the depth, denoise
//             and apply passes; the bounce light adds passes of its own)
//
// What each setting's text can be, and what anything else means, is kept as each owner had it:
//   occlusion  mcla_gpu_ao            off, low, high; no `auto`; anything else is off
//   contact    mcla_gpu_contact_shadows   auto, off, low, high; anything else is off
//   shafts     mcla_gpu_sun_shafts    auto, off, low, high; anything else counts as `auto`
//   bounce     mcla_gpu_ssgi          auto, off, low, high; anything else is off; `auto` is off until the
//                                     class is known (and off on every class for now: gpu_class.h);
//                                     needs the occlusion on (it reads its images)
//   car refl   mcla_gpu_car_selfrefl  auto, off, low, high; anything else is off (car_selfrefl.h)
// ===========================================================================

#include <cstdint>
#include <string>
#include <string_view>

#include "gpu_class.h"

namespace mcla::gpu::host_plan {

enum class Pass : uint8_t { kOcclusion, kContact, kShafts, kBounce, kCarRefl };
inline constexpr int kPassCount = 5;

// The same three levels, in the same order, as the owners' own Mode enums.
enum class Level : uint8_t { kOff, kLow, kHigh };

constexpr std::string_view PassName(Pass pass) {
  switch (pass) {
    case Pass::kOcclusion: return "ambient occlusion";
    case Pass::kContact: return "contact shadows";
    case Pass::kShafts: return "sun shafts";
    case Pass::kBounce: return "bounce light";
    case Pass::kCarRefl: return "car self-reflections";
  }
  return "?";
}

constexpr std::string_view LevelName(Level level) {
  switch (level) {
    case Level::kOff: return "off";
    case Level::kLow: return "low";
    case Level::kHigh: return "high";
  }
  return "?";
}

// What one setting came to.
struct Choice {
  Level level = Level::kOff;
  bool automatic = false;  // `auto`: the class's cell decided (also a text the setting does not know, for the shafts)
  bool unknown = false;    // the text is not a value of the setting
  bool held = false;       // the setting says on, but a pass it needs is off (HoldBounce)
};

// The graphics upgrade whose `auto` cell the pass reads (the occlusion has none).
constexpr bool HasAuto(Pass pass) {
  return pass != Pass::kOcclusion;
}
constexpr gpu_class::Effect EffectOf(Pass pass) {
  switch (pass) {
    case Pass::kContact: return gpu_class::Effect::kContactShadows;
    case Pass::kShafts: return gpu_class::Effect::kSunShafts;
    case Pass::kCarRefl: return gpu_class::Effect::kCarSelfRefl;
    default: return gpu_class::Effect::kSsgi;
  }
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

// "off", "low" or "high" exactly (the text is lower case already); false for anything else.
inline bool ParseLevel(std::string_view text, Level* level) {
  if (text == "off") {
    *level = Level::kOff;
  } else if (text == "low") {
    *level = Level::kLow;
  } else if (text == "high") {
    *level = Level::kHigh;
  } else {
    return false;
  }
  return true;
}

// The level `saved` (the setting's text, any case) comes to for `pass` on class `cls`. `class_known`:
// the class is one the process really found (gpu_class::Known()); only the bounce light's `auto` looks at
// it (it acts as on the weakest class until then, so a player is never given the strong class's cost
// before it is known).
inline Choice Resolve(Pass pass, gpu_class::GpuClass cls, bool class_known, std::string_view saved) {
  Choice choice;
  const std::string text = Lower(saved);
  if (!HasAuto(pass)) {
    choice.unknown = !ParseLevel(text, &choice.level);
    if (choice.unknown) {
      choice.level = Level::kOff;
    }
    return choice;
  }
  choice.automatic = text == "auto";
  if (choice.automatic && pass == Pass::kBounce && !class_known) {
    return choice;  // off
  }
  const std::string resolved = gpu_class::ResolveFor(cls, EffectOf(pass), text);
  if (!ParseLevel(resolved, &choice.level)) {
    choice.unknown = true;
    choice.level = Level::kOff;
    if (pass == Pass::kShafts) {
      // A hand-edited text counts as `auto` there.
      choice.automatic = true;
      ParseLevel(gpu_class::ResolveFor(cls, EffectOf(pass), "auto"), &choice.level);
    }
  }
  return choice;
}

// The bounce light reads the occlusion's images and passes: it is off while the occlusion is, whatever its
// own setting says (the draws and the precache both ask here).
inline Choice HoldBounce(Choice bounce, Level occlusion) {
  if (occlusion == Level::kOff && bounce.level != Level::kOff) {
    bounce.level = Level::kOff;
    bounce.held = true;
  }
  return bounce;
}

struct Plan {
  Choice pass[kPassCount];
  const Choice& of(Pass p) const { return pass[static_cast<int>(p)]; }
  bool on(Pass p) const { return of(p).level != Level::kOff; }
};

// The plan of a start from the settings' texts. The bounce light is held off while the occlusion is. The car
// self-reflections have no dependency (they make their own history pass), so they are on wherever their setting is.
inline Plan Decide(gpu_class::GpuClass cls, bool class_known, std::string_view occlusion,
                   std::string_view contact, std::string_view shafts, std::string_view bounce,
                   std::string_view car_refl = "off") {
  Plan plan;
  plan.pass[static_cast<int>(Pass::kOcclusion)] = Resolve(Pass::kOcclusion, cls, class_known, occlusion);
  plan.pass[static_cast<int>(Pass::kContact)] = Resolve(Pass::kContact, cls, class_known, contact);
  plan.pass[static_cast<int>(Pass::kShafts)] = Resolve(Pass::kShafts, cls, class_known, shafts);
  plan.pass[static_cast<int>(Pass::kBounce)] =
      HoldBounce(Resolve(Pass::kBounce, cls, class_known, bounce), plan.of(Pass::kOcclusion).level);
  plan.pass[static_cast<int>(Pass::kCarRefl)] = Resolve(Pass::kCarRefl, cls, class_known, car_refl);
  return plan;
}

// "contact shadows low (auto on the mid class)", "ambient occlusion high (set)", "sun shafts off (auto
// on the low class)", "sun shafts high ('hgih' is no value: taken as auto on the high class)".
inline std::string Describe(Pass pass, const Choice& choice, gpu_class::GpuClass cls) {
  std::string text(PassName(pass));
  text += ' ';
  text += LevelName(choice.level);
  text += " (";
  if (choice.held) {
    text += "needs the ambient occlusion, which is off";
  } else if (choice.unknown && choice.automatic) {
    text += "not a value of the setting: taken as auto on the ";
    text += gpu_class::ClassName(cls);
    text += " class";
  } else if (choice.unknown) {
    text += "not a value of the setting: off";
  } else if (choice.automatic) {
    text += "auto on the ";
    text += gpu_class::ClassName(cls);
    text += " class";
  } else {
    text += "set";
  }
  text += ')';
  return text;
}

// What the occlusion's pipeline groups need when the occlusion is at `occlusion`, the contact shadows at
// `contact` (both are drawn with the depth passes, the denoise and the apply) and the bounce light at
// `bounce` (its own passes on top of the occlusion's: the pause menu's and the map's world, and a frame
// that cannot have it, run the plain occlusion). `census`: the occlusion's debug census, which runs the
// depth passes and the coarser levels with everything off.
struct AmbientNeeds {
  bool depth = false;      // the linear depth, plain and multisampled
  bool mip = false;        // the coarser depth levels
  bool occlusion = false;  // the visibility, its history (plain and multisampled) and the vehicle table
  bool denoise = false;
  bool apply = false;      // the pass that multiplies the colour (every sample count in use)
  bool contact = false;    // the contact shadows' march
  bool bounce = false;     // the bounce light's colour chain, bitmask, denoise, history, filter and apply
  bool any() const { return depth || mip || occlusion || denoise || apply || contact || bounce; }
};

inline AmbientNeeds Ambient(Level occlusion, Level contact, Level bounce, bool census) {
  AmbientNeeds needs;
  const bool ao = occlusion != Level::kOff;
  const bool cs = contact != Level::kOff;
  needs.depth = ao || cs || census;
  needs.mip = ao || census;
  needs.occlusion = ao;
  needs.denoise = ao || cs;
  needs.apply = ao || cs;
  needs.contact = cs;
  needs.bounce = ao && bounce != Level::kOff;  // it needs the occlusion's images
  return needs;
}

}  // namespace mcla::gpu::host_plan
