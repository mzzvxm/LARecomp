#ifndef REXGLUE_HAS_XEO3_TARGET
#include "menu_mouse.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#ifdef _WIN32
#include <Windows.h>
#endif

#include "imgui.h"
#include "settings/larecomp_settings.h"
#include <rex/hid/input_system.h>
#include <rex/input_keys.h>
#include <rex/ppc/call.h>
#include <rex/ppc/recompiled_code.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/function_dispatcher.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/ui_system.h>

#include "mc_engine/core/logging.h"
#include "mc_engine/display/ultrawide.h"
#include "mc_engine/hooks/hooks_internal.h"
#include "mc_engine/input/input_state.h"
#include "mc_engine/input/map_mouse.h"
#include "mc_engine/input/menu_mouse_core.h"
#include "mc_engine/keyboard_native.h"
#include "mc_engine/menus/pause_menu.h"
#include "mc_engine/menus/swf_dom.h"
#include "mc_engine/sdk_access.h"
#include "mc_engine/ui/settings.h"
#include "mc_engine/ui_events.h"
#include "larecomp_guest.h"

REX_EXTERN(__imp__rex_sub_821E6800);  // play a UI sound by name

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
    auto* rt = REX_RUNTIME();
    PPCFunc* fn = rt ? rt->FindFunction(fn_addr).value_or(nullptr) : nullptr;
    if (!fn) return false;
    const uint32_t r = larecomp::guest::Call<uint32_t>(fn, uint32_t(args)...);
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
    if (name >= 0x82000000u && name < 0x82FFFFF0u) larecomp::guest::Call<void>(__imp__rex_sub_821E6800, name);
}

// ── the window's mouse events (UI thread) -> atomics ────────────────────

// The wheel's detent in the units of g_wheel_units and of the wheel accumulator. 0.10 gave integer wheel units (MouseEvent::kScrollPerDetent, 120 on Windows); 0.11 gives detents, fractional
// for a smooth wheel, so the sum keeps the old units (the same constant as map_mouse.cpp's).
constexpr int32_t kScrollPerDetent = 120;

std::atomic<uint64_t> g_pos{0};          // the last pointer position, window pixels, x in the high half
std::atomic<uint64_t> g_move_seq{0};     // bumped by every motion event
std::atomic<uint64_t> g_click_pos{0};    // where the last left click was: its own message's place (mm::ClickPlaces), else the event's
std::atomic<uint64_t> g_click_event_pos{0};  // the place the window layer put on the same button-down event (SDL's last pointer position: not necessarily where the click was)
std::atomic<bool> g_click_from_message{false};  // g_click_pos came from the click's own message
std::atomic<uint64_t> g_motion_pos{0};   // the pointer at the last MOTION event
std::atomic<uint32_t> g_left_downs{0};   // button-down counts
std::atomic<uint32_t> g_right_downs{0};
std::atomic<uint32_t> g_back_downs{0};   // the back button (X1)
std::atomic<int32_t> g_wheel_units{0};
std::atomic<bool> g_listener_attached{false};
std::atomic<bool> g_wants_pointer{false};

uint64_t Pack(int32_t x, int32_t y) { return (uint64_t(uint32_t(x)) << 32) | uint32_t(y); }
void Unpack(uint64_t v, int32_t* x, int32_t* y) {
    *x = int32_t(uint32_t(v >> 32));
    *y = int32_t(uint32_t(v & 0xFFFFFFFFu));
}

