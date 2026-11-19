#ifndef REXGLUE_HAS_XEO3_TARGET
#include "menu_mouse.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "imgui.h"
#include <rex/cvar.h>
#include <rex/input/input_system.h>
#include <rex/ppc.h>
#include <rex/ppc/function.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

#include "mc_engine/core/logging.h"
#include "mc_engine/display/ultrawide.h"
#include "mc_engine/hooks/hooks_internal.h"
#include "mc_engine/input/input_state.h"
#include "mc_engine/input/map_mouse.h"
#include "mc_engine/input/menu_mouse_core.h"
#include "mc_engine/keyboard_native.h"
#include "mc_engine/menus/pause_menu.h"
#include "mc_engine/menus/swf_dom.h"
#include "mc_engine/ui/settings.h"

REX_EXTERN(__imp__rex_sub_821E6800);  // play a UI sound by name

REXCVAR_DEFINE_BOOL(menu_mouse, true, "MCLA/Input",
    "Use the mouse in the game's menus: moving the pointer over a row highlights it, a left click is A on it (on the left or right part of a settings row, the arrow: it "
    "steps the value), the right button is B, the wheel moves the highlight. The pointer stays on screen while a menu is up and the mouse is what you last used. Off, "
    "the mouse does nothing in menus.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(menu_mouse_row_width, 0.0, "MCLA/Input",
    "Debug: the width of a menu row for the mouse, in the 1280 x 720 stage's pixels. 0 = from what the row holds (never under 320). Rows the pointer is said to be on but "
    "is not (or the reverse) at the right edge of the list: set this and look again.")
    .range(0.0, 1280.0)
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(menu_mouse_row_offset, 0.0, "MCLA/Input",
    "Debug: moves every menu row's place for the mouse up or down, in the stage's pixels (a row hangs from the origin of its slot; if the highlight lands half a row off "
    "from the pointer, this is the number: about -12 puts the origin in the middle of the row).")
    .range(-60.0, 60.0)
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(menu_mouse_dump, false, "MCLA/Input",
    "Debug: log the clip tree of the movie that is up (the `menu` clip, its slots and what they hold: names, alpha, place on the stage) once, and again each time this is set to "
    "true. For a menu that falls back: this is how its rows are found.")
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace larecomp {
namespace {

namespace mm = larecomp::menumouse;

// ── guest addresses and layouts ─────────────────────────────────────────

// mcListViewFixed (ui_forms.cpp / pause_menu.cpp): row pointers at +176, row count (u16) at +180, the highlighted row at +184, the first row on screen at +256 when +260 is set.
constexpr uint32_t kListRows = 176;
constexpr uint32_t kListCount = 180;
constexpr uint32_t kListSelect = 184;
constexpr uint32_t kListScroll = 256;
constexpr uint32_t kListScrolls = 260;
// vfunc 162 of the list: select a row (list, index). It is what the list's own UP / DOWN handler (sub_826334D0) calls after it has worked out the row and played the sound.
constexpr uint32_t kVfSelectRow = 648;

// UIMenu (pause_menu.cpp): the base class's vtable, the highlighted row (-1 = none) and "the child shown on that row".
constexpr uint32_t kUiMenuVtable = 0x82020654;
constexpr uint32_t kUiMenuSelectedVfunc = 0x150;
constexpr uint32_t kUiMenuChildAtVfunc = 0x1E0;
constexpr int kMaxMenuRows = 32;

// The row classes with `< >` values (pause_menu.cpp: 0x8208E83C is the class behind SUBTITLES and AUTO SAVE and every row of ours with values; 0x82098BFC the raw-number rows such as
// STEERING SENS.). The click on their left / right side steps the value.
constexpr uint32_t kValueRowVtables[] = {0x8208E83C, 0x82098BFC};

// "UI_MENU_UP" / "UI_MENU_DOWN" style scroll sounds: one-entry char* tables the list's key handler plays when the highlight moves (sub_826334D0).
constexpr uint32_t kSoundScrollUp = 0x82826014;
constexpr uint32_t kSoundScrollDown = 0x82826018;

// swfINSTANCE (swf_dom.cpp has the rest): the parent at +184, the first child +188, the next sibling +192, the name +196; the local matrix a b c d tx ty at +8; the alpha byte at
// +79; the dirty byte at +81 (the matrix is rebuilt from _x / _y at the next draw); the timeline's base translation at +92 / +96 and the script's _x / _y at +116 / +120 (a rebuild
// puts base + property into the matrix: sub_825F8C70).
constexpr uint32_t kInstMatrix = 8;
constexpr uint32_t kInstAlpha = 79;
constexpr uint32_t kInstDirty = 81;
constexpr uint32_t kInstBaseX = 92;
constexpr uint32_t kInstBaseY = 96;
constexpr uint32_t kInstPropX = 116;
constexpr uint32_t kInstPropY = 120;
constexpr uint32_t kInstParent = 184;
constexpr uint32_t kInstFirstChild = 188;
constexpr uint32_t kInstNext = 192;
constexpr uint32_t kInstName = 196;

constexpr int kMaxSlots = 24;  // slot0 .. slot23 (ui_palette.cpp RowSlot)

// ── small guest helpers (guest thread) ──────────────────────────────────

bool Plausible(uint32_t ea) { return ea >= 0x10000u && ea < 0xC0000000u; }
bool HeapLike(uint32_t ea) { return ea >= 0x40000000u && ea < 0xE0000000u; }
bool VtableLike(uint32_t ea) { return ea >= 0x82000000u && ea < 0x82FFFFF0u && (ea & 3u) == 0; }

uint32_t R32(const uint8_t* b, uint32_t ea) { return ReadGuestU32(b, ea); }
uint16_t R16(const uint8_t* b, uint32_t ea) { return uint16_t((uint32_t(b[ea]) << 8) | b[ea + 1]); }

// A guest function by address; false when the dispatcher does not have it.
template <typename... Args>
bool CallAt(uint32_t fn_addr, uint32_t* result, Args... args) {
    auto* rt = rex::Runtime::instance();
    auto* dispatcher = rt ? rt->function_dispatcher() : nullptr;
    PPCFunc* fn = dispatcher ? dispatcher->GetFunction(fn_addr) : nullptr;
    if (!fn) return false;
    const uint32_t r = rex::ppc::GuestToHostFunction<uint32_t>(fn, uint32_t(args)...);
    if (result) *result = r;
    return true;
}

// The slot `slot` of an object's vtable as a callable guest function; 0 when the object is not what it should be.
uint32_t VSlot(const uint8_t* b, uint32_t obj, uint32_t slot) {
    if (!Plausible(obj)) return 0;
    const uint32_t vt = R32(b, obj);
    if (!VtableLike(vt)) return 0;
    const uint32_t fn = R32(b, vt + slot);
    return VtableLike(fn) ? fn : 0;
}

void PlayScrollSound(const uint8_t* b, bool up) {
    const uint32_t name = R32(b, up ? kSoundScrollUp : kSoundScrollDown);
    if (name >= 0x82000000u && name < 0x82FFFFF0u) rex::ppc::GuestToHostFunction<void>(__imp__rex_sub_821E6800, name);
}

// ── the window's mouse events (UI thread) -> atomics ────────────────────

std::atomic<uint64_t> g_pos{0};          // the last pointer position, window pixels, x in the high half
std::atomic<uint64_t> g_move_seq{0};     // bumped by every motion event
std::atomic<uint64_t> g_click_pos{0};    // the pointer where the last left button went down
std::atomic<uint32_t> g_left_downs{0};   // button-down counts
std::atomic<uint32_t> g_right_downs{0};
std::atomic<uint32_t> g_back_downs{0};   // the back button (X1)
std::atomic<int32_t> g_wheel_units{0};
std::atomic<bool> g_listener_attached{false};
std::atomic<bool> g_listener_posted{false};
std::atomic<bool> g_wants_pointer{false};

uint64_t Pack(int32_t x, int32_t y) { return (uint64_t(uint32_t(x)) << 32) | uint32_t(y); }
void Unpack(uint64_t v, int32_t* x, int32_t* y) {
    *x = int32_t(uint32_t(v >> 32));
    *y = int32_t(uint32_t(v & 0xFFFFFFFFu));
}

class MenuMouseListener final : public rex::ui::WindowInputListener {
public:
    void OnMouseMove(rex::ui::MouseEvent& e) override {
        g_pos.store(Pack(e.x(), e.y()), std::memory_order_relaxed);
        g_move_seq.fetch_add(1, std::memory_order_relaxed);
    }
    void OnMouseDown(rex::ui::MouseEvent& e) override {
        g_pos.store(Pack(e.x(), e.y()), std::memory_order_relaxed);
        switch (e.button()) {
            case rex::ui::MouseEvent::Button::kLeft:
                g_click_pos.store(Pack(e.x(), e.y()), std::memory_order_relaxed);
                g_left_downs.fetch_add(1, std::memory_order_relaxed);
                break;
            case rex::ui::MouseEvent::Button::kRight: g_right_downs.fetch_add(1, std::memory_order_relaxed); break;
            case rex::ui::MouseEvent::Button::kX1: g_back_downs.fetch_add(1, std::memory_order_relaxed); break;
            default: break;
        }
    }
    void OnMouseWheel(rex::ui::MouseEvent& e) override {
        if (e.scroll_y()) g_wheel_units.fetch_add(e.scroll_y(), std::memory_order_relaxed);
    }
};

MenuMouseListener g_listener;

rex::ui::Window* GetWindow() {
    auto* rt = rex::Runtime::instance();
    auto* isys = rt ? static_cast<rex::input::InputSystem*>(rt->input_system()) : nullptr;
    return isys ? isys->window() : nullptr;
}

// Layer 0 like the map's, the pointer's and the keyboard's listeners: it never marks an event handled, and what an overlay handles first never reaches it. Added on the UI thread, the
// thread that walks the window's listener lists.
void EnsureListener(rex::ui::Window* window) {
    if (g_listener_attached.load(std::memory_order_relaxed) || g_listener_posted.exchange(true)) return;
    const bool queued = window->app_context().CallInUIThreadDeferred([window]() {
        window->AddInputListener(&g_listener, 0);
        g_listener_attached.store(true, std::memory_order_relaxed);
        MC_INFO("[menu-mouse] mouse listener attached to the window");
    });
    if (!queued) g_listener_posted.store(false);
}

// ── what is on screen ───────────────────────────────────────────────────

enum class Kind : uint8_t { None, List, UiMenu };

// The menu the pointer is over, as read this tick.
struct MenuView {
    Kind kind = Kind::None;
    uint32_t obj = 0;        // the list or the UIMenu
    int selected = -1;       // the highlighted row, -1 for none / unknown
    int count = 0;           // rows in the list (UIMenu: the rows its vfunc names)
    mm::Layout layout;       // the rows on the stage
    bool rows_known = false; // the layout is plausible and the highlight can be read: hover and click-on-row work
    char movie[48] = {};
};

// What this file remembers between ticks (guest thread only).
struct State {
    uint64_t tick = 0;
    uint64_t last_move_seq = 0;
    uint32_t last_left = 0, last_right = 0, last_back = 0;
    mm::WheelAccum wheel{int(rex::ui::MouseEvent::kScrollPerDetent), 6};
    mm::PressPacer pacer{2};
    mm::PendingClick pending;
    mm::WalkWatch walk{8};
    int walk_target = -1;
    uint32_t walk_obj = 0;
    mm::Act queue[8] = {};
    int queued = 0;
    uint32_t gave_up_obj = 0;          // a UIMenu whose walk stalled: it falls back until another one is on screen
    uint32_t reported_obj = 0;         // the menu the last [menu] line was about
    int reported_kind = -1;
    char reported_movie[48] = {};
    bool dump_armed = true;
    int logged = 0;
};
State g_state;

constexpr int kMaxLogLines = 800;  // hover / click / wheel lines per run: enough to read a tour, not a flood

// One decision, one line (an info line: the tour and the owner read them); a run stops logging after kMaxLogLines of them.
#define MM_LOG(...)                     \
    do {                                \
        if (g_state.logged < kMaxLogLines) { \
            ++g_state.logged;           \
            MC_INFO(__VA_ARGS__);       \
        }                               \
    } while (0)

// ── the clip tree ───────────────────────────────────────────────────────

uint8_t Alpha(const uint8_t* b, uint32_t inst) { return b[inst + kInstAlpha]; }

// The local matrix of an instance as the next draw will have it: a b c d as stored; the translation is base + property when a rebuild is waiting (sub_825F8C70).
mm::Affine LocalOf(const uint8_t* b, uint32_t inst) {
    mm::Affine m;
    m.a = ReadGuestF32(b, inst + kInstMatrix + 0);
    m.b = ReadGuestF32(b, inst + kInstMatrix + 4);
    m.c = ReadGuestF32(b, inst + kInstMatrix + 8);
    m.d = ReadGuestF32(b, inst + kInstMatrix + 12);
    m.tx = ReadGuestF32(b, inst + kInstMatrix + 16);
    m.ty = ReadGuestF32(b, inst + kInstMatrix + 20);
    if (b[inst + kInstDirty]) {
        m.tx = ReadGuestF32(b, inst + kInstBaseX) + ReadGuestF32(b, inst + kInstPropX);
        m.ty = ReadGuestF32(b, inst + kInstBaseY) + ReadGuestF32(b, inst + kInstPropY);
    }
    return m;
}

// The instance's matrix to the stage: its own, then each parent's up to the root. False when the chain does not end (a loop or a broken pointer) or holds a hidden instance.
bool ToStage(const uint8_t* b, uint32_t inst, mm::Affine* out, bool* visible) {
    mm::Affine m = LocalOf(b, inst);
    bool shown = Alpha(b, inst) != 0;
    uint32_t cur = R32(b, inst + kInstParent);
    int guard = 0;
    while (cur) {
        if (!Plausible(cur) || ++guard > 16) return false;
        m = mm::Then(m, LocalOf(b, cur));
        shown = shown && Alpha(b, cur) != 0;
        cur = R32(b, cur + kInstParent);
    }
    *out = m;
    *visible = shown;
    return true;
}

// The right-most start of anything inside a slot, in the slot's own x (its children and theirs; the value's arrow is one of them).
float ChildRight(const uint8_t* b, uint32_t slot) {
    float right = 0.0f;
    int guard = 0;
    for (uint32_t c = R32(b, slot + kInstFirstChild); Plausible(c) && guard < 64; c = R32(b, c + kInstNext), ++guard) {
        if (!Alpha(b, c)) continue;
        const mm::Affine lc = LocalOf(b, c);
        if (std::isfinite(lc.tx)) right = std::max(right, lc.tx);
        int inner = 0;
        for (uint32_t g = R32(b, c + kInstFirstChild); Plausible(g) && inner < 32; g = R32(b, g + kInstNext), ++inner) {
            if (!Alpha(b, g)) continue;
            const mm::Affine lg = LocalOf(b, g);
            if (std::isfinite(lg.tx) && std::isfinite(lc.tx)) right = std::max(right, lc.tx + lg.tx);
        }
    }
    return right;
}

void DumpTree(const uint8_t* b, uint32_t inst, int depth, int* lines) {
    if (!Plausible(inst) || depth > 4 || *lines > 160) return;
    int guard = 0;
    for (uint32_t c = R32(b, inst + kInstFirstChild); Plausible(c) && guard < 80; c = R32(b, c + kInstNext), ++guard) {
        const uint32_t np = R32(b, c + kInstName);
        const char* name = Plausible(np) ? reinterpret_cast<const char*>(b + np) : "";
        char shown[40] = {};
        for (int i = 0; i < 39 && name[i] >= 0x20 && name[i] < 0x7F; ++i) shown[i] = name[i];
        mm::Affine st;
        bool vis = false;
        const bool ok = ToStage(b, c, &st, &vis);
        MC_INFO("[menu-mouse] tree {:>{}}{} @0x{:08X} alpha {} stage ({:.1f}, {:.1f}){}", "", depth * 2, shown[0] ? shown : "(unnamed)", c, int(Alpha(b, c)),
                ok ? st.tx : 0.0f, ok ? st.ty : 0.0f, vis ? "" : " hidden");
        ++*lines;
        DumpTree(b, c, depth + 1, lines);
    }
}

// The clip that holds the rows: root.screen.menu (the pause movie's, ui_palette.cpp PauseMenuClip), else root.menu, else root.screen.
uint32_t MenuClipOf(const uint8_t* b, const larecomp::swf::Movie& mv) {
    const uint32_t screen = larecomp::swf::FindChild(b, mv.root, "screen");
    if (screen) {
        if (const uint32_t menu = larecomp::swf::FindChild(b, screen, "menu")) return menu;
    }
    if (const uint32_t menu = larecomp::swf::FindChild(b, mv.root, "menu")) return menu;
    return screen;
}

// The slots of the movie's menu as the layout wants them. `scroll` is added to the slot's number to give the list's row. Returns the number of slots found (shown or not).
int ReadSlots(const uint8_t* b, const char* movie_name, int scroll, mm::SlotIn* out, bool* movie_up) {
    *movie_up = false;
    larecomp::swf::Movie mv;
    if (!larecomp::swf::FindMovie(b, movie_name, mv) || !mv.enabled) return 0;
    *movie_up = true;
    const uint32_t menu = MenuClipOf(b, mv);
    if (!menu) return 0;
    const uint32_t slots_a = larecomp::swf::FindChild(b, menu, "slotsA");
    if (!slots_a) return 0;
    int found = 0;
    for (int i = 0; i < kMaxSlots; ++i) {
        char name[8];
        std::snprintf(name, sizeof(name), "slot%d", i);
        const uint32_t slot = larecomp::swf::FindChild(b, slots_a, name);
        if (!slot) continue;
        mm::SlotIn s;
        s.row = i + scroll;
        bool vis = false;
        if (!ToStage(b, slot, &s.to_stage, &vis)) continue;
        s.visible = vis;
        s.child_right = vis ? ChildRight(b, slot) : 0.0f;
        out[found++] = s;
    }
    return found;
}

// ── reading the menu ────────────────────────────────────────────────────

bool IsValueRow(const uint8_t* b, uint32_t list, int row) {
    const uint32_t rows = R32(b, list + kListRows);
    if (!HeapLike(rows) || row < 0 || row >= 64) return false;
    const uint32_t obj = R32(b, rows + 4u * uint32_t(row));
    if (!HeapLike(obj)) return false;
    const uint32_t vt = R32(b, obj);
    for (uint32_t v : kValueRowVtables)
        if (vt == v) return true;
    return false;
}

// A live mcListViewFixed: the vtable has the select slot, the counts make sense.
bool ReadList(const uint8_t* b, uint32_t list, int* selected, int* count, int* scroll) {
    if (!HeapLike(list) || !VSlot(b, list, kVfSelectRow)) return false;
    const int n = int(R16(b, list + kListCount));
    const int sel = int(int32_t(R32(b, list + kListSelect)));
    if (n < 1 || n > 64 || sel < -1 || sel >= n) return false;
    *count = n;
    *selected = sel;
    *scroll = R32(b, list + kListScrolls) ? int(int32_t(R32(b, list + kListScroll))) : 0;
    return *scroll >= 0 && *scroll < n;
}

// A live UIMenu of the game's own class. The highlighted row comes from its vfunc; the rows are counted through "the child shown on row N" until it names none.
bool ReadUiMenu(const uint8_t* b, uint32_t menu, int* selected, int* count) {
    if (!HeapLike(menu) || R32(b, menu) != kUiMenuVtable) return false;
    const uint32_t sel_fn = VSlot(b, menu, kUiMenuSelectedVfunc);
    const uint32_t child_fn = VSlot(b, menu, kUiMenuChildAtVfunc);
    if (!sel_fn || !child_fn) return false;
    uint32_t sel = 0;
    if (!CallAt(sel_fn, &sel, menu)) return false;
    int n = 0;
    for (; n < kMaxMenuRows; ++n) {
        uint32_t child = 0;
        if (!CallAt(child_fn, &child, menu, uint32_t(n)) || child == 0) break;
    }
    *selected = int(int32_t(sel));
    *count = n;
    return n >= 1 && n < kMaxMenuRows && *selected >= -1 && *selected < n;
}

// Reads whatever menu is up. Always fills `view.movie`; `view.kind` is None when nothing readable is there (a fallback menu).
//   PAUSEMOVIE        the list the game rendered last (the pause pages and every settings page are lists in this movie; no other movie holds one);
//   any other movie   the UIMenu FlashNavigator filled last, and only when the movie shows slots at all (the clip tree is read first: no call is made into a menu object for a
//                     screen that has no rows to point at).
void ReadMenu(const uint8_t* b, MenuView& view) {
    view = MenuView{};
    larecomp::keyboard::CurrentMovieName(view.movie, sizeof(view.movie));
    if (!view.movie[0]) return;

    const bool pause_movie = std::strcmp(view.movie, "PAUSEMOVIE") == 0;
    int selected = -1, count = 0, scroll = 0;
    uint32_t obj = 0;
    if (pause_movie) {
        obj = PauseMenuLastList();
        if (!obj || !ReadList(b, obj, &selected, &count, &scroll)) return;
        view.kind = Kind::List;
    } else {
        obj = PauseMenuLastUiMenu();
        if (!obj || obj == g_state.gave_up_obj) return;
        view.kind = Kind::UiMenu;
    }
    view.obj = obj;

    mm::SlotIn slots[kMaxSlots];
    bool movie_up = false;
    const int found = ReadSlots(b, view.movie, scroll, slots, &movie_up);
    if (!movie_up || found == 0) {
        view.kind = Kind::None;
        return;
    }
    view.layout = mm::BuildLayout(slots, found, float(REXCVAR_GET(menu_mouse_row_width)), float(REXCVAR_GET(menu_mouse_row_offset)));
    if (view.kind == Kind::UiMenu && view.layout.n > 0) {
        if (!ReadUiMenu(b, obj, &selected, &count)) {
            view.kind = Kind::None;
            return;
        }
    }
    view.selected = selected;
    view.count = count;
    // Every row the layout names has to exist in the list.
    bool inside = true;
    for (int i = 0; i < view.layout.n; ++i) inside = inside && view.layout.rows[i].row < count;
    view.rows_known = inside && mm::Plausible(view.layout);
}

// ── pressing ────────────────────────────────────────────────────────────

larecomp::kbm::MenuKey KeyOf(mm::Act act) {
    switch (act) {
        case mm::Act::Back: return larecomp::kbm::MenuKey::Back;
        case mm::Act::Up: return larecomp::kbm::MenuKey::Up;
        case mm::Act::Down: return larecomp::kbm::MenuKey::Down;
        case mm::Act::Left: return larecomp::kbm::MenuKey::Left;
        case mm::Act::Right: return larecomp::kbm::MenuKey::Right;
        default: return larecomp::kbm::MenuKey::Accept;
    }
}

const char* ActName(mm::Act act) {
    switch (act) {
        case mm::Act::Accept: return "ACCEPT";
        case mm::Act::Back: return "BACK";
        case mm::Act::Up: return "UP";
        case mm::Act::Down: return "DOWN";
        case mm::Act::Left: return "LEFT";
        case mm::Act::Right: return "RIGHT";
        default: return "none";
    }
}

void Queue(mm::Act act) {
    if (act == mm::Act::None || g_state.queued >= int(sizeof(g_state.queue) / sizeof(g_state.queue[0]))) return;
    g_state.queue[g_state.queued++] = act;
}

// One queued press per ready tick: the next one waits a poll, so two presses stay two.
void DrainQueue() {
    if (g_state.queued == 0 || !g_state.pacer.Ready()) return;
    const mm::Act act = g_state.queue[0];
    for (int i = 1; i < g_state.queued; ++i) g_state.queue[i - 1] = g_state.queue[i];
    --g_state.queued;
    larecomp::keyboard::PressMenuKey(KeyOf(act));
    g_state.pacer.Fired();
}

void ResetWork() {
    g_state.queued = 0;
    g_state.pending = mm::PendingClick{};
    g_state.walk_target = -1;
    g_state.walk.Reset();
    g_state.wheel.Clear();
    g_state.pacer.Reset();
}

// ── the tick ────────────────────────────────────────────────────────────

mm::HudRect HudNow(rex::ui::Window* window) {
    const uint32_t w = window->GetActualPhysicalWidth();
    const uint32_t h = window->GetActualPhysicalHeight();
    return mm::HudOf(mc::ultrawide::FrameFor(w, h), mc::ultrawide::UiWidthFraction(), mc::ultrawide::UiHeightFraction());
}

void ReportMenu(const MenuView& view, const mm::HudRect& hud) {
    State& s = g_state;
    if (view.obj == s.reported_obj && int(view.kind) == s.reported_kind && std::strcmp(view.movie, s.reported_movie) == 0) return;
    s.reported_obj = view.obj;
    s.reported_kind = int(view.kind);
    std::snprintf(s.reported_movie, sizeof(s.reported_movie), "%s", view.movie);
    static const char* const kKind[] = {"unreadable", "list", "UIMenu"};
    if (view.kind == Kind::None) {
        MM_LOG("[menu-mouse] menu {}: FALLBACK (no list or UIMenu to read; hover off, click = ACCEPT)", view.movie);
        return;
    }
    if (!view.rows_known) {
        MM_LOG("[menu-mouse] menu {}: FALLBACK ({} 0x{:08X} {} rows, highlight {}; the clip tree gave {} shown slot(s), pitch {:.1f}, plausible {}; hover off, click = ACCEPT)", view.movie,
            kKind[int(view.kind)], view.obj, view.count, view.selected, view.layout.n, view.layout.pitch, mm::Plausible(view.layout) ? "yes" : "no");
        return;
    }
    float px0, py0, px1, py1;
    mm::StageToWindow(hud, view.layout.rows[0].x0, view.layout.rows[0].y0, &px0, &py0);
    mm::StageToWindow(hud, view.layout.rows[0].x1, view.layout.rows[0].y1, &px1, &py1);
    MM_LOG("[menu-mouse] menu {}: {} 0x{:08X}, {} rows, highlight {}, {} on screen (rows {}..{}), pitch {:.1f}, width {:.1f}; first row stage ({:.1f}, {:.1f}) = window ({:.0f}, {:.0f})-({:.0f}, {:.0f})", view.movie,
        kKind[int(view.kind)], view.obj, view.count, view.selected, view.layout.n, view.layout.rows[0].row, view.layout.rows[view.layout.n - 1].row, view.layout.pitch, view.layout.width,
        view.layout.rows[0].x0, view.layout.rows[0].y0, px0, py0, px1, py1);
}

// Moves the highlight of the list to `row`: the list's own select slot, the scroll sound when it changes. False when the call could not be made.
bool SelectListRow(const uint8_t* b, uint32_t list, int row) {
    const uint32_t fn = VSlot(b, list, kVfSelectRow);
    if (!fn) return false;
    uint32_t ignored = 0;
    if (!CallAt(fn, &ignored, list, uint32_t(row))) return false;
    return true;
}

void Tick(uint8_t* b) {
    State& s = g_state;
    ++s.tick;
    s.pacer.Step();

    rex::ui::Window* window = GetWindow();
    if (!window) return;
    EnsureListener(window);

    // The events since the last tick.
    const uint64_t move_seq = g_move_seq.load(std::memory_order_relaxed);
    const bool moved = move_seq != s.last_move_seq;
    s.last_move_seq = move_seq;
    const uint32_t lefts = g_left_downs.load(std::memory_order_relaxed);
    const uint32_t rights = g_right_downs.load(std::memory_order_relaxed);
    const uint32_t backs = g_back_downs.load(std::memory_order_relaxed);
    const int left_clicks = int(lefts - s.last_left);
    const int right_clicks = int(rights - s.last_right) + int(backs - s.last_back);
    s.last_left = lefts;
    s.last_right = rights;
    s.last_back = backs;
    const int wheel_units = g_wheel_units.exchange(0, std::memory_order_relaxed);

    const bool setting = REXCVAR_GET(menu_mouse);
    const bool kbm_active = larecomp::input::ActiveInput() == larecomp::input::InputDevice::KeyboardMouse;
    const bool over_overlay = mcla::ui::SettingsOpen() || (ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureMouse);
    // A menu screen of the game's: the game is entered, a menu or a movie that is not the road's is up; not the phone, not the full map (it has its own mouse).
    const bool menu_up = larecomp::keyboard::GameEntered() && larecomp::keyboard::MenuScreenUp() && !larecomp::keyboard::PhoneScreenUp() && !MapMouseWantsPointer();

    g_wants_pointer.store(mm::WantsPointer(setting, menu_up, kbm_active, window->HasFocus()), std::memory_order_relaxed);

    if (!setting || !menu_up || over_overlay) {
        // Nothing of this is the menu's: what the mouse did meanwhile is not saved up for the next menu.
        if (s.queued || s.pending.active || s.walk_target >= 0 || s.wheel.Pending() != 0) ResetWork();
        s.reported_obj = 0;
        s.reported_kind = -1;
        s.reported_movie[0] = 0;
        s.gave_up_obj = 0;
        return;
    }

    s.wheel.Add(wheel_units);
    const bool dump = REXCVAR_GET(menu_mouse_dump);
    if (!dump) s.dump_armed = true;
    const bool work = moved || left_clicks || right_clicks || s.wheel.HasStep() || s.pending.active || s.walk_target >= 0 || s.queued || (dump && s.dump_armed);
    if (!work) return;

    MenuView view;
    ReadMenu(b, view);
    const mm::HudRect hud = HudNow(window);
    ReportMenu(view, hud);

    if (dump && s.dump_armed) {
        s.dump_armed = false;
        larecomp::swf::Movie mv;
        if (view.movie[0] && larecomp::swf::FindMovie(b, view.movie, mv)) {
            MC_INFO("[menu-mouse] tree of {} (root 0x{:08X}, enabled {}):", view.movie, mv.root, mv.enabled);
            int lines = 0;
            DumpTree(b, mv.root, 0, &lines);
        }
    }

    int32_t mx, my;
    Unpack(g_pos.load(std::memory_order_relaxed), &mx, &my);
    float sx = 0.0f, sy = 0.0f;
    mm::WindowToStage(hud, float(mx), float(my), &sx, &sy);

    // Right button and back button: BACK, whatever is under them.
    for (int i = 0; i < right_clicks; ++i) {
        Queue(mm::Act::Back);
        MM_LOG("[menu-mouse] {} right / back button: BACK", view.movie);
    }

    // The wheel: a step up or down per wheel notch, one press at a time.
    if (s.queued == 0) {
        const int step = s.wheel.TakeOne();
        if (step != 0) {
            Queue(step > 0 ? mm::Act::Up : mm::Act::Down);
            MM_LOG("[menu-mouse] {} wheel {}", view.movie, step > 0 ? "up: UP" : "down: DOWN");
        }
    }

    // A left click.
    for (int i = 0; i < left_clicks; ++i) {
        int32_t cx, cy;
        Unpack(g_click_pos.load(std::memory_order_relaxed), &cx, &cy);
        float csx = 0.0f, csy = 0.0f;
        mm::WindowToStage(hud, float(cx), float(cy), &csx, &csy);
        const bool rows_known = view.rows_known && view.selected >= -1;
        const int hit = rows_known ? mm::HitRow(view.layout, csx, csy) : -1;
        mm::Zone zone = mm::Zone::Label;
        bool value_row = false;
        if (rows_known && hit >= 0) {
            value_row = view.kind == Kind::List && IsValueRow(b, view.obj, hit);
            if (const mm::RowBox* box = mm::BoxOfRow(view.layout, hit)) zone = mm::ZoneOf(*box, csx, value_row);
        }
        const mm::ClickPlan plan = mm::PlanLeftClick(rows_known, hit, zone, value_row);
        MM_LOG("[menu-mouse] {} left click at window ({}, {}) = stage ({:.1f}, {:.1f}): row {}{} -> {}{}", view.movie, cx, cy, csx, csy, hit, value_row ? " (a value row)" : "", ActName(plan.act),
            plan.row >= 0 && plan.row != view.selected ? " after moving the highlight there" : (rows_known ? "" : " (fallback: on the highlighted row)"));
        if (plan.act == mm::Act::None) continue;
        if (plan.row < 0 || plan.row == view.selected) {
            Queue(plan.act);
        } else {
            s.pending = mm::StartPending(plan);
            // The highlight goes there at once for a list; a UIMenu is walked there below.
            if (view.kind == Kind::List && SelectListRow(b, view.obj, plan.row)) PlayScrollSound(b, plan.row < view.selected);
            else {
                s.walk_target = plan.row;
                s.walk_obj = view.obj;
            }
        }
    }

    // Hover. A pointer that moved off every row lets go of a walk that was going to the row it had been over.
    if (moved && s.walk_target >= 0 && !s.pending.active && (!view.rows_known || mm::HitRow(view.layout, sx, sy) < 0)) {
        s.walk_target = -1;
        s.walk.Reset();
    }
    if (!s.pending.active) {
        const int target = mm::HoverTarget(setting, moved, kbm_active, view.rows_known, view.rows_known ? mm::HitRow(view.layout, sx, sy) : -1, view.selected);
        if (target >= 0) {
            if (view.kind == Kind::List) {
                if (SelectListRow(b, view.obj, target)) {
                    PlayScrollSound(b, target < view.selected);
                    MM_LOG("[menu-mouse] {} hover: row {} (was {})", view.movie, target, view.selected);
                }
            } else if (view.kind == Kind::UiMenu) {
                s.walk_target = target;
                s.walk_obj = view.obj;
                MM_LOG("[menu-mouse] {} hover: walking {} -> row {}", view.movie, view.selected, target);
            }
        }
    }

    // A click waiting for the highlight to arrive.
    if (s.pending.active) {
        bool done = false;
        const mm::Act act = mm::StepPending(s.pending, view.selected, &done);
        if (act != mm::Act::None) Queue(act);
        if (done && act == mm::Act::None) {
            MM_LOG("[menu-mouse] {} the highlight never reached row {}: click dropped", view.movie, s.pending.row);
            s.walk_target = -1;
        }
    }

    // A UIMenu's highlight is moved with the arrows, one press per ready tick, watched for a menu that does not move.
    if (s.walk_target >= 0) {
        if (view.kind != Kind::UiMenu || view.obj != s.walk_obj || view.selected == s.walk_target) {
            s.walk_target = -1;
            s.walk.Reset();
        } else if (s.queued == 0 && s.pacer.Ready()) {
            s.walk.Pressed(view.selected);
            if (s.walk.Stalled()) {
                MM_LOG("[menu-mouse] {} UIMenu 0x{:08X}: the arrows did not move the highlight (8 presses in a row): hover off for this menu", view.movie, view.obj);
                s.gave_up_obj = view.obj;
                s.walk_target = -1;
                s.pending = mm::PendingClick{};
                s.walk.Reset();
            } else {
                Queue(mm::WalkStep(view.selected, s.walk_target));
            }
        }
    }

    DrainQueue();
}

}  // namespace

bool MenuMouseWantsPointer() { return g_wants_pointer.load(std::memory_order_relaxed); }

void MenuMouseTick(uint8_t* base) {
    if (!base) return;
    Tick(base);
}

}  // namespace larecomp

#else  // REXGLUE_HAS_XEO3_TARGET

#include "menu_mouse.h"

namespace larecomp {
void MenuMouseTick(uint8_t*) {}
bool MenuMouseWantsPointer() { return false; }
}  // namespace larecomp

#endif  // REXGLUE_HAS_XEO3_TARGET
