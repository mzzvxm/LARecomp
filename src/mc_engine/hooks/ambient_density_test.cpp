// Check of the ambient zones' density numbers (ambient_density.h): the traffic density scale. A program of its own: it needs
// no ReXGlue, no guest and no game, and exits 0 when every check holds.
//
//   clang++ -std=c++20 -O1 -Wall -Wextra -I src/mc_engine/hooks src/mc_engine/hooks/ambient_density_test.cpp -o ambient_density_test
//   ./ambient_density_test
//
// What it pins down: a scale of 1 leaves the traffic fields exactly as the game wrote them (and the other fields as they were
// before the traffic scale existed), 0 gives 0 and 2 doubles at every player speed through the zone's own lerp (sub_826F79F0),
// the master switch off is the game's own, a bad scale is bounded, and a zone that is read again (the XML parse, a setting that
// moves, the same setting for a thousand frames) never compounds a scale.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <initializer_list>

#include "ambient_density.h"

using namespace mcla::ambient;

namespace {

int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const char* what, double a = 0.0, double b = 0.0) {
    ++g_checks;
    if (!ok) {
        ++g_failed;
        std::printf("FAIL: %s (%.9g, %.9g)\n", what, a, b);
    }
}

// What the game leaves in a zone after the density_tuning.xml parse (the constructor's defaults of sub_826F5B18; the file may
// change them, the numbers only need to be plausible and different from each other).
ZoneDensity StockZone() {
    ZoneDensity z;
    z.spawn = 180.0f;
    z.unspawn = 400.0f;
    z.cull = 700.0f;
    z.ped = 0.007f;
    z.parked = 0.25f;
    z.dens_low = 1.5f;
    z.dens_high = 1.0f;
    return z;
}

// The game's own density factor, the last lines of the zone update (sub_826F79F0), instruction by instruction: with
// t = (speed - low_mph) / (high_mph - low_mph) clamped to 0..1 by the fsel pairs, zone + 960 = fmadds(t, high - low, low).
float GuestFactor(const ZoneDensity& z, float speed_mph, float low_mph = 65.0f, float high_mph = 90.0f) {
    const float t_raw = (speed_mph - low_mph) / (high_mph - low_mph);
    float t = t_raw >= 0.0f ? t_raw : 0.0f;
    t = (t - 1.0f) >= 0.0f ? 1.0f : t;
    return std::fmaf(t, z.dens_high - z.dens_low, z.dens_low);
}

bool Near(float a, float b, float tol = 1e-6f) { return std::fabs(a - b) <= tol * std::max(1.0f, std::fabs(b)); }

void TestStockIsUntouched() {
    const ZoneDensity orig = StockZone();
    const DensityWant want;  // 1, 1, 1, unspawn untouched
    const ZoneDensity out = ComputeDensity(orig, true, want);
    Check(out.dens_low == orig.dens_low, "scale 1: the density at low speed is the game's, bit for bit", out.dens_low, orig.dens_low);
    Check(out.dens_high == orig.dens_high, "scale 1: the density at high speed is the game's, bit for bit", out.dens_high, orig.dens_high);
    Check(out.ped == orig.ped && out.parked == orig.parked, "ped and parked x1 are the game's");
    Check(out.unspawn == orig.unspawn, "an unspawn distance of 0 keeps the game's");
    Check(Near(out.spawn, 135.0f) && Near(out.cull, 525.0f), "the spawn and cull radii keep their 0.75 (what the tuning did before the traffic scale)", out.spawn, out.cull);

    const ZoneDensity off = ComputeDensity(orig, false, DensityWant{250.0f, 0.0f, 0.0f, 0.0f});
    Check(off == orig, "the master switch off: the zone is the game's own, whatever the scales say");
}

void TestScales() {
    const ZoneDensity orig = StockZone();
    DensityWant want;
    want.traffic = 0.0f;
    ZoneDensity z = ComputeDensity(orig, true, want);
    Check(z.dens_low == 0.0f && z.dens_high == 0.0f, "scale 0: both ends of the density are 0");
    for (float mph : {0.0f, 30.0f, 65.0f, 77.5f, 90.0f, 140.0f, 400.0f}) {
        Check(GuestFactor(z, mph) == 0.0f, "scale 0: the game's density factor is 0 at every speed", mph);
    }
    want.traffic = 2.0f;
    z = ComputeDensity(orig, true, want);
    for (float mph : {0.0f, 30.0f, 65.0f, 77.5f, 90.0f, 140.0f}) {
        Check(Near(GuestFactor(z, mph), 2.0f * GuestFactor(orig, mph)), "scale 2: the factor doubles at every speed", mph, GuestFactor(z, mph));
    }
    want.traffic = 0.5f;
    z = ComputeDensity(orig, true, want);
    for (float mph : {0.0f, 40.0f, 80.0f, 200.0f}) {
        Check(Near(GuestFactor(z, mph), 0.5f * GuestFactor(orig, mph)), "scale 0.5: the factor halves at every speed", mph, GuestFactor(z, mph));
    }
    // the stock factor itself: 1.5 at walking pace, 1.0 on the motorway, the lerp between
    Check(Near(GuestFactor(orig, 0.0f), 1.5f) && Near(GuestFactor(orig, 90.0f), 1.0f) && Near(GuestFactor(orig, 77.5f), 1.25f), "the lerp model gives the stock 1.5 / 1.25 / 1.0");
    // the traffic scale touches nothing else
    Check(z.ped == orig.ped && z.parked == orig.parked && z.unspawn == orig.unspawn, "the traffic scale leaves ped, parked and unspawn alone");
}

