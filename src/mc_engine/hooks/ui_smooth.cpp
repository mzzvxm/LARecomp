// Smooth UI (ui_interpolate): the Flash movies step at the console's 30 a second and
// what moved in the last step is drawn between where it was and where it is.
//
// ui.cpp paces the movies and tells this file about every update and step of a paced
// movie (UiSmoothTouch, UiSmoothNoteStep). The player's draw walk computes a node's
// WORLD matrix and colour transform from its LOCAL ones (which only a step changes) and
// its parent's world ones, then draws the node or walks its children:
//
//   sub_825FB348(node r3, parent's state block r4)
//     0x825FB38C-0x825FB454  world matrix  = local matrix x parent world matrix
//                            (rows a b / c d / tx ty; node +8 local, +32 world;
//                            parent block +24)
//     0x825FB480  bl sub_825F8B20: world colour transform = local x parent world x
//                            tint (node +56 local, +64 world; parent block +56, tint at
//                            +68)
//     0x825FB484  lhz r9,170(r30)  <- the hook: r30 = the node, r31 = the parent block
//
// For a node whose local values changed in the last step the hook redoes both world
// values with the local values blended between the step before and the last one, by how
// far into the step period the frame is. The node's draw and all its children (which
// read this node's world values) follow. The local values are not touched, so the next
// step's deltas and the game's scripts see what they always saw.
//
// Everything is guest memory read and written on the guest's draw path (the world values
// are what the guest's own D3D calls consume afterwards), so it does not depend on which
// renderer sits below the guest. The picture is one step behind the game's state, which
// is the price of the smoothness (up to 33 ms at 30 steps a second). Nothing is blended
// while the frame time is longer than 0.6 of a step (under 50 FPS).

#ifndef REXGLUE_HAS_XEO3_TARGET
#include <rex/cvar.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "hooks.h"
#include "hooks_internal.h"
#include "../swf_dom.h"

