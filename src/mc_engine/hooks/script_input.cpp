// Buttons that game scripts poll: Start on the title screen and the attract video.
//
// Those two screens are not driven by the menu key path but by a script thread. It
// asks the native IS_PAD_BUTTON_DOWN (sub_822C8AF8) whether a button was "pressed in
// this update": the native compares the pad's new button mask (pad + 8) with the
// previous update's (pad + 12) and answers yes for exactly one update. The thread does
// not run in every update (scrThread::Update adds a fixed step to the thread's clocks; its
// looks were 20 ms apart at 147 FPS and 33 ms at 60 FPS), so above 30 FPS it looks in some
// updates and skips the others. A press that comes in an update the thread skips is over before its next look,
// and Start has to be pressed again until one lands on a look.
//
// The hook sits in the native, at the cntlzw that turns the press into the answer. For
// each place in a script that asks (the running thread and its program counter, with the
// button) it remembers whether the button was down at that place's previous look. A
// button that is down now, was up then and is not reported as pressed in this update came
// down between two looks: the script is told once that it was pressed. A place that starts
// asking with the button already down is told nothing (a Start still held from the
// previous screen is not a new press), nor is one that did not look for script_pad_latch_ms
// (the script was not polling).
//
// A real press in an update the script looks in is answered by the game as before. Only
// the title and attract screens were watched; every script that polls a pad button with
// this native gets the same treatment, and script_pad_latch_ms = 0 turns it off.

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
    "A script that polls a pad button only every few updates (the title screen's Start above "
    "30 FPS) is told about a press it missed, at its next look, if its previous look was at most "
    "this many milliseconds before. Without it Start has to be pressed several times on the "
    "title screen and the attract video. 0 = the game's own behaviour: a press is seen only by a "
    "script that looks in the update it happened in.")
    .range(0, 500)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

// scrThread::Run (sub_82554E20) stores the thread it runs here; the thread's +16 is the
// program counter of the opcode being run.
constexpr uint32_t kCurrentScriptThread = 0x828DA3BC;
constexpr uint32_t kScriptThreadPc = 16;

// The places that ask about a button. The title's thread asks about eight buttons from
// one place in a loop, so the table holds a few threads' worth.
struct Look {
    uint32_t thread = 0;
    uint32_t pc = 0;
    uint32_t button = 0;
    bool down = false;
    int64_t at_ms = 0;  // 0 = unused
};
constexpr int kLooks = 64;

std::mutex g_mutex;
Look g_looks[kLooks];
int g_next_look = 0;
int g_told = 0;

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace

// 0x822C8B34 in sub_822C8AF8 (IS_PAD_BUTTON_DOWN), the `cntlzw r6,r8` that turns the
// press into the answer: r8 = the press (the button's bit of old ^ new, and new; nonzero
// = pressed in this update, and the hook may set it), r5 = the button, r3 = the pad's
// new button mask.
void Hook_ScriptPadRead(PPCRegister& r8, PPCRegister& r5, PPCRegister& r3) {
    const int32_t window_ms = REXCVAR_GET(script_pad_latch_ms);
    if (window_ms <= 0) return;
    const uint8_t* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;
    const uint32_t thread = ReadGuestU32(base, kCurrentScriptThread);
    const uint32_t pc = thread ? ReadGuestU32(base, thread + kScriptThreadPc) : 0;
    const uint32_t button = r5.u32;
    const bool down = button < 32 && ((r3.u32 >> button) & 1) != 0;
    const int64_t now = NowMs();

    std::lock_guard<std::mutex> lock(g_mutex);
    Look* look = nullptr;
    for (Look& l : g_looks) {
        if (l.at_ms != 0 && l.thread == thread && l.pc == pc && l.button == button) {
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
        look->button = button;
        look->down = down;
        look->at_ms = now;
        return;
    }
    if (r8.u32 == 0 && down && !look->down && now - look->at_ms <= window_ms) {
        r8.u64 = 1;
        if (g_told++ < 4) {
            LARECOMP_APP_INFO("[ScriptPad] button {} came down between two looks of the script "
                              "({} ms apart): it is told the press", button, now - look->at_ms);
        }
    }
    look->down = down;
    look->at_ms = now;
}
#endif  // REXGLUE_HAS_XEO3_TARGET
