// MCLA GPU renderer -- the sun shadow filter. See shadow_filter.h.

#include "shadow_filter.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <format>
#include <mutex>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "effect_compare.h"
#include "gpu_class.h"

REXCVAR_DEFINE_STRING(
    mcla_gpu_shadow_filter, "auto", "MCLA/GPU",
    "How the sun's shadows are filtered: game (the game's own: four taps at an angle that changes from "
    "pixel to pixel, a grainy edge), soft (more taps on a disk, turned by noise of the screen pixel, a "
    "smooth edge of the same width, or narrower on a finer shadow map: SHADOW DETAIL), pcss (soft, with "
    "contact hardening: a shadow is sharp where it touches what casts it and softer with the distance "
    "from it, as the sun's own shadows are; an integrated graphics card gets soft). auto (the default) "
    "picks by the graphics card: game on an integrated one, soft on the rest. The sun's shadows show from "
    "8 to 17 hours in clear weather. Costs graphics card time in the screen's shadowed pixels (pcss "
    "about the same as soft: away from the edges it takes fewer taps). Live; the first time soft or pcss "
    "shows, the graphics card builds its shaders (a short stutter).")
    .allowed({"auto", "game", "soft", "pcss"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(
    mcla_gpu_shadow_filter_taps, 0, "MCLA/GPU",
    "Diagnostic: how many taps the soft shadow filter takes, 4 to 32. 0: by the graphics card (8 on an "
    "integrated one, 16 on the rest). Live.")
    .debug_only()
    .range(0, 32)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(
    mcla_gpu_shadow_filter_radius, 1.0, "MCLA/GPU",
    "Diagnostic: how wide the soft shadow filter is, as a multiple of the game's own filter measured in "
    "texels of the shadow map (1: the edge of a shadow takes the same number of shadow map texels, so "
    "the same width as the game's on its own 1280x1280 map, half as wide on high and a third on ultra: "
    "mcla_gpu_shadow_resolution). Live.")
    .debug_only()
    .range(0.25, 4.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(
    mcla_gpu_shadow_filter_sun, 4.0, "MCLA/GPU",
    "Diagnostic: how big the sun is for the contact hardening of pcss, as a multiple of its real angular "
    "radius (0.27 degrees). 1 is the real sun, whose penumbra is narrower than one texel of the shadow map "
    "for anything lower than about 3 metres, so a shadow is sharp everywhere; a larger sun makes the "
    "edge of the shadow of a car or a pole visibly softer with the distance from what casts it (the "
    "width is capped at twice the soft filter's). Live.")
    .debug_only()
    .range(0.25, 15.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(
    mcla_gpu_shadow_filter_pcf, false, "MCLA/GPU",
    "Diagnostic: how a tap of the soft shadow filter reads the shadow map. Off: with the game's own "
    "sampler, which compares a depth blended from four texels (what the game's four taps do). On: "
    "it compares the four texels one by one and blends the answers (a true percentage-closer tap); "
    "the shadow edge is a little smoother and its bias a little different. Live.")
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

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
              TapsFor(cls, REXCVAR_GET(mcla_gpu_shadow_filter_taps)),
              mode == Mode::kPcss
                  ? std::format(", 8 blocker taps, sun x{:g}", std::clamp(REXCVAR_GET(mcla_gpu_shadow_filter_sun),
                                                                         kMinSunTuning, kMaxSunTuning))
                  : std::string());
}

void EnsureRegistered() {
  static std::once_flag registered;
  std::call_once(registered, [] {
    g_choice.store(static_cast<uint8_t>(ParseChoice(Lower(REXCVAR_GET(mcla_gpu_shadow_filter)))),
                   std::memory_order_relaxed);
    rex::cvar::RegisterChangeCallback("mcla_gpu_shadow_filter", [](std::string_view, std::string_view value) {
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
  return MakeConstants(cls, REXCVAR_GET(mcla_gpu_shadow_filter_taps), REXCVAR_GET(mcla_gpu_shadow_filter_radius),
                       g_atlas_scale_inverse.load(std::memory_order_relaxed), REXCVAR_GET(mcla_gpu_shadow_filter_pcf),
                       temporal_upscaler, frame, pcss, REXCVAR_GET(mcla_gpu_shadow_filter_sun));
}

void SetAtlasRadiusScale(float scale_inverse) {
  g_atlas_scale_inverse.store(std::clamp(scale_inverse, 0.125f, 1.0f), std::memory_order_relaxed);
}

}  // namespace mcla::gpu::shadow_filter
