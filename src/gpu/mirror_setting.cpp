// MCLA GPU renderer -- the side mirrors' setting as the renderer and the guest's hooks read it. See mirror_setting.h.

#include "mirror_setting.h"

#include <atomic>
#include <cstdint>
#include <string>

#include "settings/larecomp_settings.h"
#include <rex/logging/log_category.h>

#include "gpu.h"
#include "live_text.h"

namespace mcla::gpu {

namespace {

constexpr const char* kPrefix = "[mirror setting]";

LiveText& SideWord() {
  static LiveText text("mcla_gpu_side_mirrors");
  return text;
}

LiveText& TracedWord() {
  static LiveText text("mcla_gpu_car_mirror_rt");
  return text;
}

// The last answer that was logged, packed: a line is written when the answer changes (a word, the card's class or tier, a debug switch) and not otherwise. 0 = nothing logged yet.
std::atomic<uint64_t> g_logged{0};

}  // namespace

namespace mirror_setting {

Result Current() {
  // Until the class is found (the Xenia renderer never finds it) a setting whose `auto` differs by class acts as on the weakest (gpu_class.h Known): the Low class keeps the game's own mirrors.
  const std::string word = SideWord().Get();
  const std::string traced = TracedWord().Get();
  Facts f;
  f.word = word;
  f.rt_word = traced;
  f.cls = gc::Known() ? gc::Current() : gc::GpuClass::kLow;
  f.handheld = gc::Handheld();
  f.tier = gc::CurrentRtTier();
  f.debug_view = larecomp::settings::mcla_gpu_mirror_view.Get();
  f.debug_glass = larecomp::settings::mcla_gpu_mirror_glass.Get();
  f.debug_overlay = larecomp::settings::mcla_gpu_mirror_overlay.Get();  // MV4's debug switch for the glass overlay: it adds the overlay where the row's view is not running (and is the overlay's only switch under a forced view)
  return Resolve(f);
}

void LogIfChanged(const Result& now) {
  // The row's answer on this card, once at the start and again when it changes: the line the cost chain and a tester's log read.
  const uint64_t key = 1ull | (uint64_t(now.mode) << 4) | (uint64_t(now.every) << 8) | (uint64_t(now.finder ? 1 : 0) << 16) | (uint64_t(now.overlay ? 1 : 0) << 17) | (uint64_t(now.replaced ? 1 : 0) << 18) |
                       (uint64_t(now.why) << 20);
  if (g_logged.exchange(key, std::memory_order_relaxed) == key) {
    return;
  }
  const gc::GpuClass cls = gc::Known() ? gc::Current() : gc::GpuClass::kLow;
  REXLOG_INFO("{} side mirrors: mode {} (every {} frame{}), glass finder {}, glass overlay {}: {} (word '{}', class {}, ray tracing tier {})", kPrefix, now.mode, now.every, now.every == 1 ? "" : "s",
              now.finder ? "on" : "off", now.overlay ? "wanted" : "not wanted", WhyName(now.why), SideWord().Get(), gc::ClassName(cls), gc::RtTierName(gc::CurrentRtTier()));
}

}  // namespace mirror_setting

// The value the guest's hooks and the host read in the place of the old debug switch alone (gpu.h). Any thread, cheap.
int MirrorViewMode() { return mirror_setting::Current().mode; }

}  // namespace mcla::gpu
