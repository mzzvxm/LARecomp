// Check of the camera rig's maths, track parser and evaluator (camera.h). A program of its own: it needs no game, no ReXGlue and
// no log, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_camera_test     (CMakeLists.txt)
//   <build dir>/larecomp_camera_test                             the checks
//   <build dir>/larecomp_camera_test a.scene b.scene             no checks: reads the camera keys of each scene file and says what
//                                                                the game would say (exit 1 when one has an error)
//
// The compile by hand (the msvc-target clang of labuild, include path src/mc_engine/cinematic):
//   clang++ -std=c++23 camera_test.cpp camera.cpp scene.cpp -o camera_test.exe

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "camera.h"
#include "scene.h"

using namespace mc::cinematic;
using namespace mc::cinematic::camera;

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

void CheckNum(double got, double want, const std::string& what, double tol = 1e-9) {
    ++g_checks;
    if (!(std::fabs(got - want) <= tol)) {
        ++g_failed;
        std::printf("FAILED: %s: got %.12g, want %.12g\n", what.c_str(), got, want);
    }
}

void CheckVec(Vec3 got, Vec3 want, const std::string& what, double tol = 1e-9) {
    ++g_checks;
    if (!(std::fabs(got.x - want.x) <= tol && std::fabs(got.y - want.y) <= tol && std::fabs(got.z - want.z) <= tol)) {
        ++g_failed;
        std::printf("FAILED: %s: got (%.9g, %.9g, %.9g), want (%.9g, %.9g, %.9g)\n", what.c_str(), got.x, got.y, got.z, want.x, want.y,
                    want.z);
    }
}

// A camera basis is a proper rotation: unit, orthogonal, and the game's world is right-handed, so the right is forward x up and
// right x up = BACK = -forward (the slots' rows right, up, back are a proper rotation).
bool Proper(const Basis& b, double tol = 1e-9) {
    const auto unit = [&](Vec3 v) { return std::fabs(Length(v) - 1.0) < tol; };
    return unit(b.right) && unit(b.up) && unit(b.forward) && std::fabs(Dot(b.right, b.up)) < tol && std::fabs(Dot(b.up, b.forward)) < tol &&
           std::fabs(Dot(b.right, b.forward)) < tol && Length(Cross(b.right, b.up) + b.forward) < tol;
}

double AngleBetween(Vec3 a, Vec3 b) {
    return std::acos(std::clamp(Dot(Normalize(a, {1, 0, 0}), Normalize(b, {1, 0, 0})), -1.0, 1.0)) / kDeg;
}

// ---------------------------------------------------------------------------------------------------------------- maths

void TestVectors() {
    CheckVec(Cross({1, 0, 0}, {0, 1, 0}), {0, 0, 1}, "x cross y is z");
    CheckVec(Cross({0, 1, 0}, {0, 0, 1}), {1, 0, 0}, "y cross z is x");
    CheckNum(Dot({1, 2, 3}, {4, 5, 6}), 32.0, "dot");
    CheckNum(Length({3, 4, 0}), 5.0, "length");
    CheckVec(Normalize({0, 0, 0}, {0, 0, 1}), {0, 0, 1}, "a zero vector takes the fallback");
    CheckVec(Normalize({0, 5, 0}, {0, 0, 1}), {0, 1, 0}, "normalize");
    CheckVec(Lerp(Vec3{0, 0, 0}, Vec3{2, 4, 6}, 0.5), {1, 2, 3}, "lerp");
}

void TestBasis() {
    const Basis level = BasisFromYawPitch(0, 0);
    CheckVec(level.forward, {0, 0, 1}, "yaw 0 looks along +Z");
    CheckVec(level.right, {-1, 0, 0}, "yaw 0 looks along +Z (south): right is -X (west), the right-handed world's");
    CheckVec(level.up, {0, 1, 0}, "yaw 0: up is +Y");
    CheckVec(BasisFromYawPitch(90, 0).forward, {1, 0, 0}, "yaw 90 looks along +X");
    CheckVec(BasisFromYawPitch(90, 0).right, {0, 0, 1}, "yaw 90 looks along +X (east): right is +Z (south)");
    CheckVec(BasisFromYawPitch(180, 0).right, {1, 0, 0}, "yaw 180 looks north: right is east");
    CheckVec(Basis{}.right, {-1, 0, 0}, "the default basis looks along +Z with the right at -X");
    Check(Proper(Basis{}), "the default basis is a proper rotation");
    CheckVec(BasisFromYawPitch(-90, 0).forward, {-1, 0, 0}, "yaw -90 looks along -X");
    CheckNum(BasisFromYawPitch(0, 30).forward.y, 0.5, "pitch 30 looks up by sin 30");
    CheckNum(BasisFromYawPitch(0, 300).forward.y, std::sin(89 * kDeg), "pitch is clamped to 89", 1e-12);
    for (int yaw = -180; yaw <= 180; yaw += 45) {
        for (int pitch = -89; pitch <= 89; pitch += 30) {
            Check(Proper(BasisFromYawPitch(yaw, pitch)), "yaw " + std::to_string(yaw) + " pitch " + std::to_string(pitch) + " is a proper rotation");
        }
    }
    // roll
    const Basis rolled = WithRoll(level, 90);
    CheckVec(rolled.right, {0, 1, 0}, "roll +90: the right side is up");
    CheckVec(rolled.up, {1, 0, 0}, "roll +90: up swings to the left (+X, the old right)");
    CheckVec(rolled.forward, {0, 0, 1}, "roll leaves forward alone");
    Check(Proper(WithRoll(BasisFromYawPitch(33, -12), 27.5)), "a roll keeps a proper rotation");
    CheckVec(WithRoll(level, 0).right, level.right, "roll 0 is no change");
    // look at
    const Basis fallback = BasisFromYawPitch(10, 0);
    CheckVec(BasisLookAt({0, 0, 0}, {0, 0, 10}, fallback).forward, {0, 0, 1}, "look along +Z");
    CheckVec(BasisLookAt({0, 0, 0}, {10, 0, 0}, fallback).forward, {1, 0, 0}, "look along +X");
    CheckVec(BasisLookAt({1, 2, 3}, {1, 2, 3}, fallback).forward, fallback.forward, "looking at where you stand keeps the fallback");
    CheckVec(BasisLookAt({0, 5, 0}, {0, 15, 0}, BasisFromYawPitch(90, 0)).forward, {0, 1, 0}, "look straight up");
    Check(Proper(BasisLookAt({0, 5, 0}, {0, 15, 0}, BasisFromYawPitch(90, 0))), "looking straight up is a proper rotation");
    Check(Proper(BasisLookAt({3, 4, 5}, {-20, 1, 40}, fallback)), "look at is a proper rotation");
    CheckNum(BasisLookAt({0, 0, 0}, {3, 4, 12}, fallback).right.y, 0.0, "look at keeps the horizon level (no roll)");
    // heading
    CheckNum(HeadingOf({0, 0, 1}, 9.0), 0.0, "heading of +Z");
    CheckNum(HeadingOf({1, 0, 0}, 9.0), kPi / 2, "heading of +X");
    CheckNum(HeadingOf({0, 1, 0}, 9.0), 9.0, "heading of straight up keeps the old one");
    CheckVec(HeadingBasis(kPi / 2).forward, {1, 0, 0}, "heading basis forward at 90 degrees", 1e-12);
    CheckVec(HeadingBasis(kPi / 2).right, {0, 0, 1}, "heading basis right at 90 degrees (facing east, the right is south)", 1e-12);
    CheckVec(HeadingBasis(kPi).right, {1, 0, 0}, "heading basis right at 180 degrees (facing north, the right is east)", 1e-12);
    Check(Proper(HeadingBasis(1.234)), "heading basis is a proper rotation");
}

