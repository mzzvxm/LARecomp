// Check of the route reader and sampler (path_follow.h). A program of its own: it needs no game, no ReXGlue and no log, and exits 0 when
// every check holds.
//
//   cmake --build <build dir> --target larecomp_path_test      (CMakeLists.txt)
//   <build dir>/larecomp_path_test [route files ...]          (with files: each is parsed and summarised; exit 1 for one the game would refuse)
//
// The compile by hand (the msvc-target clang of labuild, include path src/mc_engine/cinematic):
//   clang++ -std=c++23 -Wall -Wextra src/mc_engine/cinematic/path_follow_test.cpp src/mc_engine/cinematic/path_follow.cpp -o path_test.exe

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "path_follow.h"

using namespace mc::cinematic;

namespace {

int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_failed;
        std::printf("FAILED: %s\n", what.c_str());
    }
}

void CheckNum(double got, double want, const std::string& what, double tol = 1e-6) {
    ++g_checks;
    if (!(std::fabs(got - want) <= tol)) {
        ++g_failed;
        std::printf("FAILED: %s: got %.9g, want %.9g\n", what.c_str(), got, want);
    }
}

Route Parse(const std::string& text, bool* ok = nullptr, std::string* err = nullptr) {
    Route r;
    std::string e;
    const bool good = ParseRoute(text, &r, &e);
    if (ok) *ok = good;
    if (err) *err = e;
    return r;
}

bool Fails(const std::string& text, const std::string& part) {
    Route r;
    std::string e;
    const bool ok = ParseRoute(text, &r, &e);
    if (ok || e.find(part) == std::string::npos) {
        std::printf("  (route text '%s': ok=%d error '%s', wanted '%s')\n", text.substr(0, 60).c_str(), ok ? 1 : 0, e.c_str(), part.c_str());
        return false;
    }
    return true;
}

PathSampler Build(const std::string& text, const SpeedSpec& speed = {}) {
    PathSampler s;
    std::string e;
    Route r = Parse(text);
    if (!s.Build(r, speed, &e)) std::printf("  (build failed: %s)\n", e.c_str());
    return s;
}

double Dist(const PathSample& a, const PathSample& b) {
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.z - b.z) * (a.z - b.z));
}

double HeadingDiff(double a, double b) {
    double d = std::fabs(a - b);
    return d > 180.0 ? 360.0 - d : d;
}

// ---------------------------------------------------------------------------------------------------------------- the file

void TestParse() {
    {
        bool ok = false;
        const Route r = Parse("# a street\n819.60,-808.40,4.0\n819.62,-807.49,4.0\n\n819.64 -806.59 6\n", &ok);
        Check(ok && r.points.size() == 3, "three points, a comment and a blank line skipped");
        CheckNum(r.points[0].x, 819.60, "x");
        CheckNum(r.points[0].z, -808.40, "z");
        CheckNum(r.points[0].speed, 4.0, "speed");
        CheckNum(r.points[2].speed, 6.0, "blanks work as commas");
        Check(!r.has_y, "no height");
    }
    {
        bool ok = false;
        const Route r = Parse("1,2\n3,4\n", &ok);
        Check(ok && r.points[0].speed < 0.0, "a point with no speed has none (the sampler uses the default)");
    }
    {
        bool ok = false;
        const Route r = Parse("1,2,5,17.5\n3,4,5,17.7\r\n", &ok);
        Check(ok && r.has_y && r.points[1].has_y, "heights on every point");
        CheckNum(r.points[1].y, 17.7, "y");
    }
    {
        bool ok = false;
        const Route r = Parse("1,2,5 # the first\n3,4,5\n", &ok);
        Check(ok && r.points.size() == 2, "a comment after a point");
    }
    {
        // the harness's own files parse as they are
        bool ok = false;
        const Route r = Parse("# W1SHOTS spot sunset-e: Sunset and Vine ... x,z,speed (m/s)\n-588.51,-645.76,4.0\n-584.52,-645.89,4.0\n-580.52,-646.01,4.0\n", &ok);
        Check(ok && r.points.size() == 3, "a route file of the test harness");
    }
    Check(Fails("1,2\n", "at least 2 points"), "one point");
    Check(Fails("", "at least 2 points"), "an empty file");
    Check(Fails("1,2\n3\n", "line 2: a route point is x,z"), "one number on a line");
    Check(Fails("1,2\n3,4,5,6,7\n", "line 2: a route point is x,z"), "five numbers on a line");
    Check(Fails("1,2\n3,x\n", "line 2: 'x' is not a number"), "a word");
    Check(Fails("1,2\n3,nan\n", "line 2: 'nan' is not a number"), "nan");
    Check(Fails("1,2\n3,4,-1\n", "line 2: a speed from 0 to 1000"), "a negative speed");
    Check(Fails("1,2\n3,4,5000\n", "line 2: a speed from 0 to 1000"), "a silly speed");
    Check(Fails("1,2\n999999,4\n", "line 2: a coordinate outside the world"), "outside the world");
    Check(Fails("1,2,5,1\n3,4,5\n", "the height (the fourth number) is on 1 of 2 points"), "a height on some points only");
    Check(Fails("1,2,5\n3,4,5,1\n", "the height (the fourth number) is on 1 of 2 points"), "a height on the last point only");
    {
        std::string big;
        for (size_t i = 0; i < kMaxRoutePoints + 1; ++i) big += std::to_string(i) + ",0\n";
        Check(Fails(big, "more than 20000 points"), "too many points");
        std::string ok;
        for (size_t i = 0; i < kMaxRoutePoints; ++i) ok += std::to_string(i) + ",0\n";
        bool good = false;
        Parse(ok, &good);
        Check(good, "20000 points are fine");
    }
}

