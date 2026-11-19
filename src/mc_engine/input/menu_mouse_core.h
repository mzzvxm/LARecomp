#pragma once
//
// The mouse in the game's menus: the pure part (menu_mouse.cpp is the glue), written so that larecomp_menu_mouse_test
// (menu_mouse_test.cpp) can check every rule without the game. KBM-DESIGN.md, phase P6.
//
// What the game's menus are: Flash movies on a 1280 x 720 stage, laid out for 16:9 and put in the middle 16:9 block of the picture's frame
// (mc/frame: the `hud` rectangle; a wider or taller picture squeezes the UI into it). They take no mouse at all: the UI's ioMapper has pad
// and key sources and nothing for a pointer. So the mouse is made of the two things the game does have:
//
//   rows      the movie lays its rows out in `menu.slotsA.slot0 .. slotN` (the pause movie's Fillmenu: `_y = row * list_interline + list_margin`; ui_palette.cpp
//             reads the same clips), each an instance with a local matrix; walking the instance tree up to the root gives every row's place on the
//             stage. This file turns those places into row boxes, the pointer into a stage point, and the stage point into a row.
//   keys      what the pointer does goes in through the game's own menu buttons (a virtual key per button, kbm_input.h MenuKey): ACCEPT, BACK, the four
//             arrows. A click on a row therefore does exactly what A on it does (the sound, the transition, the form's own accept handler), and
//             the wheel is UP / DOWN. Nothing here writes into a menu's state; the one direct call is the list's own "select row" slot, for the hover.
//
// Where the rows cannot be read (a movie with no `slotsA`, a layout that does not pass Plausible), the menu falls back to what needs no
// geometry: hover does nothing, a left click is ACCEPT on whatever is highlighted, the wheel and the right button work as always.
//
// No game, no SDK, no window in this file.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "mc_engine/display/frame_fit.h"

