// Mouse look, the runtime half: see mouse_look.h for the design and mouse_look_core.h for the rules.
#include "mouse_look.h"
#include "mouse_look_core.h"

#ifndef REXGLUE_HAS_XEO3_TARGET

#include <atomic>
#include <chrono>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/input/input_system.h>
#include <rex/runtime.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

#include "imgui.h"

#include "adaptive_triggers.h"
#include "input_state.h"
#include "mc_engine/keyboard_native.h"
#include "mc_engine/core/logging.h"
#include "map_mouse.h"
#include "mc_engine/online/server_browser/browser_ui.h"
#include "mc_engine/ui/settings.h"

REXCVAR_DEFINE_BOOL(mouse_look_log, false, "MCLA/Camera",
    "Log what the mouse look does: the raw mouse counts and the view angles about every tenth of a second while it moves "
    "(at most 4000 lines). The changes of relative mouse mode are always logged.")
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(mouse_menu_drag, true, "MCLA/Camera",
    "Off the road (the garage, the pause menu, the photo mode), hold a mouse button and drag to move the right stick: it turns the "
    "garage's camera, enters the photo mode from the pause menu (drag sideways) and moves the photo camera, as the controller's right stick does.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

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
std::atomic<bool> g_drag_on{false};   // the gate is on because of a menu drag: the deltas are the drag's, not the look's
std::atomic<int32_t> g_drag_axes{0};  // rx in the low 16 bits, ry in the high ones
DragStick g_drag_stick;               // game thread
int64_t g_drag_last_ms = 0;

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

void MenuDragDiagTick();  // menu_drag_diag.cpp

void Tick() {
  MenuDragDiagTick();
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
    f.drag = REXCVAR_GET(mouse_menu_drag) && larecomp::keyboard::MouseDragButtonHeld() && (f.menu || !f.driving);
  }
  const bool want = g_gate.Step(f, now);
  const bool drag = want && g_gate.why() == Why::Drag;
  g_drag_on.store(drag, std::memory_order_relaxed);
  {
    const float dt = g_drag_last_ms ? float(now - g_drag_last_ms) / 1000.0f : 0.0f;
    g_drag_last_ms = now;
    float dx = 0.0f, dy = 0.0f;
    const bool live = drag && g_relative_on.load(std::memory_order_relaxed);
    if (live) g_sum.Take(dx, dy);
    StepDrag(g_drag_stick, live, dx, dy, dt, DragParams{});
    const int16_t rx = SteerToAxis(g_drag_stick.x), ry = SteerToAxis(g_drag_stick.y);
    g_drag_axes.store(int32_t(uint32_t(uint16_t(rx)) | (uint32_t(uint16_t(ry)) << 16)), std::memory_order_relaxed);
  }

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
  if (g_drag_on.load(std::memory_order_relaxed)) return false;  // a menu drag owns the motion
  if (Owner(NowMs()) != client) return false;
  g_sum.Take(dx, dy);
  return true;
}

bool Engaged() { return g_relative_on.load(std::memory_order_relaxed); }

bool MenuDragAxes(int16_t& rx, int16_t& ry) {
  const uint32_t v = uint32_t(g_drag_axes.load(std::memory_order_relaxed));
  rx = int16_t(uint16_t(v & 0xFFFF));
  ry = int16_t(uint16_t(v >> 16));
  return v != 0;
}

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
bool MenuDragAxes(int16_t& rx, int16_t& ry) {
  rx = ry = 0;
  return false;
}

}  // namespace larecomp::mouselook

#endif  // REXGLUE_HAS_XEO3_TARGET
