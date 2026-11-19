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

// ---------------------------------------------------------------------------------------------------------------------------------
// Round 2: where a movie is drawn, the world matrix, the rows a list draws, the model of the pause pages
void TestFrameScale() {
    // The pause movie: a 1280 x 720 frame on the 1280 x 720 screen is drawn 1:1.
    StageScale s = FrameScale(1280, 1.0f, 720, 1.0f, 1280, 720);
    CHECK(s.applied && Near(s.x, 1.0, 1e-6) && Near(s.y, 1.0, 1e-6));
    // The garage: a 1920 x 1080 frame is drawn at two thirds; 35 movie units between rows are 23.3 stage pixels.
    s = FrameScale(1280, 1.0f, 720, 1.0f, 1920, 1080);
    CHECK(s.applied && Near(s.x, 2.0 / 3.0, 1e-5) && Near(s.y, 2.0 / 3.0, 1e-5));
    CHECK(Near(35.0f * s.y, 23.33, 0.01));
    // The screen's size is cut to a whole pixel count first (a k of 0.7777 on 1280 is 995 pixels).
    s = FrameScale(1280, 0.7777f, 720, 1.0f, 1280, 720);
    CHECK(s.applied && Near(s.x, 995.0 / 1280.0, 1e-6));
    // Missing or absurd numbers: no scale, drawn 1:1.
    s = FrameScale(0, 1.0f, 720, 1.0f, 1280, 720);
    CHECK(!s.applied && s.x == 1.0f && s.y == 1.0f);
    s = FrameScale(1280, 1.0f, 720, 1.0f, 0, 0);
    CHECK(!s.applied);
    s = FrameScale(1280, 0.0f, 720, 1.0f, 1280, 720);
    CHECK(!s.applied);
    s = FrameScale(1280, 1.0f, 720, 1.0f, 100000, 720);
    CHECK(!s.applied);
    s = FrameScale(1280, 1.0f, 720, 1.0f, 64, 720);  // a x scale of 20: absurd
    CHECK(!s.applied);
    s = FrameScale(std::nanf(""), 1.0f, 720, 1.0f, 1280, 720);
    CHECK(!s.applied);

    // A matrix to the stage: the translation and the axes are scaled, x with x and y with y.
    Affine m;
    m.a = 1.0f;
    m.d = 2.0f;
    m.tx = 600.0f;
    m.ty = 300.0f;
    const StageScale two_thirds = FrameScale(1280, 1.0f, 720, 1.0f, 1920, 1080);
    const Affine st = Scaled(m, two_thirds);
    CHECK(Near(st.tx, 400.0, 1e-3) && Near(st.ty, 200.0, 1e-3) && Near(st.a, 2.0 / 3.0, 1e-5) && Near(st.d, 4.0 / 3.0, 1e-5));
    // A slot's origin through the scaled world matrix: the garage's first row (movie 1290, 350) lands at (860, 233).
    Affine world;
    world.tx = 1290.0f;
    world.ty = 350.0f;
    float x, y;
    Apply(Scaled(world, two_thirds), 0, 0, &x, &y);
    CHECK(Near(x, 860.0, 0.01) && Near(y, 233.33, 0.01));
}

void TestWorldMatrix() {
    Affine zeros;
    zeros.a = zeros.d = 0.0f;
    CHECK(!WorldUsable(zeros));  // an instance that was never drawn
    Affine ok;
    ok.tx = 12.0f;
    CHECK(WorldUsable(ok));
    Affine nan = ok;
    nan.tx = std::nanf("");
    CHECK(!WorldUsable(nan));
    Affine flat = ok;
    flat.d = 0.0f;
    CHECK(!WorldUsable(flat));
    CHECK(ChooseMatrix(true, true) == MatrixSource::World);
    CHECK(ChooseMatrix(true, false) == MatrixSource::World);
    CHECK(ChooseMatrix(false, true) == MatrixSource::Fold);
    CHECK(ChooseMatrix(false, false) == MatrixSource::None);
}

