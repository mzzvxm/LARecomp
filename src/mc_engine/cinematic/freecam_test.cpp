// Check of the free camera's flight, the pad, the chords and the raw track file (freecam.h). A program of its own: it needs no game,
// no ReXGlue and no log, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_freecam_test     (CMakeLists.txt)
//   <build dir>/larecomp_freecam_test                             the checks
//   <build dir>/larecomp_freecam_test --raw flight.cam.csv        no checks: reads a raw track file and says what is in it (exit 1 when it is not one)
//   <build dir>/larecomp_freecam_test --synth flight.cam.csv      no checks: flies a scripted pad flight (the same moves as tours\cin-fly.tour:
//                                                                 forward and turns, a climb, a tilt, a zoom, a cut) and writes the raw track
//   <build dir>/larecomp_freecam_test --compare flight.cam.csv flight.scene [--max-pos m] [--max-aim deg] [--max-roll deg] [--max-fov deg]
//                                                                 evaluates the scene's camera track with the real rig at every sample's time and
//                                                                 says how far it is from the raw track (exit 1 over a bound): the replay follows the flight
//
// The compile by hand (the msvc-target clang of labuild, include path src/mc_engine/cinematic):
//   clang++ -std=c++23 freecam_test.cpp freecam.cpp camera.cpp scene.cpp -o freecam_test.exe

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "camera.h"
#include "freecam.h"
#include "scene.h"

using namespace mc::cinematic;
using namespace mc::cinematic::camera;
using namespace mc::cinematic::freecam;

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

// A flight without smoothing, for the checks of one control at a time.
Params Crisp() {
    Params p;
    p.inertia = 0.0;
    return p;
}

// Runs the flyer for `seconds` at `fps` with the same pad every update.
void Fly(Flyer& f, const Pad& pad, double seconds, double fps, const Params& p) {
    const int n = static_cast<int>(std::lround(seconds * fps));
    for (int i = 0; i < n; ++i) f.Step(pad, 1.0 / fps, p);
}

// ---------------------------------------------------------------------------------------------------------------- the pad

void TestPad() {
    uint8_t s[kGuestPadBytes] = {};
    s[0] = 0x00; s[1] = 0x00; s[2] = 0x01; s[3] = 0x02;  // the packet number
    s[4] = 0x10; s[5] = 0x10;                            // A and Start
    s[6] = 255;                                          // LT
    s[7] = 0;                                            // RT
    s[8] = 0x7F; s[9] = 0xFF;                            // LX +32767
    s[10] = 0x80; s[11] = 0x00;                          // LY -32768
    s[12] = 0x40; s[13] = 0x00;                          // RX +16384
    s[14] = 0xC0; s[15] = 0x00;                          // RY -16384
    const Pad pad = PadFromGuest(s);
    Check(pad.buttons == (kBtnA | kBtnStart), "the buttons are read big endian");
    CheckNum(pad.lt, 1.0, "LT 255 is 1");
    CheckNum(pad.rt, 0.0, "RT 0 is 0");
    CheckNum(pad.lx, 1.0, "LX +32767 is 1");
    CheckNum(pad.ly, -1.0, "LY -32768 is -1 (clamped)");
    CheckNum(pad.rx, 16384.0 / 32767.0, "RX", 1e-9);
    CheckNum(pad.ry, -16384.0 / 32767.0, "RY", 1e-9);

    ClearGuestPad(s);
    Check(((s[4] << 8) | s[5]) == kBtnStart, "the game keeps Start and loses A");
    bool zero = s[6] == 0 && s[7] == 0;
    for (int i = 8; i < 16; ++i) zero = zero && s[i] == 0;
    Check(zero, "the triggers and the sticks are cleared");
    Check(s[0] == 0 && s[1] == 0 && s[2] == 1 && s[3] == 2, "the packet number is left alone");

    double x = 9, y = 9;
    StickResponse(0.1, 0.0, 0.15, &x, &y);
    CheckNum(x, 0.0, "a stick inside the dead zone reads 0");
    CheckNum(y, 0.0, "a stick inside the dead zone reads 0 (y)");
    StickResponse(1.0, 0.0, 0.15, &x, &y);
    CheckNum(x, 1.0, "a full stick is 1");
    StickResponse(0.0, -1.0, 0.15, &x, &y);
    CheckNum(y, -1.0, "a full stick down is -1");
    StickResponse(0.575, 0.0, 0.15, &x, &y);
    CheckNum(x, std::pow(0.5, kStickExponent), "halfway between the zone's edge and the rim is 0.5 bent by the exponent", 1e-9);
    StickResponse(0.3, 0.4, 0.15, &x, &y);
    CheckNum(x / y, 0.75, "the direction is kept");
    double prev = 0.0;
    bool monotone = true;
    for (int i = 0; i <= 100; ++i) {
        StickResponse(i / 100.0, 0.0, 0.15, &x, &y);
        if (x < prev - 1e-12) monotone = false;
        prev = x;
    }
    Check(monotone, "the response never goes down as the stick goes out");
    StickResponse(1.0, 1.0, 0.15, &x, &y);
    CheckNum(std::hypot(x, y), 1.0, "a diagonal corner of the square reads at most 1");
    CheckNum(TriggerResponse(0.04), 0.0, "a light trigger is 0");
    CheckNum(TriggerResponse(1.0), 1.0, "a full trigger is 1");
    CheckNum(TriggerResponse(0.525), 0.5, "a trigger between the zone and the end", 1e-9);
}

