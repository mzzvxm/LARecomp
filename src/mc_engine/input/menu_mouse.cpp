#ifndef REXGLUE_HAS_XEO3_TARGET
#include "menu_mouse.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
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
    "Debug: log what the mouse sees of the menu that is up, once, and again each time this is set to true: the movie (its frame, the display state, the scale onto the stage), the candidates (the "
    "list and the UIMenu the game rendered last, their fields and counters), the clip tree's outline and every clip that holds slot0..slotN with each slot's local, world and stage "
    "place. For a menu that falls back: this is how its rows are found.")
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(menu_mouse_dump_new, false, "MCLA/Input",
    "Debug: the same dump (menu_mouse_dump) once for every new menu the mouse meets (a new movie, list or UIMenu, or one that is read differently than before), fallbacks included, up to "
    "24 a run. For the tours: set it before the first menu opens.")
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
constexpr uint32_t kInstWorld = 32;  // the world matrix the draw of the last frame left: a b c d tx ty (sub_825FB348; ui_smooth.cpp's header has the layout)
constexpr uint32_t kInstBaseX = 92;
constexpr uint32_t kInstBaseY = 96;
constexpr uint32_t kInstPropX = 116;
constexpr uint32_t kInstPropY = 120;
constexpr uint32_t kInstParent = 184;
constexpr uint32_t kInstFirstChild = 188;
constexpr uint32_t kInstNext = 192;
constexpr uint32_t kInstName = 196;

constexpr int kMaxSlots = 24;  // slot0 .. slot23 (ui_palette.cpp RowSlot)

// How a movie is put on the screen (sub_825EE970 -> sub_825EE428): the swfFILE's frame (+36 width, +44 height, floats) onto the game's display state, a pointer at 0x82839704 to a struct with
// the width (+688) and height (+692) as ints and their factors (+648, +652) as floats: scale = int(width x k) / frame width.
constexpr uint32_t kFileFrameW = 36;
constexpr uint32_t kFileFrameH = 44;
constexpr uint32_t kDisplayPtrCell = 0x82839704;
constexpr uint32_t kDisplayW = 688;
constexpr uint32_t kDisplayH = 692;
constexpr uint32_t kDisplayKx = 648;
constexpr uint32_t kDisplayKy = 652;
// mcListViewFixed's other fields (sub_82631F88 / sub_82631FD0): the rows in a table (+192, its vfunc +4 = the count) or a vhsm state (+196, its vfunc +324 = the count; the pause screen's own
// pages) instead of the row objects at +176; the rows that fit on screen (+188).
constexpr uint32_t kObjFlags = 16;
constexpr uint32_t kObjActive = 0x20;
constexpr uint32_t kVfIsActive = 60;  // the question the renderer asks first (it draws nothing for an inactive list)
constexpr uint32_t kListMaxRows = 188;
constexpr uint32_t kListTable = 192;
constexpr uint32_t kListAdapter = 196;
constexpr uint32_t kVfTableCount = 4;
constexpr uint32_t kVfAdapterCount = 324;

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
    bool model = false;      // the rows are the model's (mm::ModelLayout), not the clip tree's: the movie has no slot clips
    char movie[48] = {};
    // What the reading found, for the log (ReportMenu) and the dump.
    mm::StageScale scale;
    bool scale_known = false;
    char clip[96] = {};      // the clip that holds the slots, as a path
    int clip_slots = 0;
    int clip_shown = 0;
    int clip_world = 0;
    char why[640] = {};      // what each candidate lacked
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
    int reported_state = -1;           // how it was read then (None / clip tree / model): a change is reported again
    char reported_movie[48] = {};
    uint32_t reports_of_obj = 0;       // how many lines the same list / UIMenu has had (a menu read one way, then another, then back must not flood)
    bool dump_armed = true;
    int logged = 0;
    int dumps = 0;
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

// The instance's name as text, "(unnamed)" for none.
void NameOf(const uint8_t* b, uint32_t inst, char* out, size_t cap) {
    out[0] = 0;
    const uint32_t np = R32(b, inst + kInstName);
    size_t i = 0;
    if (Plausible(np)) {
        const char* name = reinterpret_cast<const char*>(b + np);
        for (; i + 1 < cap && name[i] >= 0x20 && name[i] < 0x7F; ++i) out[i] = name[i];
    }
    out[i] = 0;
    if (i == 0) std::snprintf(out, cap, "(unnamed)");
}

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

// The matrix the draw of the last frame left in the instance (+32: a b c d tx ty = local x the parent's world, sub_825FB348): the instance's place in the movie's units, exactly as drawn.
// Not usable (false) for an instance that was never drawn (zeros).
bool WorldOf(const uint8_t* b, uint32_t inst, mm::Affine* out) {
    out->a = ReadGuestF32(b, inst + kInstWorld + 0);
    out->b = ReadGuestF32(b, inst + kInstWorld + 4);
    out->c = ReadGuestF32(b, inst + kInstWorld + 8);
    out->d = ReadGuestF32(b, inst + kInstWorld + 12);
    out->tx = ReadGuestF32(b, inst + kInstWorld + 16);
    out->ty = ReadGuestF32(b, inst + kInstWorld + 20);
    return mm::WorldUsable(*out);
}

