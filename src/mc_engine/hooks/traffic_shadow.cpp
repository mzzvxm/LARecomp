// Traffic and AI cars cast into the sun's shadow cascades (the TRAFFIC SHADOWS setting).
//
// The story, the guest facts and the rule that picks the cars are in gpu/traffic_shadow.h (pure, with its
// own check). This file is the part that touches the guest: the hook at 0x8236066C in Vehicle::Draw
// (sub_82360518) and the settings.
//
//   0x82360668  lbz   r11,594(r30)       r30 = the vehicle: the byte the constructor sets for the player's car only
//   0x8236066C  cmplwi cr6,r11,0         <- the hook runs before this one; r11 = 1 makes the game draw the car
//   0x82360670  beq   cr6,0x82360688        into the current phase (sub_8235FA90) instead of skipping it
//
// This place is reached only in the render phases 0x20 0x40 0x80 0x100 (the four sun cascades) and 0x1000 (the
// shadow collector) for a vehicle whose +593 is set (the branch at 0x82360664 skips the rest otherwise). The
// hook acts in the four cascades only: the collector reads the atlas the cascades make, and a car that is
// not in it keeps the look it has (it receives nothing, as before).
//
// Registers: r11 = +594 (the hook raises it), r13 = the thread's guest context (which copy of the entity's
// matrix the thread reads), r28 = the phase, r30 = the vehicle.

#ifndef REXGLUE_HAS_XEO3_TARGET
#include <rex/cvar.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>

#include "hooks.h"
#include "hooks_internal.h"
#include "larecomp_log.h"
#if defined(LARECOMP_HAS_GPU)
#include "gpu/gpu_class.h"
#include "gpu/live_text.h"
#include "gpu/traffic_shadow.h"
#endif

