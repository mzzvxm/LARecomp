#pragma once
//
// Which input device the player is using: a controller, or the keyboard and mouse. The one answer the
// rest of the exe codes against (KBM-DESIGN.md, phase P2). Last input wins, with hysteresis; hot-plug
// comes from events; nothing here polls.
//
// This header is the stable part. Phases P3 (the native keyboard), P4 (live button prompts), P5 (mouse
// look, the pointer) and P7 (the ImGui dialogs, the controller toast) are written against exactly these
// declarations: keep them as they are, add to them rather than change them.
//
//   ActiveInput()        the device that produced the last real input. Before any input: Pad when a
//                        controller is connected, else KeyboardMouse.
//   ActiveGlyphFamily()  the button glyphs the prompts should show now: the controller's family while a
//                        controller is in use, Keyboard while the keyboard and mouse are, with the
//                        button_prompts setting (xbox, playstation, keyboard) as an override of both.
//                        Switch, Steam Deck and generic controllers give Xbox (v1, owner decision).
//   PromptStrip()        additive: the strip of the game's own art the movies' `platform` flag picks,
//                        Xbox or PlayStation, never Keyboard. It follows the last controller used (or the
//                        forced setting) and does NOT flip when the keyboard takes over, so the key caps
//                        P4 paints go over the strip that was on screen.
//   Generation()         moves whenever the device, the family, the strip or the setting changed (24 bits,
//                        wraps). A consumer that repaints compares this with the value it painted for.
//   AddInputListener()   a call on every change, with the state already updated.
//
// Threads: every reader is an atomic load and safe from any thread, the game thread included. The state
// is written from the UI thread (keys, mouse, focus), from the thread that polls the pad (the game's
// XamInputGetState wrapper, wheel/wheel_fake.cpp) and from a pool thread of the device watcher (the
// start-up guess). No scan, no syscall and no allocation on a reader's path.
//
// A controller that dies in the player's hands: the SDK's NOP stand-in keeps user 0 connected, so the game would
// never notice. When the pad that made the last controller input is removed from the SDK's list while a pad is
// the active device and the game is being driven (no wheel), the game is told "not connected" for user 0 for ten
// polls (InputActivityScope and PadLossCaps below), which makes it open its pause menu (KBM P0c); then the
// stand-in answers again, so the keyboard and a returning pad work as usual. Nothing happens when the keyboard
// and mouse are the active device, at the title, in menus or loads, or when the pad that left was not in use.
//
// The rules (thresholds in input_state_core.h, KBM-DESIGN.md 2.2): a pad button going down, a trigger
// rising past 30, a stick crossing its dead zone or moving by more than 2000 counts; a key going down
// (not a repeat, not key-up, not a lone Shift / Ctrl / Alt / Win, a lock key, Pause, Print Screen, or one
// of F1-F12 that no kb_* binding names), a mouse button going down and a wheel notch count at once; the
// host key of an extra_button_* setting (a back paddle) counts as a pad button; mouse motion counts only
// after at least 8 px of travel within 200 ms in two events, not within 1 s of a controller's last input,
// not within 250 ms of a switch, not in the first 300 ms after the window got focus.
//

#include <cstdint>
#include <string>
#include <vector>

