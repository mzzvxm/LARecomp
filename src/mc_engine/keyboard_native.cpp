// See keyboard_native.h for the design and what every address below is.
#include "keyboard_native.h"
#include "ultrawide.h"

#ifndef REXGLUE_HAS_XEO3_TARGET

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "settings/larecomp_settings.h"
#include <rex/platform/file.h>
#include <rex/hid/input_system.h>
#include <rex/ppc/call.h>
#include <rex/ppc/recompiled_code.h>
#include <rex/ppc/context.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/function_dispatcher.h>
#include <rex/kernel/guest_memory.h>
#include <rex/ui/image_decode.h>
#include <rex/ui/ui_system.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/window.h>

#include "imgui.h"
#include "hooks/hooks_internal.h"
#include "input_state.h"
#include "kbm_input.h"
#include "logging.h"
#include "modloader/texture.h"
#include "mouse_look_core.h"
#include "prompt_glyphs.h"
#include "native_gfx/guest/texture_format.h"
#include "online/server_browser/browser_ui.h"
#include "wheel/wheel_input.h"

#if defined(_WIN32)
#include <Windows.h>
#endif

// The SDK's own old keyboard-and-mouse mode (mnk_mode, mnk_mouse, keybind_*) is retired as a player path: LARecomp forces it off at
// start (larecomp_app.h) and Hook_KeyboardFeed puts it back off if anything turns it on later, so a key can never be a pad button AND a
// native key at once (Enter as Start and accept).





// button_prompts and prompts_live are owned by hooks/ui.cpp (the platform flag); the glyph family itself comes from the input state (input_state.h, which folds button_prompts in).


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

// Scancodes of the four extra pad buttons: none of them is in byte_82825050, so no real key
// produces them (checked at run time too). They travel the same way a key does -- the pending
// table, a type-0 mapper source -- which is how Custom Controls can capture and bind them.
constexpr int kNumExtras = 4;
constexpr uint8_t kExtraScancode[kNumExtras] = {0x7B, 0x7C, 0x7D, 0x7E};
constexpr const char* kExtraName[kNumExtras] = {"L4", "L5", "R4", "R5"};

bool ExtrasEnabled();  // defined with the extra-button feed below

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

// ── keys and the mouse: window events -> one latch ──────────────────────
// Nothing here asks the OS what is pressed (no GetAsyncKeyState, no foreground-window call): the window's key and mouse events fill a
// larecomp::kbm::KeyLatch on the UI thread, and the input poll reads it once per frame. That is the same on Windows, Linux and the
// Steam Deck, and a key tapped between two polls still reaches the game (kbm_input.h).
larecomp::kbm::KeyLatch g_keys;

// The mouse is over one of the ImGui overlays (F4, the browsers): a click or a wheel turn there is not the game's.
bool MouseOverOverlay() { return ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureMouse; }

// Layer 0 like the pointer's and the map's listeners (cursor_hide.cpp, map_mouse.cpp): it never marks an event handled, and what an
// overlay or a hotkey (F4 ...) handles first never reaches it.
class KeyEventListener final : public rex::ui::WindowInputListener, public rex::ui::WindowListener {
public:
    void OnKeyDown(rex::ui::KeyEvent& e) override { Key(e, true); }
    void OnKeyUp(rex::ui::KeyEvent& e) override { Key(e, false); }
    void OnMouseDown(rex::ui::MouseEvent& e) override { Button(e, true); }
    void OnMouseUp(rex::ui::MouseEvent& e) override { Button(e, false); }
    void OnMouseWheel(rex::ui::MouseEvent& e) override {
        const int32_t turn = e.scroll_y();
        if (turn == 0 || MouseOverOverlay()) return;
        g_keys.Pulse(turn > 0 ? larecomp::kbm::kKeyWheelUp : larecomp::kbm::kKeyWheelDown);
    }
    // Nothing stays down while the window is not the player's: SDL ends a held key's events at the focus loss, and a key released
    // elsewhere would never be heard.
    void OnLostFocus(rex::ui::UISetupEvent&) override { g_keys.ReleaseAll(); }

private:
    static void Key(rex::ui::KeyEvent& e, bool down) {
        const int id = static_cast<int>(e.virtual_key());
        if (id <= 0 || id > 254) return;
        if (down) g_keys.Down(id, e.prev_state());
        else g_keys.Up(id);
    }
    static void Button(rex::ui::MouseEvent& e, bool down) {
        int id = 0;
        switch (e.button()) {
            case rex::ui::MouseEvent::Button::kLeft: id = 0x01; break;
            case rex::ui::MouseEvent::Button::kRight: id = 0x02; break;
            case rex::ui::MouseEvent::Button::kMiddle: id = 0x04; break;
            case rex::ui::MouseEvent::Button::kX1: id = 0x05; break;
            case rex::ui::MouseEvent::Button::kX2: id = 0x06; break;
            default: return;
        }
        // A click on an overlay is not a game press; its release always lets go.
        if (down) {
            if (!MouseOverOverlay()) g_keys.Down(id, false);
        } else {
            g_keys.Up(id);
        }
    }
};

KeyEventListener g_key_listener;
std::atomic<bool> g_listener_posted{false};
std::atomic<bool> g_listener_attached{false};

// Called from every input poll (game thread): attaches the listener once the window exists. Added on the UI thread, which is the thread that
// walks the window's listener lists.
void EnsureKeyListener() {
    if (g_listener_attached.load(std::memory_order_relaxed)) return;
    auto* rt = rex::Runtime::instance();
    auto* isys = rt ? static_cast<rex::input::InputSystem*>(rt->input_system()) : nullptr;
    rex::ui::Window* window = isys ? isys->window() : nullptr;
    if (!window || g_listener_posted.exchange(true)) return;
    const bool queued = window->app_context().CallInUIThreadDeferred([window]() {
        window->AddInputListener(&g_key_listener, 0);
        window->AddListener(&g_key_listener);
        g_listener_attached.store(true, std::memory_order_relaxed);
        MC_INFO("[kbnative] always on (kbm-always-on-v1): keys and the mouse buttons come from the window's events");
    });
    if (!queued) g_listener_posted.store(false);
}

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
        // The mouse buttons and the wheel are keys here (LMB, RMB, MMB, MB4, MB5, WheelUp, WheelDown): their ids are in kbm_input.h.
        if (const larecomp::kbm::MouseKey* mouse = larecomp::kbm::FindMouseKeyByName(token)) {
            out.push_back({token, static_cast<uint8_t>(mouse->id)});
            continue;
        }
        const auto vk = rex::ui::ParseVirtualKey(token);
        if (vk == rex::ui::VirtualKey::kNone) continue;
        const int code = static_cast<int>(vk);
        if (code <= 0 || code > 254) continue;
        out.push_back({token, static_cast<uint8_t>(code)});
    }
    return out;
}

// The pending-key and VK tables hold DirectInput scancodes; 0 = the game has no
// key for that VK (media keys). The mouse buttons and the wheel have none in the
// game's table either: they get scancodes of their own (kbm_input.h, 0x80..0x86).
uint8_t ScancodeOf(const uint8_t* base, uint8_t vk) {
    if (const larecomp::kbm::MouseKey* mouse = larecomp::kbm::FindMouseKeyById(vk)) return mouse->scancode;
    return base[kVkToScancode + vk];
}

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

// ── did a binding change? ───────────────────────────────────────────────
// The mappers are only rewritten when a binding changed. The cvars tell: a change callback on each of them bumps one generation number, and
// every user compares the number it last applied with it (one atomic load a frame; before, each frame built the strings of all 36 cvars and
// compared them).
std::atomic<uint32_t> g_bindings_gen{1};
// The settings that only decide what the prompts draw (not what a key does): their own number, so that changing one does not rebuild the mappers.
std::atomic<uint32_t> g_prompt_settings_gen{1};

void WatchBindings() {
    static const char* const kNames[] = {
        "kb_ui_accept", "kb_ui_back", "kb_ui_x", "kb_ui_y", "kb_ui_lb", "kb_ui_rb", "kb_ui_lt", "kb_ui_rt", "kb_ui_up",
        "kb_ui_down", "kb_ui_left", "kb_ui_right", "kb_ui_start", "kb_ui_select",
        "kb_drive_steer_left", "kb_drive_steer_right", "kb_drive_gas", "kb_drive_brake", "kb_drive_handbrake", "kb_drive_weight",
        "kb_drive_nitro", "kb_drive_headlights", "kb_drive_horn", "kb_drive_camera", "kb_drive_camera_keys", "kb_drive_camera_views",
        "kb_drive_lookback", "kb_drive_map", "kb_drive_talk", "kb_drive_shift_up", "kb_drive_shift_down", "kb_drive_pause",
        "kb_drive_popup_select", "kb_drive_popup_back", "kb_steer_ramp",
        "extra_button_l4", "extra_button_l5", "extra_button_r4", "extra_button_r5",
        "keycaps_dir",
    };
    static const char* const kPromptNames[] = {"prompts_live", "prompts_ps_map"};  // button_prompts is the input state's own: its generation moves with it (input_state.h)
    static std::once_flag once;
    std::call_once(once, [] {
        // The callbacks run inside the cvar registry's lock: they only bump the number.
        for (const char* name : kNames)
            larecomp::cvar::RegisterChangeCallback(name, [](std::string_view, std::string_view) {
                g_bindings_gen.fetch_add(1, std::memory_order_relaxed);
            });
        for (const char* name : kPromptNames)
            larecomp::cvar::RegisterChangeCallback(name, [](std::string_view, std::string_view) {
                g_prompt_settings_gen.fetch_add(1, std::memory_order_relaxed);
            });
    });
}