void TestQuat() {
    // The round trip through a quaternion, over bases that use each branch of the conversion.
    std::vector<Basis> bases;
    for (int yaw = -170; yaw <= 180; yaw += 50) {
        for (int pitch = -80; pitch <= 80; pitch += 40) bases.push_back(WithRoll(BasisFromYawPitch(yaw, pitch), yaw * 0.3));
    }
    bases.push_back(Basis{});
    bases.push_back(Basis{{1, 0, 0}, {0, 1, 0}, {0, 0, -1}});   // 180 about Y
    bases.push_back(Basis{{-1, 0, 0}, {0, -1, 0}, {0, 0, -1}});  // 180 about X
    bases.push_back(Basis{{1, 0, 0}, {0, -1, 0}, {0, 0, 1}});    // 180 about Z
    int bad = 0;
    for (const Basis& b : bases) {
        const Basis r = ToBasis(ToQuat(b));
        if (Length(r.right - b.right) > 1e-9 || Length(r.up - b.up) > 1e-9 || Length(r.forward - b.forward) > 1e-9) ++bad;
    }
    Check(bad == 0, "a basis comes back from its quaternion (" + std::to_string(bad) + " of " + std::to_string(bases.size()) + " did not)");
    const Quat q = ToQuat(BasisFromYawPitch(40, 20));
    CheckNum(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z, 1.0, "a quaternion from a basis is a unit one");
    CheckVec(Rotate(ToQuat(BasisFromYawPitch(90, 0)), {0, 0, -1}), {1, 0, 0}, "rotating -Z (the slots' back) by the yaw 90 quaternion gives +X, where it looks");
    CheckVec(Rotate(ToQuat(BasisFromYawPitch(90, 0)), {1, 0, 0}), {0, 0, 1}, "rotating +X by the yaw 90 quaternion gives the right, +Z");
}

void TestSlerp() {
    const Basis a = BasisFromYawPitch(0, 0);
    const Basis b = BasisFromYawPitch(90, 0);
    const Quat qa = ToQuat(a), qb = ToQuat(b);
    CheckVec(ToBasis(Slerp(qa, qb, 0.0)).forward, a.forward, "slerp at 0 is a");
    CheckVec(ToBasis(Slerp(qa, qb, 1.0)).forward, b.forward, "slerp at 1 is b");
    CheckVec(ToBasis(Slerp(qa, qb, 0.5)).forward, BasisFromYawPitch(45, 0).forward, "slerp halfway between yaw 0 and 90 is yaw 45");
    // constant angular speed
    for (int i = 1; i <= 4; ++i) {
        const double u = i / 4.0;
        CheckNum(AngleBetween(a.forward, ToBasis(Slerp(qa, qb, u)).forward), 90.0 * u, "slerp turns at a constant rate (u=" + std::to_string(u) + ")", 1e-7);
    }
    // the short way: -q is the same rotation
    const Quat nb{-qb.w, -qb.x, -qb.y, -qb.z};
    CheckVec(ToBasis(Slerp(qa, nb, 0.5)).forward, BasisFromYawPitch(45, 0).forward, "slerp takes the short way with a negated target");
    // across the +-180 wrap
    CheckVec(ToBasis(Slerp(ToQuat(BasisFromYawPitch(170, 0)), ToQuat(BasisFromYawPitch(-170, 0)), 0.5)).forward, {0, 0, -1}, "170 to -170 passes through 180, not 0");
    // equal quaternions
    CheckVec(ToBasis(Slerp(qa, qa, 0.3)).forward, a.forward, "slerp of equal rotations");
    Check(Proper(ToBasis(Slerp(qa, ToQuat(BasisFromYawPitch(179.9, 10)), 0.37))), "slerp stays a proper rotation");
}

void TestEasing() {
    for (const Ease e : {Ease::kLinear, Ease::kIn, Ease::kOut, Ease::kInOut}) {
        CheckNum(EaseCurve(e, 0.0), 0.0, "ease starts at 0");
        CheckNum(EaseCurve(e, 1.0), 1.0, "ease ends at 1");
        double last = 0.0;
        bool monotone = true;
        for (int i = 1; i <= 100; ++i) {
            const double v = EaseCurve(e, i / 100.0);
            if (v < last - 1e-12) monotone = false;
            last = v;
        }
        Check(monotone, "an ease never goes back");
        CheckNum(EaseCurve(e, -1.0), 0.0, "ease clamps below");
        CheckNum(EaseCurve(e, 2.0), 1.0, "ease clamps above");
    }
    Check(EaseCurve(Ease::kIn, 0.25) < 0.25, "in starts slow");
    Check(EaseCurve(Ease::kOut, 0.25) > 0.25, "out starts fast");
    CheckNum(EaseCurve(Ease::kInOut, 0.5), 0.5, "inout is even at the middle");
    CheckNum(EaseCurve(Ease::kInOut, 0.25) + EaseCurve(Ease::kInOut, 0.75), 1.0, "inout is symmetric");
}

void TestFov() {
    CheckNum(LerpFov(30, 90, 0.0), 30.0, "fov at 0", 1e-9);
    CheckNum(LerpFov(30, 90, 1.0), 90.0, "fov at 1", 1e-9);
    const double mid = FovFromFocal(0.5 * (FocalFromFov(30) + FocalFromFov(90)));
    CheckNum(LerpFov(30, 90, 0.5), mid, "fov is interpolated in focal length");
    Check(LerpFov(30, 90, 0.5) < 60.0, "a zoom's middle in focal length is narrower than the angle's middle");
    CheckNum(FovFromFocal(FocalFromFov(55.0)), 55.0, "focal and fov are inverses", 1e-9);
    CheckNum(LerpFov(40, 40, 0.7), 40.0, "a constant fov stays", 1e-9);
}