// ---------------------------------------------------------------------------------------------------------------- the angles

void TestAngles() {
    const double yaws[] = {-170, -90, -10, 0, 45, 90, 170};
    const double pitches[] = {-85, -30, 0, 60, 85};
    const double rolls[] = {-40, 0, 25, 120};
    for (const double yaw : yaws) {
        for (const double pitch : pitches) {
            for (const double roll : rolls) {
                Pose p;
                p.yaw = yaw;
                p.pitch = pitch;
                p.roll = roll;
                double y = 0, pi = 0, r = 0;
                AnglesFromBasis(BasisOf(p), 0.0, &y, &pi, &r);
                char what[96];
                std::snprintf(what, sizeof(what), "angles round trip yaw %g pitch %g roll %g", yaw, pitch, roll);
                CheckNum(std::fabs(WrapDegrees(y - yaw)), 0.0, what, 1e-6);
                CheckNum(pi, pitch, what, 1e-6);
                CheckNum(std::fabs(WrapDegrees(r - roll)), 0.0, what, 1e-6);
            }
        }
    }
    // looking straight up keeps the yaw it was told
    Basis up;
    up.forward = {0, 1, 0};
    up.up = {0, 0, -1};
    up.right = {1, 0, 0};
    double y = 0, p = 0, r = 0;
    AnglesFromBasis(up, 33.0, &y, &p, &r);
    CheckNum(y, 33.0, "straight up keeps the yaw given");
    CheckNum(p, 90.0, "straight up is pitch 90", 1e-6);
    CheckNum(WrapDegrees(179.0 - -179.0), -2.0, "wrapping 358 degrees gives -2");
    CheckNum(AngleBetween({1, 0, 0}, {0, 1, 0}), 90.0, "the angle between x and y", 1e-9);
    CheckNum(AngleBetween({1, 0, 0}, {-1, 0, 0}), 180.0, "the angle between x and -x", 1e-9);
}

// ---------------------------------------------------------------------------------------------------------------- the flight

void TestMovement() {
    const Params p = Crisp();
    Pad pad;

    {  // forward
        Flyer f;
        f.Seed(Pose{});
        pad.ly = 1.0;
        Fly(f, pad, 1.0, 60, p);
        CheckVec(f.pose().position, {0, 0, 12}, "a full stick forward for a second at 12 m/s goes 12 m along +Z at yaw 0", 1e-9);
    }
    {  // backward is the other way
        Flyer f;
        f.Seed(Pose{});
        pad.ly = -1.0;
        Fly(f, pad, 0.5, 60, p);
        CheckVec(f.pose().position, {0, 0, -6}, "back", 1e-9);
    }
    {  // yaw 90 looks along +X
        Pose s;
        s.yaw = 90.0;
        Flyer f;
        f.Seed(s);
        pad = Pad{};
        pad.ly = 1.0;
        Fly(f, pad, 1.0, 60, p);
        CheckVec(f.pose().position, {12, 0, 0}, "yaw 90: forward is +X", 1e-9);
    }
    {  // strafing is level: right of a camera that looks along +X is -Z
        Pose s;
        s.yaw = 90.0;
        s.pitch = 40.0;
        Flyer f;
        f.Seed(s);
        pad = Pad{};
        pad.lx = 1.0;
        Fly(f, pad, 1.0, 60, p);
        CheckVec(f.pose().position, {0, 0, -12}, "yaw 90, pitch 40: strafing right goes -Z and does not climb", 1e-9);
        // and it matches the camera's own right on level ground
        Pose lvl;
        lvl.yaw = 90.0;
        CheckVec(BasisOf(lvl).right, {0, 0, -1}, "the camera's right at yaw 90 is -Z", 1e-12);
    }
    {  // the triggers are up and down
        Flyer f;
        f.Seed(Pose{});
        pad = Pad{};
        pad.rt = 1.0;
        Fly(f, pad, 1.0, 60, p);
        CheckVec(f.pose().position, {0, 12, 0}, "RT climbs", 1e-9);
        pad.rt = 0.0;
        pad.lt = 1.0;
        Fly(f, pad, 0.5, 60, p);
        CheckVec(f.pose().position, {0, 6, 0}, "LT descends", 1e-9);
    }
    {  // forward follows the pitch
        Pose s;
        s.pitch = 45.0;
        Flyer f;
        f.Seed(s);
        pad = Pad{};
        pad.ly = 1.0;
        Fly(f, pad, 1.0, 60, p);
        const double c = 12.0 * std::sqrt(0.5);
        CheckVec(f.pose().position, {0, c, c}, "pitch 45 looks and flies up", 1e-9);
    }
    {  // the dead zone
        Flyer f;
        f.Seed(Pose{});
        pad = Pad{};
        pad.lx = 0.1;
        pad.ly = -0.1;
        pad.rx = 0.12;
        Fly(f, pad, 1.0, 60, p);
        CheckVec(f.pose().position, {0, 0, 0}, "a stick at 0.1 does not move the camera", 0.0);
        CheckNum(f.pose().yaw, 0.0, "a right stick at 0.12 does not turn it", 0.0);
    }
    {  // the modifiers
        Flyer f;
        f.Seed(Pose{});
        pad = Pad{};
        pad.ly = 1.0;
        pad.buttons = kBtnL3;
        Fly(f, pad, 1.0, 60, p);
        CheckVec(f.pose().position, {0, 0, 48}, "L3 flies four times as fast", 1e-9);
        Flyer g;
        g.Seed(Pose{});
        pad.buttons = kBtnR3;
        Fly(g, pad, 1.0, 60, p);
        CheckVec(g.pose().position, {0, 0, 3}, "R3 flies a quarter as fast", 1e-9);
        Flyer h;
        h.Seed(Pose{});
        pad.buttons = kBtnR3 | kBtnL3;
        Fly(h, pad, 1.0, 60, p);
        CheckVec(h.pose().position, {0, 0, 12}, "both cancel", 1e-9);
    }
    {  // the speed steps: once a press
        Flyer f;
        f.Seed(Pose{});
        Pad right;
        right.buttons = kBtnDpadRight;
        for (int i = 0; i < 10; ++i) f.Step(right, 1.0 / 60, p);
        CheckNum(f.speed_multiplier(), kSpeedStep, "a held D-pad right steps the speed once");
        f.Step(Pad{}, 1.0 / 60, p);
        f.Step(right, 1.0 / 60, p);
        CheckNum(f.speed_multiplier(), kSpeedStep * kSpeedStep, "pressed again it steps again");
        Pad left;
        left.buttons = kBtnDpadLeft;
        f.Step(left, 1.0 / 60, p);
        f.Step(Pad{}, 1.0 / 60, p);
        CheckNum(f.speed_multiplier(), kSpeedStep, "D-pad left steps down", 1e-12);
        for (int i = 0; i < 200; ++i) {
            f.Step(Pad{}, 1.0 / 60, p);
            f.Step(left, 1.0 / 60, p);
        }
        CheckNum(f.speed_multiplier(), kMinSpeedMult, "the speed multiplier has a floor");
        for (int i = 0; i < 200; ++i) {
            f.Step(Pad{}, 1.0 / 60, p);
            f.Step(right, 1.0 / 60, p);
        }
        CheckNum(f.speed_multiplier(), kMaxSpeedMult, "and a ceiling");
        Flyer g;
        g.Seed(Pose{});
        g.Step(right, 1.0 / 60, p);
        Pad fwd;
        fwd.ly = 1.0;
        Fly(g, fwd, 1.0, 60, p);
        CheckNum(g.pose().position.z, 12.0 * kSpeedStep, "a step of the multiplier scales the flight", 1e-9);
        Flyer keep;  // Seed keeps the multiplier
        keep.Seed(Pose{});
        keep.Step(right, 1.0 / 60, p);
        keep.Seed(Pose{});
        CheckNum(keep.speed_multiplier(), kSpeedStep, "a seed keeps the speed multiplier");
    }
}

