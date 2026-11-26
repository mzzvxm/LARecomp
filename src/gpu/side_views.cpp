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
#include "render_cpu.h"
#include "settings/larecomp_settings.h"

namespace mcla::gpu {

namespace side_views {

namespace {

// The rules by class; every rule file defines its own (side_views.h).
const SideViewRule& Rule(SideView view) {
  switch (view) {
    case SideView::kShadow: return kShadowRule;
    case SideView::kEnv: return kEnvRule;
    case SideView::kUiMap: return kUiMapRule;
    case SideView::kFoliageImpostor: return kFoliageImpostorRule;
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
  for (const SideView view : {SideView::kReflection, SideView::kShadow, SideView::kEnv, SideView::kUiMap, SideView::kFoliageImpostor}) {
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
  // A pure function of its two arguments (every rule's matches compares them against constants), asked about the same identity and size by
  // draw after draw and by each draw several times (DepthLayer): the last answer is kept, per thread (render_cpu_mask bit 8; off: asked in
  // full every time, as before).
  struct Asked {
    EdramKey key;
    uint32_t size_bits = 0;
    bool operator==(const Asked& o) const { return key == o.key && size_bits == o.size_bits; }
  };
  static const bool memo = render_cpu::Has(larecomp::settings::render_cpu_mask.Get(), render_cpu::kForeignUseMemo);
  static thread_local render_cpu::LastAnswer<Asked, bool> last;
  const Asked asked{key, size_bits};
  bool answer = false;
  if (memo && last.Find(asked, &answer)) {
    return answer;
  }
  EdramKey identity = key;
  identity.reserved = 0;  // the rules match the layer 0 identity only
  answer = ForeignUse(Classify(identity, 0), Classify(identity, size_bits), size_bits);
  last.Put(asked, answer);
  return answer;
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

SideLevel LevelOf(SideView view) {
  // The pure part (the `auto` table, a word that is no level) is side_views.h's LevelFor.
  return LevelFor(gpu_class::Current(), Rule(view).effect, SettingText(view));
}

std::string AutoLabel(SideView view) {
  return gpu_class::AutoLabel(Rule(view).effect);
}

SideScales Wanted(Scale output) {
  SideScales out;
  for (const SideView view : kAllSideViews) {
    const SideViewRule& rule = Rule(view);
    if (rule.follows_output) {
      out[view] = OutputScale(output, rule.enabled && rule.enabled());
      continue;
    }
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

SideScales FollowOutput(const SideScales& side, Scale output) {
  SideScales out = side;
  for (const SideView view : kAllSideViews) {
    const SideViewRule& rule = Rule(view);
    if (rule.follows_output) {
      out[view] = OutputScale(output, rule.enabled && rule.enabled());
    }
  }
  return out;
}

uint32_t HostSamples(SideView view, Scale scale, uint32_t guest_samples) {
  const SideViewRule& rule = Rule(view);
  return rule.host_samples ? std::max(1u, rule.host_samples(scale, guest_samples)) : guest_samples;
}

}  // namespace side_views
}  // namespace mcla::gpu