void TestSpline() {
    const Vec3 p0{0, 0, 0}, p1{10, 0, 0}, p2{20, 5, 0}, p3{30, 5, 10};
    CheckVec(CatmullRom(p0, p1, p2, p3, 0.0), p1, "spline starts at p1");
    CheckVec(CatmullRom(p0, p1, p2, p3, 1.0), p2, "spline ends at p2");
    // a straight run is a straight line, evenly
    CheckVec(CatmullRom({0, 0, 0}, {10, 0, 0}, {20, 0, 0}, {30, 0, 0}, 0.5), {15, 0, 0}, "collinear, evenly spaced: the middle");
    CheckVec(CatmullRom({0, 0, 0}, {10, 0, 0}, {20, 0, 0}, {30, 0, 0}, 0.25), {12.5, 0, 0}, "collinear: a quarter");
    // coincident points do not give NaN
    const Vec3 same = CatmullRom({1, 1, 1}, {1, 1, 1}, {1, 1, 1}, {1, 1, 1}, 0.5);
    CheckVec(same, {1, 1, 1}, "all four points the same");
    const Vec3 dup = CatmullRom({5, 5, 5}, {5, 5, 5}, {9, 5, 5}, {9, 5, 5}, 0.5);
    Check(std::isfinite(dup.x) && std::isfinite(dup.y) && std::isfinite(dup.z), "duplicate neighbours give a finite point");
    CheckVec(CatmullRom({0, 0, 0}, {1, 1, 1}, {1, 1, 1}, {2, 2, 2}, 0.7), {1, 1, 1}, "p1 on p2: stays");
    // C1 where two segments meet: the same point and the same direction
    const Vec3 q[5] = {{0, 0, 0}, {10, 0, 0}, {20, 8, 0}, {25, 20, 6}, {40, 20, 6}};
    CheckVec(CatmullRom(q[0], q[1], q[2], q[3], 1.0), CatmullRom(q[1], q[2], q[3], q[4], 0.0), "segments meet at the key");
    const double e = 1e-5;
    const Vec3 before = CatmullRom(q[0], q[1], q[2], q[3], 1.0) - CatmullRom(q[0], q[1], q[2], q[3], 1.0 - e);
    const Vec3 after = CatmullRom(q[1], q[2], q[3], q[4], e) - CatmullRom(q[1], q[2], q[3], q[4], 0.0);
    Check(AngleBetween(before, after) < 0.05, "no kink where two segments meet (" + std::to_string(AngleBetween(before, after)) + " degrees)");
    // a sharp corner: no loop and no cusp inside the segment (the path never doubles back on its chord)
    const Vec3 c0{0, 0, 0}, c1{10, 0, 0}, c2{10.5, 0, 0.1}, c3{10.5, 0, 10};
    double last_x = c1.x - 1.0;
    bool forward_only = true;
    for (int i = 0; i <= 50; ++i) {
        const Vec3 p = CatmullRom(c0, c1, c2, c3, i / 50.0);
        if (p.x < last_x - 1e-6) forward_only = false;
        last_x = p.x;
    }
    Check(forward_only, "centripetal: a tight corner has no loop back along x");
    // spacing does not move the ends
    CheckVec(CatmullRom({-1000, 0, 0}, {0, 0, 0}, {1, 0, 0}, {5000, 0, 0}, 0.0), {0, 0, 0}, "wild neighbours, start");
    CheckVec(CatmullRom({-1000, 0, 0}, {0, 0, 0}, {1, 0, 0}, {5000, 0, 0}, 1.0), {1, 0, 0}, "wild neighbours, end");
}

void TestOrbitPoint() {
    const Basis hf = HeadingBasis(0.0);  // looking along +Z (south): the right is -X (west)
    CheckVec(OrbitPoint({0, 0, 0}, hf, 5, 0, 0), {0, 0, -5}, "az 0 is behind", 1e-12);
    CheckVec(OrbitPoint({0, 0, 0}, hf, 5, 90, 0), {-5, 0, 0}, "az 90 is on the right", 1e-12);
    CheckVec(OrbitPoint({0, 0, 0}, hf, 5, 180, 0), {0, 0, 5}, "az 180 is in front", 1e-12);
    CheckVec(OrbitPoint({0, 0, 0}, hf, 5, 270, 0), {5, 0, 0}, "az 270 is on the left", 1e-12);
    CheckVec(OrbitPoint({0, 0, 0}, hf, 5, 0, 90), {0, 5, 0}, "el 90 is straight above", 1e-12);
    CheckVec(OrbitPoint({1, 2, 3}, hf, 5, 0, 0), {1, 2, -2}, "around a target", 1e-12);
    const Basis east = HeadingBasis(kPi / 2);  // looking along +X (east): behind is -X, right is +Z (south)
    CheckVec(OrbitPoint({0, 0, 0}, east, 4, 0, 0), {-4, 0, 0}, "behind a car that faces +X", 1e-12);
    CheckVec(OrbitPoint({0, 0, 0}, east, 4, 90, 0), {0, 0, 4}, "right of a car that faces +X", 1e-12);
    CheckNum(Length(OrbitPoint({0, 0, 0}, hf, 7, 33, 21)), 7.0, "the radius holds", 1e-12);
}

void TestDamped() {
    // from rest to a still target: settles, never overshoots
    Damped d;
    d.Snap(0.0);
    bool overshoot = false;
    for (int i = 0; i < 600; ++i) {
        d.Update(10.0, 1.0 / 60.0, 0.5);
        if (d.x > 10.0 + 1e-12) overshoot = true;
    }
    Check(!overshoot, "a critically damped spring does not overshoot");
    CheckNum(d.x, 10.0, "it settles on the target", 1e-6);
    // the same time cut in different frames gives the same spring
    Damped a, b;
    a.Snap(3.0);
    b.Snap(3.0);
    a.Update(-2.0, 0.1, 0.3);
    for (int i = 0; i < 10; ++i) b.Update(-2.0, 0.01, 0.3);
    CheckNum(a.x, b.x, "one 0.1 s step equals ten 0.01 s steps", 1e-12);
    CheckNum(a.v, b.v, "... and so does the velocity", 1e-12);
    // steady lag behind a target at constant speed: speed * smooth
    Damped f;
    f.Snap(0.0);
    double target = 0.0;
    for (int i = 0; i < 1200; ++i) {
        target += 10.0 / 60.0;
        f.Update(target, 1.0 / 60.0, 0.5);
    }
    CheckNum(target - f.x, 10.0 * 0.5, "a target at 10 per second is trailed by speed x smooth", 0.1);
    // smooth 0 follows at once
    Damped z;
    z.Snap(0.0);
    CheckNum(z.Update(5.0, 0.016, 0.0), 5.0, "smooth 0 follows at once");
    // no time, no move
    Damped n;
    n.Snap(1.0);
    CheckNum(n.Update(9.0, 0.0, 0.4), 1.0, "dt 0 does not move");
    // vectors and angles
    Damped3 v;
    v.Snap({0, 0, 0});
    for (int i = 0; i < 600; ++i) v.Update({1, 2, 3}, 1.0 / 60.0, 0.2);
    CheckVec(v.Value(), {1, 2, 3}, "a vector spring settles", 1e-6);
    DampedAngle ang;
    ang.Snap(170 * kDeg);
    double prev = std::fabs(std::remainder(-170 * kDeg - ang.Value(), 2 * kPi));
    bool shrinking = true;
    for (int i = 0; i < 120; ++i) {
        ang.Update(-170 * kDeg, 1.0 / 60.0, 0.3);
        const double gap = std::fabs(std::remainder(-170 * kDeg - ang.Value(), 2 * kPi));
        if (gap > prev + 1e-9) shrinking = false;
        prev = gap;
    }
    Check(shrinking, "an angle spring from 170 to -170 degrees goes the short way (across 180)");
    CheckNum(std::fabs(std::remainder(-170 * kDeg - ang.Value(), 2 * kPi)), 0.0, "... and arrives", 1e-3);
}

// ---------------------------------------------------------------------------------------------------------------- the parser

BuildResult Build(const std::string& text, ParseResult* parsed = nullptr) {
    ParseResult r = ParseScene(text);
    BuildResult b = BuildTrack(r.scene);
    if (parsed) *parsed = std::move(r);
    return b;
}

const Diagnostic* Find(const BuildResult& r, const std::string& part) {
    for (const Diagnostic& d : r.diagnostics) {
        if (d.message.find(part) != std::string::npos) return &d;
    }
    return nullptr;
}

