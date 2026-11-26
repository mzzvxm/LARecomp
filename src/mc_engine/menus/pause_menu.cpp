#ifndef REXGLUE_HAS_XEO3_TARGET
#include "mc_engine/core/restart.h"
#include "settings/immortal.h"
#include "mc_engine/core/world_detail.h"
#include "pause_menu.h"
#include "mc_engine/gameplay/carbon_parts.h"
#include "mc_engine/gameplay/tire_camber.h"
#include "mc_engine/camera/camera_look.h"
#include "mc_engine/core/config_save.h"
#include "mc_engine/gameplay/cutscene_gallery.h"
#include "debug_menu.h"
#include "debug_menu_pages.h"
#include "display_menu.h"
#include "mc_engine/display/display_mode.h"
#include "frame_generation_menu.h"
#include "menu_items.h"
#include "pause_parity_rows.h"
#include "settings/ui_expr_auto.h"
#include "settings/ui_expr_rules.h"
#include "ui_expr_data.h"
#include "upscaling_menu.h"
#include "settings_ui/menu_catalog.h"
#include "mc_engine/core/logging.h"
#include "mc_engine/modloader/modloader.h"
#include "mc_engine/online/online_platform.h"
#include "mc_engine/online/online_toast.h"
#include "mc_engine/online/server_browser/browser_ui.h"
#include "ui_forms.h"
#include "ui_palette.h"
#include "mc_engine/core/save_protect.h"
#include "mc_engine/display/ultrawide.h"
#include "mc_engine/online/system_link.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <functional>
#include <memory>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "settings/larecomp_settings.h"
#include <rex/platform/file.h>
#include <rex/xam/input.h>
#include <rex/hid/input_system.h>
#include <rex/kernel/title_runtime.h>
#include <rex/ppc/call.h>
#include <rex/kernel/function_dispatcher.h>
#include <rex/app/app.h>
#include <rex/hid/pad.h>
#include <rex/ui/ui_system.h>

#include "mc_engine/sdk_access.h"
#include "larecomp_guest.h"

#if defined(LARECOMP_HAS_GPU)
#include "gpu/car_shadow_map_rules.h"
#include "gpu/city_refl_setting.h"
#include "gpu/city_rt_rules.h"
#include "gpu/gfx_preset_apply.h"
#include "gpu/gi_setting.h"
#include "gpu/rtao_setting.h"
#include "gpu/gpu.h"
#include "gpu/gpu_class.h"
#include "gpu/rt_tier.h"
#include "gpu/latency.h"
#include "gpu/local_shadow_rules.h"
#include "gpu/renderer_choice.h"
#include "gpu/shadow_filter.h"
#include "gpu/side_views.h"
#include "gpu/vram_warning.h"
#endif

// hooks/ui.cpp: the glyph set in use (button_prompts, auto resolved).
bool ButtonPromptsArePlayStation();