REXCVAR_DEFINE_BOOL(ui_interpolate, true, "MCLA/UI",
    "Smooth UI. Every UI movie (menus, pop-ups, map panels and the HUD) steps at the "
    "console's 30 a second, so they run at the console's speed at any frame rate, and "
    "what moved in the last step is drawn between where it was and where it is: "
    "needles, the song ticker, the menus' lists and highlights, page changes and "
    "fades glide instead of stepping. The picture is one step (up to 33 ms) behind the "
    "game; nothing is blended below 50 FPS. Off, only the movies named in "
    "ui_movie_pacing step at 30 a second and nothing is blended.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {
// The swf context the player is advancing or drawing: both write it (sub_825EDC08 and
// sub_825EE970, the draw) and clear it after.
constexpr uint32_t kUiCurrentContext = 0x828FF970;
// A node that moves farther than this many pixels of the movie in one step is a cut or
// a wrap, shown at once. One that turns round and jumps more than kTurnPx (against the
// step before, and more than 8 times as far) is a ticker wrapping round: 373 px in one
// step was seen on the HUD's.
constexpr double kSnapPx = 160.0;
constexpr double kTurnPx = 40.0;
// A move of the matrix's scale or skew terms by more than this is a flip, not a glide.
constexpr double kScaleJump = 0.75;

uint64_t NowUs() {
    return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
}

uint8_t U8(const uint8_t* base, uint32_t a) { return a < 0x10000 ? 0 : base[a]; }
uint16_t U16(const uint8_t* base, uint32_t a) {
    return a < 0x10000 ? 0 : uint16_t((uint16_t(base[a]) << 8) | base[a + 1]);
}
float F32(const uint8_t* base, uint32_t a) { return a < 0x10000 ? 0.0f : ReadGuestF32(base, a); }
void PutF32(uint8_t* base, uint32_t a, float v) {
    if (a >= 0x10000) WriteGuestF32(base, a, v);
}

// What the movie step tells the draw, per swf context.
struct StepState {
    std::atomic<uint32_t> context{0};
    std::atomic<uint32_t> serial{0};     // steps of this context noted so far
    std::atomic<uint32_t> period_us{0};  // the nominal time between its steps
    std::atomic<uint64_t> step_us{0};    // when the last step belongs on its schedule
    std::atomic<uint32_t> frame_us{0};   // the time between updates (smoothed)
    std::atomic<uint32_t> calls{0};      // updates that reached the step
    std::atomic<uint64_t> last_call_us{0};
};
std::array<StepState, 32> g_step;
size_t g_step_next = 0;  // the slot the next new context takes (the movie step's thread only)

StepState* FindStep(uint32_t context) {
    for (StepState& s : g_step)
        if (s.context.load(std::memory_order_acquire) == context) return &s;
    return nullptr;
}

StepState& SlotFor(uint32_t context) {
    if (StepState* s = FindStep(context)) return *s;
    StepState& s = g_step[g_step_next++ % g_step.size()];
    s.context.store(0, std::memory_order_relaxed);
    s.serial.store(0, std::memory_order_relaxed);
    s.period_us.store(0, std::memory_order_relaxed);
    s.frame_us.store(0, std::memory_order_relaxed);
    s.last_call_us.store(0, std::memory_order_relaxed);
    s.context.store(context, std::memory_order_release);
    return s;
}

// The player's arithmetic (sub_825FB348 at 0x825FB38C-0x825FB454): world = local x parent
// world, every product rounded to single as the recompiled code does.
void ComposeWorld(const float l[6], const float p[6], float w[6]) {
    auto f = [](double v) { return float(v); };
    w[0] = f(double(l[0]) * p[0] + f(double(l[1]) * p[2]));
    w[1] = f(double(l[0]) * p[1] + f(double(l[1]) * p[3]));
    w[2] = f(double(p[0]) * l[2] + f(double(l[3]) * p[2]));
    w[3] = f(double(p[3]) * l[3] + f(double(p[1]) * l[2]));
    w[4] = f(f(double(p[0]) * l[4] + f(double(l[5]) * p[2])) + p[4]);
    w[5] = f(f(double(p[3]) * l[5] + f(double(p[1]) * l[4])) + p[5]);
}

// sub_825F8B20: world colour transform = local x parent world x tint. Bytes 0-3
// multiply (signed, 64 = 1), bytes 4-7 add; the tint is the parent block's four bytes at
// +68.
void CombineCxform(const int8_t local[8], const uint8_t parent[8], const uint8_t tint[4],
                   uint8_t out[8]) {
    for (int i = 0; i < 4; i++) {
        const int pm = int8_t(parent[i]);
        out[4 + i] = uint8_t(((int(local[4 + i]) * pm) >> 6) + int(parent[4 + i]));
        const int t = int8_t(uint8_t((int(local[i]) * pm) >> 6));
        out[i] = uint8_t((t * int(tint[i])) / 255);
    }
}

// One display-list node the draw has met.
struct Inst {
    uint32_t ctx = 0, def = 0, parent = 0;
    uint16_t depth = 0, charid = 0;
    uint32_t serial = 0;  // the step serial at the last draw of it
    uint64_t t0_us = 0;   // when the glide in progress started (the step it was seen at)
    uint64_t seen_us = 0;
    uint32_t changed_serial = 0;      // the step that last changed its matrix or colour transform
    double last_dx = 0, last_dy = 0;  // how far that step moved it (tx, ty)
    float from_m[6]{}, to_m[6]{}, shown_m[6]{};
    int8_t from_c[8]{}, to_c[8]{}, shown_c[8]{};
};

// One time for all the nodes of a frame (and of the passes that draw it more than once):
// the update it belongs to and the time of its first node. Else a mask and what it
// masks, drawn a moment apart, would glide a fraction of a pixel apart.
struct FrameTime {
    uint32_t calls = 0;
    uint64_t now_us = 0;
};

std::mutex g_inst_mutex;
std::unordered_map<uint32_t, Inst> g_inst;
uint64_t g_inst_sweep_us = 0;
std::array<FrameTime, 32> g_frame_time;
}  // namespace

// ui.cpp: the movie with this swf context came up for a step in this update (stepped or
// held back). Keeps the update time, which tells the draw whether it is worth blending.
void UiSmoothTouch(uint32_t context, int64_t now_ns) {
    if (!context) return;
    StepState& s = SlotFor(context);
    const uint64_t now_us = uint64_t(now_ns / 1000);
    s.calls.fetch_add(1, std::memory_order_relaxed);
    const uint64_t last = s.last_call_us.load(std::memory_order_relaxed);
    if (last && now_us > last && now_us - last < 250'000) {
        const uint32_t d = uint32_t(now_us - last);
        const uint32_t old = s.frame_us.load(std::memory_order_relaxed);
        s.frame_us.store(old ? (old * 7 + d) / 8 : d, std::memory_order_relaxed);
    }
    s.last_call_us.store(now_us, std::memory_order_relaxed);
}

