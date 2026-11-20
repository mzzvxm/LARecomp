// Mouse look, the runtime half: see mouse_look.h for the design and mouse_look_core.h for the rules.
#include "mouse_look.h"
#include "mouse_look_core.h"

#ifndef REXGLUE_HAS_XEO3_TARGET

#include <atomic>
#include <chrono>
#include <cstdint>

#include "settings/larecomp_settings.h"
#include <rex/hid/input_system.h>
#include <rex/input_keys.h>
#include <rex/ui/ui_event.h>

#include "adaptive_triggers.h"
#include "input_state.h"
#include "mc_engine/keyboard_native.h"
#include "mc_engine/core/logging.h"
#include "map_mouse.h"
#include "mc_engine/sdk_access.h"
#include "mc_engine/online/server_browser/browser_ui.h"
#include "mc_engine/ui/imgui_theme.h"
#include "mc_engine/ui/settings.h"

namespace larecomp::mouselook {
namespace {

using Clock = std::chrono::steady_clock;

// Never 0: 0 means "never" in the stamps below.
int64_t NowMs() {
  static const Clock::time_point start = Clock::now();
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count() + 1000;
}

constexpr int64_t kWantFreshMs = 250;
constexpr int kMaxModeLines = 400;

std::atomic<int64_t> g_want_ms[2] = {{0}, {0}};  // when each client last asked

Gate g_gate;  // game thread only
bool g_logged_want = false;
int g_mode_lines = 0;
// The pointer mode this file asked for (game thread). The input system keeps the raw counts of a locked pointer
// (IInputSystem::TakeMotion: it sums them only while it considers the pointer locked: asked for, window in front and
// the input captured), and applies the mode on the UI thread; SDL lets go of relative mode when the window loses the
// focus and the UI system does not lock again until the input is captured, so the 0.10 workarounds (a listener that
// summed the motion, a post to the UI thread, a retry after a failed relative mode) are gone. When the host refuses
// relative mode the SDL driver recentres the pointer itself and reports the same dx / dy.
std::atomic<bool> g_locked{false};  // written on the game thread, read by cursor_hide and the camera code

bool Fresh(Client c, int64_t now) {
  const int64_t t = g_want_ms[static_cast<int>(c)].load(std::memory_order_relaxed);
  return t != 0 && now - t <= kWantFreshMs;
}

// The client that gets the deltas: the debug free camera when it asks, else the look.
Client Owner(int64_t now) { return Fresh(Client::DebugCam, now) ? Client::DebugCam : Client::Look; }

// The Director panel holds the pointer all the time it shows (not only over its dock: a human clicks on the ground and drags a car in the picture): with relative mouse mode on, no pointer
// position reaches ImGui and the cursor is hidden, so the panel could not be used with a mouse (2026-11-20, 14 of 15 checks of the first mouse run).
bool OverlayWantsMouse() {
  return mcla::ui::SettingsOpen() || larecomp::ServerBrowserCapturesInput() || mcla::ui::PointerHeldByPanel() || mcla::ui::OverlayWantsMouse();
}

}  // namespace

void Want(Client client) { g_want_ms[static_cast<int>(client)].store(NowMs(), std::memory_order_relaxed); }

void Tick() {
  rex::ui::IUISystem* ui = larecomp::sdk::UI();
  rex::hid::IInputSystem* input = larecomp::sdk::Input();
  if (!ui || !input || !ui->HasWindow()) return;
  const int64_t now = NowMs();
  const bool look = Fresh(Client::Look, now);
  const bool debug_cam = Fresh(Client::DebugCam, now);

  Facts f;
  f.wanted = look || debug_cam;
  f.focused = ui->IsFocused();
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

  // Ask for what differs from what was asked: the pointer arbiter (sdk_access.h) gives the input system the strongest
  // wish of the features, which applies it on the UI thread.
  if (want != g_locked.load(std::memory_order_relaxed)) {
    g_locked.store(want, std::memory_order_relaxed);
    (void)input->TakeMotion();  // nothing from before counts: the first events of a new grab are not the player's motion
    larecomp::pointer::Want(larecomp::pointer::Owner::kMouseLook, want ? rex::PointerMode::kLocked : rex::PointerMode::kFree);
  }
}

bool Take(Client client, float& dx, float& dy) {
  dx = 0.0f;
  dy = 0.0f;
  rex::hid::IInputSystem* input = larecomp::sdk::Input();
  if (!input || !Engaged()) return false;
  if (Owner(NowMs()) != client) return false;
  const rex::PointerDelta motion = input->TakeMotion();
  dx = motion.dx;
  dy = motion.dy;
  return true;
}

// Relative mouse mode is on: asked for, and the input system applies it (it does not while the window is behind or the
// input is not captured: GetPointerModeToApply).
// CHECK: GetPointerModeToApply is kLocked only while the window is in front and the input captured (IInputSystem::IsActive), so a focus
// loss ends the lock here without a listener; the first events after a grab (TakeMotion is drained at the change) carry no leftover.
bool Engaged() {
  if (!g_locked.load(std::memory_order_relaxed)) return false;
  rex::hid::IInputSystem* input = larecomp::sdk::Input();
  return input && input->GetPointerModeToApply() == rex::PointerMode::kLocked;
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

}  // namespace larecomp::mouselook

#endif  // REXGLUE_HAS_XEO3_TARGET
