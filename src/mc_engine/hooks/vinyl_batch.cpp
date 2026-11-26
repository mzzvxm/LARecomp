// Car vinyl regen: several layers a frame, with the layers' shapes loaded ahead.
//
// The full vinyl regen (sub_8236F910, called once a frame by sub_822C0980,
// right after the shape library's scheduler sub_82371368) draws at most ONE
// layer per call:
//   - the current layer is wa+220 (surface) / wa+224 (index);
//   - it makes sure the layer's shape has a reference (sub_8236CF60 == 0 ->
//     sub_8236CFC0) and asks for its bitmap (sub_8236F8C0); if the shape is not
//     resident yet it returns and tries again next frame;
//   - otherwise it draws the layer into the work-area render target
//     (sub_8236EB48 with the target: lock, redraw what is there, draw the
//     layer, resolve with mips), moves to the next non-empty layer
//     (sub_8236D200) and, if the next layer's shape differs, drops the current
//     shape's reference (sub_8236D030) and takes one on the next
//     (sub_8236CFC0);
//   - when no layer is left it clears wa+248 (regen busy) and zeroes every
//     shape reference count (sub_8236D0A0).
// A shape with no reference is unloaded 6 frames later (sub_82371368, the
// 6 at 0x827E754C), and a shape that gets a reference is only requested from
// the streamer by the next frame's scheduler, ~3 frames to load. So the game
// draws one layer a frame and loads each shape again every time the layers
// come back to it: a car with 218 layers and 22 shapes changes shape 56 times
// and took 317 frames (5.4 s; tools/guestmem/vinyltrace.py).
//
// Two things here, each with its own option:
//
// vinyl_layers_per_frame: after the game's call, call it again in the same
// frame while the regen still runs and the shape of the layer it would draw
// next is resident. Each call is the unmodified game step, so the picture is
// the same; only the frame it finishes on changes.
//
// vinyl_shape_prefetch: while a regen runs, hold one extra reference on each
// of the next N distinct bitmap shapes the remaining layers use (the N with
// the nearest next use; the current layer's shape is always one of them), so
// they are loaded once, side by side, and stay loaded until no layer within
// reach needs them. The references are taken through the same counters the
// game uses (lib[49+cat][local]); the game's own reference is untouched:
//   - ours are only taken AFTER a game call, so the game's "reference the
//     current shape if it has none" has already run and it owns its own one
//     (taken before it, ours would make the game skip its own and its later
//     release would take ours);
//   - one of ours is dropped only while the count is at least 1;
//   - the game zeroes all counts at the end of a regen and when the work area
//     is torn down (sub_8236D0A0, sub_8236D8B8, both hooked below): ours are
//     then simply forgotten, never released again;
//   - if the regen stops advancing for 3 s, or the streamer reports a failed
//     allocation (pgStreamable sets streamer+6212), every one of ours is
//     dropped for the rest of that regen, which is then the game's own.
// Vector shapes (IsVector) have no streaming and are left alone.

#ifndef REXGLUE_HAS_XEO3_TARGET
#include "settings/larecomp_settings.h"
#include <rex/ppc/recompiled_code.h>
#include <rex/ppc/context.h>
#include "larecomp_guest.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <vector>

#include "hooks_internal.h"
#include "settings/immortal.h"

REX_EXTERN(__imp__rex_sub_8236F910);  // the recompiled regen step
REX_EXTERN(__imp__rex_sub_8236D0A0);  // shape library: zero every reference count
REX_EXTERN(__imp__rex_sub_8236D8B8);  // work area teardown (zeroes them too)