void TestSpeedWords() {
    SpeedSpec s;
    std::string e;
    Check(ParseSpeedWord("12", &s, &e) && s.kind == SpeedKind::kMetersPerSecond && s.value == 12.0, "12 m/s");
    Check(ParseSpeedWord("0.5", &s, &e) && s.value == 0.5, "the lowest speed");
    Check(ParseSpeedWord("120", &s, &e) && s.value == 120.0, "the highest speed");
    Check(ParseSpeedWord("x1.5", &s, &e) && s.kind == SpeedKind::kScale && s.value == 1.5, "x1.5");
    Check(ParseSpeedWord("X2", &s, &e) && s.kind == SpeedKind::kScale && s.value == 2.0, "X2 (any case)");
    Check(ParseSpeedWord("0.5x", &s, &e) && s.kind == SpeedKind::kScale && s.value == 0.5, "0.5x");
    Check(ParseSpeedWord("10x", &s, &e) && s.value == 10.0, "the top factor");
    Check(!ParseSpeedWord("0.4", &s, &e) && e.find("from 0.5 to 120 metres a second") != std::string::npos, "under the lowest speed");
    Check(!ParseSpeedWord("121", &s, &e) && e.find("from 0.5 to 120") != std::string::npos, "over the highest speed");
    Check(!ParseSpeedWord("fast", &s, &e) && e.find("metres a second") != std::string::npos, "a word");
    Check(!ParseSpeedWord("x0.01", &s, &e) && e.find("a speed factor from 0.05 to 10") != std::string::npos, "a factor under the lowest");
    Check(!ParseSpeedWord("x11", &s, &e) && e.find("a speed factor from 0.05 to 10") != std::string::npos, "a factor over the highest");
    Check(!ParseSpeedWord("xx", &s, &e), "x alone is not a number");
    Check(!ParseSpeedWord("", &s, &e), "an empty word");
    Check(!ParseSpeedWord("nan", &s, &e), "nan");
    Check(DescribeSpeed({}) == "the route's own speeds", "describe: the file's");
    Check(DescribeSpeed({SpeedKind::kMetersPerSecond, 12.0}) == "12 m/s", "describe: m/s");
    Check(DescribeSpeed({SpeedKind::kScale, 1.5}) == "x1.5 of the route's speeds", "describe: factor");
}

// ---------------------------------------------------------------------------------------------------------------- the sampler

