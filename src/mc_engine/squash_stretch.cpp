#ifndef REXGLUE_HAS_XEO3_TARGET
#include "squash_stretch.h"

#include "logging.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/ppc/function.h>
#include <rex/runtime.h>

// ─────────────────────────────────────────────────────────────────────────
// "Why's This Dealer?" squash & stretch — visual only, no physics contact.
//
// Reverse engineering (default.xex):
//
//   sub_82360518          mcVehicle render dispatch. r3 = vehicle, and every
//                         vehicle draw path in the game funnels through here
//                         (world sub_8235FB78, garage/reflection sub_8235FA90,
//                         sub_8235F738). dword_8288DCF8 = the vehicle the local
//                         player drives; this function compares against it
//                         itself at 0x823607F4.
//
//   sub_823236C8(a1,a2,a3,a4)
//                         Builds the skinning palette for one drawable:
//                           a1 = model, a2 = bone matrix array (Matrix44,
//                           stride 0x40, world space), a3 = scratch,
//                           a4 = OPTIONAL extra transform matrix.
//                         It forwards a4 to sub_823788C8.
//
//   sub_823788C8          Uploads the palette to the vertex shader constant
//                         array named "gCarMtxBuffer" (handle dword_8288E044).
//                         When the extra matrix E (r30) is non-null it computes,
//                         per bone B (rows b0..b3 at +0/16/32/48):
//
//                           row_i = b_i.x*E0 + b_i.y*E1 + b_i.z*E2  (+E3 for i=3)
//
//                         i.e. B · E in row-vector convention — a POST multiply
//                         in world space. Verified in the asm at 0x823789B0..
//                         0x82378A04 (vmaddfp vD, vA, vC, vB => vD = vA*vC + vB).
//
//                         It then transposes to 3x4 and subtracts the root
//                         translation (a2+0x30) for precision, adding it back
//                         through the world matrix set in sub_823236C8's tail.
//                         That subtract/re-add cancels, so E does not have to
//                         preserve the root translation.
//
// So the engine already ships the exact lever the CarbonBounce NFS:C mod has to
// fake by hooking D3D: a per-draw extra transform folded into every bone matrix
// before the GPU sees it. The shipped code only uses it for the rigid part path
// (sub_82324258 -> sub_823240B8); everything else passes 0. We fill that 0 in.
//
// Consequences of doing it at this exact point:
//   * nothing in guest memory is written — the matrix we hand over is ours, in
//     our own scratch buffer, so collision, suspension, AI and the camera keep
//     running on the untouched car. Same guarantee BeamNG gets by scaling only
//     the render nodes.
//   * every consumer of the car palette gets it: body, all 55 parts, wheels
//     (sub_8235E0E0), shadow/depth (sub_8235EEF8) and the mirror/reflection
//     passes, because they all reach the palette through sub_823236C8.
//
// Known limits (deliberate, not bugs):
//   * culling and the shadow bounds still use the unsquashed bounding box, so a
//     very wide car can pop at the screen edge. Keep squash_amplitude sane.
//   * the collision hull never changes, so at big amplitudes the visual body
//     sinks into the road while the car still rides on the stock hull. That is
//     the meme, and it is why squash_pivot exists.
//   * headlight/exhaust effects are separate emitters positioned from the real
//     matrices, so they stay at stock height.
// ─────────────────────────────────────────────────────────────────────────

