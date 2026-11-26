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
// 3. The turn of the shadow (fix/traffic-shadow-rotation, gpu/traffic_shadow_pose.h): the shadow technique of every ambient
//    shader group is rigid (no bone matrices) and the game's World matrix for the item's body draw is a translation, so a
//    raised traffic car was drawn into the cascades at its position but never turned. The hook at 0x827917C0 (the `bl
//    sub_8217D918` that sets the World matrix in sub_827914B8; r3 = the four rows at r1+144, r18 = the item's block, r27 =
//    the item) puts the body bone's rotation into that stack copy in a cascade phase. A debug log (mcla_gpu_traffic_shadows_log)
//    prints the body's heading against the shadow's and, from the hook at 0x82793608 (the `bl sub_82795960` that makes the
//    soft decal in phase 0x10000), against the decal's.
//
// Every test of the first two hooks has a counter (gpu/traffic_shadow_gates.h) and `[traffic-shadows] gates:` / `window:` /
// `census:` lines print them, so a run says which test turns the cars away and whether the distances are right.

#ifndef REXGLUE_HAS_XEO3_TARGET
#include "settings/larecomp_settings.h"
#include <rex/ppc/context.h>
#include <rex/ppc/recompiled_code.h>
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
#include "guest_timing.h"
#include "larecomp_log.h"
#include "larecomp_guest.h"
#if defined(LARECOMP_HAS_GPU)
#include "gpu/car_shadow_map_rules.h"
#include "gpu/cascade_schedule.h"
#include "gpu/gpu_class.h"
#include "gpu/live_text.h"
#include "gpu/traffic_shadow.h"
#include "gpu/traffic_shadow_gates.h"
#include "gpu/traffic_shadow_pose.h"
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

// Legacy auto waits for the native device to open; the compatibility
// renderer never reports a native device.
ts::Mode CurrentMode() {
    const std::string text = Setting().Get();
    if (text == "auto" && !mcla::gpu::gpu_class::Known()) return ts::Mode::kOff;
    return ts::ModeFor(text);
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
// how many qualify to cast, how far, and the pre-cull estimate of the extra draw count.
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
        "frame before reach culling [I: {} per car and cascade]",
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
    // CHECK: compare cutoffs and caster counts with the switch flipped in Near and All.
    const bool cast = s.selector.Decide(id, phase, distance, limits,
                                       [] { return larecomp::settings::mcla_fix_guest_shadow_work.Get(); });
    const ts::FrameStats& f = s.selector.last();
    if (f.frame != s.published_frame) {
        s.published_frame = f.frame;
        ts::Publish(f.cutoff, ts::NowNs());  // the contact pass's screen-space term stands aside within it
        MaybeLog(s, limits);
    }
    return cast;
}

