#pragma once
//
// The one watcher for "which game controllers are plugged in", shared by everything in the exe that
// used to scan for them (the DualSense trigger effects, adaptive_triggers.cpp; the PlayStation
// button prompts of button_prompts = auto, hooks/ui.cpp through ui/pad_vendor.h).
//
// Windows: CM_Register_Notification for the HID device interface class (cfgmgr32, Windows 8+). It
// needs no window and no message loop, so it cannot take WM_INPUT or WM_DEVICECHANGE from SDL (the
// SDK's SDL registers for both; a second RegisterRawInputDevices from this process would replace
// its registration, KBM-DESIGN.md 3.1). The OS hands each arrival and removal to a thread-pool
// callback with the interface's symbolic link, which already names the vendor and product
// (VID_054C&PID_0CE6), so nothing is queried. One list of the present interfaces is taken at
// start, on a thread of its own, after the registration so that nothing falls between the two.
//
// Elsewhere (Linux, later macOS): the process has ONE SDL, the SDK's. An SDL event watch hears
// the joystick and gamepad added/removed events that the SDK's driver already pumps, and the
// answers are read from SDL's own list, once per change. The watch can be added only once SDL's
// gamepad layer is up (trigger_sdl.h), which is noticed by a flag check on the first calls, not
// by polling for devices.
//
// The game thread only ever reads atomics (SonyPadPresent, DualSenseCount, Generation). No scan
// runs on it, ever. A consumer that needs more than the answers (the DualSense interface paths)
// reads them on a thread of its own after Generation() moved, or gets a call on every change
// through AddChangeListener (keep that cheap: set a flag, notify a condition variable).
//
// For the active-device work (KBM-DESIGN.md phase P2, feat/input-state): build on this, do not add
// a second notification. SonyPadPresent() is the seed for button_prompts = auto until a device has
// been used; Generation() / AddChangeListener() say "a pad came or went" (the active device falls
// back when its pad is gone); the SDK's own device list stays the source for WHICH pad is in use.
//

#include <cstdint>
#if defined(_WIN32)
#include <string>
#include <vector>
#endif

namespace larecomp::devwatch {

// Starts watching. Idempotent, any thread, returns at once (the list is taken on another thread:
// until it lands, Ready() is false and the answers read "nothing plugged in"). Called early from
// the app's setup; the first query starts it too if nobody did.
void Start();

// Stops the notifications (Windows: unregisters; waits for a callback that is running). Idempotent.
// Never from inside a change listener. The process may also end without it.
void Stop();

// The first list of devices has landed (Windows). Elsewhere: SDL's gamepad layer has been found.
bool Ready();

// A Sony controller (vendor 054C, over USB or Bluetooth) is plugged in. Atomic read.
bool SonyPadPresent();

// How many DualSense / DualSense Edge HID interfaces are present. Windows only: 0 elsewhere, where
// the DualSense code reads SDL's list itself after Generation() moved. Atomic read.
uint32_t DualSenseCount();

// Moves whenever something this module reports changed (Windows: the Sony flag or the DualSense
// set; elsewhere: SDL added or removed a joystick). Atomic read (elsewhere it also notices that SDL's
// gamepad layer came up, once).
uint32_t Generation();

// Called on every change, on the thread that noticed it (Windows: a pool thread; elsewhere
// whichever thread SDL posted the event from), with the state already updated. For the life of the
// process; at most 8. Returns false when the table is full.
using ChangeListener = void (*)(void* arg);
bool AddChangeListener(ChangeListener fn, void* arg);

#if defined(_WIN32)
// The DualSense interface paths (lower-case, sorted), copied under a lock: not for the game thread.
std::vector<std::wstring> DualSensePaths();
#endif

}  // namespace larecomp::devwatch
