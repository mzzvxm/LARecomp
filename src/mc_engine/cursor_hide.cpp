#ifndef REXGLUE_HAS_XEO3_TARGET
#include "cursor_hide.h"
#include "logging.h"
#include "map_mouse.h"
#include "ui/settings.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>

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

REXCVAR_DECLARE(bool, cam_freelook);
REXCVAR_DECLARE(bool, mnk_mode);

namespace {

using Clock = std::chrono::steady_clock;
using Visibility = rex::ui::Window::CursorVisibility;

constexpr auto kIdleBeforeHide = std::chrono::milliseconds(3000);
// A bit over a third of the stick's travel, so drift does not count as use.
constexpr int kStickPast = 12000;
constexpr int kTriggerPast = 30;

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

// Any real pad (the keyboard has its own native path and is not looked at):
// a button, a trigger or a stick well past the dead zone.
bool PadInUse(rex::input::InputSystem* isys) {
    rex::input::X_INPUT_STATE st{};
    if (isys->GetState(0, &st) != 0) return false;
    const uint16_t buttons = st.gamepad.buttons;
    if (buttons != 0) return true;
    if (st.gamepad.left_trigger >= kTriggerPast || st.gamepad.right_trigger >= kTriggerPast)
        return true;
    const int16_t axes[4] = {st.gamepad.thumb_lx, st.gamepad.thumb_ly, st.gamepad.thumb_rx,
                             st.gamepad.thumb_ry};
    for (int16_t a : axes) {
        if (std::abs(int(a)) >= kStickPast) return true;
    }
    return false;
}

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
    // and mouse look hides and recentres it. Both own it while they are up; so does
    // anything else that left the window on kHidden.
    if (MapMouseHidesCursor() || REXCVAR_GET(cam_freelook)) return;
    if (!ours && mode != Visibility::kVisible) return;

    static Clock::time_point last_activity = Clock::now();
    static uint64_t last_seq = 0;
    static bool pad_hid = false;  // a pad was used after the last mouse activity
    static bool pad_was_in_use = false;
    static bool was_focused = true;

    const auto now = Clock::now();
    const bool focused = window->HasFocus();
    // Overlays take the mouse events before the listener sees them, so the mouse
    // being over one counts as activity too: the pointer stays while it is used,
    // and does not vanish the moment the overlay closes. The F4 panel keeps it for
    // as long as it is open, wherever the mouse is.
    const bool over_overlay = mcla::ui::SettingsOpen() ||
                              (ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureMouse);
    const uint64_t seq = g_mouse_seq.load(std::memory_order_relaxed);
    if (seq != last_seq || focused != was_focused || over_overlay) {
        last_seq = seq;
        was_focused = focused;
        last_activity = now;
        pad_hid = false;
    }
    // On the rising edge only: a stick held while the mouse is nudged would
    // otherwise hide the pointer again on the next frame.
    const bool pad_in_use = PadInUse(isys);
    if (pad_in_use && !pad_was_in_use) pad_hid = true;
    pad_was_in_use = pad_in_use;

    // The map keeps the pointer visible when the option to hide it there is off.
    const bool want_hidden = REXCVAR_GET(hide_mouse_pointer) && focused && !over_overlay &&
                             !MapMouseWantsPointer() &&
                             (pad_hid || now - last_activity >= kIdleBeforeHide);
    if (want_hidden != ours) Post(window, want_hidden);
}

#else  // REXGLUE_HAS_XEO3_TARGET

void TickCursorHide() {}

#endif  // REXGLUE_HAS_XEO3_TARGET