void TestListWindow() {
    int first = -1, end = -1;
    // A list that does not scroll draws every row.
    ListWindow(3, false, 7, 6, &first, &end);
    CHECK(first == 0 && end == 3);
    // A list that scrolls: from the scroll to scroll + the rows that fit.
    ListWindow(11, true, 2, 6, &first, &end);
    CHECK(first == 2 && end == 8);
    // The end is cut at the row count.
    ListWindow(11, true, 8, 6, &first, &end);
    CHECK(first == 8 && end == 11);
    ListWindow(4, true, 0, 6, &first, &end);
    CHECK(first == 0 && end == 4);

    // The clip tree and the window.
    SlotIn slots[3];
    for (int i = 0; i < 3; ++i) slots[i] = MakeSlot(i + 2, 0, 0, 140, 331 + 25 * i, true);
    const Layout lay = BuildLayout(slots, 3, 0.0f, 0.0f);
    CHECK(SlotsMatchList(lay, 2, 8));        // rows 2..4 inside 2..7
    CHECK(SlotsMatchList(lay, 0, 5));
    CHECK(!SlotsMatchList(lay, 3, 8));       // row 2 is outside: an older list
    CHECK(!SlotsMatchList(lay, 0, 4));       // row 4 is outside
    CHECK(!SlotsMatchList(Layout{}, 0, 8));  // no rows shown at all

    // Which kind of menu the game rendered last.
    CHECK(ListIsLater(5, 3));
    CHECK(!ListIsLater(3, 5));
    CHECK(ListIsLater(4, 4));
    CHECK(ListIsLater(0, 0));
}

