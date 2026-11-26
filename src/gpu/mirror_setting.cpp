#include "mirror_setting.h"

#include <atomic>
#include <rex/logging/log_category.h>

#include "gpu.h"
#include "live_text.h"
#include "settings/larecomp_settings.h"

namespace mcla::gpu::mirror_setting {

Result Current() {
  static LiveText word("mcla_gpu_side_mirrors");
  return Resolve(word.Get(), larecomp::settings::mcla_gpu_mirror_view.Get(), larecomp::settings::mcla_gpu_mirror_glass.Get(),
                 larecomp::settings::mcla_gpu_mirror_overlay.Get());
}

void LogIfChanged(const Result& now) {
  static std::atomic<unsigned> logged{0};
  const unsigned key = unsigned(now.mode) | (unsigned(now.finder) << 4) | (unsigned(now.overlay) << 5) | 0x80u;
  if (logged.exchange(key, std::memory_order_relaxed) != key) {
    REXLOG_INFO("[mirror setting] mirrors: view {}, glass finder {}, glass overlay {}", now.mode, now.finder, now.overlay);
  }
}

}  // namespace mcla::gpu::mirror_setting

namespace mcla::gpu {

int MirrorViewMode() { const int mode = mirror_setting::Current().mode;
  return mode != 0 ? mode : int(larecomp::settings::rearview.Get()); }

}  // namespace mcla::gpu
