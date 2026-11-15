// A check of mouse look's and keyboard steering's pure rules (mouse_look_core.h) that needs no game: the raw delta sum, the relative-mode gate, the look angles and their
// return behind the car, the sensitivity rule, the steering ramp.
//   cmake --build <dir> --target larecomp_mouse_look_test, then run it (exit 0 = passed).
// Or by hand: clang++ -std=c++20 -I src/mc_engine src/mc_engine/mouse_look_test.cpp -o mouse_look_test

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#include "mouse_look_core.h"

namespace {

using namespace larecomp::mouselook;

int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const char* what, int line) {
    ++g_checks;
    if (!ok) {
        ++g_failed;
        std::printf("FAILED line %d: %s\n", line, what);
    }
}
#define CHECK(cond) Check((cond), #cond, __LINE__)

bool Near(double a, double b, double eps) { return std::fabs(a - b) <= eps; }

// ---------------------------------------------------------------------------------------------------------------------------------
// The delta sum
// ---------------------------------------------------------------------------------------------------------------------------------
void TestDeltaSum() {
    DeltaSum s;
    float dx = 1.0f, dy = 1.0f;
    s.Take(dx, dy);
    CHECK(dx == 0.0f && dy == 0.0f);  // nothing added: nothing taken

    s.Add(3.0f, -2.0f);
    s.Add(4.0f, 5.0f);
    s.Take(dx, dy);
    CHECK(dx == 7.0f && dy == 3.0f);
    s.Take(dx, dy);
    CHECK(dx == 0.0f && dy == 0.0f);  // taken once: exchange(0)

    // Fractions add up (a window whose pixel density is not 1 reports fractional counts): ten times 0.3 is 3.0, not 0 and not 10 times a rounded value.
    for (int i = 0; i < 10; ++i) s.Add(0.3f, -0.3f);
    s.Take(dx, dy);
    CHECK(Near(dx, 3.0, 0.02) && Near(dy, -3.0, 0.02));

    s.Add(5.0f, 5.0f);
    s.Clear();
    s.Take(dx, dy);
    CHECK(dx == 0.0f && dy == 0.0f);

    // The UI thread adds while the game thread takes: nothing is lost and nothing counted twice.
    DeltaSum shared;
    std::atomic<bool> go{false};
    constexpr int kEvents = 20000;
    std::thread ui([&] {
        while (!go.load()) {}
        for (int i = 0; i < kEvents; ++i) shared.Add(1.0f, -2.0f);
    });
    double sum_x = 0.0, sum_y = 0.0;
    go.store(true);
    for (int i = 0; i < 2000; ++i) {
        float x = 0.0f, y = 0.0f;
        shared.Take(x, y);
        sum_x += x;
        sum_y += y;
    }
    ui.join();
    float x = 0.0f, y = 0.0f;
    shared.Take(x, y);
    sum_x += x;
    sum_y += y;
    CHECK(Near(sum_x, double(kEvents), 0.5) && Near(sum_y, -2.0 * kEvents, 0.5));
}

