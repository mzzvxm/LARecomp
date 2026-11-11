// Render phase culling: perf_no_shadows clears phase bits in the renderer's
// enable mask, and the other perf_ switches go through the dev options. Also
// the guard for the impostor search that never ends, and the freeze watchdog
// that reports which phase a hang stopped in.

#ifndef REXGLUE_HAS_XEO3_TARGET
#include <rex/cvar.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <thread>

#include "hooks.h"
#include "hooks_internal.h"
#include "larecomp_log.h"

// ── Render phase culling ────────────────────────────────────────────────────
// These reactivate the game's own dev command-line switches (see
// dev_options.cpp). They are not new code paths: the retail renderer still
// contains every branch, only the switch that reaches it was stripped.
//
// perf_no_shadows is the big one. sub_822E47E0 (the renderer ctor) reacts to it
// with `phase_mask &= 0xFFFF9E1F`, clearing render phase bits 0x20 0x40 0x80
// 0x100 0x2000 0x4000 in the enable mask at renderer+448. The phase loop in
// sub_822E6408 only dispatches a phase's draw lists when its bit is set, so
// those passes stop existing entirely: no shadow-map traversal, no
// shadowDepth/shadowAlphaDepth/shadowBlend technique draws, no shadow render
// targets. Restart-only because the mask is computed once, at renderer init.
REXCVAR_DEFINE_BOOL(perf_no_shadows, false, "MCLA/Performance",
    "Drop every real-time shadow render phase. Largest single framerate win; the world "
    "loses cast shadows. Applied straight to the renderer's phase enable mask, so it "
    "takes effect immediately.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// Which render phase bits perf_no_shadows clears.
//
// The game's own `noshadows` clears 0x61E0 (0x20 0x40 0x80 0x100 0x2000 0x4000)
// — the sun cascade phases. Measured with the clock held at midnight, none of
// those appear in renderer+364 at all, so clearing them changes nothing. The
// shadow that is actually drawn comes from phase 0x400: sub_823120C8's case 4
// runs when `(mask & 0x20)` OR `((mask & 0x400) && night)`, and sub_823112C0
// routes 0x400 to shadowNight / shadowFastBlend. `noshadows` never touches it.
//
// Default is therefore 0x65E0 = the stock set plus 0x400, so one setting covers
// both the day (cascade) and night (blend) path.
//   0x61E0  stock noshadows — sun cascades only
//   0x400   the night/blend shadow phase only
//   0x65E0  both
//   0x7DF0  everything sub_823120C8 dispatches on; also takes 0x10/0x200/0x800,
//           which are unrelated live passes, so it corrupts the frame
static constexpr uint32_t kShadowPhaseBitsDefault = 0x65E0u;

REXCVAR_DEFINE_STRING(perf_shadow_phase_bits, "0x65E0", "MCLA/Performance",
    "Render phase bits perf_no_shadows clears in renderer+448 (hex). 0x65E0 = sun cascades "
    "(0x61E0, what the game's own 'noshadows' clears) plus the night/blend shadow phase "
    "0x400 that it misses. 0x400 alone isolates the night path.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(perf_no_race_shadows, false, "MCLA/Performance",
    "Drop the shadow pass during races only (dev switch 'noraceshadows').")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(perf_fast_vehicle_shadows, false, "MCLA/Performance",
    "Cheap blob shadow under vehicles instead of the real-time one (dev switch "
    "'fastVehShadows'). Use instead of perf_no_shadows to keep world shadows. Applies at the "
    "next start of the game.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

// Load-time, not per-frame: sub_8230B778 skips allocating the ImpostorDepth /
// ShadowImpostor / ImpostorColor / ImpostorNormal render targets. node+4 is kept
// in sync with this cvar every frame, but the game makes these targets once, with
// the world, at start-up (the fork logged one world setup and 53 impostor setups
// at start-up and none when a race started or was quit: sub_82305230 ->
// sub_8230C388 -> sub_8230B778), so the change is seen at the next start. This
// said "the next load of that system" before, and the pause row "(ON RELOAD)".
// CHECK: toggled in the pause menu, then a district change: the impostors do not
// change until the game is started again.
REXCVAR_DEFINE_BOOL(perf_no_impostors, false, "MCLA/Performance",
    "Do not allocate the foliage impostor render targets (dev switch 'noimpostors'). "
    "Distant trees lose their billboards. Applies at the next start of the game.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

// Same shape: sub_82310478 skips the prop parse for $/city/<district> entirely.
// CHECK: when that parse runs. If it is part of the world's set-up at start-up, like the
// impostor targets, this too is seen only at the next start (tag, description and the pause
// row's "(ON RELOAD)" would then say restart).
REXCVAR_DEFINE_BOOL(perf_no_trees, false, "MCLA/Performance",
    "Skip loading the prop/foliage set (dev switch 'notrees'). Last resort. Applies on "
    "the next district load.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// Read once by the game, when it starts (the fork: "the options the game reads once, at
// start-up": FULLSCREEN BLUR is one of them), and the owner saw it do nothing when flipped
// during play. The tag and the pause row's "(RESTART)" stay.
// CHECK: that a flip is not seen at a district load either (an earlier survey read the log's
// "(applies on next load of that system)" as "applies at the next load").
// Not a candidate for a live switch through the phase mask (ApplyRenderPhaseMask): the fork's
// frame-structure survey puts the 0x2 phase on the 256x256 reflection view, and the blur passes
// (motion blur, the bloom blurs) are inside the post phase (0x08000000) with the bright pass and the
// luminance chain, so no phase bit drops a blur alone.
// CHECK: which pass nofsblur skips (not found in the code; the survey is the fork's, not re-read).
REXCVAR_DEFINE_BOOL(perf_no_fullscreen_blur, false, "MCLA/Performance",
    "Skip the full-screen blur pass (dev switch 'nofsblur'). Applies at the next start of "
    "the game.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

// Bumped once per frame from Patch_DeltaTimePre; read by the freeze watchdog.
std::atomic<uint64_t> g_frame_heartbeat{0};

// 0x823126A4: `bl sub_8230D988` inside the sun-cascade shadow phase (phase bit
// 0x20) of sub_823120C8, with r3 already loaded from dword_8288D0A0.
//
// sub_8230D988 picks the next impostor to refresh with an unbounded round-robin
// search:
//
//   do { do { v6 = (v6 + 1) % count; } while (!entry[v6].f168); } while (!entry[v6].f196);
//
// Neither loop has an iteration bound, and the caller only checks that the array
// exists and count != 0 — not that any entry qualifies. With notrees or
// noimpostors on, the array is allocated with a non-zero count but nothing is
// ever filled in, so the search spins forever the first time the phase runs.
// That only happens in daylight, since the call sits behind `mask & 0x20`.
//
// Returning true jumps to 0x823126A8, which is the exact target of the game's
// own `beq` at 0x8231268C for the null-manager case — not a made-up exit.
//
// Gated on the array's real contents rather than on the cvars, so it covers any
// other way of ending up with an empty impostor pool.
bool Patch_ImpostorShadowGuard(PPCRegister& r3) {
    auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return false;

    const uint32_t mgr = static_cast<uint32_t>(r3.u64);
    if (mgr == 0) return true;

    const uint32_t arr = ReadGuestU32(base, mgr + 16);
    if (arr == 0) return true;

    const uint32_t count = (uint32_t(base[arr + 12]) << 8) | uint32_t(base[arr + 13]);
    const uint32_t entries = ReadGuestU32(base, arr + 8);
    if (count == 0 || entries == 0) return true;

    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t e = entries + i * 224u;
        if (ReadGuestU32(base, e + 168) && ReadGuestU32(base, e + 196)) return false;
    }

    static bool logged = false;
    if (!logged) {
        logged = true;
        LARECOMP_APP_INFO(
            "[Impostor] no refreshable entry in {} slot(s) — skipping sub_8230D988, which "
            "would search for one forever", count);
    }
    return true;
}

// ── Render phase enable mask ────────────────────────────────────────────────
//
// perf_no_shadows reproduces what the game's own `noshadows` switch does inside
// the renderer constructor sub_822E47E0:
//
//   822e49c8  lwz    r11, 0x1C0(r31)
//   822e49cc  rlwinm r10, r11, 0,27,22   ; clears 0x20 0x40 0x80 0x100
//   822e49d0  rlwinm r10, r10, 0,19,16   ; clears 0x2000 0x4000
//   822e49d4  stw    r10, 0x1C0(r31)
//
// Doing it here instead of through the dev switch means it does not depend on
// hitting the one instant between the node being registered and the constructor
// reading it, and it applies without a restart. sub_822E6408 re-reads
// renderer+448 every frame, and the only other writers are that constructor and
// the runtime setter at 0x822E51F8, so re-asserting per frame is idempotent and
// heals itself if the game turns the phases back on.
static constexpr uint32_t kRendererPtr = 0x8287E064u;
static constexpr uint32_t kPhaseEnableMask = 448u;   // enable mask, written by the ctor
static constexpr uint32_t kPhaseRequested = 364u;    // phases asked for this frame
static constexpr uint32_t kPhaseEffective = 360u;    // requested & enable, what consumers read

static uint32_t ShadowPhaseBits() {
    const std::string s = rex::cvar::GetFlagByName("perf_shadow_phase_bits");
    if (s.empty()) return kShadowPhaseBitsDefault;
    const uint32_t v = uint32_t(std::strtoul(s.c_str(), nullptr, 0));
    return v ? v : kShadowPhaseBitsDefault;
}

void ApplyRenderPhaseMask() {
    static bool was_applied = false;
    static uint32_t applied_bits = 0;
    static uint32_t stock_bits = 0;  // of applied_bits, which were set before we touched them
    static bool have_stock = false;

    const bool want = rex::cvar::GetFlagByName("perf_no_shadows") == "true";
    const uint32_t bits = ShadowPhaseBits();
    if (!want && !was_applied) return;

    auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;
    const uint32_t renderer = ReadGuestU32(base, kRendererPtr);
    if (renderer == 0) return;

    const uint32_t cur = ReadGuestU32(base, renderer + kPhaseEnableMask);

    // Turning off, or the bit set changed under us: put back what we took before
    // taking anything else, so the restore is never wider than the capture.
    if (was_applied && (!want || bits != applied_bits)) {
        const uint32_t restored = (cur & ~applied_bits) | stock_bits;
        if (restored != cur) WriteGuestU32(base, renderer + kPhaseEnableMask, restored);
        LARECOMP_APP_INFO("[PhaseMask] restored 0x{:08X}: renderer+448 0x{:08X} -> 0x{:08X}",
                          applied_bits, cur, restored);
        was_applied = false;
        have_stock = false;
        if (!want) return;
    }

    const uint32_t now = ReadGuestU32(base, renderer + kPhaseEnableMask);
    if (!have_stock) {
        stock_bits = now & bits;
        applied_bits = bits;
        have_stock = true;
    }

    const uint32_t next = now & ~bits;
    if (next != now) {
        WriteGuestU32(base, renderer + kPhaseEnableMask, next);
        if (!was_applied) {
            LARECOMP_APP_INFO("[PhaseMask] clearing 0x{:08X}: renderer+448 0x{:08X} -> 0x{:08X}",
                              bits, now, next);
        }
    }
    was_applied = true;
}

// ── Freeze watchdog ─────────────────────────────────────────────────────────
//
// A hang leaves nothing in the log: no exception, no crash handler, just the
// last line before it stopped. This bumps a counter every frame and a host
// thread reports the guest's render state when the counter stalls, so the next
// freeze says which render phase it died in instead of nothing at all.
//
// renderer+356 is the phase bit sub_822E6408 was on when it stopped advancing.
static constexpr uint32_t kPhaseCurrent = 356u;

void StartFreezeWatchdog() {
    std::thread([] {
        uint64_t last = 0;
        int stalled = 0;
        bool reported = false;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            const uint64_t now = g_frame_heartbeat.load(std::memory_order_relaxed);
            if (now != last) {
                last = now;
                stalled = 0;
                reported = false;
                continue;
            }
            if (now == 0) continue;  // not running yet
            if (++stalled < 5 || reported) continue;
            reported = true;

            auto* base = rex::Runtime::instance()->virtual_membase();
            if (!base) {
                LARECOMP_APP_ERROR("[Watchdog] frame stalled {}s, no guest membase", stalled);
                continue;
            }
            const uint32_t renderer = ReadGuestU32(base, kRendererPtr);
            if (renderer == 0) {
                LARECOMP_APP_ERROR("[Watchdog] frame stalled {}s, renderer null", stalled);
                continue;
            }
            LARECOMP_APP_ERROR(
                "[Watchdog] frame stalled {}s at phase 0x{:08X} | enable=0x{:08X} "
                "requested=0x{:08X} effective=0x{:08X}",
                stalled, ReadGuestU32(base, renderer + kPhaseCurrent),
                ReadGuestU32(base, renderer + kPhaseEnableMask),
                ReadGuestU32(base, renderer + kPhaseRequested),
                ReadGuestU32(base, renderer + kPhaseEffective));
            LARECOMP_APP_ERROR("[Watchdog] perf_no_shadows={} bits={} no_trees={} no_impostors={}",
                               rex::cvar::GetFlagByName("perf_no_shadows"),
                               rex::cvar::GetFlagByName("perf_shadow_phase_bits"),
                               rex::cvar::GetFlagByName("perf_no_trees"),
                               rex::cvar::GetFlagByName("perf_no_impostors"));
        }
    }).detach();
}

// One-shot probe for the render phase switches. Prints, the first frame the
// renderer exists, whether our dev-option writes survived to that point and what
// the phase enable mask actually ended up as.
//
//   node+4 != 0            -> sub_822E47E0 saw the switch
//   mask & 0x61E0 == 0     -> noshadows took (bits 20/40/80/100/2000/4000)
//
// renderer = dword_8287E064, +448 enable mask, +364 requested, +360 effective.
void LogRenderPhaseMaskOnce() {
    // The one-shot version read renderer+360 before the first RenderPhases call
    // and got 0xCDCDCDCD (uninitialised heap fill), which said nothing. Sample a
    // handful of real frames instead: +364 is what the frame asked for and +360
    // is what actually reached the consumers.
    static int samples = 0;
    static int frame = 0;
    static uint32_t last_effective = 0xFFFFFFFFu;
    if (samples >= 8) return;
    if (++frame % 60) return;

    auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;
    const uint32_t renderer = ReadGuestU32(base, kRendererPtr);
    if (renderer == 0) return;

    const uint32_t effective = ReadGuestU32(base, renderer + kPhaseEffective);
    if (effective == last_effective) return;
    last_effective = effective;
    ++samples;

    const uint32_t bits = ShadowPhaseBits();
    LARECOMP_APP_INFO(
        "[PhaseMask] enable=0x{:08X} requested=0x{:08X} effective=0x{:08X} | 0x{:X} in effective: "
        "0x{:X}",
        ReadGuestU32(base, renderer + kPhaseEnableMask),
        ReadGuestU32(base, renderer + kPhaseRequested), effective, bits, effective & bits);

    if (samples == 1) {
        LARECOMP_APP_INFO(
            "[PhaseMask] node+4: noimpostors(0x8288D0DC)=0x{:08X} notrees(0x8288D0F0)=0x{:08X} "
            "nofsblur(0x8288BA84)=0x{:08X}",
            ReadGuestU32(base, 0x8288D0DCu), ReadGuestU32(base, 0x8288D0F0u),
            ReadGuestU32(base, 0x8288BA84u));
    }
}
#else // REXGLUE_HAS_XEO3_TARGET
// XEO3 stubs: empty implementations so the linker resolves codegen calls.

#include <rex/ppc/context.h>

bool Patch_ImpostorShadowGuard(PPCRegister& r3) { return false; }
#endif // REXGLUE_HAS_XEO3_TARGET
