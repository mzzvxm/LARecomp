#pragma once
//
// A pause-menu row that only means something while another row is on.
//
// RowDynamics (menu_items.h) lets a row grey itself out (`enabled`) and say why (`suffix`), computed each time
// the row is drawn. A row that depends on another row needs the same two functions every time: grey while
// that row is off, and a short reason after the label. This header makes them from one place, so a new
// dependent row is one line and every dependent row behaves the same (the test is row_dependency_test.cpp).
//
//   bool ShadowsOn() { return !CvarGetBool("perf_no_shadows"); }
//   constexpr RowDynamics kShadowDetailDynamics =
//       DependentRow<ShadowsOn, kNeedsShadowsReason>({.str_label = ShadowDetailLabel});
//
// What a grey row does is the game's own disabled row (pause_menu.cpp, kRowValueEnabled): the label is drawn
// grey with no value, the cursor still lands on it (a move never skips a disabled row), and left / right / A /
// a click do nothing (the list asks the row first). Nothing is written to its setting, so the value it had
// comes back when the row it needs is turned on again. The state is read live on every draw and on every
// poll, so turning the other row on or off (by A, by the F4 panel or by the console) changes this one at once.
//
// Width. A page is as wide as its widest label (suffix included) plus its widest value over every row and
// every state (pause_menu.cpp, "Page width"; 41 characters at most), so a reason is short: it counts against
// the page in every row that has it. Header only, so a standalone program can check it.

#include "menu_items.h"

namespace mcla_menu {

// What a grey row says after its label while SHADOWS is off: "TRAFFIC SHADOWS (NO SHADOW)". At most 12
// characters: LIGHTING AND SHADOWS has labels of 15 ("FOLIAGE SHADOWS", "TRAFFIC SHADOWS") and values of 14
// ("AUTO (DEFAULT)"), and 15 + 12 + 14 is the 41 a page can take. " (SHADOWS OFF)" would make it 43.
inline constexpr char kNeedsShadowsReason[] = " (NO SHADOW)";

// The two functions of a row that needs another row: `On` says whether the row it needs is on, `Reason` is
// the text after the label while it is not (empty while it is). One instance per (On, Reason) pair.
template <bool (*On)(), const char* Reason>
struct DependsOn {
    static bool Enabled() { return On(); }
    static const char* Suffix() { return On() ? "" : Reason; }
};

// `own` (the row's own labels and list, if it has any) with `enabled` and `suffix` set from the row it
// needs. A row that already has an `enabled` or a `suffix` of its own composes them itself: this replaces both.
template <bool (*On)(), const char* Reason>
constexpr RowDynamics DependentRow(RowDynamics own = {}) {
    own.enabled = &DependsOn<On, Reason>::Enabled;
    own.suffix = &DependsOn<On, Reason>::Suffix;
    return own;
}

// RowDynamics::enabled; a row without one is always enabled. The menu asks this before it takes an A, a click
// or a step on a row (pause_menu.cpp MenuFormAccept, PollNativeRows) and before it draws the row.
inline bool RowEnabled(const ItemDef& it) {
    return !it.dynamics || !it.dynamics->enabled || it.dynamics->enabled();
}

}  // namespace mcla_menu
