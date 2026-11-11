// Ultrawide: the aspect ratio the game is shown at. See ultrawide.h.

#ifndef REXGLUE_HAS_XEO3_TARGET
#include "ultrawide.h"

#include <rex/cvar.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <string>
#include <string_view>

#include "hooks/hooks_internal.h"
#include "larecomp_log.h"
#if defined(LARECOMP_HAS_GPU)
#include "gpu/gpu.h"
#endif

REXCVAR_DECLARE(std::string, aspect_ratio);  // hooks/graphics.cpp

namespace mc::ultrawide {

namespace {

// The window's size, width << 32 | height; 0 until the app says.
std::atomic<uint64_t> g_window{0};
// The aspect ratio in force, between kBaseAspect and kMaxAspect.
std::atomic<double> g_aspect{kBaseAspect};

// The setting's text, as the last change left it (a change callback is given the new value; reading
// the cvar inside the callback is not safe, it may run with the registry locked).
std::mutex g_ask_mutex;
std::string g_ask;
bool g_ask_known = false;  // Init ran

// The SDK presenter (the Xenia renderer) fits the 1280x720 picture into the window with black bars
// unless present_letterbox is off, when it stretches the picture over the whole window: what a
// widened picture needs. The value given to it last; the Plume renderer has its own present.
bool g_xenia_stretch = false;

// The fixed choices of the setting. 16:10 is below the game's own shape: the window then keeps the
// 16:9 picture with black bars above and below, as 16:9 does.
double FixedAspect(std::string_view ask) {
  if (ask == "21:9") return 7.0 / 3.0;
  if (ask == "32:9") return kMaxAspect;
  return kBaseAspect;
}

double Choose(std::string_view ask, uint64_t window) {
  const uint32_t width = uint32_t(window >> 32);
  const uint32_t height = uint32_t(window & 0xFFFFFFFFu);
  double aspect = kBaseAspect;
  if (ask == "auto" || ask.empty()) {
    if (width != 0 && height != 0) {
      aspect = double(width) / double(height);
    }
  } else {
    aspect = FixedAspect(ask);
  }
  aspect = std::clamp(aspect, kBaseAspect, kMaxAspect);
  // A window a few pixels off 16:9 (a frame, a scaling factor's rounding) is 16:9: nothing to
  // widen, and the picture keeps its place.
  if (aspect < kBaseAspect * 1.005) {
    aspect = kBaseAspect;
  }
  return aspect;
}

// The setting or the window changed. Never called with the cvar registry locked except from the
// setting's own change callback, which takes `ask` from the callback's argument.
void Recompute(std::string_view ask) {
  const uint64_t window = g_window.load(std::memory_order_acquire);
  const double aspect = Choose(ask, window);
  const double before = g_aspect.exchange(aspect, std::memory_order_acq_rel);
  if (std::abs(aspect - before) > 1e-4) {
    LARECOMP_APP_INFO("ultrawide: the aspect ratio is {:.4f}:1 (setting \"{}\", window {}x{}){}",
                      aspect, ask, uint32_t(window >> 32), uint32_t(window & 0xFFFFFFFFu),
                      aspect > kBaseAspect * 1.001 ? "" : ", the game's own 16:9");
    ApplyAspectRatioPatch();
  }
}

// The window's size arrives (SetupPresentation) before Init does: the cvar has its value by then.
std::string Ask() {
  {
    std::lock_guard<std::mutex> lock(g_ask_mutex);
    if (g_ask_known) {
      return g_ask;
    }
  }
  return REXCVAR_GET(aspect_ratio);
}

bool PlumeRenderer() {
#if defined(LARECOMP_HAS_GPU)
  return mcla::gpu::Enabled();
#else
  return false;
#endif
}

}  // namespace

void SetWindowSize(uint32_t width, uint32_t height) {
  if (width == 0 || height == 0) {
    return;
  }
  g_window.store(uint64_t(width) << 32 | height, std::memory_order_release);
  Recompute(Ask());
  // The Xenia renderer's present follows the picture. Here, on the UI thread outside any cvar
  // callback, where setting another cvar is safe.
  // CHECK: the Xenia renderer in a 21:9 window: the picture fills it (present_letterbox off) and
  // goes back to black bars when the window is made 16:9 again.
  if (!PlumeRenderer()) {
    const bool stretch = Active();
    if (stretch != g_xenia_stretch) {
      g_xenia_stretch = stretch;
      rex::cvar::SetFlagByName("present_letterbox", stretch ? "false" : "true");
    }
  }
}

double DisplayAspect() { return g_aspect.load(std::memory_order_acquire); }

double UiWidthFraction() {
  const double aspect = DisplayAspect();
  return aspect > kBaseAspect * 1.001 ? kBaseAspect / aspect : 1.0;
}

// From InitHooks, once, before any guest code: the setting is read, and a change of it (the F4
// panel, the launcher's config) is followed while the game runs.
void Init() {
  {
    std::lock_guard<std::mutex> lock(g_ask_mutex);
    g_ask = REXCVAR_GET(aspect_ratio);
    g_ask_known = true;
  }
  rex::cvar::RegisterChangeCallback("aspect_ratio", [](std::string_view, std::string_view value) {
    {
      std::lock_guard<std::mutex> lock(g_ask_mutex);
      g_ask = std::string(value);
    }
    Recompute(value);
  });
  // The aspect ratio the window's size (SetupPresentation) and the setting made, to the guest.
  Recompute(Ask());
  ApplyAspectRatioPatch();
}

}  // namespace mc::ultrawide
#endif  // REXGLUE_HAS_XEO3_TARGET
