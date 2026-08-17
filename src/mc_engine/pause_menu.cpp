#ifndef REXGLUE_HAS_XEO3_TARGET
#include "pause_menu.h"
#include "carbon_parts.h"
#include "logging.h"

#include <atomic>
#include <cctype>
#include <functional>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/input/input.h>
#include <rex/input/input_system.h>
#include <rex/runtime.h>
#include <rex/ppc/function.h>
#include <rex/system/function_dispatcher.h>

REXCVAR_DEFINE_BOOL(show_test_movie, false, "MCLA/Settings",
    "Show the TestMovie debug button in the pause menu")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(garage_tire_offset, true, "MCLA/Patches",
    "Add a TIRE OFFSET row to the garage wheels > dimensions screen, editing "
    "the per-car TireOffset0/1 bytes retail parses but never uses.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

std::atomic<uint32_t> g_pausetab_addr{0};
std::atomic<uint32_t> g_settingsmenu_addr{0};

constexpr uint32_t kStringTableGlobal  = 0x8286D7FC;
constexpr uint32_t kVHSMGlobal         = 0x8286D804;
constexpr uint32_t kAllocStateFn       = 0x8268D578;
constexpr uint32_t kUIObjectCtorFn     = 0x8263AD60;
constexpr uint32_t kUIMenuVtable       = 0x82020654;
constexpr uint32_t kGuestMallocFn      = 0x82130528;
constexpr uint32_t kStackPushFn        = 0x8268F6A8;
constexpr uint32_t kStackPopFn         = 0x8268DFF0;
constexpr uint32_t kIsButtonClickedFn  = 0x82661508;

constexpr uint32_t kFindMovieFn       = 0x821F9FB8;
constexpr uint32_t kSetFlashIntFn     = 0x825EE0E0;
constexpr uint32_t kGetFlashIntFn     = 0x825EE270;
constexpr uint32_t kStringLookupFn    = 0x82218310;
constexpr uint32_t kGetFlashObjFn     = 0x825ED480;
constexpr uint32_t kPopulateMenuFn    = 0x82220308;
constexpr uint32_t kSetFlashStrFn     = 0x827227B8;
constexpr uint32_t kSetFlashPropIntFn = 0x82722678;
constexpr uint32_t kShowMovieClipFn   = 0x82720E28;
constexpr uint32_t kPopulateCallback  = 0x82762488;

// state+12 is the index of the state's active child. Measured against live
// rows: PaintType holds 2 and its active value Metallic is its third child,
// PaintArea holds 0 with Body active, and every leaf holds -1. It is also
// what the enable/disable pair tests first (`parent[3] >= 0` in sub_8268F2E0).
// A row left at -1 has no current value, so the widget draws no `< >` and has
// nothing to move between.
constexpr uint32_t kStateActiveChild  = 12;
// sub_821FA230 — find a UI object by name in the active screen's registry.
constexpr uint32_t kFindUIObjectFn    = 0x821FA230;

// ── Native value rows ───────────────────────────────────────────────────
//
// mcListViewFixed::render (sub_82631FD0) has three row sources and only the
// last of them can draw `< >`:
//
//   list+196  vhsm state    every row is written with type=1, template=56,
//                           count=0 — hard-coded in the renderer. No value
//                           area exists on that path at all, which is what
//                           kept the arrows inside the label text.
//   list+192  table adapter
//   list+176  widget array  the renderer calls row->vfunc196(flashObj) and
//                           then row->vfunc176(), so the row object writes
//                           its own Flash properties.
//
// The `< >` is therefore the row class's own render. sub_82631A20 (vtable
// 0x8208E83C — the class behind SUBTITLES and AUTO SAVE) writes one Flash
// element per value, each element's name being the string-table lookup of
// that value's key, then type/count/select. Same presentation as STEERING
// SENS. (0x82098BFC) but with text values instead of that class's raw ints.
//
// Left and right come for free. The list's input handler (sub_826334D0,
// keys '3'/'5' = left, '4'/'6' = right) forwards to the selected row's
// vfunc32; 0x8208E83C's (sub_826316E0) steps the select at +208 through
// sub_8262FFF0 and the list re-renders itself because the row reports no
// owner (vfunc552 reads +96, which the ctor leaves at 0). Nothing here has
// to read the pad, so the tab-switching that made that approach fail is not
// involved.
constexpr uint32_t kOptionRowCtorFn = 0x826349F0;  // (obj, nameKey, values, wrap)
constexpr uint32_t kLabelRowCtorFn  = 0x82633D40;  // (obj, nameKey, childCap)
constexpr uint32_t kSetRowLabelFn   = 0x8263B860;  // (obj, nameKey) -> resolves +48
constexpr uint32_t kStrListVtable   = 0x82091D04;  // {vtbl, char** keys, count, stride}

// Checkbox row (CONTROLLER presets, cheat list). Like the value class it has
// no constructor of its own — the label ctor runs and the vtable is written
// over it. render sub_82632CA0 adds to the plain label:
//
//   +216 != -1  ->  property1 = +216      (icon / group, -1 leaves it alone)
//   +209 != 0   ->  select = (+208 != 0), type = (+212 ? 4 : 6)
//   +209 == 0   ->  type = 0              (draws as an ordinary label)
//
// vtable slot 8 is the base's stub, so this class has no left/right at all:
// the box is flipped by whoever handles A, which here is the click hook.
constexpr uint32_t kToggleRowVtable = 0x8209788C;
constexpr uint32_t kRowChecked      = 208;  // u8
constexpr uint32_t kRowHasBox       = 209;  // u8, 0 = draw as plain label
constexpr uint32_t kRowEnabled      = 212;  // 0 greys the box out
constexpr uint32_t kRowProperty1    = 216;  // -1 = leave property1 unwritten
// The game's own rows are laid out 240 bytes apart (sub_8264D908 places
// option_subtitles/autosave/arrow at +2848/+3088/+3328); 0x8208E83C's last
// field is the wrap flag at +232.
// (+200 is the value source, 0 meaning "use my children"; +232 is the wrap
// flag. Both are set by the ctor from its arguments.)
constexpr uint32_t kRowObjectSize   = 240;
constexpr uint32_t kRowSelect       = 208;  // current value index

constexpr uint32_t kListRows        = 176;  // row pointer array
constexpr uint32_t kListCountCap    = 180;  // count:u16 then capacity:u16
constexpr uint32_t kListSelect      = 184;
constexpr uint32_t kListTableSource = 192;
constexpr uint32_t kListStateSource = 196;
constexpr uint32_t kListScroll      = 256;  // first visible row (vfunc660)

constexpr uint32_t kStr_PAUSEMOVIE      = 0x8201D028;
constexpr uint32_t kStr_menu            = 0x82030A34;
constexpr uint32_t kStr_tabs_count      = 0x8208D4DC;
constexpr uint32_t kStr_name            = 0x8200CC24;
constexpr uint32_t kStr_select          = 0x8202026C;
constexpr uint32_t kStr_count           = 0x8201D7BC;
constexpr uint32_t kStr_template        = 0x8200CC2C;
constexpr uint32_t kStr_type            = 0x8200CC1C;
constexpr uint32_t kStr_description     = 0x8208D498;
constexpr uint32_t kStr_transition_type = 0x8201CED0;
constexpr uint32_t kStr_FakeMenu        = 0x8209D810;
constexpr uint32_t kStr_TransitionLevel = 0x82020430;
constexpr uint32_t kStr_emptyString     = 0x82000F9E;
constexpr uint32_t kStr_updated         = 0x82024F50;
constexpr uint32_t kStr_update          = 0x8201D7C4;

// ── RexGlue submenu state ──────────────────────────────────────────────

// mcListViewFixed instance driving the pause menu Flash list. +196 =
// optional vhsm-state adapter: when set, the list renders that state's
// children on every re-render. g_last_list tracks the most recently
// populated list; g_pauselist_addr is pinned from it at click time so the
// adapter is only ever forced onto the pause tab's own list.
std::atomic<uint32_t> g_last_list{0};
std::atomic<uint32_t> g_pauselist_addr{0};
std::atomic<uint32_t> g_saved_list_adapter{0};
std::atomic<uint32_t> g_saved_list_select{0};
// The rest of the list's row state, saved alongside the adapter because the
// native rows are installed by swapping the row array itself.
std::atomic<uint32_t> g_saved_list_rows{0};
std::atomic<uint32_t> g_saved_list_countcap{0};
std::atomic<uint32_t> g_saved_list_table{0};
std::atomic<uint32_t> g_saved_list_scroll{0};

// ── Guest memory helpers ────────────────────────────────────────────────

uint8_t* GetMembase() {
    auto* rt = rex::Runtime::instance();
    return rt ? rt->virtual_membase() : nullptr;
}

uint32_t ReadGuestBE32(uint32_t ea) {
    uint8_t* base = GetMembase();
    if (!base) return 0;
    uint8_t* p = base + ea;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8)  | uint32_t(p[3]);
}

void WriteGuestBE32(uint32_t ea, uint32_t v) {
    uint8_t* base = GetMembase();
    if (!base) return;
    uint8_t* p = base + ea;
    p[0] = uint8_t((v >> 24) & 0xFF);
    p[1] = uint8_t((v >> 16) & 0xFF);
    p[2] = uint8_t((v >> 8)  & 0xFF);
    p[3] = uint8_t(v         & 0xFF);
}

uint8_t ReadGuestU8(uint32_t ea) {
    uint8_t* base = GetMembase();
    return base ? base[ea] : 0;
}

void WriteGuestU8(uint32_t ea, uint8_t v) {
    uint8_t* base = GetMembase();
    if (base) base[ea] = v;
}

uint16_t ReadGuestBE16(uint32_t ea) {
    uint8_t* base = GetMembase();
    if (!base) return 0;
    uint8_t* p = base + ea;
    return (uint16_t(p[0]) << 8) | uint16_t(p[1]);
}

// ── String table hash (reimplemented from sub_821C9790) ────────────────

uint32_t MCLAHashString(const char* str) {
    uint32_t hash = 0;
    bool in_quotes = (*str == '"');
    if (in_quotes) ++str;

    while (*str) {
        char c = *str;
        if (in_quotes && c == '"') break;
        ++str;

        uint8_t ch = static_cast<uint8_t>(c);
        if (ch >= 'A' && ch <= 'Z')
            ch += 32;
        else if (ch == '\\')
            ch = '/';

        uint32_t v = ch + hash;
        hash = ((1025u * v) >> 6) ^ (1025u * v);
    }

    return 32769u * (((9u * hash) >> 11) ^ (9u * hash));
}

// ── String table hash-map lookup (reimplemented from sub_826BDDB0) ─────

uint32_t HashMapLookup(uint32_t hashmap_ea, uint32_t hash) {
    uint32_t buckets_ptr = ReadGuestBE32(hashmap_ea);
    uint16_t num_buckets = ReadGuestBE16(hashmap_ea + 4);
    if (!num_buckets || !buckets_ptr) return 0;

    uint32_t idx  = hash % num_buckets;
    uint32_t node = ReadGuestBE32(buckets_ptr + 4 * idx);

    while (node) {
        if (ReadGuestBE32(node) == hash)
            return ReadGuestBE32(node + 4);
        node = ReadGuestBE32(node + 8);
    }
    return 0;
}

// ── Patch a string table entry by overwriting its buffer ───────────────

bool PatchStringTableEntry(const char* key, const char* text) {
    uint32_t table = ReadGuestBE32(kStringTableGlobal);
    if (!table) {
        MC_WARN("[pause-menu] string table pointer is null");
        return false;
    }

    uint32_t hash = MCLAHashString(key);
    uint32_t buf  = HashMapLookup(table + 16, hash);
    if (!buf) {
        MC_WARN("[pause-menu] entry '{}' not found (hash 0x{:08X}, table 0x{:08X})",
                key, hash, table);
        return false;
    }

    uint8_t* base = GetMembase();
    if (!base) return false;
    size_t len = std::strlen(text);
    if (len > 131) len = 131;
    std::memcpy(base + buf, text, len);
    base[buf + len] = 0;

    return true;
}

// ── Linked-list DetachMenuItem ────────────────────────────────────────

void GuestDetachMenuItem(uint32_t child) {
    if (!GetMembase() || !child) return;

    uint32_t parent = ReadGuestBE32(child + 32);
    if (!parent) return;

    uint32_t prev = ReadGuestBE32(child + 40);
    uint32_t next = ReadGuestBE32(child + 36);

    if (prev)
        WriteGuestBE32(prev + 36, next);
    else
        WriteGuestBE32(parent + 44, next);

    if (next)
        WriteGuestBE32(next + 40, prev);

    WriteGuestBE32(child + 32, 0);
    WriteGuestBE32(child + 36, 0);
    WriteGuestBE32(child + 40, 0);
}

// ── Linked-list AppendMenuItem (reimplemented from sub_8268CC80) ────────

void GuestAppendMenuItem(uint32_t parent, uint32_t child) {
    if (!GetMembase() || !parent || !child) return;

    uint32_t child_parent = ReadGuestBE32(child + 32);
    if (child_parent != 0) return;

    uint32_t head = ReadGuestBE32(parent + 44);

    if (head == 0) {
        WriteGuestBE32(parent + 44, child);
        WriteGuestBE32(child + 40, 0);
        WriteGuestBE32(child + 32, parent);
        WriteGuestBE32(child + 36, 0);
    } else {
        uint32_t cur = head;
        for (uint32_t nxt; (nxt = ReadGuestBE32(cur + 36)) != 0; cur = nxt)
            ;
        WriteGuestBE32(cur  + 36, child);
        WriteGuestBE32(child + 40, cur);
        WriteGuestBE32(child + 32, parent);
        WriteGuestBE32(child + 36, 0);
    }
}

// ── Guest PPC function dispatch ───────────────────────────────────────

uint32_t CallGuestFn1(uint32_t fn_addr, uint32_t arg0) {
    auto* rt = rex::Runtime::instance();
    if (!rt || !rt->function_dispatcher()) return 0;
    PPCFunc* fn = rt->function_dispatcher()->GetFunction(fn_addr);
    if (!fn) {
        MC_WARN("[pause-menu] guest fn 0x{:08X} not in dispatcher", fn_addr);
        return 0;
    }
    return rex::ppc::GuestToHostFunction<uint32_t>(fn, arg0);
}

uint32_t CallGuestFn2(uint32_t fn_addr, uint32_t a0, uint32_t a1) {
    auto* rt = rex::Runtime::instance();
    if (!rt || !rt->function_dispatcher()) return 0;
    PPCFunc* fn = rt->function_dispatcher()->GetFunction(fn_addr);
    if (!fn) return 0;
    return rex::ppc::GuestToHostFunction<uint32_t>(fn, a0, a1);
}

uint32_t CallGuestFn4(uint32_t fn_addr, uint32_t a0, uint32_t a1, uint32_t a2,
                      uint32_t a3) {
    auto* rt = rex::Runtime::instance();
    if (!rt || !rt->function_dispatcher()) return 0;
    PPCFunc* fn = rt->function_dispatcher()->GetFunction(fn_addr);
    if (!fn) return 0;
    return rex::ppc::GuestToHostFunction<uint32_t>(fn, a0, a1, a2, a3);
}

uint32_t CallGuestFn3(uint32_t fn_addr, uint32_t a0, uint32_t a1, uint32_t a2) {
    auto* rt = rex::Runtime::instance();
    if (!rt || !rt->function_dispatcher()) return 0;
    PPCFunc* fn = rt->function_dispatcher()->GetFunction(fn_addr);
    if (!fn) return 0;
    return rex::ppc::GuestToHostFunction<uint32_t>(fn, a0, a1, a2);
}

uint32_t AllocGuestString(const char* str) {
    uint8_t* base = GetMembase();
    if (!base) return 0;
    size_t len = std::strlen(str) + 1;
    uint32_t buf = CallGuestFn1(kGuestMallocFn, static_cast<uint32_t>(len));
    if (buf) std::memcpy(base + buf, str, len);
    return buf;
}

bool IsGuestButtonClicked(uint32_t guest_key_addr) {
    if (!guest_key_addr) return false;
    auto* rt = rex::Runtime::instance();
    if (!rt || !rt->function_dispatcher()) return false;
    PPCFunc* fn = rt->function_dispatcher()->GetFunction(kIsButtonClickedFn);
    if (!fn) return false;
    return rex::ppc::GuestToHostFunction<uint32_t>(fn, guest_key_addr, 1u) != 0;
}

// ── VirtualHSM engine access ─────────────────────────────────────────

uint32_t GetRootNode() {
    uint32_t root = ReadGuestBE32(kVHSMGlobal);
    if (!root) return 0;
    return ReadGuestBE32(root + 52);
}

uint32_t GetEnginePtr() {
    uint32_t rootNode = GetRootNode();
    if (!rootNode) return 0;
    return rootNode + 4;
}