void TestLookRollZoom() {
    const Params p = Crisp();
    Pad pad;
    {
        Flyer f;
        f.Seed(Pose{});
        pad.rx = 1.0;
        Fly(f, pad, 1.0, 60, p);
        CheckNum(f.pose().yaw, 70.0, "the right stick to the right turns 70 degrees a second to the right (yaw +)", 1e-9);
        pad = Pad{};
        pad.rx = -1.0;
        Fly(f, pad, 2.0, 60, p);
        CheckNum(f.pose().yaw, -70.0, "and to the left", 1e-9);
    }
    {
        Flyer f;
        f.Seed(Pose{});
        pad = Pad{};
        pad.ry = 1.0;
        Fly(f, pad, 1.0, 60, p);
        CheckNum(f.pose().pitch, 70.0, "the right stick up looks up", 1e-9);
        Fly(f, pad, 1.0, 60, p);
        CheckNum(f.pose().pitch, kMaxPitch, "pitch stops at 89 degrees", 1e-9);
        pad.ry = -1.0;
        Fly(f, pad, 5.0, 60, p);
        CheckNum(f.pose().pitch, -kMaxPitch, "and at -89", 1e-9);
    }
    {  // roll
        Flyer f;
        f.Seed(Pose{});
        pad = Pad{};
        pad.buttons = kBtnRb;
        Fly(f, pad, 1.0, 60, p);
        CheckNum(f.pose().roll, -30.0, "RB tilts right: the right side down, roll -30 a second", 1e-9);
        pad.buttons = kBtnLb;
        Fly(f, pad, 2.0, 60, p);
        CheckNum(f.pose().roll, 30.0, "LB tilts left", 1e-9);
        // the horizon really tilts that way: with + roll the camera's right side is above level
        Pose lvl;
        lvl.roll = 30.0;
        Check(BasisOf(lvl).right.y > 0.4, "+ roll lifts the camera's right side");
        pad = Pad{};
        pad.buttons = kBtnY;
        const Events ev = f.Step(pad, 1.0 / 60, p);
        CheckNum(f.pose().roll, 0.0, "Y levels the horizon");
        Check(ev.reset_view, "Y says so");
    }
    {  // zoom
        Pose s;
        s.fov = 50.0;
        Flyer f;
        f.Seed(s);
        pad = Pad{};
        pad.buttons = kBtnDpadUp;
        Fly(f, pad, 1.0, 60, p);
        CheckNum(f.pose().fov, FovFromFocal(FocalFromFov(50.0) * std::exp(0.5)), "D-pad up zooms in: the focal length grows by e^0.5 a second", 1e-7);
        Check(f.pose().fov < 50.0, "zoomed in is narrower");
        pad.buttons = kBtnDpadDown;
        Fly(f, pad, 1.0, 60, p);
        CheckNum(f.pose().fov, 50.0, "and out again the same way", 1e-6);
        Fly(f, pad, 30.0, 60, p);
        CheckNum(f.pose().fov, p.max_fov, "the field of view stops at its widest");
        pad.buttons = kBtnDpadUp;
        Fly(f, pad, 60.0, 60, p);
        CheckNum(f.pose().fov, p.min_fov, "and at its narrowest");
        pad = Pad{};
        pad.buttons = kBtnY;
        f.Step(pad, 1.0 / 60, p);
        CheckNum(f.pose().fov, 50.0, "Y brings the seed's field of view back");
    }
}

