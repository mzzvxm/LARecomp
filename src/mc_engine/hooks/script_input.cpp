// Buttons that game scripts poll: Start on the title screen and the attract video, and every
// other place where a script asks "was this pressed in this update".
//
// A script asks one of four natives. Each answers yes for exactly one game update, the one in
// which the button came down:
//  - IS_PAD_BUTTON_DOWN (sub_822C8AF8) and IS_BUTTON_DOWN (sub_822C8AB0 -> sub_822C7FC8) read the
//    pad record at 0x82905388 + 64 * pad: the press is (old mask xor new mask) and new mask
//    (+12, +8). IS_BUTTON_DOWN answers no while a modal scene blocks input (sub_82207D30).
//  - UILogic_IsPressed (sub_822077F0) and Player_FlashingHighbeams (sub_822A7348 -> sub_822A6D48,
//    the high beams that challenge a racer) read an action record of the player's controls, 12
//    bytes at [controls + 12] + 12 * action, with sub_822A4CE0. Bit 7 of byte 0 and of byte 2
//    differ while the action is down; byte 3 is byte 2 of the update before (the input mapper,
//    sub_8260B990, copies it there and clears byte 2 at the start of each update), so the
//    answer is "down now, not down in the update before".
//
// A script thread does not run in every update (scrThread::Update adds a fixed step to the
// thread's clocks; its looks were 20 ms apart at 147 FPS and 33 ms at 60 FPS), so above 30 FPS it
// looks in some updates and skips the others. A press that comes in an update the thread skips is
// over before its next look, and the button has to be pressed again until one lands on a look
// (Start on the title screen; entering an event at a hangout is reported to need several presses
// too).
//
// Each hook sits where the native has the answer. For each place in a script that asks (the
// running thread and its program counter, with the pad or controls object and the button or
// action) it remembers whether the button was down at that place's previous look. A button that
// is down now, was up then and is not reported as pressed in this update came down between two
// looks: the script is told once that it was pressed. A place that starts asking with the button
// already down is told nothing (a Start still held from the previous screen is not a new press),
// nor is one that did not look for script_pad_latch_ms (the script was not polling). A native
// that refuses to answer (a modal scene blocks input; the high beams without a car) takes its
// place out of the table, so a press made while it was refused is not told later.
//
// A real press in an update the script looks in is answered by the game as before. Every script
// that polls with these natives gets the same treatment, and script_pad_latch_ms = 0 turns it off.

#ifndef REXGLUE_HAS_XEO3_TARGET
#include <rex/cvar.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <chrono>
#include <cstdint>
#include <mutex>

#include "hooks.h"
#include "hooks_internal.h"
#include "larecomp_log.h"

REXCVAR_DEFINE_INT32(script_pad_latch_ms, 100, "MCLA/Input",
    "A script that asks whether a button was pressed only every few updates (the title screen's "
    "Start above 30 FPS) misses a press that comes in an update it does not look in. It is told "
    "about that press, at its next look, if its previous look was at most this many milliseconds "
    "before. Without it the button has to be pressed several times. 0 = the game's own "
    "behaviour: a press is seen only by a script that looks in the update it happened in.")
    .range(0, 500)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

// scrThread::Run (sub_82554E20) stores the thread it runs here; the thread's +16 is the
// program counter of the opcode being run.
constexpr uint32_t kCurrentScriptThread = 0x828DA3BC;
constexpr uint32_t kScriptThreadPc = 16;

// The action record (see the header): 12 bytes at [controls + 12] + 12 * action.
// CHECK: read in the recompiled sub_822A4CE0, sub_822A4D48 ("released": not down now, down in the
// update before), sub_822A4DB0 (bit 7 of bytes 0 and 2 differ), sub_822A4DE8 (builds the records)
// and sub_8260B990 (rolls byte 2 into byte 3); no record has been looked at in a running game.
constexpr uint32_t kControlsRecords = 12;
constexpr uint32_t kRecordSize = 12;

enum Native { kPadButton, kPlayerButton, kUiPressed, kHighbeams, kNativeCount };
constexpr const char* kNativeName[kNativeCount] = {
    "IS_PAD_BUTTON_DOWN", "IS_BUTTON_DOWN", "UILogic_IsPressed", "Player_FlashingHighbeams"};
constexpr const char* kWhatName[kNativeCount] = {"button", "button", "action", "action"};

// The places that ask. The title's thread asks about eight buttons of four pads from one place
// in a loop, so the table holds a few threads' worth.
struct Look {
    uint32_t thread = 0;
    uint32_t pc = 0;
    uint32_t object = 0;  // the pad (the script's number or its record) or the controls object
    uint32_t what = 0;    // the button or the action
    bool down = false;
    int64_t at_ms = 0;  // 0 = unused
};
constexpr int kLooks = 128;

std::mutex g_mutex;
Look g_looks[kLooks];
int g_next_look = 0;
int g_told[kNativeCount];

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

