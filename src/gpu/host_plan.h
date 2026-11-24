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
// What each setting's text can be, and what anything else means (the words are the ones the pause menu and the launcher show: no
// graphics setting has a level called low, owner 2026-11-14; a word that is no value, such as the low of an older larecomp.toml,
// counts as `auto`: the SDK already refuses it in a larecomp.toml, and a command line or an environment variable is not checked there):
//   occlusion  mcla_gpu_ao            auto, off, high, ultra, rt; anything else counts as `auto`
//   contact    mcla_gpu_contact_shadows   auto, off, high, ultra; anything else counts as `auto`
//   shafts     mcla_gpu_sun_shafts    auto, off, light, default, strong, stronger, strongest (a strength, shafts_level.h);
//                                     the old low and high are read as default; anything else counts as `auto`.
//                                     The plan's level is off, or high (32 ray steps) / ultra (64) by the class
//   bounce     mcla_gpu_ssgi          auto, off, high, ultra; anything else counts as `auto`; `auto` is off until the
//                                     class is known (and off on every class for now: gpu_class.h);
//                                     needs the occlusion on (it reads its images)
//   car refl   mcla_gpu_car_selfrefl  auto, off, on; anything else counts as `auto` (car_selfrefl.h). The setting has one level, so
//                                     `on` is the plan's top level, ultra: Level::kHigh (the occlusion alone) is never what the setting
//                                     comes to, only the debug cvar mcla_gpu_car_selfrefl_test reaches that mode
// ===========================================================================

#include <cstdint>
#include <string>
#include <string_view>

#include "gpu_class.h"
#include "host_plan_level.h"
#include "rtao_setting.h"
#include "shafts_level.h"