REXCVAR_DEFINE_BOOL(squash_stretch, false, "MCLA/Fun",
    "Squash & stretch the player's car like the \"Why's This Dealer?\" meme. "
    "Visual only — physics and collision are untouched.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(squash_amplitude, 0.25, "MCLA/Fun",
    "Squash depth, 0..0.9. 0.25 = the body loses a quarter of its height at the "
    "bottom of the cycle and gains half of that in width.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(squash_hz, 1.5, "MCLA/Fun",
    "Oscillations per second.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(squash_pivot, 0.0, "MCLA/Fun",
    "Pivot height in metres along the up axis, in car space. 0 = scale about the "
    "car origin (the body sinks and rises). Negative values push the pivot down "
    "toward the tyre contact patch so the car keeps its ground contact.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(squash_up_axis, 2, "MCLA/Fun",
    "Which car-space axis is up: 0 = X, 1 = Y, 2 = Z (RAGE default). The other "
    "two axes take the anti-phase widening.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(squash_traffic, false, "MCLA/Fun",
    "Apply the squash to every car in the world instead of just the player's.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

// dword_8288DCF8 — the vehicle the local player is driving.
constexpr uint32_t kPlayerVehicle = 0x8288DCF8;
// vehicle+0x14 = the entity; entity+0x90 is its world Matrix44 and entity+0xD0
// the alternate (previous-frame) one. Read off the rigid part draw at
// 0x8235DF78: `lwz r11, 0x14(r31); addi r5, r11, 0xD0; ... addi r5, r11, 0x90`.
constexpr uint32_t kEntityOffset = 0x14;
constexpr uint32_t kEntityMatrix = 0x90;
// The game's allocator, same entry point pause_menu.cpp uses for guest strings.
constexpr uint32_t kGuestMallocFn = 0x82130528;

struct Mat4 {
    float m[4][4];
};

// ── guest memory (big endian) ────────────────────────────────────────

inline uint32_t Bswap32(uint32_t v) {
    return (v >> 24) | ((v >> 8) & 0x0000FF00u) | ((v << 8) & 0x00FF0000u) | (v << 24);
}

inline float GuestReadF32(const uint8_t* base, uint32_t ea) {
    uint32_t raw;
    std::memcpy(&raw, base + ea, 4);
    raw = Bswap32(raw);
    float f;
    std::memcpy(&f, &raw, 4);
    return f;
}

inline void GuestWriteF32(uint8_t* base, uint32_t ea, float f) {
    uint32_t raw;
    std::memcpy(&raw, &f, 4);
    raw = Bswap32(raw);
    std::memcpy(base + ea, &raw, 4);
}

inline uint32_t GuestReadU32(const uint8_t* base, uint32_t ea) {
    uint32_t raw;
    std::memcpy(&raw, base + ea, 4);
    return Bswap32(raw);
}

bool IsGuestPtr(uint32_t ea) {
    // Code/data live at 0x82xxxxxx, heap objects far above; anything below the
    // XEX base is either null or a garbage read.
    return ea >= 0x1000u && ea < 0xFFFFFFF0u;
}

// RAGE matrices are Matrix34: four 16-byte rows whose 4th float is PADDING and
// carries garbage. The engine masks it off every time it consumes one —
// sub_8217C088 stores a second copy at state+128 with the w of rows 0..2 ANDed
// away (unk_82860950) and row 3's forced to 1 (unk_82860930), and the palette
// upload only ever reads x/y/z. A host 4x4 multiply that keeps that padding
// bleeds it into the rotation and throws the geometry to infinity, which reads
// on screen as the part simply vanishing. So: clean on read, keep it clean.
Mat4 ReadGuestMat4(const uint8_t* base, uint32_t ea) {
    Mat4 r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r.m[i][j] = GuestReadF32(base, ea + uint32_t(i * 16 + j * 4));
    r.m[0][3] = 0.0f;
    r.m[1][3] = 0.0f;
    r.m[2][3] = 0.0f;
    r.m[3][3] = 1.0f;
    return r;
}

// Rejects anything that would move geometry out of the world instead of
// deforming it — a degenerate matrix is how a part disappears.
bool IsSane(const Mat4& m) {
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            if (!std::isfinite(m.m[i][j]) || std::fabs(m.m[i][j]) > 1e9f) return false;
    return true;
}

void WriteGuestMat4(uint8_t* base, uint32_t ea, const Mat4& v) {
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            GuestWriteF32(base, ea + uint32_t(i * 16 + j * 4), v.m[i][j]);
}

// ── row-vector matrix math (p' = p * M, same convention as the engine) ──

Mat4 Mul(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j]
                      + a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
    return r;
}