uint64_t NowMs() {
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

// The click's own place (menu_mouse_core.h "Where a click was"). The window's message hook (Windows) notes where each button-down MESSAGE was; the button-down EVENT the listener gets
// afterwards takes it, because SDL's place on that event is the pointer's last place, which the desktop cursor can replace without a motion. Both run on the UI thread; the lock is for
// a second thread that pumps SDL.
std::mutex g_places_mutex;
mm::ClickPlaces g_places;
std::atomic<uint32_t> g_hook_buttons{0};  // button-down messages the hook saw: 0 with clicks arriving = the hook is not installed or was replaced (always 0 off Windows)

mm::Button ButtonOfEvent(rex::MouseButton b) {
    switch (b) {
        case rex::MouseButton::kLeft: return mm::Button::Left;
        case rex::MouseButton::kRight: return mm::Button::Right;
        case rex::MouseButton::kX1: return mm::Button::Back;
        default: return mm::Button::Other;
    }
}

#ifdef _WIN32
std::atomic<void*> g_game_hwnd{nullptr};
std::atomic<int> g_hook_lines{0};  // diagnostic lines the hook logged (menu_mouse_dump_new), capped
std::atomic<bool> g_hook_seen_any{false};     // the hook has run (its first message was logged)
std::atomic<bool> g_hook_seen_button{false};  // ... and has seen a button-down message (logged once)
HHOOK g_message_hook = nullptr;
constexpr int kMaxHookLines = 60;

// One diagnostic line of the hook, with the place of the message and the desktop cursor in the window's client pixels. Only with menu_mouse_dump_new; a run logs 60 of them at most.
void HookLog(const char* what, const MSG* msg, bool has_place, int32_t mx, int32_t my) {
    if (g_hook_lines.fetch_add(1, std::memory_order_relaxed) >= kMaxHookLines) return;
    POINT pt = msg->pt;
    if (HWND hwnd = static_cast<HWND>(g_game_hwnd.load(std::memory_order_relaxed))) ScreenToClient(hwnd, &pt);
    if (has_place) MC_INFO("[menu-mouse] message {}: client ({}, {}); the desktop cursor was at client ({}, {}) when it was posted", what, mx, my, int(pt.x), int(pt.y));
    else MC_INFO("[menu-mouse] message {}; the desktop cursor was at client ({}, {}) when it was posted (SDL takes this place for the pointer on a leave or a focus change)", what, int(pt.x), int(pt.y));
}

// What the hook does with a message the thread's queue gave up (the UI thread, inside GetMessage / PeekMessage, before the message is translated and dispatched). It only reads the message.
void NoteQueuedMessage(const MSG* msg) {
    HWND game = static_cast<HWND>(g_game_hwnd.load(std::memory_order_relaxed));
    // The proof that the hook runs at all: the first message it sees, once (run 4's SDL hook never logged anything: it was set on the exe's own copy of SDL).
    if (!g_hook_seen_any.load(std::memory_order_relaxed) && !g_hook_seen_any.exchange(true, std::memory_order_relaxed))
        MC_INFO("[menu-mouse] message hook: the first message it sees is 0x{:04X} for window 0x{:X}{}", uint32_t(msg->message), uint64_t(reinterpret_cast<uintptr_t>(msg->hwnd)),
            msg->hwnd == game ? " (the game's window)" : "");
    mm::Button b;
    if (mm::ButtonOfMessage(msg->message, uint32_t(HIWORD(uint32_t(msg->wParam))), &b)) {
        if (!game || msg->hwnd == game) {
            int32_t x, y;
            mm::ClientOfLParam(uint64_t(msg->lParam), &x, &y);
            {
                std::lock_guard<std::mutex> lock(g_places_mutex);
                g_places.NoteMessage(b, x, y, NowMs());
            }
            g_hook_buttons.fetch_add(1, std::memory_order_relaxed);
            const char* what = b == mm::Button::Left ? "WM_LBUTTONDOWN" : (b == mm::Button::Right ? "WM_RBUTTONDOWN" : "WM_XBUTTONDOWN");
            if (!g_hook_seen_button.exchange(true, std::memory_order_relaxed)) MC_INFO("[menu-mouse] message hook: the first button-down message is {} at client ({}, {})", what, x, y);
            if (larecomp::settings::menu_mouse_dump_new.Get()) HookLog(what, msg, true, x, y);
        }
        return;
    }
    // What may move SDL's pointer without a motion of the mouse (run 3's stale click): the messages around it, for the log.
    switch (msg->message) {
        case WM_MOUSELEAVE:
        case WM_CAPTURECHANGED:
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
        case WM_ACTIVATE:
            if ((!game || msg->hwnd == game) && larecomp::settings::menu_mouse_dump_new.Get()) {
                const char* name = msg->message == WM_MOUSELEAVE ? "WM_MOUSELEAVE" : msg->message == WM_CAPTURECHANGED ? "WM_CAPTURECHANGED"
                                 : msg->message == WM_SETFOCUS ? "WM_SETFOCUS" : msg->message == WM_KILLFOCUS ? "WM_KILLFOCUS" : "WM_ACTIVATE";
                HookLog(name, msg, false, 0, 0);
            }
            break;
        case WM_MOUSEMOVE: {
            // A motion message whose own place is the desktop cursor's place (posted by the system, not by a tour) and not where the last posted one was: the desktop cursor moved
            // or the system made a motion of its own. Logged only when it is not the place of the last motion message of the other kind.
            static int32_t last_posted_x = -1, last_posted_y = -1;
            if (game && msg->hwnd == game && larecomp::settings::menu_mouse_dump_new.Get()) {
                int32_t x, y;
                mm::ClientOfLParam(uint64_t(msg->lParam), &x, &y);
                POINT pt = msg->pt;
                ScreenToClient(game, &pt);
                const bool from_os = std::abs(int(pt.x) - x) <= 1 && std::abs(int(pt.y) - y) <= 1;
                if (!from_os) {
                    last_posted_x = x;
                    last_posted_y = y;
                } else if (std::abs(x - last_posted_x) > 3 || std::abs(y - last_posted_y) > 3) {
                    HookLog("WM_MOUSEMOVE from the system", msg, true, x, y);
                    last_posted_x = x;
                    last_posted_y = y;
                }
            }
            break;
        }
        default: break;
    }
}

// WH_GETMESSAGE, a hook of the UI thread's own message queue: Windows calls it for every message GetMessage / PeekMessage hands to that thread, the posted ones (a tour's
// PostMessageW) and the real mouse's alike, whoever calls them (SDL's pump inside rexruntime.dll, a dialog, a driver). A message that is only looked at (PM_NOREMOVE) is not noted: it
// comes again when it is removed. Never swallows a message; the hooks that were there before are called on (CallNextHookEx).
//
// Why not SDL_SetWindowsMessageHook, which the first version of this used: on the 0.10 SDK larecomp.exe and rexruntime.dll each carried their own copy of SDL3 (SDL3-static.lib was linked into
// both; the DLL exported no C symbol of SDL). The window, its message loop and the hook variable that loop reads were the DLL's; the exe's call set the exe's copy, which nothing ever ran. Run 4's
// log showed it: every click `[no click message was seen]`, and no line from the hook though menu_mouse_dump_new was on.
// On the 0.11 SDK there is ONE shared SDL3.dll (the exe, the SDK's modules and the wheel all call the same copy), so SDL_SetWindowsMessageHook would reach the window's loop now. This hook stays all
// the same: it is Win32's own, it does not care which copy of SDL pumps the thread or whether the SDK changes how it pumps, and the point of it is unchanged: the click's own place is in the
// message, SDL's place on the event is the pointer's last place (menu_mouse_core.h "Where a click was").
LRESULT CALLBACK GetMessageHook(int code, WPARAM wparam, LPARAM lparam) {
    if (mm::HookMessageUsable(code, uint64_t(wparam)) && lparam) NoteQueuedMessage(reinterpret_cast<const MSG*>(lparam));
    return CallNextHookEx(nullptr, code, wparam, lparam);
}
#endif

// The UI system's input events (ui_events.h; 0.10 added a WindowInputListener to the window, layer 0): it never marks an event handled, and the events an overlay that wants the mouse keeps
// do not reach it. Runs on the UI thread, after the message hook has seen the message of the same event.
void OnInput(rex::InputEvent& e) {
    using Type = rex::InputEvent::Type;
    const int32_t ex = int32_t(e.x);
    const int32_t ey = int32_t(e.y);
    switch (e.type) {
        case Type::kMotion:
            g_pos.store(Pack(ex, ey), std::memory_order_relaxed);
            g_motion_pos.store(Pack(ex, ey), std::memory_order_relaxed);
            g_move_seq.fetch_add(1, std::memory_order_relaxed);
            break;
        case Type::kButtonDown: {
            const mm::Button button = ButtonOfEvent(e.button);
            mm::ClickPick pick;
            pick.x = ex;
            pick.y = ey;
            if (button != mm::Button::Other) {
                std::lock_guard<std::mutex> lock(g_places_mutex);
                pick = g_places.TakeEvent(button, ex, ey, NowMs());
            }
            g_pos.store(Pack(pick.x, pick.y), std::memory_order_relaxed);
            switch (button) {
                case mm::Button::Left:
                    g_click_pos.store(Pack(pick.x, pick.y), std::memory_order_relaxed);
                    g_click_event_pos.store(Pack(ex, ey), std::memory_order_relaxed);
                    g_click_from_message.store(pick.from_message, std::memory_order_relaxed);
                    g_left_downs.fetch_add(1, std::memory_order_release);
                    break;
                case mm::Button::Right: g_right_downs.fetch_add(1, std::memory_order_relaxed); break;
                case mm::Button::Back: g_back_downs.fetch_add(1, std::memory_order_relaxed); break;
                default: break;
            }
            break;
        }
        case Type::kWheel: {
            // CHECK: scroll_y is positive away from the player (wheel up), as MouseEvent::scroll_y was: the highlight moves up on a wheel turned up (map_mouse.cpp reads it the same way).
            const int32_t units = int32_t(std::lround(e.scroll_y * float(kScrollPerDetent)));
            if (units) g_wheel_units.fetch_add(units, std::memory_order_relaxed);
            break;
        }
        default: break;
    }
}

// The handler and the click's message hook, once, from the first tick that finds the window. The handler can be added from any thread; the hook is set by a task on the UI thread (as 0.10's
// listener was added there), though a thread hook can be set from any thread given the window thread's id.
void EnsureListener(rex::ui::IUISystem& ui) {
    if (g_listener_attached.exchange(true)) return;
    if (!larecomp::uievents::AddInputHandler(&OnInput)) {
        MC_WARN("[menu-mouse] the UI system's input handler table is full: the mouse does nothing in menus");
        return;
    }
    MC_INFO("[menu-mouse] mouse handler attached to the UI system's input events");
#ifdef _WIN32
    // The click's own place (see g_places): a WH_GETMESSAGE hook on the thread that owns the window (the UI thread: the task runs on it). Nothing is replaced: Windows chains it with any
    // hook already there. If the hook cannot be set, the click line says "no click message was seen" and the event's place is used, as before.
    ui.PostTask([&ui]() {
        void* window = nullptr;
        if (auto native = ui.GetNativeWindow(); native && native->type == rex::NativeWindow::Type::kWin32) window = native->window;
        HWND hwnd = static_cast<HWND>(window);
        g_game_hwnd.store(hwnd, std::memory_order_relaxed);
        const DWORD window_thread = hwnd ? GetWindowThreadProcessId(hwnd, nullptr) : 0;
        const DWORD thread_id = window_thread ? window_thread : GetCurrentThreadId();
        if (!g_message_hook) g_message_hook = SetWindowsHookExW(WH_GETMESSAGE, &GetMessageHook, nullptr, thread_id);
        if (g_message_hook)
            MC_INFO("[menu-mouse] click places come from the button messages themselves (message hook on thread {}, window 0x{:X}; it logs its first message)", uint32_t(thread_id),
                uint64_t(reinterpret_cast<uintptr_t>(hwnd)));
        else MC_WARN("[menu-mouse] the message hook could not be set (error {}): clicks take the window layer's place", uint32_t(GetLastError()));
    });
#else
    (void)ui;
#endif
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
    bool card = false;       // the rows are placed by the pause card's projection (mm::CardLayout) from the slots' world y and the prompt bar's
    bool card_wide = false;  // ... for a page with `< value >` rows
    float card_prompt_y = 0.0f;
    bool relaxed = false;    // the layout passed only the opened-up plausibility test (a clip called slotsA with shown slots)
    bool unverified = false; // a movie other than PAUSEMOVIE: the layout was read but is not trusted (menu_mouse_trust_layouts): rows_known is false
    mm::GaragePage garage = mm::GaragePage::None;  // the garage's card (mm::GarageCardLayout): the first menu or a two-row pop-up; its layout is fitted to pictures, so it is trusted
    char movie[48] = {};
    // What the reading found, for the log (ReportMenu) and the dump.
    mm::StageScale scale;
    bool scale_known = false;
    char clip[96] = {};      // the clip that holds the slots, as a path
    int clip_slots = 0;
    int clip_shown = 0;
    int clip_world = 0;
    char why[1024] = {};     // what each candidate lacked
};

// What this file remembers between ticks (guest thread only).
struct State {
    uint64_t tick = 0;
    uint64_t last_move_seq = 0;
    uint32_t last_left = 0, last_right = 0, last_back = 0;
    mm::WheelAccum wheel{int(kScrollPerDetent), 6};
    mm::PressPacer pacer{2};
    mm::PendingClick pending;
    mm::WalkWatch walk{8};
    mm::GarageTracker garage;          // which UIMenu is the garage's first menu (mm::GarageTracker)
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
bool PlaceOf(const uint8_t* b, uint32_t inst, const mm::StageScale& scale, mm::Affine* out, bool* visible, bool* world, mm::Affine* raw = nullptr) {
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
    const mm::Affine& chosen = src == mm::MatrixSource::World ? w : fold;
    *out = mm::Scaled(chosen, scale);
    if (raw) *raw = chosen;
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

// The candidates in the order to try them: the clip called slotsA with shown slots first, then the most shown slots (a clip whose slots are all hidden is the page that went away), then the
// most slots, the shallowest. Returns how many are worth trying (any with a slot), `order` holds their indexes.
int RankClips(const uint8_t* b, const ClipCand* c, int n, int* order) {
    int score[8];
    int m = 0;
    for (int i = 0; i < n && i < 8; ++i) {
        char nm[40];
        NameOf(b, c[i].inst, nm, sizeof(nm));
        // slotsA with anything shown is the list (the pause movie's and the garage's): ahead of a clip with more shown slots that is no list (the pause movie's `iconlist_online` has 24 slots
        // all shown, all at the origin: it was ranked first in the first runs, so its "shows rows 0..23" was the only reason the log gave; the unnamed clip with slot0, slot1 and slot23 at
        // stage x 1565, off the right of the screen, is some other widget and was never the list).
        const bool slots_a = std::strcmp(nm, "slotsA") == 0;
        score[i] = (slots_a && c[i].shown > 0 ? 100000 : 0) + c[i].shown * 100 + (slots_a ? 50 : 0) + c[i].slots - c[i].depth;
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

// The pause card hangs from the prompt bar: the world origin of `prompt_bg` in the movie's units (it is a child of `menu`, the parent of the slot clip `slotsA`).
struct CardAnchor {
    bool ok = false;
    float x = 0.0f, y = 0.0f;
};

CardAnchor FindCardAnchor(const uint8_t* b, uint32_t clip) {
    CardAnchor a;
    uint32_t cur = R32(b, clip + kInstParent);
    for (int up = 0; up < 3 && Plausible(cur); ++up, cur = R32(b, cur + kInstParent)) {
        const uint32_t bar = larecomp::swf::FindChild(b, cur, "prompt_bg");
        if (!bar) continue;
        mm::Affine w, fold;
        bool vis = false;
        if (WorldOf(b, bar, &w)) {
            a.x = w.tx;
            a.y = w.ty;
            a.ok = std::isfinite(a.x) && std::isfinite(a.y);
        } else if (FoldToRoot(b, bar, &fold, &vis)) {
            a.x = fold.tx;
            a.y = fold.ty;
            a.ok = std::isfinite(a.x) && std::isfinite(a.y);
        }
        return a;
    }
    return a;
}

struct Slots {
    uint32_t clip = 0;       // the clip that holds them
    CardAnchor anchor;       // the pause card's anchor next to it
    char path[96] = {};
    int found = 0;           // the slot clips read (shown or not)
    int shown = 0;           // of them visible
    int world = 0;           // shown ones placed by the world matrix of the last draw (the rest by the fold)
    mm::SlotIn in[kMaxSlots];  // .row is the slot's number: the caller adds the list's scroll
};

void ReadSlotsOf(const uint8_t* b, uint32_t clip, const mm::StageScale& scale, Slots* out) {
    *out = Slots{};
    out->clip = clip;
    out->anchor = FindCardAnchor(b, clip);
    PathOf(b, clip, out->path, sizeof(out->path));
    for (int i = 0; i < kMaxSlots; ++i) {
        char name[8];
        SlotName(i, name, sizeof(name));
        const uint32_t slot = larecomp::swf::FindChild(b, clip, name);
        if (!slot) continue;
        mm::SlotIn s;
        s.row = i;
        bool vis = false, world = false;
        mm::Affine raw;
        if (!PlaceOf(b, slot, scale, &s.to_stage, &vis, &world, &raw)) continue;
        s.visible = vis;
        s.mx = raw.tx;
        s.my = raw.ty;
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

// The slot `slot` of a vtable (by its address) as a callable guest function; 0 when it is not one.
uint32_t VtableSlot(const uint8_t* b, uint32_t vtable, uint32_t slot) {
    if (!VtableLike(vtable)) return 0;
    const uint32_t fn = R32(b, vtable + slot);
    return VtableLike(fn) ? fn : 0;
}

// A live UIMenu of the game's own class. The highlighted row comes from its vfunc; the rows are counted through "the child shown on row N" until it names none. A menu of another class
// (the garage's pop-ups have 0x8208FD84) is read too when its highlight and row slots ARE the UIMenu class's (the same two functions: calling them is calling the base class's code on an
// object that has its layout); `*base_class` says which it was. A class whose slots differ is refused, and the reason names both pairs of functions for the log.
bool ReadUiMenu(const uint8_t* b, uint32_t menu, int* selected, int* count, char* why, size_t why_cap, bool* base_class = nullptr) {
    if (!HeapLike(menu)) {
        std::snprintf(why, why_cap, "not a heap address");
        return false;
    }
    const bool is_base = R32(b, menu) == kUiMenuVtable;
    if (base_class) *base_class = is_base;
    if (!is_base) {
        const uint32_t vt = R32(b, menu);
        const uint32_t sel_base = VtableSlot(b, kUiMenuVtable, kUiMenuSelectedVfunc), child_base = VtableSlot(b, kUiMenuVtable, kUiMenuChildAtVfunc);
        const uint32_t sel_own = VtableSlot(b, vt, kUiMenuSelectedVfunc), child_own = VtableSlot(b, vt, kUiMenuChildAtVfunc);
        if (!sel_base || !child_base || sel_own != sel_base || child_own != child_base) {
            std::snprintf(why, why_cap, "vtable 0x%08X is not the UIMenu class 0x%08X (highlight / row slots 0x%08X / 0x%08X, the class's are 0x%08X / 0x%08X)", vt, kUiMenuVtable, sel_own, child_own,
                sel_base, child_base);
            return false;
        }
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

// A slot clip whose own name is slotsA (the pause movie's and the garage's lists).
bool IsSlotsAClip(const Slots& sl) {
    const size_t n = std::strlen(sl.path);
    return n >= 6 && std::strcmp(sl.path + n - 6, "slotsA") == 0 && (n == 6 || sl.path[n - 7] == '.');
}

// The rows a list's clip shows, on the stage. The pause movie is drawn on a card in a 3D scene: its rows are placed by the card's projection from the slots' world y and the prompt bar's
// (mm::CardLayout, `wide` = a page with `< value >` rows, which is a wider card); any other movie is scaled onto the stage (mm::BuildLayout).
mm::Layout LayoutOfList(const uint8_t* b, uint32_t list, bool pause_movie, const Slots& sl, const mm::SlotIn* shifted, bool* card, bool* wide) {
    *card = false;
    *wide = false;
    if (pause_movie) {
        // No prompt bar found: nothing to hang the card from, and the display state's scale is not the card's: no layout (the model takes over).
        if (!sl.anchor.ok) return mm::Layout{};
        mm::CardSlot cs[kMaxSlots];
        for (int k = 0; k < sl.found; ++k) {
            cs[k].row = shifted[k].row;
            cs[k].x = shifted[k].mx;
            cs[k].y = shifted[k].my;
            cs[k].visible = shifted[k].visible;
            if (cs[k].visible && IsValueRow(b, list, cs[k].row)) *wide = true;
        }
        *card = true;
        return mm::CardLayout(cs, sl.found, sl.anchor.y, *wide, float(larecomp::settings::menu_mouse_row_offset.Get()));
    }
    return mm::BuildLayout(shifted, sl.found, float(larecomp::settings::menu_mouse_row_width.Get()), float(larecomp::settings::menu_mouse_row_offset.Get()));
}

// Reads whatever menu is up. Always fills `view.movie` and, when anything was rendered, `view.obj` (the first candidate); `view.kind` is None when nothing readable is there (a fallback menu)
// and `view.why` says what each candidate lacked.
//   candidates   PAUSEMOVIE: the list of the page of ours on top (exact), else the list the game rendered last (the pause screen's pages and every settings page are lists in this movie).
//                any other movie: the list and the UIMenu FlashNavigator filled, the later one first (the garage has both: its main menu is a UIMenu, its customization pages are lists).
//   a candidate  is taken when the clip tree shows rows the list draws (its window) or rows the UIMenu has, in a layout that passes Plausible.
//   the model    PAUSEMOVIE only, when the movie has no slot clips at all and the list reads: the rows are put where the pictures have them (mm::ModelLayout).
void ReadMenuRaw(const uint8_t* b, MenuView& view) {
    view = MenuView{};
    larecomp::keyboard::CurrentMovieName(view.movie, sizeof(view.movie));
    if (std::strcmp(view.movie, "GARAGEMOVIE") != 0) g_state.garage.Leave();
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
    // Two passes. The strict one reads every candidate against every slot clip. When nothing passed, the clip called slotsA is read again with the limits opened up (PlausibleRelaxed): shown
    // slots in a sane order are enough to point at rows, and a menu that falls back is a menu that cannot be hovered (the first runs had seven of those with slotsA showing 3 to 6 slots).
    for (int pass = 0; pass < 2; ++pass) {
        const bool relaxed = pass == 1;
        for (int i = 0; i < nc; ++i) {
            const Cand& c = cands[i];
            if (c.kind == Kind::List) {
                ListRead lr;
                if (!ReadList(b, c.obj, &lr)) {
                    if (!relaxed) Note(view, "list 0x%08X: %s; ", c.obj, lr.why);
                    continue;
                }
                int first = 0, end = 0;
                mm::ListWindow(lr.count, lr.scrolls, lr.scroll, lr.max_rows, &first, &end);
                if (!model_obj) {
                    model_list = lr;
                    model_obj = c.obj;
                }
                if (nclips == 0) {
                    if (!relaxed) Note(view, "list 0x%08X reads (%d rows, highlight %d, window %d..%d) but there are no slot clips; ", c.obj, lr.count, lr.selected, first, end - 1);
                    continue;
                }
                // Every slot clip against this list: the one whose shown rows are rows the list draws, in a layout of text rows.
                for (int ci = 0; ci < nclips; ++ci) {
                    const Slots& sl = clips[ci];
                    if (relaxed && !IsSlotsAClip(sl)) continue;
                    mm::SlotIn shifted[kMaxSlots];
                    for (int k = 0; k < sl.found; ++k) {
                        shifted[k] = sl.in[k];
                        shifted[k].row += lr.scroll;
                        // The relaxed pass takes the slots the list draws and leaves out the rest (a page change leaves the old page's slots shown for a moment).
                        if (relaxed && (shifted[k].row < first || shifted[k].row >= end)) shifted[k].visible = false;
                    }
                    bool card = false, wide = false;
                    const mm::Layout lay = LayoutOfList(b, c.obj, pause_movie, sl, shifted, &card, &wide);
                    int lo, hi;
                    RowRange(lay, &lo, &hi);
                    if (!mm::SlotsMatchList(lay, first, end)) {
                        if (!relaxed) Note(view, "list 0x%08X (%d rows, highlight %d, window %d..%d): clip %s shows rows %d..%d; ", c.obj, lr.count, lr.selected, first, end - 1, sl.path, lo, hi);
                        continue;
                    }
                    if (!(relaxed ? mm::PlausibleRelaxed(lay) : mm::Plausible(lay))) {
                        if (!relaxed)
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
                    view.card = card;
                    view.card_wide = wide;
                    view.card_prompt_y = sl.anchor.y;
                    view.relaxed = relaxed;
                    return;
                }
                continue;
            }
            // A UIMenu: only when the movie shows slots at all (no call is made into a menu object for a screen with no rows to point at).
            if (nclips == 0) {
                if (!relaxed) Note(view, "UIMenu 0x%08X not read (no slot clips); ", c.obj);
                continue;
            }
            int selected = -1, count = 0;
            char why[220] = {};
            if (!ReadUiMenu(b, c.obj, &selected, &count, why, sizeof(why))) {
                if (!relaxed) Note(view, "UIMenu 0x%08X: %s; ", c.obj, why);
                continue;
            }
            for (int ci = 0; ci < nclips; ++ci) {
                const Slots& sl = clips[ci];
                if (relaxed && !IsSlotsAClip(sl)) continue;
                mm::SlotIn local[kMaxSlots];
                for (int k = 0; k < sl.found; ++k) {
                    local[k] = sl.in[k];
                    if (relaxed && local[k].row >= count) local[k].visible = false;
                }
                const mm::Layout lay = mm::BuildLayout(local, sl.found, float(larecomp::settings::menu_mouse_row_width.Get()), float(larecomp::settings::menu_mouse_row_offset.Get()));
                bool inside = lay.n > 0;
                for (int k = 0; k < lay.n; ++k) inside = inside && lay.rows[k].row < count;
                int lo, hi;
                RowRange(lay, &lo, &hi);
                if (!inside) {
                    if (!relaxed) Note(view, "UIMenu 0x%08X (%d rows, highlight %d): clip %s shows rows %d..%d; ", c.obj, count, selected, sl.path, lo, hi);
                    continue;
                }
                if (!(relaxed ? mm::PlausibleRelaxed(lay) : mm::Plausible(lay))) {
                    if (!relaxed)
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
                view.relaxed = relaxed;
                return;
            }
        }
    }

    // The garage's card: its first menu and its two-row pop-ups, placed by the card fitted to run 4's pictures (mm::GarageCardLayout). The garage movie's clips give no rows (no `slotN` child of its
    // menu clip shows), so only the UIMenu is needed: its row count and highlight.
    if (!pause_movie && ui && std::strcmp(view.movie, "GARAGEMOVIE") == 0) {
        int selected = -1, count = 0;
        char why[220] = {};
        bool base_class = false;
        if (!ReadUiMenu(b, ui, &selected, &count, why, sizeof(why), &base_class)) {
            Note(view, "garage card: UIMenu 0x%08X: %s; ", ui, why);
        } else {
            // A main menu object that is gone (a new visit that reuses the root's address): forget it.
            const uint32_t main_menu = g_state.garage.MainMenu();
            if (main_menu && main_menu != ui && !(HeapLike(main_menu) && R32(b, main_menu) == kUiMenuVtable)) g_state.garage.Leave();
            const mm::GaragePage page = g_state.garage.Classify(mv.root, ui, base_class, count);
            const mm::Layout lay = mm::GarageCardLayout(count, float(larecomp::settings::menu_mouse_row_offset.Get()));
            if (page == mm::GaragePage::None) {
                Note(view, "garage card: UIMenu 0x%08X (%d rows, %s class) is not the garage's first menu or a two-row pop-up, so it is not placed by the card; ", ui, count,
                    base_class ? "base" : "another");
            } else if (!mm::Plausible(lay)) {
                Note(view, "garage card: the layout is not plausible; ");
            } else {
                view.kind = Kind::UiMenu;
                view.obj = ui;
                view.selected = selected;
                view.count = count;
                view.layout = lay;
                view.rows_known = true;
                view.card = true;
                view.garage = page;
                return;
            }
        }
    }

    // Nothing the clip tree confirms. The pause movie's pages whose clips cannot be read (no slot clips, no prompt bar to hang the card from) but whose list can: the model of the card.
    if (pause_movie && model_obj) {
        int first = 0, end = 0;
        mm::ListWindow(model_list.count, model_list.scrolls, model_list.scroll, model_list.max_rows, &first, &end);
        bool values = false;
        for (int r = first; r < end && r < first + mm::kMaxRows; ++r) values = values || IsValueRow(b, model_obj, r);
        const mm::ModelPage page = values ? mm::ModelPage::Settings : mm::ModelPage::Hub;
        const mm::Layout lay = mm::ModelLayout(first, end - first, page);
        if (mm::Plausible(lay)) {
            view.kind = Kind::List;
            view.obj = model_obj;
            view.selected = model_list.selected;
            view.count = model_list.count;
            view.layout = lay;
            view.rows_known = true;
            view.model = true;
            view.card = true;
            view.card_wide = values;
            Note(view, "rows %d..%d from the model of a %s page; ", first, end - 1, values ? "value (wide)" : "plain (narrow)");
        }
    }
}

// What the mouse is allowed to believe of a reading. The pause movie's rows are placed by a projection fitted to its card (rms 0.22 stage pixels over four pages) and the garage's first menu
// and pop-ups by the one fitted to theirs (mm::GarageCardLayout, rms 0.06). Every other movie is a
// clip-tree layout through the display state's scale and nothing else, and the one picture there is of such a menu (round 1, the garage) says it is wrong: a plausible-looking list at
// the wrong place makes hover walk to the wrong row and clicks land on no row at all. Until a run measures it (the tour turns menu_mouse_trust_layouts on and the log line has the numbers
// to hold against the picture), those menus are fallback menus.
void ReadMenu(const uint8_t* b, MenuView& view) {
    ReadMenuRaw(b, view);
    if (!view.rows_known || mm::LayoutTrusted(std::strcmp(view.movie, "PAUSEMOVIE") == 0 || view.garage != mm::GaragePage::None, larecomp::settings::menu_mouse_trust_layouts.Get())) return;
    const mm::RowBox& first = view.layout.rows[0];
    // In front of what each candidate lacked (the reading's notes can fill the line).
    char head[260];
    const int hn = std::snprintf(head, sizeof(head),
        "the layout of this movie's rows (%d rows, first row stage (%.1f, %.1f), pitch %.1f) is not checked against a picture yet, so it is not used for hover or clicks on rows "
        "(menu_mouse_trust_layouts reads it anyway); ",
        view.layout.n, first.x0, first.y0, view.layout.pitch);
    const size_t len = size_t(std::max(0, std::min<int>(hn, int(sizeof(head)) - 1)));
    const size_t cur = std::strlen(view.why);
    const size_t keep = std::min(cur, sizeof(view.why) - 1 - len);
    std::memmove(view.why + len, view.why, keep);
    std::memcpy(view.why, head, len);
    view.why[len + keep] = 0;
    view.rows_known = false;
    view.unverified = true;
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

mm::HudRect HudNow(uint32_t w, uint32_t h) {
    return mm::HudOf(mc::ultrawide::FrameFor(w, h), mc::ultrawide::UiWidthFraction(), mc::ultrawide::UiHeightFraction());
}

// How a menu was read: 0 = not (the fallback), 1 = rows from the movie's clip tree, 2 = rows from the model of the pause pages, 3 = the garage's card (first menu / pop-up).
int ReadState(const MenuView& view) { return view.rows_known ? (view.garage != mm::GaragePage::None ? 3 : (view.model ? 2 : 1)) : 0; }

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
    char source[320];
    if (view.garage != mm::GaragePage::None) {
        std::snprintf(source, sizeof(source), "from the garage card fitted to run 4's pictures (%s: %d rows from the top of the card; rows at x 1000: stage y %.1f..%.1f; pitch there %.1f)",
            view.garage == mm::GaragePage::Main ? "the garage's first menu" : "a two-row pop-up", view.layout.n,
            mm::CardY(view.layout.map, 1000.0f, 0.5f * (view.layout.rows[0].v0 + view.layout.rows[0].v1)) + view.layout.shift,
            mm::CardY(view.layout.map, 1000.0f, 0.5f * (view.layout.rows[view.layout.n - 1].v0 + view.layout.rows[view.layout.n - 1].v1)) + view.layout.shift,
            (view.layout.map.c + view.layout.map.d * 1000.0f) * mm::kGarageRowUnit);
    } else if (view.model) {
        std::snprintf(source, sizeof(source), "from the model of the pause card (no readable slot clips or prompt bar), %s page", view.card_wide ? "wide" : "narrow");
    } else if (view.card) {
        std::snprintf(source, sizeof(source), "from clip %s on the pause card (%s page, prompt bar world y %.1f, %.1f movie units between rows; rows at x 300: stage y %.1f..%.1f)%s", view.clip,
            view.card_wide ? "wide" : "narrow", view.card_prompt_y, view.layout.v_pitch,
            mm::CardY(view.layout.map, 300.0f, 0.5f * (view.layout.rows[0].v0 + view.layout.rows[0].v1)) + view.layout.shift,
            mm::CardY(view.layout.map, 300.0f, 0.5f * (view.layout.rows[view.layout.n - 1].v0 + view.layout.rows[view.layout.n - 1].v1)) + view.layout.shift,
            view.relaxed ? " [relaxed plausibility]" : "");
    } else {
        std::snprintf(source, sizeof(source), "from clip %s: %d of %d slot(s) shown, %d placed by the draw's matrices, scale %.3f x %.3f%s%s", view.clip, view.clip_shown, view.clip_slots, view.clip_world,
            view.scale.x, view.scale.y, view.scale.applied ? "" : " (not applied)", view.relaxed ? " [relaxed plausibility]" : "");
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
    char why[220] = {};
    bool base_class = false;
    const bool ok = ReadUiMenu(b, ui, &selected, &count, why, sizeof(why), &base_class);
    MC_INFO("[menu-mouse] dump: UIMenu 0x{:08X} (filled #{}{}): vtable 0x{:08X}{}; {}", ui, seq, ui == g_state.gave_up_obj ? ", given up on" : "", R32(b, ui),
        ok && !base_class ? " (not the base class, but its highlight and row slots are the base class's)" : "",
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

// A shown slot's own subtree (its children and theirs, with their places): where a value row keeps its `<`, its text and its `>`. Round 3 measured those on the screen at stage x 398 (`<`), 412..438
// (the value) and 453..464 (`>`); this prints the movie's side of them so the next run can say which clip is which.
void DumpSlotTree(const uint8_t* b, uint32_t slot, int index) {
    int lines = 0;
    const auto walk = [&](auto&& self, uint32_t inst, int depth) -> void {
        int guard = 0;
        for (uint32_t c = R32(b, inst + kInstFirstChild); Plausible(c) && guard < 12 && lines < 24; c = R32(b, c + kInstNext), ++guard) {
            char nm[40];
            NameOf(b, c, nm, sizeof(nm));
            mm::Affine w;
            const bool world_ok = WorldOf(b, c, &w);
            const mm::Affine loc = LocalOf(b, c);
            MC_INFO("[menu-mouse] dump:       slot{} subtree {:>{}}{} alpha {} local ({:.1f}, {:.1f}) world {}({:.1f}, {:.1f})", index, "", depth * 2, nm, int(Alpha(b, c)), loc.tx, loc.ty,
                world_ok ? "" : "unusable ", w.tx, w.ty);
            ++lines;
            if (depth < 2) self(self, c, depth + 1);
        }
    };
    walk(walk, slot, 0);
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
        // The pause card: the prompt bar this clip hangs from, and the first two shown slots' subtrees.
        const CardAnchor anchor = FindCardAnchor(b, cands[i].inst);
        MC_INFO("[menu-mouse] dump:     card anchor of {}: prompt_bg {} world ({:.1f}, {:.1f})", path, anchor.ok ? "found" : "NOT found", anchor.x, anchor.y);
        const size_t plen = std::strlen(path);
        if (plen >= 6 && std::strcmp(path + plen - 6, "slotsA") == 0) {
            int trees = 0;
            for (int k = 0; k < kMaxSlots && trees < 2; ++k) {
                char name[8];
                SlotName(k, name, sizeof(name));
                const uint32_t slot = larecomp::swf::FindChild(b, cands[i].inst, name);
                if (!slot || !Alpha(b, slot)) continue;
                DumpSlotTree(b, slot, k);
                ++trees;
            }
        }
    }
    if (view.card && view.layout.n > 0) {
        for (int k = 0; k < view.layout.n; ++k) {
            const mm::RowBox& r = view.layout.rows[k];
            const float vm = 0.5f * (r.v0 + r.v1);
            const bool garage = view.garage != mm::GaragePage::None;
            const float xa = garage ? 900.0f : 250.0f, xb = garage ? 1000.0f : 300.0f, xc = garage ? 1100.0f : 444.0f;
            MC_INFO("[menu-mouse] dump:     card row {}: v {:.1f} (band {:.1f}..{:.1f}), stage y at x {:.0f} / {:.0f} / {:.0f} = {:.1f} / {:.1f} / {:.1f}, x {:.0f}..{:.0f}, {} row", r.row, vm, r.v0, r.v1, xa, xb, xc,
                mm::CardY(view.layout.map, xa, vm) + view.layout.shift, mm::CardY(view.layout.map, xb, vm) + view.layout.shift,
                mm::CardY(view.layout.map, xc, vm) + view.layout.shift, r.x0, r.x1, view.kind == Kind::List && IsValueRow(b, view.obj, r.row) ? "a value" : "a plain");
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

    rex::ui::IUISystem* ui = larecomp::sdk::UI();
    rex::hid::IInputSystem* input = larecomp::sdk::Input();
    if (!ui || !ui->HasWindow() || !input) return;
    EnsureListener(*ui);
    // The window's client size in physical pixels, from the last resize event the input system saw (0 before the first: map_mouse.cpp reads it the same way).
    const rex::PointerState pointer = input->GetPointer();
    const uint32_t window_w = pointer.window_width;
    const uint32_t window_h = pointer.window_height;
    if (window_w == 0 || window_h == 0) {
        g_wants_pointer.store(false, std::memory_order_relaxed);
        return;
    }

    // The events since the last tick.
    const uint64_t move_seq = g_move_seq.load(std::memory_order_relaxed);
    const bool moved = move_seq != s.last_move_seq;
    s.last_move_seq = move_seq;
    const uint32_t lefts = g_left_downs.load(std::memory_order_acquire);
    const uint32_t rights = g_right_downs.load(std::memory_order_relaxed);
    const uint32_t backs = g_back_downs.load(std::memory_order_relaxed);
    const int left_clicks = int(lefts - s.last_left);
    const int right_clicks = int(rights - s.last_right) + int(backs - s.last_back);
    s.last_left = lefts;
    s.last_right = rights;
    s.last_back = backs;
    const int wheel_units = g_wheel_units.exchange(0, std::memory_order_relaxed);

    const bool setting = larecomp::settings::menu_mouse.Get();
    const bool kbm_active = larecomp::input::ActiveInput() == larecomp::input::InputDevice::KeyboardMouse;
    const bool over_overlay = mcla::ui::SettingsOpen() || (ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureMouse);
    // A menu screen of the game's: the game is entered, a menu or a movie that is not the road's is up; not the phone, not the full map (it has its own mouse).
    const bool menu_up = larecomp::keyboard::GameEntered() && larecomp::keyboard::MenuScreenUp() && !larecomp::keyboard::PhoneScreenUp() && !MapMouseWantsPointer();

    g_wants_pointer.store(mm::WantsPointer(setting, menu_up, kbm_active, ui->IsFocused()), std::memory_order_relaxed);

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
    const bool dump = larecomp::settings::menu_mouse_dump.Get();
    const bool dump_new = larecomp::settings::menu_mouse_dump_new.Get();
    if (!dump) s.dump_armed = true;
    // With the new-menu dump on, every menu is read when it comes up, not only when the pointer does something (a debug run).
    const bool work = moved || left_clicks || right_clicks || s.wheel.HasStep() || s.pending.active || s.walk_target >= 0 || s.queued || (dump && s.dump_armed) || dump_new;
    if (!work) return;

    MenuView view;
    ReadMenu(b, view);
    const mm::HudRect hud = HudNow(window_w, window_h);
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
        // Where the click's place came from. A click takes its own message's place; when SDL's place on the same event is somewhere else (the case of run 3: the desktop cursor's place, the click
        // 2.2 s after the pointer's last move) the line says both, so the next run shows how often it happens. The last motion event's place is named too when it is not the click's.
        char motion_note[240] = {};
        {
            size_t used = 0;
            const auto add = [&](const char* fmt, int a, int b) {
                if (used + 2 < sizeof(motion_note)) used += size_t(std::max(0, std::snprintf(motion_note + used, sizeof(motion_note) - used, fmt, a, b)));
            };
            int32_t ex, ey, mox, moy;
            Unpack(g_click_event_pos.load(std::memory_order_relaxed), &ex, &ey);
            Unpack(g_motion_pos.load(std::memory_order_relaxed), &mox, &moy);
            if (!g_click_from_message.load(std::memory_order_relaxed)) {
                std::snprintf(motion_note, sizeof(motion_note), "%s",
                    g_hook_buttons.load(std::memory_order_relaxed) == 0 ? " [no click message was seen: the event's place]" : " [the click's message was not fresh: the event's place]");
                used = std::strlen(motion_note);
            } else if (std::abs(ex - cx) > 3 || std::abs(ey - cy) > 3) {
                add(" [SDL's event said window (%d, %d): not used]", ex, ey);
            }
            if (std::abs(mox - cx) > 3 || std::abs(moy - cy) > 3) add(" [the last motion event was at window (%d, %d)]", mox, moy);
        }
        const bool rows_known = view.rows_known && view.selected >= -1;
        const int hit = rows_known ? mm::HitRow(view.layout, csx, csy) : -1;
        mm::Zone zone = mm::Zone::Label;
        bool value_row = false;
        if (rows_known && hit >= 0) {
            value_row = view.kind == Kind::List && IsValueRow(b, view.obj, hit);
            if (const mm::RowBox* box = mm::BoxOfRow(view.layout, hit)) zone = mm::ZoneOf(view.layout, *box, csx, value_row);
        }
        const mm::ClickPlan plan = mm::PlanLeftClick(rows_known, hit, zone, value_row);
        MM_LOG("[menu-mouse] {} left click at window ({}, {}) = stage ({:.1f}, {:.1f}): row {}{} -> {}{}{}", view.movie, cx, cy, csx, csy, hit, value_row ? " (a value row)" : "", ActName(plan.act),
            plan.row >= 0 && plan.row != view.selected ? " after moving the highlight there" : (rows_known ? "" : " (fallback: on the highlighted row)"), motion_note);
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
                    MM_LOG("[menu-mouse] {} hover: row {} (was {}) at stage ({:.1f}, {:.1f})", view.movie, target, view.selected, sx, sy);
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
