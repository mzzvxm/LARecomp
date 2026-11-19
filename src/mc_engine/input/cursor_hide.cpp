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

#include "settings/larecomp_settings.h"
#include <rex/hid/input_system.h>
#include <rex/input_keys.h>
#include <rex/ui/ui_event.h>

#include "mc_engine/sdk_access.h"
#include "mc_engine/ui_events.h"
#include "mc_engine/ui/imgui_theme.h"


namespace {

using Clock = std::chrono::steady_clock;

constexpr auto kIdleBeforeHide = std::chrono::milliseconds(3000);

// Bumped on the UI thread by every mouse move, click and wheel turn.
std::atomic<uint64_t> g_mouse_seq{0};
std::atomic<bool> g_listener_attached{false};
// True while this file asks for a hidden pointer (the pointer arbiter, sdk_access.h, holds the other wishes).
std::atomic<bool> g_we_hid{false};

// The UI system's input events (ui_events.h): layer 0 like before, it never marks an event handled, and the ImGui
// overlays above it get first refusal.
void OnInput(rex::InputEvent& event) {
    using Type = rex::InputEvent::Type;
    if (event.type == Type::kMotion || event.type == Type::kButtonDown || event.type == Type::kWheel) {
        g_mouse_seq.fetch_add(1, std::memory_order_relaxed);
    }
}

// What this file wants the pointer to be. 0.10 set the window's cursor visibility on the UI thread, only if nobody
// else held it, and showed it again only when it was this file's hide; the arbiter keeps the strongest wish of the
// three features (mouse look locks, the map hides, this hides), so a release here never undoes another's.
void Post(bool hide) {
    if (hide) {
        if (g_we_hid.exchange(true, std::memory_order_relaxed)) return;
        larecomp::pointer::Want(larecomp::pointer::Owner::kCursorHide, rex::PointerMode::kHidden);
        MC_INFO("[cursor] pointer hidden");
    } else if (g_we_hid.exchange(false, std::memory_order_relaxed)) {
        larecomp::pointer::Want(larecomp::pointer::Owner::kCursorHide, rex::PointerMode::kFree);
        MC_INFO("[cursor] pointer shown");
    }
}

}  // namespace

void TickCursorHide() {
    rex::ui::IUISystem* ui = larecomp::sdk::UI();
    if (!ui || !ui->HasWindow()) return;
    if (!g_listener_attached.load(std::memory_order_relaxed)) {
        larecomp::uievents::AddInputHandler(&OnInput);
        g_listener_attached.store(true, std::memory_order_relaxed);
    }

    const bool ours = g_we_hid.load(std::memory_order_relaxed);
    // mnk_mode hides and captures the pointer itself and puts back what it found, so
    // anything left hidden here would stay hidden after it lets go.
    if (larecomp::settings::GetAs<bool>("hid.mnk_mode")) {
        if (ours) Post(false);
        return;
    }
    // The full map hides the pointer while the mouse drives it (map_mouse_hide_cursor)
    // and mouse look hides and locks it (relative mouse mode, mouse_look.h: only while
    // it is actually on, that is, while the keyboard and mouse are in use on the road).
    // Both own it while they are up. Our own wish is let go meanwhile, so that it does not outlast theirs.
    if (MapMouseHidesCursor() || larecomp::mouselook::Engaged()) {
        if (ours) Post(false);
        return;
    }

    static Clock::time_point last_activity = Clock::now();
    static uint64_t last_seq = 0;
    static bool was_focused = true;

    const auto now = Clock::now();
    const bool focused = ui->IsFocused();
    // Overlays take the mouse events before the listener sees them, so the mouse
    // being over one counts as activity too: the pointer stays while it is used,
    // and does not vanish the moment the overlay closes. The F4 panel keeps it for
    // as long as it is open, wherever the mouse is.
    const bool over_overlay = mcla::ui::SettingsOpen() || mcla::ui::OverlayWantsMouse();
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
    const bool want_hidden = larecomp::settings::hide_mouse_pointer.Get() && focused && !over_overlay &&
                             !MapMouseWantsPointer() && !menu_pointer &&
                             (pad_active || now - last_activity >= kIdleBeforeHide);
    if (want_hidden != ours) Post(want_hidden);
}

#else  // REXGLUE_HAS_XEO3_TARGET

void TickCursorHide() {}

#endif  // REXGLUE_HAS_XEO3_TARGET