namespace mcla::gpu::host_plan {

enum class Pass : uint8_t { kOcclusion, kContact, kShafts, kBounce, kCarRefl };
inline constexpr int kPassCount = 5;

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

// What one setting came to.
struct Choice {
  Level level = Level::kOff;
  bool automatic = false;  // `auto`: the class's cell decided (also a text the setting does not know)
  bool unknown = false;    // the text is not a value of the setting (it is then taken as `auto`)
  bool held = false;       // the setting says on, but a pass it needs is off (HoldBounce)
  // The sun shafts only (the other passes leave these): the strength the setting came to, and whether it is an
  // old value (low, high) read as default. `level` is then off for a strength of off, else the ray quality the
  // class gives (high on Mid and Low, ultra on High), so `level != kOff` still says whether the pass is on.
  shafts_level::Level strength = shafts_level::Level::kOff;
  bool legacy = false;
  bool rt = false;  // World ray traced occlusion. level is still the screen-space fallback.
};

// The graphics upgrade whose `auto` cell the pass reads (the occlusion's is the last cell of the class table: off on Low, high on
// Mid, ultra on High).
constexpr gpu_class::Effect EffectOf(Pass pass) {
  switch (pass) {
    case Pass::kOcclusion: return gpu_class::Effect::kAmbientOcclusion;
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

// "off", "high" or "ultra" exactly (the text is lower case already); false for anything else. The car self-reflections' `on` is
// the plan's ultra: Resolve turns it into that word before it asks here.
inline bool ParseLevel(std::string_view text, Level* level) {
  if (text == "off") {
    *level = Level::kOff;
  } else if (text == "high") {
    *level = Level::kHigh;
  } else if (text == "ultra") {
    *level = Level::kUltra;
  } else {
    return false;
  }
  return true;
}

// The words of a setting besides `auto` and `off`: high and ultra, and the car self-reflections' one level, on.
inline bool IsValueWord(Pass pass, std::string_view text) {
  return pass == Pass::kCarRefl ? text == "on" : (text == "high" || text == "ultra");
}

inline Choice OcclusionChoice(std::string_view saved, const rtao_setting::Decision& decision) {
  const bool automatic = rtao_setting::ParseChoice(saved) == rtao_setting::Choice::kAuto;
  return {.level = decision.screen,
          .automatic = automatic,
          .unknown = automatic && Lower(saved) != "auto",
          .rt = decision.rt};
}

// The level `saved` (the setting's text, any case) comes to for `pass` on class `cls`. `class_known`:
// the class is one the process really found (gpu_class::Known()). AO and bounce light Auto use the
// weakest class until then.
inline Choice Resolve(Pass pass, gpu_class::GpuClass cls, bool class_known, std::string_view saved,
                      gpu_class::RtTier tier = gpu_class::RtTier::kNone, bool rtao_built = false) {
  if (pass == Pass::kOcclusion) {
    return OcclusionChoice(saved, rtao_setting::Decide(cls, class_known, tier,
                                                       rtao_setting::ParseChoice(saved), rtao_built));
  }
  Choice choice;
  if (pass == Pass::kShafts) {
    const shafts_level::Choice s = shafts_level::Resolve(cls, saved);
    choice.strength = s.level;
    choice.automatic = s.automatic;
    choice.unknown = s.unknown;
    choice.legacy = s.legacy;
    if (shafts_level::On(s.level)) {
      choice.level = gpu_class::SunShaftSteps(cls) >= gpu_class::kSunShaftStepsHigh ? Level::kUltra : Level::kHigh;
    }
    return choice;
  }
  std::string text = Lower(saved);
  if (text != "auto" && text != "off" && !IsValueWord(pass, text)) {
    // Not a value of the setting (the low of an older larecomp.toml, a typo): it runs at the default, `auto`, as the sun shafts' do.
    choice.unknown = true;
    text = "auto";
  }
  choice.automatic = text == "auto";
  if (choice.automatic && pass == Pass::kBounce && !class_known) {
    return choice;  // off
  }
  std::string resolved = gpu_class::ResolveFor(cls, EffectOf(pass), text);
  if (pass == Pass::kCarRefl && resolved == "on") {
    // The car self-reflections have one level, `on`: the plan's top level. The debug cvar mcla_gpu_car_selfrefl_test still
    // reaches the occlusion-only mode (the plan's high).
    resolved = "ultra";
  }
  if (!ParseLevel(resolved, &choice.level)) {
    choice.unknown = true;  // a cell of the class table that is no level: a bug of the table (the test checks every cell)
    choice.level = Level::kOff;
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
                   std::string_view car_refl = "off", gpu_class::RtTier tier = gpu_class::RtTier::kNone,
                   bool rtao_built = false) {
  Plan plan;
  plan.pass[static_cast<int>(Pass::kOcclusion)] = Resolve(Pass::kOcclusion, cls, class_known, occlusion, tier, rtao_built);
  plan.pass[static_cast<int>(Pass::kContact)] = Resolve(Pass::kContact, cls, class_known, contact);
  plan.pass[static_cast<int>(Pass::kShafts)] = Resolve(Pass::kShafts, cls, class_known, shafts);
  plan.pass[static_cast<int>(Pass::kBounce)] =
      HoldBounce(Resolve(Pass::kBounce, cls, class_known, bounce), plan.of(Pass::kOcclusion).level);
  plan.pass[static_cast<int>(Pass::kCarRefl)] = Resolve(Pass::kCarRefl, cls, class_known, car_refl);
  return plan;
}

// "contact shadows high (auto on the mid class)", "ambient occlusion ultra (set)", "sun shafts off (auto
// on the low class)", "sun shafts default ('hgih' is no value: taken as auto on the high class)", "sun shafts
// strong (set)" (the shafts name their strength, the others their level; the car self-reflections say "on" for their one).
inline std::string Describe(Pass pass, const Choice& choice, gpu_class::GpuClass cls) {
  std::string text(PassName(pass));
  text += ' ';
  if (pass == Pass::kShafts) {
    text += shafts_level::Name(choice.strength);
  } else if (pass == Pass::kCarRefl && choice.level == Level::kUltra) {
    text += "on";  // the one level of the setting (the stored word is `on`; the plan's level is ultra)
  } else if (choice.rt) {
    text += "ray traced";
  } else {
    text += LevelName(choice.level);
  }
  text += " (";
  if (choice.held) {
    text += "needs the ambient occlusion, which is off";
  } else if (choice.legacy) {
    text += "an old value of the setting (low or high): read as default";
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