void TestInertia() {
    Params p;
    p.inertia = 0.25;
    Pad pad;
    pad.ly = 1.0;
    // the distance after 0.25 s: speed * (T - tau (1 - e^-1)) with T = tau
    const double want = 12.0 * (0.25 - 0.25 * (1.0 - std::exp(-1.0)));
    for (const double fps : {20.0, 40.0, 60.0, 120.0, 240.0}) {
        Flyer f;
        f.Seed(Pose{});
        Fly(f, pad, 0.25, fps, p);
        char what[96];
        std::snprintf(what, sizeof(what), "the start of a flight with inertia at %g updates a second", fps);
        CheckNum(f.pose().position.z, want, what, 1e-9);
    }
    Flyer a, b;
    a.Seed(Pose{});
    b.Seed(Pose{});
    Fly(a, pad, 2.0, 60, p);
    Fly(b, pad, 2.0, 240, p);
    CheckNum(a.pose().position.z, b.pose().position.z, "2 s of flight cover the same distance at 60 and at 240 updates a second", 1e-9);
    // and it coasts: let go of the stick and the camera glides on a little, then stops
    Pad none;
    const double z0 = a.pose().position.z;
    Fly(a, none, 0.1, 60, p);
    const double z1 = a.pose().position.z;
    Fly(a, none, 3.0, 60, p);
    const double z2 = a.pose().position.z;
    Check(z1 > z0 + 0.5, "it glides on after the stick is let go");
    Check(z2 - z1 < 5.0 && z2 > z1, "and the glide is short");
    const double z3 = a.pose().position.z;
    Fly(a, none, 1.0, 60, p);
    CheckNum(a.pose().position.z, z3, "and ends", 0.001);
    // dt is cut at 0.25 s
    Flyer big, cut;
    big.Seed(Pose{});
    cut.Seed(Pose{});
    big.Step(pad, 5.0, Crisp());
    cut.Step(pad, 0.25, Crisp());
    CheckNum(big.pose().position.z, cut.pose().position.z, "an update of 5 s counts as 0.25 s");
    // no time, no motion
    Flyer still;
    still.Seed(Pose{});
    still.Step(pad, 0.0, p);
    CheckVec(still.pose().position, {0, 0, 0}, "an update of no time does not move the camera");
}

void TestChords() {
    const Params p = Crisp();
    Flyer f;
    f.Seed(Pose{});
    Pad back;
    back.buttons = kBtnBack;
    Events ev = f.Step(back, 1.0 / 60, p);
    Check(!ev.toggle_record && !ev.home && !ev.mark_cut, "Back alone does nothing");
    Pad chord;
    chord.buttons = kBtnBack | kBtnA;
    ev = f.Step(chord, 1.0 / 60, p);
    Check(ev.toggle_record && !ev.home && !ev.mark_cut, "Back + A toggles the recording");
    ev = f.Step(chord, 1.0 / 60, p);
    Check(!ev.toggle_record, "held, it does not toggle again");
    f.Step(back, 1.0 / 60, p);
    ev = f.Step(chord, 1.0 / 60, p);
    Check(ev.toggle_record, "pressed again it does");
    chord.buttons = kBtnBack | kBtnX;
    ev = f.Step(chord, 1.0 / 60, p);
    Check(ev.home && !ev.toggle_record, "Back + X is home");
    chord.buttons = kBtnBack | kBtnY;
    ev = f.Step(chord, 1.0 / 60, p);
    Check(ev.mark_cut && !ev.reset_view, "Back + Y marks a cut and does not level the horizon");
    // A without Back is nothing, and the other buttons do nothing with Back held
    f.Step(Pad{}, 1.0 / 60, p);
    Pad a;
    a.buttons = kBtnA;
    ev = f.Step(a, 1.0 / 60, p);
    Check(!ev.toggle_record, "A alone does not record");
    f.Step(Pad{}, 1.0 / 60, p);
    Pad bd;
    bd.buttons = kBtnBack | kBtnDpadRight;
    f.Step(bd, 1.0 / 60, p);
    CheckNum(f.speed_multiplier(), 1.0, "with Back held the D-pad does not change the speed");
    // Back held first, then the face button: also a chord; the face button first, then Back: not (the press is over)
    f.Step(Pad{}, 1.0 / 60, p);
    Pad a_first;
    a_first.buttons = kBtnA;
    f.Step(a_first, 1.0 / 60, p);
    Pad then_back;
    then_back.buttons = kBtnA | kBtnBack;
    ev = f.Step(then_back, 1.0 / 60, p);
    Check(!ev.toggle_record, "A held before Back is not a chord");
    // Y without Back
    f.Step(Pad{}, 1.0 / 60, p);
    Pad y;
    y.buttons = kBtnY;
    ev = f.Step(y, 1.0 / 60, p);
    Check(ev.reset_view && !ev.mark_cut, "Y alone levels the horizon");
    // the sticks still fly while Back is held
    Flyer g;
    g.Seed(Pose{});
    Pad fly;
    fly.buttons = kBtnBack;
    fly.ly = 1.0;
    Fly(g, fly, 1.0, 60, p);
    CheckNum(g.pose().position.z, 12.0, "the sticks still fly with Back held", 1e-9);
}

