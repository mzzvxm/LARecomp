#pragma once
// ===========================================================================
// MCLA GPU renderer -- the compare key's rules (pure)
// ===========================================================================
// What a press of the compare key does and says, from the settings' texts: whether the chosen effect is
// on now, which way the flip goes, whether anything could show it, the words of the label, how often a
// press counts and how the label fades. No renderer, no cvars, no clock: effect_compare_test.cpp checks
// every effect against every value, and mc_engine/effect_compare.cpp only reads the settings and the
// time and calls these. The flip itself (effect_compare.h) is read by the settings' own readers.
// ===========================================================================

#include <cstdint>
#include <string>
#include <string_view>

#include "effect_compare.h"
#include "gpu_class.h"
#include "host_plan.h"
#include "shadow_filter.h"

namespace mcla::gpu::compare {

// The texts of the settings and the GPU class, as a press reads them (the saved values: the flip is not in
// them).
struct Saved {
  gpu_class::GpuClass cls = gpu_class::GpuClass::kLow;
  bool class_known = false;
  std::string occlusion;  // mcla_gpu_ao
  std::string contact;    // mcla_gpu_contact_shadows
  std::string shafts;     // mcla_gpu_sun_shafts
  std::string bounce;     // mcla_gpu_ssgi
  std::string filter;     // mcla_gpu_shadow_filter
  bool bloom = true;      // mcla_gpu_bloom_scale
};

// What the saved settings come to for one effect.
struct Standing {
  bool on = false;       // the saved setting shows the effect now
  bool blocked = false;  // a flip would show nothing: the bounce light is made from the occlusion's passes
};

inline Standing StandingOf(Effect effect, const Saved& saved) {
  const host_plan::Plan plan = host_plan::Decide(saved.cls, saved.class_known, saved.occlusion,
                                                 saved.contact, saved.shafts, saved.bounce);
  Standing out;
  switch (effect) {
    case Effect::kBounceLight:
      out.on = plan.on(host_plan::Pass::kBounce);
      out.blocked = !plan.on(host_plan::Pass::kOcclusion);
      break;
    case Effect::kContactShadows:
      out.on = plan.on(host_plan::Pass::kContact);
      break;
    case Effect::kSunShafts:
      out.on = plan.on(host_plan::Pass::kShafts);
      break;
    case Effect::kAmbientOcclusion:
      out.on = plan.on(host_plan::Pass::kOcclusion);
      break;
    case Effect::kShadowFilter: {
      std::string text = host_plan::Lower(saved.filter);
      // On is the variant being drawn: soft or pcss (the game's own filter is the only off).
      out.on = shadow_filter::ModeFor(saved.class_known ? saved.cls : gpu_class::GpuClass::kLow,
                                      shadow_filter::ParseChoice(text)) !=
               shadow_filter::Mode::kGame;
      break;
    }
    case Effect::kBloomFix:
      out.on = saved.bloom;
      break;
    case Effect::kNone:
      break;
  }
  return out;
}

// What a press leads to.
struct Press {
  Force force = Force::kNone;  // the flip to set; kNone puts the saved setting back
  bool shown_on = false;       // what the picture shows from now on
  bool blocked = false;        // nothing to flip (see Standing::blocked); the flip is left as it was
};

// A press flips the effect away from what the saved setting shows (off if it is on, on if it is off); the
// next press puts the saved setting back.
inline Press PressOf(Effect effect, Force current, const Saved& saved) {
  const Standing standing = StandingOf(effect, saved);
  Press out;
  if (standing.blocked) {
    out.force = current;
    out.shown_on = false;
    out.blocked = true;
    return out;
  }
  out.force = current == Force::kNone ? (standing.on ? Force::kOff : Force::kOn) : Force::kNone;
  out.shown_on = out.force == Force::kOn ? true : out.force == Force::kOff ? false : standing.on;
  return out;
}

// The label's two parts: the effect ("BOUNCE LIGHT") and what it is doing ("OFF"), or what is missing.
struct Label {
  std::string name;
  std::string state;
  bool good = false;  // on: the label draws the state in the menu's teal, the rest in its yellow
};

inline Label LabelOf(Effect effect, bool shown_on, bool blocked) {
  Label label;
  if (effect == Effect::kNone) {
    return label;
  }
  label.name = std::string(Info(effect).label);
  label.state = blocked ? "NEEDS AMBIENT OCCLUSION" : shown_on ? "ON" : "OFF";
  label.good = !blocked && shown_on;
  return label;
}

// The same in one line, for the log: "BOUNCE LIGHT: OFF", "BOUNCE LIGHT NEEDS AMBIENT OCCLUSION".
inline std::string LabelText(Effect effect, bool shown_on, bool blocked) {
  const Label label = LabelOf(effect, shown_on, blocked);
  if (label.name.empty()) {
    return {};
  }
  return label.name + (blocked ? " " : ": ") + label.state;
}

// A press closer than this to the one before it is dropped: a held key repeats about thirty times a
// second, and a flip that fast shows nothing.
inline constexpr int64_t kMinPressGapMs = 250;
constexpr bool AcceptPress(int64_t now_ms, int64_t last_ms) {
  return last_ms < 0 || now_ms - last_ms >= kMinPressGapMs;
}

// The label: shown fully for `hold` seconds after a press, then faded out over kLabelFadeOut; a new press
// starts it again. 0 when it is gone.
inline constexpr float kLabelFadeIn = 0.08f;
inline constexpr float kLabelFadeOut = 0.35f;
inline constexpr float kLabelHoldDefault = 1.5f;
constexpr float LabelAlpha(float age_seconds, float hold_seconds) {
  if (age_seconds < 0.0f || age_seconds >= hold_seconds + kLabelFadeOut) {
    return 0.0f;
  }
  const float in = age_seconds >= kLabelFadeIn ? 1.0f : age_seconds / kLabelFadeIn;
  const float out = age_seconds <= hold_seconds ? 1.0f : (hold_seconds + kLabelFadeOut - age_seconds) / kLabelFadeOut;
  return in < out ? in : out;
}

}  // namespace mcla::gpu::compare
