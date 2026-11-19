// Check of the traffic probe's pure part (traffic_probe_core.h): the spawn guard, the pose maths, the census and the name reader. A
// program of its own: it needs no ReXGlue, no guest and no game, and exits 0 when every check holds.
//
//   clang++ -std=c++20 -O1 -Wall -Wextra -I src/mc_engine/hooks src/mc_engine/hooks/traffic_probe_test.cpp -o traffic_probe_test
//   ./traffic_probe_test

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

#include "traffic_probe_core.h"

using namespace mcla::traffic_probe;

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

bool Near(double a, double b, double eps = 1.0e-6) { return std::fabs(a - b) <= eps; }

SpawnInputs Good() {
    SpawnInputs in;
    in.manager = true;
    in.player = true;
    in.free_roam = true;
    in.driving = true;
    in.world_updates = 1000;
    in.layout_pool = true;
    in.layout_free = 40;
    in.type_known = true;
    in.position_ok = true;
    in.calls_so_far = 0;
    return in;
}

void TestGuard() {
    Check(DecideSpawn(Good()) == Refusal::kNone, "every guard met: the call may be made");

    SpawnInputs in = Good();
    in.manager = false;
    Check(DecideSpawn(in) == Refusal::kNoManager, "no manager");
    in = Good();
    in.player = false;
    Check(DecideSpawn(in) == Refusal::kNoPlayer, "no player");
    in = Good();
    in.free_roam = false;
    Check(DecideSpawn(in) == Refusal::kNotFreeRoam, "not free roam");
    in = Good();
    in.driving = false;
    Check(DecideSpawn(in) == Refusal::kNotDriving, "not driving (paused)");
    in = Good();
    in.world_updates = kMinWorldUpdates - 1;
    Check(DecideSpawn(in) == Refusal::kTooSoon, "world too young");
    in.world_updates = kMinWorldUpdates;
    Check(DecideSpawn(in) == Refusal::kNone, "world old enough at the limit");
    in = Good();
    in.layout_pool = false;
    Check(DecideSpawn(in) == Refusal::kNoLayoutPool, "pool unreadable");
    in = Good();
    in.layout_free = 0;
    Check(DecideSpawn(in) == Refusal::kPoolFull, "no free slot");
    in.layout_free = kMinFreeLayoutSlots - 1;
    Check(DecideSpawn(in) == Refusal::kPoolFull, "one free slot is too few");
    in.layout_free = kMinFreeLayoutSlots;
    Check(DecideSpawn(in) == Refusal::kNone, "two free slots are enough");
    in = Good();
    in.type_known = false;
    Check(DecideSpawn(in) == Refusal::kUnknownType, "unknown model");
    in = Good();
    in.position_ok = false;
    Check(DecideSpawn(in) == Refusal::kBadPosition, "bad position");
    in = Good();
    in.calls_so_far = kMaxSpawnCalls;
    Check(DecideSpawn(in) == Refusal::kCapReached, "the cap");
    in.calls_so_far = kMaxSpawnCalls - 1;
    Check(DecideSpawn(in) == Refusal::kNone, "one under the cap");
    // the cap wins over everything, so a run that has used its calls never reads the world again
    in = Good();
    in.calls_so_far = kMaxSpawnCalls;
    in.manager = false;
    Check(DecideSpawn(in) == Refusal::kCapReached, "the cap is checked first");
    for (Refusal r : {Refusal::kNone, Refusal::kNoManager, Refusal::kNoPlayer, Refusal::kNotFreeRoam, Refusal::kNotDriving, Refusal::kTooSoon,
                      Refusal::kNoLayoutPool, Refusal::kPoolFull, Refusal::kUnknownType, Refusal::kBadPosition, Refusal::kCapReached}) {
        Check(std::string(RefusalText(r)).size() > 3, "every refusal has a text");
    }
}