// ---------------------------------------------------------------------------------------------------------------- the raw track

void TestRaw() {
    std::vector<Sample> in;
    for (int i = 0; i < 5; ++i) {
        Sample s;
        s.t = i / 60.0;
        s.scene_t = i < 3 ? -1.0 : 2.0 + i / 60.0;
        s.frame = 48000u + static_cast<uint32_t>(i);
        s.pose.position = {819.612 + i * 0.2, 17.6, -808.4 - i * 0.1};
        s.pose.yaw = -28.0 + 100.0 * i;  // goes past 180: the file keeps it continuous
        s.pose.pitch = -5.5;
        s.pose.roll = 1.25;
        s.pose.fov = 50.0 - i;
        s.flags = i == 3 ? kFlagCut : 0;
        in.push_back(s);
    }
    std::string text = RawHeader({"started 2026-11-13 14:00:00", "scene none"});
    for (const Sample& s : in) text += RawRow(s);
    Check(text.rfind("# larecomp freecam v1\n", 0) == 0, "the first line is the magic");
    Check(text.find("\nt,scene_t,frame,x,y,z,yaw,pitch,roll,fov,flags\n") != std::string::npos, "the header row follows the notes");
    RawTrack out;
    std::string error;
    Check(ParseRawTrack(text, &out, &error), "a file the writer made reads back: " + error);
    Check(out.notes.size() == 2 && out.notes[0] == "started 2026-11-13 14:00:00", "the notes come back");
    Check(out.samples.size() == in.size(), "every sample comes back");
    for (size_t i = 0; i < std::min(in.size(), out.samples.size()); ++i) {
        const Sample& a = in[i];
        const Sample& b = out.samples[i];
        CheckNum(b.t, a.t, "t", 1e-6);
        CheckNum(b.scene_t, a.scene_t, "scene_t", 1e-6);
        Check(b.frame == a.frame, "frame");
        CheckVec(b.pose.position, a.pose.position, "position", 1e-4);
        CheckNum(b.pose.yaw, a.pose.yaw, "yaw (continuous)", 1e-4);
        CheckNum(b.pose.pitch, a.pose.pitch, "pitch", 1e-4);
        CheckNum(b.pose.roll, a.pose.roll, "roll", 1e-4);
        CheckNum(b.pose.fov, a.pose.fov, "fov", 1e-4);
        Check(b.flags == a.flags, "flags");
    }
    // a Windows file with CRLF line ends and a blank line reads the same
    std::string crlf;
    for (char c : text) {
        if (c == '\n') crlf += '\r';
        crlf += c;
    }
    crlf += "\r\n";
    RawTrack out2;
    Check(ParseRawTrack(crlf, &out2, &error) && out2.samples.size() == in.size(), "CRLF line ends and a trailing blank line read the same");

    const auto bad = [&](const std::string& body, const std::string& what, const std::string& expect) {
        RawTrack t;
        std::string e;
        const bool ok = ParseRawTrack(body, &t, &e);
        Check(!ok, what + " is refused");
        Check(e.find(expect) != std::string::npos, what + ": the message says '" + expect + "' (got '" + e + "')");
    };
    bad("", "an empty file", "empty");
    bad("hello\n", "a file with no magic", "not a flight file");
    bad(std::string(kRawMagic) + "\n", "a file with no header row", "no header row");
    bad(std::string(kRawMagic) + "\nt,x\n", "a wrong header row", "header row");
    bad(RawHeader({}) + "0,1,2\n", "a short row", "fewer");
    bad(RawHeader({}) + "0,-1,1,0,0,0,0,0,0,50,0,9\n", "a long row", "more");
    bad(RawHeader({}) + "0,-1,1,0,0,abc,0,0,0,50,0\n", "a word in a number column", "column 6");
    bad(RawHeader({}) + "0,-1,1,0,0,nan,0,0,0,50,0\n", "a nan", "column 6");
    bad(RawHeader({}) + "1,-1,1,0,0,0,0,0,0,50,0\n0.5,-1,2,0,0,0,0,0,0,50,0\n", "time going backwards", "backwards");
    bad(RawHeader({}) + "1,-1,1,0,0,0,0,0,0,50,0\n0.5,-1,2,0,0,0,0,0,0,50,0\n", "time going backwards (line)", "line 4");

    // the numbers a recording would make
    RawTrack st;
    st.samples.resize(4);
    for (int i = 0; i < 4; ++i) {
        st.samples[static_cast<size_t>(i)].t = i * 0.5;
        st.samples[static_cast<size_t>(i)].pose.position = {i * 5.0, 0, 0};
    }
    st.samples[2].pose.position = {100, 0, 0};
    st.samples[2].flags = kFlagCut;
    st.samples[3].pose.position = {105, 0, 0};
    const Stats stats = Summarize(st);
    Check(stats.samples == 4 && stats.cuts == 1, "the stats count samples and cuts");
    CheckNum(stats.duration, 1.5, "the stats' duration");
    CheckNum(stats.distance, 5.0 + 5.0, "the jump of a cut is not distance flown");
    CheckNum(stats.max_speed, 10.0, "the fastest stretch is 10 m/s");
    const Stats none = Summarize(RawTrack{});
    Check(none.samples == 0 && none.distance == 0.0, "no samples, no stats");
}