// One bad camera line after a good first key: the build must fail on this line with `part` in the message.
void CheckBad(const std::string& line, const std::string& part) {
    const BuildResult r = Build("scene t\ncam 0 pos 0 0 0 yaw 0 pitch 0\n" + line + "\nend 20\n");
    const Diagnostic* d = Find(r, part);
    Check(!r.Ok(), "'" + line + "' is an error");
    Check(d != nullptr, "'" + line + "' says '" + part + "'");
    if (d) {
        Check(d->severity == Severity::kError, "'" + line + "' is an error, not a warning");
        Check(d->line == 3, "'" + line + "' is reported on line 3, not " + std::to_string(d->line));
    }
}

void TestParser() {
    BuildResult none = Build("scene t\nat 1 hour 12\nend 5\n");
    Check(none.Ok() && none.track.Empty() && none.diagnostics.empty(), "a scene without camera keys: an empty track and nothing said");

    const std::string text =
        "scene t\n"
        "cam 0 pos 800 5 -800 yaw 80 pitch -4 roll 0 fov 40\n"
        "cam 4 look car:player +0 1.2 0 pos 830 3 -790 fov 35 ease inout\n"
        "cam 6 attach car:player 0 2 -6 follow 0.15 heading fov 45\n"
        "cut 8\n"
        "cam 8 orbit car:p1 r 7 az 0 el 12 fov 30\n"
        "cam 10 orbit car:p1 r 9 az 180 el 20 at 0 1 0\n"
        "cam 12 off\n"
        "end 14\n";
    ParseResult parsed;
    const BuildResult r = Build(text, &parsed);
    Check(parsed.Ok(), "the scene parser takes the camera lines");
    Check(r.Ok(), "the camera keys build");
    if (!r.Ok()) {
        for (const Diagnostic& d : r.diagnostics) std::printf("  %s\n", FormatDiagnostic(d, "t").c_str());
    }
    const CameraTrack& t = r.track;
    Check(t.pos.size() == 5, "five position keys (pos, pos, attach, orbit, orbit)");
    Check(t.aim.size() == 4, "four aim keys (yaw/pitch, look, and the two orbits' automatic ones; the attach names none)");
    Check(t.fov.size() == 4, "four fov keys");
    Check(t.roll.size() == 1, "one roll key");
    Check(t.cuts.size() == 1 && t.cuts[0] == 8.0, "one cut at 8");
    Check(t.gates.size() == 2 && t.gates[0].on && t.gates[0].t == 0.0 && !t.gates[1].on && t.gates[1].t == 12.0, "take over at 0, give back at 12");
    Check(t.cars.size() == 2 && t.cars[0] == "player" && t.cars[1] == "p1", "the cars used: player, p1");
    if (t.pos.size() == 5) {
        Check(t.pos[0].kind == PosKind::kWorld && t.pos[0].point.x == 800 && t.pos[0].point.z == -800, "the first position");
        Check(t.pos[1].kind == PosKind::kWorld && t.pos[1].ease == Ease::kInOut, "the second key carries its ease");
        Check(t.pos[2].kind == PosKind::kAttach && t.pos[2].car == "player" && t.pos[2].point.y == 2 && t.pos[2].point.z == -6 &&
                  t.pos[2].heading_only && t.pos[2].follow == 0.15,
              "the attach key: car, offset, heading, follow");
        Check(t.pos[3].kind == PosKind::kOrbit && t.pos[3].car == "p1" && t.pos[3].radius == 7 && t.pos[3].elevation == 12, "the orbit key");
        Check(t.pos[4].point.y == 1 && t.pos[4].azimuth == 180, "the second orbit key: azimuth and the target offset");
    }
    if (t.aim.size() == 4) {
        Check(t.aim[0].kind == AimKind::kAngles && t.aim[0].yaw == 80 && t.aim[0].pitch == -4, "the first aim: yaw and pitch");
        Check(t.aim[1].kind == AimKind::kLookCar && t.aim[1].car == "player" && t.aim[1].point.y == 1.2, "the second aim: look at the player's car, +0 1.2 0");
        Check(t.aim[2].kind == AimKind::kLookCar && t.aim[2].car == "p1" && t.aim[3].point.y == 1, "an orbit aims at its car by itself, at its `at` point");
    }
    // shots: take-over at 0 and the cut at 8
    Check(t.shots.size() == 2 && t.shots[0] == 0.0 && t.shots[1] == 8.0, "shots start at 0 and 8");
    Check(t.ShotAt(-1) == 0 && t.ShotAt(0) == 1 && t.ShotAt(7.99) == 1 && t.ShotAt(8) == 2 && t.ShotAt(20) == 2, "the shot of a time");
    Check(t.OnAt(0) && t.OnAt(11.99) && !t.OnAt(12) && !t.OnAt(-1), "on from 0 to 12");
    Check(Summarize(t).find("5 position keys") != std::string::npos && Summarize(t).find("1 cut") != std::string::npos, "the summary");

    // a take-over after a hand-back
    const BuildResult again = Build("scene t\ncam 0 pos 0 0 0 yaw 0\ncam 2 off\ncam 5 pos 9 9 9 yaw 90\nend 9\n");
    Check(again.Ok() && again.track.gates.size() == 3 && again.track.shots.size() == 2, "off, then a new take-over: three gates, shots at 0 and 5");

    // errors, each on its line
    CheckBad("cam 1 pos 1 2", "needs three numbers");
    CheckBad("cam 1 pos 1 2 x", "needs three numbers");
    CheckBad("cam 1 attach p1", "needs a car");
    CheckBad("cam 1 attach car:p-1", "needs a car");
    CheckBad("cam 1 attach car:p1 1 2", "needs three numbers");
    CheckBad("cam 1 pos 1 2 3 attach car:p1", "two positions");
    CheckBad("cam 1 yaw", "'yaw' needs a number");
    CheckBad("cam 1 yaw 10 look 1 2 3", "two ways to aim");
    CheckBad("cam 1 look", "'look' needs three numbers");
    CheckBad("cam 1 fov 0", "fov 0.00");
    CheckBad("cam 1 fov 200", "fov 200.00");
    CheckBad("cam 1 near 5 far 2", "not beyond near");
    CheckBad("cam 1 near -1", "near -1.00");
    CheckBad("cam 1 ease bouncy", "bad ease");
    CheckBad("cam 1 ease", "bad ease");
    CheckBad("cam 1 sparkle 3", "unknown word 'sparkle'");
    CheckBad("cam 1 ease inout", "nothing it can do");
    CheckBad("cam 1 az 40", "belong to 'orbit");
    CheckBad("cam 1 pos 1 2 3 follow 1", "'follow' lags an attach");
    CheckBad("cam 1 pos 1 2 3 heading", "'heading' belongs");
    CheckBad("cam 1 attach car:p1 follow 99", "follow 99.00");
    CheckBad("cam 1 orbit car:p1 r 0", "orbit r 0.00");
    CheckBad("cam 1 orbit car:p1 el 90", "orbit el 90.00");
    CheckBad("cam 1 off now", "takes nothing else");
    CheckBad("cam 1 look 1 2 3 aimfollow 1", "'aimfollow' lags");
    // the first take-over has to say where and which way
    {
        const BuildResult a = Build("scene t\ncam 0 pos 1 2 3\nend 9\n");
        Check(!a.Ok() && Find(a, "no aim") != nullptr, "a first key without an aim is an error");
        const BuildResult b = Build("scene t\ncam 0 yaw 1 pitch 2\nend 9\n");
        Check(!b.Ok() && Find(b, "no position") != nullptr, "a first key without a position is an error");
        const BuildResult c = Build("scene t\ncam 0 fov 40\nend 9\n");
        Check(!c.Ok() && Find(c, "no position and no aim") != nullptr, "a first key with neither is an error");
        const BuildResult d = Build("scene t\ncam 0 pos 1 2 3\ncam 0 yaw 5\nend 9\n");
        Check(d.Ok(), "position and aim in two statements at the first time are fine");
        const BuildResult o = Build("scene t\ncam 0 orbit car:player r 6 az 0 el 5\nend 9\n");
        Check(o.Ok(), "an orbit alone is a position and an aim");
    }
    // warnings
    {
        const BuildResult w = Build("scene t\ncam 0 pos 0 0 0 yaw 0\ncam 5 off\ncam 6 off\nend 9\n");
        Check(w.Ok() && Find(w, "nothing to give back") != nullptr, "an off while it is off is a warning");
        const BuildResult c = Build("scene t\ncam 0 pos 0 0 0 yaw 0\ncut 5\nend 9\n");
        Check(c.Ok() && Find(c, "no camera key at or after it") != nullptr, "a cut nothing follows is a warning");
        const BuildResult n = Build("scene t\ncut 3\nend 9\n");
        Check(n.Ok() && Find(n, "without any camera key") != nullptr, "a cut with no camera track is a warning");
        const BuildResult p = Build("scene t\ncam 0 pos 0 0 0 yaw 0 pitch 95\nend 9\n");
        Check(p.Ok() && Find(p, "clamped") != nullptr, "a pitch past 89 is a warning");
    }
}