void TestClamp() {
    Check(ClampTrafficScale(NAN) == 1.0f, "not a number: the game's own");
    Check(ClampTrafficScale(INFINITY) == 1.0f, "infinity: the game's own");
    Check(ClampTrafficScale(-3.0f) == 0.0f, "negative: none");
    Check(ClampTrafficScale(7.0f) == kMaxTrafficScale, "above 2: 2");
    Check(ClampTrafficScale(0.3f) == 0.3f && ClampTrafficScale(0.0f) == 0.0f && ClampTrafficScale(2.0f) == 2.0f, "in range: as given");
    DensityWant bad;
    bad.traffic = -5.0f;
    const ZoneDensity z = ComputeDensity(StockZone(), true, bad);
    Check(z.dens_low == 0.0f && z.dens_high == 0.0f, "a negative scale is applied as 0, never as a negative density");
    bad.traffic = 9.0f;
    const ZoneDensity y = ComputeDensity(StockZone(), true, bad);
    Check(y.dens_low == 3.0f && y.dens_high == 2.0f, "a scale above 2 is applied as 2", y.dens_low, y.dens_high);
}

void TestShutsSpawner() {
    Check(ShutsSpawner(true, 0.0f), "scale 0 shuts the spawner");
    Check(ShutsSpawner(true, -1.0f), "a negative scale shuts it too");
    Check(!ShutsSpawner(true, 0.001f), "a small scale does not");
    Check(!ShutsSpawner(true, 1.0f) && !ShutsSpawner(true, 2.0f), "1 and 2 do not");
    Check(!ShutsSpawner(false, 0.0f), "the master switch off: never shut");
}

// A zone over many frames, the way ApplyAmbientDensityTuning drives it.
void TestNoCompounding() {
    ZoneState state;
    const ZoneDensity game = StockZone();  // what the XML parse leaves
    ZoneDensity guest = game;              // the guest's memory
    DensityWant want;
    want.traffic = 0.5f;

    auto frame = [&](bool force) {
        const ZoneStep s = StepZone(state, guest, true, want, force);
        if (s.write) guest = s.out;
        return s;
    };

    ZoneStep s = frame(true);
    Check(s.captured && s.write, "first sight: the zone is captured and written");
    Check(Near(guest.dens_low, 0.75f) && Near(guest.dens_high, 0.5f), "x0.5 on the first frame", guest.dens_low, guest.dens_high);
    for (int i = 0; i < 1000; ++i) frame(false);
    Check(Near(guest.dens_low, 0.75f) && Near(guest.dens_high, 0.5f), "a thousand frames later it is still x0.5, not compounded", guest.dens_low, guest.dens_high);
    s = frame(false);
    Check(!s.write && !s.captured, "a quiet frame writes nothing");

    // the same setting forced again (a cvar that moved and came back) does not compound either
    for (int i = 0; i < 50; ++i) frame(true);
    Check(Near(guest.dens_low, 0.75f), "forced 50 times: still x0.5 of the original", guest.dens_low);
    Check(state.orig == game, "the original stays what the game left", state.orig.dens_low, game.dens_low);

    // the setting moves
    want.traffic = 2.0f;
    frame(true);
    Check(Near(guest.dens_low, 3.0f) && Near(guest.dens_high, 2.0f), "x0.5 to x2 gives 2 times the ORIGINAL, not 4 times what was written", guest.dens_low, guest.dens_high);
    want.traffic = 0.0f;
    frame(true);
    Check(guest.dens_low == 0.0f && guest.dens_high == 0.0f, "to 0: both ends 0");
    want.traffic = 1.0f;
    frame(true);
    Check(guest.dens_low == game.dens_low && guest.dens_high == game.dens_high, "back to 1: the game's own numbers again, exactly", guest.dens_low, game.dens_low);

    // the game parses the XML again (a new zone density, as a tune reload would give)
    want.traffic = 0.5f;
    frame(true);
    guest = game;
    guest.dens_low = 2.0f;
    guest.dens_high = 1.2f;
    guest.ped = 0.01f;
    s = frame(false);
    Check(s.captured && s.write, "a re-parse is noticed without a setting moving");
    Check(Near(guest.dens_low, 1.0f) && Near(guest.dens_high, 0.6f), "the scale is applied to the NEW original", guest.dens_low, guest.dens_high);
    Check(Near(guest.ped, 0.01f), "the other fields follow the re-parse too", guest.ped);

    // a re-parse that changes only the traffic fields is noticed
    ZoneDensity reparse = guest;
    reparse.dens_low = 1.7f;
    reparse.dens_high = 1.1f;
    guest = reparse;
    s = frame(false);
    Check(s.captured, "a change of the traffic density fields alone counts as the game's write");
    Check(Near(guest.dens_low, 0.85f) && Near(guest.dens_high, 0.55f), "and gets the scale", guest.dens_low, guest.dens_high);

    // the master switch
    ZoneState state2;
    ZoneDensity guest2 = game;
    const ZoneStep a = StepZone(state2, guest2, false, want, true);
    Check(a.write && a.out == game, "the master switch off writes the game's own numbers back");
}

}  // namespace

int main() {
    TestStockIsUntouched();
    TestScales();
    TestClamp();
    TestShutsSpawner();
    TestNoCompounding();
    std::printf("ambient_density_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
