// Mouse look, the runtime half: see mouse_look.h for the design and mouse_look_core.h for the rules.
#include "mouse_look.h"
#include "mouse_look_core.h"

#ifndef REXGLUE_HAS_XEO3_TARGET

#include <atomic>
#include <chrono>
#include <cstdint>

#include "settings/larecomp_settings.h"
#include <rex/hid/input_system.h>
#include <rex/kernel/title_runtime.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/window.h>

#include "imgui.h"

#include "adaptive_triggers.h"
#include "input_state.h"
#include "keyboard_native.h"
#include "logging.h"
#include "map_mouse.h"
#include "online/server_browser/browser_ui.h"
#include "ui/settings.h"

namespace larecomp::mouselook {
namespace {

using Clock = std::chrono::steady_clock;

// Never 0: 0 means "never" in the stamps below.
int64_t NowMs() {
  static const Clock::time_point start = Clock::now();
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count() + 1000;
}

constexpr int64_t kWantFreshMs = 250;
constexpr int64_t kRetryAfterFailureMs = 2000;
constexpr int kMaxModeLines = 400;

std::atomic<int64_t> g_want_ms[2] = {{0}, {0}};  // when each client last asked
DeltaSum g_sum;                                  // filled on the UI thread while relative mode is on, taken on the game thread

std::atomic<bool> g_relative_on{false};      // SDL took relative mode (set on the UI thread); the listener only sums while it is true
std::atomic<bool> g_pending{false};          // a change is queued for the UI thread
std::atomic<int64_t> g_fail_until_ms{0};     // the last attempt failed: no new one before this time
std::atomic<rex::ui::Window*> g_window{nullptr};
std::atomic<bool> g_listener_posted{false};

Gate g_gate;  // game thread only
bool g_logged_want = false;
int g_mode_lines = 0;

rex::ui::Window* GameWindow() {
  auto* rt = rex::Runtime::instance();
  auto* isys = rt ? static_cast<rex::input::InputSystem*>(rt->input_system()) : nullptr;
  return isys ? isys->window() : nullptr;
}

bool Fresh(Client c, int64_t now) {
  const int64_t t = g_want_ms[static_cast<int>(c)].load(std::memory_order_relaxed);
  return t != 0 && now - t <= kWantFreshMs;
}

// The client that gets the deltas: the debug free camera when it asks, else the look.
Client Owner(int64_t now) { return Fresh(Client::DebugCam, now) ? Client::DebugCam : Client::Look; }

// The window's events, layer 0 like the pointer's, the map's and the keyboard's listeners: it never marks an event handled, and an overlay above it gets first refusal.
class Events final : public rex::ui::WindowInputListener, public rex::ui::WindowListener {
 public:
  // In relative mode dx / dy are raw counts (SDK: SDL's xrel times the pixel density, 1.0 on the machines tried) and x / y do not move.
  void OnMouseMove(rex::ui::MouseEvent& e) override {
    if (g_relative_on.load(std::memory_order_relaxed)) g_sum.Add(e.dx(), e.dy());
  }
  // SDL lets go of relative mode when the window loses the focus and would take it again at the focus's return (its window flag stays set): clear the flag now, on the UI
  // thread, so that a window that comes back in a menu does not find the pointer locked. The game thread's next tick asks again if it still wants it.
  void OnLostFocus(rex::ui::UISetupEvent&) override {
    if (g_relative_on.exchange(false, std::memory_order_relaxed)) {
      if (auto* w = g_window.load(std::memory_order_relaxed)) w->SetRelativeMouseMode(false);
    }
    g_sum.Clear();
  }
};

Events g_events;

// The listener is added on the UI thread, the thread that walks the window's lists (keyboard_native.cpp does the same).
void EnsureListener(rex::ui::Window* window) {
  if (g_window.load(std::memory_order_relaxed) == window) return;
  if (g_listener_posted.exchange(true)) return;
  const bool queued = window->app_context().CallInUIThreadDeferred([window]() {
    window->AddInputListener(&g_events, 0);
    window->AddListener(&g_events);
    g_window.store(window, std::memory_order_relaxed);
  });
  if (!queued) g_listener_posted.store(false);
}

// Relative mode is set on the UI thread, in order: the last post is the one that stands.
void Post(rex::ui::Window* window, bool on) {
  g_pending.store(true, std::memory_order_relaxed);
  const bool queued = window->app_context().CallInUIThread([window, on]() {
    if (on) {
      g_sum.Clear();  // nothing from before counts: the first events of a new grab are not the player's motion
      const bool ok = window->SetRelativeMouseMode(true);
      g_relative_on.store(ok, std::memory_order_relaxed);
      if (!ok) {
        g_fail_until_ms.store(NowMs() + kRetryAfterFailureMs, std::memory_order_relaxed);
        MC_WARN("[mouselook] the window did not take relative mouse mode: no mouse look (tried again in {} s)", kRetryAfterFailureMs / 1000);
      }
    } else {
      g_relative_on.store(false, std::memory_order_relaxed);
      window->SetRelativeMouseMode(false);
      g_sum.Clear();
    }
    g_pending.store(false, std::memory_order_relaxed);
  });
  if (!queued) g_pending.store(false, std::memory_order_relaxed);
}

bool OverlayWantsMouse() {
  return mcla::ui::SettingsOpen() || larecomp::ServerBrowserCapturesInput() || (ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureMouse);
}

}  // namespace

void Want(Client client) { g_want_ms[static_cast<int>(client)].store(NowMs(), std::memory_order_relaxed); }

void Tick() {
  rex::ui::Window* window = GameWindow();
  if (!window) return;
  EnsureListener(window);
  const int64_t now = NowMs();
  const bool look = Fresh(Client::Look, now);
  const bool debug_cam = Fresh(Client::DebugCam, now);

  Facts f;
  f.wanted = look || debug_cam;
  f.focused = window->HasFocus();
  f.overlay = OverlayWantsMouse();
  if (debug_cam) {
    // The debug free-fly camera is a tool: the device, the road and the screens are not its business.
    f.keyboard_mouse = true;
    f.driving = true;
  } else {
    f.keyboard_mouse = larecomp::input::ActiveInput() == larecomp::input::InputDevice::KeyboardMouse;
    f.driving = larecomp::triggers::IsDriving();
    f.menu = larecomp::keyboard::MenuScreenUp();
    f.phone = larecomp::keyboard::PhoneScreenUp();
    f.map = MapMouseWantsPointer();
  }
  const bool want = g_gate.Step(f, now);

  if (want != g_logged_want) {
    g_logged_want = want;
    if (g_mode_lines < kMaxModeLines) {
      ++g_mode_lines;
      MC_INFO("[mouselook] relative mouse mode {}: {}", want ? "ON" : "OFF", WhyText(g_gate.why()));
    }
  }

  // Ask the UI thread for what differs from what it has, one change at a time.
  if (want != g_relative_on.load(std::memory_order_relaxed) && !g_pending.load(std::memory_order_relaxed) &&
      (!want || now >= g_fail_until_ms.load(std::memory_order_relaxed))) {
    Post(window, want);
  }
}

bool Take(Client client, float& dx, float& dy) {
  dx = 0.0f;
  dy = 0.0f;
  if (!g_relative_on.load(std::memory_order_relaxed)) return false;
  if (Owner(NowMs()) != client) return false;
  g_sum.Take(dx, dy);
  return true;
}

bool Engaged() { return g_relative_on.load(std::memory_order_relaxed); }

}  // namespace larecomp::mouselook

#else  // REXGLUE_HAS_XEO3_TARGET

namespace larecomp::mouselook {

void Want(Client) {}
void Tick() {}
bool Take(Client, float& dx, float& dy) {
  dx = 0.0f;
  dy = 0.0f;
  return false;
}
bool Engaged() { return false; }

}  // namespace larecomp::mouselook

#endif  // REXGLUE_HAS_XEO3_TARGET
