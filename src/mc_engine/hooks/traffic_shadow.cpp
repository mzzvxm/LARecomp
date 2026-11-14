// Traffic and AI cars cast into the sun's shadow cascades (the TRAFFIC SHADOWS setting).
//
// The story, the guest facts and the rule that picks the cars are in gpu/traffic_shadow.h (pure, with its
// own check). This file is the part that touches the guest, in two routes:
//
// 1. The racers (and the player's car, which the game casts itself): the hook at 0x8236066C in Vehicle::Draw
//    (sub_82360518), the item draw of the renderer's mcCarModelClass client.
//
//   0x82360668  lbz   r11,594(r30)       r30 = the vehicle: the byte the constructor sets for the player's car only
//   0x8236066C  cmplwi cr6,r11,0         <- the hook runs before this one; r11 = 1 makes the game draw the car
//   0x82360670  beq   cr6,0x82360688        into the current phase (sub_8235FA90) instead of skipping it
//
//   This place is reached in the phases 0x20 0x40 0x80 0x100 (the four sun cascades; the client's mask has no
//   0x1000) for a vehicle whose +593 is set (the branch at 0x82360664 skips the rest otherwise). Registers: r11 =
//   +594 (the hook raises it), r13 = the thread's guest context (which copy of the entity's matrix the thread
//   reads), r28 = the phase, r30 = the vehicle.
//
// 2. TRAFFIC, which is not a vehicle of that client at all (the first run's finding, traffic_shadow.h): the items
//    of the mcAmbientCarRenderClass client. The phase loop (sub_822E6408) asks `client mask & phase` at
//    0x822E6870 (r11 = the mask, r30 = the phase, r31 = the client): the hook adds the cascade phase to the mask
//    of that one client. Its item draw (sub_82793360, r3 = the item, r4 = the phase) then runs in the cascades,
//    where it draws nothing, so the hook at its entry tells it the phase is 0x8000 (the plain body draw) for
//    the cars the selector picks.
//
// Every test of both hooks has a counter (gpu/traffic_shadow_gates.h) and one `[traffic-shadows] gates:` line
// prints them, so a run says which test turns the cars away.

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
#include "gpu/traffic_shadow_gates.h"
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
// The traffic's items (the item draw sub_82793360 reads these; traffic_shadow.h has the story).
constexpr uint32_t kItemCar = 16u;          // the car object
constexpr uint32_t kItemModels = 24u;       // -> a block whose +32 is the model count
constexpr uint32_t kItemLod = 28u;          // i32, negative = not drawn
constexpr uint32_t kItemEnabled = 36u;      // u8
constexpr uint32_t kItemFlags = 556u;       // u8: 0x20 skips the LOD test, 0x80 = a body to draw
constexpr uint32_t kItemVisible = 557u;     // u8 + the buffer index: bit 4 = seen by the main view
constexpr uint32_t kModelCount = 32u;       // i32, in the block [item+24] points to
constexpr uint32_t kCarFlags = 500u;        // u8 in the car object

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

ts::Gates& G() {
    static ts::Gates gates;
    return gates;
}

