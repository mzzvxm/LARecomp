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
    float mx = 0.0f, my = 0.0f;  // the slot's origin in the movie's own units (the matrix before the movie's scale): the pause card's projection works from these
};

// A row on the stage: from the slot's own origin, `width` wide and `pitch` tall (a Fillmenu row is a bar that hangs from its origin).
struct RowBox {
    int row = -1;
    float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
    float v0 = 0.0f, v1 = 0.0f;  // a card layout only: the row's band in the movie's own vertical units (see "The pause card")
    bool Contains(float x, float y, float margin_left) const { return x >= x0 - margin_left && x < x1 && y >= y0 && y < y1; }
    float Width() const { return x1 - x0; }
};

// How a card-drawn movie's units become stage points (the pause menu; see "The pause card" below): the rows' y is not a straight scaling of the movie's y, it depends on the x it is
// read at (the card is turned away from the player).
struct CardMap {
    float a = 0.0f, b = 0.0f, c = 0.0f, d = 0.0f;  // Y = a + b X + (c + d X) v, v = the slot's world y minus the prompt bar's
};

struct Layout {
    int n = 0;
    RowBox rows[kMaxRows];
    float pitch = 0.0f;  // stage pixels between rows (a card layout: at x = 300)
    float width = 0.0f;
    // A card layout (CardLayout): rows are hit in v, not in stage y.
    bool card = false;
    bool wide = false;       // a page with `< value >` rows (the wide card) as against a plain list (the narrow one)
    CardMap map;
    float shift = 0.0f;      // calibration, stage pixels (menu_mouse_row_offset)
    float v_pitch = 0.0f;    // movie units between rows
    // Where a value row's label ends and its `<` ends, as stage x (0 = not known: ZoneOf works from the row's width).
    float zone_label_end = 0.0f;
    float zone_split = 0.0f;
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
    if (l.card) {
        if (!(l.v_pitch >= 15.0f && l.v_pitch <= 70.0f)) return false;
    } else if (!(l.pitch >= 8.0f && l.pitch <= 90.0f)) {
        return false;
    }
    for (int i = 0; i < l.n; ++i) {
        const RowBox& r = l.rows[i];
        if (!std::isfinite(r.x0) || !std::isfinite(r.y0)) return false;
        if (r.x0 < -80.0f || r.x1 > kStageW + 400.0f || r.y0 < -40.0f || r.y1 > kStageH + 80.0f) return false;
        if (i > 0 && !(r.y0 > l.rows[i - 1].y0 && r.row > l.rows[i - 1].row)) return false;
    }
    return true;
}

// The same test with the limits opened up, for the one clip the pause movie and the garage call `slotsA` when the strict one refused it: rows still one strictly under the other and
// finite, a pitch of text rows at any scale, the place anywhere near the stage. A menu read this way is logged as such.
inline bool PlausibleRelaxed(const Layout& l) {
    if (l.n < 1) return false;
    if (!(l.pitch >= 6.0f && l.pitch <= 140.0f)) return false;
    for (int i = 0; i < l.n; ++i) {
        const RowBox& r = l.rows[i];
        if (!std::isfinite(r.x0) || !std::isfinite(r.y0) || !std::isfinite(r.x1) || !std::isfinite(r.y1)) return false;
        if (r.x0 < -400.0f || r.x0 > kStageW + 400.0f || r.y0 < -200.0f || r.y1 > kStageH + 300.0f) return false;
        if (i > 0 && !(r.y0 > l.rows[i - 1].y0 && r.row > l.rows[i - 1].row)) return false;
    }
    return true;
}