namespace larecomp::input {

enum class InputDevice : uint8_t { Pad = 0, KeyboardMouse = 1 };

enum class GlyphFamily : uint8_t { Xbox = 0, PlayStation = 1, Keyboard = 2 };

// What the host's settings say about a key (KBM-DESIGN.md 2.2), asked when a key goes down (keyboard_native.h RoleOfKey):
//   Plain        no setting names it;
//   Bound        one of the kb_ui_* / kb_drive_* bindings names it: F1-F12 count as keyboard activity only then;
//   ExtraButton  a host key of an extra_button_* setting: the pad software of a pad with back paddles sends it and the game takes it from the keyboard feed as the pad
//                button L4 / L5 / R4 / R5, so pressing it is PAD activity, not keyboard activity.
enum class KeyRole : uint8_t { Plain = 0, Bound = 1, ExtraButton = 2 };

InputDevice ActiveInput();
GlyphFamily ActiveGlyphFamily();
GlyphFamily PromptStrip();
uint32_t Generation();

// Called on every change (device, family, strip or the button_prompts setting), on the thread that
// noticed it (the UI thread for keys and mouse, the thread that polls the pad, a pool thread for the
// start-up guess), with the state already updated. Keep it cheap (set a flag, bump a counter) and read
// the answers with the functions above. For the life of the process; at most 8. Returns false when the
// table is full. Do not call this from inside a listener. The state's first set-up (the start-up guess and
// the button_prompts setting, at the first read of any answer) is not announced: read the answers once
// after registering.
using InputListener = void (*)(void* arg);
bool AddInputListener(InputListener fn, void* arg);

// ---- the controllers the SDK lists (the controller toast, KBM P7) -------------------------------------------------
//
// One controller of the SDK's device list. The SDK's NOP stand-in and the keyboard-and-mouse pad of mnk_mode are not
// controllers and are not here.
struct ControllerInfo {
  uint64_t id = 0;   // the SDK's handle: never reused within a run, so a pad that comes back is a new one
  std::string name;  // the SDK's name for it ("PS5 Controller"); empty when the driver gave none
};

// The controllers as of the last time the SDK's list changed, in connection order; a copy, any thread. Empty before
// the game's first input poll (the list is read there, on the thread that polls the pad: the SDK says to).
std::vector<ControllerInfo> Controllers();

// Called when that set changes (a controller came or went; the first list too, when it holds one), on the thread that
// polls the pad, with Controllers() already updated. Same rules as AddInputListener: cheap (set a flag), for the life
// of the process, at most 8, returns false when the table is full, not from inside a listener. Nothing polls: the
// call comes from the SDK's device counter moving.
bool AddControllerListener(InputListener fn, void* arg);

// An overlay that reads the pads itself while it is open (the browsers' dialogs, ui/gamepad.h ReadOverlayPads) tells the
// state what it read, once per frame it draws: the SDK answers the game with a pad at rest while such an overlay is
// open, so the poll of the game's own state cannot see the player pick up a pad inside it. The same rules decide
// whether that was activity (a button going down, a stick across its dead zone); the pad's family is not known here
// and stays what it was. `buttons` are XInput's bits, sticks -1..1 with up and right positive, `connected` is false when
// no pad answered. UI thread.
void NoteOverlayPad(bool connected, uint16_t buttons, float lx, float ly, float rx, float ry);

// Per game frame (hooks.cpp, Patch_DeltaTimePre): attaches the window listener on the first frame the
// window exists, nothing else. Not for other phases.
void Tick();

// On the stack of the XamInputGetState wrapper (wheel/wheel_fake.cpp), constructed right after the
// FreecamPadScope so that it runs first when the wrapper returns: it sees the pad state the game is
// about to read (the SDK's, the player's dead zones, a wheel, the test harness's pad) before the free
// camera clears it. `result` is the wrapper's r3: 0 = a pad answered. Also notices that the SDK's list
// of devices changed (one counter compared per call).
//
// It is also the one place that answers "not connected": when the controller in the player's hands was
// removed while the game is driven (the rule is PadLossRule in input_state_core.h), the destructor clears
// the state and sets `result` to ERROR_DEVICE_NOT_CONNECTED (0x48F) for the next few polls of user 0, so
// that the game opens its pause menu; then the SDK's stand-in answers again. The FreecamPadScope after it
// sees the failed call and leaves the state alone.
class InputActivityScope {
 public:
  InputActivityScope(uint8_t* base, uint32_t user, uint32_t state, uint64_t& result)
      : base_(base), user_(user), state_(state), result_(result) {}
  ~InputActivityScope();
  InputActivityScope(const InputActivityScope&) = delete;
  InputActivityScope& operator=(const InputActivityScope&) = delete;

 private:
  [[maybe_unused]] uint8_t* base_;  // unused in the XEO3 stub, where the destructor does nothing
  [[maybe_unused]] uint32_t user_;
  [[maybe_unused]] uint32_t state_;
  [[maybe_unused]] uint64_t& result_;
};

// The XamInputGetCapabilities wrapper (wheel/wheel_fake.cpp), right after the SDK answered: while the
// "not connected" window above is open, user 0's capabilities are cleared and `r3` is 0x48F too (the
// game asks for both). Does nothing otherwise. `caps` is the guest address of the structure.
void PadLossCaps(uint8_t* base, uint32_t user, uint32_t caps, uint64_t& r3);

}  // namespace larecomp::input