// ---------------------------------------------------------------------------------------------------------------- the rig

struct FakeCars : CarSource {
    std::map<std::string, CarPose> cars;
    int reads = 0;
    bool Get(const std::string& id, CarPose* out) override {
        ++reads;
        const auto it = cars.find(id);
        if (it == cars.end()) return false;
        *out = it->second;
        return true;
    }
};

CarPose CarAt(Vec3 pos, double yaw_deg = 0, double pitch_deg = 0) {
    CarPose p;
    p.position = pos;
    p.frame = BasisFromYawPitch(yaw_deg, pitch_deg);
    return p;
}

// Steps a rig to a scene time in frames of at most 0.1 s (the scene clock never steps more than 0.25 s; a bigger step is a jump
// and cuts), and returns the last frame.
struct Player {
    Rig rig;
    double t = 0.0;
    bool started = false;
    View Go(const CameraTrack& track, CarSource& cars, double to) {
        if (!started) {
            started = true;
            t = to;
            return rig.Evaluate(track, to, cars);
        }
        View v;
        while (t < to - 1e-12) {
            t = std::min(t + 0.1, to);
            v = rig.Evaluate(track, t, cars);
        }
        if (to <= t - 1e-12) {  // a step back
            t = to;
            v = rig.Evaluate(track, t, cars);
        }
        return v;
    }
};

CameraTrack Track(const std::string& text) {
    BuildResult r = Build(text);
    if (!r.Ok()) {
        std::printf("test scene does not build:\n");
        for (const Diagnostic& d : r.diagnostics) std::printf("  %s\n", FormatDiagnostic(d, "t").c_str());
    }
    return std::move(r.track);
}

// The rows the game's camera slots hold: right, up and BACK. The first smoke run wrote the forward vector into the third row, so the
// camera looked away from its car and no cut was ever met; the numbers below are the game's own camera (the E5 run's chase camera at
// the car's spawn), and the sides come from the smoke run's first picture (tour_02: the blue Murcielago left of the player's car, the
// red one right of it).
void TestSlotRows() {
    // the round trip, and the rows are a proper rotation: right x up = back
    bool round = true, rows_proper = true;
    for (int yaw = -170; yaw <= 180; yaw += 50) {
        for (int pitch = -80; pitch <= 80; pitch += 40) {
            const Basis b = WithRoll(BasisFromYawPitch(yaw, pitch), yaw * 0.3);
            const SlotRows rows = ToSlotRows(b);
            const Basis r = FromSlotRows(rows);
            if (Length(r.right - b.right) > 1e-12 || Length(r.up - b.up) > 1e-12 || Length(r.forward - b.forward) > 1e-12) round = false;
            if (Length(Cross(rows.right, rows.up) - rows.back) > 1e-9 || Length(rows.back + b.forward) > 1e-12) rows_proper = false;
        }
    }
    Check(round, "a basis comes back from its slot rows");
    Check(rows_proper, "the slot rows are right, up, BACK: right x up = back = minus forward");
    CheckVec(ToSlotRows(BasisLookAt({0, 0, 0}, {0, 0, 10}, Basis{})).back, {0, 0, -1}, "a camera that looks at +Z has -Z in the third row");
    CheckVec(ToSlotRows(BasisFromYawPitch(90, 0)).back, {-1, 0, 0}, "a camera that looks along +X has -X in the third row");

    // The game's own camera: third row (0.4737, 0.0670, -0.8782), second row (-0.0168, 0.9976, 0.0671), at (823.84, 19.61, -816.17);
    // the player's car it follows is at (819.61, 17.56, -808.41): the camera looks toward the car, along MINUS the third row.
    const Vec3 up{-0.0168, 0.9976, 0.0671}, back{0.4737, 0.0670, -0.8782}, at{823.84, 19.61, -816.17};
    SlotRows game;
    game.up = Normalize(up, {0, 1, 0});
    game.back = Normalize(back, {0, 0, -1});
    game.right = Cross(game.up, game.back);  // menu_camera.cpp's recipe, "the one the game takes"
    const Basis seen = FromSlotRows(game);
    const Vec3 car = Vec3{819.61, 17.56, -808.41} - at;
    Check(Dot(seen.forward, Normalize(car, {0, 0, 1})) > 0.95, "the game's camera looks toward the car it follows (forward is minus the third row)");
    Check(Dot(game.back, Normalize(car, {0, 0, 1})) < -0.95, "the third row points away from the car it follows: it is the BACK");
    const Vec3 murcielago = Vec3{817.0, 17.2, -797.3} - at;   // p1: left of the player's car on screen
    const Vec3 red_car = Vec3{809.4, 17.6, -798.0} - at;      // p2: right of it
    Check(Dot(seen.right, murcielago) < 0.0, "a car at x 817 is on the LEFT of that camera (the right is forward x up)");
    Check(Dot(seen.right, red_car) > 0.0, "a car at x 809 is on the RIGHT of that camera");

    // The rig's camera written to the slots looks at its car: the third row points from the car to the camera.
    FakeCars cars;
    cars.cars["p1"] = CarAt({817.0, 17.2, -797.3}, 62);
    const CameraTrack tr = Track("scene t\ncam 0 orbit car:p1 r 9 az 20 el 8 fov 40\nend 99\n");
    Rig rig;
    const View v = rig.Evaluate(tr, 0.0, cars);
    const SlotRows w = ToSlotRows(v.basis);
    const Vec3 to_camera = Normalize(v.position - Vec3{817.0, 17.2, -797.3}, {0, 0, 1});
    Check(Dot(w.back, to_camera) > 0.95, "an orbit camera's third row points from its car to the camera, so it looks at the car");
    Check(Dot(v.basis.forward, to_camera) < -0.95, "and the camera looks along minus that, at the car");
    // az 20 is 20 degrees toward the car's right: the camera is on the car's right side (the car faces heading 62: forward = (sin, 0, cos))
    const CarPose p1 = cars.cars["p1"];
    Check(Dot(Normalize(v.position - Vec3{817.0, 17.2, -797.3}, {0, 0, 1}), p1.frame.right) > 0.2, "az 20 puts the camera on the car's right side, not its left");
}