// ---------------------------------------------------------------------------------------------------------------- the scene's own meaning

std::string Num4(double v) {
    char b[48];
    std::snprintf(b, sizeof(b), "%.4f", v);
    return b;
}

// One `cam` line with all the channels of a sample.
std::string CamLine(const Sample& s, double t) {
    return "cam " + Num4(t) + " pos " + Num4(s.pose.position.x) + " " + Num4(s.pose.position.y) + " " + Num4(s.pose.position.z) + " yaw " + Num4(s.pose.yaw) +
           " pitch " + Num4(s.pose.pitch) + " roll " + Num4(s.pose.roll) + " fov " + Num4(s.pose.fov) + "\n";
}

class NoCars final : public CarSource {
  public:
    bool Get(const std::string&, CarPose*) override { return false; }
};

// What the scene's camera track says at the times of the samples, against the samples. Returns false when the scene does not build.
struct Errors {
    double pos = 0.0, pos_rms = 0.0, aim = 0.0, roll = 0.0, fov = 0.0;
    double at_pos = 0.0;
    size_t n = 0;
};
bool Compare(const std::vector<Sample>& samples, const std::string& scene_text, Errors* e, std::string* why) {
    ParseResult pr;
    pr = ParseScene(scene_text);
    if (!pr.Ok()) {
        for (const Diagnostic& d : pr.diagnostics) {
            if (d.severity == Severity::kError) *why += FormatDiagnostic(d, "scene") + "\n";
        }
        return false;
    }
    const BuildResult built = BuildTrack(pr.scene);
    if (!built.Ok()) {
        for (const Diagnostic& d : built.diagnostics) {
            if (d.severity == Severity::kError) *why += FormatDiagnostic(d, "camera") + "\n";
        }
        return false;
    }
    Rig rig;
    NoCars cars;
    double sum2 = 0.0;
    for (const Sample& s : samples) {
        const View v = rig.Evaluate(built.track, s.t, cars);
        if (!v.active) continue;
        ++e->n;
        const double dp = Length(v.position - s.pose.position);
        sum2 += dp * dp;
        if (dp > e->pos) {
            e->pos = dp;
            e->at_pos = s.t;
        }
        const Basis want = BasisOf(s.pose);
        e->aim = std::max(e->aim, AngleBetween(v.basis.forward, want.forward));
        // the roll: the angle between the two up vectors once both look the same way is the difference of the rolls; compare through the angles
        double y = 0, p = 0, r = 0;
        AnglesFromBasis(v.basis, 0.0, &y, &p, &r);
        e->roll = std::max(e->roll, std::fabs(WrapDegrees(r - s.pose.roll)));
        e->fov = std::max(e->fov, std::fabs(v.fov - s.pose.fov));
    }
    if (e->n) e->pos_rms = std::sqrt(sum2 / static_cast<double>(e->n));
    return true;
}

void TestSceneMeaning() {
    // a key line in the scene's words is the pose the flight keeps: position, aim, roll and field of view
    const double yaws[] = {-120, 0, 33, 90, 170};
    const double pitches[] = {-60, 0, 25};
    const double rolls[] = {-15, 0, 40};
    int bad = 0;
    for (const double yaw : yaws) {
        for (const double pitch : pitches) {
            for (const double roll : rolls) {
                Sample s;
                s.t = 0.0;
                s.pose.position = {800.5, 12.25, -790.75};
                s.pose.yaw = yaw;
                s.pose.pitch = pitch;
                s.pose.roll = roll;
                s.pose.fov = 42.5;
                const std::vector<Sample> one{s};
                Errors e;
                std::string why;
                const bool ok = Compare(one, "scene a\n" + CamLine(s, 0.0) + "end 5\n", &e, &why);
                if (!ok || e.n != 1 || e.pos > 1e-3 || e.aim > 1e-3 || e.roll > 1e-3 || e.fov > 1e-3) ++bad;
            }
        }
    }
    Check(bad == 0, "a cam key in the scene's words puts the camera exactly where the flight's pose says (" + std::to_string(bad) + " of 45 off)");
}

// ---------------------------------------------------------------------------------------------------------------- a scripted flight

// One control of the script, in updates at 60 a second.
struct Move {
    int from, to;
    double lx, ly, rx, ry, lt, rt;
    uint16_t buttons;
};
struct Script {
    std::vector<Move> moves;
    int length = 0;
};