bool GuestStackPush(uint32_t statePtr) {
    uint32_t rootNode = GetRootNode();
    if (!rootNode) return false;
    uint32_t navStack = rootNode + 776;
    uint32_t engine   = rootNode + 4;

    auto* rt = rex::Runtime::instance();
    if (!rt || !rt->function_dispatcher()) return false;
    PPCFunc* fn = rt->function_dispatcher()->GetFunction(kStackPushFn);
    if (!fn) return false;

    rex::ppc::GuestToHostFunction<uint32_t>(fn, navStack, engine, statePtr, 0u);
    MC_INFO("[pause-menu] stackPush state 0x{:08X}", statePtr);
    return true;
}

bool GuestStackPop() {
    uint32_t rootNode = GetRootNode();
    if (!rootNode) return false;
    uint32_t navStack = rootNode + 776;
    uint32_t engine   = rootNode + 4;

    auto* rt = rex::Runtime::instance();
    if (!rt || !rt->function_dispatcher()) return false;
    PPCFunc* fn = rt->function_dispatcher()->GetFunction(kStackPopFn);
    if (!fn) return false;

    uint32_t newTop = rex::ppc::GuestToHostFunction<uint32_t>(fn, navStack, engine, 1u);
    MC_INFO("[pause-menu] stackPop -> top 0x{:08X}", newTop);
    return true;
}

// ── Hash map insertion (reimplemented from sub_82389998) ──────────────

bool InsertHashMapEntry(uint32_t hashmap_ea, uint32_t hash, uint32_t value) {
    uint32_t buckets = ReadGuestBE32(hashmap_ea);
    uint16_t num_buckets = ReadGuestBE16(hashmap_ea + 4);
    if (!num_buckets || !buckets) return false;

    uint32_t node = CallGuestFn1(kGuestMallocFn, 12);
    if (!node) return false;

    WriteGuestBE32(node + 0, hash);
    WriteGuestBE32(node + 4, value);

    uint32_t bucket_idx = hash % num_buckets;
    uint32_t old_head = ReadGuestBE32(buckets + bucket_idx * 4);
    WriteGuestBE32(node + 8, old_head);
    WriteGuestBE32(buckets + bucket_idx * 4, node);
    return true;
}

// ── Create a new entry in MCLA's string table ─────────────────────────

bool CreateStringTableEntry(const char* key, const char* text) {
    uint32_t table = ReadGuestBE32(kStringTableGlobal);
    if (!table) return false;

    uint8_t* base = GetMembase();
    if (!base) return false;

    uint32_t text_buf = CallGuestFn1(kGuestMallocFn, 132);
    if (!text_buf) return false;

    size_t len = std::strlen(text);
    if (len > 131) len = 131;
    std::memcpy(base + text_buf, text, len);
    base[text_buf + len] = 0;

    uint32_t hash = MCLAHashString(key);
    if (!InsertHashMapEntry(table + 16, hash, text_buf)) return false;

    return true;
}

void EnsureStringTableText(const char* key, const char* text) {
    if (!PatchStringTableEntry(key, text))
        CreateStringTableEntry(key, text);
}

// ── Allocate a new vhsmState via the engine factory ───────────────────

uint32_t CreateNewMenuState(const char* name) {
    uint32_t engine = GetEnginePtr();
    if (!engine) {
        MC_WARN("[pause-menu] engine pointer not available");
        return 0;
    }

    uint32_t state = CallGuestFn1(kAllocStateFn, engine);
    if (!state) {
        MC_WARN("[pause-menu] AllocState failed");
        return 0;
    }

    uint32_t count = ReadGuestBE32(engine + 80);
    uint32_t state_index = count - 1;

    uint32_t hash = MCLAHashString(name);
    InsertHashMapEntry(engine + 88, hash, state_index);

    uint8_t* base = GetMembase();
    size_t name_len = std::strlen(name) + 1;
    uint32_t name_buf = CallGuestFn1(kGuestMallocFn, static_cast<uint32_t>(name_len));
    if (name_buf && base) {
        std::memcpy(base + name_buf, name, name_len);
        WriteGuestBE32(state + 20, name_buf);
    }

    WriteGuestBE32(state + 16, 0x42);
    WriteGuestBE32(state + 24, 0);
    WriteGuestBE32(state + 28, 0);
    WriteGuestBE32(state + 32, 0);
    WriteGuestBE32(state + 36, 0);
    WriteGuestBE32(state + 40, 0);
    WriteGuestBE32(state + 44, 0);

    return state;
}

// Replicates the class factory's "UIMenu" branch (sub_82222080):
// malloc(56) + UIObject ctor (sub_8263AD60) + UIMenu vftable, then registers
// the state in the engine array/hash map the same way kAllocStateFn does.
uint32_t CreateUIMenuState(const char* name) {
    uint32_t engine = GetEnginePtr();
    if (!engine) return 0;

    uint32_t state = CallGuestFn1(kGuestMallocFn, 56);
    if (!state) return 0;

    CallGuestFn1(kUIObjectCtorFn, state);
    WriteGuestBE32(state, kUIMenuVtable);

    uint32_t arr = ReadGuestBE32(engine + 76);
    uint32_t cnt = ReadGuestBE32(engine + 80);
    if (!arr) return 0;
    WriteGuestBE32(arr + 4 * cnt, state);
    WriteGuestBE32(engine + 80, cnt + 1);

    uint32_t hash = MCLAHashString(name);
    InsertHashMapEntry(engine + 88, hash, cnt);

    uint8_t* base = GetMembase();
    size_t name_len = std::strlen(name) + 1;
    uint32_t name_buf = CallGuestFn1(kGuestMallocFn, static_cast<uint32_t>(name_len));
    if (name_buf && base) {
        std::memcpy(base + name_buf, name, name_len);
        WriteGuestBE32(state + 20, name_buf);
    }

    return state;
}

// ── Pause list control ────────────────────────────────────────────────

uint32_t GetPauseMovieFlashCtx() {
    uint32_t root = ReadGuestBE32(kVHSMGlobal);
    if (!root) return 0;
    uint32_t movieObj = CallGuestFn2(kFindMovieFn, root, kStr_PAUSEMOVIE);
    if (!movieObj) return 0;
    return ReadGuestBE32(movieObj + 56);
}

void SetMenuTitle(uint32_t titleStr) {
    if (!titleStr) return;
    uint32_t flashCtx = GetPauseMovieFlashCtx();
    if (!flashCtx) return;
    uint32_t menuHash = MCLAHashString("menu");
    uint32_t menuObj = CallGuestFn3(kGetFlashObjFn, flashCtx, menuHash, kStr_menu);
    if (menuObj) CallGuestFn3(kSetFlashStrFn, menuObj, kStr_name, titleStr);
}

// Re-render the pause list through its own vtable[44] (sub_826326D0 →
// sub_82631FD0). With list+196 pointing at one of our menus, the list renders
// its children; with it restored, it renders the current tab again.
void RefreshPauseList() {
    uint32_t list = g_pauselist_addr.load(std::memory_order_relaxed);
    if (!list) {
        MC_WARN("[pause-menu] pause list not captured yet");
        return;
    }
    uint32_t vtable = ReadGuestBE32(list);
    uint32_t fn = vtable ? ReadGuestBE32(vtable + 176) : 0;
    if (fn) CallGuestFn1(fn, list);
}

void RestoreTabDisplay(uint32_t controller) {
    (void)controller;

    uint32_t list = g_pauselist_addr.load(std::memory_order_relaxed);
    if (list) {
        WriteGuestBE32(list + kListRows,
                       g_saved_list_rows.load(std::memory_order_relaxed));
        WriteGuestBE32(list + kListCountCap,
                       g_saved_list_countcap.load(std::memory_order_relaxed));
        WriteGuestBE32(list + kListTableSource,
                       g_saved_list_table.load(std::memory_order_relaxed));
        WriteGuestBE32(list + kListStateSource,
                       g_saved_list_adapter.load(std::memory_order_relaxed));
        WriteGuestBE32(list + kListSelect,
                       g_saved_list_select.load(std::memory_order_relaxed));
        WriteGuestBE32(list + kListScroll,
                       g_saved_list_scroll.load(std::memory_order_relaxed));
        RefreshPauseList();

    }
    MC_INFO("[pause-menu] tab display restored");
}

// ── CVar access by name (uniform for larecomp AND SDK cvars) ───────────

std::string CvarGet(const char* name) {
    return rex::cvar::GetFlagByName(name);
}

bool CvarGetBool(const char* name) {
    return rex::cvar::GetFlagByName(name) == "true";
}

double CvarGetDouble(const char* name) {
    return std::strtod(rex::cvar::GetFlagByName(name).c_str(), nullptr);
}

void CvarSet(const char* name, const char* value) {
    rex::cvar::SetFlagByName(name, value);
    MC_INFO("[pause-menu] cvar '{}' -> '{}'", name, value);
}

void CvarSetDouble(const char* name, double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g", v);
    CvarSet(name, buf);
}

// ── Item / menu tables ─────────────────────────────────────────────────
//
// Each submenu is a table of ItemDef; the Flash list's selected index maps
// straight into the table, so item order in the list = table order. A click
// cycles the item's cvar to its next value; labels are rebuilt from the live
// cvar value on every render, so F4-side changes show up too.

enum class ItemKind { kBool, kStrCycle, kDblCycle, kSave, kCarbonBit };

struct ItemDef {
    const char* key;      // guest state name + string-table key (unique!)
    ItemKind kind;
    const char* cvar;     // ignored for kSave
    const char* prefix;   // label prefix, e.g. "FULLSCREEN: "
    const char* suffix;   // e.g. " (RESTART)"
    bool inverted;        // kBool: cvar true renders as OFF (disable_* cvars)
    const char* const* svals;   // kStrCycle: cvar values
    const char* const* slabels; // kStrCycle: display labels (parallel)
    int nvals;                  // kStrCycle / kDblCycle count
    const double* dvals;        // kDblCycle values
    const char* dfmt;           // kDblCycle value format, e.g. "%.1fX"
    // kDblCycle: label to show instead of the formatted number when the value
    // is 0. Lets a numeric row carry an "off / leave it alone" entry without
    // rendering it as a bare "0". Null = format 0 like any other value.
    const char* zero_label;
};

constexpr ItemDef Bool(const char* key, const char* cvar, const char* prefix,
                       bool inverted = false, const char* suffix = "") {
    return {key, ItemKind::kBool, cvar, prefix, suffix, inverted,
            nullptr, nullptr, 0, nullptr, nullptr};
}

constexpr ItemDef Str(const char* key, const char* cvar, const char* prefix,
                      const char* const* vals, const char* const* labels, int n,
                      const char* suffix = "") {
    return {key, ItemKind::kStrCycle, cvar, prefix, suffix, false,
            vals, labels, n, nullptr, nullptr};
}

constexpr ItemDef Dbl(const char* key, const char* cvar, const char* prefix,
                      const double* vals, int n, const char* fmt,
                      const char* suffix = "", const char* zero_label = nullptr) {
    return {key, ItemKind::kDblCycle, cvar, prefix, suffix, false,
            nullptr, nullptr, n, vals, fmt, zero_label};
}

constexpr ItemDef Save(const char* key) {
    return {key, ItemKind::kSave, nullptr, "", "", false,
            nullptr, nullptr, 0, nullptr, nullptr};
}

// Carbon fiber part group. Unlike every other item here this one is NOT
// backed by a cvar: it flips a bit in the customization data of the car the
// player is driving, so each car carries its own selection and it persists
// in the save on its own. `nvals` carries the group bit.
constexpr ItemDef Carbon(const char* key, uint8_t group, const char* prefix,
                         const char* suffix = "") {
    return {key, ItemKind::kCarbonBit, nullptr, prefix, suffix, false,
            nullptr, nullptr, group, nullptr, nullptr};
}

// ── Value tables ───────────────────────────────────────────────────────

constexpr const char* kResVals[]   = {"", "720p", "1080p", "1440p", "4k"};
constexpr const char* kResNames[]  = {"DEFAULT", "720P", "1080P", "1440P", "4K"};
constexpr double kResScaleVals[]   = {1.0, 2.0, 3.0, 4.0};

constexpr const char* kEffectVals[]  = {"bilinear", "cas", "fsr", "fsr2", "fsr3"};
constexpr const char* kEffectNames[] = {"BILINEAR", "CAS", "FSR 1", "FSR 2", "FSR 3"};
constexpr const char* kFsrQVals[]  = {"auto", "nativeaa", "quality", "balanced",
                                      "performance", "ultra_performance"};
constexpr const char* kFsrQNames[] = {"AUTO", "NATIVE AA", "QUALITY", "BALANCED",
                                      "PERFORMANCE", "ULTRA PERF"};
constexpr double kCasSharpVals[] = {0.0, 0.25, 0.5, 0.75, 1.0};
constexpr double kFsrSharpVals[] = {0.0, 0.2, 0.5, 1.0, 2.0};

constexpr const char* kFreecamVals[]  = {"off", "free"};
constexpr const char* kFreecamNames[] = {"OFF", "ON"};
constexpr double kCamSpeedVals[] = {10.0, 20.0, 40.0, 80.0, 150.0, 300.0};

constexpr double kFovValues[] = {0.8, 0.9, 1.0, 1.1, 1.2, 1.3, 1.4,
                                 1.5, 1.6, 1.7, 1.8, 1.9, 2.0};
constexpr double kLodValues[] = {0.1, 0.5, 1.0, 2.0, 5.0, 10.0};

// Performance. Traffic/ped/parked mirror the ranges the cvars declare; the
// fragment-tune tables bracket the two values the engine itself uses — 250 is
// the fragTuneStruct constructor default and 3000 is what the shipped tune file
// loads. 0 is "leave the tune file alone" and renders as STOCK.
constexpr double kUnspawnVals[]      = {100.0, 150.0, 200.0, 250.0, 300.0,
                                        400.0, 500.0, 600.0};
constexpr double kDensityVals[]      = {0.0, 0.25, 0.5, 0.75, 1.0, 1.5, 2.0};
constexpr double kFragDrawDistVals[] = {0.0, 250.0, 500.0, 1000.0, 1500.0,
                                        2000.0, 3000.0, 4000.0};
constexpr double kBreakFpsVals[]     = {0.0, 5.0, 10.0, 15.0, 20.0, 30.0, 60.0};

// Which render phase bits SHADOWS clears.
//
// STOCK is what the game's own `noshadows` switch clears: the sun cascade
// phases 0x20/0x40/0x80/0x100 plus 0x2000/0x4000. Measured at midnight those
// are not even in renderer+364, so clearing them does nothing — the shadow that
// is actually drawn comes from phase 0x400, which sub_823112C0 routes to the
// shadowNight / shadowFastBlend technique group and which `noshadows` leaves
// alone. Hence BLEND.
//
// ALL also takes 0x10/0x200/0x800, which are live passes unrelated to shadows;
// it corrupts the frame and is only useful for narrowing a phase down.
constexpr const char* kShadowBitVals[]  = {"0x61E0", "0x400", "0x65E0", "0x7DF0"};
constexpr const char* kShadowBitNames[] = {"STOCK", "BLEND ONLY", "STOCK+BLEND", "ALL (BREAKS)"};

// Time of day. The four the game's own TodMenu uses (sunrise 6.0, afternoon
// 16.75, sunset 17.4, night 23.5) plus round hours to fill the day out.
constexpr double kTodValues[] = {0.0, 3.0, 6.0, 9.0, 12.0, 14.0,
                                 16.75, 17.4, 19.0, 21.0, 23.5};
constexpr double kTodSpeedVals[] = {1.0, 2.0, 3.0, 5.0, 10.0};

// Weather indices as the engine orders them (name table at 0x827E1B00).
constexpr const char* kWeatherVals[]  = {"game", "real", "nice", "cloudy",
                                         "stormy", "foggy"};
constexpr const char* kWeatherNames[] = {"GAME", "REAL (LIVE)", "NICE", "CLOUDY",
                                         "STORMY", "FOGGY"};

// ── Menu contents ──────────────────────────────────────────────────────

const ItemDef kVideoItems[] = {
    Bool("PM_RxFullscreen", "fullscreen", "FULLSCREEN: "),
    Bool("PM_RxVsync",      "vsync",      "VSYNC: "),
    Str ("PM_RxResolution", "resolution", "RESOLUTION: ",
         kResVals, kResNames, 5, " (RESTART)"),
    Dbl ("PM_RxResScale",   "resolution_scale", "RES SCALE: ",
         kResScaleVals, 4, "%gX", " (RESTART)"),
    Save("PM_RxSaveVideo"),
};