// The row under a stage point, -1 for none. The result is the list's row index (RowBox::row), not the position in the layout.
inline float CardV(const CardMap& m, float x, float y);  // (below)
inline int HitRow(const Layout& l, float sx, float sy) {
    if (l.card) {
        // The pointer's place in the movie's vertical units at its own x, then the row whose band holds it.
        const float v = CardV(l.map, sx, sy - l.shift);
        for (int i = 0; i < l.n; ++i) {
            const RowBox& r = l.rows[i];
            if (sx >= r.x0 - kMarginLeft && sx < r.x1 && v >= r.v0 && v < r.v1) return r.row;
        }
        return -1;
    }
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
// A layout that knows where the arrows are (the pause card's) says so in stage x; any other goes by the row's width as above.
inline Zone ZoneOf(const Layout& l, const RowBox& box, float sx, bool value_row) {
    if (!value_row || l.zone_label_end <= 0.0f || l.zone_split <= l.zone_label_end) return ZoneOf(box, sx, value_row);
    if (sx < l.zone_label_end) return Zone::Label;
    return sx < l.zone_split ? Zone::ValueLeft : Zone::ValueRight;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Where a movie is drawn (round 2: the first run read the garage's rows 20 stage pixels from the top-left corner and 35 apart, where the picture has them at 870 / 244 and 22.6)
// ---------------------------------------------------------------------------------------------------------------------------------
//
// Two things the first reading left out. (1) The player keeps every instance's WORLD matrix in the instance (+32: a b c d tx ty, written by the draw of the last frame, sub_825FB348:
// world = local x the parent's world), so the place a row is drawn at can be read, whatever the local matrices up the chain say (ui_smooth.cpp's header has the layout). (2) A movie is
// not drawn 1:1: sub_825EE428 puts the movie's frame (the swfFILE's +36 / +44, floats) onto the game's screen (the display state's width x kx and height x ky), so a movie authored on a
// bigger frame is drawn smaller (the garage's menu: 35 movie units between its rows, 22.6 on the stage). stage = world x (screen x k / frame).

struct StageScale {
    float x = 1.0f, y = 1.0f;
    bool applied = false;  // false: the numbers were missing or absurd, the movie is taken as drawn 1:1
};

inline StageScale FrameScale(float screen_w, float kx, float screen_h, float ky, float frame_w, float frame_h) {
    StageScale s;
    const auto sane = [](float v, float lo, float hi) { return std::isfinite(v) && v >= lo && v <= hi; };
    if (!sane(screen_w, 160.0f, 16384.0f) || !sane(screen_h, 90.0f, 16384.0f) || !sane(kx, 0.1f, 8.0f) || !sane(ky, 0.1f, 8.0f) || !sane(frame_w, 64.0f, 16384.0f) ||
        !sane(frame_h, 36.0f, 16384.0f))
        return s;
    // The game's own arithmetic: the screen's size is cut to a whole number of pixels before the division.
    const float sx = float(int(screen_w * kx)) / frame_w;
    const float sy = float(int(screen_h * ky)) / frame_h;
    if (!sane(sx, 0.2f, 5.0f) || !sane(sy, 0.2f, 5.0f)) return s;
    s.x = sx;
    s.y = sy;
    s.applied = true;
    return s;
}

// A matrix in the movie's units -> the same matrix to the stage.
inline Affine Scaled(const Affine& m, const StageScale& s) {
    Affine r;
    r.a = m.a * s.x;
    r.c = m.c * s.x;
    r.tx = m.tx * s.x;
    r.b = m.b * s.y;
    r.d = m.d * s.y;
    r.ty = m.ty * s.y;
    return r;
}

// An instance's stored world matrix says something only if it is finite and not flat: an instance the player has not drawn yet has zeros there.
inline bool WorldUsable(const Affine& m) {
    if (!FiniteAffine(m)) return false;
    const float det = m.a * m.d - m.b * m.c;
    return std::fabs(det) > 1e-6f;
}

// Which matrix a row's place comes from: the world matrix the draw left in the instance, else the fold of the local matrices up the chain, else none.
enum class MatrixSource : uint8_t { None, World, Fold };
inline MatrixSource ChooseMatrix(bool world_usable, bool fold_ok) { return world_usable ? MatrixSource::World : (fold_ok ? MatrixSource::Fold : MatrixSource::None); }

// ---------------------------------------------------------------------------------------------------------------------------------
// Which rows are on screen, and the model of the pause menu's pages
// ---------------------------------------------------------------------------------------------------------------------------------

// The rows a list draws: mcListViewFixed's vfuncs 660 / 664 (0x8228F4E8, 0x8228F508): from the scroll (0 when the list does not scroll) to the smaller of scroll + the rows that fit and the
// row count (all of them when it does not scroll). Slot j of the movie shows row first + j.
inline void ListWindow(int count, bool scrolls, int scroll, int max_rows, int* first, int* end) {
    if (!scrolls) {
        *first = 0;
        *end = count;
        return;
    }
    *first = scroll;
    *end = std::min(scroll + std::max(max_rows, 0), count);
}

// The clip tree and the list agree: at least one row is shown and every shown row is one the list draws. A list the movie is not showing (an older one) shows rows outside its window or
// none at all.
inline bool SlotsMatchList(const Layout& l, int first, int end) {
    if (l.n < 1) return false;
    for (int i = 0; i < l.n; ++i)
        if (l.rows[i].row < first || l.rows[i].row >= end) return false;
    return true;
}

// Where a list's rows come from (sub_82631F88): a vhsm state (+196: the pause screen's own pages), a table (+192), else the row objects the half word at +180 counts (our pages). The first
// run read only the half word.
enum class RowSource : uint8_t { Adapter, Table, Packed };
inline RowSource RowSourceOf(uint32_t adapter, uint32_t table) { return adapter ? RowSource::Adapter : (table ? RowSource::Table : RowSource::Packed); }

// Whether what was read of a list (or UIMenu) makes sense for a live one.
inline constexpr int kMaxListRows = 64;
enum class Verdict : uint8_t { Ok, NotActive, BadCount, BadHighlight, BadScroll };
inline Verdict JudgeList(bool active, int count, int highlight, bool scrolls, int scroll) {
    if (!active) return Verdict::NotActive;
    if (count < 1 || count > kMaxListRows) return Verdict::BadCount;
    if (highlight < -1 || highlight >= count) return Verdict::BadHighlight;
    if (scrolls && (scroll < 0 || scroll >= count)) return Verdict::BadScroll;
    return Verdict::Ok;
}
// A UIMenu counts its rows by asking for "the child shown on row N" until none: `cap` is where it stops asking (reached = the answer is not trusted).
inline Verdict JudgeUiMenu(int rows, int highlight, int cap) {
    if (rows < 1 || rows >= cap) return Verdict::BadCount;
    if (highlight < -1 || highlight >= rows) return Verdict::BadHighlight;
    return Verdict::Ok;
}

// Which of the two kinds of menu the game rendered last: the list or the UIMenu (their counters; the larger is the later; 0 = never).
inline bool ListIsLater(uint32_t list_seq, uint32_t ui_menu_seq) { return list_seq >= ui_menu_seq; }

// ---------------------------------------------------------------------------------------------------------------------------------
// The pause card (round 3: the numbers below are measured in the pictures of the run kbmp6-kbm-menu-mouse, 2026-11-19)
// ---------------------------------------------------------------------------------------------------------------------------------
//
// The pause movie is not drawn onto the screen: the game draws it into a texture and shows that on a card in its own 3D scene (the script's `menu_group` / `screen_mesh`: ui_smooth.cpp), seen
// through the menu camera. So there is no scaling that takes a slot's place in the movie to a place on the screen, and the display state's factors (what rounds 1 and 2 read, and
// what changes with every movie drawn: 1.0, 0.4, 0.2, 0.356 were all logged for this one) are the texture's, not the card's. Measured in the pictures: the card is turned away a little, so
// its right side is farther: rows converge toward the right (a row 25 stage pixels apart at x 250 is 22.8 apart at x 444) and tilt (the top row falls to the right, the bottom one rises,
// the row in between stays level). Everything fits ONE projection (Y = a + b X + (c + d X) v, 0.2 stage pixels rms, 0.5 at worst over 8 rows of 4 pages: the pause page, the
// SETTINGS tab, the GRAPHICS list and the UPSCALING page) when v is the slot's WORLD y in the movie minus the world y of `prompt_bg` (the prompt bar; the card hangs from its bottom
// edge, so the page's height does not matter: the lists with a title and a description have it 23 units lower). X is the stage x of the point. The row's band in v is its slot's v
// plus or minus half the pitch (35 movie units; the centre of the bar is the slot's world y, whatever the pitch is on screen).
//
// The x of a row is no use for hit testing: the pages slide in from the side (slot x -129 at the start of a page change, 23.2 at rest, +20 on the highlighted row), and the card is
// wider for the pages with `< value >` rows. The list's left edge and right edge are measured per kind: the plain lists (pause page, tabs, the GRAPHICS list) 218..553, the value pages
// 156..574. The `<` of a value row is at stage x 398 on both value pages measured (the UPSCALING page now and the old GRAPHICS QUALITY page), the `>` follows the value's text (453 .. 512).

inline constexpr CardMap kPauseCard{551.0626f, -0.0801920f, 0.806185f, -3.55484e-4f};
// The plain lists' bar spans 218..553; the row begins at 204 (the panel's margin to the left of the bar is the row's too, and a value page read as a plain one by mistake still takes a pointer on its labels).
inline constexpr float kCardNarrowX0 = 204.0f, kCardNarrowX1 = 553.0f;
inline constexpr float kCardWideX0 = 156.0f, kCardWideX1 = 574.0f;
inline constexpr float kCardLabelEnd = 386.0f;  // a value row: left of this is the label (the `<` begins at 388)
inline constexpr float kCardArrowSplit = 411.0f;  // between the `<` (center 398) and the value's text (412 .. 438 for OFF); the text and the `>` after it step forward
inline constexpr float kCardPitchAtX = 300.0f;  // where Layout::pitch is worked out

inline float CardY(const CardMap& m, float x, float v) { return m.a + m.b * x + (m.c + m.d * x) * v; }

// The other way: the v a stage point has at its own x. A huge number when the card's scale there is not positive (a point far off to the right).
inline float CardV(const CardMap& m, float x, float y) {
    const float s = m.c + m.d * x;
    return s > 0.05f ? (y - m.a - m.b * x) / s : 1.0e9f;
}

// One `slotN` of the pause movie as the glue read it: its WORLD origin in the movie's units (what the last draw left in the instance), whether it is shown, and the row it shows.
struct CardSlot {
    int row = -1;
    float x = 0.0f, y = 0.0f;
    bool visible = false;
};

inline constexpr float kCardDefaultPitch = 35.0f;  // the movie's units between slots (Fillmenu's list_interline)

// The slots -> a card layout. `prompt_y` is the world y of `prompt_bg`; `wide` is the page kind; `shift` moves every row (calibration, stage pixels). Slots in row order.
inline Layout CardLayout(const CardSlot* slots, int count, float prompt_y, bool wide, float shift) {
    Layout l;
    l.card = true;
    l.map = kPauseCard;
    l.wide = wide;
    l.shift = shift;
    float vs[kMaxRows];
    int idx[kMaxRows];
    int n = 0;
    for (int i = 0; i < count && n < kMaxRows; ++i) {
        if (!slots[i].visible || !std::isfinite(slots[i].y) || !std::isfinite(prompt_y)) continue;
        vs[n] = slots[i].y - prompt_y;
        idx[n] = slots[i].row;
        l.rows[n].row = slots[i].row;
        ++n;
    }
    l.n = n;
    l.v_pitch = MedianPitch(vs, idx, n, kCardDefaultPitch);
    l.pitch = (l.map.c + l.map.d * kCardPitchAtX) * l.v_pitch;
    const float x0 = wide ? kCardWideX0 : kCardNarrowX0;
    const float x1 = wide ? kCardWideX1 : kCardNarrowX1;
    l.width = x1 - x0;
    if (wide) {
        l.zone_label_end = kCardLabelEnd;
        l.zone_split = kCardArrowSplit;
    }
    for (int i = 0; i < n; ++i) {
        RowBox& r = l.rows[i];
        r.v0 = vs[i] - 0.5f * l.v_pitch;
        r.v1 = vs[i] + 0.5f * l.v_pitch;
        r.x0 = x0;
        r.x1 = x1;
        r.y0 = CardY(l.map, x0, r.v0) + shift;
        r.y1 = CardY(l.map, x0, r.v1) + shift;
    }
    return l;
}

// The pause pages when the movie's clips cannot be read at all but the list can: the first page's slots (world y 78.9, 35 apart) under a prompt bar at 344.7 (the numbers of every page
// with no title; a page with one has both 23 lower, which moves the rows 3 stage pixels). Used only then, and the log says so.
enum class ModelPage : uint8_t { Hub, Settings };
inline constexpr float kModelSlot0Y = 78.9f;
inline constexpr float kModelPromptY = 344.7f;

inline Layout ModelLayout(int first_row, int shown, ModelPage page) {
    shown = std::clamp(shown, 0, kMaxRows);
    CardSlot slots[kMaxRows];
    for (int i = 0; i < shown; ++i) {
        slots[i].row = first_row + i;
        slots[i].x = 23.2f;
        slots[i].y = kModelSlot0Y + kCardDefaultPitch * float(i);
        slots[i].visible = true;
    }
    return CardLayout(slots, shown, kModelPromptY, page == ModelPage::Settings, 0.0f);
}

// ---------------------------------------------------------------------------------------------------------------------------------
// The garage card (round 5: measured in run 4's pictures, 2880 x 1620 = stage x 2.25; tools in h11-work\kbm-p6-5)
// ---------------------------------------------------------------------------------------------------------------------------------
//
// The garage's first menu (GARAGE: MANAGE VEHICLES, CUSTOMIZE VEHICLE, VEHICLE SHOWROOM, EXIT GARAGE) and its pop-ups (EXIT: YES / NO) are drawn on the same kind of card as the pause menu:
// a quad in the game's 3D menu scene, at the right of the screen, turned a little (the rows' separators rise to the right by 0.040 stage pixels per pixel at the top and by 0.010 at the bottom,
// and the rows are 21.7 apart at the card's left edge and 23.5 at its right). The movie's clips give nothing to read here (the garage's menu clip has no `slotN` children that show: run 4's
// dump had only the icon lists, the level meters and the cursor), so the rows are placed by the picture alone: a separator pair (two thin lines 2.4 stage pixels apart) lies between every two
// rows, and the row's band is the space between the centres of two pairs. 520 measurements (5 separators x 52 columns x 2 resting places of the card) fit one projection,
//     Y = a + b X + (c + d X) s,   s = the separator's number (0 = above the first row), X = stage x,
// with rms 0.06 stage pixels (the lines were tracked to 0.1 pixel of the 2880-wide picture), worst 0.17. It is written in the pause card's form (CardMap, v = 35 s) so the same inversion
// serves both. The card rests in two places, 3.3 stage pixels apart (the first visit to the garage and every return from a sub-menu, 7.5 of the picture's pixels: the same picture
// shifted by exactly (0, 7) pixels, nothing else changed, in 5 pictures each): the model sits in the middle, so it is 1.6 off either (a row is 22.7). The pop-up's YES / NO are the
// first two rows' places in the second resting place (pictures 34..40: separators at 512.3 / 564.6 / 616.7, the same as the menu's after a return).
//
// Left and right: the highlight bar spans picture x 1954..2477 at every row measured (rows 0 and 1, both resting places) = stage 868.4..1100.9.
inline constexpr float kGarageRowUnit = 35.0f;  // v units per row: the pause card's units, so Plausible's v-pitch test (15..70) holds
inline constexpr CardMap kGarageCard{269.7848f, -0.0398466f, 0.431454f, 2.17459e-4f};
inline constexpr float kGarageX0 = 868.4f, kGarageX1 = 1100.9f;
inline constexpr float kGaragePitchAtX = 1000.0f;
// The card's two resting places and the model's, for the log and the tests: separator 0's stage y at x 1000.
inline constexpr float kGarageSep0YFirst = 228.3f, kGarageSep0YReturn = 231.6f;

// The first `rows` rows of the garage card (a menu or a pop-up: both hang from the top of the card). `shift` is the calibration (menu_mouse_row_offset).
inline Layout GarageCardLayout(int rows, float shift) {
    Layout l;
    rows = std::clamp(rows, 0, kMaxRows);
    l.card = true;
    l.map = kGarageCard;
    l.shift = shift;
    l.v_pitch = kGarageRowUnit;
    l.pitch = (l.map.c + l.map.d * kGaragePitchAtX) * kGarageRowUnit;
    l.width = kGarageX1 - kGarageX0;
    l.n = rows;
    for (int i = 0; i < rows; ++i) {
        RowBox& r = l.rows[i];
        r.row = i;
        r.v0 = kGarageRowUnit * float(i);
        r.v1 = kGarageRowUnit * float(i + 1);
        r.x0 = kGarageX0;
        r.x1 = kGarageX1;
        r.y0 = CardY(l.map, kGarageX0, r.v0) + shift;
        r.y1 = CardY(l.map, kGarageX0, r.v1) + shift;
    }
    return l;
}

// Which page of the garage a UIMenu is, for the card above. Only two pages have a picture: the garage's FIRST menu (its own UIMenu, the game's base class, a few rows: the first such menu
// the garage movie shows is it, and it stays the same object for the whole visit) and a two-row pop-up (EXIT: YES / NO). Every other page (MANAGE VEHICLES, CUSTOMIZATION, the showroom's)
// is another size of card with other rows: it is not placed by this card, it falls back.
enum class GaragePage : uint8_t { None, Main, Popup };
inline constexpr int kGarageMainRowsMin = 2, kGarageMainRowsMax = 6, kGaragePopupRows = 2;

class GarageTracker {
public:
    // Not in the garage (any other movie): forget the visit.
    void Leave() {
        root_ = 0;
        main_ = 0;
    }
    // `root` = the garage movie's root instance (a new one is a new visit), `menu` the UIMenu filled last, `base_class` it is the game's own UIMenu class (not a class that only shares its
    // highlight and row slots), `rows` its row count.
    GaragePage Classify(uint32_t root, uint32_t menu, bool base_class, int rows) {
        if (!root || !menu) return GaragePage::None;
        if (root != root_) {
            root_ = root;
            main_ = 0;
        }
        if (base_class) {
            if (rows < kGarageMainRowsMin || rows > kGarageMainRowsMax) return GaragePage::None;
            if (main_ == 0) main_ = menu;
            return menu == main_ ? GaragePage::Main : GaragePage::None;
        }
        return rows == kGaragePopupRows ? GaragePage::Popup : GaragePage::None;
    }
    uint32_t MainMenu() const { return main_; }

private:
    uint32_t root_ = 0;
    uint32_t main_ = 0;
};

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

// Whether a menu's rows, as read, may be believed for hover and for clicks on rows. The pause movie's are (a projection fitted to its card, four pages, rms 0.22 stage pixels), and so are
// the garage card's (GarageCardLayout: the caller passes true for them). Another
// movie's are a clip-tree layout through the display state's scale, which the one picture of such a menu (the garage, round 1) contradicts: they are believed only when the debug setting
// menu_mouse_trust_layouts says so, until a run has measured them. A menu that is not believed is a fallback menu (hover off, a click is ACCEPT on the highlighted row).
inline bool LayoutTrusted(bool pause_movie, bool trust_setting) { return pause_movie || trust_setting; }

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

// ---------------------------------------------------------------------------------------------------------------------------------
// Where a click was
// ---------------------------------------------------------------------------------------------------------------------------------
//
// The window layer reports a button going down with a place, and that place is NOT the click's own: SDL's Windows backend throws away the position of WM_LBUTTONDOWN (its lParam) and
// puts the pointer position it last had, from the last motion it took, on the event. That position is replaced by the desktop cursor's own at several moments that have nothing to do
// with the click (WM_MOUSELEAVE and a focus change read the message's cursor position; a captured mouse is polled with GetCursorPos on every pump while no leave is being tracked), so
// a click that comes a while after the last motion can land where the desktop cursor is, not where the pointer was moved to. The tour of run 3 had exactly that: the click on GO TO
// GARAGE came 2.2 s after the pointer's last move and was read at the same window point (1832, 1126) in three runs, the desktop cursor's resting place, while every click 0.25 s
// after its move was right.
//
// A real mouse's WM_LBUTTONDOWN carries the click's own client position, and so does the message a tour posts. So the click takes its place from its own message: the window's message hook
// (menu_mouse.cpp, a WH_GETMESSAGE hook on the UI thread: it sees the message before SDL does; SDL_SetWindowsMessageHook did not work, the exe has its own copy of SDL) notes the position of every button-down message, and the button-down event the listener gets
// afterwards takes the oldest note of that button that is still fresh. No fresh note (a platform with no message hook, raw mouse input, a hook another part took over): the event's own place.
enum class Button : uint8_t { Left, Right, Back, Other };
inline constexpr int kButtonCount = 4;
inline constexpr int kClickNotes = 4;                   // button-down messages kept per button until their events arrive (a double click is two)
inline constexpr uint64_t kClickNoteMaxAgeMs = 250;     // the message and its event are handled within one pump of the UI thread; a note older than this belongs to an event that never came

struct ClickPick {
    int32_t x = 0, y = 0;     // the place the click takes
    bool from_message = false;  // from the click's own message (false: the event's place, no fresh note)
};

class ClickPlaces {
public:
    // A button-down MESSAGE was seen (the window's message hook, UI thread), at this client place.
    void NoteMessage(Button b, int32_t x, int32_t y, uint64_t now_ms) {
        const int i = int(b);
        if (i < 0 || i >= kButtonCount) return;
        Queue& q = q_[i];
        if (q.n == kClickNotes) {  // full: the oldest goes
            for (int k = 1; k < q.n; ++k) q.e[k - 1] = q.e[k];
            --q.n;
        }
        q.e[q.n++] = Entry{x, y, now_ms};
        ++noted_;
    }
    // A button-down EVENT arrived (the window listener, UI thread) with the event's own place: the place the click takes.
    ClickPick TakeEvent(Button b, int32_t event_x, int32_t event_y, uint64_t now_ms) {
        ClickPick pick;
        pick.x = event_x;
        pick.y = event_y;
        const int i = int(b);
        if (i >= 0 && i < kButtonCount) {
            Queue& q = q_[i];
            int drop = 0;  // notes too old to be this event's (their event never came)
            while (drop < q.n && now_ms - q.e[drop].t > kClickNoteMaxAgeMs) ++drop;
            if (drop) {
                for (int k = drop; k < q.n; ++k) q.e[k - drop] = q.e[k];
                q.n -= drop;
                stale_ += drop;
            }
            if (q.n > 0) {
                pick.x = q.e[0].x;
                pick.y = q.e[0].y;
                pick.from_message = true;
                for (int k = 1; k < q.n; ++k) q.e[k - 1] = q.e[k];
                --q.n;
            }
        }
        (pick.from_message ? from_message_ : from_event_) += 1;
        return pick;
    }
    uint32_t Noted() const { return noted_; }
    uint32_t FromMessage() const { return from_message_; }
    uint32_t FromEvent() const { return from_event_; }
    uint32_t Stale() const { return stale_; }

private:
    struct Entry {
        int32_t x = 0, y = 0;
        uint64_t t = 0;
    };
    struct Queue {
        Entry e[kClickNotes];
        int n = 0;
    };
    Queue q_[kButtonCount];
    uint32_t noted_ = 0, from_message_ = 0, from_event_ = 0, stale_ = 0;
};

// A button-down message of the window: which button it is (WM_LBUTTONDOWN / WM_LBUTTONDBLCLK, WM_RBUTTON..., WM_XBUTTON... with XBUTTON1 = the back button), false for any other
// message. `msg` is the message id and `wparam_hi` the high word of its wParam (the X button number). The ids are Windows' (winuser.h) and written here so the rules need no Windows header.
inline constexpr uint32_t kWmLButtonDown = 0x0201, kWmLButtonDblClk = 0x0203, kWmRButtonDown = 0x0204, kWmRButtonDblClk = 0x0206, kWmXButtonDown = 0x020B, kWmXButtonDblClk = 0x020D;
inline bool ButtonOfMessage(uint32_t msg, uint32_t wparam_hi, Button* out) {
    switch (msg) {
        case kWmLButtonDown:
        case kWmLButtonDblClk: *out = Button::Left; return true;
        case kWmRButtonDown:
        case kWmRButtonDblClk: *out = Button::Right; return true;
        case kWmXButtonDown:
        case kWmXButtonDblClk: *out = wparam_hi == 1 ? Button::Back : Button::Other; return true;
        default: return false;
    }
}

// A message's lParam as a client place (two signed 16-bit halves: GET_X_LPARAM / GET_Y_LPARAM).
inline void ClientOfLParam(uint64_t lparam, int32_t* x, int32_t* y) {
    *x = int32_t(int16_t(uint16_t(lparam & 0xFFFFu)));
    *y = int32_t(int16_t(uint16_t((lparam >> 16) & 0xFFFFu)));
}

// The thread message hook (WH_GETMESSAGE, menu_mouse.cpp GetMessageHook) is called with `code` HC_ACTION (0) for a message that is being handed out, and with a negative code for
// the hook chain's own business (the proc must pass those on untouched). `wparam` says whether the message is removed from the queue (PM_REMOVE, 1) or only looked at
// (PM_NOREMOVE, 0: SDL peeks ahead for the key messages it pairs, the same message comes again). A message counts once: when it is removed.
inline constexpr int kHcAction = 0;
inline constexpr uint64_t kPmRemove = 1;
inline bool HookMessageUsable(int code, uint64_t wparam) { return code == kHcAction && wparam == kPmRemove; }

}  // namespace larecomp::menumouse