// The same moves as tours\cin-fly.tour (the chord of the recording at the start and at the end, forward and turns, a climb, a tilt, a zoom,
// a cut mark, home is left out: it needs the game's camera).
Script FlightScript() {
    Script s;
    s.moves = {
        {30, 34, 0, 0, 0, 0, 0, 0, static_cast<uint16_t>(kBtnBack | kBtnA)},   // Back + A: record
        {70, 250, 0, 0.8, 0.25, 0, 0, 0, 0},                                    // forward, turning right
        {160, 250, 0, 0, 0, 0, 0, 0.6, 0},                                      // and climbing
        {250, 400, 0, 1.0, -0.3, 0.1, 0, 0, 0},                                 // turning back, a little up
        {300, 360, 0, 0, 0, 0, 0, 0, kBtnRb},                                   // a tilt to the right
        {400, 490, 0, 0, 0, 0, 0, 0, kBtnDpadUp},                               // a zoom in
        {420, 490, 0.4, 0, 0, 0, 0, 0, 0},                                      // while strafing
        {560, 564, 0, 0, 0, 0, 0, 0, static_cast<uint16_t>(kBtnBack | kBtnY)}, // Back + Y: a cut mark
        {600, 780, 0, 0.6, 0.4, 0, 0, 0, 0},                                    // on in a new direction
        {640, 700, 0, 0, 0, 0, 0.7, 0, 0},                                      // down
        {880, 884, 0, 0, 0, 0, 0, 0, static_cast<uint16_t>(kBtnBack | kBtnA)}, // Back + A: stop
    };
    s.length = 940;
    return s;
}

// Flies the script from `seed` and records between the two chords of Back + A, like the game's glue does.
std::vector<Sample> FlyScript(const Script& script, const Pose& seed, const Params& params, double fps) {
    Flyer f;
    f.Seed(seed);
    std::vector<Sample> out;
    bool recording = false;
    bool pending_cut = false;
    double t = 0.0;
    for (int i = 0; i < script.length; ++i) {
        Pad pad;
        for (const Move& m : script.moves) {
            if (i < m.from || i >= m.to) continue;
            pad.lx += m.lx;
            pad.ly += m.ly;
            pad.rx += m.rx;
            pad.ry += m.ry;
            pad.lt += m.lt;
            pad.rt += m.rt;
            pad.buttons = static_cast<uint16_t>(pad.buttons | m.buttons);
        }
        const Events ev = f.Step(pad, 1.0 / fps, params);
        if (ev.toggle_record) {
            recording = !recording;
            t = 0.0;
            if (recording) {
                out.clear();
                pending_cut = false;
            }
        }
        if (ev.mark_cut) pending_cut = true;
        if (recording) {
            Sample s;
            s.t = t;
            s.frame = 100000u + static_cast<uint32_t>(i);
            s.pose = f.pose();
            s.flags = pending_cut ? kFlagCut : 0;
            pending_cut = false;
            out.push_back(s);
            t += 1.0 / fps;
        }
    }
    return out;
}

void TestScript() {
    const Params p;  // the defaults: inertia and all
    Pose seed;
    seed.position = {819.6, 17.6, -808.4};
    seed.yaw = -28.0;
    seed.fov = 50.0;
    const std::vector<Sample> flight = FlyScript(FlightScript(), seed, p, 60.0);
    Check(flight.size() > 780 && flight.size() < 880, "the recording holds the flight between the two chords (" + std::to_string(flight.size()) + " samples)");
    Check(!flight.empty() && flight.front().t == 0.0, "the recording starts at t = 0");
    bool ordered = true, cut = false, rolled = false;
    double min_fov = 999, max_y = -1e9;
    for (size_t i = 1; i < flight.size(); ++i) {
        if (flight[i].t <= flight[i - 1].t) ordered = false;
    }
    for (const Sample& s : flight) {
        cut = cut || (s.flags & kFlagCut);
        rolled = rolled || s.pose.roll < -5.0;
        min_fov = std::min(min_fov, s.pose.fov);
        max_y = std::max(max_y, s.pose.position.y);
    }
    Check(ordered, "the times only go up");
    Check(cut, "the cut mark is in the file");
    Check(rolled, "the tilt happened");
    Check(min_fov < 40.0, "the zoom happened");
    Check(max_y > seed.position.y + 3.0, "the climb happened");

    // the flight at 240 updates a second ends where the one at 60 does (the inertia is exact in time)
    const std::vector<Sample> hi = FlyScript(FlightScript(), seed, p, 240.0);
    (void)hi;  // the script is in updates: at 240 it is a quarter as long; the comparison is made on the straight pieces in TestInertia

    // the recorder's text: write it, read it, and the rig with every 12th sample as a key follows the flight
    RawTrack track;
    track.samples = flight;
    std::string text = RawHeader({"test flight"});
    for (const Sample& s : flight) text += RawRow(s);
    RawTrack back;
    std::string error;
    Check(ParseRawTrack(text, &back, &error) && back.samples.size() == flight.size(), "the flight's file reads back: " + error);

    std::string scene = "scene fly fps 60 step 1/60\n";
    for (size_t i = 0; i < flight.size(); i += 12) {
        if (flight[i].flags & kFlagCut) scene += "cut " + Num4(flight[i].t) + "\n";
        scene += CamLine(flight[i], flight[i].t);
    }
    scene += "end " + Num4(flight.back().t + 1.0) + "\n";
    Errors e;
    std::string why;
    const bool ok = Compare(flight, scene, &e, &why);
    Check(ok, "the flight as a scene builds: " + why);
    if (ok) {
        std::printf("scripted flight: %zu samples over %.2f s, every 12th as a key: position error max %.3f m (rms %.3f), aim %.2f deg, roll %.2f deg, fov %.2f deg\n",
                    e.n, flight.back().t, e.pos, e.pos_rms, e.aim, e.roll, e.fov);
        Check(e.n == flight.size(), "the rig has the camera on at every sample");
        Check(e.pos < 0.5 && e.aim < 1.0 && e.roll < 1.0 && e.fov < 0.5, "a key every 0.2 s follows the flown flight closely");
    }
}

