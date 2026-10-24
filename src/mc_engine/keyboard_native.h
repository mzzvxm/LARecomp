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
// while driving.
//
// ── What this does ──────────────────────────────────────────────────────
//
// 1. Feed (Hook_KeyboardFeed, in sub_82614C70 just before the buffer swap):
//    writes the host keyboard state into byte_82905738, the pending table the
//    game's own event loop would have filled.
// 2. Menus: the UI's mapper (FlashNavigator::fnInputHandler, built once with 24
//    pad-only entries by sub_8221D560) gets extra type-0 entries pointing at the
//    SAME output as the button they stand for. Enter is "A" and Esc is "B"
//    there, and nothing else: Start is simply never given a key, so Enter in the
//    pause menu is accept and cannot also be Start.
// 3. Driving: sub_822A4DE8 rebuilds the player's mapper from the control config
//    (the xenon_controls*.map tables) and clears it first, so the hook at its
//    return (Hook_ControlMapRebuilt) adds the keyboard entries after every
//    rebuild. Here Esc/Enter is PAUSE, Caps Lock the weight transfer (the "B" of
//    the pad), WASD gas/brake/steer. The pad entries are untouched.
//
// Menus and driving are different mappers, so the same key can mean different
// things in each without any context switching or rewriting cvars.
//
// ── Glyphs ──────────────────────────────────────────────────────────────
//
// button_prompts = keyboard keeps platform = 0 (a third strip would need 49 more
// frames in every movie) and instead paints a key cap over the pixels of the
// Xbox A/B/X/Y/LB/RB/LT/RT bitmaps in shared_latin.xtd. The movies carry
// separate copies of each glyph (prompt.xsf uses shared_49/41/1/48/37, pause and
// popup shared_2/10/12/43/18) so all of them are written. The originals are
// kept and put back when the mode changes.

#include <rex/ppc.h>

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