// General affine inverse. The car root is rigid in practice, but a general
// inverse costs nothing here and survives a car that carries scale.
bool Invert(const Mat4& w, Mat4& out) {
    const float a = w.m[0][0], b = w.m[0][1], c = w.m[0][2];
    const float d = w.m[1][0], e = w.m[1][1], f = w.m[1][2];
    const float g = w.m[2][0], h = w.m[2][1], i = w.m[2][2];

    const float A = e * i - f * h;
    const float B = -(d * i - f * g);
    const float C = d * h - e * g;
    const float det = a * A + b * B + c * C;
    if (std::fabs(det) < 1e-12f) return false;
    const float inv = 1.0f / det;

    out = Mat4{};
    out.m[0][0] = A * inv;
    out.m[0][1] = -(b * i - c * h) * inv;
    out.m[0][2] = (b * f - c * e) * inv;
    out.m[1][0] = B * inv;
    out.m[1][1] = (a * i - c * g) * inv;
    out.m[1][2] = -(a * f - c * d) * inv;
    out.m[2][0] = C * inv;
    out.m[2][1] = -(a * h - b * g) * inv;
    out.m[2][2] = (a * e - b * d) * inv;

    const float tx = w.m[3][0], ty = w.m[3][1], tz = w.m[3][2];
    out.m[3][0] = -(tx * out.m[0][0] + ty * out.m[1][0] + tz * out.m[2][0]);
    out.m[3][1] = -(tx * out.m[0][1] + ty * out.m[1][1] + tz * out.m[2][1]);
    out.m[3][2] = -(tx * out.m[0][2] + ty * out.m[1][2] + tz * out.m[2][2]);
    out.m[3][3] = 1.0f;
    return true;
}

// ── state ────────────────────────────────────────────────────────────

std::atomic<bool> g_armed{false};        // this vehicle gets the effect
std::atomic<bool> g_have_root{false};    // root matrix cached for this vehicle
Mat4 g_root{};                           // car root, world space
std::atomic<uint32_t> g_scratch{0};      // 16-byte aligned guest scratch (2 x 64 B)

// Second slot: the palette matrix is consumed inside sub_823236C8 before it
// returns, but the world matrix path runs interleaved with it, so they get
// separate buffers rather than racing over one.
constexpr uint32_t kScratchWorld = 64;

double NowSeconds() {
    using clock = std::chrono::steady_clock;
    static const clock::time_point start = clock::now();
    return std::chrono::duration<double>(clock::now() - start).count();
}

uint8_t* Membase() {
    auto* rt = rex::Runtime::instance();
    return rt ? rt->virtual_membase() : nullptr;
}

// Allocates the scratch from the game's own heap once and keeps the 16-byte
// aligned address. Both sinks load the matrix with lvx128, which masks the low
// four address bits — a misaligned buffer would be read silently wrong.
uint32_t EnsureScratch() {
    uint32_t cur = g_scratch.load(std::memory_order_acquire);
    if (cur) return cur;

    auto* rt = rex::Runtime::instance();
    if (!rt || !rt->function_dispatcher()) return 0;
    PPCFunc* fn = rt->function_dispatcher()->GetFunction(kGuestMallocFn);
    if (!fn) return 0;

    uint32_t raw = rex::ppc::GuestToHostFunction<uint32_t>(fn, 160u);
    if (!IsGuestPtr(raw)) {
        MC_WARN("[squash] guest malloc failed");
        return 0;
    }
    uint32_t aligned = (raw + 15u) & ~15u;
    g_scratch.store(aligned, std::memory_order_release);
    MC_INFO("[squash] scratch matrix at 0x{:08X}", aligned);
    return aligned;
}