// ui.cpp: the movie with this swf context is about to take a step (one-frame mode steps
// exactly once per call that is let through). step_ns is when the step belongs on its
// schedule, period_ns the time between steps.
void UiSmoothNoteStep(uint32_t context, int64_t step_ns, int64_t period_ns) {
    if (!context) return;
    StepState& s = SlotFor(context);
    s.period_us.store(uint32_t(period_ns / 1000), std::memory_order_relaxed);
    s.step_us.store(uint64_t(step_ns / 1000), std::memory_order_relaxed);
    s.serial.fetch_add(1, std::memory_order_release);
}

// Mid-asm hook at 0x825FB484 in sub_825FB348 (larecomp_config.toml). r30 = the node,
// r31 = the parent's state block. Runs on the draw thread for every node of every movie.
void Hook_UiSmoothDraw(PPCRegister& r30, PPCRegister& r31) {
    if (!REXCVAR_GET(ui_interpolate)) return;
    auto* rt = rex::Runtime::instance();
    uint8_t* base = rt ? rt->virtual_membase() : nullptr;
    if (!base) return;
    // Nodes of our own (swf_dom.cpp) are placed by LARecomp's code, not by a step.
    if (larecomp::swf::DrawingRaw()) return;

    const uint32_t inst = r30.u32;
    const uint32_t parent_s = r31.u32;
    const uint32_t ctx = ReadGuestU32(base, kUiCurrentContext);
    if (inst < 0x10000 || parent_s < 0x10000 || !ctx) return;
    const StepState* step = FindStep(ctx);
    if (!step) return;  // a movie ui.cpp does not pace (the 3D icons, the logos): drawn as it is
    const uint32_t serial = step->serial.load(std::memory_order_acquire);
    const uint32_t period_us = step->period_us.load(std::memory_order_relaxed);
    const uint64_t step_us = step->step_us.load(std::memory_order_relaxed);
    const uint32_t frame_us = step->frame_us.load(std::memory_order_relaxed);
    if (!serial || !period_us) return;
    // The step that hides the UI around a photo must show as it is.
    if (PhotoCaptureInProgress(base) || PhotoCityReturning()) return;
    // Worth doing only when several frames are drawn per step; else it would only add
    // delay. The state is tracked anyway so that it is current when the frame rate rises.
    // (A frame longer than 0.6 of a step is drawn as the step left it.)
    const bool apply = frame_us && uint64_t(frame_us) * 10 <= uint64_t(period_us) * 6;
    const uint32_t calls = step->calls.load(std::memory_order_relaxed);
    const uint64_t window_us = period_us;

    float live_m[6];
    int8_t live_c[8];
    for (int k = 0; k < 6; k++) live_m[k] = F32(base, inst + 8 + 4 * k);
    for (int k = 0; k < 8; k++) live_c[k] = int8_t(U8(base, inst + 56 + k));
    float parent_w[6];
    uint8_t parent_c[8], tint[4];
    for (int k = 0; k < 6; k++) parent_w[k] = F32(base, parent_s + 24 + 4 * k);
    for (int k = 0; k < 8; k++) parent_c[k] = U8(base, parent_s + 56 + k);
    for (int k = 0; k < 4; k++) tint[k] = U8(base, parent_s + 68 + k);

    std::lock_guard<std::mutex> lock(g_inst_mutex);
    if (g_inst.bucket_count() < 4096) {
        g_inst.reserve(4096);  // a menu opening adds a few hundred nodes at once: no rehash mid-frame
    }
    FrameTime& frame_time = g_frame_time[size_t(step - g_step.data()) % g_frame_time.size()];
    const uint64_t real_now = NowUs();
    if (frame_time.calls != calls || real_now - frame_time.now_us > 6000) {
        frame_time = {calls, real_now};
    }
    const uint64_t now = frame_time.now_us;
    if (real_now - g_inst_sweep_us > 2'000'000) {
        g_inst_sweep_us = real_now;
        for (auto it = g_inst.begin(); it != g_inst.end();) {
            it = real_now - it->second.seen_us > 5'000'000 ? g_inst.erase(it) : std::next(it);
        }
    }

    const uint32_t def = ReadGuestU32(base, inst + 4);
    const uint16_t depth = U16(base, inst + 166);
    const uint16_t charid = U16(base, inst + 168);
    const uint32_t parent = ReadGuestU32(base, inst + 184);
    Inst& e = g_inst[inst];
    const bool fresh = e.ctx != ctx || e.def != def || e.depth != depth || e.charid != charid ||
                       e.parent != parent;
    if (fresh) {
        // New, or another character at this place: shown as it is.
        e = {};
        e.ctx = ctx;
        e.def = def;
        e.depth = depth;
        e.charid = charid;
        e.parent = parent;
        e.serial = serial;
        e.t0_us = step_us;
        std::memcpy(e.from_m, live_m, sizeof live_m);
        std::memcpy(e.to_m, live_m, sizeof live_m);
        std::memcpy(e.shown_m, live_m, sizeof live_m);
        std::memcpy(e.from_c, live_c, sizeof live_c);
        std::memcpy(e.to_c, live_c, sizeof live_c);
        std::memcpy(e.shown_c, live_c, sizeof live_c);
    } else if (std::memcmp(live_m, e.to_m, sizeof live_m) != 0 ||
               std::memcmp(live_c, e.to_c, sizeof live_c) != 0) {
        // A step changed this node (or something else did): glide from what was shown to
        // the new values, unless steps went by without a draw of it (then nothing to glide
        // from).
        const bool gap = serial - e.serial > 1;
        // How far this step moved the node (against the last step's value, not against
        // what was shown).
        const double dx = double(live_m[4]) - e.to_m[4], dy = double(live_m[5]) - e.to_m[5];
        const double step_move = std::hypot(dx, dy);
        // Whether the step before this one moved it too, and the other way: the mark of a
        // wrap (a ticker that has run off one end and comes in at the other), which is a
        // cut and not a movement.
        const bool followed = e.changed_serial && serial - e.changed_serial == 1 && !gap;
        const double prev_dx = e.last_dx, prev_dy = e.last_dy;
        const double last_move = std::hypot(prev_dx, prev_dy);
        const bool turned = followed && step_move > kTurnPx && dx * prev_dx + dy * prev_dy < 0 &&
                            step_move > 8.0 * last_move;
        std::memcpy(e.from_m, gap ? live_m : e.shown_m, sizeof live_m);
        std::memcpy(e.from_c, gap ? live_c : e.shown_c, sizeof live_c);
        std::memcpy(e.to_m, live_m, sizeof live_m);
        std::memcpy(e.to_c, live_c, sizeof live_c);
        e.t0_us = step_us;
        e.changed_serial = serial;
        e.last_dx = dx;
        e.last_dy = dy;
        // A cut, a wrap or a flip is not a movement: shown at once.
        double scale_jump = 0;
        for (int k = 0; k < 4; k++) {
            scale_jump = std::max(scale_jump, std::fabs(double(e.to_m[k]) - double(e.from_m[k])));
        }
        const bool jumped = std::fabs(double(e.to_m[4]) - e.from_m[4]) > kSnapPx ||
                            std::fabs(double(e.to_m[5]) - e.from_m[5]) > kSnapPx;
        if (jumped || turned || scale_jump > kScaleJump) {
            std::memcpy(e.from_m, live_m, sizeof live_m);
        }
    }
    e.serial = serial;
    e.seen_us = real_now;

    double alpha = 1.0;
    if (now < e.t0_us) {
        alpha = 0.0;
    } else if (now - e.t0_us < window_us) {
        alpha = double(now - e.t0_us) / double(window_us);
    }
    bool m_diff = false, c_diff = false;
    for (int k = 0; k < 6; k++) {
        e.shown_m[k] = alpha >= 1.0
                           ? e.to_m[k]
                           : float(double(e.from_m[k]) + (double(e.to_m[k]) - e.from_m[k]) * alpha);
        m_diff |= e.shown_m[k] != live_m[k];
    }
    for (int k = 0; k < 8; k++) {
        e.shown_c[k] = alpha >= 1.0 ? e.to_c[k]
                                    : int8_t(std::lround(double(e.from_c[k]) +
                                                         (double(e.to_c[k]) - e.from_c[k]) * alpha));
        c_diff |= e.shown_c[k] != live_c[k];
    }
    if (!apply || (!m_diff && !c_diff)) return;
    if (m_diff) {
        float w[6];
        ComposeWorld(e.shown_m, parent_w, w);
        for (int k = 0; k < 6; k++) PutF32(base, inst + 32 + 4 * k, w[k]);
    }
    if (c_diff) {
        uint8_t cx[8];
        CombineCxform(e.shown_c, parent_c, tint, cx);
        for (int k = 0; k < 8; k++) base[inst + 64 + k] = cx[k];
    }
}

#endif  // REXGLUE_HAS_XEO3_TARGET
