#pragma once
//
// Native keyboard for the game's own input layer, and the keyboard glyphs the
// prompts show for it.
//
// ── What the game already has ───────────────────────────────────────────
//
// Every input source goes through rage's ioMapper (sub_8260B990): a list of
// {type, value, output} entries evaluated once a frame, each output keeping the
// largest value any of its sources produced. Type 6/8 read pad buttons, 9 pad
// axes, 1..5 the mouse, **0 a keyboard key** (value = scancode, read from
// byte_82905538[256 * buf + key]) and **14 a pair of keys that make one axis**
// (low byte = the key that pulls to 0, the next byte = the key that pulls to 0xFF,
// neither or both = centre 0x80).
//
// The table behind type 0 is filled by sub_82614C70, which asks
// XamInputGetKeystrokeEx(255, flags = 2) for key events and turns each VK into a
// DirectInput scancode through byte_82825050. The SDK answers that call with
// X_ERROR_DEVICE_NOT_CONNECTED (xam_input.cpp: the keyboard flag is not the
// gamepad flag), so the keyboard layer has always been empty. mnk_mode gets
// round it by pretending to be a pad, which is why a key there can only ever be
// a pad button and one key cannot mean "accept" in a menu and something else
// while driving. That mode (mnk_mode, mnk_mouse, keybind_*) is retired: LARecomp
// forces it off, and the native keyboard below is always on.
//
// ── What this does ──────────────────────────────────────────────────────
//
// 1. Feed (Hook_KeyboardFeed, in sub_82614C70 just before the buffer swap):
//    writes the keys into byte_82905738, the pending table the game's own event
//    loop would have filled. The keys come from the WINDOW'S EVENTS (key down,
//    key up, mouse button, wheel; a layer-0 listener on the UI thread fills a
//    larecomp::kbm::KeyLatch, kbm_input.h), never from the OS: no polling, the
//    same on Windows, Linux and the Steam Deck, and a key tapped between two
//    polls still reaches the game. The five mouse buttons and the two wheel
//    directions are keys too (LMB RMB MMB MB4 MB5 WheelUp WheelDown, scancodes
//    0x80..0x86), so kb_ui_* / kb_drive_* can name them and Options >
//    Controller > Customize can bind them. Mouse look is not here.
// 2. Menus: the UI's mapper (FlashNavigator::fnInputHandler, built once with 24
//    pad-only entries by sub_8221D560) gets extra type-0 entries pointing at the
//    SAME output as the button they stand for. Enter is "A" and Esc is "B"
//    there, and nothing else: Start is simply never given a key, so Enter in the
//    pause menu is accept and cannot also be Start.
// 3. Driving: sub_822A4DE8 rebuilds the player's mapper from the control config
//    (the xenon_controls*.map tables) and clears it first, so the hook at its
//    return (Hook_ControlMapRebuilt) adds the keyboard entries after every
//    rebuild. Here Esc/Enter is PAUSE, Ctrl the weight transfer (the "B" of
//    the pad), WASD gas/brake/steer. The pad entries are untouched. The steering keys are
//    not mapper entries any more: they ramp to full lock and back (kb_steer_rise, kb_steer_return)
//    and the XamInputGetState wrapper adds the value to the left stick (KeySteerScope below).
//
// Menus and driving are different mappers, so the same key can mean different
// things in each without any context switching or rewriting cvars.
//
// ── Glyphs ──────────────────────────────────────────────────────────────
//
// The prompts follow the device in use, live, on every screen (KBM-DESIGN.md P4; the pure part is prompt_glyphs.h). The movies' platform flag is pinned to Xbox (hooks/ui.cpp,
// prompts_live) and the glyph family in use (input_state.h: Xbox, PlayStation, Keyboard) is painted into the Xbox A/B/X/Y/LB/RB/LT/RT/Start/Back bitmaps of shared_latin.xtd and the
// Sidekick phone's embedded accept glyph: the originals for Xbox, the PlayStation set (the game's own PlayStation bitmaps, scaled up; the shoulders from the pictures shipped next to
// the key caps until the game's own are identified), or key caps (the menu's keys while a menu is open, the driving keys on the road). The movies carry separate copies of each
// glyph (prompt.xsf uses shared_49/41/1/48/37, pause and popup shared_2/10/12/43/18) so all of them are written. The originals are kept and put back for the Xbox set.
// A repaint happens on a change only (PollPrompts: the input state's listener sets a flag; a menu opening or closing with the keyboard in use, a binding, a new dictionary), on the
// game thread, from the input poll; the art is built once when the dictionary is found (WarmArt).

#include <cstddef>
#include <cstdint>

#include <rex/ppc/context.h>

#include "mc_engine/input/input_state.h"
#include "mc_engine/input/kbm_input.h"

// Before `lis r30` at 0x82614CFC: the end of the keystroke poll, reached on every
// path out of sub_82614C70, ahead of the memcpy that publishes the key table.
void Hook_KeyboardFeed();