// ---------------------------------------------------------------------------------------------------------------------------------
// The sensitivity rule
// ---------------------------------------------------------------------------------------------------------------------------------
void TestSensitivityRule() {
    constexpr double kTwoPi = 6.283185307179586;
    CHECK(kLookRadiansPerCount == 0.0035);
    CHECK(float(kLookRadiansPerCount) == LookParams{}.sens);  // the parameters' default is the rule's

    // 1795 counts are a full circle at the default.
    const double counts_per_circle = kTwoPi / kLookRadiansPerCount;
    CHECK(Near(counts_per_circle, 1795.0, 1.0));
    // ... which is the distance on the desk the header gives (cm = counts / DPI * 2.54).
    CHECK(Near(counts_per_circle / 400.0 * 2.54, 11.4, 0.1));
    CHECK(Near(counts_per_circle / 800.0 * 2.54, 5.7, 0.05));
    CHECK(Near(counts_per_circle / 1600.0 * 2.54, 2.85, 0.05));
    // A quick glance to the side (90 degrees).
    CHECK(Near(RadiansFor(449.0, kLookRadiansPerCount), kTwoPi / 4.0, 0.002));
    // The same hand movement turns twice as far on a mouse with twice the DPI, and the sensitivity row halves it back.
    CHECK(Near(RadiansFor(1600.0, 0.0035), RadiansFor(800.0, 0.007), 1e-12));
    // The rows' values are radians per count: the middle one is the default.
    const double rows[] = {0.0015, 0.0025, 0.0035, 0.005, 0.007, 0.01};
    CHECK(rows[2] == kLookRadiansPerCount);

    // One step of the look: dx counts turn the view by dx * sens, for any frame time.
    for (float dt : {1.0f / 30.0f, 1.0f / 60.0f, 1.0f / 240.0f}) {
        LookState s;
        LookParams p;
        StepLook(s, true, 100.0f, 0.0f, dt, p);
        CHECK(Near(s.yaw, 0.35, 1e-5) && s.pitch == 0.0f);
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------
// The look angles
// ---------------------------------------------------------------------------------------------------------------------------------
void TestLookAngles() {
    LookParams p;
    LookState s;
    // Motion turns the view; pitch follows dy; the signs are plain.
    StepLook(s, true, 100.0f, -50.0f, 1.0f / 60.0f, p);
    CHECK(s.yaw > 0.0f && s.pitch < 0.0f);
    CHECK(Near(s.yaw, 0.35, 1e-5) && Near(s.pitch, -0.175, 1e-5));
    CHECK(s.idle == 0.0f);

    // Inversion flips the axis, not the other.
    LookParams inv = p;
    inv.invert_y = true;
    LookState t;
    StepLook(t, true, 100.0f, -50.0f, 1.0f / 60.0f, inv);
    CHECK(Near(t.yaw, 0.35, 1e-5) && Near(t.pitch, 0.175, 1e-5));
    inv.invert_x = true;
    LookState u;
    StepLook(u, true, 100.0f, -50.0f, 1.0f / 60.0f, inv);
    CHECK(Near(u.yaw, -0.35, 1e-5) && Near(u.pitch, 0.175, 1e-5));

    // The limits: yaw at pi, pitch at 0.7, either way.
    LookState big;
    StepLook(big, true, 100000.0f, 100000.0f, 1.0f / 60.0f, p);
    CHECK(big.yaw == p.yaw_limit && big.pitch == p.pitch_limit);
    StepLook(big, true, -1000000.0f, -1000000.0f, 1.0f / 60.0f, p);
    CHECK(big.yaw == -p.yaw_limit && big.pitch == -p.pitch_limit);

    // A single count is real motion (a raw sensor does not jitter): it resets the idle clock. Under half a count (a fractional leftover) is not.
    LookState idle;
    idle.idle = 0.2f;
    StepLook(idle, true, 1.0f, 0.0f, 1.0f / 60.0f, p);
    CHECK(idle.idle == 0.0f);
    idle.idle = 0.2f;
    StepLook(idle, true, 0.2f, 0.1f, 1.0f / 60.0f, p);
    CHECK(idle.idle > 0.2f);
}

// The view comes back behind the car after a short idle, as the game's own lookaround does (LookaroundDelayTime, then LookaroundDecayRate).
void TestRecentre() {
    LookParams p;  // 0.35 s, rate 4 per second
    for (float dt : {1.0f / 30.0f, 1.0f / 60.0f, 1.0f / 144.0f, 1.0f / 240.0f}) {
        LookState s;
        StepLook(s, true, 200.0f, 80.0f, dt, p);  // look: yaw 0.7, pitch 0.28
        const float yaw0 = s.yaw, pitch0 = s.pitch;
        CHECK(Near(yaw0, 0.7, 1e-5) && Near(pitch0, 0.28, 1e-5));
        // Still for under the delay: the view stays.
        float t = 0.0f;
        while (t < 0.30f) {
            StepLook(s, true, 0.0f, 0.0f, dt, p);
            t += dt;
        }
        CHECK(s.yaw == yaw0 && s.pitch == pitch0);
        // Past the delay it eases back by the same fraction per second at any frame rate: after 1 s more only exp(-4) = 1.8 % is left.
        while (t < 0.35f + 1.0f) {
            StepLook(s, true, 0.0f, 0.0f, dt, p);
            t += dt;
        }
        CHECK(s.yaw < yaw0 * 0.03f && s.yaw > yaw0 * 0.012f);
        CHECK(s.pitch < pitch0 * 0.03f && s.pitch > pitch0 * 0.012f);
        // ... and it ends exactly at zero, not at a tiny number (the exponential never arrives on its own).
        while (t < 6.0f) {
            StepLook(s, true, 0.0f, 0.0f, dt, p);
            t += dt;
        }
        CHECK(s.yaw == 0.0f && s.pitch == 0.0f);
    }

    // Moving again before the delay is up keeps the view where the hand put it.
    LookState s;
    StepLook(s, true, 100.0f, 0.0f, 1.0f / 60.0f, p);
    for (int i = 0; i < 12; ++i) StepLook(s, true, 0.0f, 0.0f, 1.0f / 60.0f, p);  // 0.2 s still
    StepLook(s, true, 10.0f, 0.0f, 1.0f / 60.0f, p);
    CHECK(Near(s.yaw, 0.385, 1e-5));
    for (int i = 0; i < 12; ++i) StepLook(s, true, 0.0f, 0.0f, 1.0f / 60.0f, p);
    CHECK(Near(s.yaw, 0.385, 1e-5));  // the clock started over

    // A delay of 0 starts the return at once (the row's OFF).
    LookParams now = p;
    now.return_delay = 0.0f;
    LookState z;
    StepLook(z, true, 100.0f, 0.0f, 1.0f / 60.0f, now);
    StepLook(z, true, 0.0f, 0.0f, 1.0f / 60.0f, now);
    CHECK(z.yaw < 0.35f);

    // Not engaged (a controller took over, a menu opened, the setting is off): the view eases back at once, with no delay, and the idle clock is reset.
    LookState off;
    StepLook(off, true, 100.0f, 0.0f, 1.0f / 60.0f, p);
    const float before = off.yaw;
    StepLook(off, false, 0.0f, 0.0f, 1.0f / 60.0f, p);
    CHECK(off.yaw < before && off.yaw > 0.0f && off.idle == 0.0f);
    // The deltas of a tick that is not engaged are ignored.
    LookState ignored;
    StepLook(ignored, false, 500.0f, 500.0f, 1.0f / 60.0f, p);
    CHECK(ignored.yaw == 0.0f && ignored.pitch == 0.0f);
}

// ---------------------------------------------------------------------------------------------------------------------------------
// The gate
// ---------------------------------------------------------------------------------------------------------------------------------
Facts Road() {
    Facts f;
    f.wanted = true;
    f.keyboard_mouse = true;
    f.focused = true;
    f.driving = true;
    return f;
}

void TestDecide() {
    CHECK(Decide(Road()) == Why::On);

    Facts f = Road();
    f.wanted = false;
    CHECK(Decide(f) == Why::Off);
    f = Road(); f.keyboard_mouse = false;
    CHECK(Decide(f) == Why::PadInUse);
    f = Road(); f.focused = false;
    CHECK(Decide(f) == Why::NoFocus);
    f = Road(); f.menu = true;
    CHECK(Decide(f) == Why::Menu);
    f = Road(); f.overlay = true;
    CHECK(Decide(f) == Why::Overlay);
    f = Road(); f.map = true;
    CHECK(Decide(f) == Why::Map);
    f = Road(); f.phone = true;
    CHECK(Decide(f) == Why::Phone);
    f = Road(); f.driving = false;
    CHECK(Decide(f) == Why::NotDriving);

    // Every one of those is a "no": exactly the all-clear is a yes (a table over the eight booleans).
    int yes = 0;
    for (int bits = 0; bits < 256; ++bits) {
        Facts g;
        g.wanted = bits & 1;
        g.keyboard_mouse = bits & 2;
        g.focused = bits & 4;
        g.driving = bits & 8;
        g.menu = bits & 16;
        g.phone = bits & 32;
        g.map = bits & 64;
        g.overlay = bits & 128;
        const bool want = g.wanted && g.keyboard_mouse && g.focused && g.driving && !g.menu && !g.phone && !g.map && !g.overlay;
        CHECK((Decide(g) == Why::On) == want);
        yes += want ? 1 : 0;
    }
    CHECK(yes == 1);

    // The first reason wins and is a hard one when there is one: a menu over a car that is not driven is a menu, a controller in use with the window out of focus is the focus.
    f = Road(); f.driving = false; f.menu = true;
    CHECK(Decide(f) == Why::Menu);
    f = Road(); f.focused = false; f.keyboard_mouse = false;
    CHECK(Decide(f) == Why::NoFocus);
    CHECK(WhyText(Why::PadInUse) != nullptr && WhyText(Why::On) != nullptr);
}

void TestGate() {
    Gate g;
    int64_t t = 5000;
    Facts road = Road();

    // Taking hold needs everything to say yes for 150 ms, so a menu that closes does not make the pointer jump the first frame.
    CHECK(!g.Step(road, t));
    CHECK(!g.Step(road, t + 100));
    CHECK(g.Step(road, t + 150));
    CHECK(g.on());
    t += 150;

    // A hard "no" lets go at once, every kind of it.
    for (int kind = 0; kind < 6; ++kind) {
        Gate h;
        int64_t u = 100;
        h.Step(road, u);
        CHECK(h.Step(road, u + 150));
        Facts f = road;
        if (kind == 0) f.keyboard_mouse = false;  // a controller took over
        if (kind == 1) f.focused = false;         // focus lost
        if (kind == 2) f.menu = true;             // a menu opened
        if (kind == 3) f.overlay = true;          // an overlay
        if (kind == 4) f.wanted = false;          // the setting was turned off
        if (kind == 5) f.map = true;              // the map took the pointer
        CHECK(!h.Step(f, u + 151));
        CHECK(!h.on());
        // ... and when the reason is gone it takes 150 ms again, not at once.
        CHECK(!h.Step(road, u + 152));
        CHECK(!h.Step(road, u + 250));
        CHECK(h.Step(road, u + 302));
    }
    {
        Gate h;
        h.Step(road, 0);
        h.Step(road, 200);
        Facts f = road;
        f.phone = true;
        CHECK(!h.Step(f, 201));  // the phone is a hard one too
    }

    // A soft "no" (the HUD skipped a beat) is ridden out for 500 ms; a longer one lets go.
    Gate soft;
    soft.Step(road, 0);
    CHECK(soft.Step(road, 160));
    Facts nd = road;
    nd.driving = false;
    CHECK(soft.Step(nd, 200));
    CHECK(soft.Step(nd, 500));
    CHECK(soft.Step(nd, 699));
    CHECK(!soft.Step(nd, 701));  // 500 ms since the first soft no at 200
    CHECK(!soft.Step(nd, 750));
    // ... but a soft no that ends in time costs nothing, and the next one starts a new count.
    Gate blink;
    blink.Step(road, 0);
    CHECK(blink.Step(road, 160));
    CHECK(blink.Step(nd, 200));
    CHECK(blink.Step(nd, 600));
    CHECK(blink.Step(road, 650));
    CHECK(blink.Step(nd, 700));
    CHECK(blink.Step(nd, 1100));   // 400 ms into the new count
    CHECK(!blink.Step(nd, 1201));  // 501 ms

    // A soft no does not get a gate that was never on to take hold, and a hard one during it wins at once.
    Gate off;
    CHECK(!off.Step(nd, 0));
    CHECK(!off.Step(nd, 1000));
    Gate mixed;
    mixed.Step(road, 0);
    CHECK(mixed.Step(road, 200));
    CHECK(mixed.Step(nd, 300));
    Facts hard = nd;
    hard.menu = true;
    CHECK(!mixed.Step(hard, 310));

    // The engage delay starts over when the "yes" is interrupted.
    Gate flick;
    flick.Step(road, 0);
    flick.Step(road, 100);
    Facts menu = road;
    menu.menu = true;
    flick.Step(menu, 120);
    CHECK(!flick.Step(road, 130));
    CHECK(!flick.Step(road, 250));  // only 120 ms since the new start at 130
    CHECK(flick.Step(road, 281));
}

// ---------------------------------------------------------------------------------------------------------------------------------
// The steering ramp
// ---------------------------------------------------------------------------------------------------------------------------------
// Runs the ramp for `seconds` at a frame time and returns the value.
float Run(float value, int dir, float seconds, float dt, const SteerParams& p) {
    int steps = int(std::lround(seconds / dt));
    for (int i = 0; i < steps; ++i) value = StepSteer(value, dir, dt, p);
    return value;
}

void TestSteerRamp() {
    SteerParams p;  // 0.40 s up, 0.25 s back
    CHECK(Near(p.rise_s, 0.40, 1e-6) && Near(p.return_s, 0.25, 1e-6));

    // The documented defaults, at the frame rates a PC runs: full lock in 0.4 s, half of it in 0.2 s, linear.
    for (float dt : {1.0f / 30.0f, 1.0f / 60.0f, 1.0f / 144.0f, 1.0f / 240.0f}) {
        CHECK(Near(Run(0.0f, +1, 0.2f, dt, p), 0.5, 0.02));
        CHECK(Near(Run(0.0f, -1, 0.2f, dt, p), -0.5, 0.02));
        CHECK(Near(Run(0.0f, +1, 0.1f, dt, p), 0.25, 0.02));
        CHECK(Near(Run(0.0f, +1, 0.4f, dt, p), 1.0, 0.02));
        CHECK(Run(0.0f, +1, 2.0f, dt, p) == 1.0f);   // never past full lock
        CHECK(Run(0.0f, -1, 2.0f, dt, p) == -1.0f);
        // Let go at full lock: back to centre in 0.25 s, and it stays there.
        CHECK(Near(Run(1.0f, 0, 0.125f, dt, p), 0.5, 0.02));
        CHECK(Run(1.0f, 0, 0.3f, dt, p) == 0.0f);
        CHECK(Run(-1.0f, 0, 0.3f, dt, p) == 0.0f);
        CHECK(Run(0.0f, 0, 1.0f, dt, p) == 0.0f);    // at rest it stays at rest
    }

    // The ramp's own numbers, exactly: one step is dt / rise_s.
    CHECK(Near(StepSteer(0.0f, +1, 0.04f, p), 0.1, 1e-6));
    CHECK(Near(StepSteer(0.5f, 0, 0.025f, p), 0.4, 1e-6));  // 1 / 0.25 s = 4 per second

    // A tap is a small correction: 50 ms of key is 12.5 % of lock and then it comes back; it never reaches the end.
    float v = Run(0.0f, +1, 0.05f, 1.0f / 240.0f, p);
    CHECK(Near(v, 0.125, 0.01));
    CHECK(Run(v, 0, 0.1f, 1.0f / 240.0f, p) < 0.01f);

    // Steering the other way from full lock: back through the centre at the return rate (0.25 s), then on at the rise rate.
    CHECK(Near(Run(1.0f, -1, 0.125f, 1.0f / 240.0f, p), 0.5, 0.02));
    CHECK(Near(Run(1.0f, -1, 0.25f, 1.0f / 240.0f, p), 0.0, 0.02));
    CHECK(Near(Run(1.0f, -1, 0.45f, 1.0f / 240.0f, p), -0.5, 0.02));
    CHECK(Run(1.0f, -1, 0.7f, 1.0f / 240.0f, p) == -1.0f);
    // The same in one big frame: it goes through the centre inside the step and out the other side, not a jump to the end.
    CHECK(Near(StepSteer(0.25f, -1, 0.1f, p), -0.0f + (-(0.1f - 0.0625f) / 0.4f), 1e-5));
    CHECK(StepSteer(0.25f, -1, 0.1f, p) < 0.0f && StepSteer(0.25f, -1, 0.1f, p) > -0.5f);

    // Both keys (or neither) cancel: the direction is 0 and the wheel returns to centre. One key is its own side.
    CHECK(SteerDirection(false, false) == 0);
    CHECK(SteerDirection(true, true) == 0);
    CHECK(SteerDirection(true, false) == -1);
    CHECK(SteerDirection(false, true) == +1);
    CHECK(Near(Run(0.8f, SteerDirection(true, true), 0.05f, 1.0f / 240.0f, p), 0.8 - 0.2, 0.02));

    // A stall must not become a jump: dt is clamped to 0.1 s.
    CHECK(Near(StepSteer(0.0f, +1, 5.0f, p), 0.25, 1e-6));
    CHECK(StepSteer(0.3f, +1, -1.0f, p) == 0.3f);  // no negative time

    // The times are tunable, and a silly one cannot divide by zero.
    SteerParams quick;
    quick.rise_s = 0.05f;
    quick.return_s = 0.05f;
    CHECK(Near(Run(0.0f, +1, 0.05f, 1.0f / 240.0f, quick), 1.0, 0.03));
    SteerParams zero;
    zero.rise_s = 0.0f;
    zero.return_s = 0.0f;
    const float z = StepSteer(0.0f, +1, 0.1f, zero);
    CHECK(std::isfinite(z) && z >= 0.0f && z <= 1.0f);

    // At every frame rate the value is the same function of time (a game at 30 FPS steers like one at 240).
    const float at240 = Run(0.0f, +1, 0.3f, 1.0f / 240.0f, p);
    const float at60 = Run(0.0f, +1, 0.3f, 1.0f / 60.0f, p);
    const float at30 = Run(0.0f, +1, 0.3f, 1.0f / 30.0f, p);
    CHECK(Near(at240, at60, 0.02) && Near(at60, at30, 0.02));

    // It is monotonic while a key is held and while it returns.
    float last = 0.0f;
    bool monotonic = true;
    for (int i = 0; i < 200; ++i) {
        const float next = StepSteer(last, +1, 1.0f / 240.0f, p);
        monotonic = monotonic && next >= last;
        last = next;
    }
    CHECK(monotonic);
    for (int i = 0; i < 200; ++i) {
        const float next = StepSteer(last, 0, 1.0f / 240.0f, p);
        monotonic = monotonic && next <= last;
        last = next;
    }
    CHECK(monotonic && last == 0.0f);
}

void TestSteerAxis() {
    CHECK(SteerToAxis(0.0f) == 0);
    CHECK(SteerToAxis(1.0f) == 32767);
    CHECK(SteerToAxis(-1.0f) == -32767);
    CHECK(SteerToAxis(0.5f) == 16384 || SteerToAxis(0.5f) == 16383);
    CHECK(SteerToAxis(5.0f) == 32767 && SteerToAxis(-5.0f) == -32767);  // clamped

    // Key and stick on one axis: the larger magnitude wins, a tie is the stick's.
    CHECK(MergeAxis(0, 16000) == 16000);
    CHECK(MergeAxis(0, -16000) == -16000);
    CHECK(MergeAxis(20000, 16000) == 20000);
    CHECK(MergeAxis(-20000, 16000) == -20000);
    CHECK(MergeAxis(-20000, -32767) == -32767);
    CHECK(MergeAxis(15000, 15000) == 15000);
    CHECK(MergeAxis(-15000, 15000) == -15000);
    CHECK(MergeAxis(-32768, 100) == -32768);  // the int16 minimum has no overflow in the comparison
    CHECK(MergeAxis(0, 0) == 0);
}

}  // namespace

int main() {
    TestDeltaSum();
    TestSensitivityRule();
    TestLookAngles();
    TestRecentre();
    TestDecide();
    TestGate();
    TestSteerRamp();
    TestSteerAxis();
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