// Every reader goes through here, so the callbacks are in place before the first value is read.
uint32_t BindingsGen() {
    WatchBindings();
    return g_bindings_gen.load(std::memory_order_relaxed);
}

uint32_t PromptSettingsGen() {
    WatchBindings();
    return g_prompt_settings_gen.load(std::memory_order_relaxed);
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
uint32_t g_drive_gen_applied = 0;  // the bindings generation the live driving mappers carry (0 = none built yet)
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
    // With the ramp on (kb_steer_ramp) the steering keys are not mapper entries at all: the input poll ramps them and the XamInputGetState wrapper adds the value to the left stick
    // (KeySteerScope), so the pad's own entries for the axis stay as they are.
    uint32_t steer_out = 0;
    if (larecomp::settings::kb_steer_ramp.Get()) {
        d.steer_pad.clear();
        d.steer_pad_type.clear();
        d.steer_keys.clear();
        d.steer_scancodes.clear();
        d.steer_active = -2;
    } else if (out_of("STEER_AXIS", steer_out)) {
        const auto left = ParseKeyList(larecomp::settings::kb_drive_steer_left.Get());
        const auto right = ParseKeyList(larecomp::settings::kb_drive_steer_right.Get());
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
            if (!sc) continue;
            add(kTypeKey, sc, out);
            if (larecomp::kbm::FindMouseKeyByScancode(sc)) MC_INFO("[kbnative] drive {} <- {} (scancode 0x{:02X})", bind.action, k.name, sc);
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
    // The game proper has started: the ultrawide title prompt goes back to the middle (ultrawide.h).
    mc::ultrawide::NoteGameEntered();
    const bool keyboard = larecomp::settings::keyboard_native.Get();
    if (!keyboard && !ExtrasEnabled()) return;
    uint8_t* b = GuestBase();
    const uint32_t ea = static_cast<uint32_t>(r30.u64);
    if (!b || !HeapPtr(ea)) return;
    if (ReadGuestU32(b, ea + 4) != 0) return;  // player 0 only
    const uint32_t mapper = ReadGuestU32(b, ea + 8);
    const uint32_t outs = ReadGuestU32(b, ea + 12);
    if (!HeapPtr(mapper) || !HeapPtr(outs)) return;
    if (!keyboard) {
        // Extra pad buttons only: the game's own rebuild made the mapper; just report which
        // of its sources are keys, to see whether a bound extra button reached it.
        const uint32_t n = ReadGuestU32(b, mapper);
        MC_INFO("[kbnative] control map rebuilt: mapper @0x{:08X}, {} entries", mapper, n);
        for (uint32_t i = 0; i < n && i < 64; ++i) {
            const uint32_t type = ReadGuestU32(b, mapper + 4 * (2 + i));
            if (type != kTypeKey && type != kTypeKeyAxis) continue;
            const uint32_t value = ReadGuestU32(b, mapper + 4 * (66 + i)) & 0xFFFFFF;
            const uint32_t out = ReadGuestU32(b, mapper + 4 * (130 + i));
            MC_INFO("[kbnative]   entry {}: type {} value 0x{:X} -> output @0x{:08X} (action {})",
                    i, type, value, out, out >= outs ? (out - outs) / 12 : 0xFFFFu);
        }
        return;
    }

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
    g_drive_gen_applied = BindingsGen();
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
uint32_t g_ui_gen_applied = 0;  // the bindings generation the menu mapper carries (0 = none built yet)
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
        MC_INFO("[kbnative] menu {}", open ? "opened" : "closed");  // an edge, a few a minute at most: the tests read it (a pause the game opened by itself shows here)
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

    const uint32_t gen = BindingsGen();
    if (mapper == g_nav_mapper && count == g_nav_count_after && gen == g_ui_gen_applied) return;

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
                if (larecomp::kbm::FindMouseKeyByScancode(sc))
                    MC_INFO("[kbnative] menu button 0x{:X} <- {} (scancode 0x{:02X})", bind.button, k.name, sc);
            }
        }
    }
    g_nav_mapper = mapper;
    g_nav_count_after = ReadGuestU32(b, mapper);
    g_ui_gen_applied = gen;
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

// ── extra pad buttons ───────────────────────────────────────────────────
std::string ExtraCvar(int i) {
    switch (i) {
        case 0: return larecomp::settings::extra_button_l4.Get();
        case 1: return larecomp::settings::extra_button_l5.Get();
        case 2: return larecomp::settings::extra_button_r4.Get();
        default: return larecomp::settings::extra_button_r5.Get();
    }
}

std::vector<uint8_t> g_extra_vks[kNumExtras];  // host keys of each extra button
bool g_extra_swallow[256] = {};                // host keys taken from the keyboard feed
uint32_t g_extra_gen = 0;                      // the bindings generation the lists above were read at
uint8_t g_extra_prev[kNumExtras] = {};

void RefreshExtras() {
    const uint32_t gen = BindingsGen();
    if (gen == g_extra_gen) return;
    g_extra_gen = gen;
    std::memset(g_extra_swallow, 0, sizeof(g_extra_swallow));
    for (int i = 0; i < kNumExtras; ++i) {
        g_extra_vks[i].clear();
        for (const KeyRef& k : ParseKeyList(ExtraCvar(i))) {
            g_extra_vks[i].push_back(k.vk);
            g_extra_swallow[k.vk] = true;
        }
    }
}

bool ExtrasEnabled() {
    RefreshExtras();
    for (const auto& v : g_extra_vks)
        if (!v.empty()) return true;
    return false;
}

// -1 when the scancode is not one of the extra buttons'.
int ExtraIndexOfScancode(uint32_t sc) {
    for (int i = 0; i < kNumExtras; ++i)
        if (kExtraScancode[i] == sc) return i;
    return -1;
}

// ── key state feed ──────────────────────────────────────────────────────
uint8_t g_prev_state[256] = {};    // per scancode: down at the last poll
uint8_t g_just_pressed[256] = {};  // scancodes that went down this poll
std::vector<uint8_t> g_down_sc;    // the scancodes written as down at the last poll
std::vector<uint8_t> g_down_next;  // ... and at this one
std::vector<uint8_t> g_just_list;  // the scancodes g_just_pressed has set

// The keys of this poll (the latch's held keys plus the ones tapped since the last poll; empty while an overlay has the keyboard) into the game's
// pending-key table. Only the keys that are down are touched, and the ones that were down a poll ago are let go once.
void FeedKeys(uint8_t* b, const larecomp::kbm::KeySnapshot& keys) {
    RefreshExtras();
    for (uint8_t sc : g_just_list) g_just_pressed[sc] = 0;
    g_just_list.clear();
    g_down_next.clear();
    bool now[256] = {};
    keys.ForEach([&](int id) {
        if (g_extra_swallow[id]) return;  // a key that an extra pad button sends is that button, not a keyboard key
        const uint8_t sc = ScancodeOf(b, static_cast<uint8_t>(id));
        if (!sc || now[sc]) return;
        now[sc] = true;
        g_down_next.push_back(sc);
    });
    for (uint8_t sc : g_down_sc) {
        if (now[sc]) continue;
        b[kKeysPending + sc] = 0;
        if (const larecomp::kbm::MouseKey* mouse = larecomp::kbm::FindMouseKeyByScancode(sc))
            MC_INFO("[kbnative] mouse key {} up", mouse->name);
    }
    for (uint8_t sc : g_down_next) {
        b[kKeysPending + sc] = 0x80;
        if (!g_prev_state[sc]) {
            g_just_pressed[sc] = 1;
            g_just_list.push_back(sc);
            if (const larecomp::kbm::MouseKey* mouse = larecomp::kbm::FindMouseKeyByScancode(sc))
                MC_INFO("[kbnative] mouse key {} down", mouse->name);  // the mouse buttons and the wheel are rare enough to log: a click, a notch
        }
    }
    for (uint8_t sc : g_down_sc) g_prev_state[sc] = 0;
    for (uint8_t sc : g_down_next) g_prev_state[sc] = 1;
    g_down_sc.swap(g_down_next);
}