const ItemDef kRecompItems[] = {
    Bool("PM_RxMsaa",        "disable_msaa",             "MSAA: ",  true, " (RESTART)"),
    Bool("PM_RxFoliage",     "disable_imposter_shadows", "FOLIAGE SHADOWS: ", true, " (RESTART)"),
    Bool("PM_RxRubber",      "disable_rubberbanding",    "AI RUBBERBAND: ",   true, " (RESTART)"),
    Bool("PM_RxVinylLayers", "extra_vinyl_layers",       "EXTRA VINYL LAYERS: ", false, " (RESTART)"),
    Bool("PM_RxFps60",       "fps_60",                   "60 FPS: "),
    Bool("PM_RxDof",         "disable_dof",              "DEPTH OF FIELD: ", true),
    Bool("PM_RxBlur",        "disable_motion_blur",      "MOTION BLUR: ",    true),
    // BadassBaboon's Recomp Adjustments: in-game pause menu options
    Bool("PM_RxSteerFps",    "scale_steering_with_fps",  "60FPS STEERING FIX: "),
    Dbl ("PM_RxLodTraffic",  "lod_traffic_scale", "TRAFFIC LOD: ", kLodValues, 6, "%gX"),
    Dbl ("PM_RxLodCity",     "lod_city_scale",    "CITY LOD: ",    kLodValues, 6, "%gX"),
    Save("PM_RxSaveRecomp"),
};

// Everything that trades image quality for framerate, in one place.
//
// The four SHADOWS/IMPOSTORS/FOLIAGE/SCREEN BLUR toggles reactivate the game's
// own dev switches (noshadows, noimpostors, notrees, nofsblur). They are read
// once, during renderer init, so they only take effect on a restart — the
// suffix says so on the row.
//
// PROP DRAW DIST and BREAK FPS FLOOR patch rage::fragTuneStruct in place after
// the game has parsed $/tune/types/fragments, so they apply live. "Prop" here
// means a breakable fragment (pole, sign, fence, barrier), not the whole world.
const ItemDef kPerfItems[] = {
    Bool("PM_RxNoShadows",   "perf_no_shadows",           "SHADOWS: ", true),
    Str ("PM_RxShadowBits",  "perf_shadow_phase_bits",    "SHADOW PHASES: ",
         kShadowBitVals, kShadowBitNames, 4),
    Bool("PM_RxFastVehShad", "perf_fast_vehicle_shadows", "CHEAP CAR SHADOW: ", false, " (RESTART)"),
    // Load-time switches: noimpostors skips allocating the impostor render
    // targets, notrees skips the prop parse. node+4 is kept in sync with the
    // cvar every frame, so a change lands the next time that system loads —
    // a district change — rather than needing the process restarted.
    Bool("PM_RxNoImpostors", "perf_no_impostors",         "TREE IMPOSTORS: ", true, " (ON RELOAD)"),
    Bool("PM_RxNoTrees",     "perf_no_trees",             "FOLIAGE: ", true, " (ON RELOAD)"),
    Bool("PM_RxNoFsBlur",    "perf_no_fullscreen_blur",   "SCREEN BLUR: ", true, " (RESTART)"),
    Bool("PM_RxSingleTile",  "single_tile",               "SINGLE TILE: "),
    // BadassBaboon's Recomp Adjustments: city ambient density.
    Bool("PM_RxAmbientTune", "enable_ambient_tuning",     "CITY AMBIENT CULLING: "),
    Dbl ("PM_RxUnspawn",     "traffic_unspawn_dist", "TRAFFIC RANGE: ",
         kUnspawnVals, int(sizeof(kUnspawnVals) / sizeof(kUnspawnVals[0])), "%gM"),
    Dbl ("PM_RxPedDensity",  "ped_density_scale",    "PEDESTRIANS: ",
         kDensityVals, int(sizeof(kDensityVals) / sizeof(kDensityVals[0])), "%gX"),
    Dbl ("PM_RxParkedCars",  "parked_car_scale",     "PARKED CARS: ",
         kDensityVals, int(sizeof(kDensityVals) / sizeof(kDensityVals[0])), "%gX"),
    Dbl ("PM_RxFragDist",    "global_max_draw_distance", "PROP DRAW DIST: ",
         kFragDrawDistVals, int(sizeof(kFragDrawDistVals) / sizeof(kFragDrawDistVals[0])),
         "%gM", "", "STOCK"),
    Dbl ("PM_RxBreakFps",    "breaking_frame_rate_limit", "BREAK FPS FLOOR: ",
         kBreakFpsVals, int(sizeof(kBreakFpsVals) / sizeof(kBreakFpsVals[0])),
         "%g", "", "STOCK"),
    Save("PM_RxSavePerf"),
};

const ItemDef kFfxItems[] = {
    Str ("PM_RxUpscaler",   "present_effect", "UPSCALER: ",
         kEffectVals, kEffectNames, 5),
    Str ("PM_RxFsrQuality", "present_fsr_quality_mode", "FSR QUALITY: ",
         kFsrQVals, kFsrQNames, 6),
    Dbl ("PM_RxCasSharp",   "present_cas_additional_sharpness",
         "CAS SHARPNESS: ", kCasSharpVals, 5, "%g"),
    Dbl ("PM_RxFsrSharp",   "present_fsr_sharpness_reduction",
         "FSR SHARP REDUCE: ", kFsrSharpVals, 5, "%g"),
    Save("PM_RxSaveFfx"),
};

const ItemDef kCamItems[] = {
    // BadassBaboon's Recomp Adjustments: in-game pause menu smooth chase camera toggle
    Bool("PM_RxSmoothCam", "smooth_chase_cam", "SMOOTH CHASE CAM: "),
    Dbl ("PM_RxFov1P",    "fov_1p_scale", "1P FOV: ", kFovValues, 13, "%.1fX"),
    Dbl ("PM_RxFov3P",    "fov_3p_scale", "3P FOV: ", kFovValues, 13, "%.1fX"),
    Str ("PM_RxFreecam",  "debug_cam", "FREECAM: ", kFreecamVals, kFreecamNames, 2),
    Dbl ("PM_RxCamSpeed", "debug_cam_speed", "CAM SPEED: ", kCamSpeedVals, 6, "%g"),
    Save("PM_RxSaveCam"),
};

const ItemDef kTodItems[] = {
    // Listed in the order they outrank each other: real clock beats a held
    // hour, a held hour beats the day-cycle speed.
    Bool("PM_RxTodReal",  "time_of_day_realtime", "REAL CLOCK: "),
    Bool("PM_RxTodHold",  "time_of_day_hold",  "HOLD TIME: "),
    Dbl ("PM_RxTodHour",  "time_of_day",       "TIME: ",
         kTodValues, int(sizeof(kTodValues) / sizeof(kTodValues[0])), "%gH"),
    Dbl ("PM_RxTodSpeed", "time_of_day_speed", "DAY SPEED: ",
         kTodSpeedVals, int(sizeof(kTodSpeedVals) / sizeof(kTodSpeedVals[0])), "%gX"),
    Str ("PM_RxWeather",  "weather", "WEATHER: ",
         kWeatherVals, kWeatherNames,
         int(sizeof(kWeatherVals) / sizeof(kWeatherVals[0]))),
    Save("PM_RxSaveTod"),
};

// Carbon fiber parts. The retail game shipped the carbon code but not the
// menu entry that reaches it, so this submenu is the only way in. Every item
// edits the car you are driving — the selection lives in that car's own
// customization data and goes into the save with it, so each car keeps its
// own. Nothing here is a global setting and SAVE SETTINGS is not needed.
//
// ROOF is labelled for what it actually does: a convertible's soft top has no
// carbon technique and disappears instead of turning carbon. TRUNK is
// per-model too — cars whose trunk shader lacks the technique stay painted.
const ItemDef kCarbonItems[] = {
    Carbon("PM_RxCarbonHood",    kCarbonHood,    "HOOD: "),
    Carbon("PM_RxCarbonTrunk",   kCarbonTrunk,   "TRUNK: "),
    Carbon("PM_RxCarbonDoors",   kCarbonDoors,   "DOORS: "),
    Carbon("PM_RxCarbonRoof",    kCarbonRoof,    "ROOF: ", " (DROPS SOFT TOP)"),
    Carbon("PM_RxCarbonBumpers", kCarbonBumpers, "BUMPERS: "),
    Carbon("PM_RxCarbonBody",    kCarbonBody,    "FENDERS & SKIRTS: "),
    Carbon("PM_RxCarbonSpoiler", kCarbonSpoiler, "SPOILER: "),
    Carbon("PM_RxCarbonExtras",  kCarbonExtras,  "GRILL & INTERCOOLER: "),
};

struct MenuDef {
    const char* btn_key;   // button state name + its string-table key
    const char* label;     // button label AND submenu title
    const char* menu_key;  // UIMenu state name
    const ItemDef* items;
    int num_items;
};

const MenuDef kMenus[] = {
    {"PM_RxTabVideo",  "REXGLUE SETTINGS", "RxVideoMenu",
     kVideoItems,  int(sizeof(kVideoItems)  / sizeof(kVideoItems[0]))},
    {"PM_RxTabRecomp", "RECOMP SETTINGS",  "RxRecompMenu",
     kRecompItems, int(sizeof(kRecompItems) / sizeof(kRecompItems[0]))},
    {"PM_RxTabPerf",   "PERFORMANCE",      "RxPerfMenu",
     kPerfItems,   int(sizeof(kPerfItems)   / sizeof(kPerfItems[0]))},
    {"PM_RxTabFfx",    "FIDELITY FX",      "RxFfxMenu",
     kFfxItems,    int(sizeof(kFfxItems)    / sizeof(kFfxItems[0]))},
    {"PM_RxTabCam",    "DEBUG CAMERA",     "RxCamMenu",
     kCamItems,    int(sizeof(kCamItems)    / sizeof(kCamItems[0]))},
    {"PM_RxTabTod",    "TIME OF DAY",      "RxTodMenu",
     kTodItems,    int(sizeof(kTodItems)    / sizeof(kTodItems[0]))},
    {"PM_RxTabCarbon", "CARBON FIBER",     "RxCarbonMenu",
     kCarbonItems, int(sizeof(kCarbonItems) / sizeof(kCarbonItems[0]))},
};
constexpr int kNumMenus = int(sizeof(kMenus) / sizeof(kMenus[0]));

// Per-menu runtime state (guest addresses).
std::atomic<uint32_t> g_menu_btn_key[kNumMenus] = {};  // guest key string for IsButtonClicked
std::atomic<uint32_t> g_menu_state[kNumMenus]   = {};  // UIMenu guest state
std::atomic<uint32_t> g_menu_title[kNumMenus]   = {};  // guest title string

// Pause-menu probe: armed only while the pause menu is actually being shown.
std::atomic<bool> g_pm_probe_armed{false};
uint32_t g_pm_seen[64] = {};
int g_pm_seen_count = 0;
std::atomic<int>  g_active_menu{-1};                   // -1 = not in any submenu
std::atomic<bool> g_menus_created{false};
std::atomic<bool> g_settings_saved{false};             // "SETTINGS SAVED!" label state

// ── Labels & clicks ────────────────────────────────────────────────────

int FindClosestIndex(double value, const double* table, int count) {
    int best = 0;
    double bestDiff = -1.0;
    for (int i = 0; i < count; ++i) {
        double d = value > table[i] ? value - table[i] : table[i] - value;
        if (bestDiff < 0.0 || d < bestDiff) {
            bestDiff = d;
            best = i;
        }
    }
    return best;
}

int FindStringIndex(const std::string& value, const char* const* vals, int count) {
    for (int i = 0; i < count; ++i)
        if (value == vals[i]) return i;
    return 0;
}

// ── Item value model ───────────────────────────────────────────────────
//
// A native row keeps the label and the value apart: the label is the row's
// own text (widget+48, resolved from the string table by the ctor) and the
// values are a separate list the row steps through. So the prefix loses its
// trailing ": " and the value moves out of the label entirely.

// A plain on/off setting is a checkbox, not a two-entry value list — that is
// what SUBTITLES and AUTO SAVE are. Carbon stays a value row: PAINT and
// CARBON are two named finishes rather than a thing being on or off, and the
// per-part control is already working that way.
bool ItemIsToggle(const ItemDef& it) { return it.kind == ItemKind::kBool; }

int ItemValueCount(const ItemDef& it) {
    switch (it.kind) {
    case ItemKind::kBool:
    case ItemKind::kCarbonBit: return 2;
    case ItemKind::kStrCycle:
    case ItemKind::kDblCycle:  return it.nvals;
    case ItemKind::kSave:      return 0;
    }
    return 0;
}

std::string ItemValueLabel(const ItemDef& it, int i) {
    switch (it.kind) {
    case ItemKind::kBool:      return i ? "ON" : "OFF";
    case ItemKind::kCarbonBit: return i ? "CARBON" : "PAINT";
    case ItemKind::kStrCycle:  return it.slabels[i];
    case ItemKind::kDblCycle: {
        if (it.zero_label && it.dvals[i] == 0.0) return it.zero_label;
        char buf[48];
        std::snprintf(buf, sizeof(buf), it.dfmt, it.dvals[i]);
        return buf;
    }
    case ItemKind::kSave: break;
    }
    return "";
}

// Which value the live cvar (or, for carbon, the car itself) is currently on.
int ItemValueIndex(const ItemDef& it) {
    switch (it.kind) {
    case ItemKind::kBool: {
        bool on = CvarGetBool(it.cvar);
        if (it.inverted) on = !on;
        return on ? 1 : 0;
    }
    case ItemKind::kCarbonBit:
        return (CarbonHaveCar() && CarbonHasGroup(uint8_t(it.nvals))) ? 1 : 0;
    case ItemKind::kStrCycle:
        return FindStringIndex(CvarGet(it.cvar), it.svals, it.nvals);
    case ItemKind::kDblCycle:
        return FindClosestIndex(CvarGetDouble(it.cvar), it.dvals, it.nvals);
    case ItemKind::kSave: break;
    }
    return 0;
}

// Push a value index the row has already moved to back into the cvar.
void ItemSetValueIndex(const ItemDef& it, int i) {
    const int n = ItemValueCount(it);
    if (n <= 0 || i < 0 || i >= n) return;
    switch (it.kind) {
    case ItemKind::kBool: {
        bool on = (i != 0);
        if (it.inverted) on = !on;
        CvarSet(it.cvar, on ? "true" : "false");
        g_settings_saved.store(false, std::memory_order_relaxed);
        break;
    }
    case ItemKind::kStrCycle:
        CvarSet(it.cvar, it.svals[i]);
        g_settings_saved.store(false, std::memory_order_relaxed);
        break;
    case ItemKind::kDblCycle:
        CvarSetDouble(it.cvar, it.dvals[i]);
        g_settings_saved.store(false, std::memory_order_relaxed);
        break;
    case ItemKind::kCarbonBit:
        // Goes straight into the car's customization data, which is its own
        // source of truth — only touch it when it actually disagrees.
        if (CarbonHaveCar() && (CarbonHasGroup(uint8_t(it.nvals)) ? 1 : 0) != i)
            CarbonToggleGroup(uint8_t(it.nvals));
        break;
    case ItemKind::kSave:
        break;
    }
}

// The row's own text. Carbon rows say so when there is no car to edit, since
// their value list would otherwise claim a state the car does not have.
std::string ItemRowLabel(const ItemDef& it) {
    if (it.kind == ItemKind::kSave)
        return g_settings_saved.load(std::memory_order_relaxed)
                   ? "SETTINGS SAVED!" : "SAVE SETTINGS";

    std::string label = it.prefix;
    while (!label.empty() && (label.back() == ' ' || label.back() == ':'))
        label.pop_back();

    if (it.kind == ItemKind::kCarbonBit && !CarbonHaveCar())
        return label + " (NO CAR)";
    return label + it.suffix;
}

// dir is +1 for A / right and -1 for left. The pause list is a real
// mcListViewFixed whose select index at +184 tracks the highlight, so a value
// can be stepped without the item having to be re-entered.
void ApplyItem(const ItemDef& it, int dir) {
    switch (it.kind) {
    case ItemKind::kBool:
        CvarSet(it.cvar, CvarGetBool(it.cvar) ? "false" : "true");
        g_settings_saved.store(false, std::memory_order_relaxed);
        break;
    case ItemKind::kStrCycle: {
        int i = FindStringIndex(CvarGet(it.cvar), it.svals, it.nvals);
        CvarSet(it.cvar, it.svals[(i + dir + it.nvals) % it.nvals]);
        g_settings_saved.store(false, std::memory_order_relaxed);
        break;
    }
    case ItemKind::kDblCycle: {
        int i = FindClosestIndex(CvarGetDouble(it.cvar), it.dvals, it.nvals);
        CvarSetDouble(it.cvar, it.dvals[(i + dir + it.nvals) % it.nvals]);
        g_settings_saved.store(false, std::memory_order_relaxed);
        break;
    }
    case ItemKind::kCarbonBit:
        // Goes straight into the car's customization data — the game's own
        // save carries it, so there is nothing to write to larecomp.toml.
        CarbonToggleGroup(uint8_t(it.nvals));
        break;
    case ItemKind::kSave: {
        auto config_path =
            rex::filesystem::GetExecutableFolder() / "larecomp.toml";
        rex::cvar::SaveConfig(config_path);
        g_settings_saved.store(true, std::memory_order_relaxed);
        MC_INFO("[pause-menu] settings saved to {}", config_path.string());
        break;
    }
    }
}

void ClickItem(const ItemDef& it) { ApplyItem(it, +1); }