const uint8_t* GuestBase() {
    auto* rt = REX_RUNTIME();
    return rt ? rt->GetMembase().GetBase() : nullptr;
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
bool FindEyes(const uint8_t* base, ts::Gates& gates, bool ambient_route, bool census, Eyes& out) {
    if (!base) return false;
    out.main_viewport = MainViewport(base, gates, ambient_route);
    out.have_main = out.main_viewport != 0 && ReadViewportEye(base, out.main_viewport, out.main_eye);
    if (ts::NeedsCurrentEye(out.have_main, census, larecomp::settings::mcla_fix_guest_shadow_work.Get())) {
        out.current_viewport = ReadGuestU32(base, ts::kCurrentViewportGlobal);
        out.have_current = ReadViewportEye(base, out.current_viewport, out.current_eye);
    }
    const float* chosen = out.have_main ? out.main_eye : out.current_eye;
    for (int i = 0; i < 3; ++i) out.eye[i] = chosen[i];
    return out.have_main || out.have_current;
}

bool MissesCascade(const uint8_t* base, uint32_t phase, float distance, float length,
                   const float (&at)[3], const Eyes& eyes) {
    const int cascade = ts::CascadeIndex(phase);
    if (!base || !eyes.have_main || !ts::BeyondCascadeFar(cascade, distance, length) ||
        !larecomp::settings::mcla_gpu_traffic_shadows_reach.Get()) return false;

    namespace cs = mcla::gpu::cascade_schedule;
    const uint64_t frame = cs::FrameSerial();
    if (frame == 0) return false;
    struct PhaseReach {
        uint64_t frame = 0;
        float far_scale = 0.0f;
        ts::CascadeReach reach;
    };
    static thread_local PhaseReach phases[4];
    PhaseReach& cached = phases[cascade - 1];
    if (cached.frame != frame) {
        cached = {.frame = frame};
        constexpr uint32_t kShadowManager = 0x8288D2B4;
        constexpr uint32_t kFarSlice = 160u;
        constexpr uint32_t kAtlasMatrix = 16u;
        const uint32_t manager = ReadGuestU32(base, kShadowManager);
        const auto addresses = cs::ReadTileAddresses(manager, uint8_t(cascade),
            [base](uint32_t address) { return ReadGuestU32(base, address); });
        if (addresses.Valid() && addresses.viewport == ReadGuestU32(base, ts::kCurrentViewportGlobal)) {
            cached.far_scale = ReadGuestF32(base, manager + kFarSlice);
            ts::AtlasMatrixUV matrix{};
            // CHECK: confirm the tile matrix and current viewport at both item hooks,
            // then inspect near-edge and low-sun shadows with the reach switch flipped.
            for (uint32_t row = 0; row < 4; ++row) {
                for (uint32_t col = 0; col < 2; ++col) {
                    matrix[row][col] = ReadGuestF32(base, addresses.tile + kAtlasMatrix + 16u * row + 4u * col);
                }
            }
            cached.reach = ts::MakeCascadeReach(cascade, matrix, eyes.main_eye);
        }
    }
    return ts::BeyondCascadeFar(cascade, distance, length, cached.far_scale) &&
           cached.reach.MissesTile(at, length);
}

// --- the turn of the shadow (gpu/traffic_shadow_pose.h) --------------------------------------------------------------------

ts::PoseCounts& Pose() {
    static ts::PoseCounts counts;
    return counts;
}

struct PoseLog {
    std::mutex lock;
    ts::YawHistory body;    // the turn rate of each car's body (by item)
    ts::YawHistory decal;   // the same read at the decal
    uint64_t logging_frame = std::numeric_limits<uint64_t>::max();  // the frame whose cars are being logged
    uint64_t last_logged = 0;
    bool any_logged = false;
};
PoseLog& PL() {
    static PoseLog log;
    return log;
}

// Whether this frame's cars are logged (mcla_gpu_traffic_shadows_log frames apart; every car of a chosen frame). `frame` is
// the selector's frame counter, which advances once per rendered frame while the cascades run.
bool LogThisFrame(uint64_t frame) {
    const int every = larecomp::settings::mcla_gpu_traffic_shadows_log.Get();
    if (every <= 0) return false;
    PoseLog& log = PL();
    std::lock_guard<std::mutex> guard(log.lock);
    if (log.logging_frame == frame) return true;
    if (log.any_logged && frame < log.last_logged + uint64_t(every)) return false;
    log.logging_frame = frame;
    log.last_logged = frame;
    log.any_logged = true;
    return true;
}

uint64_t CurrentFrame() {
    State& s = S();
    std::lock_guard<std::mutex> guard(s.lock);
    return s.selector.last().frame;
}

// The item's block, [[item+24] + (buffer + 2) * 4], as sub_82793360 finds it for the render thread; 0 if a link is not plausible.
uint32_t ItemBlock(const uint8_t* base, uint32_t item) {
    if (!PlausiblePtr(item)) return 0;
    const uint32_t buffer = ReadGuestU32(base, ts::kAmbientBufferGlobal);
    if (!ts::AmbientBufferValid(buffer)) return 0;
    const uint32_t blocks = ReadGuestU32(base, item + kItemModels);
    if (!PlausiblePtr(blocks)) return 0;
    const uint32_t block = ReadGuestU32(base, blocks + 4u * (buffer + 2u));
    return PlausiblePtr(block) ? block : 0;
}

void ReadBone(const uint8_t* base, uint32_t block, float bone[3][4]) {
    for (uint32_t r = 0; r < 3; ++r)
        for (uint32_t c = 0; c < 4; ++c) bone[r][c] = ReadGuestF32(base, block + ts::kBlockBones + 16u * r + 4u * c);
}

}  // namespace
#endif