// Scale factors for the current instant. The up axis loses what the other two
// gain, so the car reads as squashing rather than just shrinking.
void CurrentScale(float& sx, float& sy, float& sz, float& pivot) {
    double amp = REXCVAR_GET(squash_amplitude);
    if (amp < 0.0) amp = 0.0;
    if (amp > 0.9) amp = 0.9;

    const double phase = NowSeconds() * REXCVAR_GET(squash_hz) * 6.283185307179586;
    const double s = std::sin(phase);

    const float up = float(1.0 - amp * s);
    const float wide = float(1.0 + amp * s * 0.5);

    sx = sy = sz = wide;
    switch (REXCVAR_GET(squash_up_axis)) {
        case 0: sx = up; break;
        case 1: sy = up; break;
        default: sz = up; break;
    }
    pivot = float(REXCVAR_GET(squash_pivot));
}

// E = W⁻¹ · (scale about the car-space pivot) · W, so the world-space post
// multiply the engine does lands as a car-space scale.
bool BuildSquashMatrix(const Mat4& world, Mat4& out) {
    Mat4 winv;
    if (!Invert(world, winv)) return false;

    float sx, sy, sz, pivot;
    CurrentScale(sx, sy, sz, pivot);

    // Scale about a point on the up axis: q -> (q - p) * S + p.
    Mat4 s{};
    s.m[0][0] = sx;
    s.m[1][1] = sy;
    s.m[2][2] = sz;
    s.m[3][3] = 1.0f;
    switch (REXCVAR_GET(squash_up_axis)) {
        case 0: s.m[3][0] = pivot * (1.0f - sx); break;
        case 1: s.m[3][1] = pivot * (1.0f - sy); break;
        default: s.m[3][2] = pivot * (1.0f - sz); break;
    }

    out = Mul(Mul(winv, s), world);
    return true;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────
// Hooks
// ─────────────────────────────────────────────────────────────────────────

// sub_82360518 entry — r3 = vehicle being rendered.
void Hook_SquashSelectVehicle(PPCRegister& r3) {
    if (!REXCVAR_GET(squash_stretch)) {
        g_armed.store(false, std::memory_order_relaxed);
        return;
    }

    uint8_t* base = Membase();
    if (!base) {
        g_armed.store(false, std::memory_order_relaxed);
        return;
    }

    const uint32_t vehicle = uint32_t(r3.u64);
    const uint32_t player = GuestReadU32(base, kPlayerVehicle);
    const bool armed = IsGuestPtr(vehicle)
                    && (REXCVAR_GET(squash_traffic) || vehicle == player);

    g_armed.store(armed, std::memory_order_relaxed);
    g_have_root.store(false, std::memory_order_relaxed);
    if (!armed) return;

    // One root matrix for the whole car. Parts carry their own matrix arrays,
    // so scaling each about its own bone 0 would pull the car apart — every
    // palette this vehicle builds has to be scaled about the same point.
    const uint32_t entity = GuestReadU32(base, vehicle + kEntityOffset);
    if (IsGuestPtr(entity)) {
        Mat4 root = ReadGuestMat4(base, entity + kEntityMatrix);
        Mat4 probe;
        if (Invert(root, probe)) {   // rejects a not-yet-filled matrix
            g_root = root;
            g_have_root.store(true, std::memory_order_relaxed);
        }
    }

    EnsureScratch();
}

// sub_823236C8 entry — r4 = bone matrix array, r6 = extra transform (0 for the
// skinned path, the entity matrix for the rigid path).
void Hook_SquashInjectMatrix(PPCRegister& r4, PPCRegister& r6) {
    if (!g_armed.load(std::memory_order_relaxed)) return;

    const uint32_t bones = uint32_t(r4.u64);
    if (!IsGuestPtr(bones)) return;

    uint8_t* base = Membase();
    if (!base) return;

    const uint32_t scratch = EnsureScratch();
    if (!scratch) return;

    const uint32_t existing = uint32_t(r6.u64);

    // Fallback for the case where the entity matrix was not readable at select
    // time: take bone 0 of the first palette this vehicle builds. Less exact
    // (bone 0 of a part is the part origin) but still one shared pivot.
    if (!g_have_root.load(std::memory_order_relaxed)) {
        Mat4 root = ReadGuestMat4(base, bones);
        // The rigid path already carries an extra transform; the effective world
        // matrix is bone0 · E_existing, and that is what the squash must be
        // expressed in.
        if (IsGuestPtr(existing)) root = Mul(root, ReadGuestMat4(base, existing));
        g_root = root;
        g_have_root.store(true, std::memory_order_relaxed);
    }

    Mat4 squash;
    if (!BuildSquashMatrix(g_root, squash)) return;

    // Preserve whatever the engine wanted to do: it computes B · E, so chaining
    // is E_final = E_existing · squash.
    if (IsGuestPtr(existing))
        squash = Mul(ReadGuestMat4(base, existing), squash);

    if (!IsSane(squash)) return;

    WriteGuestMat4(base, scratch, squash);
    r6.u64 = scratch;
}

// The non-palette sinks. Not everything on a car is skinned, and what is not
// never reaches the gCarMtxBuffer palette, so the palette hook alone leaves it
// at stock size. Four call sites, all with r4 = Matrix44:
//
//   0x82322A10  set-world-if-changed, the a26==0 (no palette) branch of
//               sub_82325040 — the generic rigid geometry route.
//   0x82324148  sub_823240B8's non-skinned branch, which sets the world matrix
//               straight from the extra matrix.
//   0x8235AA90  sub_8235A628 pushing the per-wheel matrix before the TYRE draw
//               (the rims go through the palette in sub_82359F58, which is why
//               rims scaled and tyres did not).
//   0x82378B40  the EXHAUST. sub_82359C10 (part slots 23 exh_pipe / 24
//               exh_side) does not use either route: sub_82378B40 transposes a
//               single Matrix44 and uploads it into gCarMtxBuffer directly with
//               count = 1, bypassing sub_823236C8. sub_82359C10 is its only
//               caller, so hooking the whole function is safe.
//
// Post-multiplying here matches the palette exactly: both apply · E in world
// space, so skinned and rigid geometry deform identically and nothing gets
// scaled twice (the identity world matrices the skinned path sets — 0x8235D6DC,
// 0x82359E64, 0x8235FE74 — are deliberately NOT hooked; scaling those would
// square the effect on the body).
void Hook_SquashWorldMatrix(PPCRegister& r4) {
    if (!g_armed.load(std::memory_order_relaxed)) return;
    if (!g_have_root.load(std::memory_order_relaxed)) return;

    const uint32_t src = uint32_t(r4.u64);
    if (!IsGuestPtr(src)) return;

    uint8_t* base = Membase();
    if (!base) return;

    const uint32_t scratch = EnsureScratch();
    if (!scratch) return;

    Mat4 squash;
    if (!BuildSquashMatrix(g_root, squash)) return;

    const Mat4 scaled = Mul(ReadGuestMat4(base, src), squash);
    // Leave the register alone rather than hand the GPU a matrix that would
    // delete the part.
    if (!IsSane(scaled)) return;

    WriteGuestMat4(base, scratch + kScratchWorld, scaled);
    r4.u64 = scratch + kScratchWorld;
}

#else  // REXGLUE_HAS_XEO3_TARGET

#include <rex/ppc/context.h>
void Hook_SquashSelectVehicle(PPCRegister&) {}
void Hook_SquashInjectMatrix(PPCRegister&, PPCRegister&) {}
void Hook_SquashWorldMatrix(PPCRegister&) {}

#endif  // REXGLUE_HAS_XEO3_TARGET