// ── Native rows: build, install, sync ──────────────────────────────────
//
// Measured earlier and still true: giving the item *states* `.sN` children
// does NOT turn them into value rows — that is a garage widget trick, and on
// the pause list the children rendered as nothing at all. What follows takes
// the other route entirely and stops using states as rows: real row objects
// of the game's own value class, handed to the list through its widget array.

constexpr int kMaxRowsPerMenu = 16;

struct NativeRows {
    uint32_t array = 0;                     // guest array of row pointers
    uint32_t row[kMaxRowsPerMenu] = {};     // guest row objects
    uint32_t name[kMaxRowsPerMenu] = {};    // guest key string, kept for relabels
    int last_index[kMaxRowsPerMenu] = {};   // last select we pushed to the cvar
    std::string last_label[kMaxRowsPerMenu];
    int count = 0;
};
NativeRows g_rows[kNumMenus];

// The value list a row steps through: {vtable, char** keys, count, stride}.
// vfunc4 returns +8 and vfunc12 returns keys[stride * row + col], which is
// all sub_82631A20 and sub_826316E0 ever ask of it. The keys are string-table
// keys, not display text — the renderer looks each one up itself, on every
// render, so moving a value is a matter of rewriting the table entry.
//
// The count is always three, never the real number of values. sub_82631A20
// writes one Flash element per value and — unlike sub_82631450, which stops
// at vfunc84 — never checks how many elements the row clip actually has, so
// a list longer than the clip runs straight off the end of the array. Three
// is what the game's own numeric rows use (sub_82631450 writes exactly
// min/value/max with select=1), so it is the width the clip is known to
// have. The row therefore shows a sliding window — previous, current, next —
// and its select sits at the middle, which is also what makes a step
// readable: the select leaving 1 is the direction the player pressed.
constexpr int kRowWindow = 3;

// Key of the window slot `slot` of item `it`. Stable for the row's lifetime;
// only the text behind it moves.
void RowWindowKey(const ItemDef& it, int slot, char* out, size_t out_size) {
    std::snprintf(out, out_size, "%s_w%d", it.key, slot);
}

uint32_t BuildRowValueSet(const ItemDef& it) {
    if (ItemValueCount(it) <= 0) return 0;

    uint32_t keys = CallGuestFn1(kGuestMallocFn, uint32_t(4 * kRowWindow));
    if (!keys) return 0;

    for (int slot = 0; slot < kRowWindow; ++slot) {
        char wkey[96];
        RowWindowKey(it, slot, wkey, sizeof(wkey));
        // Created at full width here so later moves can patch in place.
        EnsureStringTableText(wkey, "");
        uint32_t key_str = AllocGuestString(wkey);
        if (!key_str) return 0;
        WriteGuestBE32(keys + uint32_t(4 * slot), key_str);
    }

    uint32_t set = CallGuestFn1(kGuestMallocFn, 16);
    if (!set) return 0;
    WriteGuestBE32(set +  0, kStrListVtable);
    WriteGuestBE32(set +  4, keys);
    WriteGuestBE32(set +  8, uint32_t(kRowWindow));
    WriteGuestBE32(set + 12, 1);  // one column per row
    return set;
}

// Centre the window on `idx` and put the row's select back on the middle
// slot. Called for every value move, so the row is always one step away from
// either neighbour no matter how long the real list is.
void CentreRowWindow(NativeRows& nr, int i, const ItemDef& it, int idx) {
    const int n = ItemValueCount(it);
    if (n <= 0 || !nr.row[i]) return;

    for (int slot = 0; slot < kRowWindow; ++slot) {
        char wkey[96];
        RowWindowKey(it, slot, wkey, sizeof(wkey));
        const int v = ((idx + slot - 1) % n + n) % n;
        EnsureStringTableText(wkey, ItemValueLabel(it, v).c_str());
    }

    nr.last_index[i] = idx;
    WriteGuestBE32(nr.row[i] + kRowSelect, 1);
}

bool BuildNativeRows(int m) {
    NativeRows& nr = g_rows[m];
    if (nr.array) return true;

    const MenuDef& md = kMenus[m];
    if (md.num_items > kMaxRowsPerMenu) {
        MC_WARN("[pause-menu] '{}' has {} items, over the {} row cap",
                md.label, md.num_items, kMaxRowsPerMenu);
        return false;
    }

    uint8_t* base = GetMembase();
    uint32_t arr = CallGuestFn1(kGuestMallocFn, uint32_t(4 * md.num_items));
    if (!base || !arr) return false;

    for (int i = 0; i < md.num_items; ++i) {
        const ItemDef& it = md.items[i];

        // The ctor resolves the label through the string table once and
        // caches it at +48, so the text has to be registered first.
        std::string label = ItemRowLabel(it);
        EnsureStringTableText(it.key, label.c_str());

        uint32_t name = AllocGuestString(it.key);
        uint32_t row  = CallGuestFn1(kGuestMallocFn, kRowObjectSize);
        if (!name || !row) return false;
        std::memset(base + row, 0, kRowObjectSize);

        if (it.kind == ItemKind::kSave) {
            // No value list: a plain label row (0x8208DACC), which renders
            // through sub_82631C08 and draws no arrows.
            CallGuestFn3(kLabelRowCtorFn, row, name, 0);
        } else if (ItemIsToggle(it)) {
            CallGuestFn3(kLabelRowCtorFn, row, name, 0);
            WriteGuestBE32(row, kToggleRowVtable);
            WriteGuestU8(row + kRowChecked, uint8_t(ItemValueIndex(it)));
            WriteGuestU8(row + kRowHasBox, 1);
            WriteGuestBE32(row + kRowEnabled, 1);
            WriteGuestBE32(row + kRowProperty1, 0xFFFFFFFFu);
        } else {
            uint32_t set = BuildRowValueSet(it);
            if (!set) return false;
            CallGuestFn4(kOptionRowCtorFn, row, name, set, 1);
        }

        nr.row[i]        = row;
        nr.name[i]       = name;
        nr.last_index[i] = ItemIsToggle(it) ? ItemValueIndex(it) : 0;
        nr.last_label[i] = label;
        if (it.kind != ItemKind::kSave && !ItemIsToggle(it))
            CentreRowWindow(nr, i, it, ItemValueIndex(it));
        WriteGuestBE32(arr + uint32_t(4 * i), row);
    }

    nr.count = md.num_items;
    nr.array = arr;
    MC_INFO("[pause-menu] '{}' native rows built ({} rows)",
            md.label, nr.count);
    return true;
}

// Re-resolve a row's label after its text changed. The ctor caches the
// looked-up string at +48 where vfunc332 reads it, so rewriting the string
// table is not enough on its own — sub_8263B860 has to run again.
void RelabelRow(NativeRows& nr, int i, const ItemDef& it) {
    std::string label = ItemRowLabel(it);
    if (label == nr.last_label[i] || !nr.row[i] || !nr.name[i]) return;
    nr.last_label[i] = label;
    EnsureStringTableText(it.key, label.c_str());
    CallGuestFn2(kSetRowLabelFn, nr.row[i], nr.name[i]);
}

// Point the list at our rows. Also clears both higher-priority row sources,
// because sub_82631FD0 only reaches the widget array when they are null.
void InstallNativeRows(int m, uint32_t list) {
    NativeRows& nr = g_rows[m];
    if (!list || !nr.array || nr.count <= 0) return;

    const uint32_t count_cap =
        (uint32_t(nr.count) << 16) | uint32_t(nr.count);

    if (ReadGuestBE32(list + kListRows) == nr.array &&
        ReadGuestBE32(list + kListCountCap) == count_cap &&
        ReadGuestBE32(list + kListStateSource) == 0 &&
        ReadGuestBE32(list + kListTableSource) == 0)
        return;

    WriteGuestBE32(list + kListStateSource, 0);
    WriteGuestBE32(list + kListTableSource, 0);
    WriteGuestBE32(list + kListRows, nr.array);
    WriteGuestBE32(list + kListCountCap, count_cap);
}

// Rows own their value while the menu is open, so this only runs where the
// value can have moved behind their back: entering the menu, and after A.
void SyncNativeRowsFromCvars(int m) {
    NativeRows& nr = g_rows[m];
    const MenuDef& md = kMenus[m];

    for (int i = 0; i < nr.count && i < md.num_items; ++i) {
        const ItemDef& it = md.items[i];
        RelabelRow(nr, i, it);

        if (it.kind == ItemKind::kSave) continue;

        const int idx = ItemValueIndex(it);
        if (ItemIsToggle(it)) {
            nr.last_index[i] = idx;
            WriteGuestU8(nr.row[i] + kRowChecked, uint8_t(idx));
            continue;
        }
        CentreRowWindow(nr, i, it, idx);
    }
}

// Left/right are handled entirely inside the row, which then makes the list
// re-render — so the render is where the move is noticed and written through
// to the cvar. It runs before any row is drawn, so the value the player sees
// and the value the cvar holds never disagree on screen.
void PollNativeRows(int m) {
    NativeRows& nr = g_rows[m];
    const MenuDef& md = kMenus[m];

    for (int i = 0; i < nr.count && i < md.num_items; ++i) {
        const ItemDef& it = md.items[i];

        // SAVE tracks g_settings_saved, carbon tracks whether there is a car
        // to edit — both change without the row being touched.
        if (it.kind == ItemKind::kSave || it.kind == ItemKind::kCarbonBit)
            RelabelRow(nr, i, it);
        if (it.kind == ItemKind::kSave) continue;

        const int n = ItemValueCount(it);
        if (n <= 0) continue;

        // A checkbox has no input of its own — the box only ever moves
        // because the cvar did, whether that was A here or the F4 menu.
        if (ItemIsToggle(it)) {
            const int checked = ItemValueIndex(it);
            if (checked != nr.last_index[i]) {
                nr.last_index[i] = checked;
                WriteGuestU8(nr.row[i] + kRowChecked, uint8_t(checked));
            }
            continue;
        }

        // The window is always re-centred on slot 1, so a select anywhere
        // else is a step the player just took, and which way.
        const int slot = int(ReadGuestBE32(nr.row[i] + kRowSelect));
        if (slot != 1) {
            const int dir = slot < 1 ? -1 : +1;
            const int idx = ((nr.last_index[i] + dir) % n + n) % n;
            ItemSetValueIndex(it, idx);
            CentreRowWindow(nr, i, it, idx);
            continue;
        }

        // Carbon lives in the car, not in a cvar, so the car is the one that
        // wins: a row that did not move but no longer matches the car (the
        // player swapped cars, or there was no car to toggle) is pulled back
        // rather than written through.
        if (it.kind == ItemKind::kCarbonBit) {
            const int real = ItemValueIndex(it);
            if (real != nr.last_index[i])
                CentreRowWindow(nr, i, it, real);
        }
    }
}

// ── Flash display ──────────────────────────────────────────────────────

void PopulateSubmenuFlash() {
    int m = g_active_menu.load(std::memory_order_relaxed);
    if (m < 0) return;
    uint32_t sub  = g_menu_state[m].load(std::memory_order_relaxed);
    uint32_t list = g_pauselist_addr.load(std::memory_order_relaxed);
    if (!sub || !list) return;

    InstallNativeRows(m, list);
    RefreshPauseList();

    // TodMenu onenter: game.setInt('PAUSEMOVIE','tabs_count',0) — hides the
    // tab strip so Flash presents a submenu instead of the SettingsMenu tab.
    uint32_t flashCtx = GetPauseMovieFlashCtx();
    if (flashCtx)
        CallGuestFn3(kSetFlashIntFn, flashCtx, kStr_tabs_count, 0);

    uint32_t title = g_menu_title[m].load(std::memory_order_relaxed);
    if (!title) {
        title = AllocGuestString(kMenus[m].label);
        g_menu_title[m].store(title, std::memory_order_relaxed);
    }
    SetMenuTitle(title);
}

// ── Submenu creation ───────────────────────────────────────────────────

void EnsureSubmenus() {
    if (g_menus_created.load(std::memory_order_relaxed)) return;

    uint32_t settings = g_settingsmenu_addr.load(std::memory_order_relaxed);
    if (!settings) return;

    for (int m = 0; m < kNumMenus; ++m) {
        const MenuDef& md = kMenus[m];

        uint32_t btn = CreateNewMenuState(md.btn_key);
        if (!btn) return;
        GuestAppendMenuItem(settings, btn);
        EnsureStringTableText(md.btn_key, md.label);
        g_menu_btn_key[m].store(AllocGuestString(md.btn_key),
                                std::memory_order_relaxed);

        uint32_t sub = CreateUIMenuState(md.menu_key);
        if (!sub) return;

        // Give the submenu an owner. vhsmState's "find my root" virtual
        // (vtable+204, sub_8268D638) walks up the +32 parent chain; a state
        // with no parent falls through to a lookup that returns NULL, and
        // sub_82224678 dereferences that result without checking:
        //
        //     v5 = vfunc204(state, ...);
        //     v7 = *(uint8_t **)(*(_DWORD *)v5 + 20);   // NULL -> access violation
        //
        // Any state transition started from inside a submenu (opening the
        // camera, for one) takes that path, so a parentless submenu is a
        // crash waiting to happen. Only the back-pointer is set: `settings`
        // keeps its own child list untouched (the submenu is entered through
        // GuestStackPush, not by being listed), so this stays invisible while
        // making the root walk terminate.
        WriteGuestBE32(sub + 32, settings);

        g_menu_state[m].store(sub, std::memory_order_relaxed);
        EnsureStringTableText(md.menu_key, md.label);

        // One child state per item. These are no longer what the list draws —
        // the native rows are — but the submenu is entered through the state
        // stack and Hook_PopulateRedirect still resolves against them, so the
        // tree stays the shape the engine expects.
        for (int i = 0; i < md.num_items; ++i) {
            const ItemDef& it = md.items[i];
            uint32_t st = CreateNewMenuState(it.key);
            if (!st) continue;
            GuestAppendMenuItem(sub, st);
        }

        // Rows come after the states so the string-table labels they cache
        // are the final ones.
        BuildNativeRows(m);

        MC_INFO("[pause-menu] submenu '{}' created ({} items)",
                md.label, md.num_items);
    }

    g_menus_created.store(true, std::memory_order_relaxed);
}

// ── Submenu item click handler ─────────────────────────────────────────

bool HandleSubmenuIndexClick(uint32_t idx) {
    int m = g_active_menu.load(std::memory_order_relaxed);
    if (m < 0) return false;
    const MenuDef& md = kMenus[m];
    if (idx >= uint32_t(md.num_items)) return false;

    // A steps the value forward, the same as right. The row holds the select
    // itself, so the cvar move has to be mirrored back into it.
    ClickItem(md.items[idx]);
    SyncNativeRowsFromCvars(m);
    PopulateSubmenuFlash();
    return true;
}

// ── Options > Controller: extra "PLAYSTATION BUTTONS" row ──────────────
//
// This screen is not the pause tab list, so none of the RexGlue submenu
// machinery applies — the row goes into the game's own control-scheme list.
//
// Its ctor sub_8264DF48 constructs ELEVEN checkbox rows in place at
// screen+2784 (240 bytes apart, vtable 0x8209788C, +212 = 1) and then
// registers only `presetTable.count - 1` = 8 of them into the shared row
// array, which is why the list reports count = 8, capacity = 11. Slots 8..10
// are fully constructed rows nobody uses, so the extra row costs no
// allocation at all: point the array's next free slot at slot 8 and bump the
// count. The screen's own array/count pair (screen+848 / screen+852) IS the
// list's +176 / +180, so one write updates both views.
//
// The list lives at screen+672 (measured: list 0xBECC9710 - 672 = 0xBECC9470,
// and screen+2784 == the logged row[0] 0xBECC9F50), which also gives the
// populate hook a localisation-proof way to recognise the screen: the vtable
// sub_8264DF48 stamps at the end of construction.
constexpr uint32_t kCtrlScreenVtable = 0x820995F4;
constexpr uint32_t kCtrlListOffset   = 672;   // mcListViewFixed inside the screen
constexpr uint32_t kCtrlRowsOffset   = 2784;  // first of the 11 embedded rows
constexpr const char* kCtrlRowKey    = "PM_PSButtons";
constexpr const char* kCtrlRowLabel  = "PLAYSTATION BUTTONS";

std::atomic<uint32_t> g_ctrl_row{0};        // guest row object, 0 = not installed
std::atomic<int>      g_ctrl_row_index{-1}; // its index in the list

bool ButtonPromptsArePS() {
    return CvarGet("button_prompts") == "playstation";
}

// Re-render the list through its own vtable slot 184, the same call the
// screen's key handler makes after it changes anything.
void RefreshList(uint32_t list) {
    uint32_t vt = list ? ReadGuestBE32(list) : 0;
    uint32_t fn = vt ? ReadGuestBE32(vt + 184) : 0;
    if (fn) CallGuestFn1(fn, list);
}