// ---------------------------------------------------------------------------------------------------------------- the files

std::string ReadFile(const char* path, bool* ok) {
    std::ifstream in(path, std::ios::binary);
    *ok = static_cast<bool>(in);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

int RawFile(const char* path) {
    bool ok = false;
    const std::string text = ReadFile(path, &ok);
    if (!ok) {
        std::printf("%s: cannot open\n", path);
        return 1;
    }
    RawTrack t;
    std::string error;
    if (!ParseRawTrack(text, &t, &error)) {
        std::printf("%s: %s\n", path, error.c_str());
        return 1;
    }
    const Stats s = Summarize(t);
    std::printf("%s: %zu samples over %.2f s, %.1f m flown, fastest %.1f m/s, %zu cut(s)\n", path, s.samples, s.duration, s.distance, s.max_speed, s.cuts);
    return 0;
}

int Synth(const char* path) {
    Pose seed;
    seed.position = {819.6, 17.6, -808.4};
    seed.yaw = -28.0;
    seed.fov = 50.0;
    const std::vector<Sample> flight = FlyScript(FlightScript(), seed, Params{}, 60.0);
    std::string text = RawHeader({"synthetic: the scripted pad flight of freecam_test.cpp"});
    for (const Sample& s : flight) text += RawRow(s);
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        std::printf("%s: cannot write\n", path);
        return 1;
    }
    out << text;
    std::printf("%s: %zu samples\n", path, flight.size());
    return 0;
}

int CompareFiles(int argc, char** argv) {
    const char* raw_path = nullptr;
    const char* scene_path = nullptr;
    double max_pos = -1, max_aim = -1, max_roll = -1, max_fov = -1;
    for (int i = 2; i < argc; ++i) {
        const auto value = [&](double* v) {
            if (i + 1 < argc) *v = std::atof(argv[++i]);
        };
        if (!std::strcmp(argv[i], "--max-pos")) value(&max_pos);
        else if (!std::strcmp(argv[i], "--max-aim")) value(&max_aim);
        else if (!std::strcmp(argv[i], "--max-roll")) value(&max_roll);
        else if (!std::strcmp(argv[i], "--max-fov")) value(&max_fov);
        else if (!raw_path) raw_path = argv[i];
        else if (!scene_path) scene_path = argv[i];
    }
    if (!raw_path || !scene_path) {
        std::printf("usage: --compare flight.cam.csv flight.scene [--max-pos m] [--max-aim deg] [--max-roll deg] [--max-fov deg]\n");
        return 2;
    }
    bool ok = false;
    const std::string raw_text = ReadFile(raw_path, &ok);
    if (!ok) {
        std::printf("%s: cannot open\n", raw_path);
        return 1;
    }
    RawTrack raw;
    std::string error;
    if (!ParseRawTrack(raw_text, &raw, &error)) {
        std::printf("%s: %s\n", raw_path, error.c_str());
        return 1;
    }
    const std::string scene_text = ReadFile(scene_path, &ok);
    if (!ok) {
        std::printf("%s: cannot open\n", scene_path);
        return 1;
    }
    Errors e;
    std::string why;
    if (!Compare(raw.samples, scene_text, &e, &why)) {
        std::printf("%s: the scene does not build:\n%s", scene_path, why.c_str());
        return 1;
    }
    std::printf("compare: %zu of %zu samples on the scene's camera; position error max %.3f m at t=%.2f (rms %.3f), aim %.3f deg, roll %.3f deg, fov %.3f deg\n", e.n,
                raw.samples.size(), e.pos, e.at_pos, e.pos_rms, e.aim, e.roll, e.fov);
    int rc = 0;
    if (e.n != raw.samples.size()) {
        std::printf("compare: the scene's camera is not on at %zu sample(s)\n", raw.samples.size() - e.n);
        rc = 1;
    }
    if (max_pos >= 0 && e.pos > max_pos) { std::printf("compare: position error over %.3f m\n", max_pos); rc = 1; }
    if (max_aim >= 0 && e.aim > max_aim) { std::printf("compare: aim error over %.3f deg\n", max_aim); rc = 1; }
    if (max_roll >= 0 && e.roll > max_roll) { std::printf("compare: roll error over %.3f deg\n", max_roll); rc = 1; }
    if (max_fov >= 0 && e.fov > max_fov) { std::printf("compare: fov error over %.3f deg\n", max_fov); rc = 1; }
    return rc;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 2 && !std::strcmp(argv[1], "--raw")) return RawFile(argv[2]);
    if (argc > 2 && !std::strcmp(argv[1], "--synth")) return Synth(argv[2]);
    if (argc > 1 && !std::strcmp(argv[1], "--compare")) return CompareFiles(argc, argv);
    if (argc > 1) {
        std::printf("usage: freecam_test [--raw file | --synth file | --compare raw scene ...]\n");
        return 2;
    }
    TestPad();
    TestAngles();
    TestMovement();
    TestLookRollZoom();
    TestInertia();
    TestChords();
    TestRaw();
    TestSceneMeaning();
    TestScript();
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