namespace {

constexpr uint32_t kShapeLibrary = 0x8288DFC4;  // mcCarVinylShapeLibrary*
constexpr uint32_t kScheduleStamp = 0x827E754C; // value the scheduler stamps on a referenced shape
constexpr uint32_t kSurfaceStart = 0x827E9770;  // per-surface first slot, in layers
constexpr uint32_t kStreamer = 0x8286CE50;      // pgStreamer*; +6212 = an allocation failed

constexpr uint32_t kWaSurfaceCap = 24;  // +4*s: layers per surface
constexpr uint32_t kWaSurfaces = 44;    // surface count
constexpr uint32_t kWaSurface = 220;    // current layer: surface (-1 = none)
constexpr uint32_t kWaLayer = 224;      //                index   (-1 = none)
constexpr uint32_t kWaPaint = 244;      // paint (car config owner), 0 = idle
constexpr uint32_t kWaBusy = 248;       // u8: full regen running
constexpr uint32_t kWaTeardown = 252;   // u8: work area being torn down

// Library: lib+12+4*cat shape count, lib+4*(26+cat) descriptors (16 bytes,
// +8 flags), lib+4*(49+cat) reference counts, lib+4*(72+cat) schedule stamps.
constexpr uint32_t kCategories = 23;

inline uint8_t ReadGuestU8(const uint8_t* base, uint32_t addr) { return base[addr]; }
inline uint16_t ReadGuestU16(const uint8_t* base, uint32_t addr) {
    return uint16_t((uint32_t(base[addr]) << 8) | base[addr + 1]);
}
inline bool IsGuestPtr(uint32_t ea) { return ea >= 0x10000u && ea < 0xFFFF0000u; }

struct Layer {
    uint16_t shape;
    bool vector;
};

// The layer slot of (surface, index): sub_82393118.
bool ReadLayer(const uint8_t* base, uint32_t wa, uint32_t surf, uint32_t idx, Layer& out) {
    const uint32_t paint = ReadGuestU32(base, wa + kWaPaint);
    if (!IsGuestPtr(paint)) return false;
    const uint32_t block = ReadGuestU32(base, paint + 132);
    if (!IsGuestPtr(block)) return false;
    const uint32_t slot = block + 64 + 2244 + 20 * (ReadGuestU32(base, kSurfaceStart + 4 * surf) + idx);
    out.shape = ReadGuestU16(base, slot + 16);
    out.vector = ReadGuestU8(base, slot + 19) != 0;
    return out.shape != 0xFFFF;
}

bool RegenRunning(const uint8_t* base, uint32_t wa) {
    return IsGuestPtr(wa) && !ReadGuestU8(base, wa + kWaTeardown) && ReadGuestU8(base, wa + kWaBusy) &&
           IsGuestPtr(ReadGuestU32(base, wa + kWaPaint));
}

bool CurrentLayer(const uint8_t* base, uint32_t wa, uint32_t& surf, uint32_t& idx) {
    const int32_t s = int32_t(ReadGuestU32(base, wa + kWaSurface));
    const int32_t i = int32_t(ReadGuestU32(base, wa + kWaLayer));
    if (s < 0 || s > 4 || i < 0 || i >= 64) return false;
    surf = uint32_t(s);
    idx = uint32_t(i);
    return true;
}

// Address of a bitmap shape's reference count, 0 if out of the library.
uint32_t RefCountAddr(const uint8_t* base, uint32_t lib, uint16_t shape) {
    const uint32_t cat = shape / 1000u, local = shape % 1000u;
    if (cat >= kCategories) return 0;
    if (local >= ReadGuestU32(base, lib + 12 + 4 * cat)) return 0;
    const uint32_t counts = ReadGuestU32(base, lib + 4 * (49 + cat));
    return IsGuestPtr(counts) ? counts + 4 * local : 0;
}

// Would the next call draw a layer? The same tests the game makes on the way
// to sub_8236EB48: regen running, a current layer, and its shape resident
// (sub_8236F8C0 / sub_8236E618).
bool NextLayerDrawable(const uint8_t* base, uint32_t wa) {
    if (!RegenRunning(base, wa)) return false;
    uint32_t surf, idx;
    Layer layer;
    if (!CurrentLayer(base, wa, surf, idx) || !ReadLayer(base, wa, surf, idx, layer)) return false;

    const uint32_t lib = ReadGuestU32(base, kShapeLibrary);
    if (!IsGuestPtr(lib)) return false;
    uint32_t desc;
    if (layer.vector) {
        // lib+0 = vector shape count, lib+4 = their 16-byte descriptors.
        if (int32_t(ReadGuestU32(base, lib + 0)) <= 0) return false;
        desc = ReadGuestU32(base, lib + 4) + 16u * layer.shape;
    } else {
        const uint32_t cat = layer.shape / 1000u, local = layer.shape % 1000u;
        if (cat >= kCategories) return false;
        const uint32_t stamps = ReadGuestU32(base, lib + 4 * (72 + cat));
        if (!IsGuestPtr(stamps) ||
            ReadGuestU32(base, stamps + 4 * local) != ReadGuestU32(base, kScheduleStamp))
            return false;
        desc = ReadGuestU32(base, lib + 4 * (26 + cat)) + 16u * local;
    }
    if (!IsGuestPtr(desc)) return false;
    return (ReadGuestU32(base, desc + 8) & 0x30000000u) == 0x30000000u;
}

// --- prefetch ---------------------------------------------------------------

std::atomic<uint32_t> g_wipes{0};  // bumped whenever the game zeroes the counts

struct Prefetch {
    std::mutex mu;
    std::vector<uint16_t> held;  // shapes we hold one reference on
    uint32_t wipes_seen = 0;
    uint32_t wa = 0, paint = 0, lib = 0;  // the regen the references belong to
    uint32_t progress_key = ~0u;
    std::chrono::steady_clock::time_point progress_at{};
    bool alloc_failed_at_start = false;
    bool given_up = false;
};
Prefetch& g_pf = larecomp::Leak<Prefetch>();  // never destroyed (exit audit)

constexpr auto kStallLimit = std::chrono::seconds(3);

bool StreamerAllocFailed(const uint8_t* base) {
    const uint32_t streamer = ReadGuestU32(base, kStreamer);
    return IsGuestPtr(streamer) && ReadGuestU8(base, streamer + 6212) != 0;
}

void DropOne(uint8_t* base, uint32_t lib, uint16_t shape) {
    const uint32_t a = RefCountAddr(base, lib, shape);
    if (!a) return;
    const int32_t v = int32_t(ReadGuestU32(base, a));
    if (v >= 1) WriteGuestU32(base, a, uint32_t(v - 1));
}

// Called after each game call while the regen may be running.
void SyncPrefetch(uint8_t* base, uint32_t wa) {
    std::lock_guard<std::mutex> lock(g_pf.mu);

    const uint32_t wipes = g_wipes.load(std::memory_order_acquire);
    if (wipes != g_pf.wipes_seen) {
        g_pf.held.clear();  // the game zeroed every count, ours included
        g_pf.wipes_seen = wipes;
    }
    const uint32_t lib = ReadGuestU32(base, kShapeLibrary);
    if (!RegenRunning(base, wa) || !IsGuestPtr(lib)) {
        // Regen over: sub_8236D0A0 zeroed the counts on the way out.
        g_pf.held.clear();
        g_pf.wa = 0;
        return;
    }
    const uint32_t paint = ReadGuestU32(base, wa + kWaPaint);
    if (wa != g_pf.wa || paint != g_pf.paint || lib != g_pf.lib) {
        // A different regen (or library): whatever we held there is not ours
        // to release any more.
        g_pf.held.clear();
        g_pf.wa = wa;
        g_pf.paint = paint;
        g_pf.lib = lib;
        g_pf.progress_key = ~0u;
        g_pf.alloc_failed_at_start = StreamerAllocFailed(base);
        g_pf.given_up = false;
    }

    uint32_t cur_s, cur_i;
    if (!CurrentLayer(base, wa, cur_s, cur_i)) return;

    // Stall / allocation watchdog.
    const auto now = std::chrono::steady_clock::now();
    const uint32_t key = (cur_s << 8) | cur_i;
    if (key != g_pf.progress_key) {
        g_pf.progress_key = key;
        g_pf.progress_at = now;
    } else if (!g_pf.held.empty() && now - g_pf.progress_at > kStallLimit) {
        g_pf.given_up = true;
    }
    if (!g_pf.alloc_failed_at_start && StreamerAllocFailed(base)) g_pf.given_up = true;

    // The next `want` distinct bitmap shapes, in the game's layer order
    // (sub_8236D200: surface by surface, index by index), from the current one.
    const int32_t want = g_pf.given_up ? 0 : larecomp::settings::vinyl_shape_prefetch.Get();
    std::vector<uint16_t> desired;
    desired.reserve(size_t(std::max(want, 0)));
    const uint32_t surfaces = std::min<uint32_t>(ReadGuestU32(base, wa + kWaSurfaces), 5);
    for (uint32_t s = cur_s; s < surfaces && int32_t(desired.size()) < want; ++s) {
        const uint32_t cap = std::min<uint32_t>(ReadGuestU32(base, wa + kWaSurfaceCap + 4 * s), 64);
        for (uint32_t i = (s == cur_s ? cur_i : 0); i < cap && int32_t(desired.size()) < want; ++i) {
            Layer layer;
            if (!ReadLayer(base, wa, s, i, layer) || layer.vector) continue;
            if (!RefCountAddr(base, lib, layer.shape)) continue;
            if (std::find(desired.begin(), desired.end(), layer.shape) == desired.end())
                desired.push_back(layer.shape);
        }
    }

    // Drop what is out of reach, then take what came into it.
    for (auto it = g_pf.held.begin(); it != g_pf.held.end();) {
        if (std::find(desired.begin(), desired.end(), *it) == desired.end()) {
            DropOne(base, lib, *it);
            it = g_pf.held.erase(it);
        } else {
            ++it;
        }
    }
    for (uint16_t shape : desired) {
        if (std::find(g_pf.held.begin(), g_pf.held.end(), shape) != g_pf.held.end()) continue;
        const uint32_t a = RefCountAddr(base, lib, shape);
        WriteGuestU32(base, a, ReadGuestU32(base, a) + 1);
        g_pf.held.push_back(shape);
    }
}

}  // namespace