// One `[traffic-shadows] gates:` line: after the first 2000 calls of the hooks, then about every 30 s. The call
// counter is one relaxed add; the clock is read for the first line, then on every 256th call.
void NoteCall() {
    static std::atomic<bool> first_pending{true};
    static std::mutex log_lock;
    static ts::LogSchedule schedule;
    const uint64_t calls = G().BumpCalls();
    const bool first = first_pending.load(std::memory_order_relaxed);
    if (calls < ts::LogSchedule::kFirstAfterCalls || (!first && (calls & 255u) != 0)) return;
    std::lock_guard<std::mutex> guard(log_lock);
    if (!schedule.Due(calls, ts::NowNs())) return;
    first_pending.store(false, std::memory_order_relaxed);
    LARECOMP_APP_INFO("{}", G().Line(std::string(ts::ModeName(CurrentMode())).c_str()));
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

// The setting went off: forget the census (the published cutoff goes stale by itself, and the renderer's
// screen-space term then covers every car).
void NoteOff(State& s) {
    std::lock_guard<std::mutex> guard(s.lock);
    if (s.mode != ts::Mode::kOff) ResetState(s, ts::Mode::kOff);
}

// The limits of the setting in force, the debug overrides applied.
ts::Limits CurrentLimits(ts::Mode mode) {
    return ts::LimitsFor(mode, REXCVAR_GET(mcla_gpu_traffic_shadows_radius), REXCVAR_GET(mcla_gpu_traffic_shadows_cars),
                         REXCVAR_GET(mcla_gpu_traffic_shadows_cascades));
}

// The census and the decision, for either route: whether the car (any stable id: its guest address) casts in
// `phase` now. At a frame's end the cutoff is published for the contact pass and the frame's line is logged.
bool DecideAndPublish(State& s, ts::Mode mode, const ts::Limits& limits, uint32_t id, uint32_t phase, float distance) {
    std::lock_guard<std::mutex> guard(s.lock);
    if (s.mode != mode) ResetState(s, mode);  // a new setting starts from a clean census
    const bool cast = s.selector.Decide(id, phase, distance, limits);
    const ts::FrameStats& f = s.selector.last();
    if (f.frame != s.published_frame) {
        s.published_frame = f.frame;
        ts::Publish(f.cutoff, ts::NowNs());  // the contact pass's screen-space term stands aside within it
        MaybeLog(s, limits);
    }
    return cast;
}

const uint8_t* GuestBase() {
    auto* rt = rex::Runtime::instance();
    return rt ? rt->virtual_membase() : nullptr;
}

// Which copy of a thread-split state this thread reads: the byte [[r13]+72], as sub_8235F738 and sub_82794EE8
// read it; the render thread's copy if the context is not readable.
bool ThreadReadsRenderCopy(const uint8_t* base, uint32_t r13) {
    const uint32_t context = PlausiblePtr(r13) ? ReadGuestU32(base, r13) : 0u;
    return !PlausiblePtr(context) || base[context + kThreadCopyFlag] != 0;
}

}  // namespace
#endif

// 0x8236066C, before `cmplwi cr6,r11,0` in sub_82360518 (the item draw of mcCarModelClass: the player's and the
// racers' cars).
void Patch_TrafficCascadeGate(PPCRegister& r11, PPCRegister& r13, PPCRegister& r28, PPCRegister& r30) {
#if defined(LARECOMP_HAS_GPU)
    using Gate = ts::CarGate;
    ts::Gates& gates = G();
    gates.Add(Gate::kCalls);
    NoteCall();
    // The game's own caster (the player's car, +594 set): nothing to add, and the common exit of the player's
    // calls (in free roam the only one: traffic is not a vehicle of this client, see the traffic route below).
    if (r11.u64 != 0) {
        gates.Add(Gate::kGameCaster);
        return;
    }
    const uint32_t phase = uint32_t(r28.u64);
    if (ts::CascadeIndex(phase) == 0) {
        gates.Add(Gate::kOtherPhase);
        gates.car_phases().Add(phase);
        return;
    }

    State& s = S();
    const ts::Mode mode = CurrentMode();
    if (mode == ts::Mode::kOff) {
        gates.Add(Gate::kModeOff);
        NoteOff(s);
        return;
    }

    const uint8_t* base = GuestBase();
    if (!base) {
        gates.Add(Gate::kNoMemory);
        return;
    }
    const uint32_t vehicle = uint32_t(r30.u64);
    if (!PlausiblePtr(vehicle)) {
        gates.Add(Gate::kVehiclePtr);
        return;
    }

    ts::VehicleFlags flags;
    flags.side_views = base[vehicle + kVehicleSideViews];
    flags.hidden = base[vehicle + kVehicleHidden];
    flags.disabled = base[vehicle + kVehicleDisabled];
    flags.ghost = base[vehicle + kVehicleGhost];
    flags.visible = ReadU16(base, vehicle + kVehicleVisible);
    flags.enabled = ReadU16(base, vehicle + kVehicleEnabled);
    const uint32_t rejected = ts::VehicleRejectBits(flags);
    if (rejected != 0) {
        gates.Add(Gate::kIneligible);
        if (rejected & ts::kRejectSideViews) gates.Add(Gate::kSideViews);
        if (rejected & ts::kRejectHidden) gates.Add(Gate::kHidden);
        if (rejected & ts::kRejectDisabled) gates.Add(Gate::kDisabled);
        if (rejected & ts::kRejectGhost) gates.Add(Gate::kGhost);
        if (rejected & ts::kRejectNotVisible) gates.Add(Gate::kNotVisible);
        if (rejected & ts::kRejectNotEnabled) gates.Add(Gate::kNotEnabled);
        return;
    }

    // Where the car is and where the camera is (the game's own LOD distance).
    const uint32_t entity = ReadGuestU32(base, vehicle + kVehicleEntity);
    const uint32_t camera = ReadGuestU32(base, kCameraGlobal);
    if (!PlausiblePtr(entity)) {
        gates.Add(Gate::kEntityPtr);
        return;
    }
    if (!PlausiblePtr(camera)) {
        gates.Add(Gate::kCameraPtr);
        return;
    }
    // Which copy of the matrix this thread reads (as sub_8235F738 does).
    const bool render_copy = ThreadReadsRenderCopy(base, uint32_t(r13.u64));
    const uint32_t matrix = entity + (render_copy ? kEntityMatrixRender : kEntityMatrixUpdate);
    float at[3], eye[3];
    for (uint32_t i = 0; i < 3; ++i) {
        at[i] = ReadGuestF32(base, matrix + kMatrixTranslation + 4u * i);
        eye[i] = ReadGuestF32(base, camera + kCameraPosition + 4u * i);
    }
    const float distance = ts::Distance(eye, at);
    if (!std::isfinite(distance)) {
        gates.Add(Gate::kDistance);
        return;
    }

    gates.Add(Gate::kDecided);
    if (DecideAndPublish(s, mode, CurrentLimits(mode), vehicle, phase, distance)) {
        gates.Add(Gate::kCast);
        r11.u64 = 1;
    }
#else
    (void)r11; (void)r13; (void)r28; (void)r30;
#endif
}