// Idempotent: appends the row the first time and only refreshes the checkbox
// afterwards. Re-runs cleanly if the screen object is rebuilt, because the
// fresh ctor puts the count back to 8 and the array slot no longer matches.
void EnsureControllerButtonRow(uint32_t list) {
    if (list <= kCtrlListOffset) return;
    uint32_t screen = list - kCtrlListOffset;
    if (ReadGuestBE32(screen) != kCtrlScreenVtable) return;

    uint32_t arr = ReadGuestBE32(list + kListRows);
    uint32_t cc  = ReadGuestBE32(list + kListCountCap);
    uint16_t count = uint16_t(cc >> 16);
    uint16_t cap   = uint16_t(cc & 0xFFFF);
    if (!arr || count == 0) return;

    const bool ps = ButtonPromptsArePS();

    // Already ours? Just keep the box in sync with the cvar.
    int idx = g_ctrl_row_index.load(std::memory_order_relaxed);
    if (idx >= 0 && idx < int(count)) {
        uint32_t row = ReadGuestBE32(arr + uint32_t(4 * idx));
        if (row && row == screen + kCtrlRowsOffset + kRowObjectSize * uint32_t(idx)) {
            WriteGuestU8(row + kRowChecked, uint8_t(ps ? 1 : 0));
            g_ctrl_row.store(row, std::memory_order_relaxed);
            return;
        }
    }

    if (count >= cap) return;  // no spare slot — leave the screen alone

    uint32_t row = screen + kCtrlRowsOffset + kRowObjectSize * uint32_t(count);

    // One allocation for the whole session: the screen can be rebuilt, the key
    // string cannot change.
    static uint32_t name = 0;
    if (!name) {
        EnsureStringTableText(kCtrlRowKey, kCtrlRowLabel);
        name = AllocGuestString(kCtrlRowKey);
    }
    if (!name) return;

    WriteGuestBE32(row, kToggleRowVtable);
    CallGuestFn2(kSetRowLabelFn, row, name);  // resolves the text into +48
    WriteGuestU8(row + kRowChecked, uint8_t(ps ? 1 : 0));
    WriteGuestU8(row + kRowHasBox, 1);
    WriteGuestBE32(row + kRowEnabled, 1);
    WriteGuestBE32(row + kRowProperty1, 0xFFFFFFFFu);

    // list+180 and screen+852 are the same field (672 + 180 == 852), so this
    // single write is what both the list and the screen read back.
    WriteGuestBE32(arr + uint32_t(4 * count), row);
    WriteGuestBE32(list + kListCountCap,
                   (uint32_t(count + 1) << 16) | uint32_t(cap));

    g_ctrl_row.store(row, std::memory_order_relaxed);
    g_ctrl_row_index.store(int(count), std::memory_order_relaxed);
    MC_INFO("[pause-menu] controller screen 0x{:08X}: '{}' row installed at "
            "index {} (cap {})", screen, kCtrlRowLabel, count, cap);
}

}  // anonymous namespace

// ── Public API ──────────────────────────────────────────────────────────

void InitPauseMenuHooks() {
    MC_INFO("[pause-menu] hooks registered");
}

void Hook_CapturePMContinue(PPCRegister& r3) {
    uint32_t pm_continue = static_cast<uint32_t>(r3.u64);
    if (!pm_continue) return;

    uint32_t pausetab = ReadGuestBE32(pm_continue + 32);
    if (pausetab) {
        g_pausetab_addr.store(pausetab, std::memory_order_relaxed);
    }
}

bool Hook_EnablePMSave(PPCRegister& r3, PPCRegister& r4) {
    uint32_t pm_save = static_cast<uint32_t>(r3.u64);

    if (pm_save && !g_settingsmenu_addr.load(std::memory_order_relaxed)) {
        uint32_t parent = ReadGuestBE32(pm_save + 32);
        if (parent)
            g_settingsmenu_addr.store(parent, std::memory_order_relaxed);
    }

    uint32_t pausetab = g_pausetab_addr.load(std::memory_order_relaxed);
    if (pm_save && pausetab) {
        GuestDetachMenuItem(pm_save);
        GuestAppendMenuItem(pausetab, pm_save);
        EnsureStringTableText("Save (Dev Only)", "SAVE GAME");
    }

    EnsureSubmenus();

    // This only runs with the pause menu on screen, which makes it the right
    // place to arm the probe — state names cannot do it, the whole tree
    // activates once at boot.
    if (!g_pm_probe_armed.exchange(true)) {
        g_pm_seen_count = 0;
        MC_INFO("[pm-probe] armed (pause menu shown)");
    }

    r4.u64 = 1;
    return true;
}

bool Hook_EnablePMTeste(PPCRegister& r3, PPCRegister& r4) {
    if (!REXCVAR_GET(show_test_movie))
        return false;

    PatchStringTableEntry("PM_TestMovie", "Teste");

    r4.u64 = 1;
    return true;
}

bool Hook_PMLodTrafficClick(PPCRegister& r3, PPCRegister& r31) {
    (void)r31;

    if (g_active_menu.load(std::memory_order_relaxed) >= 0) {
        uint32_t list = g_pauselist_addr.load(std::memory_order_relaxed);
        uint32_t idx = list ? ReadGuestBE32(list + 184) : 0xFFFFFFFFu;
        HandleSubmenuIndexClick(idx);
        // Consume every action press inside the submenu so the game's own
        // handler never resolves the selection against SettingsMenu items.
        r3.u64 = 1;
        return true;
    }

    for (int m = 0; m < kNumMenus; ++m) {
        uint32_t btn_key = g_menu_btn_key[m].load(std::memory_order_relaxed);
        if (!btn_key || !IsGuestButtonClicked(btn_key)) continue;

        uint32_t sub = g_menu_state[m].load(std::memory_order_relaxed);
        uint32_t list = g_last_list.load(std::memory_order_relaxed);
        if (!sub || !list) return false;
        if (!BuildNativeRows(m)) return false;

        g_pauselist_addr.store(list, std::memory_order_relaxed);

        // Every row source the list has, saved together: the tab is put back
        // by restoring all of them, not just the adapter.
        g_saved_list_adapter.store(ReadGuestBE32(list + kListStateSource),
                                   std::memory_order_relaxed);
        g_saved_list_select.store(ReadGuestBE32(list + kListSelect),
                                  std::memory_order_relaxed);
        g_saved_list_rows.store(ReadGuestBE32(list + kListRows),
                                std::memory_order_relaxed);
        g_saved_list_countcap.store(ReadGuestBE32(list + kListCountCap),
                                    std::memory_order_relaxed);
        g_saved_list_table.store(ReadGuestBE32(list + kListTableSource),
                                 std::memory_order_relaxed);
        g_saved_list_scroll.store(ReadGuestBE32(list + kListScroll),
                                  std::memory_order_relaxed);
        WriteGuestBE32(list + kListSelect, 0);
        WriteGuestBE32(list + kListScroll, 0);

        g_active_menu.store(m, std::memory_order_relaxed);
        SyncNativeRowsFromCvars(m);  // pick up F4-menu changes made meanwhile
        GuestStackPush(sub);
        PopulateSubmenuFlash();
        MC_INFO("[pause-menu] entered '{}' submenu", kMenus[m].label);
        r3.u64 = 1;
        return true;
    }

    return false;
}

bool Hook_ListViewPopulate(PPCRegister& r3) {
    uint32_t list = static_cast<uint32_t>(r3.u64);
    if (!list) return false;

    g_last_list.store(list, std::memory_order_relaxed);

    // Options > Controller gets an extra row appended into its own list. No-op
    // for every other list — the screen is recognised by its vtable.
    EnsureControllerButtonRow(list);

    // Measured: navigating the pause menu emits neither state activations nor
    // flash commands — entering Options > Game logged nothing at all, while
    // every garage move logged an activate. So these rows are not states being
    // activated; mcListViewFixed writes them itself, which makes this the only
    // place the `< >` rows can be described. Dump what it is handed.
    if (CvarGetBool("carbon_menu_diag") &&
        g_pm_probe_armed.load(std::memory_order_relaxed)) {
        uint8_t* base = GetMembase();
        uint32_t adapter = ReadGuestBE32(list + 196);
        const char* adapter_name = "<none>";
        if (base && adapter) {
            uint32_t np = ReadGuestBE32(adapter + 20);
            if (np) adapter_name = reinterpret_cast<const char*>(base + np);
        }

        MC_INFO("[pm-list] populate 0x{:08X} select={} adapter=0x{:08X} '{}'",
                list, int32_t(ReadGuestBE32(list + 184)), adapter, adapter_name);

        // The first dump stopped at 44 dwords and never reached +184/+196.
        // Walk the whole object in chunks instead, 16 dwords per line, so the
        // row array and the counters around it are all visible.
        for (int base_i = 0; base_i < 64; base_i += 16) {
            char line[200];
            size_t pos = 0;
            for (int i = base_i; i < base_i + 16 && pos < sizeof(line) - 12; ++i) {
                int w = std::snprintf(line + pos, sizeof(line) - pos, "%08X ",
                                      ReadGuestBE32(list + uint32_t(i * 4)));
                if (w < 0) break;
                pos += size_t(w);
            }
            line[pos] = 0;
            MC_INFO("[pm-list]   +{:<3} {}", base_i * 4, line);
        }

        // Row array at +176, then count:u16 and capacity:u16 at +180/+182 —
        // that order, not the other way round. sub_82631FD0 reads the row
        // count as `*((u16 *)list + 90)`, the high half, and the Controller
        // preset list settles it live: it dumps 0x0008000B with eight built
        // rows and three slots left as 0xCDCDCDCD filler.
        uint32_t rows = ReadGuestBE32(list + 176);
        uint32_t packed = ReadGuestBE32(list + 180);
        uint32_t count = packed >> 16;
        if (rows >= 0xB0000000 && count > 0 && count < 32) {
            MC_INFO("[pm-list]   rows=0x{:08X} count={} cap={} packed=0x{:08X} "
                    "n164={} n168={}",
                    rows, count, packed & 0xFFFF, packed,
                    ReadGuestBE32(list + 184),
                    ReadGuestBE32(list + 188));

            for (uint32_t i = 0; i < count; ++i) {
                uint32_t row = ReadGuestBE32(rows + i * 4);
                if (row < 0xB0000000) continue;

                char line[200];
                size_t pos = 0;
                for (int k = 0; k < 16 && pos < sizeof(line) - 12; ++k) {
                    int w = std::snprintf(line + pos, sizeof(line) - pos,
                                          "%08X ",
                                          ReadGuestBE32(row + uint32_t(k * 4)));
                    if (w < 0) break;
                    pos += size_t(w);
                }
                line[pos] = 0;
                MC_INFO("[pm-list]     row[{}] 0x{:08X}: {}", i, row, line);

                // Any pointer in the row that reads as text is the label or
                // the value; printing them tells which slot is which.
                for (int k = 0; k < 16; ++k) {
                    uint32_t p = ReadGuestBE32(row + uint32_t(k * 4));
                    if (p < 0xB0000000 && (p < 0x82000000 || p > 0x82FFFFFF))
                        continue;
                    const char* s = reinterpret_cast<const char*>(base + p);
                    bool printable = true;
                    int n = 0;
                    for (; n < 24 && s[n]; ++n)
                        if (uint8_t(s[n]) < 0x20 || uint8_t(s[n]) > 0x7E) {
                            printable = false;
                            break;
                        }
                    if (printable && n >= 2)
                        MC_INFO("[pm-list]       +{} -> '{:.24s}'", k * 4, s);
                }
            }
        }

        // Whatever the adapter is, its children are the rows being drawn.
        int i = 0;
        for (uint32_t c = adapter ? ReadGuestBE32(adapter + 44) : 0;
             c && i < 20; c = ReadGuestBE32(c + 36), ++i) {
            uint32_t np = ReadGuestBE32(c + 20);
            MC_INFO("[pm-list]   row[{}] 0x{:08X} flags=0x{:X} active={} "
                    "kids=0x{:08X} label48=0x{:08X} '{}'",
                    i, c, ReadGuestBE32(c + 16),
                    int32_t(ReadGuestBE32(c + kStateActiveChild)),
                    ReadGuestBE32(c + 44), ReadGuestBE32(c + 48),
                    (base && np) ? reinterpret_cast<const char*>(base + np)
                                 : "<unnamed>");
        }
    }

    // While inside a RexGlue submenu, every re-render of the pause tab's own
    // list (and only that one) draws the active menu's native rows. This runs
    // at the top of the renderer, before a single row is written, so reading
    // the rows' select here is what carries a left/right through to the cvar
    // in the same frame it is drawn.
    int m = g_active_menu.load(std::memory_order_relaxed);
    if (m >= 0 && list == g_pauselist_addr.load(std::memory_order_relaxed)) {
        PollNativeRows(m);
        InstallNativeRows(m, list);

        if (CvarGetBool("carbon_menu_diag")) {
            const NativeRows& nr = g_rows[m];
            const MenuDef& md = kMenus[m];
            for (int i = 0; i < nr.count && i < md.num_items; ++i) {
                const ItemDef& it = md.items[i];
                const int n = ItemValueCount(it);
                const int idx = nr.last_index[i];
                if (ItemIsToggle(it)) {
                    MC_INFO("[pm-row] {} '{}' obj=0x{:08X} toggle checked={} "
                            "box={} enabled={}",
                            i, nr.last_label[i], nr.row[i],
                            ReadGuestU8(nr.row[i] + kRowChecked),
                            ReadGuestU8(nr.row[i] + kRowHasBox),
                            ReadGuestBE32(nr.row[i] + kRowEnabled));
                    continue;
                }
                MC_INFO("[pm-row] {} '{}' obj=0x{:08X} values={} idx={} '{}' "
                        "slot={}",
                        i, nr.last_label[i], nr.row[i], n, idx,
                        (idx >= 0 && idx < n) ? ItemValueLabel(it, idx)
                                              : std::string(),
                        int(ReadGuestBE32(nr.row[i] + kRowSelect)));
            }
        }
    }
    return false;
}

// Options > Controller key handler (sub_8264D0D8, r3 = screen, r4 = key).
// Key 55 is accept, 63 the customise action; both resolve the highlighted row
// against the control-scheme table and would apply a preset. Our appended row
// has no preset behind it, so when it is the one highlighted we flip
// button_prompts instead and report the key as consumed — returning true jumps
// to loc_8264D2B4, the function's own `li r3, 1` exit.
bool Hook_ControllerMenuKey(PPCRegister& r3, PPCRegister& r4) {
    const uint32_t screen = static_cast<uint32_t>(r3.u64);
    const uint32_t key    = static_cast<uint32_t>(r4.u64);
    if (key != 55 && key != 63) return false;

    const int idx = g_ctrl_row_index.load(std::memory_order_relaxed);
    const uint32_t row = g_ctrl_row.load(std::memory_order_relaxed);
    if (idx < 0 || !row || !screen) return false;
    if (ReadGuestBE32(screen) != kCtrlScreenVtable) return false;
    if (row != screen + kCtrlRowsOffset + kRowObjectSize * uint32_t(idx))
        return false;

    const uint32_t list = screen + kCtrlListOffset;
    if (int32_t(ReadGuestBE32(list + kListSelect)) != idx) return false;

    // 63 (customise controls) has nothing to do here — swallow it so the game
    // never runs the remap flow against a row with no scheme behind it.
    if (key == 63) return true;

    const bool ps = !ButtonPromptsArePS();
    CvarSet("button_prompts", ps ? "playstation" : "xbox");
    WriteGuestU8(row + kRowChecked, uint8_t(ps ? 1 : 0));
    RefreshList(list);
    MC_INFO("[pause-menu] button prompts -> {}", ps ? "PlayStation" : "Xbox 360");
    return true;
}

bool Hook_FlashCommandLog(PPCRegister& r5) {
    uint32_t cmd = static_cast<uint32_t>(r5.u64);
    if (!cmd || !GetMembase()) return false;

    int16_t type = static_cast<int16_t>(ReadGuestBE16(cmd + 2));

    char args[256];
    size_t pos = 0;
    uint32_t node = ReadGuestBE32(cmd + 4);
    for (int i = 0; node && i < 6 && pos < sizeof(args) - 24; ++i) {
        uint32_t ntype = ReadGuestBE32(node);
        uint32_t nval  = ReadGuestBE32(node + 4);
        int written;
        if (ntype == 5 && nval) {
            const char* s = reinterpret_cast<const char*>(GetMembase() + nval);
            written = std::snprintf(args + pos, sizeof(args) - pos, " '%.48s'", s);
        } else {
            written = std::snprintf(args + pos, sizeof(args) - pos, " t%u:0x%08X",
                                    ntype, nval);
        }
        if (written < 0) break;
        pos += static_cast<size_t>(written);
        node = ReadGuestBE32(node + 8);
    }
    args[pos] = 0;

    MC_INFO("[flash-cmd] type={}{}", type, args);
    return false;
}