void TestBuild() {
    PathSampler s;
    std::string e;
    Route r = Parse("0,0,10\n0,0.005,10\n");  // two points under 1 cm apart
    Check(!s.Build(r, {}, &e) && e.find("no two points") != std::string::npos, "two points less than 1 cm apart are one");
    r = Parse("0,0,10\n0,0.5,10\n");
    Check(!s.Build(r, {}, &e) && e.find("at least 1 m") != std::string::npos, "a half metre route is no route");
    Check(!s.Valid(), "a failed build is not valid");
    r = Parse("0,0,10\n0,0,10\n0,20,10\n0,20,10\n");  // duplicates are merged
    Check(s.Build(r, {}, &e) && s.PointCount() == 2, "duplicate points are merged");
    CheckNum(s.Length(), 20.0, "length");
    CheckNum(s.Duration(), 2.0, "20 m at 10 m/s is 2 s");
    CheckNum(s.MeanSpeed(), 10.0, "the mean speed");
}

void TestConstantSpeed() {
    const PathSampler s = Build("0,0,10\n0,30,10\n40,30,10\n");  // 30 m north (+z), then 40 m east (+x): 70 m
    CheckNum(s.Length(), 70.0, "length 70");
    CheckNum(s.Duration(), 7.0, "70 m at 10 m/s");
    PathSample p = s.AtTime(0.0);
    CheckNum(p.x, 0.0, "t=0 x");
    CheckNum(p.z, 0.0, "t=0 z");
    CheckNum(p.speed, 10.0, "t=0 speed");
    Check(!p.finished, "not finished at 0");
    p = s.AtTime(1.5);
    CheckNum(p.z, 15.0, "t=1.5 z", 1e-9);
    CheckNum(p.x, 0.0, "t=1.5 x");
    CheckNum(p.s, 15.0, "t=1.5 s");
    p = s.AtTime(5.0);
    CheckNum(p.x, 20.0, "t=5 x (30 m + 20 m of the east leg)", 1e-9);
    CheckNum(p.z, 30.0, "t=5 z");
    p = s.AtTime(7.0);
    Check(p.finished, "finished at the duration");
    CheckNum(p.x, 40.0, "end x");
    CheckNum(p.z, 30.0, "end z");
    CheckNum(p.speed, 0.0, "speed 0 at the end");
    p = s.AtTime(100.0);
    Check(p.finished && p.x == 40.0 && p.z == 30.0 && p.speed == 0.0, "long after the end: still the last point");
    p = s.AtTime(-3.0);
    Check(!p.finished && p.x == 0.0 && p.z == 0.0, "before the start: the first point");
    p = s.AtTime(NAN);
    Check(!p.finished && p.x == 0.0, "a time that is not a number: the first point, nothing thrown");
}

void TestSpeedProfile() {
    // 100 m with the speed going from 5 to 15 m/s along it: v(s) = 5 + 0.1 s
    const PathSampler s = Build("0,0,5\n0,100,15\n");
    const double expect = 100.0 * std::log(15.0 / 5.0) / (15.0 - 5.0);
    CheckNum(s.Duration(), expect, "exact time of a linear speed ramp: L ln(v1/v0) / (v1 - v0)", 1e-9);
    // a numeric integration of ds/dt = v(s) agrees with the closed form
    double pos = 0.0;
    const double dt = 1e-5;
    double t = 0.0;
    double worst = 0.0;
    for (int i = 0; i < 2000000 && pos < 100.0; ++i) {
        pos += (5.0 + 0.1 * pos) * dt;
        t += dt;
        if (i % 20000 == 0) worst = std::max(worst, std::fabs(s.AtTime(t).s - pos));
    }
    CheckNum(worst, 0.0, "AtTime follows the integrated speed profile", 1e-3);
    CheckNum(t, expect, "the numeric time to the end", 2e-4);
    // the speed at a sampled place is the profile's
    const PathSample mid = s.AtDistance(50.0);
    CheckNum(mid.speed, 10.0, "speed at 50 m is 10", 1e-9);
    CheckNum(s.AtTime(s.TimeAtDistance(50.0)).s, 50.0, "TimeAtDistance inverts AtTime", 1e-9);
    CheckNum(s.AtTime(0.0).speed, 5.0, "starts at 5");
    // speed and position are continuous across a point: no jump at the knee of a 3 point route
    const PathSampler k = Build("0,0,5\n0,50,5\n0,100,20\n");
    double last_s = 0.0, last_v = k.AtTime(0.0).speed;
    bool monotonic = true, smooth = true;
    for (double tt = 0.0; tt <= k.Duration(); tt += 0.01) {
        const PathSample q = k.AtTime(tt);
        if (q.s + 1e-12 < last_s) monotonic = false;
        if (std::fabs(q.speed - last_v) > 0.5) smooth = false;
        if (q.s - last_s > 20.0 * 0.01 + 1e-9) smooth = false;
        last_s = q.s;
        last_v = q.speed;
    }
    Check(monotonic, "the car never goes backwards");
    Check(smooth, "no jump in speed or place anywhere (including the knee)");
    CheckNum(k.AtTime(k.Duration()).s, 100.0, "ends at 100 m");
    // a decelerating ramp
    const PathSampler d = Build("0,0,20\n0,100,5\n");
    CheckNum(d.Duration(), 100.0 * std::log(5.0 / 20.0) / (5.0 - 20.0), "a slowing ramp has the same formula", 1e-9);
    CheckNum(d.AtTime(d.Duration() * 0.5).s, d.AtDistance(d.AtTime(d.Duration() * 0.5).s).s, "AtDistance agrees with AtTime");
    Check(d.AtTime(d.TimeAtDistance(30.0)).speed < 20.0 && d.AtTime(d.TimeAtDistance(30.0)).speed > 5.0, "speed in between");
}