void TestRigPath() {
    const CameraTrack tr = Track("scene t\n"
                                 "cam 0 pos 0 0 0 yaw 0 pitch 0 roll 0 fov 30\n"
                                 "cam 2 pos 10 0 0 yaw 90 roll 20 fov 90\n"
                                 "cam 4 pos 20 0 0\n"
                                 "end 9\n");
    FakeCars cars;
    Player rig;
    View v = rig.Go(tr, cars, 0.0);
    Check(v.active && v.took_over && v.cut, "the first frame takes over and is a cut");
    CheckVec(v.position, {0, 0, 0}, "at key 0 the camera is at key 0");
    CheckNum(v.fov, 30.0, "fov at key 0", 1e-9);
    v = rig.Go(tr, cars, 1.0);
    CheckVec(v.position, {5, 0, 0}, "collinear keys at the middle of the segment: the middle", 1e-9);
    Check(v.active && !v.cut && !v.took_over, "no cut mid-segment");
    CheckVec(v.basis.forward, BasisFromYawPitch(45, 0).forward, "the aim is slerped: yaw 45 halfway", 1e-9);
    CheckNum(v.fov, LerpFov(30, 90, 0.5), "fov halfway is in focal length", 1e-9);
    // roll halfway: 10 degrees; the horizon of the aim is level, so right tilts by 10
    const Basis flat = BasisFromYawPitch(45, 0);
    CheckVec(v.basis.right, WithRoll(flat, 10).right, "roll halfway between 0 and 20 is 10", 1e-9);
    v = rig.Go(tr, cars, 2.0);
    CheckVec(v.position, {10, 0, 0}, "at key 1 the camera is at key 1", 1e-9);
    CheckVec(v.basis.forward, BasisFromYawPitch(90, 0).forward, "at key 1 the aim is key 1's", 1e-9);
    v = rig.Go(tr, cars, 3.0);
    CheckVec(v.position, {15, 0, 0}, "the second segment's middle", 1e-9);
    CheckVec(v.basis.forward, BasisFromYawPitch(90, 0).forward, "the aim holds after its last key", 1e-9);
    CheckNum(v.fov, 90.0, "fov holds after its last key", 1e-9);
    v = rig.Go(tr, cars, 3.5);
    v = rig.Go(tr, cars, 4.0);
    v = rig.Go(tr, cars, 4.4);
    CheckVec(v.position, {20, 0, 0}, "the last key's position holds", 1e-9);

    // before the first key the camera is not the scene's
    CameraTrack late = Track("scene t\ncam 3 pos 1 2 3 yaw 0\nend 9\n");
    Player r2;
    View before = r2.Go(late, cars, 1.0);
    Check(!before.active && !before.cut && !before.handed_back, "before the first key the game's camera rules, no cut");
    View on = r2.Go(late, cars, 3.0);
    Check(on.active && on.took_over && on.cut, "at the first key: take over, a cut");
    View next = r2.Go(late, cars, 3.02);
    Check(next.active && !next.cut && !next.took_over, "the frame after: no cut");
    CheckNum(next.fov, 0.0, "a channel no key names stays the game's own (fov 0)");
    CheckNum(next.near_plane, 0.0, "... near 0");
    CheckNum(next.far_plane, 0.0, "... far 0");
}

void TestRigEase() {
    const CameraTrack tr = Track("scene t\ncam 0 pos 0 0 0 yaw 0 ease inout\ncam 4 pos 40 0 0\nend 9\n");
    FakeCars cars;
    Player rig;
    rig.Go(tr, cars, 0.0);
    const View q = rig.Go(tr, cars, 1.0);
    CheckNum(q.position.x, 40.0 * EaseCurve(Ease::kInOut, 0.25), "inout: a quarter of the time is less than a quarter of the way", 1e-9);
    Check(q.position.x < 10.0, "... less than 10");
    const View m = rig.Go(tr, cars, 2.0);
    CheckNum(m.position.x, 20.0, "inout: the middle is the middle", 1e-9);
    // the ease belongs to the key the segment leaves: the second key's linear ease does not apply to the first segment
    const CameraTrack tr2 = Track("scene t\ncam 0 pos 0 0 0 yaw 0\ncam 4 pos 40 0 0 ease in\nend 9\n");
    Player rig2;
    rig2.Go(tr2, cars, 0.0);
    CheckNum(rig2.Go(tr2, cars, 1.0).position.x, 10.0, "the ease of the arriving key does not shape the segment", 1e-9);
}

void TestRigSmooth() {
    // four keys 100 metres apart on a gentle curve, 2 s apart: sampled at 240 Hz the steps change smoothly (no jump at a key)
    const CameraTrack tr = Track("scene t\n"
                                 "cam 0 pos 0 0 0 yaw 0\ncam 2 pos 100 0 20\ncam 4 pos 200 10 60\ncam 6 pos 300 10 80\nend 9\n");
    FakeCars cars;
    Rig rig;
    std::vector<Vec3> p;
    for (int i = 0; i <= 6 * 240; ++i) p.push_back(rig.Evaluate(tr, i / 240.0, cars).position);
    double worst_ratio = 1.0;
    double biggest = 0.0;
    for (size_t i = 2; i < p.size(); ++i) {
        const double a = Length(p[i - 1] - p[i - 2]);
        const double b = Length(p[i] - p[i - 1]);
        biggest = std::max(biggest, b);
        if (a > 1e-9) worst_ratio = std::max(worst_ratio, std::max(a / b, b / a));
    }
    Check(worst_ratio < 1.5, "no jump at the keys: neighbouring steps differ by at most 1.5x (" + std::to_string(worst_ratio) + ")");
    Check(biggest < 0.5, "the largest step is about the speed (" + std::to_string(biggest) + " m at 240 Hz)");
    // the path goes through every key
    CheckVec(p[2 * 240], {100, 0, 20}, "through key 1", 1e-9);
    CheckVec(p[4 * 240], {200, 10, 60}, "through key 2", 1e-9);
    // the same time asked for after different histories gives the same camera
    Rig other;
    other.Evaluate(tr, 5.5, cars);
    const View x = other.Evaluate(tr, 3.3, cars);
    Rig third;
    third.Evaluate(tr, 0.1, cars);
    const View y = third.Evaluate(tr, 3.3, cars);
    CheckVec(x.position, y.position, "a time maps to one camera, whatever came before (no cars, no lag)", 1e-12);
}