// ── Garage carbon menu probe ────────────────────────────────────────────
//
// Measured: the item named "Hood" is a paint AREA, not a carbon switch. It
// sits under "PaintArea" next to Body / Doors / FrontBumper / RearBumper /
// Skirts, and sub_826A5528 calls vtable+80 on it when the car's carbon flag
// is set. +80 (sub_8268F200) clears the 0x2 bit at state+16, and 0x2 is the
// bit every live item carries (flags 0x42), so +80 disables and +84 enables:
// a carbon hood is simply not paintable. Retail lost the entry that turned
// carbon on, never the plumbing.
//
// PaintArea's own zones line up with our carbon groups almost one to one, so
// that screen is where a garage-side carbon submenu belongs. What is still
// missing is the level above — the menu that owns PaintArea — because that is
// what a "CARBON FIBER" button has to be appended to. Dumps two levels once.
constexpr const char* kGarageCarbonName = "RxGarageCarbon";

static std::atomic<uint32_t> g_garage_carbon_state{0};
static std::atomic<uint32_t> g_garage_carbon_key{0};

static void EnsureGarageCarbonEntry();

void Hook_CarbonGarageProbe(PPCRegister& r3) {
    (void)r3;
    uint8_t* base = GetMembase();
    if (!base) return;

    // The guest string is allocated once; the walk repeats whenever the screen
    // is rebuilt at a different address. Latching on failure was wrong: the
    // first call can land before the paint screen exists, and that used to
    // silence the probe for the rest of the session.
    static uint32_t name_str = 0;
    if (!name_str) name_str = AllocGuestString("Hood");
    uint32_t hood = name_str ? CallGuestFn1(kFindUIObjectFn, name_str) : 0;
    if (!hood) return;

    EnsureGarageCarbonEntry();

    if (!CvarGetBool("carbon_menu_diag")) return;

    static uint32_t last_hood = 0;
    if (hood == last_hood) return;
    last_hood = hood;

    auto guest_name = [&](uint32_t state) -> const char* {
        uint32_t p = state ? ReadGuestBE32(state + 20) : 0;
        return p ? reinterpret_cast<const char*>(base + p) : "<unnamed>";
    };

    auto dump = [&](const char* what, uint32_t node) {
        if (!node) {
            MC_INFO("[carbon-garage] {}: <none>", what);
            return;
        }
        MC_INFO("[carbon-garage] {} 0x{:08X} flags=0x{:08X} vtbl=0x{:08X} '{}' "
                "parent=0x{:08X} '{}'",
                what, node, ReadGuestBE32(node + 16), ReadGuestBE32(node),
                guest_name(node), ReadGuestBE32(node + 32),
                guest_name(ReadGuestBE32(node + 32)));
        int i = 0;
        for (uint32_t c = ReadGuestBE32(node + 44); c && i < 40;
             c = ReadGuestBE32(c + 36), ++i) {
            MC_INFO("[carbon-garage]   {}[{}] 0x{:08X} flags=0x{:08X} "
                    "vtbl=0x{:08X} '{}'",
                    what, i, c, ReadGuestBE32(c + 16), ReadGuestBE32(c),
                    guest_name(c));
        }
    };

    uint32_t area = ReadGuestBE32(hood + 32);        // "PaintArea"
    uint32_t screen = area ? ReadGuestBE32(area + 32) : 0;
    uint32_t owner = screen ? ReadGuestBE32(screen + 32) : 0;

    dump("area", area);
    dump("screen", screen);
    dump("owner", owner);

    // Field-level comparison: a working value row (PaintType, with its value
    // child Metallic) against the injected one. Same class, same vtable, same
    // flags, same parent — so whatever makes the game walk one row's values
    // and ignore the other's is a data field neither the name convention nor
    // the child list covers.
    auto hexdump = [&](const char* what, uint32_t node) {
        if (!node) return;
        char line[256];
        size_t pos = 0;
        for (int i = 0; i < 24 && pos < sizeof(line) - 12; ++i) {
            int w = std::snprintf(line + pos, sizeof(line) - pos, "%08X ",
                                  ReadGuestBE32(node + uint32_t(i * 4)));
            if (w < 0) break;
            pos += size_t(w);
        }
        line[pos] = 0;
        MC_INFO("[carbon-fields] {} 0x{:08X}: {}", what, node, line);
    };

    auto by_name = [&](const char* n) -> uint32_t {
        uint32_t s = AllocGuestString(n);
        return s ? CallGuestFn1(kFindUIObjectFn, s) : 0;
    };

    hexdump("PaintType", by_name("PaintType"));
    hexdump("Metallic ", by_name("Metallic"));
    hexdump("PaintArea", area);
    hexdump("Body     ", by_name("Body"));
    uint32_t ours = g_garage_carbon_state.load(std::memory_order_relaxed);
    hexdump("RxCarbon ", ours);
    hexdump("RxCarb.s0", ours ? ReadGuestBE32(ours + 44) : 0);

    // Activating a real menu state emits the flash commands that populate the
    // screen ('activate PartColorMenu' is immediately followed by cmd 112/112/
    // 52). That executable content is what state+24 / +28 point at, and an
    // injected state has both at zero — which is why pushing ours rendered
    // nothing even though the push itself succeeded. Open those records.
    auto follow = [&](const char* what, uint32_t state, uint32_t field) {
        if (!state) return;
        uint32_t target = ReadGuestBE32(state + field);
        if (!target) {
            MC_INFO("[carbon-content] {}+{}: null", what, field);
            return;
        }
        char line[192];
        size_t pos = 0;
        for (int i = 0; i < 16 && pos < sizeof(line) - 12; ++i) {
            int w = std::snprintf(line + pos, sizeof(line) - pos, "%08X ",
                                  ReadGuestBE32(target + uint32_t(i * 4)));
            if (w < 0) break;
            pos += size_t(w);
        }
        line[pos] = 0;
        MC_INFO("[carbon-content] {}+{} -> 0x{:08X}: {}", what, field, target,
                line);
    };

    uint32_t colormenu = by_name("PartColorMenu");
    uint32_t submenu = by_name("PartColorSubMenu");
    hexdump("ColorMenu", colormenu);
    hexdump("ColorSub ", submenu);
    for (uint32_t f : {24u, 28u, 48u}) {
        follow("PartColorMenu", colormenu, f);
        follow("PartColorSub ", submenu, f);
        follow("PaintType    ", by_name("PaintType"), f);
        follow("PaintArea    ", area, f);
    }

    // Decoded from the raw dump: state+24 is a flash command list, the same
    // shape the [flash-cmd] logger walks.
    //
    //   command  {u16 flags, u16 type, u32 args, u32 next}
    //   argument {u32 type,  u32 value, u32 next}   (type 5 = string)
    //
    //   BF23F580  0000|0000  args BF23F590  next BF23F5C0   type 0 = block
    //   BF23F590  000A|0070  args BF23F5A0  next 0          0x70 = 112
    //   BF23F5A0  type 3  value 0x690  next BF23F5B0
    //   BF23F5B0  type 1  value 5      next 0
    //
    // which is exactly the `cmd 112, 112, 52` burst that follows
    // 'activate PartColorMenu'. Walk the whole tree so the commands a menu
    // emits to populate itself can be read off and rebuilt.
    std::function<void(uint32_t, int, bool)> walk =
        [&](uint32_t node, int depth, bool is_cmd) {
        for (int guard = 0; node && guard < 64; ++guard) {
            uint32_t w0 = ReadGuestBE32(node);
            uint32_t p1 = ReadGuestBE32(node + 4);
            uint32_t p2 = ReadGuestBE32(node + 8);
            const char* pad = "                ";
            int indent = depth < 4 ? depth * 2 : 8;

            if (is_cmd) {
                MC_INFO("[carbon-cmd] {:.{}}cmd type={} flags=0x{:04X} "
                        "args=0x{:08X} next=0x{:08X}",
                        pad, indent, w0 & 0xFFFF, w0 >> 16, p1, p2);
                if (p1 >= 0xB0000000)
                    walk(p1, depth + 1, (w0 & 0xFFFF) == 0);
            } else {
                if (w0 == 5 && p1 >= 0xB0000000) {
                    MC_INFO("[carbon-cmd] {:.{}}arg str '{:.40s}'", pad, indent,
                            reinterpret_cast<const char*>(base + p1));
                } else {
                    MC_INFO("[carbon-cmd] {:.{}}arg type={} value=0x{:08X}",
                            pad, indent, w0, p1);
                    if (w0 == 0 && p1 >= 0xB0000000) walk(p1, depth + 1, true);
                }
            }
            node = p2;
        }
    };

    MC_INFO("[carbon-cmd] === PartColorMenu content ===");
    walk(ReadGuestBE32(colormenu + 24), 0, true);
    MC_INFO("[carbon-cmd] === PartColorSubMenu content ===");
    walk(ReadGuestBE32(submenu + 24), 0, true);
    MC_INFO("[carbon-cmd] === PaintType content ===");
    walk(ReadGuestBE32(by_name("PaintType") + 24), 0, true);
}

// ── Garage carbon entry ─────────────────────────────────────────────────
//
// Measured layout of the garage paint screen:
//
//   MenuStyleGarage
//   └── PartColorSubMenu
//       ├── PartColorMenu
//       │   ├── PaintWholeCar
//       │   ├── PaintArea  ── Body / Doors / Hood / FrontBumper / RearBumper / Skirts
//       │   ├── PaintType / FadeType / Color_A_Closed / PickColorA / ...
//       └── ColorPicker
//
// The carbon entry goes in as a sibling of PaintArea under PartColorMenu.
// One entry, not a nested list: the garage has no list-adapter plumbing of
// ours, so the item cycles the per-car preset and shows the result in its own
// label. Per-part switches stay in the pause menu submenu.
static void SetGarageCarbonActiveChild() {
    uint32_t item = g_garage_carbon_state.load(std::memory_order_relaxed);
    if (!item) return;

    const uint8_t mask = CarbonGetMask();
    int index = 0;
    for (int i = 0; i < CarbonPresetCount(); ++i) {
        if (CarbonPresetMaskAt(i) == mask) { index = i; break; }
    }
    WriteGuestBE32(item + kStateActiveChild, uint32_t(index));
}

static void RefreshGarageCarbonLabel() {
    std::string label = std::string("CARBON FIBER: ") + CarbonPresetName();
    for (auto& c : label) c = char(std::toupper(static_cast<unsigned char>(c)));
    EnsureStringTableText(kGarageCarbonName, label.c_str());
    SetGarageCarbonActiveChild();
}

// Value rows in this screen are a parent state plus one child per value:
// PaintType carries Metallic / Pearl, PaintWholeCar carries PaintEntireBody /
// PaintParts. Moving the highlight between values activates the child, so the
// carbon row is built the same way, one child per preset. An item with no
// children — which is what the first attempt created — has nothing to move
// between, which is exactly why it sat there inert.
// The children follow a naming convention the widget relies on: `<parent>.sN`.
// Measured in the parts screens —
//
//   activate 'Cust_Front' / 'Cust_Front.s1' .. '.s7' / '.s0'
//   activate 'PartMenu.s1' .. '.s5' / '.s0'
//   activate 'PickColorB.s0' .. '.s4'
//
// Free-form child names (the first attempt used RxCarbonP0..P6) are invisible
// to it, which is why the row rendered an empty value bracket and left/right
// did nothing at all.
static const char* GarageCarbonChildName(int i) {
    static char buf[8][40];
    std::snprintf(buf[i], sizeof(buf[0]), "%s.s%d", kGarageCarbonName, i);
    return buf[i];
}

static void EnsureGarageCarbonEntry() {
    if (g_garage_carbon_state.load(std::memory_order_relaxed)) {
        RefreshGarageCarbonLabel();
        return;
    }

    uint32_t area_name = AllocGuestString("PaintArea");
    uint32_t area = area_name ? CallGuestFn1(kFindUIObjectFn, area_name) : 0;
    uint32_t parent = area ? ReadGuestBE32(area + 32) : 0;   // PartColorMenu
    if (!parent) return;

    uint32_t item = CreateNewMenuState(kGarageCarbonName);
    if (!item) return;

    GuestAppendMenuItem(parent, item);
    // 0x42 is what every live sibling carries; bit 0x2 is the visible/enabled
    // bit (PickColorA sits at 0x40 while it is hidden).
    WriteGuestBE32(item + 16, 0x42);

    int n = CarbonPresetCount();
    if (n > 8) n = 8;
    for (int i = 0; i < n; ++i) {
        const char* key = GarageCarbonChildName(i);
        uint32_t child = CreateNewMenuState(key);
        if (!child) continue;
        GuestAppendMenuItem(item, child);
        WriteGuestBE32(child + 16, 0x42);

        std::string label = CarbonPresetNameAt(i);
        for (auto& c : label) c = char(std::toupper(static_cast<unsigned char>(c)));
        EnsureStringTableText(key, label.c_str());
    }

    g_garage_carbon_state.store(item, std::memory_order_relaxed);
    g_garage_carbon_key.store(AllocGuestString(kGarageCarbonName),
                              std::memory_order_relaxed);
    RefreshGarageCarbonLabel();  // also seeds state+12 with the active value

    // Still missing versus a live row: +24 and +48, per-state records every
    // real state owns and an injected one does not (parents additionally
    // carry +28). If the active-child index alone is not enough, those are
    // the next thing to chase — they are almost certainly where the command
    // a row emits when picked lives, which is the same gap that leaves the
    // A press with nowhere to land.

    MC_INFO("[carbon-garage] entry 0x{:08X} + {} values added under 0x{:08X}",
            item, n, parent);
}

// ── Garage carbon submenu ───────────────────────────────────────────────
//
// A real submenu, not a value row: the list adapter (list+196) is repointed at
// a menu state of ours and the vhsm stack is pushed, which is the same trick
// the pause menu submenus use. What the garage does not give us is a click —
// an injected state emits no command, so A and B are read straight from the
// pad instead. Everything else (highlight movement, rendering) stays the
// game's own.
constexpr const char* kCarbonMenuName = "RxCarbonMenu";

struct CarbonGroupDef {
    uint8_t bit;
    const char* key;
    const char* label;
};

constexpr CarbonGroupDef kCarbonGroups[] = {
    {kCarbonHood,    "RxCarbonItemHood",    "HOOD"},
    {kCarbonTrunk,   "RxCarbonItemTrunk",   "TRUNK"},
    {kCarbonDoors,   "RxCarbonItemDoors",   "DOORS"},
    {kCarbonRoof,    "RxCarbonItemRoof",    "ROOF"},
    {kCarbonBumpers, "RxCarbonItemBumpers", "BUMPERS"},
    {kCarbonBody,    "RxCarbonItemBody",    "FENDERS & SKIRTS"},
    {kCarbonSpoiler, "RxCarbonItemSpoiler", "SPOILER"},
    {kCarbonExtras,  "RxCarbonItemExtras",  "GRILL & INTERCOOLER"},
};
constexpr int kNumCarbonGroups =
    int(sizeof(kCarbonGroups) / sizeof(kCarbonGroups[0]));

static std::atomic<uint32_t> g_carbon_menu{0};
static std::atomic<uint32_t> g_carbon_items[8] = {};
static std::atomic<uint32_t> g_carbon_saved_title{0};
static std::atomic<uint32_t> g_carbon_saved_adapter{0};
static std::atomic<uint32_t> g_carbon_saved_select{0};
static std::atomic<bool> g_carbon_menu_open{false};
static std::atomic<bool> g_on_carbon_row{false};

// Each group is a value row of its own: PAINT / CARBON live in the children,
// so left and right change it the way PaintType changes between Metallic and
// Pearl. state+12 carries which value is current, and the game keeps it up to
// date once the row is built — the row is inert while it reads -1.
static const char* CarbonItemValueName(int group, int value) {
    static char buf[16][48];
    int slot = group * 2 + value;
    std::snprintf(buf[slot], sizeof(buf[0]), "%s.s%d", kCarbonGroups[group].key,
                  value);
    return buf[slot];
}

static void RefreshCarbonMenuLabels() {
    for (int i = 0; i < kNumCarbonGroups; ++i) {
        const auto& g = kCarbonGroups[i];
        EnsureStringTableText(g.key, g.label);
        EnsureStringTableText(CarbonItemValueName(i, 0), "PAINT");
        EnsureStringTableText(CarbonItemValueName(i, 1), "CARBON");

        uint32_t item = g_carbon_items[i].load(std::memory_order_relaxed);
        if (item)
            WriteGuestBE32(item + kStateActiveChild,
                           CarbonHasGroup(g.bit) ? 1u : 0u);
    }
}

