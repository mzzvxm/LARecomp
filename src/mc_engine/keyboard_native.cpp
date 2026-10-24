// See keyboard_native.h for the design and what every address below is.
#include "keyboard_native.h"

#ifndef REXGLUE_HAS_XEO3_TARGET

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/xmemory.h>
#include <rex/ui/image_decode.h>
#include <rex/ui/keybinds.h>

#include "hooks/hooks_internal.h"
#include "logging.h"
#include "modloader/texture.h"
#include "native_gfx/guest/texture_format.h"
#include "online/server_browser/browser_ui.h"

#if defined(_WIN32)
#include <Windows.h>
#endif

// The SDK's own switch; reading it by name keeps this independent of the SDK's
// link line. With both on, a key is a pad button AND a native key at once.
// (Defined in rex input; queried through the registry.)

REXCVAR_DEFINE_BOOL(keyboard_native, false, "MCLA/Input",
    "Native keyboard. Feeds the keyboard straight into the game's own input layer "
    "(rage ioMapper), which the SDK's keystroke API never filled: menus and driving "
    "get separate key sets, so Enter can be 'accept' in a menu and Pause on the road "
    "without ever being both. Turn mnk_mode OFF while this is on -- mnk_mode makes "
    "keys pad buttons, and Enter would then also be Start. The mouse and the "
    "analogue sticks are not touched.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// ── Menus (the FlashNavigator mapper). Comma separated key names, the same names
// as keybind_* ("Return", "Escape", "Space", "PageUp", "F5", "Semicolon" ...).
// A combination such as Shift+Up counts as its last key.
REXCVAR_DEFINE_STRING(kb_ui_accept, "Return,Space", "MCLA/Input/Keyboard - menus",
    "Menus: A / accept").lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(kb_ui_back, "Escape,Backspace", "MCLA/Input/Keyboard - menus",
    "Menus: B / back").lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(kb_ui_x, "X", "MCLA/Input/Keyboard - menus", "Menus: X")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(kb_ui_y, "Y", "MCLA/Input/Keyboard - menus", "Menus: Y")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(kb_ui_lb, "Q", "MCLA/Input/Keyboard - menus", "Menus: LB (previous tab)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(kb_ui_rb, "E", "MCLA/Input/Keyboard - menus", "Menus: RB (next tab)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(kb_ui_lt, "PageUp", "MCLA/Input/Keyboard - menus", "Menus: LT")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(kb_ui_rt, "PageDown", "MCLA/Input/Keyboard - menus", "Menus: RT")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(kb_ui_up, "Up,W", "MCLA/Input/Keyboard - menus", "Menus: up")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(kb_ui_down, "Down,S", "MCLA/Input/Keyboard - menus", "Menus: down")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(kb_ui_left, "Left,A", "MCLA/Input/Keyboard - menus", "Menus: left")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(kb_ui_right, "Right,D", "MCLA/Input/Keyboard - menus", "Menus: right")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(kb_ui_start, "Return,Escape", "MCLA/Input/Keyboard - menus",
    "Start: opens the pause menu on the road and answers \"PRESS START\" on the title screen. "
    "It is Start only while NO menu is open: with a menu up these keys are accept/back and "
    "nothing else, which is what keeps Enter from being accept and Start at once. A key held "
    "when a menu closes stays off until released, so the Enter that picks CONTINUE cannot "
    "reopen the pause menu.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(kb_ui_select, "Tab", "MCLA/Input/Keyboard - menus",
    "Menus: the Back/Select button (GPS, map). Start has no key here on purpose.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// ── Driving (the player's control-map mapper).
REXCVAR_DEFINE_STRING(kb_drive_steer_left, "A,Left", "MCLA/Input/Keyboard - driving",
    "Driving: steer left (pairs with the right key at the same position)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(kb_drive_steer_right, "D,Right", "MCLA/Input/Keyboard - driving",
    "Driving: steer right")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(kb_drive_gas, "W,Up", "MCLA/Input/Keyboard - driving", "Driving: accelerate")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(kb_drive_brake, "S,Down", "MCLA/Input/Keyboard - driving",
    "Driving: brake / reverse").lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(kb_drive_handbrake, "Space", "MCLA/Input/Keyboard - driving",
    "Driving: handbrake (the pad's A)").lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(kb_drive_weight, "CapsLock", "MCLA/Input/Keyboard - driving",
    "Driving: weight transfer, the 'turn in' of the pad's B")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(kb_drive_nitro, "Shift", "MCLA/Input/Keyboard - driving",
    "Driving: nitro (the pad's X)").lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(kb_drive_headlights, "H", "MCLA/Input/Keyboard - driving",
    "Driving: headlights (the pad's Y)").lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(kb_drive_horn, "G", "MCLA/Input/Keyboard - driving", "Driving: horn")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(kb_drive_camera, "V", "MCLA/Input/Keyboard - driving",
    "Driving: change camera, one step along the game's cycle (the pad's RB)").lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(kb_drive_camera_keys, "1,2,3,4,5", "MCLA/Input/Keyboard - driving",
    "Driving: keys that jump straight to a camera, slot 1..5 (empty = off). The game only "
    "has a button that steps through its cameras; the jump presses it for you, as many "
    "times as the cycle needs, and stops on the one asked for.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(kb_drive_camera_views, "4,0,3,2,1", "MCLA/Input/Keyboard - driving",
    "Driving: which camera each slot above selects, as the game numbers them: 0 bumper, "
    "1 cockpit, 2/3/4 the chase views. Default: slot 1 action(4), 2 bumper(0), 3 classic(3), "
    "4 static(2), 5 cockpit(1). Swap numbers here if 3 and 4 turn out the other way round.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(kb_drive_lookback, "R", "MCLA/Input/Keyboard - driving",
    "Driving: look back").lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(kb_drive_map, "M", "MCLA/Input/Keyboard - driving", "Driving: map")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(kb_drive_talk, "T", "MCLA/Input/Keyboard - driving", "Driving: talk")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(kb_drive_shift_up, "E", "MCLA/Input/Keyboard - driving",
    "Driving: gear up (manual transmission)").lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(kb_drive_shift_down, "Q", "MCLA/Input/Keyboard - driving",
    "Driving: gear down (manual transmission)").lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(kb_drive_pause, "", "MCLA/Input/Keyboard - driving",
    "Driving: the PAUSE action of the control map. Empty by default: the pause menu is opened "
    "by Start in the UI mapper (kb_ui_start), which is where the pad's Start goes as well.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(kb_drive_popup_select, "", "MCLA/Input/Keyboard - driving",
    "Driving: answer a pop-up with A (empty = pad only; Enter would also be Pause)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(kb_drive_popup_back, "", "MCLA/Input/Keyboard - driving",
    "Driving: answer a pop-up with B (empty = pad only)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(keycaps_dir, "", "MCLA/Input",
    "Folder of the 64x64 key cap PNGs the keyboard glyphs are painted from, named by "
    "lower-case key (return.png, escape.png, a.png, capslock.png ...). Empty = the "
    "set embedded in the executable (src/keycaps/). Built by tools/keycaps/pack_keycaps.py.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// button_prompts is owned by hooks/ui.cpp (platform flag); this only reads it.
REXCVAR_DECLARE(std::string, button_prompts);

namespace {

// ── addresses ───────────────────────────────────────────────────────────
constexpr uint32_t kVkToScancode   = 0x82825050;  // byte_82825050[256]
constexpr uint32_t kKeysPending    = 0x82905738;  // byte_82905738[256], the event-fed state
constexpr uint32_t kUiManagerPtr   = 0x8286D804;  // atSingleton<mcUIManager>
constexpr uint32_t kUiNavigatorOff = 52;          // mcUIManager +52 = FlashNavigator
constexpr uint32_t kNavMapperOff   = 263140;      // FlashNavigator +263140 = the UI ioMapper
constexpr uint32_t kUiDictOff      = 13284;       // mcUIManager +13284 = shared_latin dictionary
constexpr uint32_t kActionTable    = 0x8204FF90;  // {u32 id, char* name}[]
constexpr uint32_t kActionTableEnd = 0x82050158;
constexpr uint32_t kRebuildFn      = 0x822A4DE8;  // sub_822A4DE8(controller)
constexpr int kMapperCapacity = 64;               // entries the struct has room for
constexpr uint32_t kNavOriginalCount = 24;
// A scancode no real key produces (byte_82825050 has none): written by hand to press the
// camera button, which is bound to it as well as to the real key.
constexpr uint8_t kVirtualCamKey = 0x7F;

// ioMapper entry types used here.
constexpr uint32_t kTypeKey     = 0;
constexpr uint32_t kTypeKeyAxis = 14;

// The pad bits the game re-shuffles XInput into (sub_82614D30, pad+8), which is what
// the UI mapper's type-6 entries test. Found by value in the live mapper so a layout
// surprise fails to find a button instead of binding the wrong one.
enum UiButton : uint32_t {
    kBtnA = 0x40, kBtnB = 0x20, kBtnX = 0x80, kBtnY = 0x10, kBtnLB = 0x04, kBtnRB = 0x08,
    kBtnLT = 0x01, kBtnRT = 0x02, kBtnStart = 0x800, kBtnBack = 0x100,
    kBtnUp = 0x1000, kBtnDown = 0x4000, kBtnLeft = 0x8000, kBtnRight = 0x2000,
};

// ── small helpers ───────────────────────────────────────────────────────
uint8_t* GuestBase() {
    auto* rt = rex::Runtime::instance();
    return rt ? rt->virtual_membase() : nullptr;
}

bool HeapPtr(uint32_t ea) { return ea >= 0x40000000u && ea < 0xE0000000u; }

#if defined(_WIN32)
bool WindowFocused() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}
bool HostKeyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }
#else
bool WindowFocused() { return false; }
bool HostKeyDown(int) { return false; }
#endif

std::string Trim(std::string s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

struct KeyRef {
    std::string name;  // as written, for the key cap file
    uint8_t vk = 0;
};

// "A,Left" -> [A, Left]. A combination counts as its last key.
std::vector<KeyRef> ParseKeyList(const std::string& list) {
    std::vector<KeyRef> out;
    size_t pos = 0;
    while (pos <= list.size()) {
        size_t comma = list.find(',', pos);
        if (comma == std::string::npos) comma = list.size();
        std::string token = Trim(list.substr(pos, comma - pos));
        pos = comma + 1;
        if (token.empty()) continue;
        const size_t plus = token.rfind('+');
        if (plus != std::string::npos && plus + 1 < token.size()) token = Trim(token.substr(plus + 1));
        const auto vk = rex::ui::ParseVirtualKey(token);
        if (vk == rex::ui::VirtualKey::kNone) continue;
        const int code = static_cast<int>(vk);
        if (code <= 0 || code > 254) continue;
        out.push_back({token, static_cast<uint8_t>(code)});
    }
    return out;
}

// The pending-key and VK tables hold DirectInput scancodes; 0 = the game has no
// key for that VK (mouse buttons, media keys).
uint8_t ScancodeOf(const uint8_t* base, uint8_t vk) { return base[kVkToScancode + vk]; }

// ── mapper access ───────────────────────────────────────────────────────
// {u32 count; u32 default pad; u32 type[64]; u32 value[64]; u32 output[64]}
bool MapperAppend(uint8_t* b, uint32_t m, uint32_t type, uint32_t value, uint32_t out) {
    const uint32_t n = ReadGuestU32(b, m);
    if (n >= static_cast<uint32_t>(kMapperCapacity)) return false;
    WriteGuestU32(b, m + 4 * (2 + n), type);
    WriteGuestU32(b, m + 4 * (66 + n), 0xFF000000u | value);
    WriteGuestU32(b, m + 4 * (130 + n), out);
    WriteGuestU32(b, m, n + 1);  // the count last: the mapper is read on this thread, but be tidy
    return true;
}

// ── action names -> ids ─────────────────────────────────────────────────
std::unordered_map<std::string, uint32_t> g_action_ids;

void LoadActionIds(const uint8_t* b) {
    if (!g_action_ids.empty()) return;
    for (uint32_t p = kActionTable; p < kActionTableEnd; p += 8) {
        const uint32_t id = ReadGuestU32(b, p);
        const uint32_t name = ReadGuestU32(b, p + 4);
        if (name < 0x82000000u || name >= 0x82900000u || id > 255) continue;
        g_action_ids[reinterpret_cast<const char*>(b + name)] = id;
    }
    MC_INFO("[kbnative] {} control actions known", g_action_ids.size());
}

// ── signatures (so the mappers are only rewritten when a binding changed) ──
std::string UiSignature() {
    std::string s;
    for (const std::string* v : {&REXCVAR_GET(kb_ui_accept), &REXCVAR_GET(kb_ui_back),
                                 &REXCVAR_GET(kb_ui_x), &REXCVAR_GET(kb_ui_y),
                                 &REXCVAR_GET(kb_ui_lb), &REXCVAR_GET(kb_ui_rb),
                                 &REXCVAR_GET(kb_ui_lt), &REXCVAR_GET(kb_ui_rt),
                                 &REXCVAR_GET(kb_ui_up), &REXCVAR_GET(kb_ui_down),
                                 &REXCVAR_GET(kb_ui_left), &REXCVAR_GET(kb_ui_right),
                                 &REXCVAR_GET(kb_ui_select), &REXCVAR_GET(kb_ui_start)}) {
        s += *v;
        s += '|';
    }
    return s;
}

struct DriveBinding {
    const char* action;
    std::string& (*cvar)();
};
const DriveBinding kDriveBindings[] = {
    {"GAS", &FLAGS_kb_drive_gas_storage_},
    {"BRAKE", &FLAGS_kb_drive_brake_storage_},
    {"HANDBRAKE", &FLAGS_kb_drive_handbrake_storage_},
    {"WEIGHT_XFER", &FLAGS_kb_drive_weight_storage_},
    {"NITRO", &FLAGS_kb_drive_nitro_storage_},
    {"HEADLIGHTS", &FLAGS_kb_drive_headlights_storage_},
    {"HORN", &FLAGS_kb_drive_horn_storage_},
    {"CAM_UP", &FLAGS_kb_drive_camera_storage_},
    {"LOOKBACK", &FLAGS_kb_drive_lookback_storage_},
    {"MAP", &FLAGS_kb_drive_map_storage_},
    {"TALK", &FLAGS_kb_drive_talk_storage_},
    {"SHIFT_UP", &FLAGS_kb_drive_shift_up_storage_},
    {"SHIFT_DOWN", &FLAGS_kb_drive_shift_down_storage_},
    {"PAUSE", &FLAGS_kb_drive_pause_storage_},
    {"POPUP_SELECT", &FLAGS_kb_drive_popup_select_storage_},
    {"POPUP_BACK", &FLAGS_kb_drive_popup_back_storage_},
};

std::string DriveSignature() {
    std::string s = REXCVAR_GET(kb_drive_steer_left) + "|" + REXCVAR_GET(kb_drive_steer_right) + "|";
    for (const auto& d : kDriveBindings) s += d.cvar() + "|";
    return s;
}

// ── driving mapper ──────────────────────────────────────────────────────
struct DriveStruct {
    uint32_t ea = 0;
    uint32_t mapper = 0;
    uint32_t outs = 0;
    uint32_t base_count = 0;  // entries the game's own rebuild left, before ours
    // Steering. An output keeps the largest value any source gave it (reset to 0 each
    // frame), so a key pair cannot share STEER_AXIS with the pad stick: its centre (0x80)
    // would beat every left value the stick gives, and its "left" (0) never beats the
    // stick's centre. While a steering key is down the pad's own entries for that output
    // are parked (type 13, which contributes nothing) and only the pair speaks; while none
    // is down it is the other way round.
    std::vector<uint32_t> steer_pad;      // the game's entries for STEER_AXIS: indices
    std::vector<uint32_t> steer_pad_type; // ... and the type each had
    std::vector<uint32_t> steer_keys;     // our type-14 entries: indices
    std::vector<uint8_t> steer_scancodes; // [2i], [2i+1] = left/right keys of steer_keys[i]
    int steer_active = -2;                // which pair speaks (-1 = none, the pad does); -2 = unset
};
constexpr int kMaxDriveStructs = 8;
DriveStruct g_drive[kMaxDriveStructs];
std::string g_drive_sig_applied;  // the signature the live mappers carry
// The player's control mapper has been built, i.e. the game proper has started. The title and
// sign-in screens come before it and need keys the game itself never has.
bool g_game_entered = false;
bool g_reapplying = false;

void AppendDriveEntries(uint8_t* b, DriveStruct& d) {
    LoadActionIds(b);
    int added = 0, dropped = 0;
    auto out_of = [&](const char* action, uint32_t& out) {
        auto it = g_action_ids.find(action);
        if (it == g_action_ids.end() || it->second > 73) return false;
        out = d.outs + 12 * it->second;
        return true;
    };
    auto add = [&](uint32_t type, uint32_t value, uint32_t out) {
        if (MapperAppend(b, d.mapper, type, value, out)) ++added; else ++dropped;
    };

    // Steering: left/right keys at the same position make one axis pair.
    uint32_t steer_out = 0;
    if (out_of("STEER_AXIS", steer_out)) {
        const auto left = ParseKeyList(REXCVAR_GET(kb_drive_steer_left));
        const auto right = ParseKeyList(REXCVAR_GET(kb_drive_steer_right));
        const size_t n = std::min(left.size(), right.size());
        d.steer_pad.clear();
        d.steer_pad_type.clear();
        d.steer_keys.clear();
        d.steer_scancodes.clear();
        for (uint32_t i = 0; i < ReadGuestU32(b, d.mapper); ++i) {
            if (ReadGuestU32(b, d.mapper + 4 * (130 + i)) != steer_out) continue;
            d.steer_pad.push_back(i);
            d.steer_pad_type.push_back(ReadGuestU32(b, d.mapper + 4 * (2 + i)));
        }
        for (size_t i = 0; i < n; ++i) {
            const uint8_t l = ScancodeOf(b, left[i].vk), r = ScancodeOf(b, right[i].vk);
            if (!l || !r) continue;
            const uint32_t at = ReadGuestU32(b, d.mapper);
            // Starts parked (type 13): the pair only speaks while a steering key is down.
            if (MapperAppend(b, d.mapper, 13, uint32_t(l) | (uint32_t(r) << 8), steer_out)) {
                d.steer_keys.push_back(at);
                d.steer_scancodes.push_back(l);
                d.steer_scancodes.push_back(r);
                ++added;
            } else {
                ++dropped;
            }
        }
        d.steer_active = -2;
    }
    for (const auto& bind : kDriveBindings) {
        uint32_t out = 0;
        if (!out_of(bind.action, out)) continue;
        for (const KeyRef& k : ParseKeyList(bind.cvar())) {
            const uint8_t sc = ScancodeOf(b, k.vk);
            if (sc) add(kTypeKey, sc, out);
        }
    }
    uint32_t cam_out = 0;
    if (out_of("CAM_UP", cam_out)) add(kTypeKey, kVirtualCamKey, cam_out);
    MC_INFO("[kbnative] driving mapper @0x{:08X}: {} key entries added ({} dropped, now {})",
            d.mapper, added, dropped, ReadGuestU32(b, d.mapper));
}

}  // namespace

// Hook at the return of sub_822A4DE8. The game has just cleared and rebuilt the
// mapper from the control map; the keyboard entries go on the end.
void Hook_ControlMapRebuilt(PPCRegister& r30) {
    if (!REXCVAR_GET(keyboard_native)) return;
    uint8_t* b = GuestBase();
    const uint32_t ea = static_cast<uint32_t>(r30.u64);
    if (!b || !HeapPtr(ea)) return;
    if (ReadGuestU32(b, ea + 4) != 0) return;  // player 0 only
    const uint32_t mapper = ReadGuestU32(b, ea + 8);
    const uint32_t outs = ReadGuestU32(b, ea + 12);
    if (!HeapPtr(mapper) || !HeapPtr(outs)) return;

    DriveStruct* slot = nullptr;
    for (auto& d : g_drive) if (d.ea == ea) { slot = &d; break; }
    if (!slot) for (auto& d : g_drive) if (!d.ea) { slot = &d; break; }
    if (!slot) slot = &g_drive[0];
    slot->ea = ea;
    slot->mapper = mapper;
    slot->outs = outs;
    slot->base_count = ReadGuestU32(b, mapper);
    if (!g_game_entered) {
        g_game_entered = true;
        MC_INFO("[kbnative] game entered: Start now live only while no menu is open");
    }
    AppendDriveEntries(b, *slot);
    g_drive_sig_applied = DriveSignature();
}

namespace {

// ── menus (the FlashNavigator mapper) ───────────────────────────────────
struct UiBind {
    uint32_t button;
    std::string& (*cvar)();
};
const UiBind kUiBinds[] = {
    {kBtnA, &FLAGS_kb_ui_accept_storage_},   {kBtnB, &FLAGS_kb_ui_back_storage_},
    {kBtnX, &FLAGS_kb_ui_x_storage_},        {kBtnY, &FLAGS_kb_ui_y_storage_},
    {kBtnLB, &FLAGS_kb_ui_lb_storage_},      {kBtnRB, &FLAGS_kb_ui_rb_storage_},
    {kBtnLT, &FLAGS_kb_ui_lt_storage_},      {kBtnRT, &FLAGS_kb_ui_rt_storage_},
    {kBtnUp, &FLAGS_kb_ui_up_storage_},      {kBtnDown, &FLAGS_kb_ui_down_storage_},
    {kBtnLeft, &FLAGS_kb_ui_left_storage_},  {kBtnRight, &FLAGS_kb_ui_right_storage_},
    {kBtnBack, &FLAGS_kb_ui_select_storage_},
    {kBtnStart, &FLAGS_kb_ui_start_storage_},
};

uint32_t g_nav_mapper = 0;
uint32_t g_nav_count_after = 0;
std::string g_ui_sig_applied;
struct NavKey {
    uint32_t idx;      // entry index in the UI mapper
    uint8_t sc;        // its scancode
    bool start;        // a Start key (title screen only)
    uint32_t value;    // what the entry currently holds: sc, or 0 = off
};
std::vector<NavKey> g_nav_keys;
bool g_menu_was_open = false;
std::vector<uint8_t> g_disarmed;  // scancodes held when a menu opened, off until released

// The movie registered under `name` in mcUIManager's movie map (+60), 0 if none.
uint32_t FindMovie(const uint8_t* b, const char* name) {
    const uint32_t mgr = ReadGuestU32(b, kUiManagerPtr);
    if (!HeapPtr(mgr)) return 0;
    const uint32_t buckets = ReadGuestU32(b, mgr + 60);
    const uint32_t count = (uint32_t(b[mgr + 64]) << 8) | b[mgr + 65];
    if (!HeapPtr(buckets) || count > 4096) return 0;
    for (uint32_t i = 0; i < count; ++i) {
        for (uint32_t node = ReadGuestU32(b, buckets + 4 * i), guard = 0; node && guard < 256;
             node = ReadGuestU32(b, node + 12), ++guard) {
            const uint32_t key = ReadGuestU32(b, node);
            if (key < 0x10000u || key >= 0xE0000000u) continue;
            if (std::strncmp(reinterpret_cast<const char*>(b + key), name, 32) != 0) continue;
            const uint32_t holder = ReadGuestU32(b, node + 8);
            return HeapPtr(holder) ? ReadGuestU32(b, holder) : 0;
        }
    }
    return 0;
}

// A UI form is open (pause, garage, message box ...): mcMainForm's open-form stack,
// engine (*0x8286D8D4) + 0x13F80, head at +212.
bool MenuOpen(const uint8_t* b) {
    const uint32_t eng = ReadGuestU32(b, 0x8286D8D4);
    if (!HeapPtr(eng)) return false;
    return ReadGuestU32(b, eng + 0x13F80 + 212) != 0;
}

bool KeyDown(const uint8_t* b, uint8_t sc) { return b[kKeysPending + sc] != 0; }

// Which keys are live this frame.
//
//  * Before the game is entered everything is: the title and sign-in screens have to be
//    passable, and Start is a key so "PRESS START" can be answered.
//  * In the game Start (Esc / Enter by default) is live only while no UI form is open --
//    it is what opens the pause menu. The moment a menu is up those same keys are only
//    accept/back, so Enter is never accept AND Start.
//  * A key held when a menu opens or closes stays off for Start until it is released:
//    the Esc that opened the pause menu must not also be "back", and the Enter that picked
//    CONTINUE must not reopen it.
void UpdateNavEntries(uint8_t* b) {
    if (g_nav_mapper == 0 || g_nav_keys.empty()) return;
    const bool open = MenuOpen(b);
    if (open != g_menu_was_open) {
        // A menu opened or closed: keys held right now (the Esc/Enter that did it) must go
        // up before they can be Start again.
        g_disarmed.clear();
        for (const NavKey& k : g_nav_keys)
            if (KeyDown(b, k.sc)) g_disarmed.push_back(k.sc);
        g_menu_was_open = open;
    }
    g_disarmed.erase(std::remove_if(g_disarmed.begin(), g_disarmed.end(),
                                    [&](uint8_t sc) { return !KeyDown(b, sc); }),
                     g_disarmed.end());

    for (NavKey& k : g_nav_keys) {
        bool live = true;
        if (k.start && g_game_entered) {
            live = !open && std::find(g_disarmed.begin(), g_disarmed.end(), k.sc) == g_disarmed.end();
        }
        const uint32_t want = live ? k.sc : 0u;
        if (want == k.value) continue;
        WriteGuestU32(b, g_nav_mapper + 4 * (66 + k.idx), 0xFF000000u | want);
        k.value = want;
    }
}

// The output a pad button of the UI mapper writes to, found by its mask.
uint32_t UiOutputFor(const uint8_t* b, uint32_t mapper, uint32_t mask) {
    const uint32_t n = std::min<uint32_t>(ReadGuestU32(b, mapper), kNavOriginalCount);
    for (uint32_t i = 0; i < n; ++i) {
        if (ReadGuestU32(b, mapper + 4 * (2 + i)) != 6) continue;
        if ((ReadGuestU32(b, mapper + 4 * (66 + i)) & 0xFFFFFF) == mask)
            return ReadGuestU32(b, mapper + 4 * (130 + i));
    }
    return 0;
}

void EnsureNavKeys(uint8_t* b) {
    const uint32_t ui = ReadGuestU32(b, kUiManagerPtr);
    if (!HeapPtr(ui)) return;
    const uint32_t nav = ReadGuestU32(b, ui + kUiNavigatorOff);
    if (!HeapPtr(nav)) return;
    const uint32_t mapper = nav + kNavMapperOff;
    const uint32_t count = ReadGuestU32(b, mapper);
    if (count < kNavOriginalCount || count > static_cast<uint32_t>(kMapperCapacity)) return;

    const std::string sig = UiSignature();
    if (mapper == g_nav_mapper && count == g_nav_count_after && sig == g_ui_sig_applied) return;

    // The constructor's 24 pad entries must still be there before anything is added.
    if (ReadGuestU32(b, mapper + 4 * 2) != 6 ||
        (ReadGuestU32(b, mapper + 4 * 66) & 0xFFFFFF) != 0x1000) {
        return;
    }
    WriteGuestU32(b, mapper, kNavOriginalCount);  // back to the game's own entries
    g_nav_keys.clear();

    int added = 0;
    for (const UiBind& bind : kUiBinds) {
        const uint32_t out = UiOutputFor(b, mapper, bind.button);
        if (!out) continue;
        for (const KeyRef& k : ParseKeyList(bind.cvar())) {
            const uint8_t sc = ScancodeOf(b, k.vk);
            const uint32_t idx = ReadGuestU32(b, mapper);
            if (!sc) continue;
            if (MapperAppend(b, mapper, kTypeKey, sc, out)) {
                ++added;
                g_nav_keys.push_back({idx, sc, bind.button == kBtnStart, sc});
            }
        }
    }
    g_nav_mapper = mapper;
    g_nav_count_after = ReadGuestU32(b, mapper);
    g_ui_sig_applied = sig;
    MC_INFO("[kbnative] menu mapper @0x{:08X}: {} key entries added (now {})", mapper, added,
            g_nav_count_after);
}

// Re-run the game's own rebuild for the controllers we have seen, so an edited
// driving binding applies without waiting for the game to rebuild by itself.
void ReapplyDrive(uint8_t* b) {
    auto* rt = rex::Runtime::instance();
    if (!rt || !rt->function_dispatcher() || g_reapplying) return;
    PPCFunc* fn = rt->function_dispatcher()->GetFunction(kRebuildFn);
    if (!fn) return;
    g_reapplying = true;
    for (auto& d : g_drive) {
        if (!d.ea) continue;
        // Still the same controller? Otherwise it was freed and its address reused.
        if (ReadGuestU32(b, d.ea + 8) != d.mapper || ReadGuestU32(b, d.ea + 12) != d.outs ||
            ReadGuestU32(b, d.ea + 4) != 0) {
            d = DriveStruct{};
            continue;
        }
        rex::ppc::GuestToHostFunction<uint32_t>(fn, d.ea);
    }
    g_reapplying = false;
}

// Called every frame. See DriveStruct for why the pad and the key pair take turns.
//
// Only ONE pair may speak at a time, too: a pair with neither key down answers "centre"
// (0x80), which beats the "left" (0) of the pair that is pressed, so A,D and Left,Right
// cannot both be live. The pair that has a key down is, and the rest are parked.
void UpdateSteering(uint8_t* b) {
    for (auto& d : g_drive) {
        if (!d.ea || d.steer_keys.empty()) continue;
        if (ReadGuestU32(b, d.ea + 8) != d.mapper || ReadGuestU32(b, d.ea + 12) != d.outs) {
            d = DriveStruct{};  // freed and reused
            continue;
        }
        int active = -1;
        for (size_t i = 0; i < d.steer_keys.size() && active < 0; ++i) {
            if (b[kKeysPending + d.steer_scancodes[2 * i]] ||
                b[kKeysPending + d.steer_scancodes[2 * i + 1]])
                active = static_cast<int>(i);
        }
        if (active == d.steer_active) continue;
        for (size_t i = 0; i < d.steer_keys.size(); ++i)
            WriteGuestU32(b, d.mapper + 4 * (2 + d.steer_keys[i]),
                          static_cast<int>(i) == active ? kTypeKeyAxis : 13u);
        for (size_t i = 0; i < d.steer_pad.size(); ++i)
            WriteGuestU32(b, d.mapper + 4 * (2 + d.steer_pad[i]),
                          active >= 0 ? 13u : d.steer_pad_type[i]);
        d.steer_active = active;
    }
}

// ── key state feed ──────────────────────────────────────────────────────
uint8_t g_prev_state[256] = {};
uint8_t g_just_pressed[256] = {};  // scancodes that went down this frame

void FeedKeys(uint8_t* b) {
    // Nothing while the server browser has the keyboard.
    const bool focused = WindowFocused() && !larecomp::ServerBrowserCapturesInput();
    uint8_t state[256] = {};
    uint8_t mapped[256] = {};
    for (int vk = 1; vk < 255; ++vk) {
        const uint8_t sc = b[kVkToScancode + vk];
        if (!sc) continue;
        mapped[sc] = 1;
        if (focused && HostKeyDown(vk)) state[sc] = 0x80;
    }
    for (int sc = 1; sc < 256; ++sc) {
        if (mapped[sc]) b[kKeysPending + sc] = state[sc];
        g_just_pressed[sc] = (state[sc] && !g_prev_state[sc]) ? 1 : 0;
        g_prev_state[sc] = state[sc];
    }
}

// ── direct camera selection ───────────────────────────────────────────
// The player's camera state is *(racer + 880) (racer = slot 2 or 34 of *(0x82874374)),
// whose +80 is the rig sub_822C0320 reports: measured, the button steps it
// 4 -> 3 -> 2 -> 1 -> 0 -> 4 (0 bumper, 1 cockpit, 2/3/4 chase). Writing +80 does nothing -- the
// view does not follow -- so the jump is made the way the player makes it: the camera
// button (CAM_UP) is pressed, through a virtual key, until the rig is the one asked for.
uint32_t CameraControl(const uint8_t* b) {
    const uint32_t list = ReadGuestU32(b, 0x82874374);
    if (!HeapPtr(list)) return 0;
    uint32_t racer = ReadGuestU32(b, list + 8);
    if (!HeapPtr(racer)) racer = ReadGuestU32(b, list + 136);
    if (!HeapPtr(racer)) return 0;
    const uint32_t ctl = ReadGuestU32(b, racer + 880);
    return HeapPtr(ctl) ? ctl : 0;
}

struct CameraJump {
    int target = -1;   // rig wanted, -1 = idle
    int phase = 0;     // frames into the current press/release
    int presses = 0;
    bool was_down[5] = {};
};
CameraJump g_cam;
constexpr int kCamPressFrames = 3, kCamSettleFrames = 8, kCamMaxPresses = 6;

std::vector<int> ParseIntList(const std::string& s) {
    std::vector<int> out;
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t comma = s.find(',', pos);
        if (comma == std::string::npos) comma = s.size();
        const std::string t = Trim(s.substr(pos, comma - pos));
        pos = comma + 1;
        if (!t.empty()) out.push_back(std::atoi(t.c_str()));
    }
    return out;
}

void UpdateCameraJump(uint8_t* b) {
    uint8_t& virt = b[kKeysPending + kVirtualCamKey];
    const auto keys = ParseKeyList(REXCVAR_GET(kb_drive_camera_keys));
    const auto views = ParseIntList(REXCVAR_GET(kb_drive_camera_views));
    const uint32_t ctl = (g_game_entered && !MenuOpen(b)) ? CameraControl(b) : 0;

    // Key edges (host state: these keys have no mapper entry of their own).
    int wanted = -1;
    for (size_t i = 0; i < keys.size() && i < 5; ++i) {
        const bool down = WindowFocused() && !larecomp::ServerBrowserCapturesInput() &&
                          HostKeyDown(keys[i].vk);
        if (down && !g_cam.was_down[i] && i < views.size()) wanted = views[i];
        g_cam.was_down[i] = down;
    }
    if (!ctl) {  // not on the road: nothing to drive, and let go of the button
        g_cam.target = -1;
        virt = 0;
        return;
    }
    if (wanted >= 0) {
        g_cam.target = wanted;
        g_cam.phase = 0;
        g_cam.presses = 0;
    }
    if (g_cam.target < 0) { virt = 0; return; }

    const int rig = static_cast<int>(ReadGuestU32(b, ctl + 80));
    if (g_cam.phase == 0 && rig == g_cam.target) {  // already there, or arrived
        g_cam.target = -1;
        virt = 0;
        return;
    }
    // phase 0..kCamPressFrames-1 held, then released while the rig settles.
    if (g_cam.phase == 0 && g_cam.presses >= kCamMaxPresses) {
        g_cam.target = -1;
        virt = 0;
        return;
    }
    virt = g_cam.phase < kCamPressFrames ? 0x80 : 0;
    if (++g_cam.phase >= kCamPressFrames + kCamSettleFrames) {
        g_cam.phase = 0;
        ++g_cam.presses;
    }
}

// ── glyphs ──────────────────────────────────────────────────────────────
// One entry per UI button whose bitmap(s) carry the Xbox glyph. A glyph can appear in
// several movies under different shared_N, so every copy is written. The round
// Start and Back glyphs are 64x32 bitmaps; the face and shoulder buttons are 64x64.
struct GlyphTarget {
    const char* label;
    std::string& (*cvar)();   // null for a spare slot (see below)
    uint32_t width, height;
    std::vector<const char*> textures;
    int icon = 0;             // iconID the rows of the game use for this glyph (1-based frame)
    int slot = -1;            // spare slot number, -1 = a menu button's own glyph
};
const GlyphTarget kGlyphs[] = {
    {"A", &FLAGS_kb_ui_accept_storage_, 64, 64, {"shared_2.dds", "shared_49.dds"}, 1},
    {"B", &FLAGS_kb_ui_back_storage_, 64, 64, {"shared_45.dds"}, 2},
    {"X", &FLAGS_kb_ui_x_storage_, 64, 64, {"shared_4.dds"}, 3},
    {"Y", &FLAGS_kb_ui_y_storage_, 64, 64, {"shared_42.dds"}, 4},
    {"LB", &FLAGS_kb_ui_lb_storage_, 64, 64, {"shared_10.dds", "shared_41.dds"}, 5},
    {"RB", &FLAGS_kb_ui_rb_storage_, 64, 64, {"shared_1.dds", "shared_12.dds"}, 6},
    {"LT", &FLAGS_kb_ui_lt_storage_, 64, 64, {"shared_43.dds", "shared_48.dds"}, 7},
    {"RT", &FLAGS_kb_ui_rt_storage_, 64, 64, {"shared_18.dds", "shared_37.dds"}, 8},
    // iconID 9 = Start (prompt.xsf: shared_47, pause/popup: shared_33), 10 = Back (31 / 38).
    {"Start", &FLAGS_kb_ui_start_storage_, 64, 32, {"shared_47.dds", "shared_33.dds"}, 9},
    {"Back", &FLAGS_kb_ui_select_storage_, 64, 32, {"shared_31.dds", "shared_38.dds"}, 10},
    // Four spare glyphs for the Custom Controls screen: the PlayStation cross, circle, square
    // and triangle (iconID 50..53, 32x32). With button_prompts = keyboard platform stays 0 and
    // nothing shows them, so they are free to carry the cap of whichever other key a row is
    // bound to: the screen draws a binding as "iconID" text, so a key with no glyph of its
    // own gets one of these.
    // The Sidekick phone (calls to a race) draws its own "accept" button: a gray 64x64 Back glyph
    // embedded in sidekick.xsf (character 106, the same texture is character 646), not one of
    // the shared dictionary's. The phone accepts with the Back button, so it shows that key.
    {"Phone", &FLAGS_kb_ui_select_storage_, 64, 64, {"sidekick:106"}, 0},
    {"Slot0", nullptr, 32, 32, {"shared_24.dds"}, 50, 0},
    {"Slot1", nullptr, 32, 32, {"shared_13.dds"}, 51, 1},
    {"Slot2", nullptr, 32, 32, {"shared_15.dds"}, 52, 2},
    {"Slot3", nullptr, 32, 32, {"shared_36.dds"}, 53, 3},
};
constexpr int kNumSlots = 4;
std::string g_slot_key[kNumSlots];  // key name painted on each spare glyph, "" = free

struct GlyphTexture {
    uint32_t base = 0;                 // guest address of the pixel data
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> original;     // what the game shipped, to put back
    std::string painted;               // key cap currently on it ("" = the original)
};
std::unordered_map<std::string, GlyphTexture> g_glyph_tex;  // by "shared_N.dds"
uint32_t g_glyph_dict = 0;
std::unordered_map<std::string, std::vector<uint8_t>> g_cap_cache;  // "key@WxH" -> tiled bytes
std::string g_glyph_sig_applied;
uint32_t g_glyph_tick = 0;

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// <key>.png for 64x64 glyphs, <key>_w.png for the 64x32 ones.
std::string CapStem(const std::string& key, uint32_t width, uint32_t height) {
    return Lower(key) + (width == 64 && height == 32 ? "_w" : "");
}

// The key caps are embedded in the exe (src/keycaps/, RCDATA KEYCAP_<STEM>); keycaps_dir
// replaces them with a folder of loose PNGs.
bool ReadCapBytes(const std::string& key, uint32_t width, uint32_t height,
                  std::vector<uint8_t>* bytes) {
    const std::string stem = CapStem(key, width, height);
    const std::string dir = REXCVAR_GET(keycaps_dir);
    if (!dir.empty()) {
        const std::filesystem::path path = std::filesystem::path(dir) / (stem + ".png");
        if (!bytes) return std::filesystem::exists(path);
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        bytes->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        return true;
    }
#if defined(_WIN32)
    std::string name = "KEYCAP_" + stem;
    for (char& c : name) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    HRSRC res = FindResourceA(nullptr, name.c_str(), RT_RCDATA);
    if (!res) return false;
    if (!bytes) return true;
    HGLOBAL handle = LoadResource(nullptr, res);
    const DWORD size = SizeofResource(nullptr, res);
    const uint8_t* data = handle ? static_cast<const uint8_t*>(LockResource(handle)) : nullptr;
    if (!data || !size) return false;
    bytes->assign(data, data + size);
    return true;
#else
    return false;
#endif
}

bool CapExists(const std::string& key, uint32_t width, uint32_t height) {
    return ReadCapBytes(key, width, height, nullptr);
}

// Reads the key cap into a width x height RGBA image; empty when it is not there.
mc::modloader::Image LoadKeycap(const std::string& key, uint32_t width, uint32_t height) {
    mc::modloader::Image out;
    std::vector<uint8_t> bytes;
    if (!ReadCapBytes(key, width, height, &bytes)) return out;
    int w = 0, h = 0;
    std::vector<uint8_t> rgba = rex::ui::DecodeImageRGBA(bytes.data(), bytes.size(), w, h);
    if (rgba.empty() || w <= 0 || h <= 0) return out;
    mc::modloader::Image src;
    src.rgba = std::move(rgba);
    src.width = static_cast<uint32_t>(w);
    src.height = static_cast<uint32_t>(h);
    if (src.width == width && src.height == height) return src;
    mc::modloader::ResizeImage(src, width, height, out);
    return out;
}

// The tiled, swapped bytes of a key cap, cached. An unknown key falls back to the
// blank cap; an empty result means not even that exists.
const std::vector<uint8_t>& TiledCap(const std::string& key, uint32_t width, uint32_t height) {
    const std::string id = key + "@" + std::to_string(width) + "x" + std::to_string(height);
    auto it = g_cap_cache.find(id);
    if (it != g_cap_cache.end()) return it->second;
    mc::modloader::Image img = LoadKeycap(key, width, height);
    if (img.empty()) img = LoadKeycap("blank", width, height);
    std::vector<uint8_t> tiled;
    if (!img.empty()) {
        std::vector<uint8_t> linear;
        mc::modloader::CompressBlocks(img, mc::modloader::BlockFormat::kBc3, linear);
        mc::modloader::TileXbox360(linear, width, height, mc::modloader::BlockFormat::kBc3, tiled);
    }
    return g_cap_cache.emplace(id, std::move(tiled)).first->second;
}

// A small texture's blocks are NOT a contiguous 8 KB: the surface is tiled into a 128-wide
// virtual image, so the 16x16 blocks of a 64x64 glyph are scattered over ~14 KB, and the
// xtd packer puts other (smaller) textures in the holes between them. TileXbox360 returns
// the whole 16 KB virtual image with the padding filled in -- writing that over the guest
// allocation would stamp other textures. So only the blocks of the picture itself, at the
// offsets the GPU reads them from, are ever touched.
template <typename Fn>
void ForEachBlock(uint32_t width, uint32_t height, Fn&& fn) {
    const uint32_t wb = (width + 3) / 4, hb = (height + 3) / 4;
    uint32_t index = 0;
    for (uint32_t y = 0; y < hb; ++y)
        for (uint32_t x = 0; x < wb; ++x)
            fn(index++, mcla::native_gfx::TiledOffset2D(x, y, wb, 16));
}


// The Sidekick's own embedded Back glyph. The movie is resolved by name in the UI manager's
// movie map (+60) like everywhere else; character 106 of its swfFILE is a bitmap whose +12 is a
// grcTexture embedded in the movie's own memory (vtable 0x8200A45C, header at +0x1C).
bool g_sidekick_ok = false;
constexpr uint32_t kGrcTextureVtable = 0x8200A45C;
constexpr uint32_t kSidekickChar = 106;

uint32_t FindMovieByName(const uint8_t* b, const char* name) {
    const uint32_t mgr = ReadGuestU32(b, kUiManagerPtr);
    if (!HeapPtr(mgr)) return 0;
    const uint32_t buckets = ReadGuestU32(b, mgr + 60);
    const uint32_t count = (uint32_t(b[mgr + 64]) << 8) | b[mgr + 65];
    if (!HeapPtr(buckets) || count > 4096) return 0;
    for (uint32_t i = 0; i < count; ++i) {
        for (uint32_t node = ReadGuestU32(b, buckets + 4 * i), guard = 0; node && guard < 256;
             node = ReadGuestU32(b, node + 12), ++guard) {
            const uint32_t key = ReadGuestU32(b, node);
            if (key < 0x10000u || key >= 0xE0000000u) continue;
            if (std::strncmp(reinterpret_cast<const char*>(b + key), name, 32) != 0) continue;
            const uint32_t holder = ReadGuestU32(b, node + 8);
            return HeapPtr(holder) ? ReadGuestU32(b, holder) : 0;
        }
    }
    return 0;
}

bool ScanSidekick(uint8_t* b) {
    const uint32_t movie = FindMovieByName(b, "SIDEKICKMOVIE");
    if (!HeapPtr(movie)) return false;
    const uint32_t ctx = ReadGuestU32(b, movie + 56);
    if (!HeapPtr(ctx)) return false;
    const uint32_t swf = ReadGuestU32(b, ctx + 28);
    if (!HeapPtr(swf)) return false;
    const uint32_t tab = ReadGuestU32(b, swf + 24);
    const uint32_t count = (uint32_t(b[swf + 28]) << 8) | b[swf + 29];
    if (!HeapPtr(tab) || count <= kSidekickChar) return false;
    const uint32_t ch = ReadGuestU32(b, tab + 4 * kSidekickChar);
    if (!HeapPtr(ch) || b[ch + 8] != 4) return false;
    const uint32_t tex = ReadGuestU32(b, ch + 12);
    if (!HeapPtr(tex) || ReadGuestU32(b, tex) != kGrcTextureVtable) return false;
    const uint32_t w = (uint32_t(b[tex + 0x20]) << 8) | b[tex + 0x21];
    const uint32_t h = (uint32_t(b[tex + 0x22]) << 8) | b[tex + 0x23];
    const uint32_t hdr = ReadGuestU32(b, tex + 0x1C);
    if (w != 64 || h != 64 || !HeapPtr(hdr)) return false;
    const uint32_t fc1 = ReadGuestU32(b, hdr + 0x20);
    const uint32_t base = fc1 & 0xFFFFF000;
    if ((fc1 & 0x3F) != 20 || ((fc1 >> 6) & 3) != 1 || !HeapPtr(base)) return false;
    GlyphTexture t;
    t.base = base;
    t.width = w;
    t.height = h;
    t.original.resize(size_t((w / 4) * (h / 4) * 16));
    ForEachBlock(w, h, [&](uint32_t i, uint32_t off) {
        std::memcpy(t.original.data() + size_t(i) * 16, b + base + off, 16);
    });
    g_glyph_tex["sidekick:106"] = std::move(t);
    g_sidekick_ok = true;
    MC_INFO("[kbnative] sidekick accept glyph @0x{:08X}", base);
    return true;
}

// Finds the shared_latin dictionary's textures by name and checks each really is a
// DXT5 tiled/endian-1 surface of the size expected before it is ever written, and
// that it has the block layout the writer assumes.
bool ScanDictionary(uint8_t* b) {
    const uint32_t ui = ReadGuestU32(b, kUiManagerPtr);
    if (!HeapPtr(ui)) return false;
    const uint32_t dict = ReadGuestU32(b, ui + kUiDictOff);
    if (!HeapPtr(dict)) return false;
    if (dict == g_glyph_dict && !g_glyph_tex.empty()) return true;

    const uint32_t vals = ReadGuestU32(b, dict + 0x18);
    const uint32_t count = (uint32_t(b[dict + 0x1C]) << 8) | b[dict + 0x1D];
    if (!HeapPtr(vals) || count == 0 || count > 512) return false;

    g_glyph_tex.clear();
    g_sidekick_ok = false;
    for (const GlyphTarget& g : kGlyphs) {
        for (const char* tex_name : g.textures) {
            for (uint32_t i = 0; i < count; ++i) {
                const uint32_t tex = ReadGuestU32(b, vals + 4 * i);
                if (!HeapPtr(tex)) continue;
                const uint32_t name_ptr = ReadGuestU32(b, tex + 0x18);
                if (!HeapPtr(name_ptr) && name_ptr < 0x82000000u) continue;
                if (std::strncmp(reinterpret_cast<const char*>(b + name_ptr), tex_name, 24) != 0) continue;
                const uint32_t w = (uint32_t(b[tex + 0x20]) << 8) | b[tex + 0x21];
                const uint32_t h = (uint32_t(b[tex + 0x22]) << 8) | b[tex + 0x23];
                const uint32_t hdr = ReadGuestU32(b, tex + 0x1C);
                if (w != g.width || h != g.height || !HeapPtr(hdr)) {
                    MC_WARN("[kbnative] {} is {}x{} / header 0x{:08X}, expected {}x{}: skipped",
                            tex_name, w, h, hdr, g.width, g.height);
                    break;
                }
                const uint32_t fc1 = ReadGuestU32(b, hdr + 0x20);  // fetch constant dword 1
                const uint32_t fmt = fc1 & 0x3F, endian = (fc1 >> 6) & 3;
                const uint32_t base = fc1 & 0xFFFFF000;
                if (fmt != 20 || endian != 1 || !HeapPtr(base)) {
                    MC_WARN("[kbnative] {}: format {} endian {} base 0x{:08X}, expected DXT5/8-in-16: "
                            "skipped", tex_name, fmt, endian, base);
                    break;
                }
                GlyphTexture t;
                t.base = base;
                t.width = w;
                t.height = h;
                t.original.resize(size_t((w / 4) * (h / 4) * 16));
                ForEachBlock(w, h, [&](uint32_t i, uint32_t off) {
                    std::memcpy(t.original.data() + size_t(i) * 16, b + base + off, 16);
                });
                MC_INFO("[kbnative] {} base 0x{:08X} {}x{}", tex_name, base, w, h);
                g_glyph_tex[tex_name] = std::move(t);
                break;
            }
        }
    }
    g_glyph_dict = dict;
    g_glyph_sig_applied.clear();
    MC_INFO("[kbnative] glyph dictionary @0x{:08X}: {} textures found", dict, g_glyph_tex.size());
    return !g_glyph_tex.empty();
}

void PaintGlyph(uint8_t* b, const char* tex_name, const std::string& key) {
    auto it = g_glyph_tex.find(tex_name);
    if (it == g_glyph_tex.end()) return;
    GlyphTexture& t = it->second;
    if (t.painted == key) return;
    if (key.empty()) {
        ForEachBlock(t.width, t.height, [&](uint32_t i, uint32_t off) {
            std::memcpy(b + t.base + off, t.original.data() + size_t(i) * 16, 16);
        });
    } else {
        const std::vector<uint8_t>& cap = TiledCap(key, t.width, t.height);
        if (cap.empty()) return;
        bool ok = true;
        ForEachBlock(t.width, t.height, [&](uint32_t, uint32_t off) { ok = ok && off + 16 <= cap.size(); });
        if (!ok) {
            MC_WARN("[kbnative] {}: the tiled key cap is shorter than the surface, left alone", tex_name);
            return;
        }
        ForEachBlock(t.width, t.height, [&](uint32_t, uint32_t off) {
            std::memcpy(b + t.base + off, cap.data() + off, 16);
        });
    }
    t.painted = key;
}

// Which key a glyph shows: the first key of its binding that has a key cap, else
// the first key (drawn as the blank cap), else nothing (the Xbox art stays).
std::string GlyphKeyFor(const GlyphTarget& g) {
    if (!g.cvar) return g.slot >= 0 ? g_slot_key[g.slot] : std::string();
    const auto keys = ParseKeyList(g.cvar());
    for (const KeyRef& k : keys) {
        if (CapExists(k.name, g.width, g.height)) return k.name;
    }
    return keys.empty() ? std::string() : keys.front().name;
}

void TickGlyphs(uint8_t* b) {
    const bool want = REXCVAR_GET(button_prompts) == "keyboard";
    if (!want && g_glyph_tex.empty()) return;  // never painted, nothing to restore
    if (!ScanDictionary(b)) return;
    if (!g_sidekick_ok && ScanSidekick(b)) g_glyph_sig_applied.clear();

    std::string sig = want ? "keyboard|" : "off|";
    if (want) sig += UiSignature() + REXCVAR_GET(keycaps_dir);
    if (sig == g_glyph_sig_applied) return;

    for (const GlyphTarget& g : kGlyphs) {
        const std::string key = want ? GlyphKeyFor(g) : std::string();
        for (const char* tex_name : g.textures) PaintGlyph(b, tex_name, key);
    }
    g_glyph_sig_applied = sig;
    MC_INFO("[kbnative] glyphs -> {}", want ? "keyboard key caps" : "original art");
}

bool g_warned_mnk = false;

}  // namespace


namespace {

// Before the game is entered the screens (title "PRESS START", the sign-in and
// profile steps) read the pad's raw bits (unk_82905388 + 64 * pad, +8), not the UI
// mapper -- 36 places read that struct directly. Until the HUD movie first exists the
// menu keys are also OR'd into pad 0's bits, right after sub_82614D30 builds them, so
// those screens can be passed with the keyboard. Once the game is entered this stops
// and the two mappers alone decide what a key means.
struct PadKey {
    uint32_t mask;
    std::vector<uint8_t> scancodes;
};
std::vector<PadKey> g_padkeys;
std::string g_padkeys_sig;

void RebuildPadKeys(const uint8_t* b) {
    g_padkeys.clear();
    for (const UiBind& bind : kUiBinds) {
        if (bind.button == kBtnLT || bind.button == kBtnRT) continue;  // analogue bytes, not bits
        PadKey pk{bind.button, {}};
        for (const KeyRef& k : ParseKeyList(bind.cvar())) {
            const uint8_t sc = ScancodeOf(b, k.vk);
            if (sc) pk.scancodes.push_back(sc);
        }
        if (!pk.scancodes.empty()) g_padkeys.push_back(std::move(pk));
    }
}

}  // namespace

// Hook at loc_82615048 of sub_82614D30; r31 is the pad being built.
void Hook_PadBitsFinalize(PPCRegister& r31) {
    if (g_game_entered || !REXCVAR_GET(keyboard_native)) return;
    uint8_t* b = GuestBase();
    const uint32_t pad = static_cast<uint32_t>(r31.u64);
    if (!b || pad != 0x82905388u) return;  // pad 0 only
    const std::string sig = UiSignature();
    if (sig != g_padkeys_sig) {
        g_padkeys_sig = sig;
        RebuildPadKeys(b);
    }
    uint32_t bits = ReadGuestU32(b, pad + 8);
    for (const PadKey& pk : g_padkeys) {
        for (uint8_t sc : pk.scancodes) {
            if (b[kKeysPending + sc]) { bits |= pk.mask; break; }
        }
    }
    WriteGuestU32(b, pad + 8, bits);
}


// "Custom buttons" (Options > Controller > Customize): pressing accept starts a capture, and
// from then on mcControllerCustomizeForm swallows every key and polls sub_82727010 each
// frame for "which pad button was pressed", which answers (type, value) from the raw pad
// state only. With the keyboard as the only device that never happens: the screen sits on
// "press a button" for ever and nothing, Esc included, gets out of it.
//
// This answers it from the keyboard: the key that just went down comes back as a
// type-0 source (value = scancode), the same thing the mappers above are built from.
// sub_82726870 then stores it in the control map like any pad button -- except where
// it refuses (the steering axis wants an axis; Esc, scancode 1, returns "accepted, nothing
// changed", which is also how the player backs out).
//
// Placed after the prologue of sub_82727010 (0x82727044: r28 = out type, r24 = out value
// pointer), and finishing through its own "store r11 to *r24, return 1" tail at 0x8272737C.
bool Hook_ControlCaptureKey(PPCRegister& r11, PPCRegister& r28) {
    if (!REXCVAR_GET(keyboard_native)) return false;
    uint8_t* b = GuestBase();
    const uint32_t out_type = static_cast<uint32_t>(r28.u64);
    if (!b || !HeapPtr(out_type)) return false;
    for (int sc = 1; sc < 256; ++sc) {
        if (!g_just_pressed[sc]) continue;
        WriteGuestU32(b, out_type, 0);  // type 0 = keyboard key
        r11.u64 = static_cast<uint64_t>(sc);
        MC_INFO("[kbnative] custom controls: captured scancode 0x{:02X}", sc);
        return true;
    }
    return false;
}


// ── Custom Controls: how a keyboard binding is drawn ────────────────────
// sub_82679678(type, value, &isIcon) names a binding for the row: pad buttons come back as a
// decimal iconID the row draws as a glyph ("17"), analogue directions as text. A keyboard
// binding (type 0) came back empty, so the row showed nothing. Now it is answered with
//  * the iconID of the menu-button glyph that already shows that very key (E is the RB glyph,
//    Q the LB one ...), else
//  * the iconID of a spare glyph painted with that key cap, else
//  * the name of the key as text.
// The strings live in a small block of guest memory allocated on first use.
uint32_t g_label_block = 0;
constexpr uint32_t kLabelText = 512;   // text names at +512 + 16 * scancode

uint32_t LabelBlock(uint8_t* b) {
    if (g_label_block) return g_label_block;
    auto* rt = rex::Runtime::instance();
    if (!rt || !rt->memory()) return 0;
    const uint32_t size = kLabelText + 16 * 256;
    const uint32_t block = rt->memory()->SystemHeapAlloc(size, 0x1000);
    if (!block) return 0;
    std::memset(b + block, 0, size);
    for (int icon = 1; icon < 64; ++icon)
        std::snprintf(reinterpret_cast<char*>(b + block + 8 * icon), 8, "%d", icon);
    g_label_block = block;
    return block;
}

// The name of the key a scancode belongs to ("" when none).
std::string KeyNameOfScancode(const uint8_t* b, uint8_t sc) {
    for (int vk = 1; vk < 255; ++vk) {
        if (b[kVkToScancode + vk] != sc) continue;
        const std::string n = rex::ui::VirtualKeyToString(static_cast<rex::ui::VirtualKey>(vk));
        if (!n.empty()) return n;
    }
    return {};
}

uint8_t ScancodeOfName(const uint8_t* b, const std::string& name) {
    for (const KeyRef& k : ParseKeyList(name)) {
        const uint8_t sc = ScancodeOf(b, k.vk);
        if (sc) return sc;
    }
    return 0;
}

bool Hook_ControlLabel(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5) {
    if (!REXCVAR_GET(keyboard_native)) return false;
    if (static_cast<uint32_t>(r3.u64) != 0) return false;  // pad / axis: the own labels of the game
    const uint32_t key = static_cast<uint32_t>(r4.u64);
    uint8_t* b = GuestBase();
    if (!b || key == 0 || key > 255) return false;
    const uint32_t block = LabelBlock(b);
    if (!block) return false;
    const uint32_t flag = static_cast<uint32_t>(r5.u64);

    if (REXCVAR_GET(button_prompts) == "keyboard") {
        // 1. A menu-button glyph already showing this key.
        for (const GlyphTarget& g : kGlyphs) {
            if (g.slot >= 0 || !g.icon) continue;
            auto it = g_glyph_tex.find(g.textures[0]);
            if (it == g_glyph_tex.end() || it->second.painted.empty()) continue;
            if (ScancodeOfName(b, it->second.painted) != key) continue;
            if (HeapPtr(flag)) b[flag] = 1;
            r3.u64 = block + 8 * g.icon;
            return true;
        }
        // 2. A spare glyph: the one this key already has, else the first free one.
        int slot = -1;
        for (int i = 0; i < kNumSlots && slot < 0; ++i)
            if (!g_slot_key[i].empty() && ScancodeOfName(b, g_slot_key[i]) == key) slot = i;
        for (int i = 0; i < kNumSlots && slot < 0; ++i) {
            if (!g_slot_key[i].empty()) continue;
            const std::string name = KeyNameOfScancode(b, static_cast<uint8_t>(key));
            if (name.empty()) break;
            g_slot_key[i] = name;
            for (const GlyphTarget& g : kGlyphs)
                if (g.slot == i)
                    for (const char* tex : g.textures) PaintGlyph(b, tex, name);
            slot = i;
        }
        if (slot >= 0) {
            if (HeapPtr(flag)) b[flag] = 1;
            r3.u64 = block + 8 * (50 + slot);
            return true;
        }
    }
    // 3. The name of the key.
    const std::string name = KeyNameOfScancode(b, static_cast<uint8_t>(key));
    char* text = reinterpret_cast<char*>(b + block + kLabelText + 16 * key);
    std::snprintf(text, 16, "%s", name.empty() ? "?" : name.c_str());
    if (HeapPtr(flag)) b[flag] = 0;
    r3.u64 = block + kLabelText + 16 * key;
    return true;
}

void Hook_KeyboardFeed() {
    uint8_t* b = GuestBase();
    if (!b) return;

    // Glyphs are independent of the native keyboard (a pad player can still ask for
    // key caps), so they tick on their own, a few times a second.
    if ((++g_glyph_tick & 0x1F) == 0) TickGlyphs(b);

    if (!REXCVAR_GET(keyboard_native)) return;
    if (!g_warned_mnk && REXCVAR_QUERY(bool, mnk_mode)) {
        g_warned_mnk = true;
        MC_WARN("[kbnative] mnk_mode is ON: its keys are also pad buttons, so Enter will be "
                "Start as well as accept. Turn mnk_mode off for the native keyboard alone.");
    }
    FeedKeys(b);
    EnsureNavKeys(b);
    UpdateNavEntries(b);
    UpdateSteering(b);
    UpdateCameraJump(b);
    if (!g_drive_sig_applied.empty() && DriveSignature() != g_drive_sig_applied) {
        g_drive_sig_applied = DriveSignature();  // before: the rebuild below refreshes it again
        ReapplyDrive(b);
    }
}

#else  // REXGLUE_HAS_XEO3_TARGET

void Hook_KeyboardFeed() {}
void Hook_ControlMapRebuilt(PPCRegister&) {}
void Hook_PadBitsFinalize(PPCRegister&) {}
bool Hook_ControlCaptureKey(PPCRegister&, PPCRegister&) { return false; }
bool Hook_ControlLabel(PPCRegister&, PPCRegister&, PPCRegister&) { return false; }

#endif  // REXGLUE_HAS_XEO3_TARGET