namespace {

std::atomic<uint32_t> g_pausetab_addr{0};
std::atomic<uint32_t> g_settingsmenu_addr{0};

constexpr uint32_t kStringTableGlobal  = 0x8286D7FC;
constexpr uint32_t kVHSMGlobal         = 0x8286D804;
constexpr uint32_t kAllocStateFn       = 0x8268D578;
constexpr uint32_t kUIObjectCtorFn     = 0x8263AD60;
constexpr uint32_t kUIMenuVtable       = 0x82020654;
constexpr uint32_t kGuestMallocFn      = 0x82130528;
constexpr uint32_t kStackPopFn         = 0x8268DFF0;
constexpr uint32_t kIsButtonClickedFn  = 0x82661508;

constexpr uint32_t kFindMovieFn       = 0x821F9FB8;
constexpr uint32_t kSetFlashIntFn     = 0x825EE0E0;
constexpr uint32_t kGetFlashIntFn     = 0x825EE270;
constexpr uint32_t kStringLookupFn    = 0x82218310;
constexpr uint32_t kPopulateMenuFn    = 0x82220308;
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
// Every row's "enabled" byte, u8, 1 = enabled: the base constructor
// (sub_82633D40) and 0x8208E83C's (sub_826349F0) store 1 there; vfunc72 of all
// four row vtables reads it (0x82633D30, lbz r3,0xB8(r3)) and vfunc76 writes
// it. A value row with 0 renders greyed (sub_82631A20 writes the Flash type
// as (cntlzw(vfunc72) >> 4) & 2: 0 enabled, 2 disabled) and its left/right
// handler (sub_826316E0) returns 0 without moving the select; the list's A
// ('7', sub_826334D0) also asks vfunc72 before it acts. Cursor moves do not
// skip such a row. The pause movie draws a type 2 row as its label in grey
// and no value at all (the render still writes the values).
constexpr uint32_t kRowValueEnabled = 0xB8;

constexpr uint32_t kListRows        = 176;  // row pointer array
constexpr uint32_t kListCountCap    = 180;  // count:u16 then capacity:u16
constexpr uint32_t kListSelect      = 184;

constexpr uint32_t kStr_PAUSEMOVIE      = 0x8201D028;
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

// The mcListViewFixed most recently rendered (Hook_ListViewPopulate); the
// garage carbon diagnostics read its selection.
std::atomic<uint32_t> g_last_list{0};
// The last list the renderer will actually draw: the active flag (+16 & 0x20, sub_8221CDD0, the first thing sub_82631FD0 asks) is set. The pause screen prepares the lists of several tabs
// and offers the inactive ones to the render hook too; the mouse must point at the one on screen. Read by PauseMenuLastList.
std::atomic<uint32_t> g_last_active_list{0};
// The UIMenu FlashNavigator most recently filled into a movie's "menu" object (Hook_PopulateRedirect: the garage, the pop-ups), and which of the two lists / menus was
// the latest (a counter: the mouse in the menus uses the later one). Read by PauseMenuLastList / PauseMenuLastUiMenu.
std::atomic<uint32_t> g_last_ui_menu{0};
std::atomic<uint32_t> g_populate_seq{0};
std::atomic<uint32_t> g_last_list_seq{0};
std::atomic<uint32_t> g_last_ui_menu_seq{0};

// ── Guest memory helpers ────────────────────────────────────────────────

uint8_t* GetMembase() {
    auto* rt = REX_RUNTIME();
    return rt ? rt->GetMembase().GetBase() : nullptr;
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
        // Not a warning: the only real caller is EnsureStringTableText, which
        // treats a miss as "create it instead". Logging an expected branch at
        // warning level produced ~180 warnings per session and buried the ones
        // that mattered. A null table (above) IS unexpected and still warns.
        MC_DEBUG("[pause-menu] entry '{}' absent, will create (hash 0x{:08X}, table 0x{:08X})",
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
    auto* rt = REX_RUNTIME();
    if (!rt) return 0;
    PPCFunc* fn = rt->FindFunction(fn_addr).value_or(nullptr);
    if (!fn) {
        MC_WARN("[pause-menu] guest fn 0x{:08X} not in dispatcher", fn_addr);
        return 0;
    }
    return larecomp::guest::Call<uint32_t>(fn, arg0);
}

uint32_t CallGuestFn2(uint32_t fn_addr, uint32_t a0, uint32_t a1) {
    auto* rt = REX_RUNTIME();
    if (!rt) return 0;
    PPCFunc* fn = rt->FindFunction(fn_addr).value_or(nullptr);
    if (!fn) return 0;
    return larecomp::guest::Call<uint32_t>(fn, a0, a1);
}

uint32_t CallGuestFn4(uint32_t fn_addr, uint32_t a0, uint32_t a1, uint32_t a2,
                      uint32_t a3) {
    auto* rt = REX_RUNTIME();
    if (!rt) return 0;
    PPCFunc* fn = rt->FindFunction(fn_addr).value_or(nullptr);
    if (!fn) return 0;
    return larecomp::guest::Call<uint32_t>(fn, a0, a1, a2, a3);
}

uint32_t CallGuestFn3(uint32_t fn_addr, uint32_t a0, uint32_t a1, uint32_t a2) {
    auto* rt = REX_RUNTIME();
    if (!rt) return 0;
    PPCFunc* fn = rt->FindFunction(fn_addr).value_or(nullptr);
    if (!fn) return 0;
    return larecomp::guest::Call<uint32_t>(fn, a0, a1, a2);
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
    auto* rt = REX_RUNTIME();
    if (!rt) return false;
    PPCFunc* fn = rt->FindFunction(kIsButtonClickedFn).value_or(nullptr);
    if (!fn) return false;
    return larecomp::guest::Call<uint32_t>(fn, guest_key_addr, 1u) != 0;
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

bool GuestStackPop() {
    uint32_t rootNode = GetRootNode();
    if (!rootNode) return false;
    uint32_t navStack = rootNode + 776;
    uint32_t engine   = rootNode + 4;

    auto* rt = REX_RUNTIME();
    if (!rt) return false;
    PPCFunc* fn = rt->FindFunction(kStackPopFn).value_or(nullptr);
    if (!fn) return false;

    uint32_t newTop = larecomp::guest::Call<uint32_t>(fn, navStack, engine, 1u);
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

// -- Language ----------------------------------------------------------
//
// The .strtbl files carry eleven language blocks in the order of the engine's
// own name table (off_8281B908): en es fr de it pt jp cht chs ko no. Six of
// them hold text: en/es/fr/de/it (4018 strings each) and jp. pt and the rest
// are zero length, and pt's start offset equals jp's, so asking the loader for
// pt on the disc's tables reads the Japanese block (no Japanese font). pt is
// therefore offered only when a mod ships string tables that fill it: see
// "Portuguese" below.
//
// The game chooses its language itself. sub_8238C120 calls the mapper
// sub_82387928, which turns the console's XConfig user language
// (ExGetXConfigSetting(3, 9) in sub_82457AB0: the SDK's `user_language` cvar,
// XLanguage ids) into the block index and stores it at singleton+1320; the
// string table (sub_82218A38), the hard-coded download dialog and the "lang"
// pushed into the UI movies all follow from that (CHECK: what the movies'
// scripts do with "lang" is not read). English is simply what `user_language`
// defaults to in the SDK. The German and Italian arms of the mapper are shut
// only on a console whose XGetGameRegion() & 0xFF00 is zero (the Americas); the
// SDK answers 0xFFFF, so with region 0xFFFF every arm is open:
//   XLanguage 1 en -> 0,  5 es -> 1,  4 fr -> 2,  3 de -> 3,  6 it -> 4;
// 2 (Japanese) and everything else give 0 (English) with the "fell back" flag.
// InitLanguageSetting (below) writes `user_language` from the `language` cvar
// before the guest runs, so the game takes the whole path itself.
//
// jp is never offered: sub_821FE648 loads ui/jp_fonts and the jp_*.xsf movies
// when the mapper returns 6, which it never does, and those movies are not on
// the disc (only the jp_*.sharetex are); forcing the index was measured to be a
// hard crash.
constexpr uint32_t kSetLanguageFn = 0x82218A38;  // (mgr, langIndex)
// The game's own language setter, as its dev language form (sub_8264BCB8) calls
// it: sub_8238C070(sub_82387B90(), idx) stores idx at singleton+1320 and at the
// global 0x8288E5A0 (read by the hard-coded download dialog) and sets the
// singleton's settings-changed byte (+0x3E050), then the form reloads the string
// table. sub_82387B90 returns the singleton (0x8288E920).
constexpr uint32_t kLanguageSingletonFn = 0x82387B90;  // () -> singleton
constexpr uint32_t kSetLanguageStateFn  = 0x8238C070;  // (singleton, langIndex)

constexpr const char* kLangVals[]  = {"auto", "en", "es", "fr", "de", "it", "pt"};
constexpr const char* kLangNames[] = {"AUTO", "ENGLISH", "ESPANOL", "FRANCAIS",
                                      "DEUTSCH", "ITALIANO", "PORTUGUES"};
// Index into the .strtbl language table, parallel to kLangVals. -1 = auto:
// whatever the mapper gives for the `user_language` the SDK and the player have.
// 5 (pt) is not a mapper result: the mapper only gives 0..4, so pt is set by
// rewriting the string table's index (Hook_StringTableLanguage).
constexpr int kLangIndex[] = {-1, 0, 1, 2, 3, 4, 5};
// The XLanguage id that makes the mapper give that index (region 0xFFFF).
// 0 = `user_language` is left alone: auto, and pt (no XLanguage gives 5).
constexpr uint32_t kLangXLanguage[] = {0, 1, 5, 4, 3, 6, 0};
constexpr int kNumLangs = int(sizeof(kLangVals) / sizeof(kLangVals[0]));
// pt: its block in the .strtbl language table, and its place in the lists above.
constexpr int kPtIndex = 5;
constexpr int kPtPos = 6;
static_assert(kLangIndex[kPtPos] == kPtIndex);

int AutoLanguageIndex();

// Reload the string table in place.
//
// Safe to call with the game running because sub_82218A38 opens with
// sub_825F2188(mgr+4), which overwrites the table pointer with a fresh, empty
// hash map *before* anything is freed -- the destructor that follows
// (sub_825F2D88) then tears down that empty one, so the outgoing language's
// entries and text buffers are never released. That matters: on a cache miss
// sub_82218268 hands out the string buffer itself (entry+8), so widgets do
// hold raw pointers into the old table. Leaking it (~200 KB a switch) is what
// keeps every one of them readable.
//
// What does not update is anything that resolved its text once and kept the
// pointer -- a row's label is cached at +48 by its constructor -- nor what the
// game read once at start (the "lang" its UI movies were given, the +1238
// "fell back" flag). Those keep the previous language until the screen is
// rebuilt, or until the game is started again, which is why the LANGUAGE row
// says "(RESTART)".
bool ApplyLanguageNow(int idx) {
    if (idx < 0) return false;
    uint32_t mgr = ReadGuestBE32(kStringTableGlobal);
    if (!mgr) return false;

    // The game's own setter first, as its dev language form does, so the singleton
    // and the global the hard-coded dialog reads agree with the string table.
    // pt is the exception: 5 is not a language the game knows (its mapper cannot
    // return it, and the movies and the dialog have nothing for it), so the
    // singleton goes back to auto's index and only the string table reads block 5.
    // CHECK: the settings-changed byte it sets (singleton+0x3E050, the one the
    // profile code sets after a change) may make the game write its profile.
    const int state_idx = idx == kPtIndex ? AutoLanguageIndex() : idx;
    if (const uint32_t singleton = CallGuestFn1(kLanguageSingletonFn, 0))
        CallGuestFn2(kSetLanguageStateFn, singleton, uint32_t(state_idx));

    MC_INFO("[language] reloading string table as index {}", idx);
    CallGuestFn2(kSetLanguageFn, mgr, uint32_t(idx));

    // The display cache at mgr+16 survives the reload untouched, so the
    // entries we injected there are still live -- but re-assert them anyway,
    // since they are the only strings in the game with no .strtbl entry to
    // fall back on.
    return true;
}

// Set from the cvar change callback (which runs under the cvar registry lock)
// and drained on the guest thread, where calling into guest code is safe.
std::atomic<int> g_pending_language{-1};

// The XLanguage id `user_language` had before InitLanguageSetting wrote it:
// what AUTO means (the SDK's default, English, or what the player put in
// larecomp.toml). Kept so AUTO is a real value the live switch can go back to,
// not just "whatever loaded at boot".
uint32_t g_auto_xlanguage = 1;

// The string-table index AUTO gives: the mapper's answer (sub_82387928, region
// 0xFFFF) for g_auto_xlanguage. Anything the mapper does not know is English.
int AutoLanguageIndex() {
    for (int i = 1; i < kNumLangs; ++i)
        if (kLangXLanguage[i] != 0 && kLangXLanguage[i] == g_auto_xlanguage) return kLangIndex[i];
    return 0;
}

// -- Portuguese --------------------------------------------------------
//
// pt is the empty eleventh block of every shipped .strtbl. It has text only when a
// mod ships string tables that fill it: modloader/<mod>/files/ui/mc4stringtable*.strtbl
// (the mod loader packs them into xarchive_mods.rpf, which is mounted last and wins).
// perf/scripts/make_pt_test_mod.py writes such a mod from the disc's files.
//
// A .strtbl is little endian: u32 n (11), u32 offset[n] (absolute file offsets, 0 = no
// block), u32 512, a key-name table only tools read (u32 count, then per name a u32
// length and length + 1 bytes), then the blocks. sub_825F23C0 reads n and the offsets,
// keeps offset[lang] (0 gives "No data for language"), seeks there and reads the block:
// u32 count (0 gives "No translations available"), then per string a u32 key hash, u16
// flags, u32 name length + name, u32 text length (UTF-16 chars, the NUL included) + text
// (UTF-16LE), two f32 and two u8. It never looks at a block's length. On the disc pt's
// offset equals jp's, so a block runs from its offset to the next language's.
//
// The tables the game reads with the language index (mgr+28, every one of them block
// `lang` of its own file):
//   ui/mc4stringtable.strtbl            sub_822185E8, inside SetLanguage (boot, every switch)
//   ui/mc4stringtable_0001..0008.strtbl sub_82217778, the same call; the disc has 0001-0003
//   ui/mc4stringtable_tms.strtbl        sub_82217618, when the TMS data loads (later)
//   mc4stringtable_downloaded           sub_82217618, for downloaded content (later), from
//                                       the content's own mount ("%s:\mc4stringtable_downloaded",
//                                       0x820393E4), not from ui/, so a mod cannot replace it
// Every table the disc has holds Japanese text under pt, and one the mod does not ship is
// the disc's, so a table without its own pt block would put Japanese text (no font) on
// the keys it holds: vehicle and paint names in _0001-_0003, the TMS texts. The game
// cannot be told per table, and _tms reads the index when it loads, so pt is available
// only when EVERY ui/ table the disc has is shipped by a mod with text in its pt block,
// and any other one a mod ships has one too. Otherwise this run uses auto. The downloaded
// table is not required (no mod can supply it): with downloaded content installed, its
// few texts would read their Japanese block under pt.
struct StringTableFile {
    const char* path;  // archive path
    bool on_disc;      // not shipped by a mod: the disc's file is read, and it has no pt text
};
constexpr StringTableFile kStringTableFiles[] = {
    {"ui/mc4stringtable.strtbl", true},
    {"ui/mc4stringtable_0001.strtbl", true},
    {"ui/mc4stringtable_0002.strtbl", true},
    {"ui/mc4stringtable_0003.strtbl", true},
    {"ui/mc4stringtable_0004.strtbl", false},
    {"ui/mc4stringtable_0005.strtbl", false},
    {"ui/mc4stringtable_0006.strtbl", false},
    {"ui/mc4stringtable_0007.strtbl", false},
    {"ui/mc4stringtable_0008.strtbl", false},
    {"ui/mc4stringtable_tms.strtbl", true},
};

// Whether the pt block of one .strtbl has text, from its header. False, with the reason,
// for a file that cannot be read, has no pt slot or offset, whose block is zero length
// (it starts where the next language's does, as on the disc) or holds no strings.
bool PtBlockHasText(const std::filesystem::path& file, std::string& why) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        why = "cannot be read";
        return false;
    }
    in.seekg(0, std::ios::end);
    const uint64_t size = uint64_t(in.tellg());
    auto u32_at = [&](uint64_t at, uint32_t& value) {
        uint8_t b[4] = {};
        in.clear();
        in.seekg(std::streamoff(at));
        if (!in.read(reinterpret_cast<char*>(b), 4)) return false;
        value = uint32_t(b[0]) | uint32_t(b[1]) << 8 | uint32_t(b[2]) << 16 | uint32_t(b[3]) << 24;
        return true;
    };

    uint32_t n = 0, start = 0, next = 0, count = 0;
    if (!u32_at(0, n) || n <= uint32_t(kPtIndex) || n > 64 || !u32_at(4 + 4 * kPtIndex, start)) {
        why = "has no pt slot in its header";
        return false;
    }
    if (start == 0) {
        why = "has no pt block (its offset is 0)";
        return false;
    }
    // The next language's offset ends the block; the last language's block ends with the file.
    uint64_t end = size;
    if (n > uint32_t(kPtIndex) + 1 && u32_at(4 + 4 * (kPtIndex + 1), next) && next != 0) end = next;
    if (start >= end || end > size) {
        why = "has an empty pt block (it ends where it starts)";
        return false;
    }
    if (!u32_at(start, count) || count == 0 || start + 4 > end) {
        why = "has a pt block with no strings";
        return false;
    }
    return true;
}

// 1: every table the game loads has Portuguese text; 0: not; -1: not decided yet.
std::atomic<int> g_pt_state{-1};

// Whether pt is usable. Only reads what DecidePortuguese found, so it is safe under the cvar
// lock; before the decision it is false.
bool PortugueseAvailable() { return g_pt_state.load(std::memory_order_relaxed) == 1; }

// Decides once, at the first SetLanguage call (the boot load). The mod archive is built before
// the guest starts, so the files the mods ship are known by then; OnPreSetup, where the language
// is set up, is too early. `wanted`: the `language` setting is pt, so a "no" is worth a warning.
void DecidePortuguese(bool wanted) {
    if (g_pt_state.load(std::memory_order_relaxed) >= 0) return;

    int filled = 0;
    std::string why;
    for (const StringTableFile& table : kStringTableFiles) {
        const std::filesystem::path file = mc::modloader::ModRawFile(table.path);
        if (file.empty()) {
            if (!table.on_disc) continue;
            why = filled == 0
                      ? std::string("no mod ships string tables with Portuguese text (a mod's "
                                    "files/ui/mc4stringtable*.strtbl with the pt block filled)")
                      : std::string(table.path) + " is not shipped by the mod, and the disc's "
                                                  "has no Portuguese text";
            break;
        }
        std::string problem;
        if (!PtBlockHasText(file, problem)) {
            why = std::string(table.path) + " from the mod " + problem;
            break;
        }
        ++filled;
    }

    const bool ok = why.empty();
    g_pt_state.store(ok ? 1 : 0, std::memory_order_relaxed);
    if (ok)
        MC_INFO("[language] Portuguese is available: {} string table(s) from mods have pt text", filled);
    else if (wanted)
        MC_WARN("[language] language = pt, but {}: using auto (string table {}); the setting is kept "
                "for when the mod is installed", why, AutoLanguageIndex());
    else
        MC_DEBUG("[language] Portuguese is not offered: {}", why);
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

// ── CVar access by name (uniform for larecomp AND SDK cvars) ───────────

// ── Held values: what the next save writes into larecomp.toml ──────────
//
// A choice in g_deferred is not the live cvar's value: CvarGet answers with it,
// and a save (SaveSettingsNow, WriteDeferredCvars) writes it. What is held today
// is the ONLINE choice (SYSTEM LINK, below), which system_link.cpp applies
// itself, and the RENDERER choice of the hidden DEBUG menu (below). The GRAPHICS API
// row, which was held here too (a restart-only cvar changes its live value with
// SetFlagByName), is not in the pause menu any more (SETTINGS-REVIEW.md, SX-G5):
// the launcher's Graphics API row sets it. The render scale (mcla_gpu_resolution_scale,
// mcla_gpu_fsr_preset) and the upscaler modes (mcla_gpu_fsr31_mode, _fsr4_mode,
// _dlss_mode) were never held: the renderer makes its targets again at the new
// size at a frame boundary (TemporalSystem::UpdatePath, EdramTargets::SetScales).
std::mutex g_deferred_mutex;
std::vector<std::pair<std::string, std::string>>& g_deferred = larecomp::Leak<std::vector<std::pair<std::string, std::string>>>();

void SetDeferred(const char* name, const char* value) {
    std::lock_guard<std::mutex> lock(g_deferred_mutex);
    for (auto& [k, v] : g_deferred) {
        if (k == name) {
            v = value;
            return;
        }
    }
    g_deferred.emplace_back(name, value);
}

// Settings a menu row only HOLDS for the next launch (CvarSet below): never written to the live cvar, which would either be refused or
// change under the code that read it at start. gpu.driver is the SDK's renderer choice (renderer_choice.h): startup-only, so the SDK
// refuses a write once the app runs ("can no longer change"). The RENDERER row of the hidden DEBUG menu holds it; the next save writes
// it to larecomp.toml, and the "restart now?" prompt offers the restart (the row's label says "(RESTART)").
const char* const kDeferredCvars[] = {
    "gpu.driver",
};

bool IsDeferredCvar(std::string_view name) {
    for (const char* d : kDeferredCvars)
        if (name == d) return true;
    return false;
}

// The held values on their own: only these lines of the toml change, the live
// cvars are not written (a settings save, SaveSettingsNow, does both).
void WriteDeferredCvars(const std::filesystem::path& path) {
    larecomp::HeldCvars pending;
    {
        std::lock_guard<std::mutex> lock(g_deferred_mutex);
        pending = g_deferred;
    }
    if (pending.empty()) return;
    if (larecomp::SaveHeldCvarsToConfig(path, pending))
        MC_INFO("[pause-menu] {} restart-only setting(s) written to {}; they apply on the next launch",
                pending.size(), path.string());
    else
        MC_ERROR("[pause-menu] could not write the restart-only settings to {}", path.string());
}

// The key of a flat `key = value` line, or empty for anything else.
std::string TomlLineKey(const std::string& line) {
    const size_t p = line.find_first_not_of(" \t");
    if (p == std::string::npos || line[p] == '#' || line[p] == '[') return {};
    size_t e = p;
    while (e < line.size() && line[e] != '=' && line[e] != ' ' && line[e] != '\t') ++e;
    const size_t q = line.find_first_not_of(" \t", e);
    if (e == p || q == std::string::npos || line[q] != '=') return {};
    return line.substr(p, e - p);
}

std::string CvarGet(const char* name) {
    if (!name) return {};
    {
        std::lock_guard<std::mutex> lock(g_deferred_mutex);
        for (const auto& [k, v] : g_deferred)
            if (k == name) return v;
    }
    if (std::string_view(name) == "mcla_gpu_vendor_denoiser")
        return mcla_menu::VendorDenoiserOn() ? "true" : "false";
    return larecomp::cvar::GetFlagByName(name);
}

bool CvarGetBool(const char* name) {
    return CvarGet(name) == "true";
}

double CvarGetDouble(const char* name) {
    return std::strtod(CvarGet(name).c_str(), nullptr);
}

// DISPLAY > HDR > TEST PATTERN is a preview, not a setting: the bars replace the whole picture, this
// menu included (the game draws the pause menu into the same frame: a pause frame has no UI layer
// the present could keep above the bars, frame_layers.h), so they show only while their row is the
// highlighted row of the open HDR menu (HdrTestPatternTick), and they are never written to
// larecomp.toml (config_save.h NoteSessionOnlyCvar). True while the menu's row has them on.
constexpr const char* kHdrTestPatternCvar = "mcla_gpu_hdr_test_pattern";
std::atomic<bool> g_hdr_pattern_by_menu{false};

// The menu is about to change `name`: remembers what it was, for the "restart now?" prompt
// (below, "Restart now?").
void NoteMenuWrite(const char* name);

void CvarSet(const char* name, const char* value) {
#if defined(LARECOMP_HAS_GPU)
    // A row of IMAGE QUALITY, LIGHTING AND SHADOWS, REFLECTIONS or WORLD DETAIL written for the player while a named graphics preset is held turns the preset into CUSTOM, with the rows as
    // they are (gpu/gfx_preset_apply.h). Before the write, so that the value the player picked is the player's own and not one the preset holds. Any other write is none of its business.
    mcla::gpu::gfx_preset::NoteRowWrite(name);
#endif
    NoteMenuWrite(name);
    if (std::string_view(name) == kHdrTestPatternCvar)
        g_hdr_pattern_by_menu.store(std::string_view(value) == "true", std::memory_order_relaxed);
    if (IsDeferredCvar(name)) {
        MC_INFO("[pause-menu] cvar '{}' -> '{}' held for the next launch", name, value);
        SetDeferred(name, value);
        return;
    }
    // Logged before the write, not after: a cvar's change callbacks run
    // synchronously on this (guest) thread with the cvar registry mutex held,
    // so an apply that hangs used to leave no trace at all -- the last line in
    // the log was whatever came before the click.
    MC_INFO("[pause-menu] cvar '{}' -> '{}'", name, value);
    larecomp::cvar::SetFlagByName(name, value);
    MC_INFO("[pause-menu] cvar '{}' applied", name);
}

void CvarSetDouble(const char* name, double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g", v);
    CvarSet(name, buf);
}

// ── SYSTEM LINK ONLINE ─────────────────────────────────────────────────
//
// mcla_online_enable / _server belong to the fork SDK's online client
// (src/kernel/xam/mcla_online.cpp) and are reached by name only: the stock SDK
// has none of them, and declaring them is a link error there (see rmr_client).
//
// The game takes its network identity when it opens its sockets, and the SDK
// only routes a socket through the relay if online was on when that socket was
// bound. Flipping the cvar in a running game therefore left a process that
// finds sessions but cannot join or host one: the logs of 28/08 have four joins
// out of four timing out after 13 s from such a process, and the same host
// joined in 0.12 s from one that booted online, and the F4 overlay offered
// exactly that flip as a live checkbox. Both the ONLINE checkbox below and
// the overlay row now switch through system_link.cpp, which has the game
// close and reopen its sockets and rebuild its player records around the
// change, so it applies at once -- the overlay's own flip is only the request.
//
// The SDK also switches it off on its own when the server misses its first ack
// (3 s, xam_net.cpp). The next settings save then wrote the live value -- off,
// i.e. no line at all -- and the next launch came up offline. So the choice in
// larecomp.toml is held like a restart-only row: every save from here writes it
// back, and the pause menu puts it back if another save dropped it.
//
// The checkbox lives in the game's own SYSTEM LINK menu, the "LAN" state that
// PM_Lan opens (sub_82667630 -> sub_8265F3C8): a UIMenu of QUICK CRUISE,
// QUICK MATCH and CREATE MATCH (LAN.s0..s2). An ONLINE state is appended to
// it, and the list is drawn through native rows so that row can be a real
// checkbox (see "The SYSTEM LINK list, drawn as native rows" below). An
// injected state carries no command, so the UIMenu never acts on A for it: A
// is taken where the game hands its keys to the list (Hook_MenuKeyDispatch,
// key '7'). The connection itself is shown by online_toast.cpp.
constexpr const char* kOnlineEnableCvar = "mcla_online_enable";
constexpr const char* kOnlineServerCvar = "mcla_online_server";

// -1 = no online client in this build, else whether it booted online.
std::atomic<int> g_online_boot{-1};

std::filesystem::path ConfigPath() {
    return rex::platform::GetExecutableFolder() / "larecomp.toml";
}

// The value the pause menu holds for `name`, empty when it holds none.
std::string HeldValue(const char* name) {
    std::lock_guard<std::mutex> lock(g_deferred_mutex);
    for (const auto& [k, v] : g_deferred)
        if (k == name) return v;
    return {};
}

// The value text of `key` in the flat toml, empty when the key is absent.
std::string TomlLineValue(const std::filesystem::path& path, const std::string& key) {
    std::ifstream in(path, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        if (TomlLineKey(line) != key) continue;
        const size_t b = line.find_first_not_of(" \t", line.find('=') + 1);
        if (b == std::string::npos) return {};
        // A comment after a bare value (a save keeps the player's) is not part of it.
        const size_t e = line[b] == '"' ? line.find_last_not_of(" \t\r") + 1
                                        : std::min(line.find_first_of(" \t#\r", b), line.size());
        return e <= b ? std::string() : line.substr(b, e - b);
    }
    return {};
}

// Sampled before the guest starts: the SDK's own switch-off happens inside
// the guest's first XNetGetTitleXnAddr, so any later read could already be the
// flipped value. What is held is larecomp.toml's line, not the boot value: a
// --mcla_online_enable on the command line decides this run only, and must
// not end up written into the file.
void InitOnlineSettings() {
    // Linux (online_platform.h): no online client exists there yet. The ONLINE row is still added, greyed
    // and labelled, so g_online_boot is "a client exists, booted offline" for it; nothing is held and no
    // callback is installed.
    if (!larecomp::kOnlineAvailable) {
        g_online_boot.store(0, std::memory_order_relaxed);
        // An online line from a larecomp.toml made on Windows, or --mcla_online_enable, would have the SDK
        // route the game's sockets through a relay it cannot reach: the game would find sessions it can
        // not join, and say "Server is offline" 3 s into the start. So it is off for this run. NoteStartupCvar
        // makes a save leave larecomp.toml's line as the player has it.
        const bool asked = larecomp::cvar::GetFlagInfo(kOnlineEnableCvar) &&
                           larecomp::cvar::GetFlagByName(kOnlineEnableCvar) == "true";
        if (asked) {
            larecomp::cvar::SetFlagByName(kOnlineEnableCvar, "false");
            larecomp::NoteStartupCvar(kOnlineEnableCvar);
        }
        MC_WARN("[online] {}: the game runs offline{}", larecomp::kOnlineUnavailableText,
                asked ? " (mcla_online_enable is ignored for this run; larecomp.toml keeps its line)"
                      : " (the ONLINE row of the SYSTEM LINK menu says so)");
        return;
    }
    if (!larecomp::cvar::GetFlagInfo(kOnlineEnableCvar)) return;

    const bool on = larecomp::cvar::GetFlagByName(kOnlineEnableCvar) == "true";
    g_online_boot.store(on ? 1 : 0, std::memory_order_relaxed);
    if (TomlLineValue(ConfigPath(), kOnlineEnableCvar) == "true")
        SetDeferred(kOnlineEnableCvar, "true");

    // The F4 overlay's row stays Live: a change there is held for
    // larecomp.toml and handed to system_link.cpp, exactly like the checkbox,
    // instead of being the bare flip that left the game half online. This
    // runs on the overlay's thread with the cvar registry locked, so it only
    // stores -- the net update does the rest.
    larecomp::cvar::RegisterChangeCallback(
        kOnlineEnableCvar, [](std::string_view, std::string_view value) {
            if (SystemLinkOnlineSettingCvar()) return;
            const bool on = value == "true";
            SetDeferred(kOnlineEnableCvar, on ? "true" : "false");
            SystemLinkOnlineRequest(on);
        });

    MC_INFO("[online] booted {}; switched from SYSTEM LINK > ONLINE in the pause menu "
            "or from F4", on ? "online" : "offline");
}

// The overlay's "Save to config" writes the live value, so once the SDK has
// switched online off a save there drops the player's line. Only possible
// while the live value and the held choice disagree; checked when the pause
// menu comes up, at most every few seconds.
void KeepOnlineChoiceInConfig() {
    if (g_online_boot.load(std::memory_order_relaxed) < 0) return;

    const std::string held = HeldValue(kOnlineEnableCvar);
    if (held.empty() || larecomp::cvar::GetFlagByName(kOnlineEnableCvar) == held) return;

    static std::chrono::steady_clock::time_point last{};
    const auto now = std::chrono::steady_clock::now();
    if (last != std::chrono::steady_clock::time_point{} &&
        now - last < std::chrono::seconds(5))
        return;
    last = now;

    const auto path = ConfigPath();
    const std::string on_disk = TomlLineValue(path, kOnlineEnableCvar);
    if (on_disk == held || (on_disk.empty() && held == "false")) return;

    MC_INFO("[online] larecomp.toml has {} = '{}', the choice is '{}'; writing it back",
            kOnlineEnableCvar, on_disk, held);
    WriteDeferredCvars(path);
}

// The same write as leaving a settings submenu (Hook_RexGlueCancel).
void SaveSettingsNow() {
    const auto path = ConfigPath();
    larecomp::HeldCvars held;
    {
        std::lock_guard<std::mutex> lock(g_deferred_mutex);
        held = g_deferred;
    }
    // What changed in this session and the held restart-only choices, merged
    // into the file: its other lines, including those of cvars this mode never
    // registers (the emulated GPU's, in the Plume mode), stay as they are.
    if (larecomp::SaveCvarsToConfig(path, held))
        MC_INFO("[pause-menu] settings saved to {}", path.string());
    else
        MC_ERROR("[pause-menu] could not save the settings to {}", path.string());
}

// ── SYSTEM LINK > ONLINE ─────────────────────────────────────────────────
//
// The SYSTEM LINK window is not a list widget. "LAN" is a UIMenu under
// MenuStylePaused that FlashNavigator draws into the "menu" object of the
// pop-up movie (popup.xsf): sub_82220308 writes one element per visible child
// -- text1 = label, property1 = enabled, count/select = its own sub-options --
// and never a `type`. The movie's Fillmenu shows checkboxMC[] for an element
// whose type is 4 (6 = greyed out), ticked when its select is 1, so the ONLINE
// element gets type 4 and its select right after the game fills it
// (Hook_LanFillElement). A goes to the menu's own event handler
// (sub_8263B9A0, event 52), where Hook_LanAccept takes it when the highlighted
// child is ONLINE. Redraws go through the movie's TransitionLevel, which is
// what FlashNavigator's fill command sets after filling (sub_82220FE0 case 0).

constexpr const char* kLanMenuName  = "LAN";
constexpr const char* kLanOnlineKey = "PM_RxLanOnline";
constexpr uint32_t kFindStateByNameFn = 0x82691650;  // (engine, name) -> state, 0 if none

constexpr uint32_t kGetFlashPropIntFn = 0x82722398;  // (object, name, int* out)
constexpr uint32_t kPlaySoundFn     = 0x821E6800;  // (sound name)
constexpr uint32_t kMenuSelectSound = 0x8282600C;  // -> "UI_MENU_SELECT"
constexpr uint32_t kStr_POPUPMOVIE = 0x8208D58C;
// UIMenu vtable (0x82020654) slots.
constexpr uint32_t kMenuRefreshVfunc  = 0xB0;   // sub_8263BF18: refill its movie's "menu"
constexpr uint32_t kMenuSelectedVfunc = 0x150;  // highlighted row, -1 for none
constexpr uint32_t kMenuMovieVfunc    = 0x1D4;  // the movie it draws into, 0 = FlashNavigator's
constexpr uint32_t kMenuChildAtVfunc  = 0x1E0;  // (menu, row) -> the child shown on that row
constexpr uint32_t kMenuEventAccept   = 52;     // A, from the menu input
constexpr int kElementTypeCheckbox    = 4;
constexpr int kElementTypeGreyed      = 6;     // greyed out: the ONLINE row where there is no online client

std::atomic<uint32_t> g_lan_state{0};    // the SYSTEM LINK menu
std::atomic<uint32_t> g_lan_online{0};   // its ONLINE row
// SERVER BROWSER rows (server_browser/browser_ui.h): one in SYSTEM LINK, one
// in the XBOX LIVE menu ("Online"), each its own state (a state has one parent).
constexpr const char* kLiveMenuName = "Online";
constexpr const char* kLanBrowserKey = "PM_RxServerBrowserLan";
constexpr const char* kLiveBrowserKey = "PM_RxServerBrowserLive";
std::atomic<uint32_t> g_live_state{0};
std::atomic<uint32_t> g_lan_browser{0};
std::atomic<uint32_t> g_live_browser{0};
std::atomic<bool> g_lan_items_added{false};
// The highlight is in the SYSTEM LINK menu: a row of it was the last state
// activated (activation follows the highlight). Keeps the online pop-up up.
std::atomic<bool> g_lan_active{false};
// A on ONLINE switched online; the next front-end tick writes larecomp.toml.
std::atomic<bool> g_lan_save_asked{false};
int g_lan_drawn_checked = -1;            // the box the pop-up was last given, guest thread only
int64_t g_lan_redraw_ms = 0;             // last redraw asked for, guest thread only

int64_t SteadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Checked while a switch on is under way, unchecked while a switch off is,
// otherwise whether online is really up.
bool LanOnlineChecked() {
    const int pending = SystemLinkOnlinePending();
    if (pending) return pending > 0;
    return SystemLinkOnlineConnected();
}

// The ONLINE row goes at the end of the SYSTEM LINK menu, once, the first
// time the pause menu comes up.
void EnsureLanOnlineItems() {
    if (g_online_boot.load(std::memory_order_relaxed) < 0) return;
    if (g_lan_items_added.exchange(true)) return;

    const uint32_t engine = GetEnginePtr();
    const uint32_t name = engine ? AllocGuestString(kLanMenuName) : 0;
    const uint32_t lan = name ? CallGuestFn2(kFindStateByNameFn, engine, name) : 0;
    if (!lan) {
        MC_WARN("[online] no '{}' state: the SYSTEM LINK menu gets no ONLINE row",
                kLanMenuName);
        return;
    }
    const uint32_t online = CreateNewMenuState(kLanOnlineKey);
    if (!online) return;

    // Linux: the row stays, greyed (Hook_LanFillElement), and says what is missing; there is no server to
    // browse, so the SERVER BROWSER rows are not added.
    EnsureStringTableText(kLanOnlineKey,
                          larecomp::kOnlineAvailable ? "ONLINE" : "ONLINE (NOT ON LINUX YET)");
    GuestAppendMenuItem(lan, online);
    g_lan_online.store(online, std::memory_order_relaxed);
    g_lan_state.store(lan, std::memory_order_relaxed);
    MC_INFO("[online] ONLINE row added to the SYSTEM LINK menu ('{}' 0x{:08X}){}",
            kLanMenuName, lan, larecomp::kOnlineAvailable ? "" : ", greyed: not available on Linux yet");
    if (!larecomp::kOnlineAvailable) return;

    if (const uint32_t row = CreateNewMenuState(kLanBrowserKey)) {
        EnsureStringTableText(kLanBrowserKey, "SERVER BROWSER");
        GuestAppendMenuItem(lan, row);
        g_lan_browser.store(row, std::memory_order_relaxed);
    }
    const uint32_t live_name = AllocGuestString(kLiveMenuName);
    const uint32_t live = live_name ? CallGuestFn2(kFindStateByNameFn, engine, live_name) : 0;
    if (!live) {
        MC_WARN("[online] no '{}' state: the XBOX LIVE menu gets no SERVER BROWSER row",
                kLiveMenuName);
        return;
    }
    if (const uint32_t row = CreateNewMenuState(kLiveBrowserKey)) {
        EnsureStringTableText(kLiveBrowserKey, "SERVER BROWSER");
        GuestAppendMenuItem(live, row);
        g_live_browser.store(row, std::memory_order_relaxed);
        g_live_state.store(live, std::memory_order_relaxed);
        MC_INFO("[online] SERVER BROWSER rows added (SYSTEM LINK, XBOX LIVE 0x{:08X})", live);
    }
}

// Two guest ints, for the setters and getters that take the value by address.
uint32_t GuestIntCells() {
    static std::atomic<uint32_t> cells{0};
    uint32_t c = cells.load(std::memory_order_relaxed);
    if (!c) {
        c = CallGuestFn1(kGuestMallocFn, 16);
        cells.store(c, std::memory_order_relaxed);
    }
    return c;
}

void SwfSetInt(uint32_t object, uint32_t name, int32_t value) {
    const uint32_t cell = GuestIntCells();
    if (!cell) return;
    WriteGuestBE32(cell, uint32_t(value));
    CallGuestFn3(kSetFlashPropIntFn, object, name, cell);
}

int32_t SwfGetInt(uint32_t object, uint32_t name) {
    const uint32_t cell = GuestIntCells();
    if (!cell) return -1;
    WriteGuestBE32(cell + 4, 0xFFFFFFFFu);
    CallGuestFn3(kGetFlashPropIntFn, object, name, cell + 4);
    return int32_t(ReadGuestBE32(cell + 4));
}

uint32_t MenuVfunc(uint32_t menu, uint32_t slot) {
    const uint32_t vtable = menu ? ReadGuestBE32(menu) : 0;
    return vtable ? ReadGuestBE32(vtable + slot) : 0;
}

// The movie the SYSTEM LINK menu draws into, found the way its refresh
// (sub_8263BF18) finds it: its own, else FlashNavigator's current one.
uint32_t LanMovie(uint32_t lan) {
    const uint32_t fn = MenuVfunc(lan, kMenuMovieVfunc);
    if (const uint32_t movie = fn ? CallGuestFn1(fn, lan) : 0) return movie;
    const uint32_t ui = ReadGuestBE32(kVHSMGlobal);
    const uint32_t mgr = ui ? ReadGuestBE32(ui + 0x34) : 0;
    const uint32_t holder = mgr ? ReadGuestBE32(mgr + 0x40710) : 0;
    return holder ? ReadGuestBE32(holder + 0x38) : 0;
}

const char* MovieName(uint32_t movie) {
    const uint32_t ui = ReadGuestBE32(kVHSMGlobal);
    if (!ui || !movie) return "?";
    for (const auto& [name, label] : {std::pair{kStr_POPUPMOVIE, "POPUPMOVIE"},
                                      std::pair{kStr_PAUSEMOVIE, "PAUSEMOVIE"}}) {
        const uint32_t holder = CallGuestFn2(kFindMovieFn, ui, name);
        if (holder && ReadGuestBE32(holder + 0x38) == movie) return label;
    }
    return "another movie";
}

// Refills the SYSTEM LINK pop-up and has its movie redraw the rows. The
// refill comes back through Hook_LanFillElement with the current box.
// TransitionLevel 1 keeps the scroll position (0 would reset it).
void RefreshLanPopup() {
    const uint32_t lan = g_lan_state.load(std::memory_order_relaxed);
    // Only while the window is up (vhsm active bit, +16 & 0x80): a refill
    // otherwise lands in whatever screen owns that movie's "menu" now.
    if (!lan || !(ReadGuestBE32(lan + 16) & 0x80)) return;
    const uint32_t refresh = MenuVfunc(lan, kMenuRefreshVfunc);
    const uint32_t movie = lan ? LanMovie(lan) : 0;
    if (!refresh || !movie) return;
    g_lan_redraw_ms = SteadyMs();
    CallGuestFn1(refresh, lan);
    CallGuestFn3(kSetFlashIntFn, movie, kStr_TransitionLevel, 1);
    static std::atomic<bool> told{false};
    if (!told.exchange(true))
        MC_INFO("[online] SYSTEM LINK pop-up redrawn (movie 0x{:08X} = {})", movie,
                MovieName(movie));
}

// A on ONLINE: the switch itself is a request the next net update carries
// out (online/system_link.cpp), so it is made right here; writing the config
// waits for the front-end tick.
void LanOnlineToggle() {
    const bool on = !LanOnlineChecked();
    MC_INFO("[online] ONLINE {} from the SYSTEM LINK menu", on ? "on" : "off");
    SetDeferred(kOnlineEnableCvar, on ? "true" : "false");
    SystemLinkOnlineRequest(on);
    g_lan_save_asked.store(true, std::memory_order_relaxed);
}

// Front-end tick (Hook_CarbonUiTick). The box moves on its own -- the switch
// takes a net update or two, and the SDK can give up on the server -- so while
// the SYSTEM LINK menu is up the pop-up is redrawn whenever its box is stale.
// At most twice a second, in case a refill does not come back.
void LanOnlineTick() {
    if (g_lan_save_asked.exchange(false)) SaveSettingsNow();
    if (!g_lan_active.load(std::memory_order_relaxed)) return;
    larecomp::OnlineToastPoke();
    const int checked = LanOnlineChecked() ? 1 : 0;
    if (g_lan_drawn_checked < 0 || checked == g_lan_drawn_checked) return;
    if (SteadyMs() - g_lan_redraw_ms < 500) return;
    g_lan_drawn_checked = checked;
    RefreshLanPopup();
}

// Which .strtbl language block the `language` cvar asks for. -1 = auto, i.e.
// leave whatever sub_82387928 derived from the console profile. pt is auto too
// while the mods' string tables have no Portuguese text (PortugueseAvailable);
// the cvar itself is left as it is.
int LanguageIndexFromCvar() {
    std::string v = CvarGet("language");
    for (int i = 0; i < kNumLangs; ++i)
        if (v == kLangVals[i])
            return kLangIndex[i] == kPtIndex && !PortugueseAvailable() ? -1 : kLangIndex[i];
    return -1;
}

// Same, but with AUTO resolved to the index the game itself derives from the
// `user_language` it started with.
int ResolveLanguageIndex() {
    const int idx = LanguageIndexFromCvar();
    return idx >= 0 ? idx : AutoLanguageIndex();
}

// ── Item / menu tables ─────────────────────────────────────────────────
//
// Each submenu is a table of ItemDef; the Flash list's selected index maps
// straight into the table, so item order in the list = table order. A click
// cycles the item's cvar to its next value; labels are rebuilt from the live
// cvar value on every render, so F4-side changes show up too.

// The row description and its constructors now live in menu_items.h, so a
// feature can ship its own rows from its own translation unit -- see
// CameraLookMenuItems() in camera_look.cpp, which fills out the rest of the
// DEBUG CAMERA tab from where the camera code actually is.
using mcla_menu::Action;
using mcla_menu::ActionId;
using mcla_menu::Bool;
using mcla_menu::Carbon;
using mcla_menu::Color;
using mcla_menu::Dbl;
using mcla_menu::ItemDef;
using mcla_menu::ItemKind;
using mcla_menu::kActionDebugCopyArgs;
using mcla_menu::kActionDebugOpenSettings;
using mcla_menu::kActionDebugReset;
using mcla_menu::kActionDebugShowChanged;
using mcla_menu::kActionDebugTelemetry;
using mcla_menu::kActionDumpTextures;
using mcla_menu::kActionPlayCutscene;
using mcla_menu::kActionQuitNo;
using mcla_menu::kActionQuitYes;
using mcla_menu::kActionReloadTextures;
using mcla_menu::kActionRestart;
using mcla_menu::kActionRestartLater;
using mcla_menu::kActionRestartNow;
using mcla_menu::NativeOnly;
using mcla_menu::Str;
using mcla_menu::Submenu;
using mcla_menu::XeniaOnly;

// ── Value tables ───────────────────────────────────────────────────────


constexpr double kCamSmoothVals[] = {0.2, 0.5, 0.8, 1.0, 1.2, 1.5, 2.0};

constexpr double kFovValues[] = {0.8, 0.9, 1.0, 1.1, 1.2, 1.3, 1.4,
                                 1.5, 1.6, 1.7, 1.8, 1.9, 2.0};
// World detail distances and densities are displayed in their own units.
constexpr double kTrafficLodVals[] = {0.5, 1.0, 1.5, 2.0, 3.0, 5.0};
// 40, 45 and 50 sit between the console's 30 and 60: steady on hardware that can't hold 60, and
// even divisors of 120 Hz (40), 90 Hz (45) and 100 Hz (50) displays.
constexpr double kFpsCapVals[] = {30.0, 40.0, 45.0, 50.0, 60.0, 120.0, 144.0, 0.0};

// Performance. Traffic/ped/parked mirror the ranges the cvars declare; the
// fragment-tune table brackets the two values the engine itself uses — 250 is
// the fragTuneStruct constructor default and 3000 is what the shipped tune file
// loads. 0 is "leave the tune file alone" and renders as STOCK.
constexpr double kUnspawnVals[]      = {100.0, 150.0, 200.0, 250.0, 300.0,
                                        400.0, 500.0, 600.0};
constexpr double kDensityVals[]      = {0.0, 0.25, 0.5, 0.75, 1.0, 1.5, 2.0};
constexpr double kFragDrawDistVals[] = {0.0, 250.0, 500.0, 1000.0, 1500.0,
                                        2000.0, 3000.0, 4000.0};

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

// Speed / distance units. "game" leaves the console profile's own choice (mph
// on NTSC). Both halves of this are covered: the game's metric formatter
// (sub_8238DDF0) handles menus and distances on its own, and hud_units.cpp
// rewrites the m/s->mph constant the HUD movie's ActionScript carries, since the
// live speedometer never goes through that formatter.
// No slash in the label: the value's display text is also the name of the Flash
// element the row creates for it, and AS1 slash syntax makes '/' a path
// separator -- "KM/H" is looked up as a path, the lookup returns null, and
// sub_827250A8 hands that null straight to sub_825EF9F0 (which does a bare
// `a1[2] == 5`). That is a read of guest 0x8 during UI setup, i.e. a boot crash.
// Every row the game ships stays inside A-Z, 0-9, space, '+', '.', parens.
constexpr const char* kSpeedUnitVals[]  = {"game", "kmh", "mph"};
constexpr const char* kSpeedUnitNames[] = {"GAME", "KMH", "MPH"};
constexpr const char* kStartGarageVals[]  = {"hollywood", "beaches", "sc", "last"};
constexpr const char* kStartGarageNames[] = {"HOLLYWOOD", "BEACHES", "SOUTH CENTRAL", "LAST USED"};
constexpr const char* kButtonLayoutVals[]  = {"auto", "xbox", "playstation", "keyboard"};
constexpr const char* kButtonLayoutNames[] = {"AUTO", "XBOX", "PLAYSTATION", "KEYBOARD"};
constexpr const char* kDisplayGammaVals[]  = {"tv", "pc"};
constexpr const char* kDisplayGammaNames[] = {"CONSOLE", "PC"};
// ASPECT RATIO: ":" is not allowed in a name, so the shapes read 16X9 as RESOLUTION reads 2560X1440.
constexpr const char* kAspectVals[]  = {"window", "16:9", "16:10", "21:9", "32:9"};
constexpr const char* kAspectNames[] = {"FIT WINDOW", "16X9", "16X10", "21X9", "32X9"};
// DIFFICULTY. PRESET picks whose rules the AI races by: the Complete Edition's
// (the build larecomp runs) or the launch build's, before the title update of
// December 2008 eased it. AI DIFFICULTY is a percent, -1 = the game's own choice
// (STOCK); the scales are multipliers over the shipped RubberBandTune (1 = as shipped).
constexpr const char* kDiffPresetVals[]  = {"complete", "original"};
constexpr const char* kDiffPresetNames[] = {"COMPLETE EDITION", "ORIGINAL 2008"};
constexpr double kAiDiffVals[]     = {-1.0, 0.0, 10.0, 20.0, 30.0, 40.0, 50.0,
                                      60.0, 70.0, 80.0, 90.0, 100.0};
constexpr double kRbScaleVals[]    = {0.0, 0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 2.0};
constexpr double kRbDistVals[]     = {0.25, 0.5, 0.75, 1.0, 1.5, 2.0, 3.0, 4.0};
constexpr double kRbNitroScaleVals[] = {0.0, 0.5, 1.0, 2.0, 4.0, 8.0};
constexpr double kRbNitroStartVals[] = {-1.0, 100.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0};

constexpr double kStickDeadzoneVals[] = {0.0, 5.0, 10.0, 15.0, 20.0, 25.0, 30.0};

constexpr double kImpulseStrengthVals[] = {25.0, 50.0, 75.0, 100.0};

// DUALSENSE THROTTLE / BRAKE STRENGTH: 1 (LIGHT), 2 ... 7, 8 (FIRM) (pause_parity_rows.h).

#if defined(LARECOMP_HAS_GPU)
// ── HDR ────────────────────────────────────────────────────────────────
//
// HDR (mcla_gpu_hdr) is live: the renderer applies it at the end of a frame, making the window's
// swap chain again in the other format. Old configuration values that follow
// Windows or the display are shown using their effective mode and brightness.
constexpr const char* kHdrModeVals[]  = {"off", "on"};
constexpr const char* kHdrModeNames[] = {"OFF", "ON"};
// Windows' SDR content brightness slider covers 80 to 480 nits.
constexpr double kHdrPaperWhiteVals[] = {80.0, 100.0, 120.0, 160.0, 200.0,
                                         240.0, 280.0, 320.0, 400.0, 480.0};
constexpr double kHdrPeakVals[] = {200.0, 400.0, 600.0, 800.0, 1000.0,
                                   1200.0, 1500.0, 2000.0, 3000.0, 4000.0};
// How far the scene's lights go above paper white (mcla_gpu_hdr_highlights, the HDR curve's slope):
// NORMAL, BRIGHT, BRIGHTER (the default), BRIGHTEST.
constexpr double kHdrHighlightVals[] = {1.0, 1.5, 2.0, 3.0};

#endif

// ── Menu contents ──────────────────────────────────────────────────────

// Display settings, including anti-aliasing. Presets cover the four effect pages below.
#if defined(LARECOMP_HAS_GPU)
const char* const kPostAaVals[] = {"off", "fxaa", "smaa"};
const char* const kPostAaNames[] = {"OFF", "FXAA", "SMAA"};
#endif

// CHECK: verify the AA rows and the Xenia MSAA restart prompt during the Display tour.
const ItemDef kVideoItems[] = {
    // The window mode and output resolution are live. Gamescope sets the size on Linux, where RESOLUTION is grey.
    Str("PM_RxDisplayMode", "fullscreen", "DISPLAY MODE: ", nullptr, nullptr, 0),
#if defined(LARECOMP_HAS_GPU)
    // Xenia's vsync only controls guest vblank, so it has a separate row under DEBUG > RENDERING.
    NativeOnly(Bool("PM_RxVsync", "mcla_gpu_vsync", "VSYNC: ")),
#endif
    Str("PM_RxResolution", "resolution", "RESOLUTION: ", nullptr, nullptr, 0),
#if defined(LARECOMP_HAS_GPU)
    // ASPECT RATIO: the shape of the picture in the window (mc_engine/display/frame_fit.h, ultrawide.h). FIT WINDOW fills
    // the window at any shape (a wider one shows more at the sides, a taller one more above and below);
    // 16X9, 16X10, 21X9 and 32X9 keep that shape and leave pure black bars where the window is not that
    // shape. Live, NATIVE only (the Xenia renderer's present always fills the window, 16:9 at least).
    // Width: "ASPECT RATIO: " 14 and "16X10" 5.
    NativeOnly(Str("PM_RxAspect", "aspect_ratio", "ASPECT RATIO: ", kAspectVals, kAspectNames, 5)),
    NativeOnly(Submenu("PM_RxSubGpuHdr", "HDR", "RxGpuHdrMenu")),
#endif
    // Live, both renderers: the game's gamma ramp is set again at the next frame
    // (hooks/display_gamma.cpp). CONSOLE is the console's TV ramp, PC an sRGB one. Width: 13, values 7.
    Str ("PM_RxDisplayGamma", "display_gamma",           "DISPLAY GAMMA: ",
         kDisplayGammaVals, kDisplayGammaNames, 2),
    // Grey with "(FRAME GEN)" while frame generation runs: the limiter then uses the row of its own
    // (frame_generation_menu.h) and counts rendered frames.
    Dbl("PM_RxFpsCap", "fps_limit", "FPS LIMIT: ", kFpsCapVals, 8, "%.0f", "",
                     "UNCAPPED"),
#if defined(LARECOMP_HAS_GPU)
    Str("PM_RxMsaa", "mcla_gpu_msaa", "MSAA: ", nullptr, nullptr, 0),
    NativeOnly(Str("PM_RxGpuAa", "mcla_gpu_aa", "POST AA: ", kPostAaVals, kPostAaNames, 3)),
#endif
};

// (DISPLAY GAMMA is on the DISPLAY page now, kVideoItems; the controller rows are on the CONTROLLER
// page, kControllerItems.)
// Width: the longest label is "ORIGINAL INTRO SPEED (NEXT START)" (33) and the values are ON / OFF (3),
// 36 together, under the 41 characters a page can take ("Page width" above ItemRowLabel).
const ItemDef kRecompItems[] = {
    Bool("PM_RxHidePointer", "hide_mouse_pointer",       "HIDE MOUSE POINTER: "),
    // Live (input/menu_mouse.cpp reads it every tick). The pointer hovers, clicks, steps the settings' values and scrolls with the wheel in the game's menus; off, the
    // mouse does nothing there. Width: "MOUSE IN MENUS" is 14, under HIDE MOUSE POINTER's 18.
    Bool("PM_RxMenuMouse",   "menu_mouse",               "MOUSE IN MENUS: "),
    // The legal screens and logos it paces have played before this menu can be opened, so a change
    // is seen at the next start. The word is NEXT START and not RESTART on purpose: nothing needs
    // the game started again, so the "restart now?" prompt (RowSaysRestart) must not offer it.
    // CHECK: RECOMP SETTINGS shows "ORIGINAL INTRO SPEED (NEXT START)" without a cut at the right
    // edge, and flipping the row then leaving the page does not offer "restart now?".
    Bool("PM_RxIntroSpeed",  "intro_original_speed",     "ORIGINAL INTRO SPEED: ", false,
         " (NEXT START)"),
    // The launcher's Skip Intro Videos (hooks/ui.cpp, kRequiresRestart: the legal screens and the
    // logos have played before this menu can be opened). NEXT START as the row above, for the same
    // reason: nothing needs the game started again now, so no "restart now?" prompt. Width: "SKIP
    // INTRO VIDEOS (NEXT START)" 30, under ORIGINAL INTRO SPEED's 33.
    Bool("PM_RxSkipIntro",   "skip_intro",               "SKIP INTRO VIDEOS: ", false,
         " (NEXT START)"),
};

// CONTROLLER: everything about the pad, in three runs: the game's vibration, the trigger effects
// (DualSense, then Xbox), then the sticks, and last the prompts' button set. The page is listed
// whether or not a pad is connected (nothing here asks): a keyboard player can set it up before
// plugging one in, and every row is read live by the code it belongs to, so it takes hold when a pad
// appears. A row for a pad that is not the one in use does nothing to the others (DUALSENSE rows
// touch a DualSense only, XBOX rows an Xbox One or Series pad only).
// Width ("Page width" above ItemRowLabel): the widest label is "DUALSENSE THROTTLE STRENGTH" (27), then
// "DUALSENSE BRAKE STRENGTH" (24), "XBOX TRIGGER STRENGTH" and "RIGHT STICK DEAD ZONE" (21); the widest
// value "PLAYSTATION" (11, BUTTON LAYOUT): 38 together, under the 41 a page can take; no row has a suffix.
// CHECK: on the SETTINGS tab, CONTROLLER is a new button between GAMEPLAY and INTERFACE (7 buttons
// with QUIT now, one more than before): the cursor reaches every button including QUIT, nothing is cut
// off the bottom of the tab's panel, and A opens a page of 10 rows in the order above with the line
// "Vibration, triggers, sticks and buttons." under it (not cut at the right edge). RECOMP SETTINGS has
// three rows (HIDE MOUSE POINTER, MOUSE IN MENUS, ORIGINAL INTRO SPEED); INTERFACE > UI has the three colour rows and
// SPEED UNITS, the colour swatches still lined up with their rows.
// Every row keeps the cvar it had (a value set on RECOMP SETTINGS before this change reads the same
// here, and the page's rows write larecomp.toml as before), and with no pad connected the page lists
// and steps as usual.
const ItemDef kControllerItems[] = {
    // Live: the game's rumble stops reaching the controller, and one that runs stops.
    Bool("PM_RxVibration",   "controller_vibration",     "VIBRATION: "),
    // Live. A DualSense only: a spring on R2 and a brake on L2 while a car is driven.
    Bool("PM_RxTrigResist",  "trigger_resistance",       "DUALSENSE RESISTANCE: "),
    // Live. A DualSense only: R2 vibrates while the wheels spin, L2 while the brakes lock them.
    Bool("PM_RxTrigVibrate", "trigger_vibration",        "DUALSENSE VIBRATION: "),
    // Live (adaptive_triggers.cpp reads both at every use). A DualSense only: how firm R2 and L2
    // are at the end of their travel, 1 (LIGHT) to 8 (FIRM); they matter while RESISTANCE is on and
    // are not greyed with it, as XBOX TRIGGER STRENGTH is not greyed with XBOX TRIGGER RUMBLE.
    // The launcher's DualSense Throttle / Brake Strength: same cvars, same steps. Width:
    // "DUALSENSE THROTTLE STRENGTH" 27 with "PLAYSTATION" 11 is 38 of 41.
    Dbl("PM_RxTrigThrottle", "trigger_throttle", "DUALSENSE THROTTLE STRENGTH: ",
                     mcla_menu::parity::kTriggerStrengthSteps,
                     mcla_menu::parity::kTriggerStrengthStepCount, "%.0f"),
    Dbl("PM_RxTrigBrake",    "trigger_brake",    "DUALSENSE BRAKE STRENGTH: ",
                     mcla_menu::parity::kTriggerStrengthSteps,
                     mcla_menu::parity::kTriggerStrengthStepCount, "%.0f"),
    // Live. Xbox One and Series pads: the same slip, in the trigger motors.
    Bool("PM_RxImpulse",     "impulse_triggers",         "XBOX TRIGGER RUMBLE: "),
    Dbl ("PM_RxImpulseStr",  "impulse_trigger_strength", "XBOX TRIGGER STRENGTH: ",
         kImpulseStrengthVals, 4, "%.0f%%"),
    // Live: for a stick that rests off centre (hooks/stick_deadzone.cpp).
    Dbl ("PM_RxDeadzoneL",   "left_stick_deadzone",      "LEFT STICK DEAD ZONE: ",
         kStickDeadzoneVals, 7, "%.0f%%", "", "OFF"),
    Dbl ("PM_RxDeadzoneR",   "right_stick_deadzone",     "RIGHT STICK DEAD ZONE: ",
         kStickDeadzoneVals, 7, "%.0f%%", "", "OFF"),
    // Which buttons the prompts show (hooks/ui.cpp): AUTO follows the pad in use. It moved here from
    // the UI page; the keyboard glyph set is for players with no pad, so the row is not pad-only.
    Str ("PM_RxButtonLayout", "button_prompts",          "BUTTON LAYOUT: ",
         kButtonLayoutVals, kButtonLayoutNames, 4),
};

constexpr double kCityLodVals[] = {0.5, 1.0, 1.5, 2.0};

const ItemDef kWorldDetailItems[] = {
    Dbl("PM_RxUnspawn", "traffic_unspawn_dist", "TRAFFIC RANGE: ",
                     kUnspawnVals, int(sizeof(kUnspawnVals) / sizeof(kUnspawnVals[0])), "%gM"),
    Dbl("PM_RxPedDensity", "ped_density_scale", "PEDESTRIAN DENSITY: ",
                     kDensityVals, int(sizeof(kDensityVals) / sizeof(kDensityVals[0])), "%gX"),
    Dbl("PM_RxParkedCars", "parked_car_scale", "PARKED CAR DENSITY: ",
                     kDensityVals, int(sizeof(kDensityVals) / sizeof(kDensityVals[0])), "%gX"),
    Dbl ("PM_RxFragDist",    "global_max_draw_distance", "PROP DISTANCE: ",
         kFragDrawDistVals, int(sizeof(kFragDrawDistVals) / sizeof(kFragDrawDistVals[0])),
         "%gM", "", "STOCK"),
    Dbl("PM_RxLodTraffic", "lod_traffic_scale", "TRAFFIC DETAIL DISTANCE: ", kTrafficLodVals,
                     int(sizeof(kTrafficLodVals) / sizeof(kTrafficLodVals[0])), "%gX"),
    Dbl("PM_RxLodCity", "lod_city_scale", "CITY DETAIL DISTANCE: ", kCityLodVals,
                     int(sizeof(kCityLodVals) / sizeof(kCityLodVals[0])), "%gX"),
};

#if defined(LARECOMP_HAS_GPU)
const char* const kQualityVals[] = {"off", "high", "ultra"};
const char* const kQualityNames[] = {"OFF", "HIGH", "ULTRA"};
const char* const kOnOffVals[] = {"off", "on"};
const char* const kOnOffNames[] = {"OFF", "ON"};
const char* const kCarAoRtVals[] = {"off", "ao", "contact", "both"};
const char* const kCarAoRtNames[] = {"OFF", "OCCLUSION", "SUN", "BOTH"};
const char* const kNativeAoLevelVals[] = {"light", "default", "strong", "max"};
const char* const kNativeAoLevelNames[] = {"1", "2", "3", "4"};
const char* const kNativeShadowFilterVals[] = {"game", "soft", "pcss"};
const char* const kNativeShadowFilterNames[] = {"GAME", "HIGH", "ULTRA"};
const char* const kNativeTrafficShadowsVals[] = {"off", "near", "all"};
const char* const kNativeShaftsVals[] = {"off", "light", "default", "strong", "stronger", "strongest"};
const char* const kNativeShaftsNames[] = {"OFF", "1", "2", "3", "4", "5"};
const char* const kNativeVolumetricsVals[] = {"off", "medium", "high", "ultra"};
const char* const kNativeVolumetricsNames[] = {"OFF", "MEDIUM", "HIGH", "ULTRA"};
const char* const kNativeSsgiLevelVals[] = {"light", "default", "strong", "stronger", "strongest"};
const char* const kNativeSsgiLevelNames[] = {"1", "2", "3", "4", "5"};
const char* const kSideViewVals[] = {"game", "high", "ultra"};
const char* const kSideViewNames[] = {"GAME", "HIGH", "ULTRA"};
const char* const kNativeAnisoVals[] = {"game", "off", "2", "4", "8", "16"};
const char* const kNativeAnisoNames[] = {"GAME", "OFF", "2X", "4X", "8X", "16X"};
const char* const kGfxPresetVals[] = {"vanilla", "enhanced", "enhanced_rt", "custom"};
const char* const kGfxPresetNames[] = {"VANILLA", "ENHANCED", "ENHANCED RT", "CUSTOM"};

struct LightShadowWords {
    const char* vals[mcla::gpu::local_shadow::kMenuWordMax] = {};
    const char* names[mcla::gpu::local_shadow::kMenuWordMax] = {};
    int count = 0;
};
constexpr LightShadowWords MakeLightShadowWords() {
    LightShadowWords w;
    const mcla::gpu::local_shadow::MenuWords& menu = mcla::gpu::local_shadow::kMenuWords;
    for (uint32_t i = 1; i < menu.count; ++i) {
        w.vals[i - 1] = menu.stored[i].data();   // the table's words are string literals: null terminated
        w.names[i - 1] = menu.shown[i].data();
    }
    w.count = int(menu.count) - 1;
    return w;
}
constexpr LightShadowWords kLightShadowWords = MakeLightShadowWords();
constexpr const char* const* kNativeLightShadowVals  = kLightShadowWords.vals;
constexpr const char* const* kNativeLightShadowNames = kLightShadowWords.names;
#endif

const char* const kXeniaAnisoVals[] = {"-1", "0", "1", "2", "3", "4", "5"};
const char* const kXeniaAnisoNames[] = {"GAME", "OFF", "1X", "2X", "4X", "8X", "16X"};

const ItemDef kImageQualityItems[] = {
#if defined(LARECOMP_HAS_GPU)
    NativeOnly(Str("PM_RxGpuAniso", "mcla_gpu_anisotropy", "TEXTURE FILTERING: ",
                   kNativeAnisoVals, kNativeAnisoNames, 6)),
#endif
    XeniaOnly(Str("PM_RxXeAniso", "anisotropic_override", "TEXTURE FILTERING: ",
                  kXeniaAnisoVals, kXeniaAnisoNames, 7)),
    Bool("PM_RxDof", "disable_dof", "DEPTH OF FIELD: ", true),
    Bool("PM_RxBlur", "disable_motion_blur", "MOTION BLUR: ", true),
};

const ItemDef kLightItems[] = {
    Bool("PM_RxNoShadows", "perf_no_shadows", "SUN SHADOWS: ", true),
#if defined(LARECOMP_HAS_GPU)
    NativeOnly(Str("PM_RxGpuShadowDetail", "mcla_gpu_shadow_resolution", "SUN SHADOW DETAIL: ",
                   kSideViewVals, kSideViewNames, 3)),
    NativeOnly(Str("PM_RxGpuShadowFilter", "mcla_gpu_shadow_filter", "SUN SHADOW FILTER: ",
                   kNativeShadowFilterVals, kNativeShadowFilterNames, 3)),
#endif
    Bool("PM_RxFoliage", "disable_imposter_shadows", "FOLIAGE SHADOWS: ", true),
#if defined(LARECOMP_HAS_GPU)
    NativeOnly(Str("PM_RxGpuTrafficShadows", "mcla_gpu_traffic_shadows", "TRAFFIC SHADOWS: ",
                   kNativeTrafficShadowsVals, kQualityNames, 3)),
    NativeOnly(Str("PM_RxGpuCarShadowMap", "mcla_gpu_car_shadow_map", "PLAYER CAR SHADOWS: ",
                   kQualityVals, kQualityNames, 3)),
    NativeOnly(Str("PM_RxGpuContact", "mcla_gpu_contact_shadows", "CONTACT SHADOWS: ",
                   kQualityVals, kQualityNames, 3)),
    NativeOnly(Str("PM_RxGpuRtShadows", "mcla_gpu_rt_shadows", "SUN SHADOWS RT: ",
                   kQualityVals, kQualityNames, 3)),
    NativeOnly(Str("PM_RxGpuAo", "mcla_gpu_ao", "AMBIENT OCCLUSION / RT: ",
                   nullptr, nullptr, 0)),
    NativeOnly(Str("PM_RxGpuCarAoRt", "mcla_gpu_ao_rt", "CAR OCCLUSION RT: ",
                   kCarAoRtVals, kCarAoRtNames, 4)),
    NativeOnly(Str("PM_RxGpuAoLevel", "mcla_gpu_ao_intensity", "OCCLUSION STRENGTH: ",
                   kNativeAoLevelVals, kNativeAoLevelNames, 4)),
    NativeOnly(Str("PM_RxGpuSsgi", "mcla_gpu_ssgi", "BOUNCE LIGHT: ",
                   kQualityVals, kQualityNames, 3)),
    NativeOnly(Str("PM_RxGpuRtGi", "mcla_gpu_rt_gi", "BOUNCE LIGHT RT: ",
                   kQualityVals, kQualityNames, 3)),
    NativeOnly(Str("PM_RxGpuSsgiLevel", "mcla_gpu_ssgi_intensity", "BOUNCE STRENGTH: ",
                   kNativeSsgiLevelVals, kNativeSsgiLevelNames, 5)),
    NativeOnly(Str("PM_RxGpuHeadlights", "mcla_gpu_headlights", "HEADLIGHTS: ",
                   kOnOffVals, kOnOffNames, 2)),
    NativeOnly(Str("PM_RxGpuLightShadows", "mcla_gpu_light_shadows", "LOCAL LIGHT SHADOWS: ",
                   kNativeLightShadowVals, kNativeLightShadowNames, kLightShadowWords.count)),
    NativeOnly(Str("PM_RxGpuRtLights", "mcla_gpu_rt_lights", "LOCAL LIGHT SHADOWS RT: ",
                   kOnOffVals, kOnOffNames, 2)),
    NativeOnly(Str("PM_RxGpuNeonLight", "mcla_gpu_neon_light", "NEON UNDERGLOW: ",
                   kOnOffVals, kOnOffNames, 2)),
    NativeOnly(Str("PM_RxGpuLightBounce", "mcla_gpu_light_bounce", "LIGHT BOUNCE: ",
                   kOnOffVals, kOnOffNames, 2)),
    NativeOnly(Str("PM_RxGpuSunShafts", "mcla_gpu_sun_shafts", "SUNBEAMS: ",
                   kNativeShaftsVals, kNativeShaftsNames, 6)),
    NativeOnly(Str("PM_RxGpuVolumetrics", "mcla_gpu_volumetrics", "LIGHT BEAMS: ",
                   kNativeVolumetricsVals, kNativeVolumetricsNames, 4)),
#endif
};

#if defined(LARECOMP_HAS_GPU)
// CHECK: inspect row widths and the live RT dependency states in the menu tour.
const ItemDef kReflectionItems[] = {
    NativeOnly(Str("PM_RxGpuRoadRefl", "mcla_gpu_reflection_resolution", "PLANAR REFLECTION DETAIL: ",
                   kSideViewVals, kSideViewNames, 3)),
    NativeOnly(Str("PM_RxGpuSsr", "mcla_gpu_ssr", "SURFACE REFLECTIONS: ",
                   kQualityVals, kQualityNames, 3)),
    NativeOnly(Str("PM_RxGpuSsrRt", "mcla_gpu_ssr_rt", "SURFACE REFLECTIONS RT: ",
                   kQualityVals, kQualityNames, 3)),
    NativeOnly(Str("PM_RxGpuCarRefl", "mcla_gpu_env_resolution", "PAINT REFLECTION DETAIL: ",
                   nullptr, nullptr, 0)),
    NativeOnly(Str("PM_RxGpuCarSelfRefl", "mcla_gpu_car_selfrefl", "CAR SELF-REFLECTIONS: ",
                   kOnOffVals, kOnOffNames, 2)),
    NativeOnly(Str("PM_RxGpuCarSelfReflRt", "mcla_gpu_car_selfrefl_rt", "CAR SELF-REFLECTIONS RT: ",
                   kOnOffVals, kOnOffNames, 2)),
    NativeOnly(Str("PM_RxGpuRtCityRefl", "mcla_gpu_rt_reflections", "CITY IN CAR PAINT RT: ",
                   kOnOffVals, kOnOffNames, 2)),
    NativeOnly(Str("PM_RxGpuSideMirrors", "mcla_gpu_side_mirrors", "MIRRORS: ",
                   kOnOffVals, kOnOffNames, 2)),
};
#endif

#if defined(LARECOMP_HAS_GPU)
// DISPLAY > HDR: HDR10 output and brightness controls.
const ItemDef kGpuHdrItems[] = {
    Str("PM_RxGpuHdr", "mcla_gpu_hdr", "HDR: ",
        kHdrModeVals, kHdrModeNames, 2),
    Dbl("PM_RxGpuHdrWhite", "mcla_gpu_hdr_paper_white_nits", "PAPER WHITE: ",
                     kHdrPaperWhiteVals, 10, "%.0f"),
    Dbl("PM_RxGpuHdrPeak", "mcla_gpu_hdr_peak_nits", "PEAK BRIGHTNESS: ",
                     kHdrPeakVals, 10, "%.0f"),
    // How far lights, the sun, the dusk sky and the headlights go above paper white.
    Dbl("PM_RxGpuHdrHighlights", "mcla_gpu_hdr_highlights", "HIGHLIGHTS: ",
                     kHdrHighlightVals, 4, "%g"),
    Bool("PM_RxGpuHdrTest", "mcla_gpu_hdr_test_pattern", "TEST PATTERN: "),
};
#endif

// DISPLAY > NVIDIA REFLEX / AMD ANTI-LAG 2 (mcla_gpu_low_latency, live): the GPU vendor's
// latency reducer. The row comes from the renderer's own answer (gpu/latency.h): it names the vendor's
// feature, and there is none where the GPU, the driver or the graphics API has no reducer, which is
// how a PC that cannot run it never sees it. Legacy Auto shows its effective level. Anti-Lag 2 has no
// boost, so its row stops at ON.
#if defined(LARECOMP_HAS_GPU)
const char* const kLowLatencyVals[]  = {"off", "on", "boost"};
const char* const kLowLatencyNames[] = {"OFF", "ON", "ON + BOOST"};

// Says why a reducer that is asked for is not working right now. While frame generation runs it is
// forced on (latency.cpp): the row is grey and says so. The vendor's name is shortened while a reason shows, as the denoiser row's and
// frame generation's are, so that DISPLAY's widest label stays at 24 (the page counts the widest label plus the widest value, 41 at most:
// "AMD ANTI-LAG 2 (NOT WORKING)" was 28): "ANTI-LAG 2 (NOT WORKING)" 24, "REFLEX (NOT WORKING)" 20.
const ItemDef kReflexRows[] = {
    NativeOnly(Str("PM_RxGpuReflex", "mcla_gpu_low_latency", "NVIDIA REFLEX: ",
                                kLowLatencyVals, kLowLatencyNames, 3)),
};
const ItemDef kAntiLagRows[] = {
    NativeOnly(Str("PM_RxGpuAntiLag", "mcla_gpu_low_latency", "AMD ANTI-LAG 2: ",
                                kLowLatencyVals, kLowLatencyNames, 2)),
};
#endif

// The rows DISPLAY takes from the renderer (MenuDef::extra), after its own: the vendor's latency reducer, or none.
const ItemDef* LowLatencyMenuRows(int& count) {
#if defined(LARECOMP_HAS_GPU)
    if (mcla_menu::PauseRowListed(kReflexRows[0])) {
        count = int(sizeof(kReflexRows) / sizeof(kReflexRows[0]));
        return kReflexRows;
    }
    if (mcla_menu::PauseRowListed(kAntiLagRows[0])) {
        count = int(sizeof(kAntiLagRows) / sizeof(kAntiLagRows[0]));
        return kAntiLagRows;
    }
#endif
    count = 0;
    return nullptr;
}

// DISPLAY's rows from the renderer (MenuDef::extra), after its own output rows (kVideoItems), in this order: the vendor's latency reducer (above; none where the GPU has none), then the upscaler's
// rows and frame generation's (frame_generation_menu.h UpscalingPageRows: UPSCALING, its mode row, SHARPNESS, the denoiser row where this PC has one, FRAME GENERATION, its multiplier and its FPS
// LIMIT), 8 at most. The owner folded the old UPSCALING page into DISPLAY (2026-11-23); the page's relayout hook is the upscaling rows' (UpscalingMenuRelayout), the one source of what a PC lists. The combined table is
// made once per combination of the two and kept: the menu holds pointers into it.
const ItemDef* DisplayPageRows(int& count) {
    int latency_count = 0;
    const ItemDef* latency = LowLatencyMenuRows(latency_count);
    int upscaling_count = 0;
    const ItemDef* upscaling = UpscalingPageRows(upscaling_count);
    if (upscaling_count == 0) {
        count = latency_count;
        return latency;
    }
    if (latency_count == 0) {
        count = upscaling_count;
        return upscaling;
    }
    struct Combined {
        const ItemDef* latency = nullptr;
        int latency_count = 0;
        const ItemDef* upscaling = nullptr;
        int upscaling_count = 0;
        std::vector<ItemDef> rows;
    };
    static std::mutex mutex;
    static std::vector<std::unique_ptr<Combined>>& tables = larecomp::Leak<std::vector<std::unique_ptr<Combined>>>();
    std::lock_guard lock(mutex);
    for (const std::unique_ptr<Combined>& t : tables) {
        if (t->latency == latency && t->latency_count == latency_count && t->upscaling == upscaling && t->upscaling_count == upscaling_count) {
            count = int(t->rows.size());
            return t->rows.data();
        }
    }
    auto made = std::make_unique<Combined>();
    made->latency = latency;
    made->latency_count = latency_count;
    made->upscaling = upscaling;
    made->upscaling_count = upscaling_count;
    for (int i = 0; i < latency_count; ++i) made->rows.push_back(latency[i]);
    for (int i = 0; i < upscaling_count; ++i) made->rows.push_back(upscaling[i]);
    count = int(made->rows.size());
    const ItemDef* rows = made->rows.data();
    tables.push_back(std::move(made));
    return rows;
}

// Where the rear view mirror sits (rearview_position, rear_view.cpp).
constexpr const char* kRearViewPosVals[]  = {"stock", "left_top", "middle_top",
                                             "left_down", "middle_footer", "right_down"};
constexpr const char* kRearViewPosNames[] = {"STOCK", "LEFT TOP", "MIDDLE TOP",
                                             "LEFT DOWN", "MIDDLE FOOTER", "RIGHT DOWN"};

// The camera hub (CAMERA) opens one menu per part of the camera. The free-look
// rows come from camera_look.cpp, next to the code that reads them.
const ItemDef kCamHubItems[] = {
    Submenu("PM_RxSubCamChase", "CHASE CAMERA", "RxCamChaseMenu"),
    Submenu("PM_RxSubCamFov",   "FOV",          "RxCamFovMenu"),
    Submenu("PM_RxSubCamRear",  "REAR VIEW",    "RxCamRearMenu"),
    Submenu("PM_RxSubCamLook",  "LOOK AROUND",  "RxCamLookMenu"),
};

const ItemDef kCamChaseItems[] = {
    Dbl ("PM_RxSmoothFactor", "chase_cam_smoothing_factor", "CAM SMOOTH FACTOR: ", kCamSmoothVals, 7, "%.1fX"),
    // Live (hooks/camera.cpp reads it at every shake). The launcher's Camera Shake: OFF, 0.3X to 3.0X,
    // 0.6X is the default, 1.0X the console's full shake (pause_parity_rows.h).
    Dbl ("PM_RxCamShake",     "camera_shake_scale", "CAMERA SHAKE: ", mcla_menu::parity::kCameraShakeSteps,
         mcla_menu::parity::kCameraShakeStepCount, mcla_menu::parity::kCameraShakeFormat, "",
         mcla_menu::parity::kCameraShakeZeroLabel),
};

const ItemDef kCamFovItems[] = {
    Dbl ("PM_RxFov1P",        "fov_1p_scale", "1P FOV: ", kFovValues, 13, "%.1fX"),
    Dbl ("PM_RxFov3P",        "fov_3p_scale", "3P FOV: ", kFovValues, 13, "%.1fX"),
};

// The rear view mirror the retail game shipped switched off (rear_view.cpp).
// All four are live.
const ItemDef kCamRearItems[] = {
    Bool("PM_RxRearView",     "rearview",          "REAR VIEW: "),
    Str ("PM_RxRearViewPos",  "rearview_position", "REAR VIEW POS: ",
         kRearViewPosVals, kRearViewPosNames, 6),
    Bool("PM_RxRearViewFrm",  "rearview_frame",    "REAR VIEW FRAME: "),
    // The launcher's Cars in Rear View: the racers' cars in the mirror, which the retail game leaves
    // out. It draws nothing while REAR VIEW is off; like REAR VIEW POS and FRAME above it is not
    // greyed with it (a grey reason would not fit the page: "CARS IN REAR VIEW (NO MIRROR)" 29 with
    // "MIDDLE FOOTER" 13 is 42 of 41).
    XeniaOnly(Bool("PM_RxRearViewCars", "rearview_cars", "CARS IN REAR VIEW: ")),
};

// The full map's mouse (map_mouse.cpp), the launcher's Full map section: four live switches.
// MAGNET is off by default (the map's cursor magnet feels sticky under a mouse; a stick keeps its pull
// either way). Width: "HIDE POINTER ON MAP" 19 with ON / OFF 3.
const ItemDef kMapMouseItems[] = {
    Bool("PM_RxMapMouse",     "map_mouse",             "MAP MOUSE: "),
    Bool("PM_RxMapMouseZoom", "map_mouse_zoom",        "MAP MOUSE ZOOM: "),
    Bool("PM_RxMapMouseHide", "map_mouse_hide_cursor", "HIDE POINTER ON MAP: "),
    Bool("PM_RxMapMouseMag",  "map_mouse_magnet",      "MAP CURSOR MAGNET: "),
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

// The five latin languages the disc has strings for, and PORTUGUES when the mods' string tables
// have Portuguese text (PortugueseAvailable; it is the last entry, so the list is one shorter
// without it). AUTO is the game's own default (English unless user_language says otherwise). A
// saved pt that is not available shows as AUTO, which is what runs; the setting itself is not
// touched. A change reloads the text at once, but the game reads its language in several places
// at start, so the row says "(RESTART)" and leaving the menu offers RESTART NOW.
const ItemDef kLangItems[] = {
    Str("PM_RxLanguage", "language", "LANGUAGE: ",
                     kLangVals, kLangNames, kNumLangs, " (RESTART)"),
};

#if defined(LARECOMP_HAS_GPU)
// RENDERER (the hidden DEBUG menu, RENDERING page: debug_menu_pages.cpp): which renderer the NEXT start runs. NATIVE is LARecomp's own
// (Plume), XENIA D3D12 and XENIA VULKAN are the SDK's Xenia GPU, which stays in the program as a hidden fallback for a PC where the native
// renderer will not run (gpu/renderer_fallback.h). The choice is the SDK's gpu.driver, which is startup-only, so the row HOLDS it
// (kDeferredCvars): the next save writes it to larecomp.toml, leaving the menu offers RESTART NOW (the label says "(RESTART)"), and the
// running renderer does not change. gpu/renderer_choice.h has the mapping (PickOfDriver / DriverOfPick). Not a row of any normal menu and
// not in the launcher. A launch option or an environment variable that sets gpu.driver beats the file, so the row is grey then.
// Width: "RENDERER (LAUNCH OPTION)" 24; "RENDERER: XENIA VULKAN (RESTART)" 32, under the 41 a page takes.
constexpr const char* kRendererCvar = "gpu.driver";
#if defined(_WIN32)
constexpr bool kRendererOnWindows = true;
#else
constexpr bool kRendererOnWindows = false;
#endif
bool RendererLaunchSets() {
    const larecomp::cvar::Source source = larecomp::cvar::GetFlagSource(kRendererCvar);
    return source == larecomp::cvar::Source::kCommandLine || source == larecomp::cvar::Source::kEnvironment;
}
#endif

// Cutscene gallery. One value row walking the whole table plus the button
// that plays it -- the row's value window is only ever three wide (see
// BuildRowValueSet), so a list this long costs nothing extra.
const ItemDef kCutsceneItems[] = {
    Str   ("PM_RxCutPick", "cutscene_replay", "CUTSCENE: ",
           CutsceneIds(), CutsceneLabels(), CutsceneCount()),
    Action("PM_RxCutPlay", kActionPlayCutscene),
};

// UI OPTIONS: the ui_theme_* cvars (hooks/ui_theme.cpp). Value 0 is "" (the
// game's own color); 1..24 are the swatches of ui_palette.cpp, filled in by
// InitThemePalette() before the rows are built. Labels stay inside A-Z, 0-9 and
// space like every row label (see the speed-unit note above); the swatches
// themselves are a grid ui_palette.cpp builds inside the pause movie, under the
// panel, outlining the selected row's color.
constexpr int kNumThemeColors = 1 + larecomp::kUiPaletteColors;
const char* g_theme_vals[kNumThemeColors] = {};

void InitThemePalette() {
    larecomp::UiPaletteInit();
    for (int k = 0; k < kNumThemeColors; ++k) g_theme_vals[k] = larecomp::UiPaletteHex(k);
}

#if defined(LARECOMP_HAS_GPU)
// HUD WIDTH (hud_width, ultrawide.h HudWidthFraction): on a picture wider than 16:9, how far the HUD's
// corner pieces (the minimap, speedometer, race position, lap and time, the phone) move out toward the screen
// edges, in percent. The row names three steps (the launcher's the same): CENTERED (0, the default)
// keeps the whole HUD in the middle 16:9, WIDER (50) moves the pieces halfway out, FULL (100) puts each
// piece as far from its edge as on a 16:9 screen. Live, NATIVE only (the renderer moves the pieces).
// Another value set by hand in larecomp.toml (60) gets an entry of its own, shown as its number
// (value_label). Grey with "(16X9: NO EFFECT)" while the picture is the game's own 16:9, where the setting
// moves nothing (a wider picture moves the pieces to the sides, a taller one to the top and the bottom).
// Width: "HUD WIDTH (16X9: NO EFFECT)" 27 (grey: no value), "HUD WIDTH" 9 with
// "CENTERED" 8.
constexpr double kHudWidthVals[] = {0, 50, 100};
#endif

const ItemDef kUiItems[] = {
    Color("PM_RxUiAccent", "ui_theme_accent", "MENU BLUE", g_theme_vals, kNumThemeColors),
    Color("PM_RxUiTitle",  "ui_theme_title",  "TITLES AND GLOBE", g_theme_vals,
          kNumThemeColors),
    Color("PM_RxUiFrame",  "ui_theme_frame",  "FRAME GRAY", g_theme_vals, kNumThemeColors),
    Str ("PM_RxSpeedUnits",  "speed_units",              "SPEED UNITS: ",
         kSpeedUnitVals, kSpeedUnitNames, 3),
#if defined(LARECOMP_HAS_GPU)
    NativeOnly(Dbl("PM_RxHudWidth", "hud_width", "HUD WIDTH: ", kHudWidthVals,
                                int(sizeof(kHudWidthVals) / sizeof(kHudWidthVals[0])), "%g")),
#endif
    // (BUTTON LAYOUT is on the CONTROLLER page now, kControllerItems.)
};

// The SETTINGS tab holds a few pages of rows (RECOMP SETTINGS, CONTROLLER) and a handful of hubs,
// each a list of Submenu rows. The hubs' children are ordinary menus below; a menu some row opens gets
// no button of its own (MenuIsNested).
// GRAPHICS (see "The graphics pages" above): five pages and, on the NATIVE renderer, the PRESET row after DISPLAY; REFLECTIONS on the NATIVE renderer only. (UPSCALING was the second one: its rows are DISPLAY's now, owner 2026-11-23.)
// A hub row whose page has no row for the running renderer is not listed (the Submenu row carries the page's renderer mask). Width: "LIGHTING AND SHADOWS" (20), no values.
const ItemDef kGfxHubItems[] = {
    Submenu("PM_RxSubVideo",  "DISPLAY",     "RxVideoMenu"),
#if defined(LARECOMP_HAS_GPU)
    // PRESET: the four effect pages. Offer ENHANCED RT on devices with ray tracing.
    NativeOnly(Str("PM_RxGpuPreset", "mcla_gpu_preset", "PRESET: ", kGfxPresetVals, kGfxPresetNames, 4)),
#endif
    Submenu("PM_RxSubImageQuality", "IMAGE QUALITY", "RxImageQualityMenu"),
    Submenu("PM_RxSubLighting", "LIGHTING AND SHADOWS", "RxLightingMenu"),
#if defined(LARECOMP_HAS_GPU)
    NativeOnly(Submenu("PM_RxSubReflections", "REFLECTIONS", "RxReflectionsMenu")),
#endif
    Submenu("PM_RxSubWorldDetail", "WORLD DETAIL", "RxWorldDetailMenu"),
};
const ItemDef kGameHubItems[] = {
    Submenu("PM_RxSubCam",    "CAMERA",          "RxCamMenu"),
    Submenu("PM_RxSubTod",    "TIME OF DAY",     "RxTodMenu"),
    Submenu("PM_RxSubCarbon", "CARBON FIBER",    "RxCarbonMenu"),
    Submenu("PM_RxSubDiff",   "DIFFICULTY",      "RxDiffMenu"),
    // Last, so the rows above keep their places (a tour counts its D-pad presses from the top).
    Submenu("PM_RxSubMap",    "MAP MOUSE",       "RxMapMouseMenu"),
};
// The AI: one percent for the difficulty, then the rubberband taken apart so
// each half can be moved alone. Every row is live. (The on/off switch that
// turned the rubberband off whole, and with it the leash on a leading AI
// (MinThrottle), is not a row any more: the scales below soften it.)
const ItemDef kDiffItems[] = {
    Str("PM_RxDiffPreset", "difficulty_preset", "PRESET: ", kDiffPresetVals, kDiffPresetNames, 2),
    Dbl("PM_RxAiDiff", "ai_difficulty", "AI DIFFICULTY: ",
                     kAiDiffVals, int(sizeof(kAiDiffVals) / sizeof(kAiDiffVals[0])), "%g"),
    Dbl ("PM_RxRbCatchup",   "rubberband_catchup_scale", "CATCH-UP (AI BEHIND): ",
         kRbScaleVals, int(sizeof(kRbScaleVals) / sizeof(kRbScaleVals[0])), "%gX"),
    Dbl ("PM_RxRbHoldback",  "rubberband_holdback_scale", "HOLD-BACK (AI AHEAD): ",
         kRbScaleVals, int(sizeof(kRbScaleVals) / sizeof(kRbScaleVals[0])), "%gX"),
    Dbl ("PM_RxRbDist",      "rubberband_distance_scale", "RUBBERBAND RANGE: ",
         kRbDistVals, int(sizeof(kRbDistVals) / sizeof(kRbDistVals[0])), "%gX"),
    Dbl ("PM_RxRbShortcut",  "rubberband_shortcut_scale", "AI SHORTCUTS WHEN BEHIND: ",
         kRbNitroScaleVals, int(sizeof(kRbNitroScaleVals) / sizeof(kRbNitroScaleVals[0])), "%gX"),
    Dbl("PM_RxRbNitroStart", "rubberband_nitro_start_dist", "NITRO FROM START: ",
                     kRbNitroStartVals,
                     int(sizeof(kRbNitroStartVals) / sizeof(kRbNitroStartVals[0])), "%g"),
    Dbl ("PM_RxRbNitroFin",  "rubberband_nitro_finish_scale", "NITRO NEAR FINISH: ",
         kRbNitroScaleVals, int(sizeof(kRbNitroScaleVals) / sizeof(kRbNitroScaleVals[0])), "%gX"),
    Dbl ("PM_RxRbNitroBeh",  "rubberband_nitro_behind_scale", "NITRO BEHIND YOU: ",
         kRbNitroScaleVals, int(sizeof(kRbNitroScaleVals) / sizeof(kRbNitroScaleVals[0])), "%gX"),
};
const ItemDef kIfaceHubItems[] = {
    Submenu("PM_RxSubUi",   "UI",        "RxUiMenu"),
    Submenu("PM_RxSubLang", "LANGUAGES", "RxLangMenu"),
};
const ItemDef kExtrasHubItems[] = {
    Submenu("PM_RxSubCut", "CUTSCENES", "RxCutMenu"),
#if defined(LARECOMP_HAS_GPU)
    // Texture mods on the Plume renderer (it had a page of its own on GRAPHICS). The two modder actions (DUMP SCENE TEXTURES, RELOAD
    // TEXTURE MODS) are on the DEBUG menu's TOOLS page (debug_menu_pages.cpp). Width: "TEXTURE MODS" 12.
    NativeOnly(Bool("PM_RxTexMods", "texture_mods", "TEXTURE MODS: ")),
#endif
    Bool("PM_RxVinylLayers", "extra_vinyl_layers",       "EXTRA VINYL LAYERS: ", false, " (RESTART)"),
    Bool("PM_RxBigSession",  "net_lift_big_session_limits", "NO 8P LIMITS: ", false,
         " (NEXT HOST)"),
    // The car is moved once per session, when it first appears at Hollywood's door (map_warp.cpp
    // TickStartGarage), so a change shows the next time the game is started. NEXT START and not
    // RESTART, as ORIGINAL INTRO SPEED: nothing needs the game started again now, so the "restart
    // now?" prompt (RowSaysRestart) must not offer it. Width: "START GARAGE (NEXT START)" 25 with
    // "SOUTH CENTRAL" 13.
    Str ("PM_RxStartGarage", "start_garage", "START GARAGE: ",
         kStartGarageVals, kStartGarageNames, 4, " (NEXT START)"),
};
// NO first: the cursor starts on the first row, so a stray A keeps the game.
const ItemDef kQuitItems[] = {
    Action("PM_RxQuitNo",  kActionQuitNo),
    Action("PM_RxQuitYes", kActionQuitYes),
    Action("PM_RxQuitRestart", kActionRestart),
};
// Shown over the menu that was just left when a setting that needs a restart was changed in
// this session (RestartPromptTick). NOT NOW first, like NO on the QUIT form: a stray A keeps
// playing, and B closes it the same way.
const ItemDef kRestartAskItems[] = {
    Action("PM_RxRestartLater", kActionRestartLater),
    Action("PM_RxRestartNow",   kActionRestartNow),
};

// One RexGlue menu: a native list form (ui_forms.h) of `items` rows. A menu no
// Submenu row opens gets a button on the pause SETTINGS tab and opens from the
// pause screen, the way OPTIONS does; one that a Submenu row opens is reached
// only through that row, the way CONTROLLER is.
struct MenuDef {
    const char* btn_key;   // SETTINGS-tab button: state name + string-table key
    const char* label;     // button label AND form title
    // The line the form shows under its list, where OPTIONS says "What do you
    // want to adjust?". Short and plain ASCII like the game's own (about 40
    // characters fit). Null = that same OPTIONS line.
    const char* desc;
    const char* menu_key;  // the form's title key; what a Submenu row names
    const ItemDef* items;
    int num_items;
    // Rows contributed by the feature's own translation unit, appended after
    // `items`. Null for a tab that is entirely described here. A function
    // rather than a pointer so nothing depends on static initialisation order
    // between two translation units.
    const ItemDef* (*extra)(int& count);
    // True for a menu only code opens (the "restart now?" prompt): no button on the SETTINGS tab.
    bool opened_by_code = false;
    // For a menu whose `extra` rows can change while the game runs (the upscalers: one that fails is
    // not listed any more). The guest rows are built once and indexed by position, so `extra` keeps
    // returning the rows the form was built with until this is applied: apply=false says whether the
    // live rows are no longer those, apply=true makes them the ones `extra` returns from then on.
    // EnsureMenuForm builds the form again, when it is closed, if it says so. Null: the rows never
    // change.
    bool (*relayout)(bool apply) = nullptr;
    // True for the hidden DEBUG menu: its hub and its pages (debug_menu_pages.cpp). Such a menu has no rows
    // and no button while the debug menu is locked (MenuLocked), so a locked game builds, lists and logs
    // nothing of it.
    bool debug_only = false;
};

// A menu of the hidden DEBUG menu while DebugMenuUnlocked() is false: it counts as empty (MenuRawCount),
// is not a button of the SETTINGS tab (EnsureSubmenus) and is not one of any other menu's pages
// (MenuIsNested). The answer is fixed at start-up (InitDebugMenu), so it cannot change under an open menu.
bool MenuLocked(const MenuDef& md) { return md.debug_only && !larecomp::DebugMenuUnlocked(); }

// The renderer this launch runs, as a RendererMask. The renderer is a
// restart-only choice (the SDK's gpu.driver; the hidden DEBUG menu's RENDERER row, the box
// of a start that failed, or larecomp.toml sets it), so it cannot change under an open menu and the rows listed
// never move. mcla::gpu::Enabled() is the one answer to "does LARecomp's own
// renderer run": the old mcla_gpu setting (gone on rexglue 0.11) was true under
// the XENIA renderer too, so it could not tell them apart.
uint8_t RunningRendererMask() {
    static const uint8_t mask = [] {
#if defined(LARECOMP_HAS_GPU)
        if (mcla::gpu::Enabled())
            return uint8_t(mcla_menu::kRendNative);
#endif
        return uint8_t(mcla_menu::kRendXenia);
    }();
    return mask;
}

// A submenu's rows are `items` followed by whatever `extra` returns, less the
// rows of the renderer this launch is not running. These helpers are the only
// way the rest of the file is allowed to reach any of it, so a tab with
// contributed or filtered rows behaves exactly like a plain one and the
// list's selected index keeps naming the right row.
int MenuRawCount(const MenuDef& md) {
    if (MenuLocked(md)) return 0;
    int extra = 0;
    if (md.extra) md.extra(extra);
    return md.num_items + extra;
}

const ItemDef& MenuRawAt(const MenuDef& md, int i) {
    if (i < md.num_items) return md.items[i];
    int extra = 0;
    const ItemDef* tail = md.extra ? md.extra(extra) : nullptr;
    // Callers all bound i by MenuRawCount, but a row table that shrank between
    // the two calls would be a crash rather than a wrong label, so clamp.
    int at = i - md.num_items;
    if (!tail || extra <= 0) return md.items[0];
    if (at >= extra) at = extra - 1;
    return tail[at];
}

// CHECK: verify the 41-column DISPLAY page, guest relabel, grey-row input,
// restart prompts and probe-driven relayout on D3D12, Vulkan and Xenia.
namespace expr = larecomp::uiexpr;

const expr::RuleBook& PauseRules() {
    static const larecomp::Immortal<expr::RuleBook> rules(kPauseMenuRulesToml);
    return *rules;
}

class PauseRuleEnv final : public expr::Env {
 public:
    explicit PauseRuleEnv(bool unprobed_world_scale = false) : unprobed_world_scale_(unprobed_world_scale) {}

    expr::Value Setting(std::string_view name) const override {
        const std::string key(name);
        const auto* setting = larecomp::settings::Find(name);
        if (!setting) return {};
        const auto text = expr::Value::String(CvarGet(key.c_str()));
        using Type = rex::SettingType;
        switch (setting->GetType()) {
        case Type::kBool: return expr::TypedSetting("bool", text);
        case Type::kString:
        case Type::kPath: return text;
        case Type::kF64: return expr::TypedSetting("number", text);
        default: return expr::TypedSetting("int", text);
        }
    }

    expr::Value Renderer() const override {
        return expr::Value::String(mcla_menu::NativeRendererRuns() ? "native" : "xenia");
    }
    const expr::AutoDescriptor* Auto(std::string_view name) const override {
        static const larecomp::Immortal<expr::AutoCatalog> descriptors(expr::MenuAutoDescriptors());
        const auto it = descriptors->find(name);
        return it == descriptors->end() ? nullptr : &it->second;
    }
    const expr::Def* DefOf(std::string_view name) const override {
        return PauseRules().Definition(name);
    }

    expr::Value Fact(std::string_view path) const override {
        using Value = expr::Value;
        const auto text = [](std::string_view value) { return Value::String(std::string(value)); };
        if (path == "os.exclusive_fullscreen") return Value::Bool(mcla::display::ExclusiveSupported());
        if (path == "os.live_window_resize") return Value::Bool(mcla::display::LiveResize());
        if (path == "os.window_size_fixed") return Value::Bool(mcla::display::WindowSizeFixed());
        if (path == "display.primary.size" || path == "live.output_size") {
            const auto size = path == "display.primary.size" ? mcla::display::DesktopSize() : mcla::display::OutputSize();
            return size.empty() ? Value{} : Value::String(mcla::display::SizeLabel(size));
        }
        if (path == "install.portuguese_text") return Value::Bool(PortugueseAvailable());
#if defined(LARECOMP_HAS_GPU)
        namespace gpu = mcla::gpu;
        namespace gc = gpu::gpu_class;
        if (path == "gpu.class") {
            if (unprobed_world_scale_) return text("low");
            return gc::Known() ? text(gc::ClassName(gc::Current())) : Value{};
        }
        if (path == "gpu.handheld") return Value::Bool(gc::Handheld());
        if (path == "gpu.rt_tier") return text(gc::RtTierName(gc::CurrentRtTier()));
        if (path == "gpu.rt_rung") return text(gc::RtTierName(gc::RtRung(gc::Current(), gc::CurrentRtTier())));
        if (path == "api.car_rt") return Value::Bool(gpu::CarRtReflAvailable());
        if (path == "api.city_rt_shadows") return Value::Bool(gpu::CityRtShadowsAvailable());
        if (path == "build.rt_gi_update_built") return Value::Bool(gpu::RtGiUpdateBuilt());
        if (path == "gpu.vram_warning") {
            const auto hint = gpu::vram_warning::CurrentHint();
            return hint.empty() ? Value{} : text(hint);
        }
        if (path == "live.hdr_active") return Value::Bool(gpu::HdrOutputActive());
        if (path == "features.hdr.auto_paper_white_nits")
            return Value::Number(std::nearbyint(gpu::HdrAutoPaperWhiteNits()));
        if (path == "features.hdr.auto_peak_nits")
            return Value::Number(std::nearbyint(gpu::HdrAutoPeakNits()));
        if (path == "live.ultrawide_active") return Value::Bool(mc::ultrawide::Active());
        if (path == "live.rt_gi_replaces_bounce") return Value::Bool(gpu::RtGiReplacesBounceLight());
        if (path == "live.rtao_pass_built") return Value::Bool(gpu::rtao_setting::PassBuilt());
        if (path == "live.msaa_switching") return Value::Bool(gpu::MsaaSwitching());
        if (path == "live.renderer_forced") return Value::Bool(RendererLaunchSets());
        if (path == "live.renderer_choice")
            return text(gpu::DriverOfPick(gpu::PickOfDriver(CvarGet(kRendererCvar), kRendererOnWindows)));
        if (path == "features.denoiser.vendor") return text(mcla_menu::VendorDenoiserKind());
        if (path == "live.denoiser_gate") return text(mcla_menu::VendorDenoiserState());
        if (path == "live.fsr1_preset_custom") {
            auto preset = gpu::FsrPreset::kCustom;
            gpu::ParseFsrPreset(CvarGet("mcla_gpu_fsr_preset"), &preset);
            return Value::Bool(preset == gpu::FsrPreset::kCustom);
        }
        if (path == "live.city_data") {
            using Data = gpu::city_refl_setting::Data;
            switch (gpu::city_refl_setting::DataNow()) {
            case Data::kReady: return text("ready");
            case Data::kBuilding: return text("building");
            case Data::kUnavailable: return text("unavailable");
            default: return {};
            }
        }
        if (path == "features.latency.kind") {
            using Kind = gpu::latency::Kind;
            const auto kind = gpu::latency::AvailableKind();
            return text(kind == Kind::kReflex ? "reflex" : kind == Kind::kAntiLag ? "antilag2" : "none");
        }
        if (path == "features.latency.auto_on") return Value::Bool(gpu::latency::AutoIsOn());
        if (path == "live.latency_health") {
            using Health = gpu::latency::Health;
            const auto health = gpu::latency::CurrentHealth();
            return text(health == Health::kPaused ? "paused" : health == Health::kFailed ? "failed" : "ok");
        }
        if (path == "live.frame_generation_running")
            return Value::Bool(gpu::FrameGenerationRunning() || gpu::DlssFrameGenerationRunning());
        if (path == "live.frame_generation_multiplier") return Value::Number(gpu::DlssFrameGenerationMultiplier());
        if (path.starts_with("features.upscalers.") || path == "live.dlss_waits_for_restart" ||
            path == "live.kept_upscaler" || path == "live.upscaler_state") {
            const auto a = gpu::GetUpscalerAvailability();
            if (path == "features.upscalers.fsr31") return Value::Bool(a.fsr31);
            if (path == "features.upscalers.fsr4") return Value::Bool(a.fsr4);
            if (path == "features.upscalers.dlss")
                return text(a.dlss ? "ready" : a.dlss_needs_setup ? "needs_setup" :
                            a.dlss_after_restart ? "after_restart" : "no");
            if (path == "features.upscalers.dlss_modes_refused") {
                std::vector<Value> refused;
                for (int i = 0; i < int(gpu::UpscaleMode::kCount); ++i) {
                    const auto mode = gpu::UpscaleMode(i);
                    if (a.DlssModeRefused(mode)) refused.push_back(text(gpu::ModeInfoOf(gpu::Upscaler::kDlss, mode).cvar));
                }
                return Value::List(std::move(refused));
            }
            if (path == "live.dlss_waits_for_restart") return Value::Bool(a.dlss_after_restart && !a.dlss);
            if (path == "live.kept_upscaler")
                return text(a.kept == gpu::Upscaler::kFsr31 ? "fsr31" : a.kept == gpu::Upscaler::kFsr4 ? "fsr4" :
                            a.kept == gpu::Upscaler::kDlss ? "dlss" : "off");
            if (path == "live.upscaler_state") return text(a.no_room ? "no_room" : a.switching ? "switching" : "ok");
        }
        if (path.starts_with("features.frame_generation.") || path.starts_with("live.frame_generation_") ||
            path.starts_with("live.fsr_frame_generation_") || path.starts_with("live.dlss_frame_generation_")) {
            const auto fsr = gpu::GetFrameGenerationAvailability();
            const auto dlss = gpu::GetDlssFrameGenerationAvailability();
            if (path == "features.frame_generation.fsr31") return Value::Bool(fsr.available);
            if (path == "features.frame_generation.dlss") return Value::Bool(dlss.supported);
            if (path == "features.frame_generation.dlss_after_restart") return Value::Bool(dlss.after_restart);
            if (path == "features.frame_generation.dlss_multiplier_max") return Value::Number(dlss.max_multiplier);
            if (path == "live.fsr_frame_generation_listed") return Value::Bool(fsr.probed && (fsr.available || fsr.failed));
            if (path == "live.dlss_frame_generation_listed") return Value::Bool(dlss.listed);
            if (path == "live.frame_generation_multiplier_listed") return Value::Bool(dlss.listed && dlss.multiplier_choice);
            if (path == "live.fsr_frame_generation_state")
                return text(fsr.failed ? "failed" : fsr.no_room ? "no_room" : "ok");
            if (path == "live.frame_generation_half_display") {
                if (CvarGet("mcla_gpu_frame_generation") != "dlss") return Value::Number(fsr.half_display);
                const uint32_t multiplier = gpu::DlssFrameGenerationMultiplier();
                return Value::Number(dlss.refresh_hz >= 100 && multiplier >= 2 ?
                                     uint32_t(0.97 * double(dlss.refresh_hz) / double(multiplier)) : 0);
            }
            const bool unsupported = dlss.probed && !dlss.supported && !dlss.after_restart;
            if (path == "live.dlss_frame_generation_state")
                return text(dlss.failed ? "failed" : unsupported ? "unsupported" : "ok");
            const std::string selected = CvarGet("mcla_gpu_frame_generation");
            if (path == "live.frame_generation_choice_available")
                return Value::Bool(selected == "fsr31" ? fsr.available : selected == "dlss" && (dlss.supported || dlss.running));
            if (path == "live.frame_generation_applying")
                return Value::Bool(selected == "fsr31" ? !fsr.failed && !fsr.no_room && fsr.available && !fsr.running :
                                   selected == "dlss" && !dlss.failed && !unsupported && !dlss.after_restart && !dlss.running);
        }
#else
        if (path == "gpu.class") return text("low");
        if (path == "live.frame_generation_running") return Value::Bool(false);
#endif
        return {};
    }

 private:
    [[maybe_unused]] bool unprobed_world_scale_;
};

const expr::RowRules* ItemRules(const ItemDef& item) { return PauseRules().Row(item.key); }

const toml::table* ItemState(const ItemDef& item) {
    const auto* rule = ItemRules(item);
    if (!rule) return nullptr;
    const PauseRuleEnv env;
    return expr::RuleView(*rule, env, &PauseRules()).State();
}

// A variant may replace the setting in the exported UI. In the guest menu a
// compound row can leave it null so that it doesn't own a restart prompt.
const char* VariantCvar(const ItemDef& item, const expr::RuleView& row) {
    if (const auto* setting = row.Field("setting").as_string()) return setting->get().c_str();
    return item.cvar;
}

bool HasRuleChoices(const ItemDef& item) {
    const auto* rules = ItemRules(item);
    if (item.kind != ItemKind::kStrCycle || !rules) return false;
    const auto has_values = [](const expr::RuleFields& fields) {
        return fields.data->contains("values") || fields.data->contains("values_from") || fields.data->contains("values_ref");
    };
    return has_values(rules->base) || std::any_of(rules->variants.begin(), rules->variants.end(),
        [&](const auto& variant) { return has_values(variant.second); });
}

std::optional<mcla_menu::GeneratedChoices> GenerateRuleValues(const expr::RuleView& row, const expr::Env& env) {
    const auto source = row.Field("values_from");
    if (!source) return {};
    const auto generator = source["generator"].value_or(std::string_view{});
    const auto args = source["args"];
    if (generator == "display.sizes") {
        const auto mode = args["mode"].value_or(std::string_view{});
        return mcla_menu::DisplaySizeChoices(expr::ValueText(expr::Evaluate(expr::Parse(mode), env)));
    }
    if (generator == "render_scale.shares")
        return mcla_menu::RenderScaleChoices(args["renderer"].value_or(std::string_view{}));
    if (generator == "render_scale.fsr1_modes") return mcla_menu::Fsr1ModeChoices();
    if (generator == "upscaler.sharpness")
        return mcla_menu::SharpnessChoices(args["setting"].value_or(std::string_view{}));
    return {};
}

int RuleChoiceCount(const ItemDef& item) {
    const PauseRuleEnv env;
    const expr::RuleView row(*ItemRules(item), env, &PauseRules());
    if (const auto values = GenerateRuleValues(row, env)) return int(values->entries.size());
    return int(row.Choices().size());
}

int RuleChoiceIndex(const ItemDef& item) {
    const PauseRuleEnv env;
    const expr::RuleView row(*ItemRules(item), env, &PauseRules());
    if (const auto values = GenerateRuleValues(row, env)) return values->current;
    const auto values = row.Choices();
    const auto& decode = row.Owner("decode").decode;
    const char* setting = VariantCvar(item, row);
    const std::string current = decode.expr || !decode.cases.empty() ? expr::ValueText(expr::Decode(decode, env)) :
                                setting ? CvarGet(setting) : std::string{};
    for (int i = 0; i < int(values.size()); ++i) {
        if ((*values[i])["id"].value_or(std::string_view{}) == current) return i;
    }
    return 0;
}

std::string RuleChoiceLabel(const ItemDef& item, int i) {
    const PauseRuleEnv env;
    const expr::RuleView row(*ItemRules(item), env, &PauseRules());
    if (const auto values = GenerateRuleValues(row, env))
        return i >= 0 && i < int(values->entries.size()) ? values->entries[size_t(i)].label : std::string{};
    const auto values = row.Choices();
    if (i < 0 || i >= int(values.size())) return {};
    const auto& value = *values[i];
    const auto id = value["id"].value_or(std::string_view{});
    const char* setting = VariantCvar(item, row);
    return row.Label(id, &value, {}, setting ? setting : "").value_or(std::string(id));
}

void RuleChoiceWrite(const ItemDef& item, int i) {
    const PauseRuleEnv env;
    const expr::RuleView row(*ItemRules(item), env, &PauseRules());
    if (const auto values = GenerateRuleValues(row, env)) {
        if (i < 0 || i >= int(values->entries.size()) || !values->setting) return;
        const auto& id = values->entries[size_t(i)].id;
        const auto setting = std::string_view(values->setting);
        const bool resize = setting == "resolution" || setting == "fullscreen_resolution";
        if (!resize || CvarGet(values->setting) != id)
            CvarSet(values->setting, id.c_str());
        return;
    }
    const auto values = row.Choices();
    if (i < 0 || i >= int(values.size())) return;
    const auto& value = *values[i];
    const auto id = value["id"].value_or(std::string{});
    if (const auto* writes = value["writes"].as_array()) {
        for (const auto& entry : *writes) {
            const auto* write = entry.as_table();
            if (!write) continue;
            const auto setting = (*write)["setting"].value_or(std::string{});
            auto next = (*write)["value"].value_or(std::string{});
            if (next == "$id") next = id;
            if (CvarGet(setting.c_str()) != next) CvarSet(setting.c_str(), next.c_str());
        }
    } else if (const char* setting = VariantCvar(item, row)) {
#if defined(LARECOMP_HAS_GPU)
        // Returning to the running renderer keeps the underlying driver alias
        // (for example "any"), so the restart prompt sees no change.
        if (std::string_view(setting) == kRendererCvar) {
            const std::string live = larecomp::cvar::GetFlagByName(kRendererCvar);
            const auto pick = mcla::gpu::PickOfDriver(live, kRendererOnWindows);
            if (mcla::gpu::DriverOfPick(pick) == id) {
                CvarSet(setting, live.c_str());
                return;
            }
        }
#endif
        CvarSet(setting, id.c_str());
    }
}

bool HasRuleNumberLabels(const ItemDef& item) {
    const auto* row = ItemRules(item);
    return item.kind == ItemKind::kDblCycle && row && row->base.data->contains("value");
}

std::string RuleNumberLabel(const ItemDef& item, double number) {
    const auto* rules = ItemRules(item);
#if defined(LARECOMP_HAS_GPU)
    const PauseRuleEnv env(!mcla::gpu::gpu_class::Known());
#else
    const PauseRuleEnv env;
#endif
    const expr::RuleView row(*rules, env, &PauseRules());
    namespace gc = mcla::gpu::gpu_class;
    const std::string_view key(item.key);
    double id = number;
    for (int i = 0; i < gc::kScaleCount; ++i) {
        const auto scale = gc::Scale(i);
        const auto& info = gc::ScaleOf(scale);
        if (info.cvar != item.cvar) continue;
        id = gc::ScaleFor(scale, false, gc::GpuClass::kLow, number);
        if (gc::IsAutoValue(scale, number)) {
            const auto automatic = expr::Evaluate(expr::Parse("auto_of('" + std::string(info.cvar) + "')"), env);
            if (automatic.kind == expr::Value::Kind::kNumber) id = automatic.n;
        }
        break;
    }
    if (key == "PM_RxGpuFrameGenFps") {
        if (number < 0.0) id = -1.0;
        else if (number > 0.0) id = 30.0;  // all positive caps share the same label template
    }
    if (key == "PM_RxHudWidth") id = std::clamp(number, 0.0, 100.0);
    if (id <= 0 && (key == "PM_RxGpuHdrWhite" || key == "PM_RxGpuHdrPeak")) {
        const char* fact = key == "PM_RxGpuHdrWhite" ? "features.hdr.auto_paper_white_nits" : "features.hdr.auto_peak_nits";
        const auto automatic = expr::Evaluate(expr::Parse(fact), env);
        if (automatic.kind == expr::Value::Kind::kNumber) id = automatic.n;
    }
    if ((key == "PM_RxAiDiff" || key == "PM_RxRbNitroStart") && number < 0.0) id = -1.0;
    if (key == "PM_RxTrigThrottle" || key == "PM_RxTrigBrake") id = std::round(number);
    const auto* override = row.Field("format").as_string();
    const char* format = override ? override->get().c_str() : item.dfmt;
    const std::string fallback = item.zero_label && id == 0.0 ? item.zero_label : expr::FormatNumber(id, format);
    return row.Label(expr::NumberId(id), nullptr, fallback, item.cvar, number, item.dfmt).value_or(fallback);
}

int MenuItemCount(const MenuDef& md) {
    const uint8_t mask = RunningRendererMask();
    int n = 0;
    for (int i = 0; i < MenuRawCount(md); ++i)
        if (MenuRawAt(md, i).renderers & mask) ++n;
    return n;
}

const ItemDef& MenuItemAt(const MenuDef& md, int i) {
    const uint8_t mask = RunningRendererMask();
    const int raw = MenuRawCount(md);
    for (int r = 0; r < raw; ++r) {
        const ItemDef& it = MenuRawAt(md, r);
        if (!(it.renderers & mask)) continue;
        if (i-- == 0) return it;
    }
    return MenuRawAt(md, 0);
}

const MenuDef kMenus[] = {
    // The buttons of the SETTINGS tab: the recomp's own fixes first, then the
    // hubs, with the controller page after GAMEPLAY.
    {"PM_RxTabRecomp", "RECOMP SETTINGS", "Fixes and options added by the recomp.",
     "RxRecompMenu",
     kRecompItems, int(sizeof(kRecompItems) / sizeof(kRecompItems[0]))},
    {"PM_RxTabGfx", "GRAPHICS", "Display, upscaling, picture and detail.",
     "RxGfxMenu",
     kGfxHubItems, int(sizeof(kGfxHubItems) / sizeof(kGfxHubItems[0]))},
    {"PM_RxTabGame", "GAMEPLAY", "Camera, map, time of day and cars.",
     "RxGameMenu",
     kGameHubItems, int(sizeof(kGameHubItems) / sizeof(kGameHubItems[0]))},
    // The line is 40 characters, as long as the longest the form takes (the QUIT line is 41).
    {"PM_RxTabCtrl", "CONTROLLER", "Vibration, triggers, sticks and buttons.",
     "RxCtrlMenu",
     kControllerItems, int(sizeof(kControllerItems) / sizeof(kControllerItems[0]))},
    {"PM_RxTabIface", "INTERFACE", "Menu colors, HUD width and language.",
     "RxIfaceMenu",
     kIfaceHubItems, int(sizeof(kIfaceHubItems) / sizeof(kIfaceHubItems[0]))},
    {"PM_RxTabExtras", "EXTRAS", "Extra things to watch and do.",
     "RxExtrasMenu",
     kExtrasHubItems, int(sizeof(kExtrasHubItems) / sizeof(kExtrasHubItems[0]))},
    // Reached through a hub.
    // The output rows (kVideoItems), then (MenuDef::extra: they follow the PC and the settings) the vendor's latency reducer, the upscaler's rows with the denoiser row where this PC has one, and
    // frame generation's; on XENIA RENDER SCALE is the only upscaling row. The page whose rows can change while the game runs (UpscalingMenuRelayout).
    {"PM_RxTabVideo", "DISPLAY", "Window, upscaling, frame rate, latency.",
     "RxVideoMenu",
     kVideoItems, int(sizeof(kVideoItems) / sizeof(kVideoItems[0])),
     DisplayPageRows, false, UpscalingMenuRelayout},
    {"PM_RxTabImageQuality", "IMAGE QUALITY", "Anti-aliasing, filtering, sharp UI.",
     "RxImageQualityMenu",
     kImageQualityItems, int(sizeof(kImageQualityItems) / sizeof(kImageQualityItems[0]))},
    {"PM_RxTabLighting", "LIGHTING AND SHADOWS", "Shadows, occlusion, bounce, headlights.",
     "RxLightingMenu",
     kLightItems, int(sizeof(kLightItems) / sizeof(kLightItems[0]))},
#if defined(LARECOMP_HAS_GPU)
    {"PM_RxTabReflections", "REFLECTIONS", "Road, car, city, mirrors and surfaces.",
     "RxReflectionsMenu",
     kReflectionItems, int(sizeof(kReflectionItems) / sizeof(kReflectionItems[0]))},
#endif
    {"PM_RxTabWorldDetail", "WORLD DETAIL", "Traffic, props and level of detail.",
     "RxWorldDetailMenu",
     kWorldDetailItems, int(sizeof(kWorldDetailItems) / sizeof(kWorldDetailItems[0]))},
#if defined(LARECOMP_HAS_GPU)
    // The paused world behind the menu shows the highlights too (the UI is a layer in the pause
    // view as well, gpu/frame_layers.h), so the HIGHLIGHTS row's change is seen at once. This line
    // says so: it is the only help text a menu has.
    // CHECK: the 40 characters fit under the list (the CAMERA line is as long), and the pause menu
    // shows the highlights on every screen (a screen whose UI cannot be separated shows the SDR
    // picture; the frame layers' log says which).
    {"PM_RxTabGpuHdr", "HDR", "HDR levels. The paused world shows them.",
     "RxGpuHdrMenu",
     kGpuHdrItems, int(sizeof(kGpuHdrItems) / sizeof(kGpuHdrItems[0]))},
#endif
    {"PM_RxTabCam", "CAMERA", "Chase camera, FOV, rear view, mouse look.",
     "RxCamMenu",
     kCamHubItems, int(sizeof(kCamHubItems) / sizeof(kCamHubItems[0]))},
    {"PM_RxTabCamChase", "CHASE CAMERA", "Smoothing and shake of the chase camera.",
     "RxCamChaseMenu",
     kCamChaseItems, int(sizeof(kCamChaseItems) / sizeof(kCamChaseItems[0]))},
    {"PM_RxTabCamFov", "FOV", "Field of view, cockpit and chase.",
     "RxCamFovMenu",
     kCamFovItems, int(sizeof(kCamFovItems) / sizeof(kCamFovItems[0]))},
    {"PM_RxTabCamRear", "REAR VIEW", "The rear view mirror and the cars in it.",
     "RxCamRearMenu",
     kCamRearItems, int(sizeof(kCamRearItems) / sizeof(kCamRearItems[0]))},
    // The free-look rows come from camera_look.cpp, next to the code that reads
    // them.
    {"PM_RxTabCamLook", "LOOK AROUND", "Looking around with the mouse.",
     "RxCamLookMenu",
     nullptr, 0,
     CameraLookMenuItems},
    {"PM_RxTabDiff", "DIFFICULTY", "AI difficulty and rubberband.",
     "RxDiffMenu",
     kDiffItems, int(sizeof(kDiffItems) / sizeof(kDiffItems[0]))},
    {"PM_RxTabMap", "MAP MOUSE", "Mouse control of the full map.",
     "RxMapMouseMenu",
     kMapMouseItems, int(sizeof(kMapMouseItems) / sizeof(kMapMouseItems[0]))},
    {"PM_RxTabTod", "TIME OF DAY", "Time of day, clock and weather.",
     "RxTodMenu",
     kTodItems, int(sizeof(kTodItems) / sizeof(kTodItems[0]))},
    {"PM_RxTabCarbon", "CARBON FIBER", "Carbon fiber parts of this car.",
     "RxCarbonMenu",
     kCarbonItems, int(sizeof(kCarbonItems) / sizeof(kCarbonItems[0]))},
    {"PM_RxTabLang", "LANGUAGES", "Language of the game's text.",
     "RxLangMenu",
     kLangItems, int(sizeof(kLangItems) / sizeof(kLangItems[0]))},
    {"PM_RxTabCut", "CUTSCENES", "Watch the story cutscenes again.",
     "RxCutMenu",
     kCutsceneItems, int(sizeof(kCutsceneItems) / sizeof(kCutsceneItems[0]))},
    {"PM_RxTabUi", "UI", "Colors of the menus, speed units and HUD width.",
     "RxUiMenu",
     kUiItems, int(sizeof(kUiItems) / sizeof(kUiItems[0]))},
    // The hidden DEBUG menu (debug_menu_pages.cpp): a hub button before QUIT and the eight pages it opens (seven while the XENIA renderer runs).
    // All of it is MenuDef::debug_only: with the menu locked these have no rows, no button and no log
    // line, and the SETTINGS tab is what it was. The rows are the cvars of the DEBUG verdict of
    // SETTINGS-REVIEW.md, session only (a debug cvar's change is never saved, config_save.cpp).
    // CHECK: with --debug_menu the SETTINGS tab has eight buttons and the panel holds them (the CONTROLLER
    // note above counted seven); every page up to 14 rows scrolls like the 12 and 14 row ones do.
    {"PM_RxDbgTab", "DEBUG", "Developer settings. They are not saved.",
     "RxDbgMenu",
     nullptr, 0, mcla_menu::DebugHubRows, false, nullptr, true},
    {"PM_RxDbgTabFixes", "FIXES", "Turn a fix off to look for a bug.",
     "RxDbgFixesMenu",
     nullptr, 0, mcla_menu::DebugFixesRows, false, nullptr, true},
#if defined(LARECOMP_HAS_GPU)
    // The second half of the fixes: the Plume renderer's picture (the first page was over the row cap). Its hub row is NativeOnly like the page's rows.
    {"PM_RxDbgTabFixes2", "PICTURE FIXES", "Picture fixes to turn off, one by one.",
     "RxDbgFixes2Menu",
     nullptr, 0, mcla_menu::DebugPictureFixesRows, false, nullptr, true},
    {"PM_RxDbgTabGpuFixes", "RENDERER FIXES", "Renderer switches for comparing fixes and optimizations.",
     "RxDbgGpuFixesMenu",
     nullptr, 0, mcla_menu::DebugRendererFixesRows, false, nullptr, true},
#endif
    {"PM_RxDbgTabPerf", "PERFORMANCE", "Pacing and speed switches for testing.",
     "RxDbgPerfMenu",
     nullptr, 0, mcla_menu::DebugPerformanceRows, false, nullptr, true},
    {"PM_RxDbgTabRend", "RENDERING", "Renderer views and logs.",
     "RxDbgRendMenu",
     nullptr, 0, mcla_menu::DebugRenderingRows, false, nullptr, true},
    {"PM_RxDbgTabCam", "CAMERA", "Free camera and camera tests.",
     "RxDbgCamMenu",
     nullptr, 0, mcla_menu::DebugCameraRows, false, nullptr, true},
    {"PM_RxDbgTabTools", "TOOLS", "Texture tools, panels and test items.",
     "RxDbgToolsMenu",
     nullptr, 0, mcla_menu::DebugToolsRows, false, nullptr, true},
    {"PM_RxDbgTabLog", "LOGGING", "Extra lines in the log file.",
     "RxDbgLogMenu",
     nullptr, 0, mcla_menu::DebugLoggingRows, false, nullptr, true},
    {"PM_RxDbgTabInfo", "INFO", "Changed settings and launch arguments.",
     "RxDbgInfoMenu",
     nullptr, 0, mcla_menu::DebugInfoRows, false, nullptr, true},
    // Last, so its button ends the SETTINGS tab.
    {"PM_RxQuit", "QUIT", "Close the game and return to the desktop?",
     "RxQuitMenu",
     kQuitItems, int(sizeof(kQuitItems) / sizeof(kQuitItems[0]))},
    // Opened by RestartPromptTick, never from the SETTINGS tab.
    {"PM_RxRestartAsk", "RESTART", "Restart to apply the changed settings?",
     "RxRestartAskMenu",
     kRestartAskItems, int(sizeof(kRestartAskItems) / sizeof(kRestartAskItems[0])),
     nullptr, true},
};
constexpr int kNumMenus = int(sizeof(kMenus) / sizeof(kMenus[0]));

// Per-menu runtime state (guest addresses).
std::atomic<uint32_t> g_menu_btn_key[kNumMenus] = {};  // guest key string for IsButtonClicked
std::atomic<uint32_t> g_menu_form[kNumMenus]    = {};  // its form, built on first open

// The menu `menu_key` names, -1 if none.
int MenuByKey(const char* menu_key) {
    if (!menu_key) return -1;
    for (int m = 0; m < kNumMenus; ++m)
        if (std::strcmp(kMenus[m].menu_key, menu_key) == 0) return m;
    return -1;
}

// True when some Submenu row opens menu m: it then lives inside that menu and
// has no button of its own on the SETTINGS tab.
bool MenuIsNested(int m) {
    if (kMenus[m].opened_by_code) return true;
    // A page of the locked DEBUG menu has no row that opens it (its hub is empty), but it must not turn
    // into a button of its own.
    if (MenuLocked(kMenus[m])) return true;
    for (int p = 0; p < kNumMenus; ++p)
        for (int i = 0; i < MenuRawCount(kMenus[p]); ++i) {
            const ItemDef& it = MenuRawAt(kMenus[p], i);
            if (it.kind == ItemKind::kSubmenu && MenuByKey(it.cvar) == m) return true;
        }
    return false;
}

int MenuOfForm(uint32_t form) {
    if (!form) return -1;
    for (int m = 0; m < kNumMenus; ++m)
        if (g_menu_form[m].load(std::memory_order_relaxed) == form) return m;
    return -1;
}

int MenuOfList(uint32_t list) {
    for (int m = 0; m < kNumMenus; ++m) {
        const uint32_t form = g_menu_form[m].load(std::memory_order_relaxed);
        if (form && larecomp::ui::FormList(form) == list) return m;
    }
    return -1;
}

// The menu on top of the pause screen -- the one the pad goes to -- or -1.
int TopMenu() { return MenuOfForm(larecomp::ui::TopForm()); }

// Pause-menu probe: armed only while the pause menu is actually being shown.
std::atomic<bool> g_pm_probe_armed{false};
uint32_t g_pm_seen[64] = {};
int g_pm_seen_count = 0;
std::atomic<bool> g_menus_created{false};
// False means a cvar moved since the last write, i.e. larecomp.toml is stale.
// Starts clean: entering and leaving a submenu without touching anything
// must not rewrite the file.
std::atomic<bool> g_settings_saved{true};

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

// Rows with no value list at all: they render as a plain label and a click
// runs them rather than stepping anything. A color button is one too: its
// value is drawn as a swatch beside the label (ui_palette.cpp), not as text.
// So is a submenu row, like the rows of OPTIONS.
bool ItemIsLabelOnly(const ItemDef& it) {
    return it.kind == ItemKind::kAction || it.kind == ItemKind::kColor ||
           it.kind == ItemKind::kSubmenu;
}

// The setting a row reads and writes (a checkbox) and the "restart now?" prompt looks up: its own
// `cvar`, or what variant setting says for the renderer this launch runs (VSYNC, MSAA and RENDER SCALE
// are one row each, mapped to the running renderer's own setting). Null for a row that has none.
const char* ItemCvar(const ItemDef& it) {
    const auto* rules = ItemRules(it);
    if (!rules || rules->variants.empty()) return it.cvar;
    const PauseRuleEnv env;
    // Only the Xenia mode row owns a restart prompt. On native the
    // UPSCALING selector owns the prompt when the DLSS mode waits.
    if (!it.cvar && mcla_menu::NativeRendererRuns()) return nullptr;
    return VariantCvar(it, expr::RuleView(*rules, env, &PauseRules()));
}

// A row without a matching disabled state is enabled.
bool ItemEnabled(const ItemDef& it) {
    if (const auto* state = ItemState(it)) return (*state)["enabled"].value_or(true);
    return true;
}

std::string DblLabel(const ItemDef& it, double value) {
    return RuleNumberLabel(it, value);
}

// A kDblCycle row whose values are labeled by the value rules.
bool ItemLabelsValues(const ItemDef& it) {
    return HasRuleNumberLabels(it);
}

// The values a kDblCycle row steps through: its table, and for a row that
// labels its values, also the cvar's own value, in order, when it labels as
// none of them (the value rules).
std::vector<double> DblValues(const ItemDef& it) {
    std::vector<double> values(it.dvals, it.dvals + it.nvals);
    if (!ItemLabelsValues(it)) return values;
    const double current = CvarGetDouble(it.cvar);
    const std::string label = DblLabel(it, current);
    for (const double v : values)
        if (DblLabel(it, v) == label) return values;
    values.insert(std::upper_bound(values.begin(), values.end(), current), current);
    return values;
}

// Rows whose entry the cvar sits on can differ from the one stepped to, because the list changes
// under the step (an entry for a value the row does not list is dropped once the row leaves it).
bool ItemListMoves(const ItemDef& it) { return ItemLabelsValues(it) || HasRuleChoices(it); }

int ItemValueCount(const ItemDef& it) {
    switch (it.kind) {
    case ItemKind::kBool:
    case ItemKind::kCarbonBit: return 2;
    case ItemKind::kDblCycle:  return ItemLabelsValues(it) ? int(DblValues(it).size()) : it.nvals;
    case ItemKind::kStrCycle:  return HasRuleChoices(it) ? RuleChoiceCount(it) : it.nvals;
    case ItemKind::kColor:     return it.nvals;
    case ItemKind::kAction:
    case ItemKind::kSubmenu:   return 0;
    }
    return 0;
}

std::string ItemValueLabel(const ItemDef& it, int i) {
    switch (it.kind) {
    case ItemKind::kBool:      return i ? "ON" : "OFF";
    case ItemKind::kCarbonBit: return i ? "CARBON" : "PAINT";
    case ItemKind::kStrCycle:
        if (HasRuleChoices(it)) return RuleChoiceLabel(it, i);
        if (const auto* rules = ItemRules(it)) {
            const PauseRuleEnv env;
            return expr::RuleView(*rules, env, &PauseRules()).Label(it.svals[i], nullptr, it.slabels[i], it.cvar)
                .value_or(it.slabels[i]);
        }
        return it.slabels[i];
    case ItemKind::kDblCycle: {
        if (ItemLabelsValues(it)) {
            const std::vector<double> values = DblValues(it);
            return (i >= 0 && i < int(values.size())) ? DblLabel(it, values[i])
                                                      : std::string();
        }
        if (it.zero_label && it.dvals[i] == 0.0) return it.zero_label;
        char buf[48];
        std::snprintf(buf, sizeof(buf), it.dfmt, it.dvals[i]);
        return buf;
    }
    case ItemKind::kAction:
    case ItemKind::kColor:
    case ItemKind::kSubmenu:   break;
    }
    return "";
}

// Which value the live cvar (or, for carbon, the car itself) is currently on.
int ItemValueIndex(const ItemDef& it) {
    switch (it.kind) {
    case ItemKind::kBool: {
        bool on = CvarGetBool(ItemCvar(it));
        if (it.inverted) on = !on;
        return on ? 1 : 0;
    }
    case ItemKind::kCarbonBit:
        return (CarbonHaveCar() && CarbonHasGroup(uint8_t(it.nvals))) ? 1 : 0;
    case ItemKind::kStrCycle:
        if (HasRuleChoices(it)) return RuleChoiceIndex(it);
        if (const auto* rules = ItemRules(it);
            rules && (rules->base.decode.expr || !rules->base.decode.cases.empty())) {
            const PauseRuleEnv env;
            return FindStringIndex(expr::ValueText(expr::Decode(rules->base.decode, env)), it.svals, it.nvals);
        }
        return FindStringIndex(CvarGet(it.cvar), it.svals, it.nvals);
    case ItemKind::kColor:
        return FindStringIndex(CvarGet(it.cvar), it.svals, it.nvals);
    case ItemKind::kDblCycle: {
        if (!ItemLabelsValues(it))
            return FindClosestIndex(CvarGetDouble(it.cvar), it.dvals, it.nvals);
        // The entry that labels as the cvar's value; DblValues lists one.
        const std::vector<double> values = DblValues(it);
        const std::string label = DblLabel(it, CvarGetDouble(it.cvar));
        for (int i = 0; i < int(values.size()); ++i)
            if (DblLabel(it, values[i]) == label) return i;
        return 0;
    }
    case ItemKind::kAction:
    case ItemKind::kSubmenu:   break;
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
        CvarSet(ItemCvar(it), on ? "true" : "false");
        g_settings_saved.store(false, std::memory_order_relaxed);
        break;
    }
    case ItemKind::kStrCycle:
        if (HasRuleChoices(it)) {
            RuleChoiceWrite(it, i);
            g_settings_saved.store(false, std::memory_order_relaxed);
            break;
        }
        CvarSet(it.cvar, it.svals[i]);
        g_settings_saved.store(false, std::memory_order_relaxed);
        break;
    case ItemKind::kColor:
        CvarSet(it.cvar, it.svals[i]);
        g_settings_saved.store(false, std::memory_order_relaxed);
        break;
    case ItemKind::kDblCycle:
        CvarSetDouble(it.cvar, ItemLabelsValues(it) ? DblValues(it)[size_t(i)] : it.dvals[i]);
        g_settings_saved.store(false, std::memory_order_relaxed);
        break;
    case ItemKind::kCarbonBit:
        // Goes straight into the car's customization data, which is its own
        // source of truth — only touch it when it actually disagrees.
        if (CarbonHaveCar() && (CarbonHasGroup(uint8_t(it.nvals)) ? 1 : 0) != i)
            CarbonToggleGroup(uint8_t(it.nvals));
        break;
    case ItemKind::kAction:
    case ItemKind::kSubmenu:
        break;
    }
}

// Page width. The pause movie sizes a page to its widest label (suffix
// included) plus its widest value, over every row of the page and every value
// a row can step to (the rows below the ones in view too), not row by row; past
// about 750 of the movie's units the page is cut at a straight edge on the
// right, the button prompts and the scroll bar first, at any screen size. A
// page comes to about 245 units plus 11 a character of that label and value
// (capitals of the menu's font: I and digits narrower, M and W wider).
// A page must stay at or under 41 characters of that pair (24 and 17, say), about 700 units, in every
// state of every row (the Windows and the other platforms' suffixes, both renderers, sizes such as
// "2560X1440"). A label or a value that is longer than its page's widest needs counting against that
// before it goes in, in each state a suffix can be in; each page counts its own, here and where it is
// built:
// Display has up to 17 rows. Short status labels keep the page within its 41-column limit.
// Image Quality, Lighting and Shadows, Reflections and World Detail use the same limit.
// The CONTROLLER page (kControllerItems, 8 rows, no suffixes): labels at most 21 ("XBOX TRIGGER
// STRENGTH", "RIGHT STICK DEAD ZONE"), values at most 11 ("PLAYSTATION"): 32. RECOMP SETTINGS has two
// rows now: 33 ("ORIGINAL INTRO SPEED (NEXT START)") and 3.
//
// The row's own text. Carbon rows say so when there is no car to edit, since
// their value list would otherwise claim a state the car does not have.
std::string ItemRowLabel(const ItemDef& it) {
    if (it.kind == ItemKind::kAction) {
        switch (ActionId(it.nvals)) {
        case kActionPlayCutscene:
            // The row is the only feedback there is: once the menu closes the
            // scene owns the screen, and if it refuses to start the log is the
            // only other place that says so.
            return CutsceneReplayBusy() ? "PLAYING..." : "PLAY CUTSCENE";
        case kActionQuitYes: return "YES";
        case kActionQuitNo:  return "NO";
        case kActionDumpTextures:   return "DUMP SCENE TEXTURES";
        case kActionReloadTextures: return "RELOAD TEXTURE MODS";
        case kActionRestart:        return "RESTART GAME";
        case kActionRestartLater:   return "NOT NOW";
        case kActionRestartNow:     return "RESTART NOW";
        case kActionDebugShowChanged:
        case kActionDebugCopyArgs:
        case kActionDebugReset:
        case kActionDebugOpenSettings:
        case kActionDebugTelemetry:
            return mcla_menu::DebugActionLabel(ActionId(it.nvals));
        }
        return "";
    }

    const PauseRuleEnv env;
    std::string label = it.prefix;
    std::string suffix = it.suffix;
#if defined(LARECOMP_HAS_GPU)
    if (std::string_view(it.key) == "PM_RxGpuUpsc")
        mcla::gpu::NoteUpscalerInterest("the UPSCALING row is on screen");
#endif
    if (const auto* rules = ItemRules(it)) {
        const expr::RuleView row(*rules, env, &PauseRules());
        if (const auto text = row.Field("label").value<std::string_view>()) label = *text;
        if (const auto* state = row.State()) {
            if (const auto text = (*state)["label"].value<std::string_view>())
                label = expr::ExpandTemplate(*text, env).value_or(label);
            if (const auto text = (*state)["suffix"].value<std::string_view>())
                suffix = expr::ExpandTemplate(*text, env).value_or(suffix);
        }
    }
    while (!label.empty() && (label.back() == ' ' || label.back() == ':'))
        label.pop_back();

    if (it.kind == ItemKind::kCarbonBit && !CarbonHaveCar())
        return label + " (NO CAR)";
    return label + suffix;
}

// dir is +1 for A / right and -1 for left. Each menu's list is a real
// mcListViewFixed whose select index at +184 tracks the highlight, so a value
// can be stepped without the item having to be re-entered.
void ApplyItem(const ItemDef& it, int dir) {
    switch (it.kind) {
    case ItemKind::kBool:
        CvarSet(ItemCvar(it), CvarGetBool(ItemCvar(it)) ? "false" : "true");
        g_settings_saved.store(false, std::memory_order_relaxed);
        break;
    case ItemKind::kStrCycle: {
        // Through the same choice list the row displays, including generated choices.
        const int n = ItemValueCount(it);
        if (n > 0) ItemSetValueIndex(it, ((ItemValueIndex(it) + dir) % n + n) % n);
        break;
    }
    case ItemKind::kDblCycle: {
        if (ItemLabelsValues(it)) {
            const std::vector<double> values = DblValues(it);
            const int n = int(values.size());
            CvarSetDouble(it.cvar, values[size_t((ItemValueIndex(it) + dir + n) % n)]);
            g_settings_saved.store(false, std::memory_order_relaxed);
            break;
        }
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
    case ItemKind::kAction:
        switch (ActionId(it.nvals)) {
        case kActionPlayCutscene:
            // Close our menus first, the way B would, so the pause screen is
            // all the gallery has left to take down; it drives the rest from
            // the frame tick.
            PauseMenuLeaveSubmenu();
            CutsceneRequestPlay();
            break;
        case kActionQuitYes: {
            // Same as leaving a menu: settings that moved are written first.
            if (!g_settings_saved.exchange(true, std::memory_order_relaxed)) SaveSettingsNow();
            // 0.10 closed the window on the UI thread (its close event ran the close work); 0.11 asks the app to shut down:
            // LarecompApp::OnStop does the close work (saves, devices, the renderer) before the title ends.
            if (auto* ui = larecomp::sdk::UI(); ui && ui->HasWindow()) {
                MC_INFO("[pause-menu] quit requested from the QUIT menu");
                ui->PostTask([] {
                    MclaWaitForSaves();
                    rex::app::RequestShutdown(0);
                });
            } else {
                MC_INFO("[pause-menu] quit requested, no window: shutting down");
                MclaWaitForSaves();
                rex::app::RequestShutdown(0);
            }
            break;
        }
        case kActionRestart:
        case kActionRestartNow:
            PauseMenuRestartGame();
            break;
        case kActionRestartLater:
        case kActionQuitNo:
            // Closes the QUIT form the way B does; the select sound is the A's.
            larecomp::ui::CloseForm(larecomp::ui::TopForm());
            break;
        case kActionDumpTextures:
            // Every texture held, not the next frame's: that one is this menu.
#if defined(LARECOMP_HAS_GPU)
            mcla::gpu::RequestTextureDumpAll();
#endif
            break;
        case kActionReloadTextures:
#if defined(LARECOMP_HAS_GPU)
            mcla::gpu::RequestTextureReload();
#endif
            break;
        case kActionDebugShowChanged:
        case kActionDebugCopyArgs:
        case kActionDebugReset:
        case kActionDebugOpenSettings:
        case kActionDebugTelemetry:
            // The DEBUG menu's own actions (debug_menu_pages.cpp). They write through the cvar
            // registry, so the page's rows follow once the menu syncs them (MenuFormAccept).
            mcla_menu::DebugActionRun(ActionId(it.nvals));
            break;
        }
        break;
    case ItemKind::kColor:
    case ItemKind::kSubmenu:
        // Both need the form they are on; MenuFormAccept handles them.
        break;
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

// Sizes the tables below, nothing else: the form's own row list is made with the page's row count
// (ui_forms.cpp CreateListForm), and a long page scrolls. LIGHTING AND SHADOWS is the longest page at 19 rows
// (kLightItems); a page over the cap is refused whole (BuildNativeRows), so there is room. The cap is the size of these tables, not
// the screen's: nothing past about 14 rows has been looked at in the game ("CHECK: every page up to 14 rows scrolls" above), and a page that long is no
// way to find a row, so a page that outgrows it is split into two pages, never given a higher cap. tools/check_menu_rows.py counts every page of kMenus
// against this number and fails on one over it (a debug page has no other check: it would just not open).
constexpr int kMaxRowsPerMenu = 24;

struct NativeRows {
    uint32_t row[kMaxRowsPerMenu] = {};     // guest row objects
    uint32_t name[kMaxRowsPerMenu] = {};    // guest key string, kept for relabels
    int last_index[kMaxRowsPerMenu] = {};   // last select we pushed to the cvar
    std::string last_label[kMaxRowsPerMenu];
    int count = 0;
};
// A disabled row whose select moved anyway was reported (PollNativeRows; once).
bool g_disabled_step_reported = false;
NativeRows (&g_rows)[kNumMenus] = *new NativeRows[1][kNumMenus]();

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

// The row's enabled state  into the guest object: the
// byte the game's own rows read (kRowValueEnabled), and for a checkbox also
// the word its render reads (kRowEnabled). Label-only rows have none.
void ApplyRowEnabled(NativeRows& nr, int i, const ItemDef& it) {
    if (!nr.row[i] || ItemIsLabelOnly(it)) return;
    const bool enabled = ItemEnabled(it);
    WriteGuestU8(nr.row[i] + kRowValueEnabled, enabled ? 1 : 0);
    if (ItemIsToggle(it)) WriteGuestBE32(nr.row[i] + kRowEnabled, enabled ? 1 : 0);
}

bool BuildNativeRows(int m) {
    NativeRows& nr = g_rows[m];
    if (nr.count > 0) return true;

    const MenuDef& md = kMenus[m];
    if (MenuItemCount(md) > kMaxRowsPerMenu) {
        MC_WARN("[pause-menu] '{}' has {} items, over the {} row cap",
                md.label, MenuItemCount(md), kMaxRowsPerMenu);
        return false;
    }

    uint8_t* base = GetMembase();
    if (!base) return false;

    for (int i = 0; i < MenuItemCount(md); ++i) {
        const ItemDef& it = MenuItemAt(md, i);

        // The ctor resolves the label through the string table once and
        // caches it at +48, so the text has to be registered first.
        std::string label = ItemRowLabel(it);
        EnsureStringTableText(it.key, label.c_str());

        uint32_t name = AllocGuestString(it.key);
        uint32_t row  = CallGuestFn1(kGuestMallocFn, kRowObjectSize);
        if (!name || !row) return false;
        std::memset(base + row, 0, kRowObjectSize);

        if (ItemIsLabelOnly(it)) {
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
        if (!ItemIsLabelOnly(it) && !ItemIsToggle(it))
            CentreRowWindow(nr, i, it, ItemValueIndex(it));
        ApplyRowEnabled(nr, i, it);
    }

    nr.count = MenuItemCount(md);
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

// Rows own their value while the menu is open, so this only runs where the
// value can have moved behind their back: entering the menu, and after A.
void SyncNativeRowsFromCvars(int m) {
    NativeRows& nr = g_rows[m];
    const MenuDef& md = kMenus[m];

    // Drained here rather than in the cvar callback: that one runs with the
    // registry lock held, and the reload calls back into guest code. This
    // point is on the guest thread with nothing of ours held, and it also
    // catches a `language` set from the console, not just from the row.
    const int lang = g_pending_language.exchange(-1, std::memory_order_relaxed);
    if (lang >= 0) ApplyLanguageNow(lang);

    for (int i = 0; i < nr.count && i < MenuItemCount(md); ++i) {
        const ItemDef& it = MenuItemAt(md, i);
        RelabelRow(nr, i, it);
        ApplyRowEnabled(nr, i, it);

        if (ItemIsLabelOnly(it)) continue;

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
    const int count = std::min(nr.count, MenuItemCount(md));

    // 1. The steps the player just took, written through to the cvars.
    for (int i = 0; i < count; ++i) {
        const ItemDef& it = MenuItemAt(md, i);

        // An action row's label tracks whether the scene is playing, carbon
        // tracks whether there is a car to edit -- both change without the row
        // being touched.
        if (ItemIsLabelOnly(it) || it.kind == ItemKind::kCarbonBit)
            RelabelRow(nr, i, it);
        if (ItemIsLabelOnly(it)) continue;

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
            if (!ItemEnabled(it)) {
                // The game's row refuses a step while disabled (sub_826316E0);
                // one that moved anyway is put back, not written through.
                if (!g_disabled_step_reported) {
                    g_disabled_step_reported = true;
                    MC_WARN("[pause-menu] '{}' is disabled but its select moved to {}; put "
                            "back (reported once)",
                            nr.last_label[i], slot);
                }
                CentreRowWindow(nr, i, it, nr.last_index[i]);
                continue;
            }
            const int dir = slot < 1 ? -1 : +1;
            const int idx = ((nr.last_index[i] + dir) % n + n) % n;
            ItemSetValueIndex(it, idx);
            // A row that lists the cvar's own value drops that entry once the
            // value leaves it, and a row with a list of its own can have another one now
            // (the choice above its mode row), so where it now is is read again.
            CentreRowWindow(nr, i, it, ItemListMoves(it) ? ItemValueIndex(it) : idx);
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

    // 2. What follows from the cvars as they now are, for the rows that
    //    depend on other settings: a step above, or one in another menu, can
    //    grey a row out, change its suffix or what it shows. After all the
    //    steps, so a row above the one that moved is not a render behind.
    for (int i = 0; i < count; ++i) {
        const ItemDef& it = MenuItemAt(md, i);
        if (!ItemRules(it)) continue;
        RelabelRow(nr, i, it);
        ApplyRowEnabled(nr, i, it);
        if (!ItemIsLabelOnly(it) && !ItemIsToggle(it))
            CentreRowWindow(nr, i, it, ItemValueIndex(it));
    }
}

// ── Color buttons ──────────────────────────────────────────────────────
//
// The garage's PickColor on a menu row (garage/paint.sc.xml, Color_A_Closed /
// PickColorA): closed, the row shows its color beside the label; A opens the
// strip; left/right walk it with the theme following live; A keeps the color
// and B puts the old one back. Up/down are held while the strip is open, the
// way PickColorA blocks them. ui_palette.cpp draws it, reading the state
// through PauseMenuColorButtons. Guest thread.

// Sound names the game's own menus play (.rdata strings, so guest pointers).
constexpr uint32_t kSoundBack     = 0x8208BA28;  // "UI_MENU_BACK"
constexpr uint32_t kSoundNavLeft  = 0x8208F1C0;  // "UI_MENU_NAV_LEFT"
constexpr uint32_t kSoundNavRight = 0x8208F1D4;  // "UI_MENU_NAV_RIGHT"

uint32_t g_color_form = 0;  // form whose strip is open
int g_color_open = -1;      // its list row, -1 = no strip open
int g_color_cursor = 0;     // value index under the strip's cursor
int g_color_orig = 0;       // value index the row had when the strip opened

const ItemDef* ActiveColorItem(int row) {
    const int m = MenuOfForm(g_color_form);
    if (m < 0 || row < 0 || row >= MenuItemCount(kMenus[m])) return nullptr;
    const ItemDef& it = MenuItemAt(kMenus[m], row);
    return it.kind == ItemKind::kColor ? &it : nullptr;
}

void PlayMenuSound(uint32_t name) { CallGuestFn1(kPlaySoundFn, name); }

void ColorStripClose(bool keep) {
    if (g_color_open < 0) return;
    if (const ItemDef* it = ActiveColorItem(g_color_open)) {
        if (!keep) ItemSetValueIndex(*it, g_color_orig);
        MC_INFO("[pause-menu] color strip on row {} closed, {} '{}'", g_color_open,
                keep ? "kept" : "back to", CvarGet(it->cvar));
    }
    g_color_open = -1;
    g_color_form = 0;
}

void ColorStripOpen(uint32_t form, int row, const ItemDef& it) {
    ColorStripClose(false);
    g_color_form = form;
    g_color_open = row;
    g_color_orig = g_color_cursor = ItemValueIndex(it);
    MC_INFO("[pause-menu] color strip on row {} opened at value {}", row, g_color_cursor);
}

// Walks the open strip; no wrap, the strip has an end like the garage's.
void ColorStripStep(int dir) {
    const ItemDef* it = ActiveColorItem(g_color_open);
    if (!it) {
        g_color_open = -1;
        g_color_form = 0;
        return;
    }
    const int last = ItemValueCount(*it) - 1;
    int next = g_color_cursor + dir;
    if (next < 0) next = 0;
    if (next > last) next = last;
    if (next == g_color_cursor) return;
    g_color_cursor = next;
    ItemSetValueIndex(*it, next);
    PlayMenuSound(dir < 0 ? kSoundNavLeft : kSoundNavRight);
}

// ── Native forms ───────────────────────────────────────────────────────
//
// Each menu is a list form of its own (ui_forms.h), the class OPTIONS and
// CONTROLLER are, built the first time it opens: the rows above go into the
// form's list and the form is the screen -- title, description, prompts and
// list, all drawn by the game. A SETTINGS-tab button opens its menu on top of
// the pause screen the way PM_GameOptions opens OPTIONS (sub_82668660); a
// Submenu row opens its menu on top of the one it is on, the way OPTIONS opens
// CONTROLLER (sub_8264C9F8). B is the game's own: the form closes and the one
// under it renders itself again, tabs and title included.

bool MenuFormAccept(uint32_t form, int row);
bool MenuFormCancel(uint32_t form);
void MenuFormClosed(uint32_t form);
bool MenuFormKey(uint32_t form, uint32_t key);

// How many times a menu's form may be built again for rows that changed (MenuDef::relayout): each
// one is a new form the game keeps (ui_forms.cpp holds 40, the menus use 30), and the layout changes
// once or twice in a run (an upscaler failing).
constexpr int kMaxFormRebuilds = 4;
int g_form_rebuilds = 0;

// CHECK: the page built again (needs a real DLSS failure, e.g. the fail injection of
// diffs	emporal-test.patch while DLSS runs): the log has "[pause-menu] 'DISPLAY': its rows changed
// ... built again (1 of 4)" and a second "[ui-forms] form 'DISPLAY' created" when DISPLAY is next
// opened (not before), the page opens with no DLSS in UPSCALING and no DLSS mode row, its rows and
// the highlight work, B and re-entering work, and a DISPLAY left open across the failure keeps its
// rows (DLSS mode row grey "(UNAVAILABLE)") with no row showing another row's label or value.
uint32_t EnsureMenuForm(int m) {
    const MenuDef& md = kMenus[m];
    // A form that was built with other rows than the live ones (an upscaler failed since): built
    // again, with the new rows, once it is closed. The old form and its rows stay where they are
    // (the game owns them; they are never opened again). A form that is open keeps its rows until it
    // is left (UpscalingMenuRows still returns them), so it is never indexed by the wrong ones.
    uint32_t old_form = 0;
    NativeRows old_rows;
    if (const uint32_t form = g_menu_form[m].load(std::memory_order_relaxed)) {
        if (!md.relayout || larecomp::ui::FormIsOpen(form) || g_form_rebuilds >= kMaxFormRebuilds ||
            !md.relayout(/*apply=*/false)) {
            return form;
        }
        old_form = form;
        old_rows = g_rows[m];
        g_rows[m] = NativeRows{};
        g_menu_form[m].store(0, std::memory_order_relaxed);
        md.relayout(/*apply=*/true);
        ++g_form_rebuilds;
        MC_INFO("[pause-menu] '{}': its rows changed since the page was built (what this PC lists "
                "changed: an upscaler, frame generation or the denoiser row): the page is built again ({} of {})",
                md.label, g_form_rebuilds, kMaxFormRebuilds);
    } else if (md.relayout) {
        md.relayout(/*apply=*/true);  // the rows as they are now
    }
    // The page cannot be built again: the old one is put back (its rows are the ones UpscalingMenuRows
    // no longer returns, so a row or two may be the wrong one until the next start; a build that fails
    // already warned).
    const auto fail = [&]() -> uint32_t {
        if (old_form) {
            g_menu_form[m].store(old_form, std::memory_order_relaxed);
            g_rows[m] = old_rows;
        }
        return 0;
    };
    if (!BuildNativeRows(m)) return fail();

    // Title and description are resolved through the string table once, by
    // the ctor. The description's key is the title's plus "_desc", the way
    // the game pairs "options" with "options_desc".
    EnsureStringTableText(md.menu_key, md.label);
    const std::string desc_key = std::string(md.menu_key) + "_desc";
    if (md.desc) EnsureStringTableText(desc_key.c_str(), md.desc);

    larecomp::ui::FormHandler handler;
    handler.accept = MenuFormAccept;
    handler.cancel = MenuFormCancel;
    handler.closed = MenuFormClosed;
    handler.key = MenuFormKey;

    const NativeRows& nr = g_rows[m];
    const uint32_t form = larecomp::ui::CreateListForm(
        md.menu_key, md.desc ? desc_key.c_str() : nullptr, nr.count, handler);
    if (!form) return fail();
    for (int i = 0; i < nr.count; ++i) {
        if (!larecomp::ui::AddFormRow(form, nr.row[i])) {
            MC_WARN("[pause-menu] '{}': row {} did not fit in the form", md.label, i);
            break;
        }
    }
    g_menu_form[m].store(form, std::memory_order_relaxed);
    return form;
}

// Opens menu m on top of `from`: the pause screen, or the form of the menu
// whose Submenu row was picked.
bool OpenMenu(int m, uint32_t from) {
    const uint32_t form = EnsureMenuForm(m);
    if (!form || !from) return false;
    SyncNativeRowsFromCvars(m);  // pick up F4-menu changes made meanwhile
    larecomp::ui::OpenForm(form, from);
    MC_INFO("[pause-menu] opened '{}'", kMenus[m].label);
    return true;
}

// ── "Restart now?" ─────────────────────────────────────────────────────
//
// A row whose label says "(RESTART)" (or "AFTER RESTART") writes its cvar at once, but the game
// reads it at the start, so nothing happens until it is started again. Rather than leave the
// player to look for RESTART GAME on the QUIT menu, leaving a menu in which such a setting was
// changed offers it: a form over the menu just left, NOT NOW (first, the form's default) or
// RESTART NOW (the QUIT menu's RESTART GAME: settings saved, the game started again, this one
// closed). NOT NOW and B keep today's behaviour: the setting applies at the next start.
//
// Which settings count: those the menu wrote (NoteMenuWrite, called by CvarSet, remembers each
// one's value from before) whose row says it needs a restart now (RowSaysRestart: the suffix is
// read as drawn, so a row that is live in this state, like an upscaler mode row, does not
// count), and that are not back at the value they had. One prompt per menu exit: what
// was offered is remembered per setting (`asked`), so leaving the next menu does not ask again
// until another setting changed. Guest thread only, like the menu.
struct RestartWatch {
    std::string cvar;
    std::string orig;   // its value before the menu first changed it
    std::string asked;  // its value when the player was last offered a restart
};
std::vector<RestartWatch>& g_restart_watch = larecomp::Leak<std::vector<RestartWatch>>();
// Set when one of our menus closes after B (MenuFormClosed); taken by RestartPromptTick.
std::atomic<bool> g_restart_check{false};

void NoteMenuWrite(const char* name) {
    for (const RestartWatch& w : g_restart_watch)
        if (w.cvar == name) return;
    const std::string now = CvarGet(name);
    g_restart_watch.push_back({name, now, now});
}

// "1" and "1.0" are the same value (a double row writes %g, the toml may say 1.0).
bool SameCvarValue(const std::string& a, const std::string& b) {
    if (a == b) return true;
    if (a.empty() || b.empty()) return false;
    char* end_a = nullptr;
    char* end_b = nullptr;
    const double x = std::strtod(a.c_str(), &end_a);
    const double y = std::strtod(b.c_str(), &end_b);
    return *end_a == '\0' && *end_b == '\0' && x == y;
}

// Whether the row's label, as it is drawn now, says the setting needs a restart.
bool RowSaysRestart(const ItemDef& it) {
    if (const auto* rules = ItemRules(it)) {
        const PauseRuleEnv env;
        const expr::RuleView row(*rules, env, &PauseRules());
        if (const auto* state = row.State())
            if (const auto restart = (*state)["needs_restart"].value<bool>()) return *restart;
        if (const auto apply = row.Field("apply").value<std::string_view>()) return *apply == "restart";
    }
    return it.suffix && std::string_view(it.suffix) == " (RESTART)";
}

// Whether a row that sets `cvar` (in any menu this launch lists) says it needs a restart.
bool CvarNeedsRestart(const std::string& cvar) {
    for (int m = 0; m < kNumMenus; ++m) {
        for (int i = 0; i < MenuItemCount(kMenus[m]); ++i) {
            const ItemDef& it = MenuItemAt(kMenus[m], i);
            if (it.kind != ItemKind::kBool && it.kind != ItemKind::kStrCycle &&
                it.kind != ItemKind::kDblCycle)
                continue;
            const char* row_cvar = ItemCvar(it);
            if (row_cvar && cvar == row_cvar && RowSaysRestart(it)) return true;
        }
    }
    return false;
}

// A setting that needs a restart has changed and the player has not been offered one for it.
bool RestartDue() {
    bool due = false;
    for (RestartWatch& w : g_restart_watch) {
        const std::string now = CvarGet(w.cvar.c_str());
        if (SameCvarValue(now, w.orig)) {  // back where it started: nothing to apply
            w.asked = now;
            continue;
        }
        if (!SameCvarValue(now, w.asked) && CvarNeedsRestart(w.cvar)) due = true;
    }
    return due;
}

void MarkRestartOffered() {
    for (RestartWatch& w : g_restart_watch) w.asked = CvarGet(w.cvar.c_str());
}

// Once per UI update (PauseMenuFormsTick): after a menu of ours was left, opens the prompt over
// what is on top now (the pause screen, or the menu the one left was opened from) when a setting
// that needs a restart has changed. A tick later, not inside the key handler that closed the form.
void RestartPromptTick() {
    if (!g_restart_check.exchange(false, std::memory_order_relaxed)) return;
    const uint32_t pause = larecomp::ui::PauseForm();
    const uint32_t top = larecomp::ui::TopForm();
    // The pause screen is gone, or a screen of the game's own has the pad: no prompt (the
    // settings were saved on the way out, and apply at the next start).
    if (!pause || !top || !larecomp::ui::FormIsOpen(pause)) return;
    if (top != pause && !larecomp::ui::IsOurForm(top)) return;
    const int ask = MenuByKey("RxRestartAskMenu");
    if (ask < 0 || top == g_menu_form[ask].load(std::memory_order_relaxed)) return;
    if (!RestartDue()) return;
    MarkRestartOffered();
    if (OpenMenu(ask, top))
        MC_INFO("[pause-menu] a setting that needs a restart changed: offered RESTART NOW");
    else
        MC_WARN("[pause-menu] could not open the restart prompt (top form 0x{:08X})", top);
}

bool MenuFormAccept(uint32_t form, int row) {
    const int m = MenuOfForm(form);
    if (m < 0 || row < 0 || row >= MenuItemCount(kMenus[m])) return false;
    const ItemDef& it = MenuItemAt(kMenus[m], row);
    // A greyed-out row takes no A, and no select sound, as the game's own
    // list does (sub_826334D0 asks the row's vfunc72 first).
    if (!ItemEnabled(it)) return false;

    if (it.kind == ItemKind::kSubmenu) {
        const int child = MenuByKey(it.cvar);
        if (child < 0) {
            MC_WARN("[pause-menu] '{}' row {} opens '{}', which is no menu", kMenus[m].label,
                    row, it.cvar ? it.cvar : "");
            return false;
        }
        return OpenMenu(child, form);
    }
    if (it.kind == ItemKind::kColor) {
        if (g_color_open == row && g_color_form == form)
            ColorStripClose(true);
        else
            ColorStripOpen(form, row, it);
        return true;
    }

    // A steps the value forward, the same as right. The row holds the select
    // itself, so the cvar move has to be mirrored back into it. Only the list
    // is drawn again, the way CONTROLLER redraws after a change: the whole
    // form's render is a full transition, and the list faded out and back in
    // on every checkbox. An action can close the menu (PLAY CUTSCENE does),
    // and then there is nothing to redraw.
    ClickItem(it);
    if (larecomp::ui::FormIsOpen(form)) {
        SyncNativeRowsFromCvars(m);
        larecomp::ui::RefreshFormList(form);
    }
    return true;
}

// B on an open color strip is the strip's: the old color comes back and the
// menu stays where it is. Any other B closes the form, the game's way.
bool MenuFormCancel(uint32_t form) {
    if (g_color_open < 0 || g_color_form != form) return false;
    ColorStripClose(false);
    PlayMenuSound(kSoundBack);
    return true;
}

// Settings persist on the way out instead of through a SAVE SETTINGS row.
//
// Every cvar here already applies the instant it changes - the row only
// ever controlled whether larecomp.toml was rewritten, which is not a
// distinction worth a menu entry. Writing on each change was the other
// option and is worse: a save rewrites the whole file, and scrubbing a
// value moves it several times a second, so it would put a full file write
// in the middle of gameplay. Saving once per menu left, and only when
// something actually moved, costs one write and no menu row.
void MenuFormClosed(uint32_t form) {
    if (g_color_form == form) ColorStripClose(false);
    const int m = MenuOfForm(form);
    MC_INFO("[pause-menu] left '{}'", m >= 0 ? kMenus[m].label : "?");
    if (!g_settings_saved.exchange(true, std::memory_order_relaxed)) SaveSettingsNow();
    // The prompt itself is not a menu a setting is changed in.
    if (m >= 0 && !kMenus[m].opened_by_code)
        g_restart_check.store(true, std::memory_order_relaxed);
}

// An open strip owns the d-pad: left/right walk it, the rest of the list's
// keys are held so the highlight cannot leave the row while it is open. The
// list's own codes (sub_826334D0): '/' '1' up, '0' '2' down, '3' '5' left,
// '4' '6' right; '9' ':' are the other repeating ones (sub_82631E20).
bool MenuFormKey(uint32_t form, uint32_t key) {
    if (g_color_open < 0 || g_color_form != form) return false;
    switch (key & 0xFF) {
    case '3':
    case '5': ColorStripStep(-1); return true;
    case '4':
    case '6': ColorStripStep(+1); return true;
    case '/':
    case '0':
    case '1':
    case '2':
    case '9':
    case ':': return true;
    default:  return false;
    }
}

// ── Submenu creation ───────────────────────────────────────────────────

// The SETTINGS-tab buttons, once, the first time the pause menu comes up. The
// forms themselves wait for their first open.
void EnsureSubmenus() {
    if (g_menus_created.load(std::memory_order_relaxed)) return;
    InitThemePalette();

    uint32_t settings = g_settingsmenu_addr.load(std::memory_order_relaxed);
    if (!settings) return;

    for (int m = 0; m < kNumMenus; ++m) {
        const MenuDef& md = kMenus[m];
        // The DEBUG hub and its pages exist only while the debug menu is unlocked: no button, no log line.
        if (MenuLocked(md)) continue;
        // Reached through the Submenu row that opens it, not from here.
        if (MenuIsNested(m)) continue;

        uint32_t btn = CreateNewMenuState(md.btn_key);
        if (!btn) return;
        GuestAppendMenuItem(settings, btn);
        EnsureStringTableText(md.btn_key, md.label);
        g_menu_btn_key[m].store(AllocGuestString(md.btn_key),
                                std::memory_order_relaxed);
        MC_INFO("[pause-menu] '{}' button added to SETTINGS ({} rows)", md.label,
                MenuItemCount(md));
    }

    g_menus_created.store(true, std::memory_order_relaxed);
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
    return ButtonPromptsArePlayStation();
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
    // The row moved to the RexGlue SETTINGS > INTERFACE > UI menu as "BUTTON LAYOUT" (three
    // values -- xbox, playstation, keyboard -- where this checkbox only had two, and a click
    // here silently left the keyboard layout). Nothing is added to the controller screen now.
    constexpr bool kRowOnControllerScreen = false;
    if (!kRowOnControllerScreen) return;
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

std::vector<larecomp::settings_ui::Menu> larecomp::settings_ui::AllSettingsMenus() {
    std::vector<Menu> menus;
    for (const MenuDef& menu : kMenus) {
        auto& exported = menus.emplace_back(menu.btn_key, menu.menu_key, menu.label, menu.desc, menu.debug_only,
                                            std::vector<mcla_menu::ItemDef>{});
        if (menu.num_items) {
            exported.rows.assign(menu.items, menu.items + menu.num_items);
        }
        if (menu.extra == DisplayPageRows) {
            // DISPLAY lists the vendor's latency reducer, then the upscaling block: export every row either can have.
#if defined(LARECOMP_HAS_GPU)
            exported.rows.insert(exported.rows.end(), std::begin(kReflexRows), std::end(kReflexRows));
            exported.rows.insert(exported.rows.end(), std::begin(kAntiLagRows), std::end(kAntiLagRows));
#endif
            AppendAllUpscalingPageRows(exported.rows);
        } else if (menu.extra) {
            // CameraLookMenuItems and the DEBUG accessors already return their full tables.
            int count = 0;
            const auto* rows = menu.extra(count);
            if (count) exported.rows.insert(exported.rows.end(), rows, rows + count);
        }
    }
    return menus;
}

namespace mcla_menu {

bool NativeRendererRuns() { return RunningRendererMask() == kRendNative; }

bool PauseRowListed(const ItemDef& item) {
    const auto* rules = ItemRules(item);
    if (!rules) return true;
    const PauseRuleEnv env;
    return expr::RuleView(*rules, env, &PauseRules()).IsListed();
}


}  // namespace mcla_menu

// ── Public API ──────────────────────────────────────────────────────────

// The game is ending by a way that did not go through a menu (the window's close, Alt+F4, a signal): a change made in a page that was
// still open has not been written (MenuFormClosed writes when a page is left, QUIT and RESTART write before they close).
void PauseMenuSavePendingSettings() {
    if (!g_settings_saved.exchange(true, std::memory_order_relaxed)) SaveSettingsNow();
}

// RESTART GAME: as the QUIT form's YES, but the game is started again before this one closes
// (restart.h); the new one waits for this one to be gone before it reads the settings or the saves.
// If it can't be started, the game keeps running rather than quit without it.
void PauseMenuRestartGame() {
    if (!g_settings_saved.exchange(true, std::memory_order_relaxed)) SaveSettingsNow();
    auto* ui = larecomp::sdk::UI();
    if (!ui || !ui->HasWindow()) {
        MC_WARN("[pause-menu] restart requested, but there is no window to close");
        return;
    }
    MC_INFO("[pause-menu] restart requested");
    // CHECK: rex::app::RequestRestart() starts the process again with the same arguments (the SDK's own relaunch); until it is shown to
    // wait for this process before the new one reads the config (see the CHECK in LarecompApp's constructor), the game keeps its own.
    ui->PostTask([] {
        MclaWaitForSaves();
        if (!mcla::RelaunchGame()) {
            return;
        }
        rex::app::RequestShutdown(0);
    });
}

// The graphics settings whose stored words changed (owner, 2026-11-14: the stored word is the shown word, nothing is called low):
// the ambient occlusion, the contact shadows and the bounce light are off / high / ultra, the car self-reflections off / on. No
// older value is read as a new one. A larecomp.toml from before still says "low" (or "high" for the self-reflections): the SDK
// refuses a toml value outside the setting's list when it reads the file (cvar.cpp ValidateConstraints, "not in allowed values"),
// so the setting runs at its default, auto, but the line stays in the file, because a save writes only what the player changed.
// Writing the cvar once with its own default, as the language setting does in InitLanguageSetting, makes the next save rewrite
// that line to "auto". A value from the command line or the environment is not checked by the SDK: the renderer takes a word
// that is no value as auto (gpu/host_plan.h), and the file is left alone (only a cvar the toml alone set is touched here).
static void ResetDeadGraphicsWords() {
    static const char* const kNames[] = {"mcla_gpu_ao", "mcla_gpu_contact_shadows", "mcla_gpu_ssgi",
                                         "mcla_gpu_car_selfrefl"};
    for (const char* name : kNames) {
        const larecomp::cvar::FlagEntry* info = larecomp::cvar::GetFlagInfo(name);
        if (!info || info->source != larecomp::cvar::Source::kDefault || !info->constraints.HasAllowedValues())
            continue;
        std::string saved = TomlLineValue(ConfigPath(), name);
        // The toml's own quotes are not part of the value.
        if (saved.size() >= 2 && (saved.front() == '"' || saved.front() == '\x27') &&
            saved.back() == saved.front())
            saved = saved.substr(1, saved.size() - 2);
        const auto& allowed = info->constraints.allowed_values;
        if (saved.empty() || std::find(allowed.begin(), allowed.end(), saved) != allowed.end())
            continue;
        MC_WARN("[settings] larecomp.toml has {} = '{}', which is no longer a value of the setting; "
                "using {} (the next settings save rewrites the line)", name, saved, info->default_value);
        larecomp::cvar::SetFlagByName(name, info->default_value);
    }
}

void InitPauseMenuHooks() {
    ResetDeadGraphicsWords();
    larecomp::cvar::RegisterChangeCallback(
        "language", [](std::string_view, std::string_view) {
            const int idx = ResolveLanguageIndex();
            if (idx >= 0)
                g_pending_language.store(idx, std::memory_order_relaxed);
        });
    // Runs from FinishSetupAfterBootBuild, before ReXApp::LaunchModule starts
    // the guest -- the only point where the online cvar is still the player's.
    InitOnlineSettings();
    // The test pattern lives for the run: a save never writes it, and one an older save left in the
    // file is switched off now (it would cover the menu that could switch it off).
    larecomp::NoteSessionOnlyCvar(kHdrTestPatternCvar);
    if (const larecomp::cvar::FlagEntry* info = larecomp::cvar::GetFlagInfo(kHdrTestPatternCvar);
        info && info->source == larecomp::cvar::Source::kConfig && info->getter() == "true") {
        MC_INFO("[pause-menu] {} = true in larecomp.toml is ignored: the test pattern is never kept",
                kHdrTestPatternCvar);
        larecomp::cvar::SetFlagByName(kHdrTestPatternCvar, "false");
    }
    MC_INFO("[pause-menu] hooks registered");
}

// OnPreSetup, before the runtime exists and so before any guest code: the game's
// language comes from the console's user language, which in this runtime is the
// SDK's `user_language` cvar (XLanguage id; see "Language" above). The choice in
// `language` is written there, so the game's own path -- mapper, string table, hard-coded
// text, achievements, Discord -- all see it. AUTO writes nothing: the SDK default
// (English), or the player's own user_language, stays. NoteStartupCvar keeps this
// write out of larecomp.toml: the file keeps the player's `language`.
void InitLanguageSetting() {
    const larecomp::cvar::FlagEntry* user = larecomp::cvar::GetFlagInfo("user_language");
    if (!user) {
        MC_WARN("[language] this SDK has no user_language cvar; the language choice only "
                "reloads the string table");
        return;
    }
    // The getter prints the number (the SDK's cvar is a uint32).
    g_auto_xlanguage = uint32_t(std::strtoul(user->getter().c_str(), nullptr, 10));

    // A `language` in larecomp.toml that is not offered at all (jp, a typo) was refused when
    // the file was read, so the cvar is still at its default: it is auto, as the log line says.
    // The cvar is then written once with its own default, so the next settings save rewrites
    // the old line to "auto". A saved pt is offered and stays: whether it takes effect depends
    // on the mods' files (DecidePortuguese), which are not known yet, and it must survive a
    // run without the mod.
    const larecomp::cvar::FlagEntry* lang = larecomp::cvar::GetFlagInfo("language");
    if (lang && lang->source == larecomp::cvar::Source::kDefault) {
        std::string saved = TomlLineValue(ConfigPath(), "language");
        // The toml's own quotes are not part of the value.
        if (saved.size() >= 2 && (saved.front() == '"' || saved.front() == '\x27') &&
            saved.back() == saved.front())
            saved = saved.substr(1, saved.size() - 2);
        bool offered = saved.empty();
        for (const char* v : kLangVals) offered = offered || saved == v;
        if (!offered) {
            MC_WARN("[language] larecomp.toml has language = '{}', which is not offered (the disc "
                    "has no text for it); using auto", saved);
            larecomp::cvar::SetFlagByName("language", "auto");
        }
    }

    const std::string choice = CvarGet("language");
    int pos = -1;
    for (int i = 0; i < kNumLangs; ++i)
        if (choice == kLangVals[i]) pos = i;
    if (pos < 0) {  // a value the cvar's list would have refused, but a path let through
        MC_WARN("[language] '{}' is not offered; using auto", choice);
        larecomp::cvar::SetFlagByName("language", "auto");
        pos = 0;
    }
    if (pos == 0) {
        MC_INFO("[language] auto: user_language {} stays as it is (string table {})",
                g_auto_xlanguage, AutoLanguageIndex());
        return;
    }
    if (kLangXLanguage[pos] == 0) {  // pt: no XLanguage gives string table 5
        MC_INFO("[language] pt: user_language {} stays as it is; the text comes from the string "
                "tables of a mod, checked when the game starts", g_auto_xlanguage);
        return;
    }

    const std::string xlanguage = std::to_string(kLangXLanguage[pos]);
    if (!larecomp::cvar::SetFlagByName("user_language", xlanguage)) {
        MC_WARN("[language] could not set user_language {} for '{}'; the game starts in "
                "user_language {}", xlanguage, choice, g_auto_xlanguage);
        return;
    }
    larecomp::NoteStartupCvar("user_language");
    MC_INFO("[language] {} -> user_language {} (string table {})", choice, xlanguage,
            kLangIndex[pos]);
}

// sub_82218A38(mgr, langIndex) -- the one place the string table's language is
// ever set. Hooked one instruction past the prologue, before `mr r29, r4`
// copies the index off. With InitLanguageSetting the game's own mapper already
// gives the index the `language` cvar asks for for en/es/fr/de/it (and so does
// our live reload), so there this changes nothing; it is a safety net that says
// so in the log when the two ever differ. pt is the one that needs it: the
// mapper cannot return 5, so the index is rewritten here, and only when the
// mods' string tables have Portuguese text (otherwise LanguageIndexFromCvar
// says auto and the game's own index stands).
// The first call is the boot load, which is where pt is decided: the mods' files
// are known by then.
bool Hook_StringTableLanguage(PPCRegister& r3, PPCRegister& r4) {
    (void)r3;
    DecidePortuguese(CvarGet("language") == kLangVals[kPtPos]);
    const int idx = LanguageIndexFromCvar();
    if (idx >= 0 && uint32_t(idx) != uint32_t(r4.u64)) {
        if (idx == kPtIndex) {
            MC_INFO("[language] pt: string table block {} instead of the game's {}", idx,
                    uint32_t(r4.u64));
        } else {
            // CHECK: not expected; a run with language set to de/fr/es/it must not log this.
            MC_WARN("[language] the game asked for string table {}, the language cvar says {}: "
                    "using {}", uint32_t(r4.u64), idx, idx);
        }
        r4.u64 = uint32_t(idx);
    }
    return false;
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
    EnsureLanOnlineItems();
    KeepOnlineChoiceInConfig();

    {
        const int lang =
            g_pending_language.exchange(-1, std::memory_order_relaxed);
        if (lang >= 0) ApplyLanguageNow(lang);
    }

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
    if (!larecomp::settings::show_test_movie.Get())
        return false;

    PatchStringTableEntry("PM_TestMovie", "Teste");

    r4.u64 = 1;
    return true;
}

// A on the pause screen (0x826686B8, sub_82668660 right before its own
// PM_GameOptions check). Our SETTINGS buttons open their menus the way that
// one opens OPTIONS: form->vfunc+0x2C0(form, pause form). The pause screen only
// gets the pad while it is the top form, so A inside one of our menus never
// lands here -- that is the menu's own handler (MenuFormAccept). true leaves
// through 0x82668704, the handler's `return 1`.
bool Hook_PMLodTrafficClick(PPCRegister& r3, PPCRegister& r31) {
    (void)r31;

    for (int m = 0; m < kNumMenus; ++m) {
        uint32_t btn_key = g_menu_btn_key[m].load(std::memory_order_relaxed);
        if (!btn_key || !IsGuestButtonClicked(btn_key)) continue;

        const uint32_t pause = larecomp::ui::PauseForm();
        if (!OpenMenu(m, pause)) {
            MC_WARN("[pause-menu] could not open '{}' (pause form 0x{:08X})",
                    kMenus[m].label, pause);
            return false;
        }
        r3.u64 = 1;
        return true;
    }

    return false;
}

bool Hook_ListViewPopulate(PPCRegister& r3) {
    uint32_t list = static_cast<uint32_t>(r3.u64);
    if (!list) return false;

    g_last_list.store(list, std::memory_order_relaxed);
    // The renderer's own question (vfunc 60 = sub_8221CDD0: the flag at +16 & 0x20), asked the way it asks; the flag itself when the slot is not there.
    bool active = (ReadGuestBE32(list + 16) & 0x20) != 0;
    if (const uint32_t vt = ReadGuestBE32(list)) {
        const uint32_t is_active = ReadGuestBE32(vt + 60);
        if (is_active >= 0x82000000u && is_active < 0x82FFFFF0u) active = (CallGuestFn1(is_active, list) & 0xFF) != 0;
    }
    if (active) {
        g_last_active_list.store(list, std::memory_order_relaxed);
        g_last_list_seq.store(g_populate_seq.fetch_add(1, std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    }

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

    // One of our menus' own lists. This runs at the top of the renderer,
    // before a single row is written, so reading the rows' select here is what
    // carries a left/right through to the cvar in the same frame it is drawn.
    const int m = MenuOfList(list);
    if (m >= 0) {
        PollNativeRows(m);

        if (CvarGetBool("carbon_menu_diag")) {
            const NativeRows& nr = g_rows[m];
            const MenuDef& md = kMenus[m];
            for (int i = 0; i < nr.count && i < MenuItemCount(md); ++i) {
                const ItemDef& it = MenuItemAt(md, i);
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
                        "slot={} enabled={}",
                        i, nr.last_label[i], nr.row[i], n, idx,
                        (idx >= 0 && idx < n) ? ItemValueLabel(it, idx)
                                              : std::string(),
                        int(ReadGuestBE32(nr.row[i] + kRowSelect)),
                        ReadGuestU8(nr.row[i] + kRowValueEnabled));
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
    // Runs for every Flash UI command: build the line only when it is logged.
    if (!::mc::log::mc.IsEnabled(::rex::log::Level::kInfo)) return false;
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

    LanOnlineTick();

    // Pad edges are tracked once and shared by both consumers.
    static uint16_t prev_buttons = 0;
    uint16_t pressed = 0;
    {
        if (auto* isys = larecomp::sdk::Input()) {
            if (auto reading = isys->GetState(0)) {
                const uint16_t buttons = reading->state.buttons;
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

    constexpr uint16_t kPadA = rex::hid::kPadA;
    constexpr uint16_t kPadB = rex::hid::kPadB;

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

// ── Garage camber row ───────────────────────────────────────────────────
//
// Same shape as the tire offset row, same axle chooser. The value is the car's
// Camber0/1 (see CamberDegrees in tire_camber.h), written to the live config
// and to the owned car's save record.
constexpr const char* kCamberName = "RxCamber";
constexpr int kCamberMin   = -15;
constexpr int kCamberMax   = 5;
constexpr int kCamberCount = kCamberMax - kCamberMin + 1;
constexpr uint32_t kReflCamberOff = 2032;  // f32, +0 front, +4 rear
constexpr uint32_t kCustomCamberOff = 64 + kReflCamberOff;

static std::atomic<uint32_t> g_camber_state{0};

// The notch the row shows for an axle: the stored angle, rounded and limited to
// what the row can display.
static int CamberGet(int axle) {
    const uint32_t refl = GarageReflBase();
    if (!refl) return 0;
    const uint32_t bits = ReadGuestBE32(refl + kReflCamberOff + uint32_t(axle) * 4);
    float stored;
    std::memcpy(&stored, &bits, sizeof(stored));
    const int value = int(std::lround(CamberDegrees(stored, axle)));
    if (value < kCamberMin) return kCamberMin;
    return value > kCamberMax ? kCamberMax : value;
}

static void CamberSet(int selection, int value) {
    const uint32_t refl = GarageReflBase();
    if (!refl) return;

    for (int axle = 0; axle < 2; ++axle) {
        if (selection != 2 && selection != axle) continue;
        float stored = float(value);
        if (value == 0) {
            stored = kStockCamber[axle];
        } else if (stored == kStockCamber[axle]) {
            stored -= 0.01f;  // a real angle must not read as the stock mark
        }
        uint32_t bits;
        std::memcpy(&bits, &stored, sizeof(bits));
        WriteGuestBE32(refl + kReflCamberOff + uint32_t(axle) * 4, bits);
    }
    WriteGuestBE32(refl + kReflDirtyFlags,
                   ReadGuestBE32(refl + kReflDirtyFlags) | 0x81u);
    CustomDataStoreInOwnedRecord(refl - 64, kCustomCamberOff, 8);
}

static const char* CamberChildName(int i) {
    static char buf[kCamberCount][32];
    std::snprintf(buf[i], sizeof(buf[0]), "%s.s%d", kCamberName, i);
    return buf[i];
}

static void RefreshCamberLabel() {
    EnsureStringTableText(kCamberName, "CAMBER");
    const uint32_t item = g_camber_state.load(std::memory_order_relaxed);
    if (!item) return;
    const int value = CamberGet(GarageAxleSelection() == 1 ? 1 : 0);
    WriteGuestBE32(item + kStateActiveChild, uint32_t(value - kCamberMin));
}

static void EnsureCamberEntry() {
    if (!CvarGetBool("garage_camber")) return;

    static uint32_t axle_name = 0;
    if (!axle_name) axle_name = AllocGuestString("Axle_Menu");
    const uint32_t axle = axle_name ? CallGuestFn1(kFindUIObjectFn, axle_name) : 0;
    const uint32_t parent = axle ? ReadGuestBE32(axle + 32) : 0;
    if (!parent) return;

    const uint32_t cached = g_camber_state.load(std::memory_order_relaxed);
    if (cached) {
        uint8_t* base = GetMembase();
        const uint32_t name_ptr = base ? ReadGuestBE32(cached + 20) : 0;
        if (name_ptr && ReadGuestBE32(cached + 32) == parent &&
            std::strcmp(reinterpret_cast<const char*>(base + name_ptr),
                        kCamberName) == 0) {
            RefreshCamberLabel();
            return;
        }
        g_camber_state.store(0, std::memory_order_relaxed);
    }

    const uint32_t item = CreateNewMenuState(kCamberName);
    if (!item) return;

    GuestAppendMenuItem(parent, item);
    WriteGuestBE32(item + 16, 0x42);

    for (int i = 0; i < kCamberCount; ++i) {
        const char* key = CamberChildName(i);
        const uint32_t child = CreateNewMenuState(key);
        if (!child) continue;
        GuestAppendMenuItem(item, child);
        WriteGuestBE32(child + 16, 0x42);

        char value[16];
        std::snprintf(value, sizeof(value), "%+d", i + kCamberMin);
        EnsureStringTableText(key, value);
    }

    g_camber_state.store(item, std::memory_order_relaxed);
    RefreshCamberLabel();
    MC_INFO("[camber] row 0x{:08X} + {} values under 0x{:08X}", item,
            kCamberCount, parent);
}

static bool CamberOnStateActivate(const char* name) {
    if (!name) return false;

    if (std::strcmp(name, "DimensionMenu") == 0) {
        EnsureCamberEntry();
        return false;
    }

    if (!g_camber_state.load(std::memory_order_relaxed)) return false;

    if (std::strcmp(name, "Front_Axle") == 0 ||
        std::strcmp(name, "Rear_Axle") == 0 ||
        std::strcmp(name, "Both_Axle") == 0) {
        RefreshCamberLabel();
        return false;
    }

    const size_t len = std::strlen(kCamberName);
    if (std::strncmp(name, kCamberName, len) != 0) return false;

    if (name[len] == '\0') {
        RefreshCamberLabel();
        return true;
    }

    if (name[len] != '.' || name[len + 1] != 's') return false;
    const int index = std::atoi(name + len + 2);
    if (index < 0 || index >= kCamberCount) return true;

    const int value = index + kCamberMin;
    const int selection = GarageAxleSelection();
    // The value already on the row also activates when the highlight lands on
    // it. Writing then would round a hand-edited angle, and on BOTH would copy
    // the front over the rear.
    if (value == CamberGet(selection == 1 ? 1 : 0)) return true;

    CamberSet(selection, value);
    RefreshCamberLabel();
    MC_INFO("[camber] axle {} -> {:+d}", selection, value);
    return true;
}

void CarbonOnStateActivate(const char* name, uint32_t state) {
    if (const uint32_t lan = g_lan_state.load(std::memory_order_relaxed); lan && state)
        g_lan_active.store(state == lan || ReadGuestBE32(state + 32) == lan,
                           std::memory_order_relaxed);

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
    if (CamberOnStateActivate(name)) return;

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

// 0x8222064C, sub_82220308 (FlashNavigator filling a menu's "menu" object),
// right after the element of one child was written and its fs_ attributes
// applied (sub_8221EF60) -- past the select it writes for every element.
// r27 = the menu, r28 = the element, r30 = the child.
void Hook_LanFillElement(PPCRegister& r27, PPCRegister& r28, PPCRegister& r30) {
    (void)r27;
    const uint32_t online = g_lan_online.load(std::memory_order_relaxed);
    if (!online || r30.u32 != online) return;
    const uint32_t element = r28.u32;
    // Where there is no online client (Linux) the row is greyed and never ticked.
    const int checked = larecomp::kOnlineAvailable && LanOnlineChecked() ? 1 : 0;
    SwfSetInt(element, kStr_type,
              larecomp::kOnlineAvailable ? kElementTypeCheckbox : kElementTypeGreyed);
    SwfSetInt(element, kStr_select, checked);
    g_lan_drawn_checked = checked;
    static std::atomic<bool> told{false};
    if (!told.exchange(true))
        MC_INFO("[online] SYSTEM LINK pop-up: ONLINE drawn as a checkbox "
                "(element 0x{:08X} type {} select {})",
                element, SwfGetInt(element, kStr_type), SwfGetInt(element, kStr_select));
}

// 0x8263B9F0, sub_8263B9A0 (UIMenu's event handler), once the event is known
// to come from the menu input; r30 = the menu, r28 = the event (code first).
// The ONLINE child has no transition behind it, so the game does nothing with
// A there. Every A that reaches the SYSTEM LINK menu is logged with the row it
// lands on. true leaves through loc_8263BBC0 (li r3, 1): handled.
bool Hook_LanAccept(PPCRegister& r30, PPCRegister& r28) {
    const uint32_t lan = g_lan_state.load(std::memory_order_relaxed);
    const uint32_t live = g_live_state.load(std::memory_order_relaxed);
    const uint32_t menu = r30.u32;
    if (!menu || (menu != lan && menu != live) || ReadGuestBE32(r28.u32) != kMenuEventAccept)
        return false;

    const uint32_t online = g_lan_online.load(std::memory_order_relaxed);
    const uint32_t selected_fn = MenuVfunc(menu, kMenuSelectedVfunc);
    const uint32_t child_fn = MenuVfunc(menu, kMenuChildAtVfunc);
    const int32_t row = selected_fn ? int32_t(CallGuestFn1(selected_fn, menu)) : -1;
    const uint32_t child =
        (row >= 0 && child_fn) ? CallGuestFn2(child_fn, menu, uint32_t(row)) : 0;
    const uint32_t browser = (menu == lan ? g_lan_browser : g_live_browser).load(std::memory_order_relaxed);
    if (browser && child == browser) {
        MC_INFO("[online] SERVER BROWSER from the {} menu", menu == lan ? "SYSTEM LINK" : "XBOX LIVE");
        larecomp::ServerBrowserOpen();
        if (const uint32_t sound = ReadGuestBE32(kMenuSelectSound))
            CallGuestFn1(kPlaySoundFn, sound);
        return true;
    }
    if (menu != lan) return false;
    MC_INFO("[online] A in SYSTEM LINK on row {} (0x{:08X}{})", row, child,
            child && child == online ? ", ONLINE" : "");
    if (!online || child != online) return false;

    if (!larecomp::kOnlineAvailable) {
        // Taken, with no sound and no change: the pop-up beside the menu says why.
        MC_INFO("[online] ONLINE in the SYSTEM LINK menu: {}", larecomp::kOnlineUnavailableText);
        larecomp::OnlineToastNotice();
        return true;
    }
    LanOnlineToggle();
    if (const uint32_t sound = ReadGuestBE32(kMenuSelectSound))
        CallGuestFn1(kPlaySoundFn, sound);
    return true;
}

// sub_826309E0, the container key dispatch (0x82630A14, past the prologue: r31
// container, r28 key, r27 the dispatch argument). It used to send left/right
// past the pause screen's tab strip into the list a RexGlue submenu borrowed;
// our menus are forms of their own now (ui_forms.h), with no tab strip in
// front of their lists, so the game's dispatch already gets every key to the
// right widget. Kept, inert, for the toml entry that names it.
bool Hook_MenuKeyDispatch(PPCRegister& r31, PPCRegister& r28,
                          PPCRegister& r27) {
    (void)r31;
    (void)r28;
    (void)r27;
    return false;
}

// sub_82220308 (FlashNavigator populate, 0x82220314). It redirected the
// SettingsMenu populate to the UIMenu state a submenu was pushed as; a form
// renders its own list, so there is nothing to redirect. It only notes which
// UIMenu is being filled into a movie's "menu" object (r3, the menu: the
// function reads its children with sub_8268DCB0 and calls its vfunc 332): the
// mouse in the menus (input/menu_mouse.cpp) reads the highlighted row of the
// garage's and the pop-ups' menus from it. Always false: the call goes on.
bool Hook_PopulateRedirect(PPCRegister& r3) {
    if (r3.u32) {
        g_last_ui_menu.store(r3.u32, std::memory_order_relaxed);
        g_last_ui_menu_seq.store(g_populate_seq.fetch_add(1, std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    }
    return false;
}

uint32_t PauseMenuLastList() { return g_last_active_list.load(std::memory_order_relaxed); }
uint32_t PauseMenuLastListSeq() { return g_last_list_seq.load(std::memory_order_relaxed); }
uint32_t PauseMenuLastUiMenu() { return g_last_ui_menu.load(std::memory_order_relaxed); }
uint32_t PauseMenuLastUiMenuSeq() { return g_last_ui_menu_seq.load(std::memory_order_relaxed); }
uint32_t PauseMenuTopOurList() {
    const uint32_t form = larecomp::ui::TopForm();
    return MenuOfForm(form) >= 0 ? larecomp::ui::FormList(form) : 0;
}
bool PauseMenuIsOurList(uint32_t list) { return list && MenuOfList(list) >= 0; }

// Null-guard for sub_82661210, which loads the list table-source pointer from
// [list+0xC0] and dereferences it at +0xC with no null check. A list whose rows
// are row objects (ours, in their forms) has no table source at all, so it is
// legitimately null there. Returning true jumps to 0x82661290, the function's
// own `li r3, 0` early-out.
bool MCLA_MenuListNullTableGuard(PPCRegister& r30) {
    return r30.u32 == 0;
}

// Guard for sub_8265F008 at 0x8265F0F0, which does
//
//     r10 = *(r31 + 0);            // the list's vtable
//     r9  = *(r10 + 648);          // vfunc 162
//     bctrl                        // vfunc(list, index)
//
// with no check. r31 is a menu list -- it is read at +184, the selected index,
// a few instructions earlier. Measured after a cutscene replay: that vtable is
// ZERO, so activating PauseMenu (which walks its children, SettingsMenu among
// them) calls through a dead object and takes the process down with a read at
// 0x288. Skipping the call costs one list not being refreshed; making it costs
// the game. Returning true jumps past the bctrl, to 0x8265F108, where r3 is
// reloaded from r28 anyway so nothing downstream reads the missing result.
//
// The address is logged once because the object identity is still unexplained:
// something is destroying a list the pause menu tree still points at.
bool MCLA_PauseListVtableGuard(PPCRegister& r31) {
    const uint32_t list = r31.u32;
    if (!list || ReadGuestBE32(list) != 0) return false;
    static std::atomic<bool> told{false};
    if (!told.exchange(true))
        MC_WARN("[pause-menu] list 0x{:08X} has a null vtable -- skipping the "
                "refresh call that would have crashed. Something freed a list "
                "the menu tree still references.", list);
    return true;
}

bool PauseMenuPopStack() {
    return GuestStackPop();
}

// Closes whatever of ours is open, top first, the way B would, for the exits
// that are not a B press. The cutscene gallery is one: it unpauses the game to
// play a scene, and a menu of ours left open over the pause screen would keep
// the pad after the pause menu itself went away.
void PauseMenuLeaveSubmenu() {
    ColorStripClose(false);
    int closed = 0;
    for (uint32_t form = larecomp::ui::TopmostOurForm(); form && closed < kNumMenus;
         form = larecomp::ui::TopmostOurForm()) {
        larecomp::ui::CloseForm(form);
        ++closed;
    }
    if (!closed) return;
    MC_INFO("[pause-menu] {} menu(s) of ours closed without a B press", closed);
    if (!g_settings_saved.exchange(true, std::memory_order_relaxed)) SaveSettingsNow();
}

// PM_Continue's whole body (sub_82668660 calls it on accept for that row): form 71's close, goto the in-game state, menu flag
// cleared. The cutscene gallery closes the menu with the same call (gameplay/cutscene_gallery.cpp), where it is measured: it
// is the only supported way out, popping the navigation stack by hand unwinds the screens the HUD lives on.
constexpr uint32_t kPauseMenuContinueFn = 0x8265F428;

bool PauseMenuOpen() {
    const uint32_t pause = larecomp::ui::PauseForm();
    return pause && larecomp::ui::FormIsOpen(pause);
}

bool PauseMenuClose() {
    // PM_Continue closes form 71 and then goes to the in-game state from wherever the machine is, with no check that the pause
    // screen is what is up. Asked while it is not (the game paused by the full map or a toolkit pause, or a screen that is already
    // going away), it moves the machine out of a state it knows nothing about. Only ever for the open pause screen.
    if (!PauseMenuOpen()) return false;
    PauseMenuLeaveSubmenu();
    auto* rt = REX_RUNTIME();
    PPCFunc* fn = rt ? rt->FindFunction(kPauseMenuContinueFn).value_or(nullptr) : nullptr;
    if (!fn) return false;
    larecomp::guest::Call<void>(fn);
    return true;
}

// Once per UI update, guest thread (swf_dom.cpp's mcUIManager::Update wrapper).
// Our menus only ever open over the pause screen; if the game takes the pause
// screen away under them on its own, they go with it instead of being left on
// the open-form stack holding the pad. The color strip closes as soon as its
// form is not the top one.

// Whether the highlighted row of the menu on top is DISPLAY > HDR > TEST PATTERN.
bool HdrTestPatternRowFocused() {
    const int m = MenuByKey("RxGpuHdrMenu");
    if (m < 0) return false;
    const uint32_t form = g_menu_form[m].load(std::memory_order_relaxed);
    if (!form || larecomp::ui::TopForm() != form) return false;
    const int row = larecomp::ui::FormSelectedRow(form);
    if (row < 0 || row >= MenuItemCount(kMenus[m])) return false;
    const char* cvar = MenuItemAt(kMenus[m], row).cvar;
    return cvar && std::strcmp(cvar, kHdrTestPatternCvar) == 0;
}

// The test pattern the menu's row switched on goes off as soon as that row is not the highlighted
// row of the open HDR menu: the cursor moved, the HDR menu was left or the pause menu closed.
// Its box follows the cvar (PollNativeRows). A pattern set on the command line (or from the F4
// panel or the console) is not the menu's and stays as it is.
// CHECK: the box clears when the cursor leaves the row; FormSelectedRow is the row the cursor is on
// at the tick, not one frame behind.
void HdrTestPatternTick() {
    if (!g_hdr_pattern_by_menu.load(std::memory_order_relaxed)) return;
    if (CvarGetBool(kHdrTestPatternCvar) && HdrTestPatternRowFocused()) return;
    if (CvarGetBool(kHdrTestPatternCvar)) {
        MC_INFO("[pause-menu] the HDR test pattern is off: its row is not highlighted any more");
        CvarSet(kHdrTestPatternCvar, "false");  // clears g_hdr_pattern_by_menu
    }
    g_hdr_pattern_by_menu.store(false, std::memory_order_relaxed);
}

// The HDR menu's level rows (PAPER WHITE, PEAK BRIGHTNESS, HIGHLIGHTS, TEST PATTERN) are enabled while the swap chain IS HDR10
// (HdrOutputActive), not while the setting says HDR. A step on the HDR row changes the setting at once, but the render thread switches
// the swap chain a few frames later, after the rows were drawn: they showed the state before the step ("(HDR OFF)" with HDR just
// turned on, live values with it just turned off) until the next input (Mz, 2026-11-08). Watched here once per UI update: when the
// output flips while the HDR menu is open, its rows are synced and the list drawn again, the way a row is after A.
void HdrOutputWatchTick() {
#if defined(LARECOMP_HAS_GPU)
    static bool last = false;  // guest thread only
    const bool now = mcla::gpu::HdrOutputActive();
    if (now == last) return;
    last = now;
    const int m = MenuByKey("RxGpuHdrMenu");
    if (m < 0) return;
    const uint32_t form = g_menu_form[m].load(std::memory_order_relaxed);
    if (!form || !larecomp::ui::FormIsOpen(form)) return;
    SyncNativeRowsFromCvars(m);
    larecomp::ui::RefreshFormList(form);
    MC_INFO("[pause-menu] HDR output is {} now: the HDR menu's rows follow it", now ? "on" : "off");
#endif
}

void PauseMenuFormsTick() {
    HdrTestPatternTick();
    HdrOutputWatchTick();
    RestartPromptTick();
    if (g_color_open >= 0 && larecomp::ui::TopForm() != g_color_form) ColorStripClose(false);
    if (!larecomp::ui::TopmostOurForm()) return;
    if (larecomp::ui::FormIsOpen(larecomp::ui::PauseForm())) return;
    MC_WARN("[pause-menu] the pause screen closed under our menus; closing them");
    PauseMenuLeaveSubmenu();
}

// B on the pause screen (0x82669864). It used to take a borrowed list back
// from a RexGlue submenu; our menus are forms now, and B on them is the game's
// own (sub_82653EF8, reached through ui_forms.cpp). Kept, inert, for the toml
// entry that names it.
bool Hook_RexGlueCancel(PPCRegister& r31) {
    (void)r31;
    return false;
}

bool PauseMenuUiOptionsOpen() {
    const int m = TopMenu();
    return m >= 0 && std::string_view(kMenus[m].menu_key) == "RxUiMenu";
}

// Guest thread, like the rows themselves. Values are read back from the cvars,
// except the open strip's, which is the cursor (it is also the live cvar).
bool PauseMenuColorButtons(PauseMenuColorRows* out) {
    if (!out) return false;
    *out = PauseMenuColorRows{};
    const uint32_t form = larecomp::ui::TopForm();
    const int m = MenuOfForm(form);
    if (m < 0) return false;
    const MenuDef& md = kMenus[m];
    const bool strip_here = g_color_open >= 0 && g_color_form == form;

    // Up/down are held while a strip is open (MenuFormKey). Should one get
    // past that anyway, the strip must not stay drawn on a row the highlight
    // has left, nor be confirmed from another one.
    if (strip_here) {
        const int sel = larecomp::ui::FormSelectedRow(form);
        if (sel != g_color_open) {
            MC_INFO("[pause-menu] color strip closed: the highlight moved to row {}", sel);
            ColorStripClose(false);
        }
    }
    const int open = g_color_open >= 0 && g_color_form == form ? g_color_open : -1;
    for (int i = 0; i < MenuItemCount(md) && out->count < PauseMenuColorRows::kMax; ++i) {
        const ItemDef& it = MenuItemAt(md, i);
        if (it.kind != ItemKind::kColor) continue;
        const int k = out->count++;
        out->row[k] = i;
        out->cvar[k] = it.cvar;
        out->value[k] = open == i ? g_color_cursor : ItemValueIndex(it);
        if (open == i) out->open = k;
    }
    out->values = out->count ? ItemValueCount(MenuItemAt(md, out->row[0])) : 0;
    out->scroll = larecomp::ui::FormScroll(form);
    return out->count > 0;
}

#else // REXGLUE_HAS_XEO3_TARGET
// XEO3 stubs: empty implementations so the linker resolves codegen calls.

#include <rex/ppc/context.h>
#include "pause_menu.h"

bool PauseMenuPopStack() { return false; }
bool PauseMenuUiOptionsOpen() { return false; }
bool PauseMenuColorButtons(PauseMenuColorRows* out) {
    if (out) *out = PauseMenuColorRows{};
    return false;
}
void PauseMenuLeaveSubmenu() {}
bool PauseMenuOpen() { return false; }
bool PauseMenuClose() { return false; }
void PauseMenuRestartGame() {}
void PauseMenuSavePendingSettings() {}
void PauseMenuFormsTick() {}
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
void Hook_LanFillElement(PPCRegister& r27, PPCRegister& r28, PPCRegister& r30) {}
bool Hook_LanAccept(PPCRegister& r30, PPCRegister& r28) { return false; }
void CarbonOnStateActivate(const char* name, uint32_t state) {}
bool Hook_FlashCommandLog(PPCRegister& r5) { return false; }
bool Hook_PopulateRedirect(PPCRegister& r3) { return false; }
uint32_t PauseMenuLastList() { return 0; }
uint32_t PauseMenuLastListSeq() { return 0; }
uint32_t PauseMenuLastUiMenu() { return 0; }
uint32_t PauseMenuLastUiMenuSeq() { return 0; }
uint32_t PauseMenuTopOurList() { return 0; }
bool PauseMenuIsOurList(uint32_t) { return false; }
bool MCLA_MenuListNullTableGuard(PPCRegister& r30) { return false; }
bool MCLA_PauseListVtableGuard(PPCRegister&) { return false; }
bool Hook_RexGlueCancel(PPCRegister& r31) { return false; }
bool Hook_StringTableLanguage(PPCRegister& r3, PPCRegister& r4) { return false; }
#endif // REXGLUE_HAS_XEO3_TARGET
