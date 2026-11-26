// Render phase culling: perf_no_shadows clears phase bits in the renderer's
// enable mask, and the other perf_ switches go through the dev options. Also
// the guard for the impostor search that never ends, and the freeze watchdog
// that reports which phase a hang stopped in.

#ifndef REXGLUE_HAS_XEO3_TARGET
#include "settings/immortal.h"
#include "settings/larecomp_settings.h"
#include <rex/kernel/title_runtime.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

#include "hooks.h"
#include "hooks_internal.h"
#include "larecomp_log.h"
#include "larecomp_guest.h"

#if defined(LARECOMP_HAS_GPU)
#include "gpu/cut_request.h"
#include "gpu/env_hold_link.h"
#include "gpu/gpu.h"
#include "gpu/menu_world.h"
#include "gpu/view_hold_rules.h"
#include "gpu/world_clock.h"
#include "mc_engine/camera/menu_camera.h"
#endif

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
    auto* base = larecomp::guest::Membase();
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

#if defined(LARECOMP_HAS_GPU)
namespace {
namespace env = mcla::gpu::env_hold;
namespace hold = mcla::gpu::view_hold;
namespace world_clock = mcla::gpu::world_clock;

constexpr uint32_t kEnvPhases = 0x4u | 0x8u;
constexpr uint32_t kRenderBuffer = 0x827E5640u;

// Only the phase loop's thread reads or writes this state.
thread_local hold::Sample g_env_capture{};
thread_local uint32_t g_env_renderer = 0;
thread_local uint32_t g_env_marker = 0;
thread_local bool g_env_have_capture = false;

bool GuestPointer(uint32_t p) { return p >= 0x10000u && p < 0xE0000000u; }

uint32_t EnvWorld(const uint8_t* base) {
    const uint32_t pause = ReadGuestU32(base, 0x828CCEA0u);  // Game_IsPaused
    const bool paused = GuestPointer(pause) && base[pause + 4] != 0 && base[pause + 10] == 0;
    // sub_822C0320 tests the HUD manager's photo-mode byte.
    const uint32_t hud = ReadGuestU32(base, 0x8286D8D4u);
    const bool photo = GuestPointer(hud) && base[hud + 0x51380u] != 0;
    // mcCineScript::IsScenePlaying: a live script or an active director handover.
    const uint32_t cine = ReadGuestU32(base, 0x8286D8F0u);
    const uint32_t director = ReadGuestU32(base, 0x8286D8E8u);
    const uint32_t handover = GuestPointer(director) ? ReadGuestU32(base, director + 8) : 0;
    const bool cinematic = GuestPointer(cine) &&
        (base[cine + 288] != 0 || (GuestPointer(handover) && ReadGuestF32(base, handover + 120) >= 0.0f));
    const bool garage = mcla::gpu::menu_world::GarageLighting();
    return uint32_t(paused) | (uint32_t(photo) << 1) | (uint32_t(garage) << 2) |
           (uint32_t(cinematic) << 3);
}
}  // namespace
#endif

void ApplyEnvViewHold(PPCRegister& effective, const PPCRegister& renderer) {
#if defined(LARECOMP_HAS_GPU)
    g_env_marker = 0;
    auto* base = larecomp::guest::Membase();
    if (!base || !mcla::gpu::Enabled() || !GuestPointer(renderer.u32) ||
        (effective.u32 & kEnvPhases) != kEnvPhases) {
        g_env_have_capture = false;
        return;
    }
    const bool enabled = larecomp::settings::mcla_gpu_env_hold.Get();
    const bool ready = g_env_renderer == renderer.u32 &&
        env::g_ready_renderer.load(std::memory_order_acquire) == renderer.u32;
    g_env_renderer = renderer.u32;
    g_env_marker = env::kDrawn;
    if (!enabled) {
        g_env_have_capture = false;
        return;
    }
    hold::Sample now;
    const uint32_t buffer = ReadGuestU32(base, kRenderBuffer);
    if (buffer < 4) {
        const uint32_t viewport = ReadGuestU32(base, renderer.u32 + 180u + 4u * buffer);
        if (GuestPointer(viewport)) {
            // The main viewport's world matrix is at +64. Phase 4 reads its
            // position from row 3 and builds the paraboloid in fixed world axes.
            for (uint32_t i = 0; i < 3; ++i) {
                now.camera.up[i] = ReadGuestF32(base, viewport + 80u + 4u * i);
                now.camera.forward[i] = -ReadGuestF32(base, viewport + 96u + 4u * i);
                now.camera.position[i] = ReadGuestF32(base, viewport + 112u + 4u * i);
            }
        }
    }
    now.time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const bool sharp = env::Resolution(larecomp::settings::mcla_gpu_env_resolution.Get()) == "on";
    // A rig swap can be a cut even when both cameras point along the road.
    const uint32_t rig = uint32_t(CurrentRenderedCameraRig()) & 0xFFu;
    now.world = EnvWorld(base) | (uint32_t(sharp) << 4) | (rig << 8);
    now.clock_jumps = world_clock::HourJumps();
    now.weather = world_clock::Weather();
    now.brightness = env::g_brightness.load(std::memory_order_relaxed);
    // Keep refreshing while a scripted cut is in flight through the buffered
    // renderer. The anchor is still the only consumer of the cut request.
    now.cut = mcla::gpu::cut::Global().Pending();

    // CHECK: inspect both APIs on a fast turn and across tunnel/menu transitions.
    if (hold::Decide({}, g_env_capture, now, g_env_have_capture && ready) == hold::Decision::kHold) {
        effective.u64 &= ~uint64_t(kEnvPhases);
        g_env_marker = env::kHeld;
        return;
    }
    g_env_have_capture = hold::Positioned(now.camera);
    g_env_capture = now;
#endif
}

