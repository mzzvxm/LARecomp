#pragma once
//
// The ambient zones' density tuning, as numbers: what a zone holds, what the settings ask for, and what to write
// into the zone. Pure math: no ReXGlue, no guest memory; ambient_density_test.cpp checks it alone. The guest side
// (reading and writing the 31 zones, the spawner gates) is hooks/world_tuning.cpp.
//
// The fields (mcAmbientDensityTuning, the base of every ambient zone; the offsets are the parser records of
// sub_826F65B0, read from the recompiled code and the image's strings):
//   +0x08 fTrafficSpawnOuterRadius      180 m   the traffic's spawn ring (the code scales it by 0.75)
//   +0x10 fTrafficUnspawnOuterRadius    400 m   the traffic's unspawn radius (traffic_unspawn_dist)
//   +0x14 fTrafficBoxLengthAtLowSpeed   700 m   called "cull" in this file (the code scales it by 0.75)
//   +0x5C fPedDensity                   0.007   the pedestrians' density (ped_density_scale)
//   +0x7C fDensityLowMPH                 65     the player's speed under which the low-speed density holds
//   +0x80 fDensityHighMPH                90     the player's speed over which the high-speed density holds
//   +0x84 fDensityAtLowSpeed            1.5     the TRAFFIC density factor at low speed
//   +0x88 fDensityAtHighSpeed           1.0     the TRAFFIC density factor at high speed
//   +0x98 fParkedCarDensityFactor       0.25    the parked cars (parked_car_scale)
//
// The traffic density factor is the zone's per-frame update (sub_826F79F0, its last lines): zone + 960 =
// lerp(fDensityAtLowSpeed, fDensityAtHighSpeed, clamp01((speed - fDensityLowMPH) / (fDensityHighMPH - fDensityLowMPH))).
// The spawn pass reads zone + 960 and hands it to every car type of the zone (vtable slot 11, sub_826F4E90 sums the
// answers into the zone's target count at + 1796), so scaling both ends scales the factor at every speed, and a
// factor of 0 asks for no cars [CHECK: that the slot-11 answer is a product of the factor, so 0 gives a target of 0;
// the spawn gate at 0x8282A7C8 closes the spawner whatever that answer is].

#include <cmath>

namespace mcla::ambient {

// What a zone holds (the fields above that the settings touch).
struct ZoneDensity {
    float spawn = 0.0f;     // +0x08
    float unspawn = 0.0f;   // +0x10
    float cull = 0.0f;      // +0x14
    float ped = 0.0f;       // +0x5C
    float parked = 0.0f;    // +0x98
    float dens_low = 0.0f;  // +0x84 fDensityAtLowSpeed
    float dens_high = 0.0f; // +0x88 fDensityAtHighSpeed
};

inline bool operator==(const ZoneDensity& a, const ZoneDensity& b) {
    return a.spawn == b.spawn && a.unspawn == b.unspawn && a.cull == b.cull && a.ped == b.ped && a.parked == b.parked &&
           a.dens_low == b.dens_low && a.dens_high == b.dens_high;
}
inline bool operator!=(const ZoneDensity& a, const ZoneDensity& b) { return !(a == b); }

// What the settings ask for.
struct DensityWant {
    float unspawn = 0.0f;  // traffic_unspawn_dist, metres; 0 or less keeps the game's
    float ped = 1.0f;      // ped_density_scale
    float parked = 1.0f;   // parked_car_scale
    float traffic = 1.0f;  // traffic_density_scale: 0 none, 1 the game's, up to 2
};

inline bool operator==(const DensityWant& a, const DensityWant& b) {
    return a.unspawn == b.unspawn && a.ped == b.ped && a.parked == b.parked && a.traffic == b.traffic;
}
inline bool operator!=(const DensityWant& a, const DensityWant& b) { return !(a == b); }

constexpr float kMaxTrafficScale = 2.0f;

// The traffic scale as it is applied: 0 to 2. A number that is not a number is the game's own (1); below 0 is 0.
inline float ClampTrafficScale(float scale) {
    if (!std::isfinite(scale)) return 1.0f;
    if (scale < 0.0f) return 0.0f;
    if (scale > kMaxTrafficScale) return kMaxTrafficScale;
    return scale;
}

// The zone as it should be, from what the game itself left in it (`orig`). With the master switch off (enable_ambient_tuning)
// it is the game's own. Every number is computed from `orig`, never from what was written before, so a zone that is read
// again does not compound a scale.
inline ZoneDensity ComputeDensity(const ZoneDensity& orig, bool enabled, const DensityWant& want) {
    if (!enabled) return orig;
    ZoneDensity out = orig;
    if (want.unspawn > 0.0f) out.unspawn = want.unspawn;
    out.spawn = orig.spawn > 0.0f ? orig.spawn * 0.75f : orig.spawn;
    out.cull = orig.cull > 0.0f ? orig.cull * 0.75f : orig.cull;
    out.ped = orig.ped * want.ped;
    out.parked = orig.parked * want.parked;
    const float traffic = ClampTrafficScale(want.traffic);
    out.dens_low = orig.dens_low * traffic;
    out.dens_high = orig.dens_high * traffic;
    return out;
}

// Whether a spawner is shut outright (its global enable byte held at 0) for a scale: the master switch on and a scale of 0.
// The pedestrians' and the parked cars' spawners need it (a factor of 0 still leaves a couple of cars); the traffic's is
// shut too, so that "none" holds whatever the car types answer.
inline bool ShutsSpawner(bool enabled, float scale) { return enabled && !(scale > 0.0f); }

// One zone's state between frames: what the game last left in it, and what was last written.
struct ZoneState {
    ZoneDensity orig;     // what the game itself last left in the zone
    ZoneDensity written;  // what was last pushed into it
    bool has_written = false;
};

struct ZoneStep {
    bool write = false;     // push `out` into the zone
    bool captured = false;  // the zone's numbers are not what was written: `orig` was read from the game again
    ZoneDensity out;
};

// One frame of one zone. `cur` is what the zone holds now. If it is not what was written (the XML parse, a tune reload, or the
// first sight of the zone) that is the new original; otherwise nothing is to be done unless `force` (a setting moved).
inline ZoneStep StepZone(ZoneState& z, const ZoneDensity& cur, bool enabled, const DensityWant& want, bool force) {
    ZoneStep step;
    if (!z.has_written || cur != z.written) {
        z.orig = cur;
        step.captured = true;
    } else if (!force) {
        return step;
    }
    step.out = ComputeDensity(z.orig, enabled, want);
    step.write = true;
    z.written = step.out;
    z.has_written = true;
    return step;
}

}  // namespace mcla::ambient