// At the return of sub_822A4DE8 (0x822A4EB4); r30 is the controller struct
// ({+4 player, +8 mapper, +12 outputs}) the function was rebuilding.
void Hook_ControlMapRebuilt(PPCRegister& r30);

// At loc_82615048 in sub_82614D30 (the pad's button bits are final, r31 = the pad):
// until the game is entered, ORs the menu keys into pad 0's bits for the screens that
// read the raw pad instead of the UI mapper (the title's "PRESS START").
void Hook_PadBitsFinalize(PPCRegister& r31);

// After the prologue of sub_82727010 (the "press a button" scan of Options > Controller >
// Customize, 0x82727044): lets a key that just went down answer the capture. r28 = out type
// pointer; r11 becomes the value stored at the out value pointer. Returning true jumps to the
// function's own "return 1" tail (0x8272737C).
bool Hook_ControlCaptureKey(PPCRegister& r11, PPCRegister& r28);

// Entry of sub_82679678 (the Custom Controls row label of a binding: type, value, out isIcon):
// a keyboard binding gets a key cap glyph, or its name. Returning true jumps to the
// function own `blr` at 0x82679BD4 with r3 = the label string.
bool Hook_ControlLabel(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5);

// ── What mouse look and the steering ramp need from the keyboard layer (KBM-DESIGN.md phase P5) ─────────────────────────────────────
namespace larecomp::keyboard {

// The screen the game is on, as the last input poll saw it (the same facts that choose between the menu's and the road's key caps, prompt_glyphs.h UiSignals). Any thread.
// MenuScreenUp: a menu screen is up (the pause menu, the garage, a pop-up, the full map's movie, ...) or the game is not entered yet (the title, the sign-in steps).
// PhoneScreenUp: the Sidekick phone is the current movie (it is the road's, not a menu: the driving keys work with it up).
bool MenuScreenUp();
bool PhoneScreenUp();

// The game proper has started (the player's control map has been built: the title and the sign-in steps are behind). Any thread.
bool GameEntered();

// The name of the movie the game's UI has as its current one, as of the last input poll ("PAUSEMOVIE", "GARAGEMOVIE", "POPUPMOVIE", ... ; empty for none), copied into `out` (at most `capacity`
// bytes with the end mark). Any thread.
void CurrentMovieName(char* out, size_t capacity);

// Presses one of the menu's buttons for one input poll, as if a key mapped to it went down: the mouse in the menus (menu_mouse.cpp) does its clicks and wheel steps with this.
// The button goes in through the game's own menu mapper (EnsureNavKeys binds an always-live entry per button), so it is exactly the pad button's press. Two calls before the same
// poll are one press, and presses in consecutive polls merge into one held press: leave a poll between them (menu_mouse_core.h PressPacer). Any thread.
void PressMenuKey(larecomp::kbm::MenuKey key);

// What the settings say about a key that just went down, for the active-device rules (input_state.h KeyRole, KBM-DESIGN.md 2.2): ExtraButton for a host key of an
// extra_button_* setting (the pad's paddles: the key is a pad button, the keyboard feed does not pass it on as a key), Bound for a key that a kb_ui_* / kb_drive_* binding
// names (the only way an F1-F12 key counts as keyboard activity), else Plain. The answer follows the settings: the lists are read again when a binding changed (the same
// generation number the mappers use), and not otherwise. Any thread; the window's key-down handler asks.
larecomp::input::KeyRole RoleOfKey(uint16_t vk);

// Keyboard steering with a ramp (kb_steer_ramp, kb_steer_rise, kb_steer_return; mouse_look_core.h StepSteer): the input poll works out the ramped value of the steering
// keys (left and right of kb_drive_steer_*, any key of a list counts); this object, on the stack of the XamInputGetState wrapper (wheel/wheel_fake.cpp), puts it into the left
// stick's X axis of the state the game is about to read when the wrapper returns (the larger magnitude of the key and the stick wins; a wheel, a failed call, user other than 0
// and a ramp at rest leave the state alone). It must be constructed BEFORE input_state.h's InputActivityScope, so that it runs after it: the ramp is the keyboard, and the
// device state machine must not count it as a controller's stick.
class KeySteerScope {
 public:
  KeySteerScope(uint8_t* base, uint32_t user, uint32_t state, uint64_t& result) : base_(base), user_(user), state_(state), result_(result) {}
  ~KeySteerScope();
  KeySteerScope(const KeySteerScope&) = delete;
  KeySteerScope& operator=(const KeySteerScope&) = delete;

 private:
  [[maybe_unused]] uint8_t* base_;
  [[maybe_unused]] uint32_t user_;
  [[maybe_unused]] uint32_t state_;
  [[maybe_unused]] uint64_t& result_;
};

}  // namespace larecomp::keyboard