static uint32_t EnsureCarbonMenu(uint32_t parent) {
    uint32_t menu = g_carbon_menu.load(std::memory_order_relaxed);
    if (menu) return menu;
    if (!parent) return 0;

    menu = CreateUIMenuState(kCarbonMenuName);
    if (!menu) return 0;

    // Owner only, not linked into the child list: it is entered by pushing it
    // on the stack, and a state with no parent crashes the root walk.
    WriteGuestBE32(menu + 32, parent);
    EnsureStringTableText(kCarbonMenuName, "CARBON FIBER");

    for (int i = 0; i < kNumCarbonGroups; ++i) {
        uint32_t item = CreateNewMenuState(kCarbonGroups[i].key);
        if (!item) continue;
        GuestAppendMenuItem(menu, item);   // builds the sibling chain
        WriteGuestBE32(item + 16, 0x42);

        // PAINT / CARBON as the row's two values.
        for (int v = 0; v < 2; ++v) {
            uint32_t value = CreateNewMenuState(CarbonItemValueName(i, v));
            if (!value) continue;
            GuestAppendMenuItem(item, value);
            WriteGuestBE32(value + 16, 0x42);
        }

        // Owner is the screen's menu, not the holder: the activate path finds
        // an item's position by walking item->parent's child list and writes
        // the result to parent+12, so it has to land on the state the screen
        // is actually drawing. The holder keeps +44 as our chain head.
        WriteGuestBE32(item + 32, parent);
        g_carbon_items[i].store(item, std::memory_order_relaxed);
    }
    // A menu row with no active child is inert; index 0 is the first item.
    WriteGuestBE32(menu + kStateActiveChild, 0);

    RefreshCarbonMenuLabels();
    g_carbon_menu.store(menu, std::memory_order_relaxed);
    MC_INFO("[carbon-garage] submenu 0x{:08X} created with {} items", menu,
            kNumCarbonGroups);
    return menu;
}

// sub_8268DD70 — the activate path navigation itself goes through: it finds
// the state's index in its parent's child list, writes it to parent+12, then
// runs the state's entry virtuals. Calling it is how a highlight move is
// reproduced from our side.
constexpr uint32_t kActivateStateFn = 0x8268DD70;

// Replaying a menu's own content is what repaints this screen. Activating a
// state is not enough — measured: the nudge fired 'activate RxCarbonItemHood'
// and the rows still only appeared after the highlight was moved by hand.
// What entering PartColorMenu really does is run its content, cmd 112
// (0x690,5) / cmd 112 (0x6BB,6) / cmd 52 (0x682,0x688).
//
// Those command objects already exist in guest memory, so nothing has to be
// synthesized: hand each one back to the dispatcher. The dispatcher's other
// three arguments are captured from a real call rather than guessed.
constexpr uint32_t kGarageDispatchFn = 0x826AB520;

static std::atomic<uint32_t> g_dispatch_a1{0};
static std::atomic<uint32_t> g_dispatch_a2{0};
static std::atomic<uint32_t> g_dispatch_a4{0};

// cmd 90 is this screen's refresh: it trails every value change (42, 43, 87
// are always followed by it). Rather than build one, keep the pointer to a
// real one as it goes past the dispatcher and hand that same object back.
static std::atomic<uint32_t> g_cmd_refresh{0};

static void ReplayRefreshCommand() {
    uint32_t cmd = g_cmd_refresh.load(std::memory_order_relaxed);
    uint32_t a1 = g_dispatch_a1.load(std::memory_order_relaxed);
    if (!cmd || !a1) return;
    CallGuestFn4(kGarageDispatchFn, a1,
                 g_dispatch_a2.load(std::memory_order_relaxed), cmd,
                 g_dispatch_a4.load(std::memory_order_relaxed));
}

static void ExecuteStateContent(uint32_t state) {
    uint32_t a1 = g_dispatch_a1.load(std::memory_order_relaxed);
    if (!state || !a1) return;

    uint32_t a2 = g_dispatch_a2.load(std::memory_order_relaxed);
    uint32_t a4 = g_dispatch_a4.load(std::memory_order_relaxed);

    for (uint32_t cmd = ReadGuestBE32(state + 24), guard = 0;
         cmd && guard < 32; cmd = ReadGuestBE32(cmd + 8), ++guard) {
        const uint16_t type = uint16_t(ReadGuestBE32(cmd) & 0xFFFF);
        if (type == 0) {
            // A block: its args are a nested command list.
            for (uint32_t inner = ReadGuestBE32(cmd + 4), g2 = 0;
                 inner && g2 < 32; inner = ReadGuestBE32(inner + 8), ++g2)
                CallGuestFn4(kGarageDispatchFn, a1, a2, inner, a4);
        } else {
            CallGuestFn4(kGarageDispatchFn, a1, a2, cmd, a4);
        }
    }
}

// The screen's rows are generic: PaintType's whole content is a single
// `cmd 49 <id>`, and our own row rendered and navigated with no content at
// all. What is NOT generic are the flash element ids in a menu's content
// (0x690 / 0x6BB / 0x682 / 0x688) — those address clips that exist in
// GARAGEMOVIE, so a brand new screen cannot be conjured.
//
// So the submenu reuses this screen instead of adding one: swap the children
// of PartColorMenu for our carbon items and re-activate it. Same flash
// elements, same row machinery, different rows. B swaps them back.
//
// KNOWN, ACCEPTED: the swap does not repaint on its own — the new rows appear
// on the first press of a direction after A. Four ways were measured and none
// of them redraws:
//   1. re-running the state's entry virtual (vtable+144)
//   2. driving the activate path by hand (sub_8268DD70) on the first item
//   3. replaying the menu's own content through the dispatcher, which does
//      emit the real cmd 112 / 112 / 52 burst — confirmed in the log
//   4. replaying a captured cmd 90, the refresh that trails every value change
// All four fire and the screen still waits. What repaints is the menu's own
// input-to-transition path, which only runs on a real navigation event.
// Forcing that means reaching into input processing; not worth risking what
// works for one button press.
static void OpenCarbonMenu() {
    uint32_t item = g_garage_carbon_state.load(std::memory_order_relaxed);
    uint32_t parent = item ? ReadGuestBE32(item + 32) : 0;  // PartColorMenu
    uint32_t menu = EnsureCarbonMenu(parent);
    if (!menu || !parent) return;

    uint32_t first = ReadGuestBE32(menu + 44);
    if (!first) return;

    g_carbon_saved_adapter.store(ReadGuestBE32(parent + 44),
                                 std::memory_order_relaxed);
    g_carbon_saved_select.store(ReadGuestBE32(parent + kStateActiveChild),
                                std::memory_order_relaxed);

    // The screen title is the menu state's own label at +48 ("PAINT SHOP").
    g_carbon_saved_title.store(ReadGuestBE32(parent + 48),
                               std::memory_order_relaxed);
    static uint32_t title = 0;
    if (!title) title = AllocGuestString("CARBON FIBER");
    if (title) WriteGuestBE32(parent + 48, title);

    RefreshCarbonMenuLabels();
    WriteGuestBE32(parent + 44, first);
    WriteGuestBE32(parent + kStateActiveChild, 0);

    // Re-running the state's entry work is not what repaints this screen —
    // measured, no cmd 112/52 followed it, and the rows only changed once the
    // highlight was moved by hand. Moving the highlight is the repaint, so
    // drive it: sub_8268DD70 is the activate path the navigation itself uses.
    // a2 is NOT a flag: sub_8268DD70 passes it straight to vtable+140 as a
    // pointer (`if (a2) vfunc140(state, a2)`), so the 1 that used to go here
    // was dereferenced and crashed on close. 0 keeps the index bookkeeping
    // and skips the entry virtuals.
    CallGuestFn2(kActivateStateFn, first, 0);
    ExecuteStateContent(parent);
    ReplayRefreshCommand();

    g_carbon_menu_open.store(true, std::memory_order_relaxed);
    MC_INFO("[carbon-garage] submenu opened (children of 0x{:08X} swapped)",
            parent);
}

static void CloseCarbonMenu() {
    uint32_t item = g_garage_carbon_state.load(std::memory_order_relaxed);
    uint32_t parent = item ? ReadGuestBE32(item + 32) : 0;
    if (parent) {
        uint32_t restored =
            g_carbon_saved_adapter.load(std::memory_order_relaxed);
        uint32_t title = g_carbon_saved_title.load(std::memory_order_relaxed);
        if (title) WriteGuestBE32(parent + 48, title);
        WriteGuestBE32(parent + 44, restored);
        WriteGuestBE32(parent + kStateActiveChild,
                       g_carbon_saved_select.load(std::memory_order_relaxed));
        if (restored) CallGuestFn2(kActivateStateFn, restored, 0);
        ExecuteStateContent(parent);
        ReplayRefreshCommand();
    }
    g_carbon_menu_open.store(false, std::memory_order_relaxed);
    MC_INFO("[carbon-garage] submenu closed");
}

// sub_826AB520 is the garage command dispatcher. It switches on a halfword:
//
//   lhz  r11, 2(r5)          ; command type, same layout as [flash-cmd]
//   cmplwi r11, 0xBC         ; 189 cases
//
// So garage menu items act by emitting a numbered command, and an injected
// item emits nothing — which is why clicking it did nothing at all. Unlike
// the pause menu there is no click handler of ours to piggyback on either:
// Hook_PMLodTrafficClick sits at 0x826686B8, inside the pause menu
// controller, and never runs here.
//
// This logs what actually arrives while the paint screen is up: the command
// type plus the list selection at the moment it fires. With the type that
// shows up when our entry is picked (if any), the entry can become a real
// submenu driven by the list adapter, exactly like the pause menu one.
void Hook_CarbonGarageSelect(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5,
                             PPCRegister& r6) {
    // Captured so the menu's own content can be replayed later through the
    // same dispatcher, with the same context the game passes.
    g_dispatch_a1.store(static_cast<uint32_t>(r3.u64), std::memory_order_relaxed);
    g_dispatch_a2.store(static_cast<uint32_t>(r4.u64), std::memory_order_relaxed);
    g_dispatch_a4.store(static_cast<uint32_t>(r6.u64), std::memory_order_relaxed);

    uint32_t cmd = static_cast<uint32_t>(r5.u64);
    if (!cmd || !GetMembase()) return;

    int16_t type = static_cast<int16_t>(ReadGuestBE16(cmd + 2));
    if (type == 90) g_cmd_refresh.store(cmd, std::memory_order_relaxed);

    if (!g_garage_carbon_state.load(std::memory_order_relaxed)) return;
    if (!CvarGetBool("carbon_menu_diag")) return;
    uint32_t list = g_last_list.load(std::memory_order_relaxed);
    uint32_t sel = list ? ReadGuestBE32(list + 184) : 0xFFFFFFFFu;

    MC_INFO("[carbon-garage] cmd type={} sel={} list=0x{:08X}", type, sel, list);

    uint32_t key = g_garage_carbon_key.load(std::memory_order_relaxed);
    if (key && IsGuestButtonClicked(key)) {
        CarbonCyclePreset();
        RefreshGarageCarbonLabel();
        MC_INFO("[carbon-garage] entry picked, preset now '{}'",
                CarbonPresetName());
    }
}

// Measured: activation fires when the highlight ENTERS a state, not when it is
// clicked. Moving the cursor over the entry and back cycled the preset with no
// button press at all, so this must not act on activation. Clicks show up as
// commands instead — hovering PaintType logged
//
//   activate 'PaintType' / activate 'Metallic'   <- highlight + current value
//   cmd type=42 / cmd type=90                    <- the actual press
//   activate 'PaintType'
//
// and an injected state emits no command, so the press still has nowhere to
// land. Activation is kept as the "which entry is highlighted" signal and
// logged; the value only follows once the entry has children of its own, the
// way PaintType carries Metallic and PaintArea carries its six zones.
// A and B, read from the pad. The garage never routes a press from an injected
// state anywhere — no command id, nothing for the dispatcher to switch on — so
// this is the only way to know the row was picked. Edge-triggered, and it only
// looks at the pad while the paint screen is up and the highlight is on the
// carbon row (or inside the submenu), so it cannot eat input anywhere else.
// Reading left/right off the pad here was wrong: the pause menu already binds
// those to switching tabs, so it fought the game and the item still rendered
// without the native `< >` arrows. Those arrows are not decoration — the
// widget draws them for a row that HAS values, i.e. a state with children, and
// then it handles left/right itself. So the items carry `.sN` children now,
// the same shape that made the garage rows work, and nothing polls the pad.

void Hook_CarbonUiTick(PPCRegister& r3) {
    (void)r3;

    // Pad edges are tracked once and shared by both consumers.
    static uint16_t prev_buttons = 0;
    uint16_t pressed = 0;
    {
        auto* isys = static_cast<rex::input::InputSystem*>(
            rex::Runtime::instance()->input_system());
        if (isys) {
            rex::input::X_INPUT_STATE st{};
            if (isys->GetState(0, &st) == 0) {
                const uint16_t buttons = st.gamepad.buttons;
                pressed = uint16_t(buttons & ~prev_buttons);
                prev_buttons = buttons;
            }
        }
    }

    if (!g_garage_carbon_state.load(std::memory_order_relaxed)) return;

    const bool open = g_carbon_menu_open.load(std::memory_order_relaxed);
    if (!open && !g_on_carbon_row.load(std::memory_order_relaxed)) return;

    // One line the first time the tick actually runs with the highlight on our
    // row: if it never appears, this hook is not the per-frame path in the
    // garage and the pad is never being read.
    static std::atomic_bool announced{false};
    if (!announced.exchange(true))
        MC_INFO("[carbon-garage] ui tick alive on carbon row");

    constexpr uint16_t kPadA = rex::input::X_INPUT_GAMEPAD_A;
    constexpr uint16_t kPadB = rex::input::X_INPUT_GAMEPAD_B;

    if (!open) {
        if (pressed & kPadA) OpenCarbonMenu();
        return;
    }

    if (pressed & kPadB) {
        CloseCarbonMenu();
        return;
    }

    // Nothing else to read once open: each row carries PAINT / CARBON as its
    // own values, so left and right do the work through the game's own
    // navigation and the value child's activation is what applies it.
}

// ── Pause menu probe ────────────────────────────────────────────────────
//
// The settings tab list draws labels only, but Start > PM_GameOptions > Game
// clearly renders real `< value >` rows — so PAUSEMOVIE does have the widget,
// just not on the list our submenus hang off. This dumps those screens the
// same way the garage ones were mapped: fields, children and content, once
// per state, so the shape that produces the arrows can be copied.
static void DumpUiState(const char* what, uint32_t node) {
    uint8_t* base = GetMembase();
    if (!base || !node) return;

    auto name_of = [&](uint32_t s) -> const char* {
        uint32_t p = s ? ReadGuestBE32(s + 20) : 0;
        return p ? reinterpret_cast<const char*>(base + p) : "<unnamed>";
    };

    char line[256];
    size_t pos = 0;
    for (int i = 0; i < 16 && pos < sizeof(line) - 12; ++i) {
        int w = std::snprintf(line + pos, sizeof(line) - pos, "%08X ",
                              ReadGuestBE32(node + uint32_t(i * 4)));
        if (w < 0) break;
        pos += size_t(w);
    }
    line[pos] = 0;

    MC_INFO("[pm-probe] {} 0x{:08X} '{}' parent='{}' active={} : {}", what, node,
            name_of(node), name_of(ReadGuestBE32(node + 32)),
            int32_t(ReadGuestBE32(node + kStateActiveChild)), line);

    int i = 0;
    for (uint32_t c = ReadGuestBE32(node + 44); c && i < 24;
         c = ReadGuestBE32(c + 36), ++i) {
        MC_INFO("[pm-probe]   child[{}] 0x{:08X} flags=0x{:X} active={} "
                "kids=0x{:08X} label48=0x{:08X} '{}'",
                i, c, ReadGuestBE32(c + 16),
                int32_t(ReadGuestBE32(c + kStateActiveChild)),
                ReadGuestBE32(c + 44), ReadGuestBE32(c + 48), name_of(c));
    }

    uint32_t content = ReadGuestBE32(node + 24);
    for (int g = 0; content && g < 16; content = ReadGuestBE32(content + 8), ++g) {
        MC_INFO("[pm-probe]   cmd type={} flags=0x{:04X} args=0x{:08X}",
                ReadGuestBE32(content) & 0xFFFF, ReadGuestBE32(content) >> 16,
                ReadGuestBE32(content + 4));
    }
}

// ── Garage tire offset row ──────────────────────────────────────────────
//
// The wheels > dimensions screen is a list of value rows: a parent state per
// dimension with one `<parent>.sN` child per value, exactly the shape the
// carbon entry copies. Retail builds it from wheels.sc.xml, which declares
// only four — RimSize (arg 1), TireProfile (2), TireWidth (3), RideHeight (4)
// — each one an `<include src="elements/RimDimension.sc" count=N arg=X>` whose
// children call garage.UpdateRimDimensions(X) -> sub_826A5BA8. That handler
// switches on 0..4 and nothing else, so there is no fifth dimension to enable:
// this row is ours end to end.
//
// The value it edits is not ours though. sub_82394388 reflects TireOffset0 and
// TireOffset1 as u8 at reflBase+2040/+2041 (reflBase = CustomData + 0x40), one
// per axle, right below RideHeight at +2042. They are parsed, they ride in the
// save, and they are read by absolutely nothing — the only code that touches
// them is the customization ctor sub_82395138, which zeroes the dword at
// 0x82395550 and then hands defaults to RideHeight and TireWidth only. A dead
// MC3 field with a save slot already cut for it, which is why the offset lives
// there instead of in a cvar: it belongs to the car, like paint or rims.
//
// Writing it changes nothing on screen yet. The wheel transform that would
// consume it does not exist either (sub_82354230 builds the wheel matrices
// from radius/width alone); that is the next step. This step is the row, the
// value and the round trip through the save.
constexpr const char* kTireOffsetName = "RxTireOffset";
constexpr int kTireOffsetMin   = -8;
constexpr int kTireOffsetMax   = 8;
constexpr int kTireOffsetCount = kTireOffsetMax - kTireOffsetMin + 1;