void TestJudging() {
    // Where a list's rows come from.
    CHECK(RowSourceOf(0x40001000, 0) == RowSource::Adapter);
    CHECK(RowSourceOf(0x40001000, 0x40002000) == RowSource::Adapter);
    CHECK(RowSourceOf(0, 0x40002000) == RowSource::Table);
    CHECK(RowSourceOf(0, 0) == RowSource::Packed);

    // A live list: active, a count in range, a highlight inside (or none), a scroll inside when it scrolls.
    CHECK(JudgeList(true, 3, 0, false, 0) == Verdict::Ok);
    CHECK(JudgeList(true, 3, -1, false, 0) == Verdict::Ok);
    CHECK(JudgeList(true, 11, 4, true, 5) == Verdict::Ok);
    CHECK(JudgeList(false, 3, 0, false, 0) == Verdict::NotActive);  // another tab's list: not drawn
    CHECK(JudgeList(true, 0, 0, false, 0) == Verdict::BadCount);    // the half word of a pause page (the first run's reason)
    CHECK(JudgeList(true, -1, 0, false, 0) == Verdict::BadCount);   // no source to count
    CHECK(JudgeList(true, 65, 0, false, 0) == Verdict::BadCount);
    CHECK(JudgeList(true, 64, 63, false, 0) == Verdict::Ok);
    CHECK(JudgeList(true, 3, 3, false, 0) == Verdict::BadHighlight);
    CHECK(JudgeList(true, 3, -2, false, 0) == Verdict::BadHighlight);
    CHECK(JudgeList(true, 3, 1, true, 3) == Verdict::BadScroll);
    CHECK(JudgeList(true, 3, 1, true, -1) == Verdict::BadScroll);
    CHECK(JudgeList(true, 3, 1, false, 99) == Verdict::Ok);  // a scroll that is not used is not looked at

    // A UIMenu: the rows it counts, never the cap (it was still answering), never none (the garage's second menu in the first run).
    CHECK(JudgeUiMenu(4, 0, 32) == Verdict::Ok);
    CHECK(JudgeUiMenu(4, -1, 32) == Verdict::Ok);
    CHECK(JudgeUiMenu(0, -1, 32) == Verdict::BadCount);
    CHECK(JudgeUiMenu(32, 0, 32) == Verdict::BadCount);
    CHECK(JudgeUiMenu(31, 30, 32) == Verdict::Ok);
    CHECK(JudgeUiMenu(4, 4, 32) == Verdict::BadHighlight);

    // A layout that agrees with the list's window is the one taken: a pause page of 3 rows (window 0..2), the clip tree showing three rows; an older list of two rows does not agree.
    SlotIn three[3];
    for (int i = 0; i < 3; ++i) three[i] = MakeSlot(i, 0, 0, 218, 331 + 25 * i, true);
    const Layout lay = BuildLayout(three, 3, 0.0f, 0.0f);
    int first, end;
    ListWindow(3, false, 0, 6, &first, &end);
    CHECK(SlotsMatchList(lay, first, end) && Plausible(lay));
    ListWindow(2, false, 0, 6, &first, &end);
    CHECK(!SlotsMatchList(lay, first, end));
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Round 3: the pause card. Every number below was measured in the pictures of the run kbmp6-kbm-menu-mouse (2880 x 1620, so stage = pixels / 2.25) and the dumps of its log (slot world y,
// prompt_bg world y). A measured point is the centre of the highlight bar of a row at a given x (a column of the picture).

struct Meas {
    const char* what;
    int row;        // the list's row the bar was on
    float x_px, y_px;
};

// A page of the pause movie: slot j's world y = first_y + 35 j for rows 0..n-1, the prompt bar at prompt_y.
Layout PageLayout(int n, float first_y, float prompt_y, bool wide) {
    CardSlot slots[kMaxRows];
    for (int i = 0; i < n; ++i) {
        slots[i].row = i;
        slots[i].x = 23.2f;
        slots[i].y = first_y + 35.0f * float(i);
        slots[i].visible = true;
    }
    return CardLayout(slots, n, prompt_y, wide, 0.0f);
}

void TestCardFixtures() {
    // The pause page: 3 rows, slot 0 at 78.9, prompt bar 344.7. Bar centres: row 0 (tour_02), row 1 (tour_07), row 2 (tour_06).
    const Layout p0 = PageLayout(3, 78.9f, 344.7f, false);
    CHECK(p0.card && p0.n == 3 && Plausible(p0) && Near(p0.v_pitch, 35.0, 1e-4));
    const Meas p0m[] = {{"p0 row 0", 0, 560, 765.5f}, {"p0 row 0", 0, 800, 769.0f}, {"p0 row 0", 0, 1000, 772.0f}, {"p0 row 1", 1, 560, 822.5f}, {"p0 row 1", 1, 800, 823.5f},
        {"p0 row 1", 1, 1000, 823.0f}, {"p0 row 2", 2, 560, 879.0f}, {"p0 row 2", 2, 800, 877.0f}, {"p0 row 2", 2, 1000, 874.5f}};
    for (const Meas& m : p0m) {
        const float X = m.x_px / 2.25f, Y = m.y_px / 2.25f;
        CHECK(HitRow(p0, X, Y) == m.row);
        // The bar's own centre is within a stage pixel of the model's row centre at that x.
        const RowBox* box = BoxOfRow(p0, m.row);
        CHECK(box != nullptr);
        if (box) CHECK(Near(CardY(p0.map, X, 0.5f * (box->v0 + box->v1)), Y, 1.0));
        // And 10 stage pixels (under half a row) off its centre still hits it, 14 off hits the neighbour or nothing.
        CHECK(HitRow(p0, X, Y - 10.0f) == m.row);
        CHECK(HitRow(p0, X, Y + 10.0f) == m.row);
        CHECK(HitRow(p0, X, Y + 14.0f) != m.row);
        CHECK(HitRow(p0, X, Y - 14.0f) != m.row);
    }
    // Rows converge to the right (25.2 apart at x 249, 22.8 at x 444: measured) and the first row falls to the right.
    {
        const RowBox& r0 = p0.rows[0];
        const RowBox& r1 = p0.rows[1];
        const float v0 = 0.5f * (r0.v0 + r0.v1), v1 = 0.5f * (r1.v0 + r1.v1);
        CHECK(Near(CardY(p0.map, 249.0f, v1) - CardY(p0.map, 249.0f, v0), 25.2, 0.3));
        CHECK(Near(CardY(p0.map, 444.0f, v1) - CardY(p0.map, 444.0f, v0), 22.8, 0.3));
        CHECK(CardY(p0.map, 500.0f, v0) > CardY(p0.map, 250.0f, v0));
    }
    // The tour's pointer positions on the pause page (330, 341) (330, 365) (330, 391): rows 0, 1, 2; 420 is under the last row; right of the card and left of it: none.
    CHECK(HitRow(p0, 330, 341) == 0);
    CHECK(HitRow(p0, 330, 365) == 1);
    CHECK(HitRow(p0, 330, 391) == 2);
    CHECK(HitRow(p0, 330, 420) == -1);
    CHECK(HitRow(p0, 700, 341) == -1);
    CHECK(HitRow(p0, 150, 341) == -1);
    CHECK(HitRow(p0, 210, 341) == 0);  // the `>` marker's margin

    // The GRAPHICS list (hub): title and description, so slot 0 at 106.9 and the prompt bar at 367.7; row 1 highlighted (tour_20).
    const Layout hub = PageLayout(6, 106.9f, 367.7f, false);
    CHECK(Plausible(hub));
    for (float xp : {560.0f, 800.0f, 1000.0f}) {
        const float yp = xp == 560.0f ? 830.0f : (xp == 800.0f ? 831.5f : 830.5f);
        CHECK(HitRow(hub, xp / 2.25f, yp / 2.25f) == 1);
    }
    // The SETTINGS tab: 11 rows, the first 6 shown (slot 0 at 78.9, prompt bar 344.7), row 5 highlighted (tour_21); the bar rises to the right: 1048.5, 1036.5, 1027 px at x 560, 800, 1000.
    const Layout settings = PageLayout(6, 78.9f, 344.7f, false);
    CHECK(HitRow(settings, 560.0f / 2.25f, 1048.5f / 2.25f) == 5);
    CHECK(HitRow(settings, 800.0f / 2.25f, 1036.5f / 2.25f) == 5);
    CHECK(HitRow(settings, 1000.0f / 2.25f, 1027.0f / 2.25f) == 5);
    // At the left end the rows are 56 px apart (51 at the right): the row above is at about 992 px there.
    CHECK(HitRow(settings, 560.0f / 2.25f, 992.0f / 2.25f) == 4);

    // The UPSCALING page (value rows, the wide card): slot 0 at 106.9, prompt bar 367.7. Bar centres at x 1000 px: row 0 779, row 4 983.5, row 5 1033.5 (tour_10, _12, _15); x 1200: 781.5, 976.5, 1025.
    const Layout ups = PageLayout(6, 106.9f, 367.7f, true);
    CHECK(ups.wide && Plausible(ups));
    CHECK(HitRow(ups, 1000.0f / 2.25f, 779.0f / 2.25f) == 0);
    CHECK(HitRow(ups, 1000.0f / 2.25f, 983.5f / 2.25f) == 4);
    CHECK(HitRow(ups, 1000.0f / 2.25f, 1033.5f / 2.25f) == 5);
    CHECK(HitRow(ups, 1200.0f / 2.25f, 781.5f / 2.25f) == 0);
    CHECK(HitRow(ups, 1200.0f / 2.25f, 976.5f / 2.25f) == 4);
    CHECK(HitRow(ups, 1200.0f / 2.25f, 1025.0f / 2.25f) == 5);
    // The wide card begins further left than the plain one.
    CHECK(HitRow(ups, 170, 346) == 0);
    CHECK(HitRow(hub, 170, 346) == -1);
}

void TestCardClicksOfTheRun() {
    // The clicks and hovers of the run, at the stage points the log gave, on the UPSCALING page (value rows).
    const Layout ups = PageLayout(6, 106.9f, 367.7f, true);
    // "left click at window (922, 675) = stage (409.8, 300.0): row -1 -> none": the description line above row 0 still does nothing.
    CHECK(HitRow(ups, 409.8f, 300.0f) == -1);
    // "(1147, 832) = stage (509.8, 369.8)" and "(897, 832) = stage (398.7, 369.8)": row 1 (RENDER SCALE), on its `>` side and on its `<`.
    CHECK(HitRow(ups, 509.8f, 369.8f) == 1);
    CHECK(HitRow(ups, 398.7f, 369.8f) == 1);
    const RowBox* row1 = BoxOfRow(ups, 1);
    CHECK(row1 != nullptr);
    if (row1) {
        CHECK(ZoneOf(ups, *row1, 509.8f, true) == Zone::ValueRight);
        CHECK(ZoneOf(ups, *row1, 398.7f, true) == Zone::ValueLeft);
        // The label side (the tour's 192, the row's label text begins at 162), the `<` at 391..404 (measured on the highlighted row), the value text at 412..438, the `>` at 447..460 and 464.
        CHECK(ZoneOf(ups, *row1, 192.0f, true) == Zone::Label);
        CHECK(ZoneOf(ups, *row1, 385.0f, true) == Zone::Label);
        CHECK(ZoneOf(ups, *row1, 391.0f, true) == Zone::ValueLeft);
        CHECK(ZoneOf(ups, *row1, 404.0f, true) == Zone::ValueLeft);
        CHECK(ZoneOf(ups, *row1, 425.0f, true) == Zone::ValueRight);
        CHECK(ZoneOf(ups, *row1, 453.0f, true) == Zone::ValueRight);
        CHECK(ZoneOf(ups, *row1, 464.0f, true) == Zone::ValueRight);
        // A row that is not a value row is all label, wherever it is clicked.
        CHECK(ZoneOf(ups, *row1, 398.7f, false) == Zone::Label);
    }
    // "(1832, 1126) = stage (814.2, 500.4)": far right of the card: none (the first runs read it as the row under 500 and still said -1).
    CHECK(HitRow(ups, 814.2f, 500.4f) == -1);
    // The tour's hover over the page's rows (pointer x 200, row y 369 = row 1, 421 = row 3, 469 = row 5): "hover: row 4 (was 0)" and "row 5 (was 4)" in the log were rows 1 and 3.
    CHECK(HitRow(ups, 200, 369) == 1);
    CHECK(HitRow(ups, 200, 421) == 3);
    CHECK(HitRow(ups, 200, 469) == 5);
    // The plan for those clicks: a value row's `>` side steps the value, the label side presses A, a click on no row does nothing.
    CHECK(PlanLeftClick(true, 1, Zone::ValueRight, true).act == Act::Right);
    CHECK(PlanLeftClick(true, 1, Zone::ValueLeft, true).act == Act::Left);
    CHECK(PlanLeftClick(true, -1, Zone::Label, false).act == Act::None);
}

void TestCardShape() {
    // The map and its inverse.
    for (float x : {160.0f, 250.0f, 400.0f, 550.0f})
        for (float v : {-270.0f, -230.0f, -90.0f})
            CHECK(Near(CardV(kPauseCard, x, CardY(kPauseCard, x, v)), v, 1e-2));
    CHECK(CardV(kPauseCard, 4000.0f, 300.0f) > 1.0e8f);  // far right of the card: no scale there: never a row
    // Scrolled lists: slot j shows row scroll + j; the hit gives the list's row, not the slot's.
    CardSlot slots[6];
    for (int i = 0; i < 6; ++i) {
        slots[i].row = 4 + i;
        slots[i].x = 23.2f;
        slots[i].y = 78.9f + 35.0f * float(i);
        slots[i].visible = true;
    }
    const Layout scrolled = CardLayout(slots, 6, 344.7f, false, 0.0f);
    CHECK(scrolled.rows[0].row == 4 && scrolled.rows[5].row == 9);
    CHECK(HitRow(scrolled, 330, 341) == 4);
    CHECK(HitRow(scrolled, 330, 465) == 9);
    // Hidden slots are left out and do not break the pitch (a hidden one between two shown is two rows of distance).
    slots[2].visible = false;
    const Layout gap = CardLayout(slots, 6, 344.7f, false, 0.0f);
    CHECK(gap.n == 5 && Near(gap.v_pitch, 35.0, 1e-4) && Plausible(gap));
    CHECK(HitRow(gap, 330, 389) == -1);  // the hidden row's own band belongs to nobody (row 6 would be at 390)
    // The page sliding in: the slots' x is -129 at its start; the rows are the same (hit testing does not use the slot's x).
    for (int i = 0; i < 6; ++i) slots[i].x = -106.4f, slots[i].visible = true;
    const Layout sliding = CardLayout(slots, 6, 344.7f, false, 0.0f);
    CHECK(Plausible(sliding) && HitRow(sliding, 330, 341) == 4);
    // Calibration: the shift moves every row.
    const Layout shifted = CardLayout(slots, 6, 344.7f, false, 10.0f);
    CHECK(HitRow(shifted, 330, 351) == 4 && HitRow(shifted, 330, 341) == 4 && HitRow(shifted, 330, 330) == -1);
    // Plausible refuses a card layout whose pitch is not a text row's; the relaxed test opens the stage limits for the rows of the old reading (the log's pitch 17.5 at x -170).
    CardSlot odd[3];
    for (int i = 0; i < 3; ++i) odd[i] = CardSlot{i, 23.2f, 78.9f + 150.0f * float(i), true};
    CHECK(!Plausible(CardLayout(odd, 3, 344.7f, false, 0.0f)));
    SlotIn flat[6];
    for (int i = 0; i < 6; ++i) {
        flat[i].row = i;
        flat[i].to_stage = Slot(0, 0, -170.0f + 0.0f, 89.0f + 17.5f * float(i));
        flat[i].visible = true;
    }
    const Layout old_read = BuildLayout(flat, 6, 0.0f, 0.0f);
    CHECK(!Plausible(old_read));
    CHECK(PlausibleRelaxed(old_read));
    CHECK(!PlausibleRelaxed(Layout{}));
}

void TestModelLayout() {
    // The model of the card for a page whose clips cannot be read: the pause page's three rows (tour pointer (330, 341) (330, 365) (330, 391)).
    const Layout hub = ModelLayout(0, 3, ModelPage::Hub);
    CHECK(hub.n == 3 && Plausible(hub) && hub.card);
    CHECK(HitRow(hub, 330, 341) == 0);
    CHECK(HitRow(hub, 330, 365) == 1);
    CHECK(HitRow(hub, 330, 391) == 2);
    CHECK(HitRow(hub, 330, 420) == -1);  // under the last row
    CHECK(HitRow(hub, 700, 341) == -1);  // right of the panel
    // The wide kind has the arrows' zones.
    const Layout page = ModelLayout(0, 6, ModelPage::Settings);
    CHECK(page.n == 6 && Plausible(page) && page.wide);
    CHECK(HitRow(page, 192, 346) == 0);
    const RowBox* row1 = BoxOfRow(page, 1);
    CHECK(row1 != nullptr);
    if (row1) {
        CHECK(ZoneOf(page, *row1, 192, true) == Zone::Label);
        CHECK(ZoneOf(page, *row1, 399, true) == Zone::ValueLeft);
        CHECK(ZoneOf(page, *row1, 510, true) == Zone::ValueRight);
    }
    // A scrolled page: slot 0 shows row 4.
    const Layout scrolled = ModelLayout(4, 6, ModelPage::Settings);
    CHECK(scrolled.rows[0].row == 4 && scrolled.rows[5].row == 9 && HitRow(scrolled, 200, 344) == 4);
    // The count is clamped: no rows, and no more than the layout holds.
    CHECK(ModelLayout(0, 0, ModelPage::Hub).n == 0);
    CHECK(ModelLayout(0, 500, ModelPage::Hub).n == kMaxRows);
    CHECK(!Plausible(ModelLayout(0, 0, ModelPage::Hub)));
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
    TestFrameScale();
    TestWorldMatrix();
    TestListWindow();
    TestJudging();
    TestModelLayout();
    TestCardFixtures();
    TestCardClicksOfTheRun();
    TestCardShape();
    std::printf("menu_mouse_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