// 0x8236066C, before `cmplwi cr6,r11,0` in sub_82360518 (the item draw of mcCarModelClass: the player's and the
// racers' cars).
void Patch_TrafficCascadeGate(PPCRegister& r11, PPCRegister& r13, PPCRegister& r28, PPCRegister& r30) {
    const mc::guest_timing::Scope timing(mc::guest_timing::Section::kTrafficShadowHooks);
#if defined(LARECOMP_HAS_GPU)
    using Gate = ts::CarGate;
    ts::Gates& gates = G();
    gates.Add(Gate::kCalls);
    NoteCall();
    // The game's own caster (the player's car, +594 set): nothing to add, and the common exit of the player's
    // calls (in free roam the only one: traffic is not a vehicle of this client, see the traffic route below).
    // Except where the car shadow map (gpu/car_shadow_map_rules.h Owner) casts the car's shadow on the world: there the
    // cascades must not draw the car at its low level of detail, which would put the game's own blob and its grain round the
    // map's crisp edge. r11 = 0 skips the cascade draw (the colour pass and the rest of the draw never read this branch).
    if (r11.u64 != 0) {
        gates.Add(Gate::kGameCaster);
        if (ts::CascadeIndex(uint32_t(r28.u64)) != 0 && mcla::gpu::car_shadow_map::PlayerShadowIsTheMaps()) {
            gates.Add(Gate::kPlayerLeftOut);
            r11.u64 = 0;
        } else if (r28.u32 == ts::kCascadePhases[0] && mcla::gpu::cascade_schedule::track_casters) {
            const auto* base = GuestBase();
            const uint32_t vehicle = r30.u32;
            const uint32_t entity = base && PlausiblePtr(vehicle) ? ReadGuestU32(base, vehicle + kVehicleEntity) : 0;
            if (PlausiblePtr(entity)) {
                const bool render_copy = ThreadReadsRenderCopy(base, r13.u32);
                const uint32_t row = entity + (render_copy ? kEntityMatrixRender : kEntityMatrixUpdate) + kMatrixTranslation;
                const float at[3] = {ReadGuestF32(base, row), ReadGuestF32(base, row + 4), ReadGuestF32(base, row + 8)};
                Eyes eyes;
                // The near stock caster is covered by cascades 1 and 2. A
                // detached camera can leave the player in a far cascade.
                if (FindEyes(base, gates, false, false, eyes) && ts::Distance(eyes.eye, at) > 15.0f) {
                    mcla::gpu::cascade_schedule::NoteCaster(vehicle, at, 4);
                }
            }
        }
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
    if (!FindEyes(base, gates, false, false, eyes)) {
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
    const ts::Limits limits = CurrentLimits(mode);
    if (DecideAndPublish(s, mode, limits, vehicle, phase, distance)) {
        // Retain the far-cascade refresh input even if this particular tile misses.
        if (phase == ts::kCascadePhases[0]) {
            mcla::gpu::cascade_schedule::NoteCaster(vehicle, at, uint8_t(limits.cascades));
        }
        if (MissesCascade(base, phase, distance, ts::kOpponentLengthPadding, at, eyes)) {
            gates.Add(Gate::kReach);
            return;
        }
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
    const mc::guest_timing::Scope timing(mc::guest_timing::Section::kTrafficShadowHooks);
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

// Includes both the gate and the original ambient item draw it can open.
#if defined(LARECOMP_HAS_GPU)
REX_EXTERN(__imp__rex_sub_82793360);
extern "C" REX_FUNC(rex_sub_82793360) {
    const mc::guest_timing::Scope timing(mc::guest_timing::Section::kCascadeCarDraws,
                                        mc::guest_timing::active_frame && ts::CascadeIndex(ctx.r4.u32) != 0);
    __imp__rex_sub_82793360(ctx, base);
}
#endif

// 0x82793360, the entry of sub_82793360 (the item draw of mcAmbientCarRenderClass: r3 = the item, r4 = the phase).
// In a cascade the draw knows no phase and returns; for a car the selector picks the phase becomes 0x100000, the
// branch that draws the body (traffic_shadow.h; 0x8000, the first version, is the car's lights). The draw's own tests
// (LOD, the item's and the car's flags) still apply. Only r4, a register, is changed: no guest memory is written.
void Patch_AmbientCascadeDraw(PPCRegister& r3, PPCRegister& r4, PPCRegister& r13) {
    const mc::guest_timing::Scope timing(mc::guest_timing::Section::kTrafficShadowHooks);
    TrafficProbeNoteItem(uint32_t(r3.u64));  // mcla_traffic_probe (debug): reads the item's LOD for its log; one atomic load when off
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
        have_eye = FindEyes(base, gates, true, census_phase, eyes);
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
    const float radius = ReadGuestF32(base, car + kCarRadius);
    if (census_phase) {
        // Does our distance agree with the item's own view depth (the LOD update's frustum test against the main
        // viewport)? A candidate is in the main view, so it must be at least its depth from the eye and not far more.
        const float depth_plus_radius = ReadGuestF32(base, item + kItemDepth);
        switch (ts::CheckAgainstDepth(distance, depth_plus_radius, radius)) {
            case ts::DepthVerdict::kConsistent: gates.Add(Gate::kDepthConsistent); break;
            case ts::DepthVerdict::kCloserThanDepth: gates.Add(Gate::kDepthCloser); break;
            case ts::DepthVerdict::kFarBeyondView: gates.Add(Gate::kDepthFar); break;
            default: gates.Add(Gate::kDepthUnknown); break;
        }
    }

    gates.Add(Gate::kDecided);
    const ts::Limits limits = CurrentLimits(mode);
    if (DecideAndPublish(s, mode, limits, item, phase, distance)) {
        if (census_phase) mcla::gpu::cascade_schedule::NoteCaster(item, at, uint8_t(limits.cascades));
        if (std::isfinite(radius) && radius > 0.0f &&
            MissesCascade(base, phase, distance, std::max(ts::kCarLengthPadding, 2.0f * radius), at, eyes)) {
            gates.Add(Gate::kReach);
            return;
        }
        gates.Add(Gate::kRemapped);
        r4.u64 = ts::kAmbientBodyPhase;
    }
#else
    (void)r3; (void)r4; (void)r13;
#endif
}

// 0x827917C0, the `bl sub_8217D918` of sub_827914B8 (the traffic model's body draw, model vtbl+28): sub_8217D918 sets the current
// viewport's World matrix from the four rows r3 points at (r1+144: the unit axes and block+16, a pure translation), r18 = the item's
// block, r27 = the item. In a sun cascade phase (which only the TRAFFIC SHADOWS hook above makes this draw run in) the rows get the
// body bone's rotation, because the ambient cars' shadow shader has no bone matrices (gpu/traffic_shadow_pose.h). The rows are the
// draw's own stack copy: nothing persistent is written.
void Patch_AmbientCascadeWorld(PPCRegister& r3, PPCRegister& r18, PPCRegister& r27) {
    const mc::guest_timing::Scope timing(mc::guest_timing::Section::kTrafficShadowHooks);
#if defined(LARECOMP_HAS_GPU)
    uint8_t* const base = REX_RUNTIME() ? larecomp::guest::Membase() : nullptr;
    if (!base) return;
    const uint32_t renderer = ReadGuestU32(base, ts::kRendererGlobal);
    if (!PlausiblePtr(renderer)) return;
    const uint32_t phase = ReadGuestU32(base, renderer + ts::kRendererPhaseOffset);
    const int cascade = ts::CascadeIndex(phase);
    if (cascade == 0) return;  // the colour pass, the lights: the game's own World

    ts::PoseCounts& counts = Pose();
    counts.calls.fetch_add(1, std::memory_order_relaxed);
    const uint32_t world_addr = uint32_t(r3.u64);
    const uint32_t block = uint32_t(r18.u64);
    const uint32_t item = uint32_t(r27.u64);
    if (!PlausiblePtr(world_addr) || !PlausiblePtr(block)) {
        counts.refused.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    float game_world[4][4];
    for (uint32_t r = 0; r < ts::kWorldRows; ++r)
        for (uint32_t c = 0; c < 4; ++c) game_world[r][c] = ReadGuestF32(base, world_addr + 16u * r + 4u * c);
    float bone[3][4];
    ReadBone(base, block, bone);

    const bool want_turn = larecomp::settings::mcla_gpu_traffic_shadows_rotation.Get();
    float used[4][4];
    bool turned = false;
    if (want_turn) {
        turned = ts::BodyBoneWorld(game_world, bone, used);
        if (turned) {
            for (uint32_t r = 0; r < ts::kWorldRows; ++r)
                for (uint32_t c = 0; c < 4; ++c) WriteGuestF32(base, world_addr + 16u * r + 4u * c, used[r][c]);
            counts.turned.fetch_add(1, std::memory_order_relaxed);
        } else {
            counts.refused.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        counts.switched_off.fetch_add(1, std::memory_order_relaxed);
    }

    // The debug line: once per car, in the first cascade of a logged frame.
    if (cascade != 1 || larecomp::settings::mcla_gpu_traffic_shadows_log.Get() <= 0) return;
    const uint64_t frame = CurrentFrame();
    if (!LogThisFrame(frame)) return;
    const float body_yaw = ts::BodyYaw(bone);
    const float game_yaw = ts::WorldYaw(game_world);
    const float shadow_yaw = turned ? ts::WorldYaw(used) : game_yaw;
    float rate = 0.0f;
    {
        PoseLog& log = PL();
        std::lock_guard<std::mutex> guard(log.lock);
        rate = log.body.Note(item, body_yaw, ts::NowNs());
    }
    float distance = std::numeric_limits<float>::quiet_NaN();
    {
        Eyes eyes;
        ts::Gates scratch;  // FindEyes counts its failures; the log wants none of them in the run's counters
        if (FindEyes(base, scratch, false, false, eyes)) {
            const float at[3] = {game_world[3][0] + bone[0][3], game_world[3][1] + bone[1][3], game_world[3][2] + bone[2][3]};
            distance = ts::Distance(eyes.eye, at);
        }
    }
    LARECOMP_APP_INFO(
        "[traffic-shadows] yaw: frame {} item 0x{:08X} dist {:.1f} body {:.1f} game {:.1f} shadow {:.1f} err {:.1f} rate {:.1f} "
        "turned {} (deg, m, deg/s)",
        frame, item, distance, body_yaw, game_yaw, shadow_yaw, ts::YawError(body_yaw, shadow_yaw), rate, turned ? 1 : 0);
#else
    (void)r3; (void)r18; (void)r27;
#endif
}

// 0x82793608, the `bl sub_82795960` of sub_82793360 in phase 0x10000: the soft decal under a car is made from the matrix at r1+160
// (r4: the bone 0 axes as rows, the position in row 3; r31 = the item). The log line only (mcla_gpu_traffic_shadows_log): the decal's
// heading against the body's, so a run can say whether the game's own decal turns with the car.
void Patch_AmbientDecalPose(PPCRegister& r4, PPCRegister& r31) {
    const mc::guest_timing::Scope timing(mc::guest_timing::Section::kTrafficShadowHooks);
#if defined(LARECOMP_HAS_GPU)
    if (larecomp::settings::mcla_gpu_traffic_shadows_log.Get() <= 0) return;
    uint8_t* const base = REX_RUNTIME() ? larecomp::guest::Membase() : nullptr;
    if (!base) return;
    const uint32_t decal = uint32_t(r4.u64);
    const uint32_t item = uint32_t(r31.u64);
    if (!PlausiblePtr(decal)) return;
    const uint32_t block = ItemBlock(base, item);
    if (block == 0) return;
    const uint64_t frame = CurrentFrame();
    if (!LogThisFrame(frame)) return;
    float bone[3][4];
    ReadBone(base, block, bone);
    // The decal's rows 0..2 are the bone's columns; row 2 is the model's +z axis.
    const float decal_yaw = ts::YawOfDirection(ReadGuestF32(base, decal + 32u), ReadGuestF32(base, decal + 40u));
    const float body_yaw = ts::BodyYaw(bone);
    float rate = 0.0f;
    {
        PoseLog& log = PL();
        std::lock_guard<std::mutex> guard(log.lock);
        rate = log.decal.Note(item, decal_yaw, ts::NowNs());
    }
    LARECOMP_APP_INFO("[traffic-shadows] decal: frame {} item 0x{:08X} body {:.1f} decal {:.1f} err {:.1f} rate {:.1f} (deg, deg/s)",
                      frame, item, body_yaw, decal_yaw, ts::YawError(body_yaw, decal_yaw), rate);
#else
    (void)r4; (void)r31;
#endif
}

#else  // REXGLUE_HAS_XEO3_TARGET
// XEO3 stubs: empty implementations so the linker resolves the codegen calls.
#include <rex/ppc/context.h>
void Patch_TrafficCascadeGate(PPCRegister& r11, PPCRegister& r13, PPCRegister& r28, PPCRegister& r30) {}
void Patch_AmbientCascadeClient(PPCRegister& r11, PPCRegister& r30, PPCRegister& r31) {}
void Patch_AmbientCascadeDraw(PPCRegister& r3, PPCRegister& r4, PPCRegister& r13) {}
void Patch_AmbientCascadeWorld(PPCRegister& r3, PPCRegister& r18, PPCRegister& r27) {}
void Patch_AmbientDecalPose(PPCRegister& r4, PPCRegister& r31) {}
#endif  // REXGLUE_HAS_XEO3_TARGET