// sub_826A5BA8's own path to the car being customized:
//   v6 = *(sub_822A3998(dword_82874374, 0) + 48); reflBase = *(v6 + 132) + 64
constexpr uint32_t kGarageCtxGlobal = 0x82874374;
constexpr uint32_t kGarageCtxFn     = 0x822A3998;
constexpr uint32_t kReflTireOffset  = 2040;  // +0 front, +1 rear
constexpr uint32_t kReflDirtyFlags  = 2064;  // sub_826A5BA8 tail: |= 0x81

static std::atomic<uint32_t> g_tire_offset_state{0};

static uint32_t GarageReflBase() {
    if (!GetMembase()) return 0;
    const uint32_t ctx = ReadGuestBE32(kGarageCtxGlobal);
    if (!ctx) return 0;
    const uint32_t obj = CallGuestFn2(kGarageCtxFn, ctx, 0);
    if (!obj) return 0;
    const uint32_t veh = ReadGuestBE32(obj + 48);
    if (!veh) return 0;
    const uint32_t custom = ReadGuestBE32(veh + 132);
    if (!custom) return 0;
    return custom + 64;
}

// 0 = front, 1 = rear, 2 = both — read exactly the way sub_826A5BA8 does it:
// find Axle_Menu, call its vfunc336, cache nothing. state+12 is the fallback
// for the case where the vtable slot is not there to call.
static int GarageAxleSelection() {
    static uint32_t name_str = 0;
    if (!name_str) name_str = AllocGuestString("Axle_Menu");
    const uint32_t menu = name_str ? CallGuestFn1(kFindUIObjectFn, name_str) : 0;
    if (!menu) return 0;

    const uint32_t vtbl = ReadGuestBE32(menu);
    const uint32_t fn = vtbl ? ReadGuestBE32(vtbl + 336) : 0;
    const int32_t active = fn ? int32_t(CallGuestFn1(fn, menu))
                              : int32_t(ReadGuestBE32(menu + kStateActiveChild));
    return (active >= 0 && active <= 2) ? int(active) : 0;
}

static int TireOffsetGet(int axle) {
    const uint32_t refl = GarageReflBase();
    if (!refl) return 0;
    return int(int8_t(ReadGuestU8(refl + kReflTireOffset + uint32_t(axle & 1))));
}

static void TireOffsetSet(int axle, int value) {
    const uint32_t refl = GarageReflBase();
    if (!refl) return;
    if (value < kTireOffsetMin) value = kTireOffsetMin;
    if (value > kTireOffsetMax) value = kTireOffsetMax;

    const uint8_t byte = uint8_t(int8_t(value));
    if (axle == 2) {
        WriteGuestU8(refl + kReflTireOffset + 0, byte);
        WriteGuestU8(refl + kReflTireOffset + 1, byte);
    } else {
        WriteGuestU8(refl + kReflTireOffset + uint32_t(axle & 1), byte);
    }
    // Same dirty bits the four stock dimensions raise when they commit.
    WriteGuestBE32(refl + kReflDirtyFlags,
                   ReadGuestBE32(refl + kReflDirtyFlags) | 0x81u);
}

static const char* TireOffsetChildName(int i) {
    static char buf[kTireOffsetCount][32];
    std::snprintf(buf[i], sizeof(buf[0]), "%s.s%d", kTireOffsetName, i);
    return buf[i];
}

// The row draws its own label and then the active child's text as the value,
// so the label is a plain name — putting the number in it too is what printed
// "TIRE OFFSET:+8 <+8>". Only state+12 has to track the value.
static void RefreshTireOffsetLabel() {
    const int axle = GarageAxleSelection();
    const int value = TireOffsetGet(axle == 1 ? 1 : 0);

    EnsureStringTableText(kTireOffsetName, "TIRE OFFSET");

    const uint32_t item = g_tire_offset_state.load(std::memory_order_relaxed);
    if (item) {
        WriteGuestBE32(item + kStateActiveChild,
                       uint32_t(value - kTireOffsetMin));
    }
}

static void EnsureTireOffsetEntry() {
    if (!CvarGetBool("garage_tire_offset")) return;

    // Axle_Menu is DimensionMenu's first child in wheels.sc.xml, so its parent
    // is the row list we want. Appending puts us after RideHeight.
    static uint32_t axle_name = 0;
    if (!axle_name) axle_name = AllocGuestString("Axle_Menu");
    const uint32_t axle = axle_name ? CallGuestFn1(kFindUIObjectFn, axle_name) : 0;
    const uint32_t parent = axle ? ReadGuestBE32(axle + 32) : 0;
    if (!parent) return;

    // A rebuilt screen leaves the cached state dangling, so re-check that it
    // is still our node and still hanging off the list before reusing it.
    const uint32_t cached = g_tire_offset_state.load(std::memory_order_relaxed);
    if (cached) {
        uint8_t* base = GetMembase();
        const uint32_t name_ptr = base ? ReadGuestBE32(cached + 20) : 0;
        const bool intact =
            name_ptr && ReadGuestBE32(cached + 32) == parent &&
            std::strcmp(reinterpret_cast<const char*>(base + name_ptr),
                        kTireOffsetName) == 0;
        if (intact) {
            RefreshTireOffsetLabel();
            return;
        }
        g_tire_offset_state.store(0, std::memory_order_relaxed);
        MC_INFO("[tire-offset] cached row 0x{:08X} went stale, rebuilding", cached);
    }

    const uint32_t item = CreateNewMenuState(kTireOffsetName);
    if (!item) return;

    GuestAppendMenuItem(parent, item);
    WriteGuestBE32(item + 16, 0x42);  // visible/enabled, same as every sibling

    for (int i = 0; i < kTireOffsetCount; ++i) {
        const char* key = TireOffsetChildName(i);
        const uint32_t child = CreateNewMenuState(key);
        if (!child) continue;
        GuestAppendMenuItem(item, child);
        WriteGuestBE32(child + 16, 0x42);

        char value[16];
        std::snprintf(value, sizeof(value), "%+d", i + kTireOffsetMin);
        EnsureStringTableText(key, value);
    }

    g_tire_offset_state.store(item, std::memory_order_relaxed);
    RefreshTireOffsetLabel();

    MC_INFO("[tire-offset] row 0x{:08X} + {} values under 0x{:08X}", item,
            kTireOffsetCount, parent);
}

// Returns true when the activated state belongs to the offset row, so the
// carbon dispatch below does not also look at it.
static bool TireOffsetOnStateActivate(const char* name) {
    if (!name) return false;

    if (std::strcmp(name, "DimensionMenu") == 0) {
        EnsureTireOffsetEntry();
        return false;  // the screen itself, not our row
    }

    if (!g_tire_offset_state.load(std::memory_order_relaxed)) return false;

    // The axle chooser decides which byte the row shows and edits.
    if (std::strcmp(name, "Front_Axle") == 0 ||
        std::strcmp(name, "Rear_Axle") == 0 ||
        std::strcmp(name, "Both_Axle") == 0) {
        RefreshTireOffsetLabel();
        return false;
    }

    const size_t len = std::strlen(kTireOffsetName);
    if (std::strncmp(name, kTireOffsetName, len) != 0) return false;

    if (name[len] == '\0') {  // highlight landed on the row
        RefreshTireOffsetLabel();
        return true;
    }

    // '<row>.sN' — N is two digits past .s9, so parse it, don't index a char.
    if (name[len] != '.' || name[len + 1] != 's') return false;
    const int index = std::atoi(name + len + 2);
    if (index < 0 || index >= kTireOffsetCount) return true;

    const int value = index + kTireOffsetMin;
    const int axle = GarageAxleSelection();
    TireOffsetSet(axle, value);
    RefreshTireOffsetLabel();
    MC_INFO("[tire-offset] axle {} -> {:+d}", axle, value);
    return true;
}

void CarbonOnStateActivate(const char* name, uint32_t state) {
    if (name && CvarGetBool("carbon_menu_diag")) {
        // Arming by state name does not work: the whole tree activates once
        // while it is built at boot, PM_GameOptions included, so the dump
        // budget was spent before the menu was ever on screen. g_pm_probe_armed
        // is set from Hook_EnablePMSave instead, which only runs when the
        // pause menu is really being shown.
        if (g_pm_probe_armed.load(std::memory_order_relaxed)) {
            MC_INFO("[pm-probe] activate '{}' 0x{:08X}", name, state);
            if (state) {
                bool known = false;
                for (int i = 0; i < g_pm_seen_count; ++i)
                    if (g_pm_seen[i] == state) { known = true; break; }
                if (!known && g_pm_seen_count < 64) {
                    g_pm_seen[g_pm_seen_count++] = state;
                    DumpUiState("state", state);
                    DumpUiState("parent", ReadGuestBE32(state + 32));
                }
            }
        }
    }

    if (TireOffsetOnStateActivate(name)) return;

    if (!name || !g_garage_carbon_state.load(std::memory_order_relaxed)) return;

    // Highlight tracking: the tick only reads the pad while the cursor is
    // actually parked on the carbon row.
    const bool ours = std::strncmp(name, kGarageCarbonName,
                                   std::strlen(kGarageCarbonName)) == 0 ||
                      std::strncmp(name, "RxCarbonItem", 12) == 0;
    g_on_carbon_row.store(ours, std::memory_order_relaxed);

    if (std::strcmp(name, kGarageCarbonName) == 0) {
        RefreshGarageCarbonLabel();
        return;
    }

    // Inside the submenu each row is its own value pair: '<group>.s0' is
    // PAINT, '.s1' is CARBON, and the highlight landing on one applies it.
    if (std::strncmp(name, "RxCarbonItem", 12) == 0) {
        size_t len = std::strlen(name);
        if (len > 3 && name[len - 3] == '.' && name[len - 2] == 's') {
            const int value = name[len - 1] - '0';
            for (int i = 0; i < kNumCarbonGroups; ++i) {
                if (std::strncmp(name, kCarbonGroups[i].key,
                                 std::strlen(kCarbonGroups[i].key)) != 0)
                    continue;
                const bool want = (value == 1);
                if (CarbonHasGroup(kCarbonGroups[i].bit) != want) {
                    CarbonToggleGroup(kCarbonGroups[i].bit);
                    MC_INFO("[carbon-garage] {} -> {}", kCarbonGroups[i].label,
                            want ? "CARBON" : "PAINT");
                }
                break;
            }
        }
        return;
    }

    // A value child activating means the highlight landed on that preset,
    // which is the same thing 'Metallic' / 'Pearl' mean for PaintType.
    static const size_t kPrefixLen = std::strlen(kGarageCarbonName) + 2;
    if (std::strncmp(name, kGarageCarbonName, kPrefixLen - 2) == 0 &&
        name[kPrefixLen - 2] == '.' && name[kPrefixLen - 1] == 's' &&
        name[kPrefixLen] >= '0' && name[kPrefixLen] <= '9' &&
        name[kPrefixLen + 1] == '\0') {
        int index = name[kPrefixLen] - '0';
        CarbonApplyPresetIndex(index);
        RefreshGarageCarbonLabel();
        MC_INFO("[carbon-garage] value {} -> '{}'", index, CarbonPresetName());
    }
}

// sub_826309E0 is the container key dispatch: it walks the children at +68
// (count u16 at +72) and stops at the first one whose vfunc32 consumes the
// key. The pause screen lists the tab strip before its list, so left and
// right are eaten there and the list — and with it the selected row — never
// sees them. That is why the arrows work on the game's own Options > Game
// rows but not on ours: that screen is a different one, with no tab strip
// in front of its list.
//
// Hooked past the prologue, where r31 is the container, r28 the key and r27
// the dispatch argument; returning true jumps to the "consumed" exit. While
// a RexGlue submenu is open, left and right go straight to the pause list,
// skipping every sibling ahead of it. Nothing reads the pad here — this is
// still the game routing its own key to its own widget.
// Two child arrays have to be walked, not one. sub_82667D88 builds the pause
// screen like this:
//
//   screen+68   -> { screen+2192, screen+176 (the tab strip) }
//   tabstrip+180 -> { pause_tab_mainmenu, _gamemodes, _settings, ... }
//   pause_tab_settings+68 -> { the list we drive }
//
// so the tab strip keeps its tabs at +180/+184 while everything else uses
// +68/+72. Walking only +68 stopped at the tab strip, which is exactly the
// widget that eats left and right — and why the first attempt reported
// holds=false for every container.
//
// +180 is a plain dword on other classes (on the list itself it is the
// packed row count), so it is only followed when it reads as a heap pointer
// with a sane count behind it.
static bool NodeReachesList(uint32_t node, uint32_t list, int depth,
                            int& budget) {
    if (!node || depth > 8 || --budget < 0) return false;

    const uint32_t arrays[2] = {68, 180};
    for (uint32_t off : arrays) {
        uint32_t arr = ReadGuestBE32(node + off);
        uint16_t n = ReadGuestBE16(node + off + 4);
        if (arr < 0xB0000000 || !n || n > 64) continue;
        for (uint16_t i = 0; i < n; ++i) {
            uint32_t child = ReadGuestBE32(arr + uint32_t(4 * i));
            if (child == list) return true;
            if (child >= 0xB0000000 &&
                NodeReachesList(child, list, depth + 1, budget))
                return true;
        }
    }
    return false;
}

bool Hook_MenuKeyDispatch(PPCRegister& r31, PPCRegister& r28,
                          PPCRegister& r27) {
    if (g_active_menu.load(std::memory_order_relaxed) < 0) return false;

    const uint32_t key = uint32_t(r28.u64) & 0xFF;
    if (key < '3' || key > '6') return false;  // left '3'/'5', right '4'/'6'

    uint32_t list = g_pauselist_addr.load(std::memory_order_relaxed);
    if (!list) return false;

    uint32_t container = static_cast<uint32_t>(r31.u64);
    int budget = 512;
    const bool holds = NodeReachesList(container, list, 0, budget);

    if (CvarGetBool("carbon_menu_diag"))
        MC_INFO("[pm-key] key='{}' container=0x{:08X} list=0x{:08X} holds={}",
                char(key), container, list, holds);

    if (!holds) return false;

    uint32_t vtable = ReadGuestBE32(list);
    uint32_t fn = vtable ? ReadGuestBE32(vtable + 32) : 0;
    if (!fn) return false;

    CallGuestFn3(fn, list, key, static_cast<uint32_t>(r27.u64));
    return true;
}

bool Hook_PopulateRedirect(PPCRegister& r3) {
    int m = g_active_menu.load(std::memory_order_relaxed);
    uint32_t settings = g_settingsmenu_addr.load(std::memory_order_relaxed);
    uint32_t state = static_cast<uint32_t>(r3.u64);

    if (m < 0 || !settings || state != settings)
        return false;

    uint32_t sub = g_menu_state[m].load(std::memory_order_relaxed);
    if (sub) {
        r3.u64 = sub;
        MC_INFO("[pause-menu] populate redirected to '{}' 0x{:08X}",
                kMenus[m].label, sub);
    }
    return false;
}

bool Hook_RexGlueCancel(PPCRegister& r31) {
    if (g_active_menu.load(std::memory_order_relaxed) < 0)
        return false;

    g_active_menu.store(-1, std::memory_order_relaxed);
    GuestStackPop();

    uint32_t controller = static_cast<uint32_t>(r31.u64);
    RestoreTabDisplay(controller);

    MC_INFO("[pause-menu] left RexGlue submenu");
    return true;
}

#else // REXGLUE_HAS_XEO3_TARGET
// XEO3 stubs: empty implementations so the linker resolves codegen calls.

#include <rex/ppc/context.h>

void Hook_CapturePMContinue(PPCRegister& r3) {}
bool Hook_EnablePMSave(PPCRegister& r3, PPCRegister& r4) { return false; }
bool Hook_EnablePMTeste(PPCRegister& r3, PPCRegister& r4) { return false; }
bool Hook_PMLodTrafficClick(PPCRegister& r3, PPCRegister& r31) { return false; }
bool Hook_ListViewPopulate(PPCRegister& r3) { return false; }
bool Hook_ControllerMenuKey(PPCRegister& r3, PPCRegister& r4) { return false; }
bool Hook_MenuKeyDispatch(PPCRegister& r31, PPCRegister& r28,
                          PPCRegister& r27) { return false; }
void Hook_CarbonGarageProbe(PPCRegister& r3) {}
void Hook_CarbonGarageSelect(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5,
                             PPCRegister& r6) {}
void Hook_CarbonUiTick(PPCRegister& r3) {}
void CarbonOnStateActivate(const char* name, uint32_t state) {}
bool Hook_FlashCommandLog(PPCRegister& r5) { return false; }
bool Hook_PopulateRedirect(PPCRegister& r3) { return false; }
bool Hook_RexGlueCancel(PPCRegister& r31) { return false; }
#endif // REXGLUE_HAS_XEO3_TARGET