REXCVAR_DEFINE_STRING(
    mcla_gpu_traffic_shadows, "auto", "MCLA/GPU",
    "Traffic and the other AI cars cast into the sun's shadow map like the player's car, so the "
    "ground under them is dark (the game only draws the player's car into it). off: the game's own "
    "picture. near: the nearest few cars within a short distance of the camera. all: every car "
    "within a long distance (costs more graphics and processor time). auto (the default): near on a "
    "discrete graphics card, off on integrated graphics. Daylight only (the game has no sun shadow "
    "at night and in rain). Live: it applies to the next frame, to the cars already on the road.")
    .allowed({"auto", "off", "near", "all"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(
    mcla_gpu_traffic_shadows_radius, 0.0, "MCLA/GPU",
    "Diagnostic: how far from the camera a car may be and still cast into the sun shadow map, in "
    "metres. 0 is the setting's own (near 40, all 150). Live.")
    .debug_only()
    .range(0.0, 1000.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(
    mcla_gpu_traffic_shadows_cars, 0, "MCLA/GPU",
    "Diagnostic: the most cars that cast into the sun shadow map at once (the nearest ones). 0 is "
    "the setting's own (near 4, all 16); at most 24. Live.")
    .debug_only()
    .range(0, 24)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(
    mcla_gpu_traffic_shadows_cascades, 0, "MCLA/GPU",
    "Diagnostic: the cars are drawn into the first this many of the four sun shadow cascades "
    "(each one costs the cars' draws again). 0 or 4 is all four. Live.")
    .debug_only()
    .range(0, 4)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

#if defined(LARECOMP_HAS_GPU)
namespace {

namespace ts = mcla::gpu::traffic_shadow;

// The current camera object (a global pointer); its world position is three floats at +112.
constexpr uint32_t kCameraGlobal = 0x82839704u;
constexpr uint32_t kCameraPosition = 112u;
// The vehicle: +20 the entity, whose Matrix44 (the translation is the row at +48) comes in two copies,
// +144 for the update thread and +208 for the render thread; the byte at [[r13]+72] says which this
// thread reads (sub_8235F738 picks the car's LOD by the same distance, from the same place).
constexpr uint32_t kVehicleEntity = 20u;
constexpr uint32_t kEntityMatrixUpdate = 144u;
constexpr uint32_t kEntityMatrixRender = 208u;
constexpr uint32_t kMatrixTranslation = 48u;
constexpr uint32_t kThreadCopyFlag = 72u;
// The vehicle's bytes the candidate test reads (the same ones sub_82360518 and sub_8235FA90 test).
constexpr uint32_t kVehicleVisible = 116u;      // u16: seen by the main view
constexpr uint32_t kVehicleEnabled = 120u;      // u16
constexpr uint32_t kVehicleGhost = 123u;        // u8
constexpr uint32_t kVehicleSideViews = 593u;    // u8
constexpr uint32_t kVehicleDisabled = 6467u;    // u8
constexpr uint32_t kVehicleHidden = 6478u;      // u8

bool PlausiblePtr(uint32_t p) { return p >= 0x10000u && p < 0xE0000000u; }
uint16_t ReadU16(const uint8_t* base, uint32_t a) { return uint16_t((uint32_t(base[a]) << 8) | base[a + 1]); }

mcla::gpu::LiveText& Setting() {
    static mcla::gpu::LiveText text("mcla_gpu_traffic_shadows");  // written by the pause menu's thread (live_text.h)
    return text;
}

// The mode in force: the setting with `auto` resolved for this graphics card. Until the card is known (the
// device opens after the guest starts; the Xenia renderer never knows it) `auto` is off, the game's own.
ts::Mode CurrentMode() {
    namespace gc = mcla::gpu::gpu_class;
    const std::string text = Setting().Get();
    if (text == "auto" && !gc::Known()) return ts::Mode::kOff;
    return ts::ModeFor(gc::Current(), text);
}

struct State {
    std::mutex lock;          // one render thread draws the phases; the lock only makes a second one harmless
    ts::Selector selector;
    ts::Mode mode = ts::Mode::kOff;
    uint64_t published_frame = 0;
    uint64_t logged_at = 0;
    bool ever_cast = false;
    // Sums of the frames since the last log line, for the mean the line prints (and the chain reads).
    uint64_t window_frames = 0;
    uint64_t window_candidates = 0;
    uint64_t window_casting = 0;
    uint32_t window_most = 0;
};
State& S() {
    static State state;
    return state;
}

// A new setting (or none) starts from a clean census and clean counters.
void ResetState(State& s, ts::Mode mode) {
    s.selector.Reset();
    s.mode = mode;
    s.published_frame = 0;
    s.logged_at = 0;
    s.ever_cast = false;
    s.window_frames = s.window_candidates = s.window_casting = 0;
    s.window_most = 0;
}

// One line a few seconds apart and at the first frame that has a car cast: the cars the first cascade saw,
// how many cast, how far, and the extra cascade draws that costs (the rule of thumb of traffic_shadow.h).
void MaybeLog(State& s, const ts::Limits& limits) {
    const ts::FrameStats& f = s.selector.last();
    ++s.window_frames;
    s.window_candidates += f.candidates;
    s.window_casting += f.casting;
    s.window_most = std::max(s.window_most, f.casting);
    const bool first_cast = f.casting > 0 && !s.ever_cast;
    if (!first_cast && !(f.frame >= s.logged_at + 600)) return;
    s.logged_at = f.frame;
    s.ever_cast = s.ever_cast || f.casting > 0;
    const double frames = double(std::max<uint64_t>(s.window_frames, 1));
    const double mean_casting = double(s.window_casting) / frames;
    LARECOMP_APP_INFO(
        "[traffic-shadows] {}: the last frame's first cascade saw {} candidate cars (nearest {:.1f} m), {} cast "
        "(farthest {:.1f} m; cutoff {:.1f} m of the radius {:.0f} m, at most {} cars, {} cascades); over the last {} "
        "frames: mean {:.2f} cars cast a frame (most {}) of {:.2f} candidates, about {:.0f} extra cascade draws a "
        "frame [I: {} per car and cascade]",
        ts::ModeName(s.mode), f.candidates, f.nearest, f.casting, f.farthest_cast, f.cutoff, limits.radius,
        limits.cars, limits.cascades, s.window_frames, mean_casting, s.window_most,
        double(s.window_candidates) / frames, mean_casting * limits.cascades * ts::kDrawsPerCarPerCascade,
        ts::kDrawsPerCarPerCascade);
    s.window_frames = s.window_candidates = s.window_casting = 0;
    s.window_most = 0;
}

}  // namespace
#endif

// 0x8236066C, before `cmplwi cr6,r11,0` in sub_82360518.
void Patch_TrafficCascadeGate(PPCRegister& r11, PPCRegister& r13, PPCRegister& r28, PPCRegister& r30) {
#if defined(LARECOMP_HAS_GPU)
    // The game's own caster (the player's car, +594 set): nothing to add, and the common exit of the player's
    // calls. Then the phases this does not act in (the collector 0x1000): no census, no memory read.
    if (r11.u64 != 0) return;
    const uint32_t phase = uint32_t(r28.u64);
    if (ts::CascadeIndex(phase) == 0) return;

    State& s = S();
    const ts::Mode mode = CurrentMode();
    if (mode == ts::Mode::kOff) {
        std::lock_guard<std::mutex> guard(s.lock);
        if (s.mode != ts::Mode::kOff) ResetState(s, ts::Mode::kOff);
        return;  // the published cutoff goes stale by itself: the renderer's screen-space term then covers every car
    }

    auto* rt = rex::Runtime::instance();
    const uint8_t* base = rt ? rt->virtual_membase() : nullptr;
    if (!base) return;
    const uint32_t vehicle = uint32_t(r30.u64);
    if (!PlausiblePtr(vehicle)) return;

    ts::VehicleFlags flags;
    flags.side_views = base[vehicle + kVehicleSideViews];
    flags.hidden = base[vehicle + kVehicleHidden];
    flags.disabled = base[vehicle + kVehicleDisabled];
    flags.ghost = base[vehicle + kVehicleGhost];
    flags.visible = ReadU16(base, vehicle + kVehicleVisible);
    flags.enabled = ReadU16(base, vehicle + kVehicleEnabled);
    if (!ts::Eligible(flags)) return;

    // Where the car is and where the camera is (the game's own LOD distance).
    const uint32_t entity = ReadGuestU32(base, vehicle + kVehicleEntity);
    const uint32_t camera = ReadGuestU32(base, kCameraGlobal);
    if (!PlausiblePtr(entity) || !PlausiblePtr(camera)) return;
    // Which copy of the matrix this thread reads (as sub_8235F738 does); the render thread's copy if the context is not readable.
    const uint32_t context = PlausiblePtr(uint32_t(r13.u64)) ? ReadGuestU32(base, uint32_t(r13.u64)) : 0u;
    const bool render_copy = !PlausiblePtr(context) || base[context + kThreadCopyFlag] != 0;
    const uint32_t matrix = entity + (render_copy ? kEntityMatrixRender : kEntityMatrixUpdate);
    float at[3], eye[3];
    for (uint32_t i = 0; i < 3; ++i) {
        at[i] = ReadGuestF32(base, matrix + kMatrixTranslation + 4u * i);
        eye[i] = ReadGuestF32(base, camera + kCameraPosition + 4u * i);
    }
    const float distance = ts::Distance(eye, at);
    if (!std::isfinite(distance)) return;

    const ts::Limits limits = ts::LimitsFor(mode, REXCVAR_GET(mcla_gpu_traffic_shadows_radius),
                                            REXCVAR_GET(mcla_gpu_traffic_shadows_cars),
                                            REXCVAR_GET(mcla_gpu_traffic_shadows_cascades));
    bool cast;
    {
        std::lock_guard<std::mutex> guard(s.lock);
        if (s.mode != mode) ResetState(s, mode);  // a new setting starts from a clean census
        cast = s.selector.Decide(vehicle, phase, distance, limits);
        const ts::FrameStats& f = s.selector.last();
        if (f.frame != s.published_frame) {
            s.published_frame = f.frame;
            ts::Publish(f.cutoff, ts::NowNs());  // the contact pass's screen-space term stands aside within it
            MaybeLog(s, limits);
        }
    }
    if (cast) r11.u64 = 1;
#else
    (void)r11; (void)r13; (void)r28; (void)r30;
#endif
}

#else  // REXGLUE_HAS_XEO3_TARGET
// XEO3 stub: an empty implementation so the linker resolves the codegen call.
#include <rex/ppc.h>
void Patch_TrafficCascadeGate(PPCRegister& r11, PPCRegister& r13, PPCRegister& r28, PPCRegister& r30) {}
#endif  // REXGLUE_HAS_XEO3_TARGET
