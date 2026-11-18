#pragma once
// ===========================================================================
// The window and input events of rexglue 0.11's UI system, for LARecomp's own consumers
// ===========================================================================
// 0.10 let any code add a rex::ui::WindowInputListener (layer 0: it never marks an event handled, and the ImGui
// overlays above it get first refusal) and a rex::ui::WindowListener to the window: cursor_hide, map_mouse,
// mouse_look, input_state and keyboard_native each did. 0.11's IUISystem::AddEventListener does the same job (one
// IUIEventListener per caller, events on the UI thread, held back from the listeners while an overlay wants the
// keyboard or the mouse), but its guard must be destroyed before the UI system is, and these consumers live in
// statics that outlive it. So LarecompApp registers ONE listener (its own, a ScopeGuard it lets go in OnShutdown)
// and fans every event out through this table; a consumer adds a plain function, from any thread, once.
//
// Both kinds of event arrive on the UI thread. A handler must not block and must not call back into the UI
// system's listener list. The input events are the ones the UI system gives listeners: key and mouse events that no
// overlay holds (an overlay that wants the keyboard keeps the typing from the game; one that wants the mouse keeps
// motion, button-down and wheel). Key-up and button-up always arrive (the UI system does the same).
// ===========================================================================

#include <array>
#include <atomic>
#include <mutex>

#include <rex/input_keys.h>
#include <rex/ui/ui_event.h>

namespace larecomp::uievents {

using InputHandler = void (*)(rex::InputEvent& event);
using WindowHandler = void (*)(const rex::ui::UIEvent& event);
// Focus gained (true) or lost (false), told once the UI system has recorded it. The window event itself (kFocus) carries
// no direction, and IUISystem::IsFocused() still has the old value while the listeners run (the UI system tells its
// listeners first, then updates), so LarecompApp posts the answer to the UI thread and calls these from there.
using FocusHandler = void (*)(bool focused);

namespace detail {

template <typename Fn>
struct Table {
  static constexpr int kMax = 16;
  std::mutex mutex;
  std::array<Fn, kMax> slots{};
  std::atomic<int> count{0};

  bool Add(Fn fn) {
    if (!fn) return false;
    std::lock_guard<std::mutex> lock(mutex);
    const int n = count.load(std::memory_order_relaxed);
    for (int i = 0; i < n; ++i) {
      if (slots[size_t(i)] == fn) return true;  // already there: adding twice is not an error
    }
    if (n >= kMax) return false;
    slots[size_t(n)] = fn;
    count.store(n + 1, std::memory_order_release);
    return true;
  }
};

// Never destroyed: a thread may still be inside a handler at exit, like the tables of input_state.cpp.
inline Table<InputHandler>& Inputs() {
  static Table<InputHandler>* table = new Table<InputHandler>();
  return *table;
}
inline Table<WindowHandler>& Windows() {
  static Table<WindowHandler>* table = new Table<WindowHandler>();
  return *table;
}
inline Table<FocusHandler>& Focuses() {
  static Table<FocusHandler>* table = new Table<FocusHandler>();
  return *table;
}

}  // namespace detail

// Any thread. False when the table is full (16 handlers).
inline bool AddInputHandler(InputHandler fn) { return detail::Inputs().Add(fn); }
inline bool AddWindowHandler(WindowHandler fn) { return detail::Windows().Add(fn); }
inline bool AddFocusHandler(FocusHandler fn) { return detail::Focuses().Add(fn); }

// UI thread, from LarecompApp's IUIEventListener.
inline void DispatchInput(rex::InputEvent& event) {
  auto& table = detail::Inputs();
  const int n = table.count.load(std::memory_order_acquire);
  for (int i = 0; i < n; ++i) table.slots[size_t(i)](event);
}
inline void DispatchWindow(const rex::ui::UIEvent& event) {
  auto& table = detail::Windows();
  const int n = table.count.load(std::memory_order_acquire);
  for (int i = 0; i < n; ++i) table.slots[size_t(i)](event);
}
inline void DispatchFocus(bool focused) {
  auto& table = detail::Focuses();
  const int n = table.count.load(std::memory_order_acquire);
  for (int i = 0; i < n; ++i) table.slots[size_t(i)](focused);
}

}  // namespace larecomp::uievents
