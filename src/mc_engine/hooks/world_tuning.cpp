// City density: the ambient traffic, pedestrian and parked car tuning of every
// ambient zone, and the fragTuneStruct overrides (breakable prop draw distance
// and the framerate floor for new breaks). The numbers (what a zone holds, what to
// write into it) are in ambient_density.h, which ambient_density_test.cpp checks.

#ifndef REXGLUE_HAS_XEO3_TARGET
#include "settings/larecomp_settings.h"
#include "settings/immortal.h"
#include <rex/kernel/title_runtime.h>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>

#include "ambient_density.h"
#include "hooks.h"
#include "hooks_internal.h"
#include "larecomp_log.h"
#include "larecomp_guest.h"
#include "mc_engine/core/world_detail.h"

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
using mcla::ambient::DensityWant;
using mcla::ambient::ZoneDensity;
using mcla::ambient::ZoneState;

static std::mutex g_density_mutex;
static std::map<uint32_t, ZoneState>& g_density_zones = larecomp::Leak<std::map<uint32_t, ZoneState>>();  // zone address -> state
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
static DensityWant g_density_last_want;

// Offsets verified via rage::mcAmbientDensityTuning in mcla_rage_types.h (the parser records of sub_826F65B0 name them):
//   +0x08 = spawn_max
//   +0x10 = unspawn_max
//   +0x14 = cull_max
//   +0x5C = fPedDensity (92)   -- offset slot of the parser record in sub_826F65B0;
//                                 read by sub_826F3038, which sets the zone's ped
//                                 target (+0x864) from it. +0x60 is a different field.
//   +0x84 = fDensityAtLowSpeed (132), +0x88 = fDensityAtHighSpeed (136) -- the traffic's density factor at
//                                 low and at high speed (stock 1.5 and 1.0); the zone's update (sub_826F79F0)
//                                 lerps them by the player's speed into zone + 960, which the spawn pass hands
//                                 to every car type (ambient_density.h)
//   +0x98 = fParkedCarDensityFactor (152) -- read by sub_826F23A0 (parked spawner)
static ZoneDensity ReadZoneValues(uint8_t* base, uint32_t a) {
    ZoneDensity v;
    v.spawn = ReadGuestF32(base, a + 8);
    v.unspawn = ReadGuestF32(base, a + 16);   // 400.0 stock
    v.cull = ReadGuestF32(base, a + 20);
    v.ped = ReadGuestF32(base, a + 92);
    v.parked = ReadGuestF32(base, a + 152);   // 0.25 stock
    v.dens_low = ReadGuestF32(base, a + 132);   // 1.5 stock
    v.dens_high = ReadGuestF32(base, a + 136);  // 1.0 stock
    return v;
}

static void WriteZoneValues(uint8_t* base, uint32_t a, const ZoneDensity& v) {
    WriteGuestF32(base, a + 8, v.spawn);
    WriteGuestF32(base, a + 16, v.unspawn);
    WriteGuestF32(base, a + 20, v.cull);
    WriteGuestF32(base, a + 92, v.ped);
    WriteGuestF32(base, a + 152, v.parked);
    WriteGuestF32(base, a + 132, v.dens_low);
    WriteGuestF32(base, a + 136, v.dens_high);
}

// The settings' values as the zones are to be written. The traffic range and the two densities are `auto` by default (below 0, or 0 for the
// range): the number the GPU class gives (gpu/gpu_class.h kScales; the Low cell until the class is known, which is what they were before the
// class chose them), so a zone follows the class once the device is open (the tick sees `want` change and writes the zones again).
static void ReadDensityCvars(bool& enabled, DensityWant& want) {
    using mcla::gpu::gpu_class::Scale;
    enabled = larecomp::settings::enable_ambient_tuning.Get();
    want.unspawn = static_cast<float>(
        mcla::world_detail::InForce(Scale::kTrafficRange, larecomp::settings::traffic_unspawn_dist.Get()));
    want.ped = static_cast<float>(
        mcla::world_detail::InForce(Scale::kPedDensity, larecomp::settings::ped_density_scale.Get()));
    want.parked = static_cast<float>(
        mcla::world_detail::InForce(Scale::kParkedDensity, larecomp::settings::parked_car_scale.Get()));
    want.traffic = static_cast<float>(larecomp::settings::traffic_density_scale.Get());
}