// mcCarVinylWorkArea full regen step; r3 = the work area.
extern "C" REX_FUNC(rex_sub_8236F910) {
    const uint32_t wa = ctx.r3.u32;
    const auto lr = ctx.lr;
    const uint32_t sp = ctx.r1.u32;
    __imp__rex_sub_8236F910(ctx, base);
    SyncPrefetch(base, wa);

    const int32_t budget = larecomp::settings::vinyl_layers_per_frame.Get();
    for (int32_t n = 1; n < budget && NextLayerDrawable(base, wa); ++n) {
        ctx.r1.u32 = sp;
        ctx.lr = lr;
        ctx.r3.u64 = wa;
        __imp__rex_sub_8236F910(ctx, base);
        SyncPrefetch(base, wa);
    }
}

// Shape library: every reference count to 0 (end of a regen, sub_8236EA38).
extern "C" REX_FUNC(rex_sub_8236D0A0) {
    __imp__rex_sub_8236D0A0(ctx, base);
    g_wipes.fetch_add(1, std::memory_order_release);
}

// Work area teardown: resets the regen and zeroes the counts inline.
extern "C" REX_FUNC(rex_sub_8236D8B8) {
    __imp__rex_sub_8236D8B8(ctx, base);
    g_wipes.fetch_add(1, std::memory_order_release);
}
#endif  // REXGLUE_HAS_XEO3_TARGET
