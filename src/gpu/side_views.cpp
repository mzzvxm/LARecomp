// MCLA GPU renderer -- the side view classes: the registry and what the rule files share. See
// side_views.h.

#include "side_views.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <string>

#include <rex/logging/log_category.h>

#include "device.h"
#include "edram.h"

namespace mcla::gpu {

namespace side_views {

namespace {

// The rules by class; every rule file defines its own (side_views.h).
const SideViewRule& Rule(SideView view) {
  switch (view) {
    case SideView::kShadow: return kShadowRule;
    case SideView::kEnv: return kEnvRule;
    default: return kReflectionRule;
  }
}

std::string Lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  return text;
}

}  // namespace

SideView Classify(const EdramKey& key, uint32_t size_bits) {
  for (const SideView view : {SideView::kReflection, SideView::kShadow, SideView::kEnv}) {
    const SideViewRule& rule = Rule(view);
    if (rule.matches && rule.matches(key, size_bits)) {
      return view;
    }
  }
  return SideView::kNone;
}

bool IsForeignUse(const EdramKey& key, uint32_t size_bits) {
  if (size_bits == 0) {
    return false;  // no size to tell the uses apart by: the class's
  }
  EdramKey identity = key;
  identity.reserved = 0;  // the rules match the layer 0 identity only
  return ForeignUse(Classify(identity, 0), Classify(identity, size_bits), size_bits);
}

const SideViewRule& RuleOf(SideView view) {
  return Rule(view);
}

const char* Name(SideView view) {
  return view == SideView::kNone ? "none" : Rule(view).name;
}

std::string SettingText(SideView view) {
  const SideViewRule& rule = Rule(view);
  return rule.setting ? Lower(rule.setting()) : std::string("game");
}

bool ParseLevel(const std::string& text, SideLevel* level) {
  if (text == "game") {
    *level = SideLevel::kGame;
  } else if (text == "high") {
    *level = SideLevel::kHigh;
  } else if (text == "ultra") {
    *level = SideLevel::kUltra;
  } else {
    return false;
  }
  return true;
}

SideLevel LevelOf(SideView view) {
  const SideViewRule& rule = Rule(view);
  SideLevel level = SideLevel::kGame;
  if (!ParseLevel(gpu_class::Resolve(rule.effect, SettingText(view)), &level)) {
    // auto resolves in the line above; what is left is not a value (a hand-edited file): the
    // default, which is auto.
    ParseLevel(gpu_class::Resolve(rule.effect, "auto"), &level);
  }
  return level;
}

std::string AutoLabel(SideView view) {
  return gpu_class::AutoLabel(Rule(view).effect);
}

SideScales Wanted() {
  SideScales out;
  for (const SideView view : {SideView::kShadow, SideView::kEnv, SideView::kReflection}) {
    const SideViewRule& rule = Rule(view);
    Scale scale = ScaleOfLevel(LevelOf(view));
    if (rule.test_scale) {
      const Scale test = rule.test_scale();
      if (!test.none()) {
        scale = test;
      }
    }
    out[view] = scale;
  }
  return out;
}

uint32_t HostSamples(SideView view, Scale scale, uint32_t guest_samples) {
  const SideViewRule& rule = Rule(view);
  return rule.host_samples ? std::max(1u, rule.host_samples(scale, guest_samples)) : guest_samples;
}

Scale StepDown(Scale scale) {
  // The whole scale below (3x -> 2x -> 1x; 1.5x -> 1x): never below the guest's size.
  if (scale.n <= Scale::kDenominator) {
    return Scale{};
  }
  return Scale{std::max(Scale::kDenominator,
                        (scale.n - 1) / Scale::kDenominator * Scale::kDenominator)};
}

}  // namespace side_views
}  // namespace mcla::gpu
