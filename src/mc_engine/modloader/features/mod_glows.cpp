#ifndef REXGLUE_HAS_XEO3_TARGET
//
// Light glows a mod places in the world, fed to MCLA's own glow renderer.
//
// MCLA draws a lamp's glow for props that carry one; lights a mod bakes into
// its city sectors have no prop behind them. Such a mod ships
//
//   <exe>/models/<mod>/glows.bin
//
// and this submits the glows near the camera through the same call the prop
// glows use, right after MCLA's own prop glows each frame: depth-tested,
// distance-faded and lit only in the lamp hours, like the props'. A glow can
// follow a breakable prop of the same mod (mod_breakables): it goes out while
// that prop lies knocked over.
//
// glows.bin, little-endian: 'MGLW' u32 version(1) u32 count u32 record size (32),
// then per glow f32 x y z, f32 size and intensity multipliers, u32 colour
// 0x00RRGGBB, u16 flags (1 = flashing), u16 phase, u32 breakable id it follows
// (0xFFFFFFFF = none).
//
// RE map (default.xex):
//
//   sub_82729438(propmgr, star)   the prop manager's glow pass, called by its
//                                 render class with star 1 (star glows) and 0
//                                 (glows). With 0, for each glow point of each
//                                 active prop of kind 4/5, in the lamp hours
//                                 (time > on - j or time < off + j, j a per-prop
//                                 jitter (inst & 0xF0) * 0.00104167 h):
//     fade = 1 - clamp((dist - (draw - 110)) / 30, 0, 1)
//     slot = sub_822FC250(argb, &size[2], fade * flt_827E2124, flt_827E2128,
//                         -, &pos, slot, uid, 0, 1)   with v1 = pos too
//   sub_822FC250                  AddGlow: 96 glows a frame; `slot` keeps the
//                                 glow's depth-tap history between frames
//                                 (0x60+ = untracked), `uid` must stay the same
//                                 for the same glow. Returns the slot.
//   dword_8287E26C                mcLightingManager: +14540 time of day,
//                                 +14546 garage lighting, +14564 lamps off,
//                                 +14568 lamps on (6 and 18 in TODSunNice)
//   dword_8287E064 +393           lamps always on (debug)
//   dword_82839704 +368           camera position (current viewport)
//   dword_828D4CE4 +8             prop draw distance
//   flt_827E25D0/D4               glow size (2, 2); flt_827E2124 intensity (12);
//                                 flt_827E2128 the fourth argument (12)
//
// Registers of a guest vector are stored element-reversed (lvx128 is a 16-byte
// reverse): v.f32[3] = x, [2] = y, [1] = z, [0] = w.
//

#include <rex/cvar.h>
#include <rex/runtime.h>
#include <rex/ppc/context.h>
#include <rex/ppc/function.h>
#include <rex/system/function_dispatcher.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <mutex>
#include <vector>

#include "../../logging.h"
#include "mod_breakables.h"
#include "mod_data.h"
#include "mod_glows.h"

REXCVAR_DEFINE_BOOL(mod_glows, true, "MCLA/World",
    "Light glows a mod places in the world (models/<mod>/glows.bin), drawn by MCLA's "
    "glow renderer.")
    .debug_only();
REXCVAR_DEFINE_INT32(mod_glows_max, 64, "MCLA/World",
    "Mod glows submitted per frame, nearest first. MCLA draws 96 a frame in all, "
    "car lights included.")
    .debug_only();
REXCVAR_DEFINE_DOUBLE(mod_glow_size, 1.8, "MCLA/World",
    "Mod glow size multiplier (on top of each glow's own).")
    .debug_only();
REXCVAR_DEFINE_DOUBLE(mod_glow_intensity, 1.0, "MCLA/World", "Mod glow intensity multiplier.")
    .debug_only();

REX_EXTERN(__imp__rex_sub_82729438);
REX_EXTERN(rex_sub_822FC250);

