// Diagnostic for the mouse drag off the road (mouse_look.h MenuDragAxes): which of the game's raw right-stick readers run on the
// current screen, and what the game's own pad record holds while the mouse drags. Off by default (mouse_drag_diag).
//
// The readers are the callers of sub_8221CBA0 / sub_8221CBF8, which turn byte +16 / +17 of a pad record (unk_82905388 + 64 * pad)
// into -1 .. 1. mcPlayerCamera::Update (sub_822B2AB0) is one of them too; camera_trace.cpp already wraps it, so it is left out here.
// Once a second while the switch is on: how often each reader ran, bytes 16..19 and the type word (+52) of the four pad records,
// the drag's own axes, and the two flags sub_82251FD0 tests before it reads the stick (r3+54320 must be 0, r3+54609 non-zero).

#ifndef REXGLUE_HAS_XEO3_TARGET

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>

#include <rex/cvar.h>
#include <rex/ppc.h>
#include <rex/runtime.h>

#include "mc_engine/core/logging.h"
#include "mc_engine/hooks/hooks_internal.h"
#include "mouse_look.h"

REXCVAR_DEFINE_BOOL(mouse_drag_diag, false, "MCLA/Diagnostics",
    "Log, once a second, which of the game's raw right-stick readers ran and what the game's pad records hold "
    "(for the mouse drag in the garage and the menus).")
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REX_EXTERN(__imp__rex_sub_82251FD0);
REX_EXTERN(__imp__rex_sub_8262A688);
REX_EXTERN(__imp__rex_sub_82641F10);
REX_EXTERN(__imp__rex_sub_826AB520);
REX_EXTERN(__imp__rex_sub_8271FAC8);
REX_EXTERN(__imp__rex_sub_82727010);
REX_EXTERN(__imp__rex_sub_8260B990);

namespace {

constexpr uint32_t kPadRecords = 0x82905388;  // 64 bytes per pad
constexpr int kReaders = 7;
const char* const kReaderNames[kReaders] = {"82251FD0", "8262A688", "82641F10", "826AB520", "8271FAC8", "82727010", "8260B990"};
std::atomic<uint32_t> g_calls[kReaders];
std::atomic<uint32_t> g_garage_obj{0};

uint32_t Be32(const uint8_t* base, uint32_t ea) {
    const uint8_t* p = base + ea;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

void Report(const uint8_t* base) {
    using Clock = std::chrono::steady_clock;
    static Clock::time_point last = Clock::now();
    const Clock::time_point now = Clock::now();
    if (now - last < std::chrono::seconds(1)) return;
    last = now;
    uint32_t calls[kReaders];
    for (int i = 0; i < kReaders; ++i) calls[i] = g_calls[i].exchange(0, std::memory_order_relaxed);
    int16_t rx = 0, ry = 0;
    const bool drag = larecomp::mouselook::MenuDragAxes(rx, ry);
    char pads[200];
    int n = 0;
    for (uint32_t pad = 0; pad < 4; ++pad) {
        const uint8_t* r = base + kPadRecords + 64 * pad;
        n += std::snprintf(pads + n, sizeof(pads) - n, " p%u[%3u %3u %3u %3u t%u]", pad, r[16], r[17], r[18], r[19],
                           Be32(base, kPadRecords + 64 * pad + 52));
    }
    const uint32_t g = g_garage_obj.load(std::memory_order_relaxed);
    const int f1 = g ? base[g + 54320] : -1, f2 = g ? base[g + 54609] : -1;
    MC_INFO("[drag-diag] drag={} rx={} ry={} |{}| calls {}={} {}={} {}={} {}={} {}={} {}={} {}={} | 82251FD0 obj=0x{:08X} +54320={} +54609={}",
            int(drag), rx, ry, pads, kReaderNames[0], calls[0], kReaderNames[1], calls[1], kReaderNames[2], calls[2], kReaderNames[3],
            calls[3], kReaderNames[4], calls[4], kReaderNames[5], calls[5], kReaderNames[6], calls[6], g, f1, f2);
}

inline bool On() { return REXCVAR_GET(mouse_drag_diag); }

}  // namespace

// From mouse_look.cpp's Tick, every frame: the report comes even on a screen where none of the readers runs.
namespace larecomp::mouselook {
void MenuDragDiagTick() {
    if (!On()) return;
    auto* rt = rex::Runtime::instance();
    const uint8_t* base = rt ? reinterpret_cast<const uint8_t*>(rt->virtual_membase()) : nullptr;
    if (base) Report(base);
}
}  // namespace larecomp::mouselook

#define DRAG_DIAG_WRAP(addr, index)                                  \
    extern "C" REX_FUNC(rex_sub_##addr) {                            \
        if (On()) {                                                  \
            g_calls[index].fetch_add(1, std::memory_order_relaxed);  \
            if (index == 0) g_garage_obj.store(ctx.r3.u32, std::memory_order_relaxed); \
            Report(base);                                            \
        }                                                            \
        __imp__rex_sub_##addr(ctx, base);                            \
    }

DRAG_DIAG_WRAP(82251FD0, 0)
DRAG_DIAG_WRAP(8262A688, 1)
DRAG_DIAG_WRAP(82641F10, 2)
DRAG_DIAG_WRAP(826AB520, 3)
DRAG_DIAG_WRAP(8271FAC8, 4)
DRAG_DIAG_WRAP(82727010, 5)
DRAG_DIAG_WRAP(8260B990, 6)

#undef DRAG_DIAG_WRAP

#endif  // REXGLUE_HAS_XEO3_TARGET
