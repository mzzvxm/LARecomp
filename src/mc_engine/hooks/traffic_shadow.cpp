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
//    where it draws nothing, so the hook at its entry tells it the phase is 0x100000 (the body draw of the colour
//    pass, which has no side effect on the item's LOD) for the cars the selector picks.
//
// Where the distances are measured from (the second run's finding): in a cascade phase the game's CURRENT viewport
// ([0x82839704]) is the cascade's own, about 100 m from the cars, so both routes measure from the eye of the main
// viewport, [[0x8287E064] + 180 + 4 * buffer], the one the game's own LOD tests use (gpu/traffic_shadow.h).
//
// Every test of both hooks has a counter (gpu/traffic_shadow_gates.h) and `[traffic-shadows] gates:` / `window:` /
// `census:` lines print them, so a run says which test turns the cars away and whether the distances are right.

#ifndef REXGLUE_HAS_XEO3_TARGET
#include "settings/larecomp_settings.h"
#include <rex/ppc/context.h>
#include <rex/kernel/title_runtime.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
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

#if defined(LARECOMP_HAS_GPU)
namespace {

namespace ts = mcla::gpu::traffic_shadow;

// A viewport's eye is three floats at +112. The CURRENT viewport (a global pointer) is the one being rendered in: in a
// sun cascade the cascade's own, so the distances are measured from the main viewport's eye (traffic_shadow.h).
constexpr uint32_t kCameraPosition = ts::kViewportEye;
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
constexpr uint32_t kItemLod = 28u;          // i32, negative = outside the main view, 2 = the coarsest (not drawn by the body phase)
constexpr uint32_t kItemEnabled = 36u;      // u8
constexpr uint32_t kModelCount = 32u;       // i32, in the block [item+24] points to
constexpr uint32_t kItemDepth = 32u;        // float: the view depth + radius from the LOD update's frustum test
constexpr uint32_t kCarRadius = 492u;       // float in the car object: the bounding radius the LOD update uses
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

// The `[traffic-shadows] gates:` / `window:` / `census:` lines: 20 s after the 2000th call of the hooks (which come in
// the world's first half second), then about every 30 s. The call counter is one relaxed add; the schedule (and the
// clock) is asked on every 256th call.
void NoteCall() {
    static std::mutex log_lock;
    static ts::LogSchedule schedule;
    const uint64_t calls = G().BumpCalls();
    if (calls < ts::LogSchedule::kFirstAfterCalls || (calls & 255u) != 0) return;
    std::lock_guard<std::mutex> guard(log_lock);
    if (!schedule.Due(calls, ts::NowNs())) return;
    LARECOMP_APP_INFO("{}", G().Line(std::string(ts::ModeName(CurrentMode())).c_str()));
    LARECOMP_APP_INFO("{}", G().WindowLine());
    LARECOMP_APP_INFO("{}", G().CensusLine());
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
    return ts::LimitsFor(mode, larecomp::settings::mcla_gpu_traffic_shadows_radius.Get(), larecomp::settings::mcla_gpu_traffic_shadows_cars.Get(),
                         larecomp::settings::mcla_gpu_traffic_shadows_cascades.Get());
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

// A viewport's eye (three floats at +112); false if the pointer or the numbers are not plausible.
bool ReadViewportEye(const uint8_t* base, uint32_t viewport, float eye[3]) {
    if (!PlausiblePtr(viewport)) return false;
    for (uint32_t i = 0; i < 3; ++i) eye[i] = ReadGuestF32(base, viewport + kCameraPosition + 4u * i);
    return ts::PlausiblePosition(eye);
}

// The main view's viewport for the render buffer: [[0x8287E064] + 180 + 4 * [0x827E5640]], as sub_827902F0 finds it
// for the LOD test and sub_823115E8 puts it back after the sun cascades. 0 if any link is not plausible (the counters
// say which: a false renderer pointer, a false buffer, a false viewport).
uint32_t MainViewport(const uint8_t* base, ts::Gates& gates, bool ambient_route) {
    const uint32_t renderer = ReadGuestU32(base, ts::kRendererGlobal);
    if (!PlausiblePtr(renderer)) {
        if (ambient_route) gates.Add(ts::AmbientGate::kRenderer);
        return 0;
    }
    const uint32_t buffer = ReadGuestU32(base, ts::kAmbientBufferGlobal);
    if (!ts::AmbientBufferValid(buffer)) {
        if (ambient_route) gates.Add(ts::AmbientGate::kBuffer);
        return 0;
    }
    const uint32_t viewport = ReadGuestU32(base, renderer + ts::MainViewportSlot(buffer));
    if (!PlausiblePtr(viewport)) {
        if (ambient_route) gates.Add(ts::AmbientGate::kViewport);
        return 0;
    }
    return viewport;
}

// The eye the distances are measured from: the main viewport's; if it cannot be read, the current viewport's (what the
// first version used: in a cascade the cascade's own eye, so the distances are wrong, but the counters say so). Both are
// kept: the census compares them. `eye` is the one to measure from; false if no eye was found at all.
struct Eyes {
    float eye[3] = {0, 0, 0};
    float main_eye[3] = {0, 0, 0};
    float current_eye[3] = {0, 0, 0};
    uint32_t main_viewport = 0;
    uint32_t current_viewport = 0;
    bool have_main = false;
    bool have_current = false;
};
bool FindEyes(const uint8_t* base, ts::Gates& gates, bool ambient_route, Eyes& out) {
    out.main_viewport = MainViewport(base, gates, ambient_route);
    out.have_main = out.main_viewport != 0 && ReadViewportEye(base, out.main_viewport, out.main_eye);
    out.current_viewport = ReadGuestU32(base, ts::kCurrentViewportGlobal);
    out.have_current = ReadViewportEye(base, out.current_viewport, out.current_eye);
    const float* chosen = out.have_main ? out.main_eye : out.current_eye;
    for (int i = 0; i < 3; ++i) out.eye[i] = chosen[i];
    return out.have_main || out.have_current;
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

    // Where the car is and where the eye is (the main viewport's: in a cascade the current viewport is the cascade's own).
    const uint32_t entity = ReadGuestU32(base, vehicle + kVehicleEntity);
    if (!PlausiblePtr(entity)) {
        gates.Add(Gate::kEntityPtr);
        return;
    }
    Eyes eyes;
    if (!FindEyes(base, gates, false, eyes)) {
        gates.Add(Gate::kCameraPtr);
        return;
    }
    gates.Add(eyes.have_main ? Gate::kEyeMain : Gate::kEyeFallback);
    // Which copy of the matrix this thread reads (as sub_8235F738 does).
    const bool render_copy = ThreadReadsRenderCopy(base, uint32_t(r13.u64));
    const uint32_t matrix = entity + (render_copy ? kEntityMatrixRender : kEntityMatrixUpdate);
    float at[3];
    for (uint32_t i = 0; i < 3; ++i) at[i] = ReadGuestF32(base, matrix + kMatrixTranslation + 4u * i);
    const float distance = ts::Distance(eyes.eye, at);
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
// In a cascade the draw knows no phase and returns; for a car the selector picks the phase becomes 0x100000, the
// branch that draws the body (traffic_shadow.h; 0x8000, the first version, is the car's lights). The draw's own tests
// (LOD, the item's and the car's flags) still apply. Only r4, a register, is changed: no guest memory is written.
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

    // The tests the body phase makes before the model's draw (traffic_shadow.h).
    const uint32_t models = ReadGuestU32(base, item + kItemModels);
    ts::AmbientFlags flags;
    flags.enabled = base[item + kItemEnabled];
    flags.car_flags = base[car + kCarFlags];
    flags.model_count = PlausiblePtr(models) ? int32_t(ReadGuestU32(base, models + kModelCount)) : -1;
    flags.lod = int32_t(ReadGuestU32(base, item + kItemLod));
    const uint32_t rejected = ts::AmbientRejectBits(flags);
    const bool census_phase = ts::CascadeIndex(phase) == 1;  // the first cascade: every item once a frame, the census

    // The car's world position (sub_82794EE8's matrix, translation row at +48) and the eye's. In the census phase this
    // is done for every item, whatever the tests said, so that the counters show where the cars the tests turn away are.
    Eyes eyes;
    float at[3] = {0, 0, 0};
    float distance = std::numeric_limits<float>::quiet_NaN();
    bool measured = false;
    bool have_eye = false;
    if (rejected == 0 || census_phase) {
        have_eye = FindEyes(base, gates, true, eyes);
        if (have_eye) {
            const bool render_copy = ThreadReadsRenderCopy(base, uint32_t(r13.u64));
            const uint32_t matrix = car + ts::AmbientMatrixOffset(render_copy, buffer);
            for (uint32_t i = 0; i < 3; ++i) at[i] = ReadGuestF32(base, matrix + ts::kAmbientTranslation + 4u * i);
            if (ts::PlausiblePosition(at)) {
                distance = ts::Distance(eyes.eye, at);
                measured = std::isfinite(distance);
            }
            if (census_phase) gates.NoteEyes(eyes.main_viewport, eyes.main_eye, eyes.current_viewport, eyes.current_eye);
        }
    }
    if (census_phase && measured) {
        ts::BandKind kind = ts::BandKind::kOther;
        if (rejected == 0) {
            kind = ts::BandKind::kCandidate;
        } else if ((rejected & ts::kAmbientRejectLod) != 0) {
            kind = ts::BandKind::kLodOutside;
        } else if ((rejected & ts::kAmbientRejectLodCoarse) != 0) {
            kind = ts::BandKind::kLodCoarse;
        }
        gates.AddBand(kind, distance);
    }

    if (rejected != 0) {
        gates.Add(Gate::kIneligible);
        if (rejected & ts::kAmbientRejectEnabled) gates.Add(Gate::kEnabled);
        if (rejected & ts::kAmbientRejectCarFlags) gates.Add(Gate::kCarFlags);
        if (rejected & ts::kAmbientRejectModels) gates.Add(Gate::kModels);
        if (rejected & ts::kAmbientRejectLod) gates.Add(Gate::kLod);
        if (rejected & ts::kAmbientRejectLodCoarse) gates.Add(Gate::kLodCoarse);
        return;
    }
    if (!have_eye) {
        gates.Add(Gate::kCameraPtr);
        return;
    }
    gates.Add(eyes.have_main ? Gate::kEyeMain : Gate::kEyeFallback);
    if (!ts::PlausiblePosition(at)) {
        gates.Add(Gate::kPosition);
        return;
    }
    if (!measured) {
        gates.Add(Gate::kDistance);
        return;
    }
    if (census_phase) {
        // Does our distance agree with the item's own view depth (the LOD update's frustum test against the main
        // viewport)? A candidate is in the main view, so it must be at least its depth from the eye and not far more.
        const float depth_plus_radius = ReadGuestF32(base, item + kItemDepth);
        const float radius = ReadGuestF32(base, car + kCarRadius);
        switch (ts::CheckAgainstDepth(distance, depth_plus_radius, radius)) {
            case ts::DepthVerdict::kConsistent: gates.Add(Gate::kDepthConsistent); break;
            case ts::DepthVerdict::kCloserThanDepth: gates.Add(Gate::kDepthCloser); break;
            case ts::DepthVerdict::kFarBeyondView: gates.Add(Gate::kDepthFar); break;
            default: gates.Add(Gate::kDepthUnknown); break;
        }
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
void Patch_TrafficCascadeGate(PPCRegister& r11, PPCRegister& r13, PPCRegister& r28, PPCRegister& r30) {}
void Patch_AmbientCascadeClient(PPCRegister& r11, PPCRegister& r30, PPCRegister& r31) {}
void Patch_AmbientCascadeDraw(PPCRegister& r3, PPCRegister& r4, PPCRegister& r13) {}
#endif  // REXGLUE_HAS_XEO3_TARGET