namespace larecomp::menumouse {

// The stage the game's Flash movies are laid out on (ultrawide.h kStageHalfWidth / kStageHalfHeight: the game's screen is 1280 x 720).
inline constexpr float kStageW = 1280.0f;
inline constexpr float kStageH = 720.0f;

// ---------------------------------------------------------------------------------------------------------------------------------
// The window and the stage
// ---------------------------------------------------------------------------------------------------------------------------------

// The part of the window the menus are drawn in, in the window's pixels: the middle 16:9 block of the picture's frame (frame_fit.h Layout::hud).
struct HudRect {
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
    bool empty() const { return !(w > 0.0f && h > 0.0f); }
};

// The frame (mc::ultrawide::FrameFor) and the UI's two fractions (UiWidthFraction / UiHeightFraction: 16:9 over the aspect ratio when the picture is wider than the
// game's, the aspect ratio over 16:9 when it is taller, else 1) -> where the UI lies. The same block frame_fit.h's LayoutOf calls `hud`.
inline HudRect HudOf(const mc::frame::Rect& frame, double ui_width_fraction, double ui_height_fraction) {
    HudRect r;
    if (frame.empty()) return r;
    const double fw = std::clamp(ui_width_fraction, 0.05, 1.0);
    const double fh = std::clamp(ui_height_fraction, 0.05, 1.0);
    r.w = float(double(frame.w) * fw);
    r.h = float(double(frame.h) * fh);
    r.x = float(frame.x) + (float(frame.w) - r.w) * 0.5f;
    r.y = float(frame.y) + (float(frame.h) - r.h) * 0.5f;
    return r;
}

// A window pixel as a point of the stage. The result is always computed (a pointer outside the UI block gets a point outside 0..1280 / 0..720);
// the return value says whether it is inside the UI block.
inline bool WindowToStage(const HudRect& hud, float px, float py, float* sx, float* sy) {
    if (hud.empty()) {
        *sx = 0.0f;
        *sy = 0.0f;
        return false;
    }
    *sx = (px - hud.x) / hud.w * kStageW;
    *sy = (py - hud.y) / hud.h * kStageH;
    return px >= hud.x && px < hud.x + hud.w && py >= hud.y && py < hud.y + hud.h;
}

// The other way (the log lines, the tour: where on the window a stage point is).
inline void StageToWindow(const HudRect& hud, float sx, float sy, float* px, float* py) {
    *px = hud.x + sx / kStageW * hud.w;
    *py = hud.y + sy / kStageH * hud.h;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Instance matrices
// ---------------------------------------------------------------------------------------------------------------------------------

// A swf instance's local matrix, as swf_dom.h has it: x' = a*x + c*y + tx, y' = b*x + d*y + ty.
struct Affine {
    float a = 1.0f, b = 0.0f, c = 0.0f, d = 1.0f, tx = 0.0f, ty = 0.0f;
};

// `child` first, then `parent`: the matrix of a point in the child's space in the parent's space. Folding the instance chain from the slot up to the
// root with this gives the slot's matrix to the stage.
inline Affine Then(const Affine& child, const Affine& parent) {
    Affine r;
    r.a = parent.a * child.a + parent.c * child.b;
    r.b = parent.b * child.a + parent.d * child.b;
    r.c = parent.a * child.c + parent.c * child.d;
    r.d = parent.b * child.c + parent.d * child.d;
    r.tx = parent.a * child.tx + parent.c * child.ty + parent.tx;
    r.ty = parent.b * child.tx + parent.d * child.ty + parent.ty;
    return r;
}

inline void Apply(const Affine& m, float x, float y, float* ox, float* oy) {
    *ox = m.a * x + m.c * y + m.tx;
    *oy = m.b * x + m.d * y + m.ty;
}

inline bool FiniteAffine(const Affine& m) {
    return std::isfinite(m.a) && std::isfinite(m.b) && std::isfinite(m.c) && std::isfinite(m.d) && std::isfinite(m.tx) && std::isfinite(m.ty);
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Rows
// ---------------------------------------------------------------------------------------------------------------------------------

inline constexpr int kMaxRows = 32;

// One `slotN` of the movie as the glue read it.
struct SlotIn {
    int row = -1;            // the list's row index this slot shows (the slot's number plus the list's scroll)
    Affine to_stage;         // slot -> stage
    bool visible = false;    // the slot and every instance above it have alpha
    float child_right = 0.0f;  // the right-most start of anything inside the slot (local x of its children, 0 = none found)
};

// A row on the stage: from the slot's own origin, `width` wide and `pitch` tall (a Fillmenu row is a bar that hangs from its origin).
struct RowBox {
    int row = -1;
    float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
    bool Contains(float x, float y, float margin_left) const { return x >= x0 - margin_left && x < x1 && y >= y0 && y < y1; }
    float Width() const { return x1 - x0; }
};

struct Layout {
    int n = 0;
    RowBox rows[kMaxRows];
    float pitch = 0.0f;
    float width = 0.0f;
};

// What a row's height is when only one row shows (the pause menu's rows are 25 stage pixels apart in the pictures of its pages).
inline constexpr float kFallbackPitch = 25.0f;
// What a row's width is when nothing inside the slot says (the pause menu's first page: 316; the settings pages 430).
inline constexpr float kMinRowWidth = 320.0f;
inline constexpr float kMaxRowWidth = 640.0f;
// A pointer this far left of the row's origin still counts as on it (the `>` marker hangs there).
inline constexpr float kMarginLeft = 14.0f;
// What a value arrow takes beyond its own start.
inline constexpr float kArrowAllowance = 24.0f;

// The distance between consecutive rows: the median, over the pairs of neighbours, of the step in y per row index (a hidden slot between two shown ones is two rows of
// distance, not one). `rows[i]` is the list row of `ys[i]`, in order. `fallback` when there is no step.
inline float MedianPitch(const float* ys, const int* rows, int n, float fallback) {
    float steps[kMaxRows];
    int k = 0;
    for (int i = 1; i < n && k < kMaxRows; ++i) {
        const int rows_apart = rows[i] - rows[i - 1];
        if (rows_apart < 1) continue;
        const float d = (ys[i] - ys[i - 1]) / float(rows_apart);
        if (d > 1.0f) steps[k++] = d;
    }
    if (k == 0) return fallback;
    std::sort(steps, steps + k);
    return steps[k / 2];
}

// The row width: what the slots' own content says (its right-most child plus an arrow), never under kMinRowWidth or over kMaxRowWidth; `override_w` above 0 wins.
inline float WidthFor(float widest_child_right, float override_w) {
    if (override_w > 0.0f) return std::clamp(override_w, 40.0f, 1280.0f);
    return std::clamp(widest_child_right > 0.0f ? widest_child_right + kArrowAllowance : 0.0f, kMinRowWidth, kMaxRowWidth);
}

// The visible slots -> row boxes. `y_offset` moves every row (stage pixels, for calibration). Slots must be in row order.
inline Layout BuildLayout(const SlotIn* slots, int count, float width_override, float y_offset) {
    Layout l;
    float ys[kMaxRows];
    float xs[kMaxRows];
    int idx[kMaxRows];
    int n = 0;
    float widest = 0.0f;
    for (int i = 0; i < count && n < kMaxRows; ++i) {
        if (!slots[i].visible || !FiniteAffine(slots[i].to_stage)) continue;
        float x, y;
        Apply(slots[i].to_stage, 0.0f, 0.0f, &x, &y);
        xs[n] = x;
        ys[n] = y;
        l.rows[n].row = slots[i].row;
        idx[n] = slots[i].row;
        widest = std::max(widest, slots[i].child_right);
        ++n;
    }
    l.n = n;
    l.pitch = MedianPitch(ys, idx, n, kFallbackPitch);
    l.width = WidthFor(widest, width_override);
    for (int i = 0; i < n; ++i) {
        l.rows[i].x0 = xs[i];
        l.rows[i].y0 = ys[i] + y_offset;
        l.rows[i].x1 = xs[i] + l.width;
        l.rows[i].y1 = ys[i] + y_offset + l.pitch;
    }
    return l;
}

// Whether the layout can be trusted for hover and click-on-row: at least one row, every row on (or near) the stage, rows strictly one under the other,
// the pitch of a text row. Anything else and the menu falls back (the numbers are logged once per movie).
inline bool Plausible(const Layout& l) {
    if (l.n < 1) return false;
    if (!(l.pitch >= 8.0f && l.pitch <= 90.0f)) return false;
    for (int i = 0; i < l.n; ++i) {
        const RowBox& r = l.rows[i];
        if (!std::isfinite(r.x0) || !std::isfinite(r.y0)) return false;
        if (r.x0 < -80.0f || r.x1 > kStageW + 400.0f || r.y0 < -40.0f || r.y1 > kStageH + 80.0f) return false;
        if (i > 0 && !(r.y0 > l.rows[i - 1].y0 && r.row > l.rows[i - 1].row)) return false;
    }
    return true;
}

// The row under a stage point, -1 for none. The result is the list's row index (RowBox::row), not the position in the layout.
inline int HitRow(const Layout& l, float sx, float sy) {
    for (int i = 0; i < l.n; ++i)
        if (l.rows[i].Contains(sx, sy, kMarginLeft)) return l.rows[i].row;
    return -1;
}

inline const RowBox* BoxOfRow(const Layout& l, int row) {
    for (int i = 0; i < l.n; ++i)
        if (l.rows[i].row == row) return &l.rows[i];
    return nullptr;
}

// Where along a row the pointer is, for the rows with a left / right value (`< VALUE >`: the settings rows). In a screenshot of a settings page the label takes the first 55
// % of the row, the left arrow sits at 61 %, the value is centred at 74 %, the right arrow at 87 %: so the label is the first part, and the value area splits at its centre.
enum class Zone : uint8_t { Label, ValueLeft, ValueRight };
inline constexpr float kValueStart = 0.55f;
inline constexpr float kValueSplit = 0.74f;

inline Zone ZoneOf(const RowBox& box, float sx, bool value_row) {
    if (!value_row || box.Width() <= 0.0f) return Zone::Label;
    const float rel = (sx - box.x0) / box.Width();
    if (rel < kValueStart) return Zone::Label;
    return rel < kValueSplit ? Zone::ValueLeft : Zone::ValueRight;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// What the mouse does
// ---------------------------------------------------------------------------------------------------------------------------------

// A menu button the mouse presses (kbm_input.h MenuKey has the same set; this one is the pure side).
enum class Act : uint8_t { None, Accept, Back, Up, Down, Left, Right };

// A left click. `rows_known`: the layout of the menu passed Plausible and the list's highlight can be read and set. Not known: ACCEPT on whatever is highlighted (the fallback). Known: a click
// on no row does nothing; a click on a row first moves the highlight there (`select_first`) and then presses ACCEPT, or, on a settings row's left or right part, the arrow.
struct ClickPlan {
    Act act = Act::None;
    int row = -1;             // the row to highlight first, -1 = none (the highlight stays where it is)
};

inline ClickPlan PlanLeftClick(bool rows_known, int hit_row, Zone zone, bool value_row) {
    ClickPlan p;
    if (!rows_known) {
        p.act = Act::Accept;
        return p;
    }
    if (hit_row < 0) return p;
    p.row = hit_row;
    p.act = Act::Accept;
    if (value_row && zone == Zone::ValueLeft) p.act = Act::Left;
    if (value_row && zone == Zone::ValueRight) p.act = Act::Right;
    return p;
}

// Hover: the row to highlight, -1 for none. The highlight follows the pointer only when the pointer MOVED (a pointer parked on a row does not fight the keys or the pad), the keyboard and
// mouse are the device in use (input_state.h: jitter and a bump while a controller is in use do not count), the setting is on, and the rows are known.
inline int HoverTarget(bool enabled, bool pointer_moved, bool kbm_active, bool rows_known, int hit_row, int selected_row) {
    if (!enabled || !pointer_moved || !kbm_active || !rows_known) return -1;
    return (hit_row >= 0 && hit_row != selected_row) ? hit_row : -1;
}

// The pointer is wanted on screen: a menu is up, the keyboard and mouse are the device in use, the window has the focus, the setting is on.
inline bool WantsPointer(bool setting_on, bool menu_up, bool kbm_active, bool focused) { return setting_on && menu_up && kbm_active && focused; }

// ---------------------------------------------------------------------------------------------------------------------------------
// The wheel
// ---------------------------------------------------------------------------------------------------------------------------------

// Wheel units -> whole steps. A wheel reports in units (a notch is `per_detent`, 120 on Windows; a smooth wheel or a trackpad reports fractions of it) and nothing may be rounded
// away: the remainder stays. Positive units are a turn away from the player = the highlight goes UP. A flick of many notches is capped (`cap` steps waiting): the list does not run for a
// second after the wheel stopped.
class WheelAccum {
public:
    explicit WheelAccum(int per_detent = 120, int cap = 6) : per_(per_detent > 0 ? per_detent : 120), cap_(cap > 0 ? cap : 1) {}

    void Add(int units) {
        pending_ = std::clamp(pending_ + units, -cap_ * per_, cap_ * per_);
    }
    // One whole step (+1 up, -1 down, 0 none) out of what waits; the rest stays for the next call.
    int TakeOne() {
        if (pending_ >= per_) {
            pending_ -= per_;
            return +1;
        }
        if (pending_ <= -per_) {
            pending_ += per_;
            return -1;
        }
        return 0;
    }
    void Clear() { pending_ = 0; }
    int Pending() const { return pending_; }
    // A whole step is waiting (a half notch is not one yet).
    bool HasStep() const { return pending_ >= per_ || pending_ <= -per_; }

private:
    int per_;
    int cap_;
    int pending_ = 0;
};

// ---------------------------------------------------------------------------------------------------------------------------------
// Pacing the presses
// ---------------------------------------------------------------------------------------------------------------------------------

// A virtual press is down for exactly one input poll, and two presses in consecutive polls are ONE held press to the game (its mapper repeats a held key after a delay, it does not
// count each poll). So a caller that wants separate presses leaves a poll between them: Ready() once every `gap` ticks.
class PressPacer {
public:
    explicit PressPacer(int gap = 2) : gap_(gap > 0 ? gap : 1) {}
    // Once per tick, first.
    void Step() {
        if (cooldown_ > 0) --cooldown_;
    }
    bool Ready() const { return cooldown_ == 0; }
    void Fired() { cooldown_ = gap_; }
    void Reset() { cooldown_ = 0; }

private:
    int gap_;
    int cooldown_ = 0;
};

// ---------------------------------------------------------------------------------------------------------------------------------
// Walking the highlight with the arrow buttons (the menus whose highlight cannot be set directly)
// ---------------------------------------------------------------------------------------------------------------------------------

// The button that moves the highlight from `selected` toward `target`; None when it is there. A menu's highlight may wrap or skip a row the game does not offer, so one press is
// a step in a direction, never a promise: the caller looks at the highlight again after each press.
inline Act WalkStep(int selected, int target) {
    if (selected < 0 || target < 0 || selected == target) return Act::None;
    return target < selected ? Act::Up : Act::Down;
}

// Gives up on a walk that does not move: `Stalled` is true after `limit` presses in a row that left the highlight where it was (the rows are not what the geometry says, or
// the menu does not take the arrows now). The menu then falls back until it is left.
class WalkWatch {
public:
    explicit WalkWatch(int limit = 6) : limit_(limit > 0 ? limit : 1) {}
    // One press sent while the highlight was `selected_before`. When the highlight is where it was at the press before, the press did nothing (one stall); any move starts the count again.
    void Pressed(int selected_before) {
        if (selected_before == last_selected_) ++stalls_;
        else stalls_ = 0;
        last_selected_ = selected_before;
    }
    bool Stalled() const { return stalls_ >= limit_; }
    void Reset() {
        stalls_ = 0;
        last_selected_ = -2;
    }

private:
    int limit_;
    int stalls_ = 0;
    int last_selected_ = -2;
};

// ---------------------------------------------------------------------------------------------------------------------------------
// A click waiting for the highlight
// ---------------------------------------------------------------------------------------------------------------------------------

// A left click on a row that is not highlighted yet: the highlight goes there first and the press follows once it is there (the game reads the press after the highlight changed, not before it).
// `ticks` bounds the wait: a highlight that never arrives drops the click rather than pressing ACCEPT on another row.
struct PendingClick {
    bool active = false;
    int row = -1;
    Act act = Act::None;
    int ticks = 0;
};
inline constexpr int kPendingClickTicks = 30;

inline PendingClick StartPending(const ClickPlan& plan) {
    PendingClick p;
    p.active = true;
    p.row = plan.row;
    p.act = plan.act;
    p.ticks = kPendingClickTicks;
    return p;
}

// One tick of a waiting click. `selected` is the highlight now. Returns the button to press (None while waiting, and when the click was dropped); `done` says the click is over (pressed or dropped).
inline Act StepPending(PendingClick& p, int selected, bool* done) {
    *done = false;
    if (!p.active) {
        *done = true;
        return Act::None;
    }
    if (p.row < 0 || selected == p.row) {
        *done = true;
        p.active = false;
        return p.act;
    }
    if (--p.ticks <= 0) {
        *done = true;
        p.active = false;
    }
    return Act::None;
}

}  // namespace larecomp::menumouse