void TestSpeedSpecs() {
    const std::string text = "0,0,4\n0,40,8\n0,80,8\n";
    {
        const PathSampler file = Build(text);
        const PathSampler fast = Build(text, {SpeedKind::kScale, 2.0});
        CheckNum(fast.Duration(), file.Duration() / 2.0, "x2 takes half the time", 1e-9);
        const PathSampler constant = Build(text, {SpeedKind::kMetersPerSecond, 20.0});
        CheckNum(constant.Duration(), 4.0, "80 m at a constant 20 m/s is 4 s");
        CheckNum(constant.AtTime(1.0).s, 20.0, "20 m after 1 s");
        CheckNum(constant.AtTime(1.0).speed, 20.0, "speed 20");
        CheckNum(constant.MeanSpeed(), 20.0, "mean speed of a constant speed");
    }
    {
        // a point with no speed has 12 m/s; the speed clamp: 0 becomes 0.5
        const PathSampler none = Build("0,0\n0,24\n");
        CheckNum(none.Duration(), 2.0, "no speed: 12 m/s, 24 m in 2 s");
        const PathSampler stopped = Build("0,0,0\n0,10,0\n");
        CheckNum(stopped.Duration(), 20.0, "a speed of 0 is held at 0.5 m/s: 10 m in 20 s");
        const PathSampler tiny = Build(text, {SpeedKind::kScale, 0.05});
        Check(tiny.Duration() > 0.0 && std::isfinite(tiny.Duration()), "the lowest factor still ends");
        const PathSampler huge = Build("0,0,100\n0,100,100\n", {SpeedKind::kScale, 10.0});
        CheckNum(huge.AtTime(0.0).speed, 120.0, "speeds are held at 120 m/s");
    }
}