// The extra pad buttons, each as the key state of its own scancode. Also with the keyboard switched off.
void FeedExtras(uint8_t* b, const larecomp::kbm::KeySnapshot& keys) {
    RefreshExtras();
    for (int i = 0; i < kNumExtras; ++i) {
        bool down = false;
        for (uint8_t vk : g_extra_vks[i]) down = down || keys.Test(vk);
        const uint8_t sc = kExtraScancode[i];
        if (down != (g_extra_prev[i] != 0))
            MC_INFO("[kbnative] extra button {} {}", kExtraName[i], down ? "down" : "up");
        b[kKeysPending + sc] = down ? 0x80 : 0;
        g_just_pressed[sc] = (down && !g_extra_prev[i]) ? 1 : 0;
        g_extra_prev[i] = down ? 1 : 0;
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

void UpdateCameraJump(uint8_t* b, const larecomp::kbm::KeySnapshot& snap) {
    uint8_t& virt = b[kKeysPending + kVirtualCamKey];
    // The two lists are read again only when a binding changed.
    static std::vector<KeyRef> keys;
    static std::vector<int> views;
    static uint32_t lists_gen = 0;
    if (const uint32_t gen = BindingsGen(); gen != lists_gen) {
        lists_gen = gen;
        keys = ParseKeyList(larecomp::settings::kb_drive_camera_keys.Get());
        views = ParseIntList(larecomp::settings::kb_drive_camera_views.Get());
    }
    const uint32_t ctl = (g_game_entered && !MenuOpen(b)) ? CameraControl(b) : 0;

    // Key edges (host state: these keys have no mapper entry of their own).
    int wanted = -1;
    for (size_t i = 0; i < keys.size() && i < 5; ++i) {
        const bool down = snap.Test(keys[i].vk);
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
// Live button prompts (KBM-DESIGN.md P4; the pure part, with the reasons, is prompt_glyphs.h). Every movie draws its buttons from the Xbox bitmaps of shared_latin.xtd: the
// movies' platform flag is pinned to Xbox (hooks/ui.cpp, prompts_live), because a flag change does not reach a prompt that is already on screen (KBM-P0 E1) and a write into
// these bitmaps does (the texture cache is write-watched: the next draw uploads them again). What the bitmaps hold is the glyph family's art: the originals (Xbox), the PlayStation
// set, or key caps (the menu's keys while a menu is open, the driving keys on the road).
//
// One entry per button; a glyph can appear in several movies under different shared_N, so every copy is written. The round Start and Back glyphs are 64x32 bitmaps; the face and
// shoulder buttons are 64x64.
struct GlyphTarget {
    const char* label;
    uint32_t width, height;
    std::vector<const char*> textures;
    int icon = 0;             // iconID the rows of the game use for this glyph (1-based frame)
    int slot = -1;            // spare slot number, -1 = a menu button's own glyph
    int pslot = -1;           // larecomp::prompts::Slot of a menu button's glyph, -1 = a spare slot
};
constexpr int P(larecomp::prompts::Slot s) { return static_cast<int>(s); }
const GlyphTarget kGlyphs[] = {
    {"A", 64, 64, {"shared_2.dds", "shared_49.dds"}, 1, -1, P(larecomp::prompts::Slot::A)},
    {"B", 64, 64, {"shared_45.dds"}, 2, -1, P(larecomp::prompts::Slot::B)},
    {"X", 64, 64, {"shared_4.dds"}, 3, -1, P(larecomp::prompts::Slot::X)},
    {"Y", 64, 64, {"shared_42.dds"}, 4, -1, P(larecomp::prompts::Slot::Y)},
    {"LB", 64, 64, {"shared_10.dds", "shared_41.dds"}, 5, -1, P(larecomp::prompts::Slot::LB)},
    {"RB", 64, 64, {"shared_1.dds", "shared_12.dds"}, 6, -1, P(larecomp::prompts::Slot::RB)},
    {"LT", 64, 64, {"shared_43.dds", "shared_48.dds"}, 7, -1, P(larecomp::prompts::Slot::LT)},
    {"RT", 64, 64, {"shared_18.dds", "shared_37.dds"}, 8, -1, P(larecomp::prompts::Slot::RT)},
    // iconID 9 = Start (prompt.xsf: shared_47, pause/popup: shared_33), 10 = Back (31 / 38).
    {"Start", 64, 32, {"shared_47.dds", "shared_33.dds"}, 9, -1, P(larecomp::prompts::Slot::Start)},
    {"Back", 64, 32, {"shared_31.dds", "shared_38.dds"}, 10, -1, P(larecomp::prompts::Slot::Back)},
    // The Sidekick phone (calls to a race) draws its own "accept" button: a gray 64x64 Back glyph embedded in sidekick.xsf (character 106, the same texture is character 646), not
    // one of the shared dictionary's. The phone accepts with the Back button, so it shows that key.
    {"Phone", 64, 64, {"sidekick:106"}, 0, -1, P(larecomp::prompts::Slot::Phone)},
    // Four spare glyphs for the Custom Controls screen: the PlayStation cross, circle, square and triangle (iconID 50..53, 32x32). The platform flag is pinned to 0, so no
    // movie draws them as a PlayStation button: they are free to carry the cap of whichever other key a row is bound to (the screen draws a binding as "iconID" text, so a key
    // with no glyph of its own gets one of these). They are also the source of the PlayStation faces: the originals kept below are what is copied, never the live pixels.
    {"Slot0", 32, 32, {"shared_24.dds"}, 50, 0},
    {"Slot1", 32, 32, {"shared_13.dds"}, 51, 1},
    {"Slot2", 32, 32, {"shared_15.dds"}, 52, 2},
    {"Slot3", 32, 32, {"shared_36.dds"}, 53, 3},
};
constexpr int kNumSlots = 4;
std::string g_slot_key[kNumSlots];  // key name painted on each spare glyph, "" = free

struct GlyphTexture {
    uint32_t base = 0;                 // guest address of the pixel data
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> original;     // what the game shipped (w/4 * h/4 blocks of 16 bytes, linear order, still byte-swapped as the guest holds them), to put back and to copy from
    larecomp::prompts::Art art;        // what is on it now (Original = the shipped art)
    std::string label;                 // the slot it is a glyph of ("A", "Phone", "Slot0"), for the log
    double backdrop = larecomp::prompts::kDarkBackdrop;  // the luma of what it sits on (prompt_glyphs.h SlotInfo::backdrop)
    larecomp::prompts::Ink ink;        // the ink of the shipped art (box, colour, stroke, contrast), measured once when the texture is found (MeasureTexture): what a replacement is fitted into
};
std::unordered_map<std::string, GlyphTexture> g_glyph_tex;  // by "shared_N.dds"
uint32_t g_glyph_dict = 0;
uint32_t g_dict_vals = 0, g_dict_count = 0;  // the dictionary's texture table, for the lookups by name
std::unordered_map<std::string, std::vector<uint8_t>> g_cap_cache;  // "<art>@<texture>" (e.g. "key:Tab@sidekick:106") -> tiled bytes of the art fitted for that texture
uint32_t g_glyph_tick = 0;
bool g_art_warm = false;  // the art of the glyph families has been built (WarmArt); a new texture (the Sidekick's glyph) makes it false again, and WarmArt builds what is missing

// Something that decides what the glyphs show changed (the device, the family, a binding, the menu, a new dictionary): the next input poll paints. Set from any thread (the
// input state's listener runs on the UI thread, the pad poll's thread, a pool thread); read and cleared by the poll, on the game thread.
std::atomic<bool> g_prompts_dirty{true};

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
    const std::string dir = larecomp::settings::keycaps_dir.Get();
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
    // No resources in a Linux executable: CMake copies src/keycaps next to it as keycaps/.
    // CHECK: not run on Linux (the key prompts of the keyboard; a missing folder gives the blank cap).
    const std::filesystem::path path =
        rex::platform::GetExecutableFolder() / "keycaps" / (stem + ".png");
    if (!bytes) return std::filesystem::exists(path);
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    bytes->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return !bytes->empty();
#endif
}

bool CapExists(const std::string& key, uint32_t width, uint32_t height) {
    return ReadCapBytes(key, width, height, nullptr);
}

// Reads the key cap (or shipped picture) at its own size, the picture the fitting rule works from (prompt_art.h); empty when it is not there.
larecomp::prompts::Bitmap LoadCapBitmap(const std::string& key, uint32_t width, uint32_t height) {
    larecomp::prompts::Bitmap out;
    std::vector<uint8_t> bytes;
    if (!ReadCapBytes(key, width, height, &bytes)) return out;
    int w = 0, h = 0;
    std::vector<uint8_t> rgba = rex::ui::DecodeImageRGBA(bytes.data(), bytes.size(), w, h);
    if (rgba.empty() || w <= 0 || h <= 0) return out;
    out.width = static_cast<uint32_t>(w);
    out.height = static_cast<uint32_t>(h);
    out.rgba = std::move(rgba);
    return out;
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


// What the texture shows today, measured once from the copy of the shipped blocks: the box of its ink, its colour, its stroke weight and its contrast against what it sits on. This is
// what every replacement is fitted into (prompt_art.h); the measuring is here, when the texture is found while the game loads, and never on a repaint.
void MeasureTexture(GlyphTexture& t, const std::string& label, double backdrop) {
    t.label = label;
    t.backdrop = backdrop;
    larecomp::prompts::Bitmap shipped;
    shipped.width = t.width;
    shipped.height = t.height;
    shipped.rgba = larecomp::prompts::DecodeBc3GuestSurface(t.original, t.width, t.height);
    t.ink = larecomp::prompts::MeasureOriginal(shipped, backdrop);
}

// The luma of what a glyph target sits on: its slot's (prompt_glyphs.h), the dark prompt bar for the spare Custom Controls slots.
double BackdropOf(const GlyphTarget& g) {
    return g.pslot >= 0 ? larecomp::prompts::Info(static_cast<larecomp::prompts::Slot>(g.pslot)).backdrop : larecomp::prompts::kDarkBackdrop;
}

// "ink box 12,14..51,48 luma 181 line 107,107,107 mark 68,69,68 alpha 255 stroke 3.0" for the log: the box, the mean luma, the colour of the dark band (the strokes), the colour of the marks and the alpha the
// strokes reach, as the bitmap holds them, and the width of the stroke.
std::string DescribeInk(const larecomp::prompts::Ink& ink) {
    if (!ink.valid()) return "no ink";
    char text[192];
    std::snprintf(text, sizeof(text), "ink box %d,%d..%d,%d luma %.0f line %.0f,%.0f,%.0f mark %.0f,%.0f,%.0f alpha %.0f stroke %.1f", ink.box.x0, ink.box.y0, ink.box.x1, ink.box.y1, ink.luma, ink.line_rgb[0],
                  ink.line_rgb[1], ink.line_rgb[2], ink.mark_rgb[0], ink.mark_rgb[1], ink.mark_rgb[2], ink.line_alpha, ink.stroke);
    return text;
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
    MeasureTexture(t, "Phone", larecomp::prompts::Info(larecomp::prompts::Slot::Phone).backdrop);
    // The first P4 run could not say what this glyph looks like (it is not in the dictionary): the dump writes it with the others, for the glyph sheet.
    if (const std::string dump = larecomp::settings::prompts_glyph_dump.Get(); !dump.empty()) {
        const std::vector<uint8_t> rgba = larecomp::prompts::DecodeBc3GuestSurface(t.original, w, h);
        std::error_code ec;
        std::filesystem::create_directories(dump, ec);
        std::ofstream raw(std::filesystem::path(dump) / ("sidekick_106_" + std::to_string(w) + "x" + std::to_string(h) + ".rgba"), std::ios::binary);
        raw.write(reinterpret_cast<const char*>(rgba.data()), static_cast<std::streamsize>(rgba.size()));
    }
    MC_INFO("[kbnative] sidekick accept glyph @0x{:08X}, {}", base, DescribeInk(t.ink));
    g_glyph_tex["sidekick:106"] = std::move(t);
    g_sidekick_ok = true;
    g_art_warm = false;  // its art (fitted into this ink) is built by the next poll's WarmArt, not on the first repaint
    return true;
}

// A texture of the shared_latin dictionary, as the guest's grcTexture describes it.
struct DictTexture {
    std::string name;
    uint32_t w = 0, h = 0, fmt = 0, endian = 0, base = 0;
};

bool ReadDictTexture(const uint8_t* b, uint32_t tex, DictTexture* out) {
    if (!HeapPtr(tex)) return false;
    const uint32_t name_ptr = ReadGuestU32(b, tex + 0x18);
    if (!HeapPtr(name_ptr) && name_ptr < 0x82000000u) return false;
    const char* text = reinterpret_cast<const char*>(b + name_ptr);
    size_t n = 0;
    while (n < 31 && text[n]) ++n;
    out->name.assign(text, n);
    out->w = (uint32_t(b[tex + 0x20]) << 8) | b[tex + 0x21];
    out->h = (uint32_t(b[tex + 0x22]) << 8) | b[tex + 0x23];
    const uint32_t hdr = ReadGuestU32(b, tex + 0x1C);
    if (!HeapPtr(hdr)) return false;
    const uint32_t fc1 = ReadGuestU32(b, hdr + 0x20);  // fetch constant dword 1
    out->fmt = fc1 & 0x3F;
    out->endian = (fc1 >> 6) & 3;
    out->base = fc1 & 0xFFFFF000;
    return true;
}

// The dictionary's texture of that name (the table is the one ScanDictionary found).
bool FindDictTexture(const uint8_t* b, const std::string& name, DictTexture* out) {
    for (uint32_t i = 0; i < g_dict_count; ++i) {
        DictTexture d;
        if (!ReadDictTexture(b, ReadGuestU32(b, g_dict_vals + 4 * i), &d) || d.name != name) continue;
        *out = std::move(d);
        return true;
    }
    return false;
}

// The blocks of a texture at their tiled offsets, in linear order, still as the guest holds them. A small texture's blocks are NOT a contiguous run (the ForEachBlock note above).
std::vector<uint8_t> ReadBlocks(const uint8_t* b, uint32_t base, uint32_t w, uint32_t h) {
    std::vector<uint8_t> blocks(size_t((w / 4) * (h / 4) * 16));
    ForEachBlock(w, h, [&](uint32_t i, uint32_t off) { std::memcpy(blocks.data() + size_t(i) * 16, b + base + off, 16); });
    return blocks;
}

// Writes every small DXT5 texture of the dictionary as raw RGBA (<name>_<w>x<h>.rgba) with an index.txt, to identify the bitmaps nobody has named yet (the PlayStation shoulders: the
// chain's sheet, perf\scripts\kbm\kbm_p4_sheet.py, draws them). The cvar prompts_glyph_dump names the folder; it is read once, when the dictionary is first scanned.
void DumpDictionary(const uint8_t* b, const std::string& dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream index(std::filesystem::path(dir) / "index.txt");
    int written = 0;
    for (uint32_t i = 0; i < g_dict_count; ++i) {
        DictTexture d;
        if (!ReadDictTexture(b, ReadGuestU32(b, g_dict_vals + 4 * i), &d)) continue;
        index << d.name << ' ' << d.w << ' ' << d.h << ' ' << d.fmt << ' ' << d.endian << '\n';
        if (d.fmt != 20 || d.endian != 1 || !HeapPtr(d.base) || d.w == 0 || d.h == 0 || d.w > 128 || d.h > 128 || (d.w % 4) || (d.h % 4)) continue;
        const std::vector<uint8_t> rgba = larecomp::prompts::DecodeBc3GuestSurface(ReadBlocks(b, d.base, d.w, d.h), d.w, d.h);
        std::string stem = d.name;
        if (stem.size() > 4 && stem.compare(stem.size() - 4, 4, ".dds") == 0) stem.resize(stem.size() - 4);
        std::ofstream raw(std::filesystem::path(dir) / (stem + "_" + std::to_string(d.w) + "x" + std::to_string(d.h) + ".rgba"), std::ios::binary);
        raw.write(reinterpret_cast<const char*>(rgba.data()), static_cast<std::streamsize>(rgba.size()));
        ++written;
    }
    MC_INFO("[prompts] dictionary dumped: {} bitmaps as raw RGBA in {}", written, dir);
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
    g_cap_cache.clear();  // the PlayStation art in it was decoded from the old dictionary
    g_sidekick_ok = false;
    g_dict_vals = vals;
    g_dict_count = count;
    for (const GlyphTarget& g : kGlyphs) {
        for (const char* tex_name : g.textures) {
            DictTexture d;
            if (!FindDictTexture(b, tex_name, &d)) continue;
            if (d.w != g.width || d.h != g.height) {
                MC_WARN("[kbnative] {} is {}x{}, expected {}x{}: skipped", tex_name, d.w, d.h, g.width, g.height);
                continue;
            }
            if (d.fmt != 20 || d.endian != 1 || !HeapPtr(d.base)) {
                MC_WARN("[kbnative] {}: format {} endian {} base 0x{:08X}, expected DXT5/8-in-16: skipped", tex_name, d.fmt, d.endian, d.base);
                continue;
            }
            GlyphTexture t;
            t.base = d.base;
            t.width = d.w;
            t.height = d.h;
            t.original = ReadBlocks(b, d.base, d.w, d.h);
            MeasureTexture(t, g.label, BackdropOf(g));
            MC_INFO("[kbnative] {} base 0x{:08X} {}x{}, {}", tex_name, d.base, d.w, d.h, DescribeInk(t.ink));
            g_glyph_tex[tex_name] = std::move(t);
        }
    }
    g_glyph_dict = dict;
    g_art_warm = false;
    g_prompts_dirty.store(true, std::memory_order_relaxed);
    MC_INFO("[kbnative] glyph dictionary @0x{:08X}: {} textures found", dict, g_glyph_tex.size());
    const std::string dump = larecomp::settings::prompts_glyph_dump.Get();
    if (!dump.empty()) DumpDictionary(b, dump);
    return !g_glyph_tex.empty();
}

// A dictionary bitmap the PlayStation art may be copied from: one of the glyph textures, or any small DXT5 one the debug cvar prompts_ps_map names (captured on first use).
bool HaveBitmap(const uint8_t* b, const std::string& name) {
    if (g_glyph_tex.count(name)) return true;
    DictTexture d;
    if (!FindDictTexture(b, name, &d) || d.fmt != 20 || d.endian != 1 || !HeapPtr(d.base) || d.w == 0 || d.h == 0 || d.w > 128 || d.h > 128 || (d.w % 4) || (d.h % 4)) return false;
    GlyphTexture t;
    t.base = d.base;
    t.width = d.w;
    t.height = d.h;
    t.original = ReadBlocks(b, d.base, d.w, d.h);
    MeasureTexture(t, name, larecomp::prompts::kDarkBackdrop);
    g_glyph_tex[name] = std::move(t);
    return true;
}

// The picture an art is made from, at its own size: a key cap or a shipped picture of the folder (an unknown key gets the blank cap), or one of the game's own bitmaps read back from the
// originals kept at the scan (never the live pixels). Empty when it cannot be made.
larecomp::prompts::Bitmap SourceBitmap(const larecomp::prompts::Art& art, uint32_t width, uint32_t height) {
    using larecomp::prompts::ArtKind;
    if (art.kind == ArtKind::GameBitmap) {
        larecomp::prompts::Bitmap out;
        const auto it = g_glyph_tex.find(art.id);
        if (it == g_glyph_tex.end()) return out;
        out.width = it->second.width;
        out.height = it->second.height;
        out.rgba = larecomp::prompts::DecodeBc3GuestSurface(it->second.original, it->second.width, it->second.height);
        return out;
    }
    larecomp::prompts::Bitmap cap = LoadCapBitmap(art.id, width, height);
    if (cap.empty() && art.kind == ArtKind::KeyCap) cap = LoadCapBitmap("blank", width, height);
    return cap;
}

// One line per surface built (once, with the art): the numbers of the fitting rule's check (prompt_art.h CheckFit), read back by perf\scripts\kbm\kbm_p4_report.py:
// the replacement's ink box against the original's, both lumas, the replacement's ink colour and alpha against the original's dark band colour and alpha, both mean contrasts against what the
// glyph sits on, and the verdict.
void LogFit(const std::string& tex_name, const larecomp::prompts::Art& art, const GlyphTexture& t, const larecomp::prompts::Bitmap& fitted) {
    const larecomp::prompts::FitCheck c = larecomp::prompts::CheckFit(t.ink, fitted, t.backdrop);
    MC_INFO("[prompts] fit slot={} tex={} art={} look={} orig_box={},{},{},{} box={},{},{},{} orig_luma={:.0f} luma={:.0f} orig_ink={:.0f},{:.0f},{:.0f} ink={:.0f},{:.0f},{:.0f} orig_alpha={:.0f} alpha={:.0f} "
            "orig_contrast={:.0f} contrast={:.0f} backdrop={:.0f} stroke={:.1f} ok={}",
            t.label, tex_name, larecomp::prompts::Describe(art), c.look == larecomp::prompts::Look::Line ? "line" : "solid", c.original_box.x0, c.original_box.y0, c.original_box.x1, c.original_box.y1,
            c.box.x0, c.box.y0, c.box.x1, c.box.y1, c.original_luma, c.luma, c.original_rgb[0], c.original_rgb[1], c.original_rgb[2], c.rgb[0], c.rgb[1], c.rgb[2], c.original_alpha, c.alpha, c.original_contrast,
            c.contrast, t.backdrop, t.ink.stroke, c.ok() ? 1 : 0);
}

// The tiled, swapped bytes (what the guest holds) of `art` for the texture `tex_name`: the source picture fitted into the ink of what that texture shows today and drawn in its tonal
// scheme (prompt_art.h FitArt), compressed and tiled, cached. Empty when the art cannot be made. Built when the art is warmed (WarmArt, while the game loads) and, for a key that was not
// warmed (a binding the Custom Controls screen shows), the first time it is painted.
const std::vector<uint8_t>& TiledArt(const larecomp::prompts::Art& art, const std::string& tex_name, const GlyphTexture& t) {
    const std::string id = larecomp::prompts::Describe(art) + "@" + tex_name;
    auto it = g_cap_cache.find(id);
    if (it != g_cap_cache.end()) return it->second;
    std::vector<uint8_t> tiled;
    const larecomp::prompts::Bitmap source = SourceBitmap(art, t.width, t.height);
    larecomp::prompts::Fitted fitted = larecomp::prompts::FitArt(source, larecomp::prompts::ShapeOf(art.kind), t.ink, t.backdrop, t.width, t.height);
    if (!fitted.bitmap.empty()) {
        LogFit(tex_name, art, t, fitted.bitmap);
        mc::modloader::Image img;
        img.width = fitted.bitmap.width;
        img.height = fitted.bitmap.height;
        img.rgba = std::move(fitted.bitmap.rgba);
        std::vector<uint8_t> linear;
        mc::modloader::CompressBlocks(img, mc::modloader::BlockFormat::kBc3, linear);
        mc::modloader::TileXbox360(linear, t.width, t.height, mc::modloader::BlockFormat::kBc3, tiled);
    }
    return g_cap_cache.emplace(id, std::move(tiled)).first->second;
}

// Puts `art` on a dictionary texture; true when its pixels changed. Only the blocks of the picture itself are written, at the offsets the GPU reads them from.
bool PaintGlyph(uint8_t* b, const char* tex_name, const larecomp::prompts::Art& art) {
    auto it = g_glyph_tex.find(tex_name);
    if (it == g_glyph_tex.end()) return false;
    GlyphTexture& t = it->second;
    if (t.art == art) return false;
    if (art.kind == larecomp::prompts::ArtKind::Original) {
        ForEachBlock(t.width, t.height, [&](uint32_t i, uint32_t off) {
            std::memcpy(b + t.base + off, t.original.data() + size_t(i) * 16, 16);
        });
    } else {
        const std::vector<uint8_t>& cap = TiledArt(art, tex_name, t);
        if (cap.empty()) return false;
        bool ok = true;
        ForEachBlock(t.width, t.height, [&](uint32_t, uint32_t off) { ok = ok && off + 16 <= cap.size(); });
        if (!ok) {
            MC_WARN("[kbnative] {}: the tiled art is shorter than the surface, left alone", tex_name);
            return false;
        }
        ForEachBlock(t.width, t.height, [&](uint32_t, uint32_t off) {
            std::memcpy(b + t.base + off, cap.data() + off, 16);
        });
    }
    t.art = art;
    return true;
}

bool PaintKey(uint8_t* b, const char* tex_name, const std::string& key) {
    return PaintGlyph(b, tex_name, key.empty() ? larecomp::prompts::Art{} : larecomp::prompts::Art{larecomp::prompts::ArtKind::KeyCap, key});
}

// ── which art a glyph shows ─────────────────────────────────────────────
struct KeyCvar {
    std::string_view name;
    std::string& (*value)();
};
const KeyCvar kKeyCvars[] = {
    {"kb_ui_accept", &FLAGS_kb_ui_accept_storage_},             {"kb_ui_back", &FLAGS_kb_ui_back_storage_},
    {"kb_ui_x", &FLAGS_kb_ui_x_storage_},                       {"kb_ui_y", &FLAGS_kb_ui_y_storage_},
    {"kb_ui_lb", &FLAGS_kb_ui_lb_storage_},                     {"kb_ui_rb", &FLAGS_kb_ui_rb_storage_},
    {"kb_ui_lt", &FLAGS_kb_ui_lt_storage_},                     {"kb_ui_rt", &FLAGS_kb_ui_rt_storage_},
    {"kb_ui_start", &FLAGS_kb_ui_start_storage_},               {"kb_ui_select", &FLAGS_kb_ui_select_storage_},
    {"kb_drive_handbrake", &FLAGS_kb_drive_handbrake_storage_}, {"kb_drive_weight", &FLAGS_kb_drive_weight_storage_},
    {"kb_drive_nitro", &FLAGS_kb_drive_nitro_storage_},         {"kb_drive_headlights", &FLAGS_kb_drive_headlights_storage_},
    {"kb_drive_camera", &FLAGS_kb_drive_camera_storage_},       {"kb_drive_brake", &FLAGS_kb_drive_brake_storage_},
    {"kb_drive_gas", &FLAGS_kb_drive_gas_storage_},             {"kb_drive_pause", &FLAGS_kb_drive_pause_storage_},
    {"kb_drive_map", &FLAGS_kb_drive_map_storage_},
};

// The key a binding cvar draws as on a width x height glyph: the first key of its list that has a cap, else the first key (the blank cap), else "" (the art stays).
std::string KeyOfCvar(std::string_view cvar, uint32_t width, uint32_t height) {
    for (const KeyCvar& k : kKeyCvars) {
        if (k.name != cvar) continue;
        std::vector<std::string> names;
        for (const KeyRef& key : ParseKeyList(k.value())) names.push_back(key.name);
        return larecomp::prompts::PickKey(names, [&](const std::string& n) { return CapExists(n, width, height); });
    }
    return {};
}

larecomp::prompts::Art DecideFor(const uint8_t* b, const GlyphTarget& g, larecomp::prompts::Family family, larecomp::prompts::Context ctx) {
    const auto overrides = larecomp::prompts::ParsePsMap(larecomp::settings::prompts_ps_map.Get());
    return larecomp::prompts::Decide(
        family, ctx, static_cast<larecomp::prompts::Slot>(g.pslot), overrides, [&](const std::string& name) { return HaveBitmap(b, name); },
        [&](const std::string& stem) { return CapExists(stem, g.width, g.height); },
        [&](std::string_view cvar) { return KeyOfCvar(cvar, g.width, g.height); });
}

// The glyph family the pixels follow. With prompts_live off the movies' platform flag shows the PlayStation set (the older way), so the pixels stay the Xbox ones for it.
larecomp::prompts::Family EffectiveFamily() {
    const larecomp::prompts::Family f = larecomp::input::ActiveGlyphFamily();
    if (f == larecomp::prompts::Family::PlayStation && !larecomp::settings::prompts_live.Get()) return larecomp::prompts::Family::Xbox;
    return f;
}

// ── which screen the game is on: the menu's keys or the road's (prompt_glyphs.h, UiSignals) ──
// The first P4 run: the pause menu is on mcMainForm's open-form stack (MenuOpen), the garage, its EXIT question and the full map are not, and the key caps there were the driving keys.
// Two more facts of FlashNavigator (mcUIManager +52) are read; each is one load.
constexpr uint32_t kNavCurrentMovie = 263952;  // the movie entry the statechart's `movie` command selected (flash-cmd type 3, sub_8221EEC0); 0 after 'NULL'
constexpr uint32_t kNavLayerCount = 263992;    // modal message boxes in its FIFO (hooks/photo_album.cpp kNavLayerCount: the three-entry layer queue, count 0..3)

// The name an entry is filed under in mcUIManager's movie map, "" when it is not in it (the same walk as FindMovie, the other way round).
std::string MovieNameOf(const uint8_t* b, uint32_t entry) {
    const uint32_t mgr = ReadGuestU32(b, kUiManagerPtr);
    if (!HeapPtr(mgr)) return {};
    const uint32_t buckets = ReadGuestU32(b, mgr + 60);
    const uint32_t count = (uint32_t(b[mgr + 64]) << 8) | b[mgr + 65];
    if (!HeapPtr(buckets) || count > 4096) return {};
    for (uint32_t i = 0; i < count; ++i) {
        for (uint32_t node = ReadGuestU32(b, buckets + 4 * i), guard = 0; node && guard < 256; node = ReadGuestU32(b, node + 12), ++guard) {
            const uint32_t key = ReadGuestU32(b, node);
            if (key < 0x10000u || key >= 0xE0000000u) continue;
            const uint32_t holder = ReadGuestU32(b, node + 8);
            if (!HeapPtr(holder) || ReadGuestU32(b, holder) != entry) continue;
            const char* name = reinterpret_cast<const char*>(b + key);
            size_t n = 0;
            while (n < 48 && name[n]) ++n;
            return std::string(name, n);
        }
    }
    return {};
}

uint32_t g_ui_movie_entry = 0;   // the current movie of the last poll, and its name (looked up when it changes only)
std::string g_ui_movie_name;
larecomp::prompts::UiSignals g_ui_signals;  // what the last poll saw (PollPrompts)
std::string g_ui_prev_name;                 // the movie name that went with it (for the 'was' of the log line)
std::atomic<bool> g_screen_menu{true};      // what the last poll made of it, for larecomp::keyboard::MenuScreenUp (mouse look): a menu screen is up, or the game is not entered yet
std::atomic<bool> g_screen_phone{false};    // ... and the Sidekick phone is the current movie (PhoneScreenUp)

larecomp::prompts::UiSignals UiSignalsOf(const uint8_t* b) {
    larecomp::prompts::UiSignals s;
    s.form = MenuOpen(b);
    const uint32_t ui = ReadGuestU32(b, kUiManagerPtr);
    const uint32_t nav = HeapPtr(ui) ? ReadGuestU32(b, ui + kUiNavigatorOff) : 0;
    uint32_t entry = 0;
    if (HeapPtr(nav)) {
        entry = ReadGuestU32(b, nav + kNavCurrentMovie);
        const uint32_t layers = ReadGuestU32(b, nav + kNavLayerCount);
        s.layer = layers != 0 && layers <= 3;
    }
    if (!HeapPtr(entry)) entry = 0;
    if (entry != g_ui_movie_entry) {
        g_ui_movie_entry = entry;
        g_ui_movie_name = entry ? MovieNameOf(b, entry) : std::string();
    }
    s.movie = entry != 0 && !larecomp::prompts::IsRoadMovieName(g_ui_movie_name);
    return s;
}

// The menu's keys while a screen is up and before the game is entered; the road's otherwise. Uses what the last poll saw (PollPrompts reads it once per poll, before anything asks).
bool MenuContext() { return larecomp::prompts::ContextFor(g_game_entered, g_ui_signals) == larecomp::prompts::Context::Menu; }

std::string UiReason() { return g_game_entered ? larecomp::prompts::DescribeUi(g_ui_signals, g_ui_movie_name) : std::string("game not entered"); }

// Builds every picture the families may need, once the dictionary is found (while the game is still loading): a key cap is a PNG decoded, scaled, compressed and tiled, and the
// PlayStation faces are read back from the game's bitmaps and compressed again; doing that on the first key press would be a hitch in the middle of a race. After this a repaint is
// only copies. About 40 surfaces, a few milliseconds; the line in the log says how many and how long.
void WarmArt(const uint8_t* b) {
    using namespace larecomp::prompts;
    g_art_warm = true;
    const auto t0 = std::chrono::steady_clock::now();
    const size_t before = g_cap_cache.size();
    auto warm = [&](Family f, Context c) {
        for (const GlyphTarget& g : kGlyphs) {
            if (g.pslot < 0) continue;
            const Art a = DecideFor(b, g, f, c);
            if (a.kind == ArtKind::Original) continue;
            for (const char* tex_name : g.textures) {
                const auto it = g_glyph_tex.find(tex_name);
                if (it != g_glyph_tex.end()) TiledArt(a, tex_name, it->second);  // the Sidekick's glyph is not there yet when the dictionary is found: ScanSidekick asks for another pass
            }
        }
    };
    warm(Family::Keyboard, Context::Menu);
    warm(Family::Keyboard, Context::Drive);
    warm(Family::PlayStation, Context::Menu);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    MC_INFO("[prompts] art ready: {} surfaces built in {:.1f} ms (key caps for the menu and the road, the PlayStation set)", g_cap_cache.size() - before, ms);
}

uint32_t g_prompts_repaints = 0;
bool g_prompts_menu = true;         // the context the key caps were last chosen for
uint32_t g_prompts_bindings = 0;    // the bindings generation they were chosen under
uint32_t g_prompts_settings = 0;    // ... and the generation of prompts_live / prompts_ps_map
bool g_prompts_listening = false;   // the input state's listener is registered
uint32_t g_prompts_generation = 0;  // the input state's generation at the last repaint (only used when the listener could not be registered)

void OnInputChanged(void*) { g_prompts_dirty.store(true, std::memory_order_relaxed); }

// Puts the art of the glyph family in use into every glyph bitmap: the originals, the PlayStation set, or key caps. Only called on a change (the device or the family moved, a
// binding changed, a menu opened or closed with the keyboard in use, a new dictionary), never per frame. Game thread, from the input poll. The cost is the copy of 18 small
// textures (about 4096 blocks of 16 bytes, a few tens of microseconds) and the GPU's upload of them on the next draw; the log line has the number.
void RepaintPrompts(uint8_t* b) {
    using namespace larecomp::prompts;
    const auto t0 = std::chrono::steady_clock::now();
    const Family family = EffectiveFamily();
    const bool menu = MenuContext();
    const Context ctx = menu ? Context::Menu : Context::Drive;
    g_prompts_menu = menu;
    int changed = 0;
    std::string ps_art;
    for (const GlyphTarget& g : kGlyphs) {
        if (g.pslot < 0) continue;  // the spare slots are the Custom Controls screen's
        const Art art = DecideFor(b, g, family, ctx);
        for (const char* tex_name : g.textures)
            if (PaintGlyph(b, tex_name, art)) ++changed;
        if (family == Family::PlayStation) ps_art += std::string(g.label) + "=" + Describe(art) + " ";
    }
    ++g_prompts_repaints;
    const long us = static_cast<long>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
    static const char* const kFamilyName[] = {"Xbox", "PlayStation", "Keyboard"};
    std::string keys;  // " (menu keys; movie GARAGEMOVIE)", " (driving keys)": which keys the caps are, and the screen facts that made it so
    if (family == Family::Keyboard) keys = menu ? " (menu keys; " + UiReason() + ")" : " (driving keys)";
    MC_INFO("[prompts] glyphs -> {}{}: {} bitmaps changed in {} us (repaint {}, input generation {})", kFamilyName[static_cast<int>(family)], keys, changed, us, g_prompts_repaints,
            larecomp::input::Generation());
    if (!ps_art.empty()) MC_INFO("[prompts] PlayStation art: {}", ps_art);
}

// Every input poll (Hook_KeyboardFeed): a few relaxed loads, and a repaint when something changed. The slow tick (every 32 polls) only looks for what may not exist yet: the
// dictionary, the Sidekick phone's embedded glyph.
void PollPrompts(uint8_t* b) {
    if (!g_prompts_listening) {
        // Registered here, on the game thread, once (the input state's first set-up is not announced: the dirty flag is already set).
        g_prompts_listening = larecomp::input::AddInputListener(&OnInputChanged, nullptr);
        g_prompts_dirty.store(true, std::memory_order_relaxed);
        if (!g_prompts_listening) MC_WARN("[prompts] the input state's listener table is full: the generation is compared on every poll instead");
    }
    if ((++g_glyph_tick & 0x1F) == 0) {
        if (ScanDictionary(b) && !g_sidekick_ok && ScanSidekick(b)) g_prompts_dirty.store(true, std::memory_order_relaxed);
    }
    // Which screen the game is on: three loads of FlashNavigator and the form stack. Read before the dictionary check below, because mouse look asks about the screen too
    // (MenuScreenUp / PhoneScreenUp) and must not depend on the glyph art being found.
    const larecomp::prompts::UiSignals ui = UiSignalsOf(b);
    g_screen_menu.store(larecomp::prompts::ContextFor(g_game_entered, ui) == larecomp::prompts::Context::Menu, std::memory_order_relaxed);
    g_screen_phone.store(g_ui_movie_name.compare(0, 8, "SIDEKICK") == 0, std::memory_order_relaxed);
    if (g_glyph_tex.empty()) return;  // no dictionary yet: stays dirty
    if (!g_art_warm) WarmArt(b);

    const uint32_t bindings = BindingsGen(), settings = PromptSettingsGen();
    if (bindings != g_prompts_bindings || settings != g_prompts_settings) {
        g_prompts_bindings = bindings;
        g_prompts_settings = settings;
        g_prompts_dirty.store(true, std::memory_order_relaxed);
    }
    if (!g_prompts_listening) {
        const uint32_t generation = larecomp::input::Generation();
        if (generation != g_prompts_generation) {
            g_prompts_generation = generation;
            g_prompts_dirty.store(true, std::memory_order_relaxed);
        }
    }
    // A change of the screen is one log line (the screens the first run did not know are found from it), and, while the keyboard is the family in use, a repaint when it turns the keys
    // from the road's into the menu's or back.
    if (!(ui == g_ui_signals) || (ui.movie && g_ui_movie_name != g_ui_prev_name)) {
        const std::string was = larecomp::prompts::DescribeUi(g_ui_signals, g_ui_prev_name);
        g_ui_signals = ui;
        g_ui_prev_name = g_ui_movie_name;
        MC_INFO("[prompts] screen -> {} (was {}): the keys are {}", larecomp::prompts::DescribeUi(ui, g_ui_movie_name), was, MenuContext() ? "the menu's" : "the road's");
    }
    if (larecomp::prompts::ContextNeedsRepaint(larecomp::input::ActiveGlyphFamily(), g_game_entered, g_ui_signals, g_prompts_menu)) g_prompts_dirty.store(true, std::memory_order_relaxed);
    if (g_prompts_dirty.exchange(false, std::memory_order_relaxed)) RepaintPrompts(b);
}

// The old keyboard-and-mouse mode of the SDK (mnk_mode, mnk_mouse) makes every key a pad button; with the native keyboard that is Enter as
// Start and accept at once. It is forced off at start (larecomp_app.h); this puts it back if anything turns it on later. The change
// callback runs inside the cvar registry's lock, where the cvar cannot be set again, so it only leaves a note for the next input poll.
std::atomic<bool> g_mnk_turned_on{false};

void WatchMnk() {
    static std::once_flag once;
    std::call_once(once, [] {
        for (const char* name : {"mnk_mode", "mnk_mouse"})
            larecomp::cvar::RegisterChangeCallback(name, [](std::string_view, std::string_view value) {
                if (value != "false" && value != "0") g_mnk_turned_on.store(true, std::memory_order_relaxed);
            });
    });
}

void KeepMnkOff() {
    WatchMnk();
    if (!g_mnk_turned_on.exchange(false, std::memory_order_relaxed)) return;
    larecomp::cvar::SetFlagByName("mnk_mode", "false");
    larecomp::cvar::SetFlagByName("mnk_mouse", "false");
    MC_WARN("[kbnative] mnk_mode / mnk_mouse were turned on and are put back off: the old keyboard-and-mouse mode is retired "
            "(its keys would be pad buttons as well as native keys, Enter as Start and accept at once)");
}

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
uint32_t g_padkeys_gen = 0;

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
    if (g_game_entered || !larecomp::settings::keyboard_native.Get()) return;
    uint8_t* b = GuestBase();
    const uint32_t pad = static_cast<uint32_t>(r31.u64);
    if (!b || pad != 0x82905388u) return;  // pad 0 only
    const uint32_t gen = BindingsGen();
    if (gen != g_padkeys_gen) {
        g_padkeys_gen = gen;
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
    // With the keyboard off only the extra pad buttons are answered; a real pad button is
    // left to the game's own read.
    const bool keyboard = larecomp::settings::keyboard_native.Get();
    if (!keyboard && !ExtrasEnabled()) return false;
    uint8_t* b = GuestBase();
    const uint32_t out_type = static_cast<uint32_t>(r28.u64);
    if (!b || !HeapPtr(out_type)) return false;
    for (int sc = 1; sc < 256; ++sc) {
        if (!g_just_pressed[sc]) continue;
        if (!keyboard && ExtraIndexOfScancode(sc) < 0) continue;
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
    if (const int e = ExtraIndexOfScancode(sc); e >= 0) return kExtraName[e];
    if (const larecomp::kbm::MouseKey* mouse = larecomp::kbm::FindMouseKeyByScancode(sc)) return mouse->name;
    for (int vk = 1; vk < 255; ++vk) {
        if (b[kVkToScancode + vk] != sc) continue;
        const std::string n = rex::ui::VirtualKeyToString(static_cast<rex::ui::VirtualKey>(vk));
        if (!n.empty()) return n;
    }
    return {};
}

uint8_t ScancodeOfName(const uint8_t* b, const std::string& name) {
    for (int i = 0; i < kNumExtras; ++i)
        if (Lower(name) == Lower(kExtraName[i])) return kExtraScancode[i];
    for (const KeyRef& k : ParseKeyList(name)) {
        const uint8_t sc = ScancodeOf(b, k.vk);
        if (sc) return sc;
    }
    return 0;
}

bool Hook_ControlLabel(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5) {
    if (static_cast<uint32_t>(r3.u64) != 0) return false;  // pad / axis: the own labels of the game
    const uint32_t key = static_cast<uint32_t>(r4.u64);
    const bool extra = ExtraIndexOfScancode(key) >= 0;
    if (!extra && !larecomp::settings::keyboard_native.Get()) return false;
    uint8_t* b = GuestBase();
    if (!b || key == 0 || key > 255) return false;
    const uint32_t block = LabelBlock(b);
    if (!block) return false;
    const uint32_t flag = static_cast<uint32_t>(r5.u64);

    // A key (or an extra pad button) is drawn as a cap whichever glyph family is in use: the platform flag is pinned to 0, so the spare glyphs are never a PlayStation button and a pad
    // player who bound a key sees its cap in the row. The caps of the menu glyphs (the keyboard family) are found first, then a spare glyph.
    {
        // 1. A menu-button glyph already showing this key.
        for (const GlyphTarget& g : kGlyphs) {
            if (g.slot >= 0 || !g.icon) continue;
            auto it = g_glyph_tex.find(g.textures[0]);
            if (it == g_glyph_tex.end() || it->second.art.kind != larecomp::prompts::ArtKind::KeyCap) continue;
            if (ScancodeOfName(b, it->second.art.id) != key) continue;
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
            // A mouse button has no key cap yet (they come with the mouse feel work): its name as text beats a blank cap.
            if (larecomp::kbm::FindMouseKeyByScancode(static_cast<uint8_t>(key)) && !CapExists(name, 32, 32)) break;
            // Not scanned yet (or the dictionary has no such texture): drawing the slot
            // would show the PlayStation button, so the text name below it is.
            bool ready = false;
            for (const GlyphTarget& g : kGlyphs)
                if (g.slot == i) ready = g_glyph_tex.count(g.textures[0]) != 0;
            if (!ready) break;
            g_slot_key[i] = name;
            for (const GlyphTarget& g : kGlyphs)
                if (g.slot == i)
                    for (const char* tex : g.textures) PaintKey(b, tex, name);
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

namespace {
bool g_overlay_had_keys = false;  // an overlay had the keyboard at the last poll

// ── keyboard steering with a ramp (mouse_look_core.h StepSteer) ─────────────────────────────────────────────────────
// The steering keys are the ones of kb_drive_steer_left / _right (any key of a list counts: A and the left arrow both steer left). Once per input poll the ramp takes a step
// towards what the keys say; its value, as a stick axis, goes into g_key_steer for the XamInputGetState wrapper (KeySteerScope) to add to the left stick. The ramp is the
// road's: it is zero while a menu screen is up, before the game is entered, with an overlay on the keyboard and with kb_steer_ramp off.
std::atomic<int32_t> g_key_steer{0};
std::vector<uint8_t> g_steer_left_ids, g_steer_right_ids;
uint32_t g_steer_keys_gen = 0;
float g_steer_value = 0.0f;
int g_steer_log_lines = 0;

void RefreshSteerKeys() {
    const uint32_t gen = BindingsGen();
    if (gen == g_steer_keys_gen) return;
    g_steer_keys_gen = gen;
    g_steer_left_ids.clear();
    g_steer_right_ids.clear();
    for (const KeyRef& k : ParseKeyList(larecomp::settings::kb_drive_steer_left.Get())) g_steer_left_ids.push_back(k.vk);
    for (const KeyRef& k : ParseKeyList(larecomp::settings::kb_drive_steer_right.Get())) g_steer_right_ids.push_back(k.vk);
}

void UpdateSteerRamp(const larecomp::kbm::KeySnapshot& keys) {
    using Clock = std::chrono::steady_clock;
    static Clock::time_point last = Clock::now();
    const Clock::time_point now = Clock::now();
    const float dt = std::chrono::duration<float>(now - last).count();
    last = now;

    const bool road = larecomp::settings::kb_steer_ramp.Get() && !g_screen_menu.load(std::memory_order_relaxed);  // g_screen_menu: a menu screen is up or the game is not entered (PollPrompts, this poll)
    int dir = 0;
    if (road) {
        RefreshSteerKeys();
        bool left = false, right = false;
        for (uint8_t id : g_steer_left_ids) left = left || keys.Test(id);
        for (uint8_t id : g_steer_right_ids) right = right || keys.Test(id);
        dir = larecomp::mouselook::SteerDirection(left, right);
    }
    const float before = g_steer_value;
    if (!road) {
        g_steer_value = 0.0f;  // a menu opened, the ramp is off: nothing is left over to steer with
    } else {
        larecomp::mouselook::SteerParams params;
        params.rise_s = static_cast<float>(larecomp::settings::kb_steer_rise.Get());
        params.return_s = static_cast<float>(larecomp::settings::kb_steer_return.Get());
        g_steer_value = larecomp::mouselook::StepSteer(g_steer_value, dir, dt, params);
    }
    const int16_t axis = larecomp::mouselook::SteerToAxis(g_steer_value);
    g_key_steer.store(axis, std::memory_order_relaxed);
    if (larecomp::settings::kb_steer_log.Get() && (dir != 0 || before != 0.0f || g_steer_value != 0.0f) && g_steer_log_lines < 3000) {
        ++g_steer_log_lines;
        MC_INFO("[kbnative] steer dir={:+d} value={:+.3f} axis={} dt_ms={:.1f}", dir, g_steer_value, int(axis), dt * 1000.0f);
    }
}
}  // namespace

void Hook_KeyboardFeed() {
    uint8_t* b = GuestBase();
    if (!b) return;

    // The glyphs follow the device in use, whether or not the native keyboard is on: a few relaxed loads a poll, and a repaint only when something changed (PollPrompts).
    PollPrompts(b);

    EnsureKeyListener();
    KeepMnkOff();

    // The keys of this poll, from the window's events (once a poll: the taps since the last one are used up here). While an overlay has the
    // keyboard the game gets none, and what the latch held is let go: the overlay may have swallowed the key-up.
    const bool overlay = larecomp::ServerBrowserCapturesInput();
    if (overlay && !g_overlay_had_keys) g_keys.ReleaseAll();
    g_overlay_had_keys = overlay;
    larecomp::kbm::KeySnapshot keys = g_keys.Poll();
    if (overlay) keys = {};

    const bool keyboard = larecomp::settings::keyboard_native.Get();
    if (keyboard) FeedKeys(b, keys);
    // The pad's extra buttons work with or without the native keyboard.
    FeedExtras(b, keys);
    if (!keyboard) {
        g_steer_value = 0.0f;
        g_key_steer.store(0, std::memory_order_relaxed);
        return;
    }
    EnsureNavKeys(b);
    UpdateNavEntries(b);
    UpdateSteering(b);
    UpdateSteerRamp(keys);
    UpdateCameraJump(b, keys);
    if (g_drive_gen_applied != 0 && BindingsGen() != g_drive_gen_applied) {
        g_drive_gen_applied = BindingsGen();  // before: the rebuild below refreshes it again
        ReapplyDrive(b);
    }
}

namespace larecomp::keyboard {

bool MenuScreenUp() { return g_screen_menu.load(std::memory_order_relaxed); }
bool PhoneScreenUp() { return g_screen_phone.load(std::memory_order_relaxed); }

// The XamInputGetState wrapper has finished (the SDK's state, the dead zones, the device state machine's look at it): the keyboard's steering goes into the left stick's X axis.
KeySteerScope::~KeySteerScope() {
    const int32_t key = g_key_steer.load(std::memory_order_relaxed);
    if (key == 0 || !base_ || state_ == 0) return;
    const uint32_t u = user_ & 0xFF;
    if (u != 0 && u != 0xFF) return;
    if ((result_ & 0xFFFFFFFFull) != 0) return;  // no pad answered (a lost controller's "not connected", a failed call)
    if (larecomp::wheel::WheelPresent()) return;  // a wheel owns the axis
    uint8_t* p = base_ + state_ + 8;              // X_INPUT_STATE: be32 packet, be16 buttons, u8 LT, u8 RT, then be16 LX
    const int16_t stick = static_cast<int16_t>((uint16_t(p[0]) << 8) | p[1]);
    const int16_t merged = larecomp::mouselook::MergeAxis(stick, static_cast<int16_t>(key));
    p[0] = static_cast<uint8_t>(uint16_t(merged) >> 8);
    p[1] = static_cast<uint8_t>(uint16_t(merged));
}

}  // namespace larecomp::keyboard

// sub_822A4B80(controller): "SHIFT_UP and SHIFT_DOWN are both bound", read from the control map
// of the controller's pad as entries 4 and 5 of {u32 type, u32 key}. The game tests only the TYPE
// word, because a type of 0 never occurred on a bound action in its own maps; a keyboard (or
// extra pad button) source is type 0 with the scancode in the key word, so a shift bound to one
// read as "no shift controls" and the manual shift never fired. An entry counts as bound when
// either word is set. Maps the game ships have a non-zero type wherever something is bound, so
// they answer exactly as before.
extern "C" REX_FUNC(rex_sub_822A4B80) {
    const uint32_t pad = ReadGuestU32(base, ctx.r3.u32 + 4);
    const uint32_t map = pad < 4 ? ReadGuestU32(base, 0x8287438C + 4 * pad) : 0;
    bool both = map >= 0x10000;
    for (uint32_t entry = 4; both && entry <= 5; ++entry)
        both = ReadGuestU32(base, map + 8 * entry) != 0 || ReadGuestU32(base, map + 8 * entry + 4) != 0;
    ctx.r3.u64 = both ? 1 : 0;
}

#else  // REXGLUE_HAS_XEO3_TARGET

namespace larecomp::keyboard {
bool MenuScreenUp() { return true; }
bool PhoneScreenUp() { return false; }
KeySteerScope::~KeySteerScope() {}
}  // namespace larecomp::keyboard

void Hook_KeyboardFeed() {}
void Hook_ControlMapRebuilt(PPCRegister&) {}
void Hook_PadBitsFinalize(PPCRegister&) {}
bool Hook_ControlCaptureKey(PPCRegister&, PPCRegister&) { return false; }
bool Hook_ControlLabel(PPCRegister&, PPCRegister&, PPCRegister&) { return false; }

#endif  // REXGLUE_HAS_XEO3_TARGET