// Brings one zone to the wanted state. If the game moved a field since our last
// write (the XML parse, a tune reload) that value is the new original, so the
// scale is always applied to the XML value and never compounds (ambient_density.h
// StepZone). Caller holds g_density_mutex. Returns true when the zone was
// (re)captured from the game.
static bool ReconcileZone(uint8_t* base, uint32_t a, bool enabled,
                          const DensityWant& want, bool force) {
    const mcla::ambient::ZoneStep step =
        mcla::ambient::StepZone(g_density_zones[a], ReadZoneValues(base, a), enabled, want, force);
    if (step.write) WriteZoneValues(base, a, step.out);
    return step.captured;
}

// 0x826F5CA0, in the epilogue of the mcAmbientDensityTuning constructor
// sub_826F5B18. r3 holds the ambient zone. Only registers the zone early; the
// XML parse that follows overwrites these fields and the tick below re-captures.
void MCLAAmbientDensityTuning(PPCRegister& r3) {
    const uint32_t a = static_cast<uint32_t>(r3.u64);
    if (a == 0) return;

    auto* base = larecomp::guest::Membase();
    if (!base) return;

    bool enabled = false;
    DensityWant want;
    ReadDensityCvars(enabled, want);

    std::lock_guard<std::mutex> lock(g_density_mutex);
    ReconcileZone(base, a, enabled, want, true);
}

// Global enables of the ambient spawners (bytes written by sub_826D5D90 /
// sub_826D5E20 / sub_826D5EB0, the AmbientMgr_Set{Traffic,Peds,ParkedCars}Enable natives).
// sub_826F3038 (pedestrians) and sub_826F23A0 (parked cars) bail
// out at the top when their byte is 0. The parked spawner's limit is
// max(1..2, factor * n) for small n, so a 0 factor alone still leaves a couple of
// cars per zone -- a scale of 0 therefore also closes the spawner outright.
// The traffic's spawn step (sub_826F6920) and its candidate test (sub_826EE018) read the
// traffic byte the same way; only new spawns stop (the native also sends the despawn message,
// this gate does not), so a car already on the road drives on until it leaves.
static constexpr uint32_t kTrafficSpawnEnable = 0x8282A7C8u;
static constexpr uint32_t kPedSpawnEnable    = 0x8282A7C9u;
static constexpr uint32_t kParkedSpawnEnable = 0x8282A7CAu;

struct SpawnGate {
    uint32_t addr;
    bool forcing = false;
    uint8_t saved = 1;  // what the game last had there while we were not forcing
};
static SpawnGate g_traffic_gate{kTrafficSpawnEnable};
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
    auto* base = larecomp::guest::Membase();
    if (!base) return;

    const uint32_t mgr = ReadGuestU32(base, kAmbientManagerPtr);

    std::lock_guard<std::mutex> lock(g_density_mutex);
    {
        bool en = false;
        DensityWant w;
        ReadDensityCvars(en, w);
        UpdateSpawnGate(base, g_traffic_gate, mcla::ambient::ShutsSpawner(en, w.traffic), "traffic");
        UpdateSpawnGate(base, g_ped_gate, mcla::ambient::ShutsSpawner(en, w.ped), "pedestrian");
        UpdateSpawnGate(base, g_parked_gate, mcla::ambient::ShutsSpawner(en, w.parked), "parked car");
    }
    if (mgr != g_density_manager) {
        g_density_zones.clear();  // a new manager: nothing captured applies to it
        g_density_manager = mgr;
    }
    if (mgr == 0) return;

    bool enabled = false;
    DensityWant want;
    ReadDensityCvars(enabled, want);

    const bool cvars_moved = !g_density_logged_state || g_density_last_enabled != enabled ||
                             g_density_last_want != want;
    g_density_logged_state = true;
    g_density_last_enabled = enabled;
    g_density_last_want = want;

    uint32_t recaptured = 0;
    ZoneDensity first{};
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
            "ped {:.4f}, parked {:.3f}, traffic density {:.2f} / {:.2f}); enabled={}, ped x{:.2f}, "
            "parked x{:.2f}, traffic x{:.2f}",
            recaptured, first.unspawn, first.ped, first.parked, first.dens_low, first.dens_high, enabled,
            want.ped, want.parked, want.traffic);
    } else if (cvars_moved) {
        LARECOMP_APP_INFO(
            "[Ambient Tuning] cvars applied to {} zones (enabled={}, unspawn={:.1f}, "
            "ped x{:.2f}, parked x{:.2f}, traffic x{:.2f})",
            kAmbientZoneCount, enabled, want.unspawn, want.ped, want.parked, want.traffic);
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
    auto* base = larecomp::guest::Membase();
    if (!base) return;

    const uint32_t obj = ReadGuestU32(base, kFragTunePtr);
    if (obj == 0) return;

    for (auto& f : g_frag_tune) {
        const double want = std::strtod(larecomp::cvar::GetFlagByName(f.cvar).c_str(), nullptr);

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
