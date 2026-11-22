// MCLA GPU renderer -- the sun shadow filter. See shadow_filter.h.

#include "shadow_filter.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <format>
#include <mutex>
#include <string>

#include "settings/larecomp_settings.h"
#include <rex/logging/log_category.h>

#include "effect_compare.h"
#include "gpu_class.h"


namespace mcla::gpu::shadow_filter {

namespace {

// The choice, as the cvar's change callback last saw it: the render thread never reads the cvar's string
// while another thread may be writing it (the pause menu), as gpu_class.cpp's override.
std::atomic<uint8_t> g_choice{static_cast<uint8_t>(Choice::kAuto)};
std::atomic<float> g_atlas_scale_inverse{1.0f};
std::atomic<uint8_t> g_last_logged{255};  // the last Mode (and class) the log line said, to say a change once

std::string Lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return text;
}

gpu_class::GpuClass EffectiveClass() {
  // `auto` is not the same on every class, so until the class is known it acts as on the weakest: a player
  // is never given the stronger class's cost before it is known (gpu_class.h Known()).
  return gpu_class::Known() ? gpu_class::Current() : gpu_class::GpuClass::kLow;
}

const char* ChoiceName(Choice choice) {
  switch (choice) {
    case Choice::kGame:
      return "game";
    case Choice::kSoft:
      return "soft";
    case Choice::kPcss:
      return "pcss";
    default:
      return "auto";
  }
}

// One line when the mode or the class it was worked out for changes (also the first time it is asked).
void LogMode(Mode mode) {
  const gpu_class::GpuClass cls = EffectiveClass();
  const uint8_t state = static_cast<uint8_t>((static_cast<uint8_t>(mode) << 2) | static_cast<uint8_t>(cls));
  if (g_last_logged.exchange(state, std::memory_order_relaxed) == state) return;
  const Choice choice = static_cast<Choice>(g_choice.load(std::memory_order_relaxed));
  const char* name = mode == Mode::kPcss ? "pcss (contact hardening)" : mode == Mode::kSoft ? "soft" : "the game's own";
  REXLOG_INFO("[gpu] shadow filter: {} (mcla_gpu_shadow_filter = {}, GPU class {}{}; {} taps{}; live)", name,
              ChoiceName(choice), gpu_class::ClassName(cls),
              choice == Choice::kPcss && mode == Mode::kSoft ? "; pcss is soft on a Low class: no blocker search" : "",
              TapsFor(cls, larecomp::settings::mcla_gpu_shadow_filter_taps.Get()),
              mode == Mode::kPcss
                  ? std::format(", 8 blocker taps, sun x{:g}", std::clamp(larecomp::settings::mcla_gpu_shadow_filter_sun.Get(),
                                                                         kMinSunTuning, kMaxSunTuning))
                  : std::string());
}

void EnsureRegistered() {
  static std::once_flag registered;
  std::call_once(registered, [] {
    g_choice.store(static_cast<uint8_t>(ParseChoice(Lower(larecomp::settings::mcla_gpu_shadow_filter.Get()))),
                   std::memory_order_relaxed);
    larecomp::cvar::RegisterChangeCallback("mcla_gpu_shadow_filter", [](std::string_view, std::string_view value) {
      g_choice.store(static_cast<uint8_t>(ParseChoice(Lower(std::string(value)))), std::memory_order_relaxed);
    });
  });
}

}  // namespace

Mode CurrentMode() {
  EnsureRegistered();
  Choice choice = static_cast<Choice>(g_choice.load(std::memory_order_relaxed));
  // The compare key's flip (effect_compare.h): the game's own filter or the soft one, whatever the setting says.
  switch (compare::Global().ForcedFor(compare::Effect::kShadowFilter)) {
    case compare::Force::kOff:
      choice = Choice::kGame;
      break;
    case compare::Force::kOn:
      choice = Choice::kSoft;
      break;
    default:
      break;
  }
  const Mode mode = ModeFor(EffectiveClass(), choice);
  LogMode(mode);
  return mode;
}

bool PcssRuns() { return PcssAvailable(EffectiveClass()); }

ShadowFilterConstants CurrentConstants(bool temporal_upscaler, uint64_t frame) {
  EnsureRegistered();
  const gpu_class::GpuClass cls = EffectiveClass();
  const bool pcss = ModeFor(cls, static_cast<Choice>(g_choice.load(std::memory_order_relaxed))) == Mode::kPcss;
  return MakeConstants(cls, larecomp::settings::mcla_gpu_shadow_filter_taps.Get(), larecomp::settings::mcla_gpu_shadow_filter_radius.Get(),
                       g_atlas_scale_inverse.load(std::memory_order_relaxed), larecomp::settings::mcla_gpu_shadow_filter_pcf.Get(),
                       temporal_upscaler, frame, pcss, larecomp::settings::mcla_gpu_shadow_filter_sun.Get(), larecomp::settings::mcla_gpu_noise_blue.Get());
}

void SetAtlasRadiusScale(float scale_inverse) {
  g_atlas_scale_inverse.store(std::clamp(scale_inverse, 0.125f, 1.0f), std::memory_order_relaxed);
}

}  // namespace mcla::gpu::shadow_filter
