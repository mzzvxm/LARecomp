// City density: the ambient traffic, pedestrian and parked car tuning of every
// ambient zone, and the fragTuneStruct overrides (breakable prop draw distance
// and the framerate floor for new breaks).

#ifndef REXGLUE_HAS_XEO3_TARGET
#include <rex/cvar.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>

#include "hooks.h"
#include "hooks_internal.h"
#include "larecomp_log.h"

// BadassBaboon's Recomp Adjustments: Ambient traffic & pedestrian density tuning for city performance
REXCVAR_DEFINE_BOOL(enable_ambient_tuning, true, "MCLA/Performance",
    "Enable ambient traffic and pedestrian density tuning.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(traffic_unspawn_dist, 250.0, "MCLA/Performance",
    "Traffic vehicle unspawn radius in meters (default 400.0, lower = higher FPS in city).")
    .range(100.0, 600.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(ped_density_scale, 0.5, "MCLA/Performance",
    "Pedestrian density scale multiplier (0.0 = none, 0.5 = half, 1.0 = full).")
    .range(0.0, 2.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(parked_car_scale, 0.5, "MCLA/Performance",
    "Parked car density scale multiplier (0.0 = none, 0.5 = half, 1.0 = full).")
    .range(0.0, 2.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// ── rage::fragTuneStruct overrides ──────────────────────────────────────────
// Both values live in the fragment tune the game parses out of
// $/tune/types/fragments (fragment = breakable prop: poles, signs, fences,
// barriers). They are patched in the parsed struct at runtime, so no RPF edit
// and no decryption is involved. 0 keeps whatever the tune file loaded.
REXCVAR_DEFINE_DOUBLE(global_max_draw_distance, 0.0, "MCLA/Performance",
    "fragTuneStruct::GlobalMaxDrawingDistance — draw distance for breakable props "
    "(poles, signs, fences). 0 = keep the tune file's value (3000); the engine's own "
    "constructor default is 250.")
    .range(0.0, 6000.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(breaking_frame_rate_limit, 0.0, "MCLA/Performance",
    "fragTuneStruct::BreakingFrameRateLimit — framerate floor under which the engine "
    "stops spawning new fragment breaks. 0 = keep the tune file's value (10); the "
    "engine's own constructor default is 30.")
    .range(0.0, 120.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// BadassBaboon's Recomp Adjustments: ambient traffic / pedestrian density.
//
// mcAmbientDensityTuning is not a separate object: sub_826F5CB0 calls its
// constructor sub_826F5B18 with its OWN `this` (`sub_826F5B18(a1)`), so the
// tuning fields are the base of the ~5936-byte ambient zone. The zone array is
// built in sub_826D8E70 -- 31 zones, `v5 = manager + 56476`, stride 1484 dwords
// -- and each one re-parses $/tune/ambients/density_tuning.xml through
// sub_826F4CB8, whose r31 is that zone. r31 is therefore the tuning object,
// and 31 hook firings per load is expected, one per zone.
//
// (The value passed as the parse's 4th argument is NOT the instance: it is the
// shared class descriptor returned by vtable slot 1, identical across all 31
// zones. Writing tuning fields through it corrupts a live RAGE structure.)
//
// The override has to land AFTER the parse -- hooking the constructor is
// pointless because the parse overwrites every field it set.
//
// Per-zone originals are captured from what the parse left behind, and every
// override is computed from those, so re-parsing a zone never compounds the
// scale the way the original code did.
struct DensityTuningValues {
    float spawn = 0.0f;
    float unspawn = 0.0f;
    float cull = 0.0f;
    float ped = 0.0f;
    float parked = 0.0f;
};

struct DensityZone {
    DensityTuningValues orig;     // what the game itself last left in the zone
    DensityTuningValues written;  // what this code last pushed into it
    bool has_written = false;
};

static std::mutex g_density_mutex;
static std::map<uint32_t, DensityZone> g_density_zones;  // zone address -> state
static uint32_t g_density_manager = 0;

// The ambient manager singleton (dword_8286D8E4), built by sub_822D3D38 and
// followed straight away by sub_826D89F0, which re-parses density_tuning.xml
// into all 31 zones at manager + 225904 (stride 5936). That parse runs AFTER the
// constructor hook below, so anything the constructor hook writes is wiped; the
// per-frame tick is what makes the scales stick.
static constexpr uint32_t kAmbientManagerPtr = 0x8286D8E4u;
static constexpr uint32_t kAmbientZoneBase   = 225904u;
static constexpr uint32_t kAmbientZoneStride = 5936u;
static constexpr uint32_t kAmbientZoneCount  = 31u;

// Last cvar state pushed, so unchanged frames only compare guest floats.
static bool g_density_logged_state = false;
static bool g_density_last_enabled = false;
static DensityTuningValues g_density_last_want;

// Offsets verified via rage::mcAmbientDensityTuning in mcla_rage_types.h:
//   +0x08 = spawn_max
//   +0x10 = unspawn_max
//   +0x14 = cull_max
//   +0x5C = fPedDensity (92)   -- offset slot of the parser record in sub_826F65B0;
//                                 read by sub_826F3038, which sets the zone's ped
//                                 target (+0x864) from it. +0x60 is a different field.
//   +0x98 = fParkedCarDensityFactor (152) -- read by sub_826F23A0 (parked spawner)
static DensityTuningValues ReadZoneValues(uint8_t* base, uint32_t a) {
    DensityTuningValues v;
    v.spawn = ReadGuestF32(base, a + 8);
    v.unspawn = ReadGuestF32(base, a + 16);   // 400.0 stock
    v.cull = ReadGuestF32(base, a + 20);
    v.ped = ReadGuestF32(base, a + 92);
    v.parked = ReadGuestF32(base, a + 152);   // 0.25 stock
    return v;
}

static DensityTuningValues ComputeDensity(const DensityTuningValues& orig, bool enabled,
                                          const DensityTuningValues& want) {
    if (!enabled) return orig;
    DensityTuningValues out = orig;
    if (want.unspawn > 0.0f) out.unspawn = want.unspawn;
    out.spawn = orig.spawn > 0.0f ? orig.spawn * 0.75f : orig.spawn;
    out.cull = orig.cull > 0.0f ? orig.cull * 0.75f : orig.cull;
    out.ped = orig.ped * want.ped;
    out.parked = orig.parked * want.parked;
    return out;
}

static void WriteZoneValues(uint8_t* base, uint32_t a, const DensityTuningValues& v) {
    WriteGuestF32(base, a + 8, v.spawn);
    WriteGuestF32(base, a + 16, v.unspawn);
    WriteGuestF32(base, a + 20, v.cull);
    WriteGuestF32(base, a + 92, v.ped);
    WriteGuestF32(base, a + 152, v.parked);
}

static void ReadDensityCvars(bool& enabled, DensityTuningValues& want) {
    enabled = REXCVAR_GET(enable_ambient_tuning);
    want.unspawn = static_cast<float>(REXCVAR_GET(traffic_unspawn_dist));
    want.ped = static_cast<float>(REXCVAR_GET(ped_density_scale));
    want.parked = static_cast<float>(REXCVAR_GET(parked_car_scale));
}

// Brings one zone to the wanted state. If the game moved a field since our last
// write (the XML parse, a tune reload) that value is the new original, so the
// scale is always applied to the XML value and never compounds. Caller holds
// g_density_mutex. Returns true when the zone was (re)captured from the game.
static bool ReconcileZone(uint8_t* base, uint32_t a, bool enabled,
                          const DensityTuningValues& want, bool force) {
    DensityZone& z = g_density_zones[a];
    const DensityTuningValues cur = ReadZoneValues(base, a);
    bool captured = false;
    if (!z.has_written || cur.spawn != z.written.spawn || cur.unspawn != z.written.unspawn ||
        cur.cull != z.written.cull || cur.ped != z.written.ped || cur.parked != z.written.parked) {
        z.orig = cur;
        captured = true;
    } else if (!force) {
        return false;
    }
    const DensityTuningValues out = ComputeDensity(z.orig, enabled, want);
    WriteZoneValues(base, a, out);
    z.written = out;
    z.has_written = true;
    return captured;
}

// 0x826F5CA0, in the epilogue of the mcAmbientDensityTuning constructor
// sub_826F5B18. r3 holds the ambient zone. Only registers the zone early; the
// XML parse that follows overwrites these fields and the tick below re-captures.
void MCLAAmbientDensityTuning(PPCRegister& r3) {
    const uint32_t a = static_cast<uint32_t>(r3.u64);
    if (a == 0) return;

    auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;

    bool enabled = false;
    DensityTuningValues want;
    ReadDensityCvars(enabled, want);

    std::lock_guard<std::mutex> lock(g_density_mutex);
    ReconcileZone(base, a, enabled, want, true);
}

// Global enables of the ambient spawners (bytes written by sub_826D5E20 /
// sub_826D5EB0). sub_826F3038 (pedestrians) and sub_826F23A0 (parked cars) bail
// out at the top when their byte is 0. The parked spawner's limit is
// max(1..2, factor * n) for small n, so a 0 factor alone still leaves a couple of
// cars per zone -- a scale of 0 therefore also closes the spawner outright.
static constexpr uint32_t kPedSpawnEnable    = 0x8282A7C9u;
static constexpr uint32_t kParkedSpawnEnable = 0x8282A7CAu;

struct SpawnGate {
    uint32_t addr;
    bool forcing = false;
    uint8_t saved = 1;  // what the game last had there while we were not forcing
};
static SpawnGate g_ped_gate{kPedSpawnEnable};
static SpawnGate g_parked_gate{kParkedSpawnEnable};

static void UpdateSpawnGate(uint8_t* base, SpawnGate& g, bool close, const char* label) {
    uint8_t v = base[g.addr];
    if (close) {
        if (!g.forcing) {
            g.saved = v;
            g.forcing = true;
            LARECOMP_APP_INFO("[Ambient Tuning] {} spawner closed (scale 0)", label);
        }
        if (v != 0) base[g.addr] = 0;
    } else if (g.forcing) {
        base[g.addr] = g.saved;
        g.forcing = false;
        LARECOMP_APP_INFO("[Ambient Tuning] {} spawner restored", label);
    }
}

// Per-frame: walks the 31 zones of the live manager. Costs ~155 guest float
// compares and writes only what the game or a cvar changed.
void ApplyAmbientDensityTuning() {
    auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;

    const uint32_t mgr = ReadGuestU32(base, kAmbientManagerPtr);

    std::lock_guard<std::mutex> lock(g_density_mutex);
    {
        bool en = false;
        DensityTuningValues w;
        ReadDensityCvars(en, w);
        UpdateSpawnGate(base, g_ped_gate, en && w.ped <= 0.0f, "pedestrian");
        UpdateSpawnGate(base, g_parked_gate, en && w.parked <= 0.0f, "parked car");
    }
    if (mgr != g_density_manager) {
        g_density_zones.clear();  // a new manager: nothing captured applies to it
        g_density_manager = mgr;
    }
    if (mgr == 0) return;

    bool enabled = false;
    DensityTuningValues want;
    ReadDensityCvars(enabled, want);

    const bool cvars_moved = !g_density_logged_state || g_density_last_enabled != enabled ||
                             g_density_last_want.unspawn != want.unspawn ||
                             g_density_last_want.ped != want.ped ||
                             g_density_last_want.parked != want.parked;
    g_density_logged_state = true;
    g_density_last_enabled = enabled;
    g_density_last_want = want;

    uint32_t recaptured = 0;
    DensityTuningValues first{};
    for (uint32_t i = 0; i < kAmbientZoneCount; ++i) {
        const uint32_t a = mgr + kAmbientZoneBase + i * kAmbientZoneStride;
        if (ReconcileZone(base, a, enabled, want, cvars_moved)) {
            if (recaptured == 0) first = g_density_zones[a].orig;
            ++recaptured;
        }
    }
    if (recaptured != 0) {
        LARECOMP_APP_INFO(
            "[Ambient Tuning] {} zones captured from the game (zone 0: unspawn {:.1f}, "
            "ped {:.4f}, parked {:.3f}); enabled={}, ped x{:.2f}, parked x{:.2f}",
            recaptured, first.unspawn, first.ped, first.parked, enabled, want.ped, want.parked);
    } else if (cvars_moved) {
        LARECOMP_APP_INFO(
            "[Ambient Tuning] cvars applied to {} zones (enabled={}, unspawn={:.1f}, "
            "ped x{:.2f}, parked x{:.2f})",
            kAmbientZoneCount, enabled, want.unspawn, want.ped, want.parked);
    }
}

// ── rage::fragTuneStruct overrides ──────────────────────────────────────────
//
// sub_82729B10 allocates the 1448-byte singleton and parks the pointer in
// dword_828D4CE4; sub_8275E518 is its constructor. The field offsets come from
// the parser registration in sub_8275E308, which writes each parMember's offset
// slot: the record naming "GlobalMaxDrawingDistance" (0x8282F370) gets 8 and the
// one naming "BreakingFrameRateLimit" (0x8282F400) gets 32. Both line up with
// the constructor defaults (+8 = 250.0f, +32 = 30.0f).
//
// The game overwrites both from $/tune/types/fragments after construction, so
// this runs per-frame and re-asserts the override on top of whatever the tune
// file loaded — no RPF edit, no decryption. A cvar left at 0 writes nothing and
// instead keeps re-reading the guest value, so switching back to 0 restores
// exactly what the tune file had.
static constexpr uint32_t kFragTunePtr        = 0x828D4CE4u;  // rage::fragTuneStruct*
static constexpr uint32_t kFragTuneDrawDist   = 8u;           // GlobalMaxDrawingDistance
static constexpr uint32_t kFragTuneBreakLimit = 32u;          // BreakingFrameRateLimit

struct FragTuneField {
    uint32_t offset;
    const char* cvar;
    const char* label;
    float stock = 0.0f;   // last value seen while the cvar was 0
    bool have_stock = false;
    float applied = 0.0f;
    bool have_applied = false;
};

static FragTuneField g_frag_tune[] = {
    {kFragTuneDrawDist,   "global_max_draw_distance",  "GlobalMaxDrawingDistance"},
    {kFragTuneBreakLimit, "breaking_frame_rate_limit", "BreakingFrameRateLimit"},
};

void ApplyFragTuneOverrides() {
    auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;

    const uint32_t obj = ReadGuestU32(base, kFragTunePtr);
    if (obj == 0) return;

    for (auto& f : g_frag_tune) {
        const double want = std::strtod(rex::cvar::GetFlagByName(f.cvar).c_str(), nullptr);

        if (!(want > 0.0)) {
            // Auto: track whatever the tune file left there, and put it back once
            // if we had been overriding.
            if (f.have_applied) {
                if (f.have_stock) {
                    WriteGuestF32(base, obj + f.offset, f.stock);
                    LARECOMP_APP_INFO("[FragTune] {} restored to {:.1f}", f.label, f.stock);
                }
                f.have_applied = false;
            }
            f.stock = ReadGuestF32(base, obj + f.offset);
            f.have_stock = true;
            continue;
        }

        const float v = static_cast<float>(want);
        const float cur = ReadGuestF32(base, obj + f.offset);
        if (f.have_applied && f.applied == v && cur == v) continue;

        // If the field moved out from under us the game wrote it itself — the
        // tune file landing after construction — so that is the real stock
        // value, not whatever was in the struct before the parse.
        if (!f.have_stock || (f.have_applied && cur != f.applied)) {
            f.stock = cur;
            f.have_stock = true;
        }
        WriteGuestF32(base, obj + f.offset, v);
        if (!f.have_applied || f.applied != v) {
            LARECOMP_APP_INFO("[FragTune] {} {:.1f} -> {:.1f} (fragTuneStruct 0x{:08X}+{})",
                              f.label, f.stock, v, obj, f.offset);
        }
        f.applied = v;
        f.have_applied = true;
    }
}
#else // REXGLUE_HAS_XEO3_TARGET
// XEO3 stubs: empty implementations so the linker resolves codegen calls.

#include <rex/ppc/context.h>

void MCLAAmbientDensityTuning(PPCRegister& r3) {}
#endif // REXGLUE_HAS_XEO3_TARGET