void TestRigCuts() {
    const CameraTrack tr = Track("scene t\n"
                                 "cam 0 pos 0 0 0 yaw 0 roll 10 fov 40\n"
                                 "cam 3 pos 30 0 0\n"
                                 "cut 5\n"
                                 "cam 5 pos 500 0 0 yaw 90\n"
                                 "cam 8 pos 530 0 0\n"
                                 "end 9\n");
    FakeCars cars;
    Player rig;
    rig.Go(tr, cars, 0.0);
    View v = rig.Go(tr, cars, 4.0);
    CheckVec(v.position, {30, 0, 0}, "before the cut the last key before it holds: no blend toward the next shot", 1e-9);
    v = rig.Go(tr, cars, 4.98);
    Check(!v.cut, "no cut before 5");
    v = rig.Go(tr, cars, 5.0);
    Check(v.cut && !v.took_over && v.active, "the frame at the cut time is a cut");
    CheckVec(v.position, {500, 0, 0}, "the new shot starts at its key", 1e-9);
    CheckVec(v.basis.forward, BasisFromYawPitch(90, 0).forward, "the new shot's aim", 1e-9);
    v = rig.Go(tr, cars, 5.02);
    Check(!v.cut, "only that frame is a cut");
    // a channel the new shot does not name carries on
    CheckNum(v.fov, 40.0, "fov carries on across the cut", 1e-9);
    CheckVec(v.basis.right, WithRoll(BasisFromYawPitch(90, 0), 10).right, "roll carries on across the cut", 1e-9);
    v = rig.Go(tr, cars, 6.5);
    CheckVec(v.position, {515, 0, 0}, "inside the new shot the keys blend", 1e-9);
    // a time step that skips over the cut time still cuts once
    Rig coarse;
    coarse.Evaluate(tr, 4.9, cars);
    Check(coarse.Evaluate(tr, 5.3, cars).cut, "a step across 5 is a cut");
    Check(!coarse.Evaluate(tr, 5.4, cars).cut, "... once");
    // a restart (time goes back) is a cut and starts over
    Rig re;
    re.Evaluate(tr, 7.0, cars);
    View back = re.Evaluate(tr, 0.5, cars);
    Check(back.cut && back.active, "time going back is a cut");
    // a scrub forward by a lot too
    Rig scrub;
    scrub.Evaluate(tr, 0.5, cars);
    Check(scrub.Evaluate(tr, 6.0, cars).cut, "a jump of more than half a second is a cut");

    // hand-back and a second take-over
    const CameraTrack gap = Track("scene t\ncam 0 pos 0 0 0 yaw 0\ncam 2 off\ncam 5 pos 9 9 9 yaw 90\nend 9\n");
    Player g;
    g.Go(gap, cars, 0.0);
    View a = g.Go(gap, cars, 1.9);
    Check(a.active && !a.cut, "on, no cut");
    View off = g.Go(gap, cars, 2.0);
    Check(!off.active && off.handed_back && off.cut, "the frame the camera is given back is a cut (the game's camera returns)");
    View off2 = g.Go(gap, cars, 2.02);
    Check(!off2.active && !off2.handed_back && !off2.cut, "then the game's camera rules without cuts");
    View on = g.Go(gap, cars, 5.0);
    Check(on.active && on.took_over && on.cut, "the second take-over is a cut");
    CheckVec(on.position, {9, 9, 9}, "and starts at its key", 1e-9);
}

void TestRigInherit() {
    // fov is only named in the second shot: the first runs on the game's own
    const CameraTrack tr = Track("scene t\n"
                                 "cam 0 pos 0 0 0 yaw 0\n"
                                 "cut 4\n"
                                 "cam 6 pos 5 0 0 fov 30\n"
                                 "cam 8 fov 60\n"
                                 "end 9\n");
    FakeCars cars;
    Player rig;
    rig.Go(tr, cars, 0.0);
    CheckNum(rig.Go(tr, cars, 1.0).fov, 0.0, "no fov key in the shot: the game's own", 1e-9);
    rig.Go(tr, cars, 3.9);
    const View s = rig.Go(tr, cars, 4.5);
    // between the cut (4) and the shot's first position key (6) the position runs from the carried value to the key
    CheckVec(s.position, {1.25, 0, 0}, "the carried position blends to the shot's first key (linear from the cut)", 1e-9);
    const View mid = rig.Go(tr, cars, 5.0);
    CheckVec(mid.position, {2.5, 0, 0}, "halfway to it", 1e-9);
    CheckNum(mid.fov, 30.0, "a channel's first key in the shot holds before it", 1e-9);
    CheckNum(rig.Go(tr, cars, 7.0).fov, LerpFov(30, 60, 0.5), "and runs to the next", 1e-9);
    CheckNum(rig.Go(tr, cars, 8.0).fov, 60.0, "and the next key is reached", 1e-9);
}