// The instance's local matrices folded up to the root, and whether it and every instance above it has alpha. False when the chain does not end (a loop or a broken pointer).
bool FoldToRoot(const uint8_t* b, uint32_t inst, mm::Affine* out, bool* visible) {
    mm::Affine m = LocalOf(b, inst);
    bool shown = Alpha(b, inst) != 0;
    uint32_t cur = R32(b, inst + kInstParent);
    int guard = 0;
    while (cur) {
        if (!Plausible(cur) || ++guard > 24) return false;
        m = mm::Then(m, LocalOf(b, cur));
        shown = shown && Alpha(b, cur) != 0;
        cur = R32(b, cur + kInstParent);
    }
    *out = m;
    *visible = shown;
    return true;
}

// The instance's matrix to the stage: the world matrix the draw left in it (else the fold of the local ones), through the movie's scale onto the screen. `world` says which.
bool PlaceOf(const uint8_t* b, uint32_t inst, const mm::StageScale& scale, mm::Affine* out, bool* visible, bool* world) {
    mm::Affine fold, w;
    bool shown = false;
    const bool fold_ok = FoldToRoot(b, inst, &fold, &shown);
    const bool world_ok = WorldOf(b, inst, &w);
    const mm::MatrixSource src = mm::ChooseMatrix(world_ok, fold_ok);
    if (src == mm::MatrixSource::None) return false;
    if (!fold_ok) {
        // The chain did not end, so the alpha along it is not known either: only this instance's own.
        shown = Alpha(b, inst) != 0;
    }
    *out = mm::Scaled(src == mm::MatrixSource::World ? w : fold, scale);
    *visible = shown;
    *world = src == mm::MatrixSource::World;
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

// ── where the rows are: the clip that holds slot0 .. slotN ───────────────

struct ClipCand {
    uint32_t inst = 0;
    int depth = 0;
    int slots = 0;  // slot0 .. slot23 clips it has
    int shown = 0;  // of them with alpha
};

bool SlotName(int i, char* name, size_t cap) {
    if (i < 0 || i >= kMaxSlots) return false;
    std::snprintf(name, cap, "slot%d", i);
    return true;
}

ClipCand CountSlots(const uint8_t* b, uint32_t clip) {
    ClipCand c;
    c.inst = clip;
    for (int i = 0; i < kMaxSlots; ++i) {
        char name[8];
        SlotName(i, name, sizeof(name));
        const uint32_t slot = larecomp::swf::FindChild(b, clip, name);
        if (!slot) continue;
        ++c.slots;
        if (Alpha(b, slot)) ++c.shown;
    }
    return c;
}

// The names from the root down to `inst` joined with dots ("screen.menu.slotsA"); the root itself is left out.
void PathOf(const uint8_t* b, uint32_t inst, char* out, size_t cap) {
    uint32_t chain[16];
    int n = 0;
    for (uint32_t cur = inst; cur && Plausible(cur) && n < 16; cur = R32(b, cur + kInstParent)) chain[n++] = cur;
    out[0] = 0;
    size_t used = 0;
    for (int i = n - 2; i >= 0; --i) {  // chain[n - 1] is the root
        char nm[40];
        NameOf(b, chain[i], nm, sizeof(nm));
        const int w = std::snprintf(out + used, cap - used, "%s%s", used ? "." : "", nm);
        if (w < 0 || size_t(w) >= cap - used) break;
        used += size_t(w);
    }
    if (n == 1) std::snprintf(out, cap, "(root)");
}

// Every clip of the movie (breadth first, a few levels down) that has a child called slot0: where a list's rows may be. The pause movie's is root.screen.menu.slotsA; the garage's may be
// another (a page with tabs has its own): they are all found and every one is tried against the list or UIMenu.
int FindSlotsClips(const uint8_t* b, uint32_t root, ClipCand* out, int cap) {
    int n = 0;
    constexpr int kQueue = 1536;
    constexpr int kMaxDepth = 6;
    static thread_local uint32_t queue[kQueue];
    static thread_local int depth[kQueue];
    int head = 0, tail = 0;
    queue[tail] = root;
    depth[tail++] = 0;
    while (head < tail && n < cap) {
        const uint32_t cur = queue[head];
        const int d = depth[head++];
        if (larecomp::swf::FindChild(b, cur, "slot0")) {
            out[n] = CountSlots(b, cur);
            out[n].depth = d;
            ++n;
        }
        if (d >= kMaxDepth) continue;
        int guard = 0;
        for (uint32_t c = R32(b, cur + kInstFirstChild); Plausible(c) && guard < 256 && tail < kQueue; c = R32(b, c + kInstNext), ++guard) {
            queue[tail] = c;
            depth[tail++] = d + 1;
        }
    }
    return n;
}

// The candidates in the order to try them: the most shown slots first (a clip whose slots are all hidden is the page that went away), then the one called slotsA, then the most slots, the
// shallowest. Returns how many are worth trying (any with a slot), `order` holds their indexes.
int RankClips(const uint8_t* b, const ClipCand* c, int n, int* order) {
    int score[8];
    int m = 0;
    for (int i = 0; i < n && i < 8; ++i) {
        char nm[40];
        NameOf(b, c[i].inst, nm, sizeof(nm));
        score[i] = c[i].shown * 100 + (std::strcmp(nm, "slotsA") == 0 ? 50 : 0) + c[i].slots - c[i].depth;
        order[m++] = i;
    }
    for (int i = 1; i < m; ++i) {  // insertion sort, best first
        const int k = order[i];
        int j = i - 1;
        while (j >= 0 && score[order[j]] < score[k]) {
            order[j + 1] = order[j];
            --j;
        }
        order[j + 1] = k;
    }
    return m;
}

// ── the movie's scale onto the screen ───────────────────────────────────

struct Display {
    float w = 0.0f, h = 0.0f, kx = 0.0f, ky = 0.0f;  // the game's display state (the width and height the movie is drawn into, and its two factors)
    float frame_w = 0.0f, frame_h = 0.0f;            // the movie's frame
};

mm::StageScale ScaleOf(const uint8_t* b, const larecomp::swf::Movie& mv, Display* d) {
    *d = Display{};
    const uint32_t cell = R32(b, kDisplayPtrCell);
    if (Plausible(cell)) {
        d->w = float(int32_t(R32(b, cell + kDisplayW)));
        d->h = float(int32_t(R32(b, cell + kDisplayH)));
        d->kx = ReadGuestF32(b, cell + kDisplayKx);
        d->ky = ReadGuestF32(b, cell + kDisplayKy);
    }
    if (Plausible(mv.file)) {
        d->frame_w = ReadGuestF32(b, mv.file + kFileFrameW);
        d->frame_h = ReadGuestF32(b, mv.file + kFileFrameH);
    }
    return mm::FrameScale(d->w, d->kx, d->h, d->ky, d->frame_w, d->frame_h);
}

// ── the slots ───────────────────────────────────────────────────────────

struct Slots {
    uint32_t clip = 0;       // the clip that holds them
    char path[96] = {};
    int found = 0;           // the slot clips read (shown or not)
    int shown = 0;           // of them visible
    int world = 0;           // shown ones placed by the world matrix of the last draw (the rest by the fold)
    mm::SlotIn in[kMaxSlots];  // .row is the slot's number: the caller adds the list's scroll
};

void ReadSlotsOf(const uint8_t* b, uint32_t clip, const mm::StageScale& scale, Slots* out) {
    *out = Slots{};
    out->clip = clip;
    PathOf(b, clip, out->path, sizeof(out->path));
    for (int i = 0; i < kMaxSlots; ++i) {
        char name[8];
        SlotName(i, name, sizeof(name));
        const uint32_t slot = larecomp::swf::FindChild(b, clip, name);
        if (!slot) continue;
        mm::SlotIn s;
        s.row = i;
        bool vis = false, world = false;
        if (!PlaceOf(b, slot, scale, &s.to_stage, &vis, &world)) continue;
        s.visible = vis;
        s.child_right = vis ? ChildRight(b, slot) * std::fabs(s.to_stage.a) : 0.0f;
        out->in[out->found++] = s;
        if (vis) {
            ++out->shown;
            if (world) ++out->world;
        }
    }
}

constexpr int kMaxClips = 3;  // the slot-holding clips of a movie that are tried (best ranked first)

// The movie's slot-holding clips, best first, with their slots read. Returns how many.
int ReadSlotClips(const uint8_t* b, const larecomp::swf::Movie& mv, const mm::StageScale& scale, Slots* out) {
    ClipCand cands[8];
    const int nc = FindSlotsClips(b, mv.root, cands, 8);
    int order[8];
    const int m = nc > 0 ? RankClips(b, cands, nc, order) : 0;
    int n = 0;
    for (int i = 0; i < m && n < kMaxClips; ++i) {
        ReadSlotsOf(b, cands[order[i]].inst, scale, &out[n]);
        if (out[n].found > 0) ++n;
    }
    return n;
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

struct ListRead {
    bool ok = false;
    int selected = -1;
    int count = 0;       // the rows the list has, from the game's own count (sub_82631F88: the adapter's, the table's or the +180 half word)
    int scroll = 0;
    bool scrolls = false;
    int max_rows = 0;    // the rows that fit (+188)
    char why[96] = {};
};

// The game's own row count of a list (sub_82631F88): the vhsm state's (+196: the pause screen's own pages), else the table's (+192), else the half word at +180 (our pages). The first
// run read only the half word, which is not the count for the pause screen's pages (CHECK: that is why the pause menu fell back).
int RowCountOf(const uint8_t* b, uint32_t list) {
    const uint32_t adapter = R32(b, list + kListAdapter);
    const uint32_t table = R32(b, list + kListTable);
    const mm::RowSource source = mm::RowSourceOf(adapter, table);
    if (source == mm::RowSource::Packed) return int(R16(b, list + kListCount));
    // The source's own count slot, called only if the object looks like one (a list that was freed or never filled must not be called through).
    const uint32_t src = source == mm::RowSource::Adapter ? adapter : table;
    const uint32_t fn = VSlot(b, src, source == mm::RowSource::Adapter ? kVfAdapterCount : kVfTableCount);
    uint32_t n = 0;
    if (!fn || !CallAt(fn, &n, src)) return -1;
    return int(int32_t(n));
}

// A live mcListViewFixed: the vtable has the select slot, the counts make sense.
bool ReadList(const uint8_t* b, uint32_t list, ListRead* out) {
    *out = ListRead{};
    if (!HeapLike(list)) {
        std::snprintf(out->why, sizeof(out->why), "not a heap address");
        return false;
    }
    if (!VSlot(b, list, kVfSelectRow)) {
        std::snprintf(out->why, sizeof(out->why), "no select slot (vtable 0x%08X)", R32(b, list));
        return false;
    }
    // The renderer draws only an active list (the flag at +16: sub_8221CDD0, vfunc 60): the pause screen has the lists of several tabs and only one of them is on screen.
    bool active = (R32(b, list + kObjFlags) & kObjActive) != 0;
    if (const uint32_t is_active = VSlot(b, list, kVfIsActive)) {
        uint32_t r = 0;
        if (CallAt(is_active, &r, list)) active = (r & 0xFF) != 0;
    }
    if (!active) {
        std::snprintf(out->why, sizeof(out->why), "not active (flags 0x%08X)", R32(b, list + kObjFlags));
        return false;
    }
    const int n = RowCountOf(b, list);
    const int sel = int(int32_t(R32(b, list + kListSelect)));
    const bool scrolls = R32(b, list + kListScrolls) != 0;
    const int scroll = scrolls ? int(int32_t(R32(b, list + kListScroll))) : 0;
    switch (mm::JudgeList(true, n, sel, scrolls, scroll)) {
        case mm::Verdict::BadCount:
            std::snprintf(out->why, sizeof(out->why), "row count %d (adapter 0x%08X, table 0x%08X, half word %d)", n, R32(b, list + kListAdapter), R32(b, list + kListTable), int(R16(b, list + kListCount)));
            return false;
        case mm::Verdict::BadHighlight: std::snprintf(out->why, sizeof(out->why), "highlight %d of %d rows", sel, n); return false;
        case mm::Verdict::BadScroll: std::snprintf(out->why, sizeof(out->why), "scroll %d of %d rows", scroll, n); return false;
        default: break;
    }
    out->count = n;
    out->selected = sel;
    out->scrolls = scrolls;
    out->scroll = scroll;
    out->max_rows = int(int32_t(R32(b, list + kListMaxRows)));
    out->ok = true;
    return true;
}

// A live UIMenu of the game's own class. The highlighted row comes from its vfunc; the rows are counted through "the child shown on row N" until it names none.
bool ReadUiMenu(const uint8_t* b, uint32_t menu, int* selected, int* count, char* why, size_t why_cap) {
    if (!HeapLike(menu)) {
        std::snprintf(why, why_cap, "not a heap address");
        return false;
    }
    if (R32(b, menu) != kUiMenuVtable) {
        std::snprintf(why, why_cap, "vtable 0x%08X is not the UIMenu class 0x%08X", R32(b, menu), kUiMenuVtable);
        return false;
    }
    const uint32_t sel_fn = VSlot(b, menu, kUiMenuSelectedVfunc);
    const uint32_t child_fn = VSlot(b, menu, kUiMenuChildAtVfunc);
    if (!sel_fn || !child_fn) {
        std::snprintf(why, why_cap, "no highlight / child slots");
        return false;
    }
    uint32_t sel = 0;
    if (!CallAt(sel_fn, &sel, menu)) {
        std::snprintf(why, why_cap, "the highlight slot cannot be called");
        return false;
    }
    int n = 0;
    for (; n < kMaxMenuRows; ++n) {
        uint32_t child = 0;
        if (!CallAt(child_fn, &child, menu, uint32_t(n)) || child == 0) break;
    }
    *selected = int(int32_t(sel));
    *count = n;
    switch (mm::JudgeUiMenu(n, *selected, kMaxMenuRows)) {
        case mm::Verdict::BadCount: std::snprintf(why, why_cap, "%d rows", n); return false;
        case mm::Verdict::BadHighlight: std::snprintf(why, why_cap, "highlight %d of %d rows", *selected, n); return false;
        default: return true;
    }
}

void Note(MenuView& v, const char* fmt, ...) {
    const size_t used = std::strlen(v.why);
    if (used + 2 >= sizeof(v.why)) return;
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(v.why + used, sizeof(v.why) - used, fmt, ap);
    va_end(ap);
}

// The rows the layout names, as "a..b" (for the log).
void RowRange(const mm::Layout& l, int* lo, int* hi) {
    *lo = l.n ? l.rows[0].row : -1;
    *hi = l.n ? l.rows[l.n - 1].row : -1;
}

// Reads whatever menu is up. Always fills `view.movie` and, when anything was rendered, `view.obj` (the first candidate); `view.kind` is None when nothing readable is there (a fallback menu)
// and `view.why` says what each candidate lacked.
//   candidates   PAUSEMOVIE: the list of the page of ours on top (exact), else the list the game rendered last (the pause screen's pages and every settings page are lists in this movie).
//                any other movie: the list and the UIMenu FlashNavigator filled, the later one first (the garage has both: its main menu is a UIMenu, its customization pages are lists).
//   a candidate  is taken when the clip tree shows rows the list draws (its window) or rows the UIMenu has, in a layout that passes Plausible.
//   the model    PAUSEMOVIE only, when the movie has no slot clips at all and the list reads: the rows are put where the pictures have them (mm::ModelLayout).
void ReadMenu(const uint8_t* b, MenuView& view) {
    view = MenuView{};
    larecomp::keyboard::CurrentMovieName(view.movie, sizeof(view.movie));
    if (!view.movie[0]) return;

    const bool pause_movie = std::strcmp(view.movie, "PAUSEMOVIE") == 0;
    uint32_t list = 0, ui = 0;
    if (pause_movie) {
        list = PauseMenuTopOurList();
        if (!list) list = PauseMenuLastList();
    } else {
        list = PauseMenuLastList();
        ui = PauseMenuLastUiMenu();
        if (ui && ui == g_state.gave_up_obj) {
            Note(view, "UIMenu 0x%08X was given up on (its arrows did not move the highlight); ", ui);
            ui = 0;
        }
    }
    struct Cand {
        Kind kind;
        uint32_t obj;
    } cands[2];
    int nc = 0;
    const bool list_first = mm::ListIsLater(PauseMenuLastListSeq(), PauseMenuLastUiMenuSeq());
    const auto add = [&](Kind k, uint32_t obj) {
        if (obj) cands[nc++] = Cand{k, obj};
    };
    if (list_first) {
        add(Kind::List, list);
        add(Kind::UiMenu, ui);
    } else {
        add(Kind::UiMenu, ui);
        add(Kind::List, list);
    }
    if (nc == 0) {
        Note(view, "no list or UIMenu was rendered yet");
        return;
    }
    view.obj = cands[0].obj;

    larecomp::swf::Movie mv;
    if (!larecomp::swf::FindMovie(b, view.movie, mv) || !mv.enabled) {
        Note(view, "the movie is not registered or not enabled");
        return;
    }
    Display display;
    const mm::StageScale scale = ScaleOf(b, mv, &display);
    view.scale = scale;
    view.scale_known = true;
    Slots clips[kMaxClips];
    const int nclips = ReadSlotClips(b, mv, scale, clips);
    if (nclips > 0) {
        Note(view, "clips:");
        for (int i = 0; i < nclips; ++i) Note(view, " %s (%d slots, %d shown)", clips[i].path, clips[i].found, clips[i].shown);
        Note(view, "; ");
    } else {
        Note(view, "no clip with slot0..slotN in the movie; ");
    }
    // The clip a layout was taken from, for the report.
    const auto take_clip = [&](const Slots& sl) {
        std::snprintf(view.clip, sizeof(view.clip), "%s", sl.path);
        view.clip_slots = sl.found;
        view.clip_shown = sl.shown;
        view.clip_world = sl.world;
    };

    ListRead model_list;
    uint32_t model_obj = 0;
    for (int i = 0; i < nc; ++i) {
        const Cand& c = cands[i];
        if (c.kind == Kind::List) {
            ListRead lr;
            if (!ReadList(b, c.obj, &lr)) {
                Note(view, "list 0x%08X: %s; ", c.obj, lr.why);
                continue;
            }
            int first = 0, end = 0;
            mm::ListWindow(lr.count, lr.scrolls, lr.scroll, lr.max_rows, &first, &end);
            if (!model_obj) {
                model_list = lr;
                model_obj = c.obj;
            }
            if (nclips == 0) {
                Note(view, "list 0x%08X reads (%d rows, highlight %d, window %d..%d) but there are no slot clips; ", c.obj, lr.count, lr.selected, first, end - 1);
                continue;
            }
            // Every slot clip against this list: the one whose shown rows are rows the list draws, in a layout of text rows.
            for (int ci = 0; ci < nclips; ++ci) {
                const Slots& sl = clips[ci];
                mm::SlotIn shifted[kMaxSlots];
                for (int k = 0; k < sl.found; ++k) {
                    shifted[k] = sl.in[k];
                    shifted[k].row += lr.scroll;
                }
                const mm::Layout lay = mm::BuildLayout(shifted, sl.found, float(REXCVAR_GET(menu_mouse_row_width)), float(REXCVAR_GET(menu_mouse_row_offset)));
                int lo, hi;
                RowRange(lay, &lo, &hi);
                if (!mm::SlotsMatchList(lay, first, end)) {
                    if (ci == 0) Note(view, "list 0x%08X (%d rows, highlight %d, window %d..%d): the best clip shows rows %d..%d; ", c.obj, lr.count, lr.selected, first, end - 1, lo, hi);
                    continue;
                }
                if (!mm::Plausible(lay)) {
                    if (ci == 0)
                        Note(view, "list 0x%08X: the layout of clip %s is not a list of text rows (%d rows, pitch %.1f, first row stage (%.1f, %.1f)); ", c.obj, sl.path, lay.n, lay.pitch,
                            lay.n ? lay.rows[0].x0 : 0.0f, lay.n ? lay.rows[0].y0 : 0.0f);
                    continue;
                }
                take_clip(sl);
                view.kind = Kind::List;
                view.obj = c.obj;
                view.selected = lr.selected;
                view.count = lr.count;
                view.layout = lay;
                view.rows_known = true;
                return;
            }
            continue;
        }
        // A UIMenu: only when the movie shows slots at all (no call is made into a menu object for a screen with no rows to point at).
        if (nclips == 0) {
            Note(view, "UIMenu 0x%08X not read (no slot clips); ", c.obj);
            continue;
        }
        int selected = -1, count = 0;
        char why[96] = {};
        if (!ReadUiMenu(b, c.obj, &selected, &count, why, sizeof(why))) {
            Note(view, "UIMenu 0x%08X: %s; ", c.obj, why);
            continue;
        }
        for (int ci = 0; ci < nclips; ++ci) {
            const Slots& sl = clips[ci];
            const mm::Layout lay = mm::BuildLayout(sl.in, sl.found, float(REXCVAR_GET(menu_mouse_row_width)), float(REXCVAR_GET(menu_mouse_row_offset)));
            bool inside = lay.n > 0;
            for (int k = 0; k < lay.n; ++k) inside = inside && lay.rows[k].row < count;
            int lo, hi;
            RowRange(lay, &lo, &hi);
            if (!inside) {
                if (ci == 0) Note(view, "UIMenu 0x%08X (%d rows, highlight %d): the best clip shows rows %d..%d; ", c.obj, count, selected, lo, hi);
                continue;
            }
            if (!mm::Plausible(lay)) {
                if (ci == 0)
                    Note(view, "UIMenu 0x%08X: the layout of clip %s is not a list of text rows (%d rows, pitch %.1f, first row stage (%.1f, %.1f)); ", c.obj, sl.path, lay.n, lay.pitch,
                        lay.n ? lay.rows[0].x0 : 0.0f, lay.n ? lay.rows[0].y0 : 0.0f);
                continue;
            }
            take_clip(sl);
            view.kind = Kind::UiMenu;
            view.obj = c.obj;
            view.selected = selected;
            view.count = count;
            view.layout = lay;
            view.rows_known = true;
            return;
        }
    }

    // Nothing the clip tree confirms. The pause movie's pages with no slot clips at all: the model.
    if (pause_movie && nclips == 0 && model_obj) {
        int first = 0, end = 0;
        mm::ListWindow(model_list.count, model_list.scrolls, model_list.scroll, model_list.max_rows, &first, &end);
        const mm::ModelPage page = PauseMenuIsOurList(model_obj) ? mm::ModelPage::Settings : mm::ModelPage::Hub;
        const mm::Layout lay = mm::ModelLayout(first, end - first, page);
        if (mm::Plausible(lay)) {
            view.kind = Kind::List;
            view.obj = model_obj;
            view.selected = model_list.selected;
            view.count = model_list.count;
            view.layout = lay;
            view.rows_known = true;
            view.model = true;
            Note(view, "rows %d..%d from the model of a %s page; ", first, end - 1, page == mm::ModelPage::Settings ? "settings" : "pause");
        }
    }
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

// How a menu was read: 0 = not (the fallback), 1 = rows from the movie's clip tree, 2 = rows from the model of the pause pages.
int ReadState(const MenuView& view) { return view.rows_known ? (view.model ? 2 : 1) : 0; }

// One line per menu and per way of reading it (a menu read another way later is reported again, up to a few times): `true` when a line was due, so the dump can follow it.
bool ReportMenu(const MenuView& view, const mm::HudRect& hud) {
    State& s = g_state;
    const int state = ReadState(view);
    if (view.obj == s.reported_obj && int(view.kind) == s.reported_kind && state == s.reported_state && std::strcmp(view.movie, s.reported_movie) == 0) return false;
    s.reports_of_obj = (view.obj == s.reported_obj && std::strcmp(view.movie, s.reported_movie) == 0) ? s.reports_of_obj + 1 : 1;
    s.reported_obj = view.obj;
    s.reported_kind = int(view.kind);
    s.reported_state = state;
    std::snprintf(s.reported_movie, sizeof(s.reported_movie), "%s", view.movie);
    if (s.reports_of_obj > 4) return false;  // the same menu flapping between ways of reading: the first few lines say it
    static const char* const kKind[] = {"unreadable", "list", "UIMenu"};
    if (state == 0) {
        MM_LOG("[menu-mouse] menu {} (0x{:08X}): FALLBACK ({}; hover off, click = ACCEPT)", view.movie, view.obj, view.why[0] ? view.why : "nothing to read");
        return true;
    }
    float px0, py0, px1, py1;
    mm::StageToWindow(hud, view.layout.rows[0].x0, view.layout.rows[0].y0, &px0, &py0);
    mm::StageToWindow(hud, view.layout.rows[0].x1, view.layout.rows[0].y1, &px1, &py1);
    char source[200];
    if (view.model) {
        std::snprintf(source, sizeof(source), "from the model of the pause pages (the movie has no slot clips)");
    } else {
        std::snprintf(source, sizeof(source), "from clip %s: %d of %d slot(s) shown, %d placed by the draw's matrices, scale %.3f x %.3f%s", view.clip, view.clip_shown, view.clip_slots, view.clip_world,
            view.scale.x, view.scale.y, view.scale.applied ? "" : " (not applied)");
    }
    MM_LOG("[menu-mouse] menu {}: {} 0x{:08X}, {} rows, highlight {}, {} on screen (rows {}..{}), pitch {:.1f}, width {:.1f}; first row stage ({:.1f}, {:.1f}) = window ({:.0f}, {:.0f})-({:.0f}, {:.0f}); {}",
        view.movie, kKind[int(view.kind)], view.obj, view.count, view.selected, view.layout.n, view.layout.rows[0].row, view.layout.rows[view.layout.n - 1].row, view.layout.pitch, view.layout.width,
        view.layout.rows[0].x0, view.layout.rows[0].y0, px0, py0, px1, py1, source);
    return true;
}

// ── the dump (menu_mouse_dump, menu_mouse_dump_new): what to read if a menu still falls back ──

void DumpList(const uint8_t* b, const char* label, uint32_t list, uint32_t seq) {
    if (!list) {
        MC_INFO("[menu-mouse] dump: {} list: none", label);
        return;
    }
    if (!Plausible(list)) {
        MC_INFO("[menu-mouse] dump: {} list 0x{:08X}: not a guest address", label, list);
        return;
    }
    ListRead lr;
    const bool ok = ReadList(b, list, &lr);
    char read[200];
    if (ok) {
        int first = 0, end = 0;
        mm::ListWindow(lr.count, lr.scrolls, lr.scroll, lr.max_rows, &first, &end);
        std::snprintf(read, sizeof(read), "reads: %d rows, highlight %d, window %d..%d", lr.count, lr.selected, first, end - 1);
    } else {
        std::snprintf(read, sizeof(read), "does not read: %s", lr.why);
    }
    MC_INFO("[menu-mouse] dump: {} list 0x{:08X} (rendered #{}, a page of ours: {}): vtable 0x{:08X}, select slot 0x{:08X}, flags 0x{:08X}; +176 rows 0x{:08X}, +180 0x{:08X}, +184 highlight {}, "
            "+188 rows that fit {}, +192 table 0x{:08X}, +196 vhsm state 0x{:08X}, +256 scroll {} (+260 {}); {}",
        label, list, seq, PauseMenuIsOurList(list) ? "yes" : "no", R32(b, list), VSlot(b, list, kVfSelectRow), R32(b, list + kObjFlags), R32(b, list + kListRows), R32(b, list + kListCount),
        int32_t(R32(b, list + kListSelect)), R32(b, list + kListMaxRows), R32(b, list + kListTable), R32(b, list + kListAdapter), int32_t(R32(b, list + kListScroll)), R32(b, list + kListScrolls), read);
    // The vhsm state a pause page's rows come from: its name (+20) and its first children's (the state's children at +44, each next at +36, as Hook_ListViewPopulate's diagnostics walk them).
    const uint32_t adapter = R32(b, list + kListAdapter);
    if (Plausible(adapter)) {
        const auto text = [&](uint32_t ea, char* out, size_t cap) {
            out[0] = 0;
            const uint32_t np = Plausible(ea) ? R32(b, ea + 20) : 0;
            size_t i = 0;
            if (Plausible(np)) {
                const char* nm = reinterpret_cast<const char*>(b + np);
                for (; i + 1 < cap && nm[i] >= 0x20 && nm[i] < 0x7F; ++i) out[i] = nm[i];
            }
            out[i] = 0;
        };
        char name[48], kids[240];
        text(adapter, name, sizeof(name));
        size_t used = 0;
        kids[0] = 0;
        int guard = 0;
        for (uint32_t c = R32(b, adapter + 44); Plausible(c) && guard < 10; c = R32(b, c + 36), ++guard) {
            char nm[40];
            text(c, nm, sizeof(nm));
            const int w = std::snprintf(kids + used, sizeof(kids) - used, "%s%s", used ? ", " : "", nm);
            if (w < 0 || size_t(w) >= sizeof(kids) - used) break;
            used += size_t(w);
        }
        MC_INFO("[menu-mouse] dump: {} list's vhsm state 0x{:08X} '{}', children: {}", label, adapter, name, kids);
    }
}

void DumpUiMenu(const uint8_t* b, uint32_t ui, uint32_t seq) {
    if (!ui) {
        MC_INFO("[menu-mouse] dump: UIMenu: none");
        return;
    }
    if (!Plausible(ui)) {
        MC_INFO("[menu-mouse] dump: UIMenu 0x{:08X}: not a guest address", ui);
        return;
    }
    int selected = -1, count = 0;
    char why[96] = {};
    const bool ok = ReadUiMenu(b, ui, &selected, &count, why, sizeof(why));
    MC_INFO("[menu-mouse] dump: UIMenu 0x{:08X} (filled #{}{}): vtable 0x{:08X}; {}", ui, seq, ui == g_state.gave_up_obj ? ", given up on" : "", R32(b, ui),
        ok ? "reads (" + std::to_string(count) + " rows, highlight " + std::to_string(selected) + ")" : std::string("does not read: ") + why);
}

// The clips of a slot, by name.
void KidNames(const uint8_t* b, uint32_t inst, char* out, size_t cap) {
    out[0] = 0;
    size_t used = 0;
    int guard = 0;
    for (uint32_t c = R32(b, inst + kInstFirstChild); Plausible(c) && guard < 8; c = R32(b, c + kInstNext), ++guard) {
        char nm[40];
        NameOf(b, c, nm, sizeof(nm));
        const int w = std::snprintf(out + used, cap - used, "%s%s%s", used ? ", " : "", nm, Alpha(b, c) ? "" : " (hidden)");
        if (w < 0 || size_t(w) >= cap - used) break;
        used += size_t(w);
    }
}

void DumpSlot(const uint8_t* b, uint32_t slot, int index, const mm::StageScale& scale) {
    char nm[40];
    NameOf(b, slot, nm, sizeof(nm));
    const mm::Affine loc = LocalOf(b, slot);
    mm::Affine world, fold;
    bool vis_fold = false;
    const bool world_ok = WorldOf(b, slot, &world);
    const bool fold_ok = FoldToRoot(b, slot, &fold, &vis_fold);
    mm::Affine st;
    bool vis = false, from_world = false;
    const bool placed = PlaceOf(b, slot, scale, &st, &vis, &from_world);
    char kids[160];
    KidNames(b, slot, kids, sizeof(kids));
    MC_INFO("[menu-mouse] dump:     {} (#{}) @0x{:08X} alpha {} dirty {}: local ({:.1f}, {:.1f}), world {} ({:.2f} {:.2f} {:.2f} {:.2f}; {:.1f}, {:.1f}), fold {} ({:.1f}, {:.1f}) -> stage {} ({:.1f}, {:.1f}){}; children: {}",
        nm, index, slot, int(Alpha(b, slot)), int(b[slot + kInstDirty]), loc.tx, loc.ty, world_ok ? "ok" : "unusable", world.a, world.b, world.c, world.d, world.tx, world.ty,
        fold_ok ? "ok" : "broken", fold.tx, fold.ty, placed ? (from_world ? "by world" : "by fold") : "none", st.tx, st.ty, vis ? "" : " hidden", kids);
}

// The dump: candidates, then the movie (its frame and scale, an outline of the clip tree two levels down), then every clip that holds slots with its first slots in full.
void DumpMenu(const uint8_t* b, const MenuView& view) {
    ++g_state.dumps;
    MC_INFO("[menu-mouse] dump #{}: menu {} (read as {}: {}){}", g_state.dumps, view.movie, view.kind == Kind::None ? "a fallback" : (view.model ? "the model" : "the clip tree"),
        view.kind == Kind::List ? "list" : (view.kind == Kind::UiMenu ? "UIMenu" : "nothing"), view.why[0] ? std::string("; ") + view.why : std::string());
    DumpList(b, "top of ours", PauseMenuTopOurList(), 0);
    DumpList(b, "last rendered", PauseMenuLastList(), PauseMenuLastListSeq());
    DumpUiMenu(b, PauseMenuLastUiMenu(), PauseMenuLastUiMenuSeq());

    larecomp::swf::Movie mv;
    if (!view.movie[0] || !larecomp::swf::FindMovie(b, view.movie, mv)) {
        MC_INFO("[menu-mouse] dump: movie {} is not registered", view.movie);
        return;
    }
    Display d;
    const mm::StageScale scale = ScaleOf(b, mv, &d);
    MC_INFO("[menu-mouse] dump: movie {}: entry 0x{:08X} ctx 0x{:08X} file 0x{:08X} root 0x{:08X} enabled {}; frame {:.1f} x {:.1f}, display {:.0f} x {:.0f} (k {:.3f} x {:.3f}) -> scale {:.4f} x {:.4f}{}", view.movie,
        mv.entry, mv.ctx, mv.file, mv.root, mv.enabled, d.frame_w, d.frame_h, d.w, d.h, d.kx, d.ky, scale.x, scale.y, scale.applied ? "" : " (not applied: numbers missing)");
    // The file's first words around the frame, in case +36 / +44 are not the frame.
    if (Plausible(mv.file)) {
        char words[200];
        size_t used = 0;
        for (uint32_t o = 24; o <= 60 && used + 24 < sizeof(words); o += 4) {
            const int w = std::snprintf(words + used, sizeof(words) - used, "+%u=%08X/%.1f ", o, R32(b, mv.file + o), ReadGuestF32(b, mv.file + o));
            if (w < 0) break;
            used += size_t(w);
        }
        MC_INFO("[menu-mouse] dump: swfFILE words (hex/float): {}", words);
    }

    // The outline: the root's children, theirs and theirs again, names and places only.
    int lines = 0;
    const auto outline = [&](auto&& self, uint32_t inst, int depth) -> void {
        int guard = 0;
        for (uint32_t c = R32(b, inst + kInstFirstChild); Plausible(c) && guard < 60 && lines < 150; c = R32(b, c + kInstNext), ++guard) {
            char nm[40];
            NameOf(b, c, nm, sizeof(nm));
            mm::Affine st;
            bool vis = false, from_world = false;
            const bool placed = PlaceOf(b, c, scale, &st, &vis, &from_world);
            MC_INFO("[menu-mouse] dump: tree {:>{}}{} @0x{:08X} alpha {} stage ({:.1f}, {:.1f}){}{}", "", depth * 2, nm, c, int(Alpha(b, c)), placed ? st.tx : 0.0f, placed ? st.ty : 0.0f,
                placed ? (from_world ? "" : " (fold)") : " (no place)", vis ? "" : " hidden");
            ++lines;
            if (depth < 2) self(self, c, depth + 1);
        }
    };
    outline(outline, mv.root, 0);

    // The clips that hold slots.
    ClipCand cands[8];
    const int nc = FindSlotsClips(b, mv.root, cands, 8);
    if (nc == 0) MC_INFO("[menu-mouse] dump: no clip of {} has a child called slot0 (searched {} levels down)", view.movie, 6);
    int order[8] = {};
    const int ranked = nc ? RankClips(b, cands, nc, order) : 0;
    for (int r = 0; r < ranked; ++r) {
        const int i = order[r];
        char path[96];
        PathOf(b, cands[i].inst, path, sizeof(path));
        MC_INFO("[menu-mouse] dump: slots clip #{} {} @0x{:08X}: {} slot(s), {} shown, depth {}{}", r + 1, path, cands[i].inst, cands[i].slots, cands[i].shown, cands[i].depth,
            r < kMaxClips ? " (tried against the list / UIMenu)" : "");
        int shown_lines = 0;
        for (int k = 0; k < kMaxSlots && shown_lines < 8; ++k) {
            char name[8];
            SlotName(k, name, sizeof(name));
            const uint32_t slot = larecomp::swf::FindChild(b, cands[i].inst, name);
            if (!slot) continue;
            DumpSlot(b, slot, k, scale);
            ++shown_lines;
        }
    }
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
        s.reported_state = -1;
        s.reports_of_obj = 0;
        s.reported_movie[0] = 0;
        s.gave_up_obj = 0;
        return;
    }

    s.wheel.Add(wheel_units);
    const bool dump = REXCVAR_GET(menu_mouse_dump);
    const bool dump_new = REXCVAR_GET(menu_mouse_dump_new);
    if (!dump) s.dump_armed = true;
    // With the new-menu dump on, every menu is read when it comes up, not only when the pointer does something (a debug run).
    const bool work = moved || left_clicks || right_clicks || s.wheel.HasStep() || s.pending.active || s.walk_target >= 0 || s.queued || (dump && s.dump_armed) || dump_new;
    if (!work) return;

    MenuView view;
    ReadMenu(b, view);
    const mm::HudRect hud = HudNow(window);
    const bool reported = ReportMenu(view, hud);

    // The dump: asked for (menu_mouse_dump), or a new menu was just reported (menu_mouse_dump_new, up to kMaxDumps a run). Only a movie name is needed: a menu that reads nothing is dumped too.
    constexpr int kMaxDumps = 24;
    const bool want_dump = (dump && s.dump_armed) || (dump_new && reported);
    if (want_dump && (s.dumps < kMaxDumps || (dump && s.dump_armed))) {
        s.dump_armed = false;
        DumpMenu(b, view);
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