// 0x822E6870, before `and r10,r11,r30` in sub_822E6408 (the phase loop's test of a client's phase mask): r11 = the
// client's mask, r30 = the phase, r31 = the client. For the traffic client (mcAmbientCarRenderClass) in a sun
// cascade the mask gets the phase, so the client is called in it. Nothing is written into the client.
void Patch_AmbientCascadeClient(PPCRegister& r11, PPCRegister& r30, PPCRegister& r31) {
#if defined(LARECOMP_HAS_GPU)
    using Gate = ts::AmbientGate;
    const uint32_t phase = uint32_t(r30.u64);
    if (ts::CascadeIndex(phase) == 0) return;  // most of the loop's iterations: no memory read
    const uint8_t* base = GuestBase();
    const uint32_t client = uint32_t(r31.u64);
    if (!base || !PlausiblePtr(client) || ReadGuestU32(base, client) != ts::kAmbientClientVtable) return;

    ts::Gates& gates = G();
    gates.Add(Gate::kLoopCalls);
    NoteCall();
    if (CurrentMode() == ts::Mode::kOff) {
        gates.Add(Gate::kLoopOff);
        return;
    }
    gates.Add(Gate::kLoopWidened);
    r11.u64 = ts::WidenedClientMask(uint32_t(r11.u64), phase, true);
#else
    (void)r11; (void)r30; (void)r31;
#endif
}

