// A check of the mouse in the menus' pure rules (menu_mouse_core.h) that needs no game: the window -> stage mapping for the shapes of picture the game
// makes, the matrix fold, the row boxes and their plausibility, the hit test, the settings rows' left / right zones, the click and hover plans, the wheel's
// remainder, the press pacing, the highlight walk and a click waiting for the highlight.
//   cmake --build <dir> --target larecomp_menu_mouse_test, then run it (exit 0 = passed).
// Or by hand: clang++ -std=c++20 -I src -I src/mc_engine src/mc_engine/input/menu_mouse_test.cpp -o menu_mouse_test

#include <cmath>
#include <cstdint>
#include <cstdio>

#include "menu_mouse_core.h"

namespace {

using namespace larecomp::menumouse;

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

// A pause-style slot: `slot<i>` at (x, y0 + i * pitch) under a `menu` clip at (mx, my) under a root at the origin.
Affine Slot(float mx, float my, float x, float y) {
    Affine slot;
    slot.tx = x;
    slot.ty = y;
    Affine menu;
    menu.tx = mx;
    menu.ty = my;
    return Then(Then(slot, menu), Affine{});
}

SlotIn MakeSlot(int row, float mx, float my, float x, float y, bool visible, float child_right = 0.0f) {
    SlotIn s;
    s.row = row;
    s.to_stage = Slot(mx, my, x, y);
    s.visible = visible;
    s.child_right = child_right;
    return s;
}

// ---------------------------------------------------------------------------------------------------------------------------------
void TestWindowToStage() {
    // A 16:9 window: the UI block is the whole window and the stage maps linearly.
    {
        const mc::frame::Layout l = mc::frame::LayoutOf(mc::frame::Choice::kAuto, 1920, 1080);
        const HudRect hud = HudOf(l.frame, l.ui_fraction, l.ui_height_fraction);
        CHECK(Near(hud.x, 0, 0.01) && Near(hud.y, 0, 0.01) && Near(hud.w, 1920, 0.01) && Near(hud.h, 1080, 0.01));
        float sx, sy;
        CHECK(WindowToStage(hud, 960, 540, &sx, &sy));
        CHECK(Near(sx, 640, 0.001) && Near(sy, 360, 0.001));
        CHECK(WindowToStage(hud, 0, 0, &sx, &sy));
        CHECK(Near(sx, 0, 0.001) && Near(sy, 0, 0.001));
        CHECK(!WindowToStage(hud, 1920, 540, &sx, &sy));  // the right edge is outside (pixels 0..1919)
        CHECK(Near(sx, 1280, 0.001));
        float px, py;
        StageToWindow(hud, 640, 360, &px, &py);
        CHECK(Near(px, 960, 0.001) && Near(py, 540, 0.001));
    }
    // Wider than 16:9 (a 2560 x 1080 window, 21:9): the UI is the middle 16:9 block and its centre is the window's centre; the same block frame_fit.h calls hud.
    {
        const mc::frame::Layout l = mc::frame::LayoutOf(mc::frame::Choice::kAuto, 2560, 1080);
        const HudRect hud = HudOf(l.frame, l.ui_fraction, l.ui_height_fraction);
        CHECK(Near(hud.w, double(l.hud.w), 1.0) && Near(hud.h, double(l.hud.h), 1.0));
        CHECK(Near(hud.x, double(l.hud.x), 1.0) && Near(hud.y, double(l.hud.y), 1.0));
        CHECK(Near(hud.h, 1080, 0.01));
        CHECK(Near(hud.w, 1080.0 * 16.0 / 9.0, 1.0));
        float sx, sy;
        CHECK(WindowToStage(hud, 1280, 540, &sx, &sy));
        CHECK(Near(sx, 640, 0.5) && Near(sy, 360, 0.001));
        // A pointer in the black / wide part outside the UI block is not on the stage.
        CHECK(!WindowToStage(hud, 10, 540, &sx, &sy));
        CHECK(sx < 0);
    }
    // Taller than 16:9 (a 1920 x 1200 window, 16:10): the UI is squeezed vertically about the middle.
    {
        const mc::frame::Layout l = mc::frame::LayoutOf(mc::frame::Choice::kAuto, 1920, 1200);
        const HudRect hud = HudOf(l.frame, l.ui_fraction, l.ui_height_fraction);
        CHECK(Near(hud.w, 1920, 0.01));
        CHECK(Near(hud.h, 1080, 1.0));
        CHECK(Near(hud.y, 60, 1.0));
        CHECK(Near(hud.h, double(l.hud.h), 1.0) && Near(hud.y, double(l.hud.y), 1.0));
        float sx, sy;
        CHECK(WindowToStage(hud, 960, 600, &sx, &sy));
        CHECK(Near(sy, 360, 0.5));
        CHECK(!WindowToStage(hud, 960, 20, &sx, &sy));
    }
    // A boxed window (21:9 chosen in a 16:9 window: bars above and below): the UI lies in the frame's block.
    {
        const mc::frame::Layout l = mc::frame::LayoutOf(mc::frame::Choice::k21x9, 1920, 1080);
        const HudRect hud = HudOf(l.frame, l.ui_fraction, l.ui_height_fraction);
        CHECK(Near(hud.x, double(l.hud.x), 1.0) && Near(hud.w, double(l.hud.w), 1.0));
        CHECK(Near(hud.y, double(l.hud.y), 1.0) && Near(hud.h, double(l.hud.h), 1.0));
    }
    // No frame: no UI block, nothing is hit.
    {
        const HudRect none = HudOf(mc::frame::Rect{}, 1.0, 1.0);
        float sx = 5, sy = 5;
        CHECK(none.empty());
        CHECK(!WindowToStage(none, 100, 100, &sx, &sy));
        CHECK(sx == 0.0f && sy == 0.0f);
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------
void TestAffine() {
    Affine id;
    float x, y;
    Apply(id, 3, 4, &x, &y);
    CHECK(x == 3 && y == 4);

    Affine child;
    child.tx = 10;
    child.ty = 20;
    Affine parent;
    parent.a = 2;
    parent.d = 3;
    parent.tx = 100;
    parent.ty = 200;
    const Affine both = Then(child, parent);
    Apply(both, 0, 0, &x, &y);
    CHECK(x == 2 * 10 + 100 && y == 3 * 20 + 200);  // the child's origin, in the parent's space
    Apply(both, 1, 1, &x, &y);
    CHECK(x == 2 * 11 + 100 && y == 3 * 21 + 200);

    // Applying the child and then the parent by hand gives the folded matrix.
    float cx, cy, px, py;
    Apply(child, 5, 6, &cx, &cy);
    Apply(parent, cx, cy, &px, &py);
    Apply(both, 5, 6, &x, &y);
    CHECK(Near(x, px, 1e-4) && Near(y, py, 1e-4));

    // A rotation by 90 degrees: (1, 0) goes to (0, 1).
    Affine rot;
    rot.a = 0;
    rot.b = 1;
    rot.c = -1;
    rot.d = 0;
    Affine shift;
    shift.tx = 5;
    const Affine r = Then(shift, rot);  // shift first, then rotate
    Apply(r, 0, 0, &x, &y);
    CHECK(Near(x, 0, 1e-4) && Near(y, 5, 1e-4));

    Affine bad;
    bad.tx = INFINITY;
    CHECK(!FiniteAffine(bad));
    CHECK(FiniteAffine(both));
}

// ---------------------------------------------------------------------------------------------------------------------------------
void TestLayout() {
    // The pause menu's first page: menu clip at (200, 250), slots every 25 px from y = 0.
    SlotIn slots[3];
    for (int i = 0; i < 3; ++i) slots[i] = MakeSlot(i, 200, 250, 0, 25.0f * float(i), true);
    Layout l = BuildLayout(slots, 3, 0.0f, 0.0f);
    CHECK(l.n == 3);
    CHECK(Near(l.pitch, 25, 0.001));
    CHECK(Near(l.width, kMinRowWidth, 0.001));  // nothing inside the slots says more
    CHECK(Near(l.rows[0].x0, 200, 0.001) && Near(l.rows[0].y0, 250, 0.001) && Near(l.rows[0].y1, 275, 0.001));
    CHECK(Near(l.rows[2].y0, 300, 0.001) && Near(l.rows[2].x1, 200 + kMinRowWidth, 0.001));
    CHECK(Plausible(l));

    // A hit: inside the second row; the edges of a row are inclusive at the top, exclusive at the bottom (no pointer is on two rows).
    CHECK(HitRow(l, 300, 262) == 0);
    CHECK(HitRow(l, 300, 275) == 1);
    CHECK(HitRow(l, 300, 274.99f) == 0);
    CHECK(HitRow(l, 300, 249) == -1);   // above the first row
    CHECK(HitRow(l, 300, 325) == -1);   // under the last row
    CHECK(HitRow(l, 100, 262) == -1);   // left of the list
    CHECK(HitRow(l, 200 - kMarginLeft + 1, 262) == 0);  // the marker's margin
    CHECK(HitRow(l, 200 - kMarginLeft - 1, 262) == -1);
    CHECK(HitRow(l, 200 + kMinRowWidth - 1, 262) == 0);
    CHECK(HitRow(l, 200 + kMinRowWidth, 262) == -1);

    // A scrolled list: slots 0..5 show rows 7..12; the hit gives the LIST's row.
    SlotIn scrolled[6];
    for (int i = 0; i < 6; ++i) scrolled[i] = MakeSlot(7 + i, 100, 100, 20, 25.0f * float(i), true);
    Layout s = BuildLayout(scrolled, 6, 0.0f, 0.0f);
    CHECK(s.n == 6 && Plausible(s));
    CHECK(HitRow(s, 150, 100 + 3 * 25 + 5) == 10);
    CHECK(BoxOfRow(s, 12) != nullptr && BoxOfRow(s, 6) == nullptr);

    // A hidden slot (a row that is not on screen) is skipped and does not break the pitch.
    SlotIn some[4];
    for (int i = 0; i < 4; ++i) some[i] = MakeSlot(i, 0, 100, 0, 30.0f * float(i), i != 2);
    Layout h = BuildLayout(some, 4, 0.0f, 0.0f);
    CHECK(h.n == 3 && HitRow(h, 50, 100 + 2 * 30 + 5) == -1);  // row 2 is hidden: the pointer over its place hits nothing
    CHECK(HitRow(h, 50, 100 + 3 * 30 + 5) == 3);
    CHECK(Near(h.pitch, 30, 0.001));  // the step per ROW: the hidden slot between rows 1 and 3 does not double it

    // One row only: the fallback pitch.
    SlotIn one[1] = {MakeSlot(0, 0, 100, 0, 0, true)};
    Layout o = BuildLayout(one, 1, 0.0f, 0.0f);
    CHECK(o.n == 1 && Near(o.pitch, kFallbackPitch, 0.001) && Plausible(o));

    // The width follows the content: a settings page whose arrow starts at 383 gets 407; the override wins; the caps hold.
    SlotIn wide[2];
    for (int i = 0; i < 2; ++i) wide[i] = MakeSlot(i, 100, 100, 0, 26.0f * float(i), true, 383.0f);
    Layout w = BuildLayout(wide, 2, 0.0f, 0.0f);
    CHECK(Near(w.width, 383 + kArrowAllowance, 0.001));
    CHECK(Near(BuildLayout(wide, 2, 500.0f, 0.0f).width, 500, 0.001));
    CHECK(Near(WidthFor(2000.0f, 0.0f), kMaxRowWidth, 0.001));
    CHECK(Near(WidthFor(10.0f, 0.0f), kMinRowWidth, 0.001));
    CHECK(Near(WidthFor(0.0f, 0.0f), kMinRowWidth, 0.001));

    // The y offset (calibration) moves every row.
    Layout off = BuildLayout(slots, 3, 0.0f, -12.0f);
    CHECK(Near(off.rows[0].y0, 238, 0.001) && Near(off.rows[0].y1, 263, 0.001));

    // Plausible refuses what is not a text list.
    Layout bad = l;
    bad.pitch = 3.0f;
    CHECK(!Plausible(bad));
    bad = l;
    bad.rows[2].y0 = bad.rows[1].y0;  // not strictly one under the other
    CHECK(!Plausible(bad));
    bad = l;
    bad.rows[1].x0 = 5000.0f;
    bad.rows[1].x1 = 5300.0f;
    CHECK(!Plausible(bad));
    bad = l;
    bad.rows[0].y0 = NAN;
    CHECK(!Plausible(bad));
    Layout empty;
    CHECK(!Plausible(empty));
    // A slot with a broken matrix is left out.
    SlotIn broken[2] = {MakeSlot(0, 0, 100, 0, 0, true), MakeSlot(1, 0, 100, 0, 25, true)};
    broken[1].to_stage.tx = NAN;
    CHECK(BuildLayout(broken, 2, 0.0f, 0.0f).n == 1);

    // Many rows: the layout holds kMaxRows and no more.
    SlotIn many[40];
    for (int i = 0; i < 40; ++i) many[i] = MakeSlot(i, 0, 0, 0, 10.0f * float(i), true);
    CHECK(BuildLayout(many, 40, 0.0f, 0.0f).n == kMaxRows);

    CHECK(Near(MedianPitch(nullptr, nullptr, 0, 25), 25, 1e-6));
    const float ys[] = {10, 35, 60, 85, 200};
    const int rows5[] = {0, 1, 2, 3, 4};
    CHECK(Near(MedianPitch(ys, rows5, 5, 25), 25, 1e-6));  // steps 25 25 25 115: the median holds the usual one
    const int gap[] = {0, 1, 2, 3, 7};  // the last two are four rows apart: 115 / 4 = 28.75
    CHECK(Near(MedianPitch(ys, gap, 5, 25), 25, 1e-6));
    const int same[] = {0, 0, 0, 0, 0};  // no row apart: no step at all
    CHECK(Near(MedianPitch(ys, same, 5, 25), 25, 1e-6));
}

// ---------------------------------------------------------------------------------------------------------------------------------
void TestZones() {
    RowBox box;
    box.row = 3;
    box.x0 = 100;
    box.x1 = 100 + 430;
    box.y0 = 200;
    box.y1 = 226;
    // A settings row: label, then the value's left and right halves.
    CHECK(ZoneOf(box, 100 + 0.20f * 430, true) == Zone::Label);
    CHECK(ZoneOf(box, 100 + 0.549f * 430, true) == Zone::Label);
    CHECK(ZoneOf(box, 100 + 0.61f * 430, true) == Zone::ValueLeft);   // the left arrow's place
    CHECK(ZoneOf(box, 100 + 0.739f * 430, true) == Zone::ValueLeft);
    CHECK(ZoneOf(box, 100 + 0.87f * 430, true) == Zone::ValueRight);  // the right arrow's place
    CHECK(ZoneOf(box, 100 + 0.99f * 430, true) == Zone::ValueRight);
    // A row without a value is all label.
    CHECK(ZoneOf(box, 100 + 0.87f * 430, false) == Zone::Label);
    RowBox flat = box;
    flat.x1 = flat.x0;
    CHECK(ZoneOf(flat, 100, true) == Zone::Label);
}

// ---------------------------------------------------------------------------------------------------------------------------------
void TestPlans() {
    // Left click, rows known.
    ClickPlan p = PlanLeftClick(true, 4, Zone::Label, false);
    CHECK(p.act == Act::Accept && p.row == 4);
    p = PlanLeftClick(true, 4, Zone::Label, true);  // a settings row's label: accept (a toggle flips, a button opens)
    CHECK(p.act == Act::Accept && p.row == 4);
    p = PlanLeftClick(true, 4, Zone::ValueLeft, true);
    CHECK(p.act == Act::Left && p.row == 4);
    p = PlanLeftClick(true, 4, Zone::ValueRight, true);
    CHECK(p.act == Act::Right && p.row == 4);
    p = PlanLeftClick(true, 4, Zone::ValueRight, false);  // a row with no value is not stepped
    CHECK(p.act == Act::Accept);
    p = PlanLeftClick(true, -1, Zone::Label, false);      // a click on nothing does nothing
    CHECK(p.act == Act::None && p.row == -1);
    // Rows not known: ACCEPT on the highlighted row, wherever the pointer is.
    p = PlanLeftClick(false, -1, Zone::Label, false);
    CHECK(p.act == Act::Accept && p.row == -1);
    p = PlanLeftClick(false, 2, Zone::ValueRight, true);
    CHECK(p.act == Act::Accept && p.row == -1);

    // Hover.
    CHECK(HoverTarget(true, true, true, true, 3, 1) == 3);
    CHECK(HoverTarget(true, true, true, true, 1, 1) == -1);    // already highlighted
    CHECK(HoverTarget(true, true, true, true, -1, 1) == -1);   // over nothing
    CHECK(HoverTarget(true, false, true, true, 3, 1) == -1);   // the pointer did not move: the keys and the pad keep the highlight
    CHECK(HoverTarget(true, true, false, true, 3, 1) == -1);   // a controller is in use
    CHECK(HoverTarget(true, true, true, false, 3, 1) == -1);   // rows not known (fallback)
    CHECK(HoverTarget(false, true, true, true, 3, 1) == -1);   // the setting is off
    CHECK(HoverTarget(true, true, true, true, 0, -1) == 0);    // nothing highlighted yet

    // The pointer.
    CHECK(WantsPointer(true, true, true, true));
    CHECK(!WantsPointer(false, true, true, true));
    CHECK(!WantsPointer(true, false, true, true));
    CHECK(!WantsPointer(true, true, false, true));
    CHECK(!WantsPointer(true, true, true, false));
}

// ---------------------------------------------------------------------------------------------------------------------------------
void TestWheel() {
    WheelAccum w(120, 6);
    CHECK(w.TakeOne() == 0);
    w.Add(120);
    CHECK(w.TakeOne() == +1 && w.TakeOne() == 0);
    w.Add(-120);
    CHECK(w.TakeOne() == -1);

    w.Clear();
    CHECK(!w.HasStep());
    w.Add(119);
    CHECK(!w.HasStep());
    w.Add(1);
    CHECK(w.HasStep());
    w.Clear();
    w.Add(-120);
    CHECK(w.HasStep() && w.TakeOne() == -1 && !w.HasStep());
    w.Clear();

    // A smooth wheel: fractions add up and nothing is lost.
    for (int i = 0; i < 7; ++i) w.Add(20);  // 140 units
    CHECK(w.TakeOne() == +1);
    CHECK(w.Pending() == 20);
    CHECK(w.TakeOne() == 0);
    w.Add(100);
    CHECK(w.TakeOne() == +1 && w.Pending() == 0);

    // The direction flips cleanly: up then down cancel out.
    w.Clear();
    w.Add(60);
    w.Add(-60);
    CHECK(w.TakeOne() == 0 && w.Pending() == 0);

    // A flick of thirty notches: at most the cap waits (six steps), then it stops.
    w.Clear();
    for (int i = 0; i < 30; ++i) w.Add(120);
    int steps = 0;
    while (w.TakeOne() != 0) ++steps;
    CHECK(steps == 6);
    for (int i = 0; i < 30; ++i) w.Add(-120);
    steps = 0;
    while (w.TakeOne() == -1) ++steps;
    CHECK(steps == 6);

    WheelAccum odd(0, 0);  // nonsense arguments fall back to sane ones
    odd.Add(120);
    CHECK(odd.TakeOne() == +1);
}

// ---------------------------------------------------------------------------------------------------------------------------------
void TestPacer() {
    PressPacer p(2);
    // Ready at the start; after a press it is not ready for the next tick (the key must be up for one poll), and is again after two ticks.
    int tick = 0;
    int pressed_at[8] = {};
    int n = 0;
    for (tick = 0; tick < 8; ++tick) {
        p.Step();
        if (p.Ready()) {
            p.Fired();
            pressed_at[n++] = tick;
        }
    }
    CHECK(n == 4);
    CHECK(pressed_at[0] == 0 && pressed_at[1] == 2 && pressed_at[2] == 4 && pressed_at[3] == 6);
    p.Fired();
    p.Reset();
    CHECK(p.Ready());
    PressPacer q(0);  // a nonsense gap is 1
    q.Fired();
    q.Step();
    CHECK(q.Ready());
}

// ---------------------------------------------------------------------------------------------------------------------------------
void TestWalk() {
    CHECK(WalkStep(2, 5) == Act::Down);
    CHECK(WalkStep(5, 2) == Act::Up);
    CHECK(WalkStep(3, 3) == Act::None);
    CHECK(WalkStep(-1, 3) == Act::None);  // nothing highlighted: unknown
    CHECK(WalkStep(3, -1) == Act::None);

    // A menu that moves one row per press: never stalls.
    WalkWatch ok(4);
    int sel = 0;
    for (int i = 0; i < 20; ++i) {
        ok.Pressed(sel);
        ++sel;
    }
    CHECK(!ok.Stalled());
    // A menu that ignores the presses: stalled after limit + 1 presses (the first one has nothing to compare with).
    WalkWatch dead(4);
    for (int i = 0; i < 4; ++i) dead.Pressed(7);
    CHECK(!dead.Stalled());
    dead.Pressed(7);
    CHECK(dead.Stalled());
    dead.Reset();
    CHECK(!dead.Stalled());
    // A press that did move resets the count.
    WalkWatch mixed(3);
    mixed.Pressed(1);
    mixed.Pressed(1);
    mixed.Pressed(1);
    mixed.Pressed(2);
    mixed.Pressed(2);
    CHECK(!mixed.Stalled());
}

// ---------------------------------------------------------------------------------------------------------------------------------
void TestPending() {
    ClickPlan plan;
    plan.row = 3;
    plan.act = Act::Accept;
    PendingClick p = StartPending(plan);
    bool done = true;
    // The highlight is not there yet: nothing is pressed, the click waits.
    CHECK(StepPending(p, 1, &done) == Act::None && !done && p.active);
    CHECK(StepPending(p, 2, &done) == Act::None && !done);
    // Arrived: the press goes out and the click is over.
    CHECK(StepPending(p, 3, &done) == Act::Accept && done && !p.active);

    // Already there: pressed at once.
    p = StartPending(plan);
    CHECK(StepPending(p, 3, &done) == Act::Accept && done);

    // A highlight that never arrives drops the click and never presses ACCEPT on another row.
    p = StartPending(plan);
    Act last = Act::None;
    int ticks = 0;
    do {
        last = StepPending(p, 1, &done);
        ++ticks;
    } while (!done && ticks < 100);
    CHECK(done && last == Act::None && ticks == kPendingClickTicks);

    // A click with no row to wait for (the fallback) presses at once.
    ClickPlan fallback;
    fallback.act = Act::Accept;
    p = StartPending(fallback);
    CHECK(StepPending(p, 5, &done) == Act::Accept && done);

    // An inactive click is over.
    PendingClick none;
    CHECK(StepPending(none, 0, &done) == Act::None && done);

    // The arrow press waits the same way.
    ClickPlan arrow;
    arrow.row = 2;
    arrow.act = Act::Right;
    p = StartPending(arrow);
    CHECK(StepPending(p, 0, &done) == Act::None && !done);
    CHECK(StepPending(p, 2, &done) == Act::Right && done);
}

}  // namespace

int main() {
    TestWindowToStage();
    TestAffine();
    TestLayout();
    TestZones();
    TestPlans();
    TestWheel();
    TestPacer();
    TestWalk();
    TestPending();
    std::printf("menu_mouse_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