// A script asks whether `what` of `object` was pressed in this update. `pressed` is the game's
// answer and `down` whether the button is held now. Returns true when the script is to be told
// that it was pressed.
bool ScriptLook(Native native, uint32_t object, uint32_t what, bool down, bool pressed) {
    const int32_t window_ms = REXCVAR_GET(script_pad_latch_ms);
    if (window_ms <= 0) return false;
    const uint8_t* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return false;
    const uint32_t thread = ReadGuestU32(base, kCurrentScriptThread);
    const uint32_t pc = thread ? ReadGuestU32(base, thread + kScriptThreadPc) : 0;
    const int64_t now = NowMs();

    std::lock_guard<std::mutex> lock(g_mutex);
    Look* look = nullptr;
    for (Look& l : g_looks) {
        if (l.at_ms != 0 && l.thread == thread && l.pc == pc && l.object == object &&
            l.what == what) {
            look = &l;
            break;
        }
    }
    if (!look) {
        // First look from this place: its baseline is the button as it is now.
        look = &g_looks[g_next_look];
        g_next_look = (g_next_look + 1) % kLooks;
        look->thread = thread;
        look->pc = pc;
        look->object = object;
        look->what = what;
        look->down = down;
        look->at_ms = now;
        return false;
    }
    bool tell = false;
    if (!pressed && down && !look->down && now - look->at_ms <= window_ms) {
        tell = true;
        if (g_told[native]++ < 4) {
            LARECOMP_APP_INFO("[ScriptPad] {}: {} {} came down between two looks of the script "
                              "({} ms apart): it is told the press",
                              kNativeName[native], kWhatName[native], what, now - look->at_ms);
        }
    }
    look->down = down;
    look->at_ms = now;
    return tell;
}

// The running script's place asked a native that refused to answer: it is not polling now.
void ScriptRefused() {
    if (REXCVAR_GET(script_pad_latch_ms) <= 0) return;
    const uint8_t* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;
    const uint32_t thread = ReadGuestU32(base, kCurrentScriptThread);
    const uint32_t pc = thread ? ReadGuestU32(base, thread + kScriptThreadPc) : 0;
    std::lock_guard<std::mutex> lock(g_mutex);
    for (Look& l : g_looks) {
        if (l.at_ms != 0 && l.thread == thread && l.pc == pc) l.at_ms = 0;
    }
}

// The controls object and the action of the record test that sub_822A4CE0 is about to make: the
// natives below call it with r3 = the controls object, r4 = the action, and answer with its
// result, but both are gone from the registers when it returns.
thread_local uint32_t t_controls = 0;
thread_local uint32_t t_action = 0;

void ActionAsked(PPCRegister& r3, PPCRegister& r4) {
    t_controls = r3.u32;
    t_action = r4.u32;
}

// After the record test: r3 = its answer (the hook may set it to 1).
void ActionAnswered(Native native, PPCRegister& r3) {
    const uint32_t controls = t_controls;
    const uint32_t action = t_action;
    t_controls = 0;
    if (!controls || REXCVAR_GET(script_pad_latch_ms) <= 0) return;
    const uint8_t* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;
    const uint32_t records = ReadGuestU32(base, controls + kControlsRecords);
    if (!records) return;
    const uint32_t record = records + kRecordSize * action;
    const bool down = ((base[record + 0] ^ base[record + 2]) & 0x80) != 0;
    if (ScriptLook(native, controls, action, down, (r3.u32 & 0xFF) != 0)) r3.u64 = 1;
}

}  // namespace

// 0x822C8B34 in sub_822C8AF8 (IS_PAD_BUTTON_DOWN), the `cntlzw r6,r8` that turns the
// press into the answer: r8 = the press (the button's bit of old ^ new, and new; nonzero
// = pressed in this update, and the hook may set it), r5 = the button, r3 = the pad's
// new button mask, r6 = the pad (the script's number).
void Hook_ScriptPadRead(PPCRegister& r8, PPCRegister& r5, PPCRegister& r3, PPCRegister& r6) {
    const uint32_t button = r5.u32;
    const bool down = button < 32 && ((r3.u32 >> button) & 1) != 0;
    if (ScriptLook(kPadButton, r6.u32, button, down, r8.u32 != 0)) r8.u64 = 1;
}

// 0x822C8030 in sub_822C7FC8 (IS_BUTTON_DOWN(player, button) = sub_822C8AB0 -> this), the
// same test after the modal-scene check, the `cntlzw r11,r3`: r3 = the press (nonzero = pressed;
// the hook may set it), r30 = the button, r7 = the pad's new button mask, r11 = the pad record.
void Hook_ScriptButtonRead(PPCRegister& r3, PPCRegister& r30, PPCRegister& r7, PPCRegister& r11) {
    const uint32_t button = r30.u32;
    const bool down = button < 32 && ((r7.u32 >> button) & 1) != 0;
    if (ScriptLook(kPlayerButton, r11.u32, button, down, r3.u32 != 0)) r3.u64 = 1;
}

// 0x822C7FF4 in sub_822C7FC8, the `li r3,0` taken while a modal scene blocks input.
void Hook_ScriptButtonRefused() {
    ScriptRefused();
}

// 0x8220782C in sub_822077F0 (UILogic_IsPressed(player, action)): the `bl sub_822A4CE0`;
// r3 = the controls object, r4 = the action. 0x82207830, the instruction after it: r3 = the
// answer (the hook may set it to 1).
void Hook_ScriptUiPressedAsk(PPCRegister& r3, PPCRegister& r4) {
    ActionAsked(r3, r4);
}

void Hook_ScriptUiPressedAnswer(PPCRegister& r3) {
    ActionAnswered(kUiPressed, r3);
}

// 0x822A6DC8 in sub_822A6D48 (Player_FlashingHighbeams(player) = sub_822A7348 -> this; the
// action is 9): the `bl sub_822A4CE0`, as above. 0x822A6DCC after it. 0x822A6DD4, the `li r3,0`
// that every refusal (no car, no racer to challenge) jumps to.
void Hook_ScriptHighbeamsAsk(PPCRegister& r3, PPCRegister& r4) {
    ActionAsked(r3, r4);
}

void Hook_ScriptHighbeamsAnswer(PPCRegister& r3) {
    ActionAnswered(kHighbeams, r3);
}

void Hook_ScriptHighbeamsRefused() {
    ScriptRefused();
}
#endif  // REXGLUE_HAS_XEO3_TARGET