// 0x82793360, the entry of sub_82793360 (the item draw of mcAmbientCarRenderClass: r3 = the item, r4 = the phase).
// In a cascade the draw knows no phase and returns; for a car the selector picks the phase becomes 0x8000, the
// branch that draws the body (traffic_shadow.h). The draw's own tests (visible byte, LOD, body) still apply.
void Patch_AmbientCascadeDraw(PPCRegister& r3, PPCRegister& r4, PPCRegister& r13) {
#if defined(LARECOMP_HAS_GPU)
    using Gate = ts::AmbientGate;
    ts::Gates& gates = G();
    gates.Add(Gate::kLeafCalls);
    const uint32_t phase = uint32_t(r4.u64);
    if (ts::CascadeIndex(phase) == 0) {
        gates.leaf_phases().Add(phase);
        return;
    }
    gates.Add(Gate::kLeafCascade);
    NoteCall();

    State& s = S();
    const ts::Mode mode = CurrentMode();
    if (mode == ts::Mode::kOff) {
        gates.Add(Gate::kModeOff);
        NoteOff(s);
        return;
    }
    const uint8_t* base = GuestBase();
    if (!base) {
        gates.Add(Gate::kNoMemory);
        return;
    }
    const uint32_t item = uint32_t(r3.u64);
    if (!PlausiblePtr(item)) {
        gates.Add(Gate::kItemPtr);
        return;
    }
    const uint32_t car = ReadGuestU32(base, item + kItemCar);
    if (!PlausiblePtr(car)) {
        gates.Add(Gate::kCarPtr);
        return;
    }
    const uint32_t buffer = ReadGuestU32(base, ts::kAmbientBufferGlobal);
    if (!ts::AmbientBufferValid(buffer)) {
        gates.Add(Gate::kBuffer);
        return;
    }

    // The tests the item draw makes before it draws the body (traffic_shadow.h).
    const uint32_t models = ReadGuestU32(base, item + kItemModels);
    ts::AmbientFlags flags;
    flags.enabled = base[item + kItemEnabled];
    flags.car_flags = base[car + kCarFlags];
    flags.model_count = PlausiblePtr(models) ? int32_t(ReadGuestU32(base, models + kModelCount)) : -1;
    flags.lod = int32_t(ReadGuestU32(base, item + kItemLod));
    flags.item_flags = base[item + kItemFlags];
    flags.visible = base[item + kItemVisible + buffer];
    const uint32_t rejected = ts::AmbientRejectBits(flags);
    if (rejected != 0) {
        gates.Add(Gate::kIneligible);
        if (rejected & ts::kAmbientRejectEnabled) gates.Add(Gate::kEnabled);
        if (rejected & ts::kAmbientRejectCarFlags) gates.Add(Gate::kCarFlags);
        if (rejected & ts::kAmbientRejectModels) gates.Add(Gate::kModels);
        if (rejected & ts::kAmbientRejectNotVisible) gates.Add(Gate::kNotVisible);
        if (rejected & ts::kAmbientRejectLod) gates.Add(Gate::kLod);
        if (rejected & ts::kAmbientRejectNoBody) gates.Add(Gate::kNoBody);
        return;
    }

    // The car's world position (sub_82794EE8's matrix, translation row at +48) and the camera's.
    const uint32_t camera = ReadGuestU32(base, kCameraGlobal);
    if (!PlausiblePtr(camera)) {
        gates.Add(Gate::kCameraPtr);
        return;
    }
    const bool render_copy = ThreadReadsRenderCopy(base, uint32_t(r13.u64));
    const uint32_t matrix = car + ts::AmbientMatrixOffset(render_copy, buffer);
    float at[3], eye[3];
    for (uint32_t i = 0; i < 3; ++i) {
        at[i] = ReadGuestF32(base, matrix + ts::kAmbientTranslation + 4u * i);
        eye[i] = ReadGuestF32(base, camera + kCameraPosition + 4u * i);
    }
    if (!ts::PlausiblePosition(at) || !ts::PlausiblePosition(eye)) {
        gates.Add(Gate::kPosition);
        return;
    }
    const float distance = ts::Distance(eye, at);
    if (!std::isfinite(distance)) {
        gates.Add(Gate::kDistance);
        return;
    }

    gates.Add(Gate::kDecided);
    if (DecideAndPublish(s, mode, CurrentLimits(mode), item, phase, distance)) {
        gates.Add(Gate::kRemapped);
        r4.u64 = ts::kAmbientBodyPhase;
    }
#else
    (void)r3; (void)r4; (void)r13;
#endif
}

#else  // REXGLUE_HAS_XEO3_TARGET
// XEO3 stubs: empty implementations so the linker resolves the codegen calls.
#include <rex/ppc.h>
void Patch_TrafficCascadeGate(PPCRegister& r11, PPCRegister& r13, PPCRegister& r28, PPCRegister& r30) {}
void Patch_AmbientCascadeClient(PPCRegister& r11, PPCRegister& r30, PPCRegister& r31) {}
void Patch_AmbientCascadeDraw(PPCRegister& r3, PPCRegister& r4, PPCRegister& r13) {}
#endif  // REXGLUE_HAS_XEO3_TARGET
