#ifndef REXGLUE_HAS_XEO3_TARGET
#include "cursor_hide.h"
#include "input_state.h"
#include "mc_engine/core/logging.h"
#include "map_mouse.h"
#include "menu_mouse.h"
#include "mouse_look.h"
#include "mc_engine/ui/settings.h"

#include <atomic>
#include <chrono>
#include <cstdint>

#include "imgui.h"
#include <rex/cvar.h>
#include <rex/input/input_system.h>
#include <rex/runtime.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

REXCVAR_DEFINE_BOOL(hide_mouse_pointer, true, "MCLA/Input",
    "Hide the mouse pointer during play. It goes at once when a controller is "
    "used and after about 3 seconds without mouse movement, only over the game "
    "window while it has focus. Mouse movement, a click, the wheel or losing "
    "focus brings it back, and it stays while the mouse is over an overlay.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DECLARE(bool, mnk_mode);

namespace {

using Clock = std::chrono::steady_clock;
using Visibility = rex::ui::Window::CursorVisibility;

constexpr auto kIdleBeforeHide = std::chrono::milliseconds(3000);

// Bumped on the UI thread by every mouse move, click and wheel turn.
std::atomic<uint64_t> g_mouse_seq{0};
std::atomic<bool> g_listener_attached{false};
// True while the window's kHidden is this file's. Set on the UI thread.
std::atomic<bool> g_we_hid{false};

class PointerListener final : public rex::ui::WindowInputListener {
 public:
    void OnMouseMove(rex::ui::MouseEvent&) override { Bump(); }
    void OnMouseDown(rex::ui::MouseEvent&) override { Bump(); }
    void OnMouseWheel(rex::ui::MouseEvent&) override { Bump(); }

 private:
    static void Bump() { g_mouse_seq.fetch_add(1, std::memory_order_relaxed); }
};

PointerListener g_listener;

// Cursor visibility has to be applied on the UI thread, as in map_mouse.cpp.
// Hides only if nobody else holds the window's cursor, and shows again only
// what this file hid.
void Post(rex::ui::Window* window, bool hide) {
    window->app_context().CallInUIThreadDeferred([window, hide]() {
        if (hide) {
            if (g_we_hid.load(std::memory_order_relaxed) ||
                window->GetCursorVisibility() != Visibility::kVisible) {
                return;
            }
            window->SetCursorVisibility(Visibility::kHidden);
            g_we_hid.store(true, std::memory_order_relaxed);
            MC_INFO("[cursor] pointer hidden");
        } else if (g_we_hid.exchange(false, std::memory_order_relaxed)) {
            if (window->GetCursorVisibility() == Visibility::kHidden) {
                window->SetCursorVisibility(Visibility::kVisible);
            }
            MC_INFO("[cursor] pointer shown");
        }
    });
}

}  // namespace

void TickCursorHide() {
    auto* rt = rex::Runtime::instance();
    auto* isys = rt ? static_cast<rex::input::InputSystem*>(rt->input_system()) : nullptr;
    rex::ui::Window* window = isys ? isys->window() : nullptr;
    if (!window) return;
    if (!g_listener_attached.load(std::memory_order_relaxed)) {
        // Layer 0 like the map's listener: it never marks an event handled, and
        // the ImGui overlays above it get first refusal (see below).
        window->AddInputListener(&g_listener, 0);
        g_listener_attached.store(true, std::memory_order_relaxed);
    }

    const bool ours = g_we_hid.load(std::memory_order_relaxed);
    const Visibility mode = window->GetCursorVisibility();
    if (ours && mode != Visibility::kHidden) {
        // Another feature let go of the cursor and made it visible.
        g_we_hid.store(false, std::memory_order_relaxed);
        return;
    }
    // mnk_mode hides and captures the pointer itself and puts back what it found, so
    // anything left hidden here would stay hidden after it lets go.
    if (REXCVAR_GET(mnk_mode)) {
        if (ours) Post(window, false);
        return;
    }
    // The full map hides the pointer while the mouse drives it (map_mouse_hide_cursor)
    // and mouse look hides and locks it (relative mouse mode, mouse_look.h: only while
    // it is actually on, that is, while the keyboard and mouse are in use on the road).
    // Both own it while they are up; so does anything else that left the window on kHidden.
    if (MapMouseHidesCursor() || larecomp::mouselook::Engaged()) return;
    if (!ours && mode != Visibility::kVisible) return;

    static Clock::time_point last_activity = Clock::now();
    static uint64_t last_seq = 0;
    static bool was_focused = true;

    const auto now = Clock::now();
    const bool focused = window->HasFocus();
    // Overlays take the mouse events before the listener sees them, so the mouse
    // being over one counts as activity too: the pointer stays while it is used,
    // and does not vanish the moment the overlay closes. The F4 panel keeps it for
    // as long as it is open, wherever the mouse is.
    const bool over_overlay = mcla::ui::SettingsOpen() ||
                              (ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureMouse);
    // Which device the player uses comes from the shared state machine (input_state.h: last input
    // wins, with hysteresis; a pad frame, a key or a deliberate mouse move switches it), not from
    // reading the pad here. A controller in use hides the pointer at once and keeps it hidden: a bump
    // of the mouse that the rules call jitter changes neither the device nor the pointer. With the
    // keyboard and mouse in use the pointer shows while the mouse moves and hides after 3 s still.
    const bool pad_active = larecomp::input::ActiveInput() == larecomp::input::InputDevice::Pad;
    // A menu is up and the mouse is in use (MOUSE IN MENUS): the pointer is the menu's, on screen for as long as that holds, still or not (menu_mouse.h). A controller or a
    // key that takes over makes it false, and the pointer goes at once, as it always did.
    const bool menu_pointer = larecomp::MenuMouseWantsPointer();
    const uint64_t seq = g_mouse_seq.load(std::memory_order_relaxed);
    if (over_overlay || menu_pointer || (!pad_active && (seq != last_seq || focused != was_focused))) {
        last_activity = now;
    }
    last_seq = seq;
    was_focused = focused;

    // The map keeps the pointer visible when the option to hide it there is off.
    const bool want_hidden = REXCVAR_GET(hide_mouse_pointer) && focused && !over_overlay &&
                             !MapMouseWantsPointer() && !menu_pointer &&
                             (pad_active || now - last_activity >= kIdleBeforeHide);
    if (want_hidden != ours) Post(window, want_hidden);
}

#else  // REXGLUE_HAS_XEO3_TARGET

void TickCursorHide() {}

#endif  // REXGLUE_HAS_XEO3_TARGET