void TestHeading() {
    const PathSampler s = Build("0,0,10\n0,30,10\n40,30,10\n");  // north (+z), then east (+x)
    CheckNum(s.AtDistance(0.0).heading, 0.0, "start: along +Z is 0 degrees");
    CheckNum(s.AtDistance(10.0).heading, 0.0, "mid first leg: 0");
    CheckNum(s.AtDistance(50.0).heading, 90.0, "mid second leg: along +X is 90");
    CheckNum(s.AtDistance(70.0).heading, 90.0, "the end keeps the last heading");
    // the corner at 30 m is taken over 4 m: 28 m is still 0, 32 m is still 90, in between it turns
    CheckNum(s.AtDistance(27.9).heading, 0.0, "just before the window");
    CheckNum(s.AtDistance(32.1).heading, 90.0, "just after the window");
    const double h30 = s.AtDistance(30.0).heading;
    CheckNum(h30, 45.0, "at the corner itself: 45 (the chord of an L)", 1e-9);
    double last = s.AtDistance(27.0).heading;
    bool smooth = true;
    for (double d = 27.0; d <= 33.0; d += 0.05) {
        const double h = s.AtDistance(d).heading;
        if (HeadingDiff(h, last) > 6.0) smooth = false;
        last = h;
    }
    Check(smooth, "no heading jump of more than 6 degrees per 5 cm through the corner");
    // west and south
    const PathSampler w = Build("0,0\n-20,0\n");
    CheckNum(w.AtDistance(5.0).heading, 270.0, "along -X is 270");
    const PathSampler so = Build("0,0\n0,-20\n");
    CheckNum(so.AtDistance(5.0).heading, 180.0, "along -Z is 180");
    // a diagonal: the demo street, heading 62
    const double u0 = std::sin(62.0 * 3.14159265358979323846 / 180.0), u1 = std::cos(62.0 * 3.14159265358979323846 / 180.0);
    char buf[200];
    std::snprintf(buf, sizeof(buf), "817,-797.3\n%.4f,%.4f\n", 817.0 + 50.0 * u0, -797.3 + 50.0 * u1);
    CheckNum(Build(buf).AtDistance(20.0).heading, 62.0, "a street at heading 62", 0.01);
    // a short route: the window is the whole route
    CheckNum(Build("0,0\n3,0\n").AtDistance(1.5).heading, 90.0, "a 3 m route");
    // a U turn does not make the heading jump back and forth (the chord of a hairpin)
    const PathSampler u = Build("0,0\n0,20\n3,20\n3,0\n");
    double prev = u.AtDistance(0.0).heading;
    double worst = 0.0;
    for (double d = 0.0; d <= u.Length(); d += 0.1) {
        const double h = u.AtDistance(d).heading;
        worst = std::max(worst, HeadingDiff(h, prev));
        prev = h;
    }
    Check(worst < 40.0, "a hairpin turns without a jump of 40 degrees in 10 cm");
}

void TestHeightAndNearest() {
    const PathSampler h = Build("0,0,10,17.0\n0,40,10,19.0\n");
    const PathSample p = h.AtDistance(10.0);
    Check(p.has_y, "the route has heights");
    CheckNum(p.y, 17.5, "y by the metre", 1e-9);
    const PathSampler flat = Build("0,0,10\n0,40,10\n");
    Check(!flat.AtDistance(10.0).has_y && !flat.has_y(), "a route with no heights says so");

    const PathSampler s = Build("0,0,10\n0,30,10\n40,30,10\n");
    PathSampler::Nearest n = s.NearestTo(2.0, 10.0);
    CheckNum(n.s, 10.0, "nearest: 10 m along");
    CheckNum(n.distance, 2.0, "nearest: 2 m off");
    n = s.NearestTo(0.0, 15.0);
    CheckNum(n.distance, 0.0, "on the route: 0 off");
    n = s.NearestTo(-5.0, -5.0);
    CheckNum(n.s, 0.0, "before the start: the first point");
    CheckNum(n.distance, std::sqrt(50.0), "...at its distance");
    n = s.NearestTo(100.0, 100.0);
    CheckNum(n.s, 70.0, "after the end: the last point");
    n = s.NearestTo(35.0, 40.0);
    CheckNum(n.s, 30.0 + 35.0, "beside the second leg", 1e-9);
    CheckNum(n.distance, 10.0, "...10 off");
    // a route that comes back past itself: the nearer branch wins
    const PathSampler back = Build("0,0\n0,50\n5,50\n5,0\n");
    n = back.NearestTo(4.0, 10.0);
    CheckNum(n.s, 50.0 + 5.0 + 40.0, "the return leg is the nearer one", 1e-9);
}