bool Patch_SkipHeldEnvironmentPhase(PPCRegister& phase) {
#if defined(LARECOMP_HAS_GPU)
    if ((phase.u32 & kEnvPhases) == 0 || g_env_marker == 0) return false;
    if (phase.u32 == 0x4u) {
        mcla::gpu::OnGuestSceneComposited(g_env_marker, g_env_renderer);
    }
    return g_env_marker == env::kHeld;
#else
    return false;
#endif
}

void FinishEnvViewPhase(const PPCRegister& phase) {
#if defined(LARECOMP_HAS_GPU)
    if (phase.u32 != 0x8u) return;
    if (g_env_marker == env::kDrawn) {
        mcla::gpu::OnGuestSceneComposited(env::kEnd, g_env_renderer);
    }
    g_env_marker = 0;
#endif
}

static uint32_t ShadowPhaseBits() {
    const std::string s = larecomp::cvar::GetFlagByName("perf_shadow_phase_bits");
    if (s.empty()) return kShadowPhaseBitsDefault;
    const uint32_t v = uint32_t(std::strtoul(s.c_str(), nullptr, 0));
    return v ? v : kShadowPhaseBitsDefault;
}

void ApplyRenderPhaseMask() {
    static bool was_applied = false;
    static uint32_t applied_bits = 0;
    static uint32_t stock_bits = 0;  // of applied_bits, which were set before we touched them
    static bool have_stock = false;

    const bool want = larecomp::cvar::GetFlagByName("perf_no_shadows") == "true";
    const uint32_t bits = ShadowPhaseBits();
    if (!want && !was_applied) return;

    auto* base = larecomp::guest::Membase();
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

struct FreezeWatchdogState {
    std::mutex mutex;
    bool stopped = false;
};

static FreezeWatchdogState& FreezeWatchdog() {
    static larecomp::Immortal<FreezeWatchdogState> state;
    return *state;
}

void StopFreezeWatchdog() {
    auto& watchdog = FreezeWatchdog();
    // Wait out any report already reading guest memory before shutdown can unload it.
    const std::lock_guard lock(watchdog.mutex);
    watchdog.stopped = true;
}

void StartFreezeWatchdog() {
    std::thread([] {
        auto& watchdog = FreezeWatchdog();
        uint64_t last = 0;
        int stalled = 0;
        bool reported = false;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            const std::lock_guard lock(watchdog.mutex);
            if (watchdog.stopped) return;
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

            auto* base = larecomp::guest::Membase();
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
                               larecomp::cvar::GetFlagByName("perf_no_shadows"),
                               larecomp::cvar::GetFlagByName("perf_shadow_phase_bits"),
                               larecomp::cvar::GetFlagByName("perf_no_trees"),
                               larecomp::cvar::GetFlagByName("perf_no_impostors"));
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

    auto* base = larecomp::guest::Membase();
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

void StopFreezeWatchdog() {}
bool Patch_ImpostorShadowGuard(PPCRegister& r3) { return false; }
bool Patch_SkipHeldEnvironmentPhase(PPCRegister&) { return false; }
#endif // REXGLUE_HAS_XEO3_TARGET