void TestSets() {
    int32_t done = 0;
    Check(!NewSpawnSet(0, done), "0 is no set");
    Check(NewSpawnSet(1, done) && done == 1, "0 -> 1 is a set");
    Check(!NewSpawnSet(1, done), "1 again is the same set: no second call");
    Check(!NewSpawnSet(1, done), "and again");
    Check(NewSpawnSet(2, done) && done == 2, "1 -> 2 is a new set");
    Check(!NewSpawnSet(0, done) && done == 0, "0 re-arms");
    Check(NewSpawnSet(2, done), "after the re-arm the same number is a set again");
    int calls = 0;
    int32_t d2 = 0;
    for (int i = 0; i < 1000; ++i) {
        if (NewSpawnSet(7, d2)) ++calls;  // a cvar that stays at 7 for a thousand updates
    }
    Check(calls == 1, "a value held for a thousand updates is one call", calls);
}

void TestPose() {
    // the car's back row (0, 1) points to +Z behind it: the nose is -Z: scene heading 180; the native heading is 0
    Check(Near(NoseHeadingDeg(0.0, 1.0), 180.0), "back +Z: nose heading 180", NoseHeadingDeg(0.0, 1.0));
    Check(Near(NativeHeadingDeg(0.0, 1.0), 0.0), "back +Z: native heading 0");
    // back row (sin a, cos a) for native a = 90: (1, 0): the nose is -X: scene heading 270
    Check(Near(NativeHeadingDeg(1.0, 0.0), 90.0), "back +X: native heading 90");
    Check(Near(NoseHeadingDeg(1.0, 0.0), 270.0), "back +X: nose heading 270");
    // a nose and its native heading differ by 180 (car_verbs.h NativeHeadingDeg)
    for (double a = 0.0; a < 360.0; a += 7.5) {
        const double r = a * kPi / 180.0;
        const double bx = std::sin(r), bz = std::cos(r);
        Check(Near(NativeHeadingDeg(bx, bz), a, 1.0e-6), "native heading round trip", NativeHeadingDeg(bx, bz), a);
        Check(Near(NormalizeDegrees(NoseHeadingDeg(bx, bz) - 180.0), a, 1.0e-6), "nose heading is native + 180", NoseHeadingDeg(bx, bz), a);
    }
    Check(Near(HeadingDelta(350.0, 10.0), 20.0), "delta over the wrap");
    Check(Near(HeadingDelta(10.0, 350.0), -20.0), "delta back over the wrap");
    Check(Near(HeadingDelta(0.0, 180.0), 180.0), "delta of a half turn is +180");
    Check(Near(NormalizeDegrees(-90.0), 270.0) && Near(NormalizeDegrees(720.0), 0.0) && Near(NormalizeDegrees(360.0), 0.0), "normalize");

    Vec3 out;
    // a car at the origin facing -Z (back row +Z): 25 m ahead is z = -25
    Check(PointAhead(Vec3{0, 3, 0}, 0.0, 1.0, 25.0, &out) && Near(out.x, 0.0) && Near(out.z, -25.0) && Near(out.y, 3.0), "25 m ahead of a car facing -Z",
          out.z);
    // facing +X (back row -X)
    Check(PointAhead(Vec3{10, 0, 10}, -1.0, 0.0, 20.0, &out) && Near(out.x, 30.0) && Near(out.z, 10.0), "20 m ahead of a car facing +X", out.x);
    // a back row that is not unit length (a tilted car): the horizontal part is normalised
    Check(PointAhead(Vec3{0, 0, 0}, 0.0, 0.5, 10.0, &out) && Near(out.z, -10.0), "a short back row is normalised", out.z);
    // a car on its nose: no horizontal direction
    Check(!PointAhead(Vec3{1, 2, 3}, 0.0, 0.0, 10.0, &out) && Near(out.x, 1.0) && Near(out.z, 3.0), "no horizontal back row: no point, out = pos");
    Check(!PointAhead(Vec3{1, 2, 3}, std::nan(""), 1.0, 10.0, &out), "a NaN back row: no point");
    Check(PlausibleSpawnPoint(Vec3{0, 0, 0}, Vec3{10, 0, 10}), "a near point is plausible");
    Check(!PlausibleSpawnPoint(Vec3{5000, 0, 0}, Vec3{0, 0, 0}), "5 km away is not");
    Check(!PlausibleSpawnPoint(Vec3{std::nan(""), 0, 0}, Vec3{0, 0, 0}), "NaN is not");
    Check(Near(Distance(Vec3{0, 0, 0}, Vec3{3, 4, 0}), 5.0) && Near(DistanceXZ(Vec3{0, 9, 0}, Vec3{3, 0, 4}), 5.0), "distances");
}

