#pragma once
// ===========================================================================
// How LARecomp's input and window code reaches rexglue 0.11's systems
// ===========================================================================
// 0.10: rex::Runtime::instance()->input_system() (an InputSystem*), ->display_window() (a Window*), ->app_context().
// 0.11: the systems are interfaces of the running app (rex/module.h AppInterfaces, REX_INTERFACE(T)): any thread, null
// before the app connects its modules and after it shuts them down. The accessors below cache the pointer (the pad
// poll asks on every XInput call) and drop it in Reset(), which LarecompApp::OnShutdown calls before rex::Stop.
//
// Also here: the pointer arbiter. 0.10 had a cursor and a mouse-capture state per Window that each feature could set
// (cursor_hide.cpp, map_mouse.cpp, mouse_look.cpp), and each one checked what the others had done. 0.11 has ONE pointer
// mode (rex::PointerMode: kFree, kHidden, kLocked) on IInputSystem::SetPointerMode, which the UI system applies on the
// UI thread. So each feature states what it wants and the strongest wins (Locked, then Hidden, then Free).
// ===========================================================================

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>

#include <rex/hid/input_device_list.h>
#include <rex/hid/input_system.h>
#include <rex/input_keys.h>
#include <rex/module.h>
#include <rex/ui/ui_system.h>

namespace larecomp::sdk {

namespace detail {

template <typename T>
struct Cached {
  std::atomic<T*> ptr{nullptr};
  T* Get() {
    T* p = ptr.load(std::memory_order_acquire);
    if (!p) {
      p = REX_INTERFACE(T);
      if (p) ptr.store(p, std::memory_order_release);
    }
    return p;
  }
  void Reset() { ptr.store(nullptr, std::memory_order_release); }
};

// Never destroyed: a guest thread may still ask while the process ends.
template <typename T>
inline Cached<T>& CacheOf() {
  static Cached<T>* cache = new Cached<T>();
  return *cache;
}

}  // namespace detail

// Any thread. Null before the app connects its modules and after Reset.
inline rex::hid::IInputSystem* Input() { return detail::CacheOf<rex::hid::IInputSystem>().Get(); }
// The planned additive SDK interface (the pads with their names; IInputSystem has none). Null when the SDK build has
// no input module or an older input module.
inline rex::hid::IInputDeviceList* DeviceList() { return detail::CacheOf<rex::hid::IInputDeviceList>().Get(); }
inline rex::ui::IUISystem* UI() { return detail::CacheOf<rex::ui::IUISystem>().Get(); }

// LarecompApp::OnShutdown, before the SDK stops its modules.
inline void Reset() {
  detail::CacheOf<rex::hid::IInputSystem>().Reset();
  detail::CacheOf<rex::hid::IInputDeviceList>().Reset();
  detail::CacheOf<rex::ui::IUISystem>().Reset();
}

}  // namespace larecomp::sdk

namespace larecomp::pointer {

// Who can want the pointer. Each states a mode; kFree means no opinion.
enum class Owner : uint8_t {
  kMouseLook,  // mouse_look.cpp: kLocked while the mouse drives the camera on the road
  kMap,        // map_mouse.cpp: kHidden while the mouse drives the full map
  kCursorHide, // cursor_hide.cpp: kHidden after a pad frame or an idle mouse
  kCount
};

namespace detail {
struct State {
  std::array<std::atomic<uint8_t>, size_t(Owner::kCount)> wanted{};
  std::mutex mutex;
  int applied = -1;  // what IInputSystem was last given, -1 before the first call
};
inline State& Get() {
  static State* state = new State();
  return *state;
}
}  // namespace detail

// Any thread. Records what `who` wants and gives the input system the strongest wish (it applies it on the UI thread
// when it changed). Cheap when nothing changes.
inline void Want(Owner who, rex::PointerMode mode) {
  detail::State& s = detail::Get();
  s.wanted[size_t(who)].store(uint8_t(mode), std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(s.mutex);
  uint8_t strongest = uint8_t(rex::PointerMode::kFree);
  for (const auto& w : s.wanted) {
    if (w.load(std::memory_order_relaxed) > strongest) strongest = w.load(std::memory_order_relaxed);
  }
  if (int(strongest) == s.applied) return;
  if (rex::hid::IInputSystem* input = larecomp::sdk::Input()) {
    input->SetPointerMode(rex::PointerMode(strongest));
    s.applied = int(strongest);
  }
}

// What `who` last asked.
inline rex::PointerMode WantedBy(Owner who) {
  return rex::PointerMode(detail::Get().wanted[size_t(who)].load(std::memory_order_relaxed));
}

}  // namespace larecomp::pointer