void TestRigCars() {
    FakeCars cars;
    cars.cars["p1"] = CarAt({100, 0, 0}, 0);
    {
        const CameraTrack tr = Track("scene t\ncam 0 attach car:p1 0 2 -6 look car:p1 0 1 0\nend 9\n");
        Rig rig;
        const View v = rig.Evaluate(tr, 0.0, cars);
        CheckVec(v.position, {100, 2, -6}, "attach 0 2 -6: six metres behind a car that faces +Z, two up", 1e-9);
        // looking at the car, one up: forward points from the camera to (100,1,0)
        CheckVec(v.basis.forward, Normalize(Vec3{100, 1, 0} - v.position, {0, 0, 1}), "look at the car's offset point", 1e-9);
        // the car turns to face +X: the camera swings round behind it
        cars.cars["p1"] = CarAt({100, 0, 0}, 90);
        const View w = rig.Evaluate(tr, 0.01, cars);
        CheckVec(w.position, {94, 2, 0}, "a car that faces +X: the offset turns with it", 1e-9);
    }
    {
        // heading only: the car's pitch does not tilt the camera's offset; a rigid attach follows it
        cars.cars["p1"] = CarAt({0, 0, 0}, 0, 40);
        const CameraTrack heading = Track("scene t\ncam 0 attach car:p1 0 0 -10 heading yaw 0\nend 9\n");
        const CameraTrack rigid = Track("scene t\ncam 0 attach car:p1 0 0 -10 yaw 0\nend 9\n");
        Rig a, b;
        CheckVec(a.Evaluate(heading, 0.0, cars).position, {0, 0, -10}, "heading attach ignores the car's pitch", 1e-9);
        const Vec3 rp = b.Evaluate(rigid, 0.0, cars).position;
        Check(rp.y < -5.0, "a rigid attach follows the pitch (the camera is below a nose-up car)");
    }
    {
        // the follow lag: a car at 10 m/s, `follow 0.5`: the camera trails by speed x lag
        cars.cars["p1"] = CarAt({0, 0, 0}, 0);
        const CameraTrack tr = Track("scene t\ncam 0 attach car:p1 0 0 0 follow 0.5 yaw 0\nend 99\n");
        Rig rig;
        double z = 0.0;
        View v = rig.Evaluate(tr, 0.0, cars);
        CheckVec(v.position, {0, 0, 0}, "the lag starts on the car", 1e-9);
        for (int i = 1; i <= 900; ++i) {
            z += 10.0 / 60.0;
            cars.cars["p1"] = CarAt({0, 0, z}, 0);
            v = rig.Evaluate(tr, i / 60.0, cars);
        }
        // the car's position is held over each 1/60 s step, which shortens the trail by half a step: 5 - 10/60/2
        CheckNum(z - v.position.z, 5.0 - 10.0 / 60.0 / 2.0, "a car at 10 m/s is trailed by about 5 m with follow 0.5", 0.01);
        CheckNum(v.position.x, 0.0, "no sideways drift", 1e-9);
        // a restart snaps the lag
        cars.cars["p1"] = CarAt({50, 0, 50}, 0);
        const View s = rig.Evaluate(tr, 0.0, cars);
        CheckVec(s.position, {50, 0, 50}, "after a restart the camera is on the car again, not lagging", 1e-9);
        Check(s.cut, "... and it is a cut");
    }
    {
        // a lag turns with the heading: the car spins, the camera follows round
        const CameraTrack tr = Track("scene t\ncam 0 attach car:p1 0 0 -10 heading follow 0.3 yaw 0\nend 99\n");
        cars.cars["p1"] = CarAt({0, 0, 0}, 0);
        Rig rig;
        rig.Evaluate(tr, 0.0, cars);
        cars.cars["p1"] = CarAt({0, 0, 0}, 90);
        View v = rig.Evaluate(tr, 1.0 / 60.0, cars);
        Check(v.position.x > -1.0 && v.position.z < -9.5, "the camera is still nearly behind the old heading one frame after the car turned");
        for (int i = 2; i < 200; ++i) v = rig.Evaluate(tr, i / 60.0, cars);
        CheckVec(v.position, {-10, 0, 0}, "and it ends up behind the new heading", 1e-3);
    }
    {
        // look at a moving car with the aim following
        cars.cars["p1"] = CarAt({0, 0, 20}, 0);
        const CameraTrack tr = Track("scene t\ncam 0 pos 0 0 0 look car:p1\nend 99\n");
        Rig rig;
        CheckVec(rig.Evaluate(tr, 0.0, cars).basis.forward, {0, 0, 1}, "look at the car ahead", 1e-9);
        cars.cars["p1"] = CarAt({20, 0, 0}, 0);
        CheckVec(rig.Evaluate(tr, 0.02, cars).basis.forward, {1, 0, 0}, "the car moved to +X: the camera turns to it", 1e-9);
    }
    {
        // orbit: an arc at a constant radius, looking at the car
        cars.cars["p1"] = CarAt({10, 0, 10}, 0);
        const CameraTrack tr = Track("scene t\ncam 0 orbit car:p1 r 10 az 0 el 0\ncam 4 orbit car:p1 r 10 az 180 el 0\nend 99\n");
        Rig rig;
        bool arc = true, aimed = true;
        double min_to_car = 1e9;
        for (int i = 0; i <= 240; ++i) {
            const View v = rig.Evaluate(tr, i / 60.0, cars);
            const double d = Length(v.position - Vec3{10, 0, 10});
            min_to_car = std::min(min_to_car, d);
            if (std::fabs(d - 10.0) > 1e-9) arc = false;
            if (AngleBetween(v.basis.forward, Vec3{10, 0, 10} - v.position) > 1e-3) aimed = false;
        }
        Check(arc, "an orbit from az 0 to 180 keeps its radius all the way (it never cuts through the car; closest " + std::to_string(min_to_car) + ")");
        Check(aimed, "an orbit looks at its car");
        Rig q;
        const View mid = q.Evaluate(tr, 2.0, cars);
        CheckVec(mid.position, {0, 0, 10}, "halfway round (az 90) it is on the car's right (a car that faces +Z has -X on its right)", 1e-9);
    }
    {
        // a car nobody has: the camera holds its last pose and the rig says so
        FakeCars none;
        const CameraTrack tr = Track("scene t\ncam 0 attach car:p9 0 1 0 yaw 0\nend 9\n");
        Rig rig;
        const View v = rig.Evaluate(tr, 0.0, none);
        CheckVec(v.position, {0, 1, 0}, "a missing car: the offset from the origin, level", 1e-9);
        Check(v.missing_cars == 1 && rig.MissingCars().size() == 1 && rig.MissingCars()[0] == "p9", "the rig names the car it could not find");
        none.cars["p9"] = CarAt({7, 0, 7});
        CheckVec(rig.Evaluate(tr, 0.02, none).position, {7, 1, 7}, "it appears: the camera finds it", 1e-9);
        none.cars.clear();
        CheckVec(rig.Evaluate(tr, 0.04, none).position, {7, 1, 7}, "it goes: the last pose holds", 1e-9);
    }
    {
        // each car is asked once a frame, however many keys use it
        FakeCars c;
        c.cars["p1"] = CarAt({0, 0, 0});
        const CameraTrack tr = Track("scene t\ncam 0 attach car:p1 0 1 -5 look car:p1\ncam 4 attach car:p1 3 1 -5\ncam 8 attach car:p1 0 4 -9\ncam 12 attach car:p1 0 2 -4\nend 99\n");
        Rig rig;
        rig.Evaluate(tr, 6.0, c);
        Check(c.reads == 1, "the car is read once a frame (" + std::to_string(c.reads) + ")");
    }
}

void TestRigAimBlend() {
    // a look at a point blended with fixed angles: the middle aims between them
    const CameraTrack tr = Track("scene t\ncam 0 pos 0 0 0 yaw 0\ncam 2 look 10 0 0\nend 9\n");
    FakeCars cars;
    Rig rig;
    rig.Evaluate(tr, 0.0, cars);
    const View mid = rig.Evaluate(tr, 1.0, cars);
    CheckVec(mid.basis.forward, BasisFromYawPitch(45, 0).forward, "yaw 0 blended with a look along +X: yaw 45", 1e-9);
    Check(Proper(mid.basis), "a blended aim is a proper rotation");
    const View end = rig.Evaluate(tr, 2.0, cars);
    CheckVec(end.basis.forward, {1, 0, 0}, "the look at key's aim at its time", 1e-9);
    // the look at a point follows the camera as it moves
    const CameraTrack fly = Track("scene t\ncam 0 pos 0 0 0 look 100 0 0\ncam 2 pos 100 0 -100\nend 9\n");
    Rig f;
    f.Evaluate(fly, 0.0, cars);
    const View at1 = f.Evaluate(fly, 2.0, cars);
    CheckVec(at1.basis.forward, {0, 0, 1}, "flown to (100,0,-100) the look at (100,0,0) is along +Z", 1e-9);
}

// Parse files given on the command line and say what the game would say. Returns 1 when one has an error.
int ParseFiles(int argc, char** argv) {
    int rc = 0;
    for (int i = 1; i < argc; ++i) {
        std::ifstream in(argv[i], std::ios::binary);
        if (!in) {
            std::printf("%s: cannot open\n", argv[i]);
            rc = 1;
            continue;
        }
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        ParseResult pr;
        const BuildResult r = Build(text, &pr);
        if (!pr.Ok()) {
            for (const Diagnostic& d : pr.diagnostics) std::printf("%s\n", FormatDiagnostic(d, argv[i]).c_str());
            std::printf("%s: the scene itself has %d error(s) (not the camera's)\n", argv[i], pr.ErrorCount());
            rc = 1;
            continue;
        }
        for (const Diagnostic& d : r.diagnostics) std::printf("%s\n", FormatDiagnostic(d, argv[i]).c_str());
        std::printf("%s: %s\n", argv[i], r.track.Empty() ? "no camera keys" : Summarize(r.track).c_str());
        if (!r.Ok()) rc = 1;
    }
    return rc;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1) return ParseFiles(argc, argv);
    TestVectors();
    TestBasis();
    TestQuat();
    TestSlerp();
    TestEasing();
    TestFov();
    TestSpline();
    TestOrbitPoint();
    TestDamped();
    TestParser();
    TestSlotRows();
    TestRigPath();
    TestRigEase();
    TestRigSmooth();
    TestRigCuts();
    TestRigInherit();
    TestRigCars();
    TestRigAimBlend();
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
