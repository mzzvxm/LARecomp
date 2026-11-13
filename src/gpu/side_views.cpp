// MCLA GPU renderer -- the side view classes: the registry and what the rule files share. See
// side_views.h.

#include "side_views.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <string>

#include <rex/logging.h>

#include "device.h"
#include "edram.h"

namespace mcla::gpu {

namespace side_views {

namespace {

constexpr uint64_t kMidDedicatedBytes = uint64_t(6) << 30;  // GT's first guess: under 6 GiB is Mid

GpuClassStub g_class = GpuClassStub::kHigh;
bool g_class_known = false;

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

std::string Upper(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return char(std::toupper(c)); });
  return text;
}

}  // namespace

const char* ClassName(GpuClassStub gpu_class) {
  switch (gpu_class) {
    case GpuClassStub::kLow: return "low";
    case GpuClassStub::kMid: return "mid";
    default: return "high";
  }
}

void InitializeGpuClass(Device& device) {
  // CHECK: GT. The same rule as the design's table (GFX-UPGRADES-DESIGN.md 9): a UMA GPU (an
  // integrated one, a handheld's APU) is Low, a discrete one with under 6 GiB of dedicated memory is
  // Mid, the rest High. Replace this body with a read of gpu_class::Current() when package GT is in.
  const bool uma = device.capabilities().uma;
  const uint64_t dedicated = device.device()->getDescription().dedicatedVideoMemory;
  g_class = uma ? GpuClassStub::kLow
            : dedicated < kMidDedicatedBytes ? GpuClassStub::kMid
                                             : GpuClassStub::kHigh;
  g_class_known = true;
  REXLOG_INFO("[gpu] side views: GPU class {} (uma {}, {} MiB dedicated; a stand-in for package GT)",
              ClassName(g_class), uma, dedicated >> 20);
}

GpuClassStub CurrentClass() {
  // Before the device is known (a call too early) the strongest class: nothing is made smaller.
  return g_class_known ? g_class : GpuClassStub::kHigh;
}

SideView Classify(const EdramKey& key, uint32_t size_bits) {
  for (const SideView view : {SideView::kReflection, SideView::kShadow, SideView::kEnv}) {
    const SideViewRule& rule = Rule(view);
    if (rule.matches && rule.matches(key, size_bits)) {
      return view;
    }
  }
  return SideView::kNone;
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
  const SideViewRule& rule = Rule(view);
  const std::string text = SettingText(view);
  if (text == "game") {
    return SideLevel::kGame;
  }
  if (text == "high") {
    return SideLevel::kHigh;
  }
  if (text == "ultra") {
    return SideLevel::kUltra;
  }
  // auto, and anything that is not a value (a hand-edited file): the default, which is auto.
  return rule.autos[size_t(CurrentClass())];
}

std::string AutoLabel(SideView view) {
  const SideViewRule& rule = Rule(view);
  return std::format("AUTO ({})", Upper(LevelText(rule.autos[size_t(CurrentClass())])));
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