void TestResample() {
    const PathSampler s = Build("0,0,10\n0,100,10\n");
    std::vector<PathSample> r = s.Resample(48, 8.0);
    Check(r.size() == 12, "100 m at 8 m: 12 destinations");
    CheckNum(r.back().s, 100.0, "the last one is the end");
    CheckNum(r.front().s, 100.0 / 12.0, "the first is not the start", 1e-9);
    bool even = true;
    for (size_t i = 1; i < r.size(); ++i) {
        if (std::fabs((r[i].s - r[i - 1].s) - 100.0 / 12.0) > 1e-9) even = false;
    }
    Check(even, "evenly spread");
    r = s.Resample(5, 1.0);
    Check(r.size() == 5 && std::fabs(r.back().s - 100.0) < 1e-9, "capped at the most the caller takes");
    r = s.Resample(48, 500.0);
    Check(r.size() == 1 && std::fabs(r[0].s - 100.0) < 1e-9, "a route shorter than the spacing: just the end");
    Check(s.Resample(0, 8.0).empty(), "none asked for, none given");
    Check(PathSampler().Resample(10, 5.0).empty(), "an unbuilt sampler gives none");
    const PathSampler t = Build("0,0,10,1\n0,100,10,11\n");
    r = t.Resample(10, 10.0);
    CheckNum(r.front().y, 2.0, "heights come along (1 to 11 over 100 m: 2 at 10 m)", 1e-9);
    // 64 is the game's limit: a long route cut for it never gives more
    std::string text;
    for (int i = 0; i < 3000; ++i) text += "0," + std::to_string(i) + ",10\n";
    Check(Build(text).Resample(60, 1.0).size() == 60, "a 3 km route cut to 60");
}

void TestRoundTripTime() {
    // the car's place is a function of the time alone: another sampler of the same route says the same
    const std::string text = "0,0,6\n10,10,12\n30,10,3\n30,50,14\n";
    const PathSampler a = Build(text);
    const PathSampler b = Build(text);
    bool same = true;
    for (double t = 0.0; t < a.Duration() + 1.0; t += 0.0166667) {
        if (Dist(a.AtTime(t), b.AtTime(t)) > 0.0) same = false;
    }
    Check(same, "two samplers of one route agree at every time");
    // dividing the time differently (30 fps against 60 fps) gives the same places
    bool consistent = true;
    for (int i = 0; i < 300; ++i) {
        const double t30 = i / 30.0;
        const double t60 = (2 * i) / 60.0;
        if (Dist(a.AtTime(t30), a.AtTime(t60)) > 1e-9) consistent = false;
    }
    Check(consistent, "frame 2i at 60 fps is frame i at 30 fps");
    // the distance covered in one small step is the speed times the step (to the slope of the ramp)
    bool step_ok = true;
    for (double t = 0.1; t < a.Duration() - 0.1; t += 0.37) {
        const double dt = 1e-4;
        const double covered = a.AtTime(t + dt).s - a.AtTime(t).s;
        if (std::fabs(covered / dt - a.AtTime(t).speed) > 0.05) step_ok = false;
    }
    Check(step_ok, "the speed reported is the speed the car is covering ground at");
}

int Summarise(int argc, char** argv) {
    int bad = 0;
    for (int i = 1; i < argc; ++i) {
        std::ifstream in(argv[i], std::ios::binary);
        if (!in) {
            std::printf("%s: cannot read\n", argv[i]);
            ++bad;
            continue;
        }
        std::stringstream ss;
        ss << in.rdbuf();
        Route r;
        std::string err;
        if (!ParseRoute(ss.str(), &r, &err)) {
            std::printf("%s: error: %s\n", argv[i], err.c_str());
            ++bad;
            continue;
        }
        PathSampler s;
        if (!s.Build(r, {}, &err)) {
            std::printf("%s: error: %s\n", argv[i], err.c_str());
            ++bad;
            continue;
        }
        const PathSample a = s.AtDistance(0.0), b = s.AtDistance(s.Length());
        std::printf("%s: %zu points, %.1f m, %.1f s at the route's speeds, from (%.1f, %.1f) heading %.0f to (%.1f, %.1f) heading %.0f%s\n", argv[i], s.PointCount(),
                    s.Length(), s.Duration(), a.x, a.z, a.heading, b.x, b.z, b.heading, s.has_y() ? ", with heights" : "");
    }
    return bad == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1) return Summarise(argc, argv);
    TestParse();
    TestSpeedWords();
    TestBuild();
    TestConstantSpeed();
    TestSpeedProfile();
    TestSpeedSpecs();
    TestHeading();
    TestHeightAndNearest();
    TestResample();
    TestRoundTripTime();
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