void TestCensus() {
    Census c;
    c.Begin();
    Check(c.Update(100, Vec3{0, 0, 0}, 10.0) < 0.0, "a car seen first has no speed");
    Check(c.Update(200, Vec3{50, 0, 0}, 10.0) < 0.0, "a second car, no speed");
    c.End();
    Check(c.New() == 2 && c.Gone() == 0 && c.Seen() == 2 && c.Tracked() == 2, "two new, none gone", c.New());

    c.Begin();
    const double v = c.Update(100, Vec3{15, 0, 0}, 11.0);  // 15 m in 1 s
    Check(Near(v, 15.0), "15 m in 1 s is 15 m/s", v);
    c.End();  // car 200 is not in this sample
    Check(c.New() == 0 && c.Gone() == 1 && c.Seen() == 1 && c.Tracked() == 1, "one gone", c.Gone());

    c.Begin();
    Check(c.Update(100, Vec3{15, 0, 0}, 12.0) >= 0.0 && Near(c.Update(100, Vec3{15, 0, 0}, 12.0), -1.0), "the same car twice in one instant has no speed (dt 0)");
    Check(c.Update(300, Vec3{1, 1, 1}, 12.0) < 0.0, "a new car");
    c.End();
    Check(c.New() == 1, "one new", c.New());

    // too far apart in time: no speed
    c.Begin();
    Check(c.Update(300, Vec3{100, 1, 1}, 60.0) < 0.0, "a 48 s gap gives no speed");
    c.End();

    // a slot reused after a gone sample is a new car
    Census d;
    d.Begin();
    d.Update(1, Vec3{0, 0, 0}, 0.0);
    d.End();
    d.Begin();
    d.End();  // car 1 gone
    Check(d.Gone() == 1, "gone after an empty sample");
    d.Begin();
    Check(d.Update(1, Vec3{500, 0, 0}, 5.0) < 0.0, "the reused slot is a new car, no speed");
    d.End();
    Check(d.New() == 1, "...and counted as new");
    d.Clear();
    Check(d.Tracked() == 0, "clear");
}

void TestNames() {
    const uint8_t ok[] = {'t', 'a', 'x', 'i', '_', '0', '1', 0, 'x'};
    Check(PrintableName(ok, 16) == "taxi_01", "a plain name");
    const uint8_t empty[] = {0};
    Check(PrintableName(empty, 16).empty(), "an empty name is empty");
    const uint8_t noise[] = {'a', 0x01, 'b', 0};
    Check(PrintableName(noise, 16).empty(), "a control character: not a name");
    const uint8_t hi[] = {'a', 0xC3, 0xA9, 0};
    Check(PrintableName(hi, 16).empty(), "non-ASCII: not a name");
    const uint8_t spaced[] = {'a', ' ', 'b', 0};
    Check(PrintableName(spaced, 16).empty(), "a space: not a name (the model names have none)");
    const uint8_t longer[] = {'a', 'b', 'c', 'd', 'e', 'f'};
    Check(PrintableName(longer, 4).empty(), "no terminator within the limit");
    Check(PrintableName(longer, 6).empty(), "no terminator at the exact limit");
    Check(EqualNoCase("Taxi", "tAXI") && !EqualNoCase("taxi", "taxis") && !EqualNoCase("taxi", "taxj"), "case blind compare");
    Check(FitsLayoutName("a") && FitsLayoutName(std::string(31, 'x')) && !FitsLayoutName(std::string(32, 'x')) && !FitsLayoutName(""), "the layout's name buffer");
}

void TestPools() {
    PoolCounts p;
    Check(!p.Sane(), "an unread descriptor is not sane");
    p.read = true;
    p.free = 10;
    p.live = 20;
    p.pending = 1;
    p.used = 2;
    Check(p.Sane() && p.Total() == 33, "a sane descriptor");
    p.free = PoolCounts::kSane;
    Check(!p.Sane(), "a count at the limit is garbage");
    p.free = 0xDEADBEEF;
    Check(!p.Sane(), "a garbage count is not sane");
}

}  // namespace

int main() {
    TestGuard();
    TestSets();
    TestPose();
    TestCensus();
    TestNames();
    TestPools();
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