namespace {

constexpr uint32_t kLightMgrPtr = 0x8287E26C;
constexpr uint32_t kTimeOfDayAt = 14540;
constexpr uint32_t kGarageLightAt = 14546;
constexpr uint32_t kLampsOffAt = 14564;
constexpr uint32_t kLampsOnAt = 14568;
constexpr uint32_t kDebugPtr = 0x8287E064;
constexpr uint32_t kLampsAlwaysAt = 393;
constexpr uint32_t kViewportPtr = 0x82839704;
constexpr uint32_t kCameraPosAt = 368;
constexpr uint32_t kPropDrawPtr = 0x828D4CE4;
constexpr uint32_t kGlowSize = 0x827E25D0;
constexpr uint32_t kGlowIntensity = 0x827E2124;
constexpr uint32_t kGlowArg4 = 0x827E2128;
constexpr uint32_t kGuestMallocFn = 0x82130528;
constexpr uint32_t kUidBase = 0x7E000000;
constexpr float kJitterHours = 0.00104167f;
constexpr double kBlinkPeriod = 1.2;  // seconds, flashing glows
constexpr uint32_t kMagic = 0x574C474D;  // 'MGLW'
constexpr uint32_t kNoLink = 0xFFFFFFFFu;
constexpr char kFileName[] = "glows.bin";

#pragma pack(push, 1)
struct Glow {
    float x, y, z;
    float size, intensity;
    uint32_t rgb;
    uint16_t flags, phase;
    uint32_t link;
};
#pragma pack(pop)
static_assert(sizeof(Glow) == 32);
constexpr uint16_t kFlashing = 1;

std::once_flag g_load_once;
std::vector<Glow> g_glows;
std::vector<int> g_glow_mod;
std::vector<uint32_t> g_slots;
std::vector<std::pair<float, uint32_t>> g_near;
std::atomic<uint32_t> g_scratch{0};  // guest: position (16) + size (8)
const auto g_t0 = std::chrono::steady_clock::now();

uint32_t ReadBE32(uint8_t* base, uint32_t ea) {
    const uint8_t* p = base + ea;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

float ReadF32(uint8_t* base, uint32_t ea) {
    const uint32_t bits = ReadBE32(base, ea);
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

void WriteF32(uint8_t* base, uint32_t ea, float f) {
    uint32_t v;
    std::memcpy(&v, &f, sizeof(v));
    base[ea + 0] = uint8_t(v >> 24);
    base[ea + 1] = uint8_t(v >> 16);
    base[ea + 2] = uint8_t(v >> 8);
    base[ea + 3] = uint8_t(v);
}

void Load() {
    for (const ModDataFile& file : FindModDataFiles(kFileName)) {
        std::ifstream in(file.path, std::ios::binary);
        uint32_t hdr[4] = {};
        if (!in.read(reinterpret_cast<char*>(hdr), sizeof(hdr)) || hdr[0] != kMagic || hdr[1] != 1 ||
            hdr[3] != sizeof(Glow) || hdr[2] > 200000) {
            MC_WARN("[mod-glows] {}: not a glows file", file.path.string());
            continue;
        }
        const size_t at = g_glows.size();
        g_glows.resize(at + hdr[2]);
        if (!in.read(reinterpret_cast<char*>(g_glows.data() + at), std::streamsize(sizeof(Glow) * hdr[2]))) {
            g_glows.resize(at);
            MC_WARN("[mod-glows] {}: truncated", file.path.string());
            continue;
        }
        g_glow_mod.resize(g_glows.size(), file.mod);
        MC_INFO("[mod-glows] {}: {} glows", file.mod_name, hdr[2]);
    }
    g_slots.assign(g_glows.size(), 0xFFFFFFFFu);
}

// One glow through AddGlow, on a fresh context below the caller's frame (what
// GuestToHostFunction does, plus the position in v1, which it cannot pass).
uint32_t AddGlow(PPCContext& ctx, uint8_t* base, uint32_t scratch, const Glow& g, uint32_t slot,
                 uint32_t uid, float intensity, float sx, float sy) {
    WriteF32(base, scratch + 0, g.x);
    WriteF32(base, scratch + 4, g.y);
    WriteF32(base, scratch + 8, g.z);
    WriteF32(base, scratch + 12, 1.0f);
    WriteF32(base, scratch + 16, sx);
    WriteF32(base, scratch + 20, sy);

    PPCContext c{};
    c.r1 = ctx.r1;
    c.r1.u32 -= 0x70;
    c.r13 = ctx.r13;
    c.fpscr = ctx.fpscr;
    c.r3.u64 = 0xFF000000u | g.rgb;
    c.r4.u64 = scratch + 16;
    c.r5.u64 = 0;
    c.r6.u64 = scratch;
    c.r7.u64 = slot;
    c.r8.u64 = uid;
    c.r9.u64 = 0;
    c.r10.u64 = 1;
    c.f1.f64 = intensity;
    c.f2.f64 = ReadF32(base, kGlowArg4);
    c.v1.f32[3] = g.x;
    c.v1.f32[2] = g.y;
    c.v1.f32[1] = g.z;
    c.v1.f32[0] = 1.0f;
    rex_sub_822FC250(c, base);
    ctx.fpscr = c.fpscr;
    return c.r3.u32;
}

void SubmitGlows(PPCContext& ctx, uint8_t* base) {
    std::call_once(g_load_once, Load);
    const uint32_t scratch = g_scratch.load(std::memory_order_acquire);
    if (g_glows.empty() || !scratch) return;

    const uint32_t mgr = ReadBE32(base, kLightMgrPtr);
    const uint32_t vp = ReadBE32(base, kViewportPtr);
    const uint32_t draw_obj = ReadBE32(base, kPropDrawPtr);
    if (!mgr || !vp || !draw_obj) return;
    if (base[mgr + kGarageLightAt]) return;

    const float tod = ReadF32(base, mgr + kTimeOfDayAt);
    const float on = ReadF32(base, mgr + kLampsOnAt) - 0.1f;
    const float off = ReadF32(base, mgr + kLampsOffAt) + 0.1f;
    const uint32_t dbg = ReadBE32(base, kDebugPtr);
    const bool always = dbg && base[dbg + kLampsAlwaysAt];
    if (!always && !(tod > on - 0.3f || tod < off + 0.3f)) return;  // daytime: nothing to light
    const float draw = ReadF32(base, draw_obj + 8);
    if (!(draw > 0.0f)) return;
    const float fade_start = draw - 110.0f;

    const float cx = ReadF32(base, vp + kCameraPosAt + 0);
    const float cy = ReadF32(base, vp + kCameraPosAt + 4);
    const float cz = ReadF32(base, vp + kCameraPosAt + 8);
    g_near.clear();
    const float draw2 = draw * draw;
    for (uint32_t i = 0; i < g_glows.size(); ++i) {
        const Glow& g = g_glows[i];
        const float dx = g.x - cx, dy = g.y - cy, dz = g.z - cz;
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < draw2) g_near.emplace_back(d2, i);
    }
    const size_t want = size_t(std::clamp(REXCVAR_GET(mod_glows_max), 0, 96));
    if (g_near.size() > want) {
        std::nth_element(g_near.begin(), g_near.begin() + want, g_near.end());
        g_near.resize(want);
    }

    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_t0).count();
    const float size_mul = float(REXCVAR_GET(mod_glow_size));
    const float glow_i = ReadF32(base, kGlowIntensity) * float(REXCVAR_GET(mod_glow_intensity));
    const float sx = ReadF32(base, kGlowSize + 0) * size_mul;
    const float sy = ReadF32(base, kGlowSize + 4) * size_mul;

    for (const auto& [d2, i] : g_near) {
        const Glow& g = g_glows[i];
        if (g.link != kNoLink && ModBreakableDown(g_glow_mod[i], g.link)) continue;  // its lamp lies on the ground
        const float jitter = float(g.phase & 0xF0) * kJitterHours;
        if (!always && !(tod > on - jitter || tod < off + jitter)) continue;
        if (g.flags & kFlashing) {
            const double ph = double(g.phase) / 65536.0 * kBlinkPeriod;
            if (std::fmod(t + ph, kBlinkPeriod) > kBlinkPeriod * 0.5) continue;
        }
        const float dist = std::sqrt(d2);
        const float fade = 1.0f - std::clamp((dist - fade_start) / 30.0f, 0.0f, 1.0f);
        g_slots[i] = AddGlow(ctx, base, scratch, g, g_slots[i], kUidBase + i, fade * glow_i * g.intensity,
                             sx * g.size, sy * g.size);
    }
}

}  // namespace

// The prop manager's glow pass; the mods' glows go in after MCLA's own.
extern "C" REX_FUNC(rex_sub_82729438) {
    const bool star_pass = (ctx.r4.u32 & 0xFF) != 0;
    __imp__rex_sub_82729438(ctx, base);
    if (star_pass || !REXCVAR_GET(mod_glows)) return;
    const uint64_t ret = ctx.r3.u64;
    SubmitGlows(ctx, base);
    ctx.r3.u64 = ret;
}

void TickModGlows() {
    if (g_scratch.load(std::memory_order_relaxed) || !REXCVAR_GET(mod_glows)) return;
    auto* rt = rex::Runtime::instance();
    PPCFunc* malloc_fn = rt && rt->function_dispatcher() ? rt->function_dispatcher()->GetFunction(kGuestMallocFn)
                                                        : nullptr;
    if (!malloc_fn || !rt->virtual_membase()) return;
    const uint32_t raw = rex::ppc::GuestToHostFunction<uint32_t>(malloc_fn, 64u);
    if (raw) g_scratch.store((raw + 15) & ~15u, std::memory_order_release);
}

#else  // REXGLUE_HAS_XEO3_TARGET

void TickModGlows() {}

#endif  // REXGLUE_HAS_XEO3_TARGET
