#pragma once
// The pure parts of the always-on native keyboard (keyboard_native.cpp): no SDK, no guest memory, no window, so
// larecomp_kbm_input_test (kbm_input_test.cpp) can run them without the game.
//
//  KeyLatch        the keys and the mouse as one table of 256 ids, fed by WINDOW EVENTS (key down / key up, mouse button, wheel) on the UI
//                  thread and read once per input poll on the game thread. Nothing is polled from the OS: no GetAsyncKeyState, no focus
//                  call, so it works the same on Windows, Linux and the Steam Deck. A key that goes down and up again between two polls
//                  is not lost: it is held for exactly one poll (the "pressed since the last poll" latch). Idea of the audit INPUT-EVENTS-AUDIT.md item 2.
//  mouse keys      the five mouse buttons and the two wheel directions as keys with scancodes of their own, so the game's own Options >
//                  Controller > Customize can bind them and kb_ui_* / kb_drive_* can name them (LMB, RMB, MMB, MB4, MB5, WheelUp, WheelDown).
//
// KeyLatch is lock free (a few atomics).
//
// There is no "neutral pad" here: with no controller the game still sees user 0 connected and at rest, because the SDK's own NOP
// stand-in ("None", rex::input::nop::NopInputDriver, always the last driver of CreateDefaultInputSystem, assigned to user 0 by
// SharedAssignment, idle state, caps of a gamepad) answers for it. Nothing in LARecomp has to fake a pad for a keyboard player.

#include <array>
#include <atomic>
#include <cstdint>
#include <string_view>

namespace larecomp::kbm {

// ---------------------------------------------------------------------------------------------------------------------------
// Key ids. 1..254 are the Windows virtual-key numbers the SDK's rex::ui::VirtualKey uses (the game's VK -> scancode table is indexed by
// them); 0 and 255 are never used. The mouse takes ids no real key event produces: the five buttons keep their VK numbers (1 2 4 5 6),
// the wheel takes 7 and 0x0A, which Windows leaves undefined.
// ---------------------------------------------------------------------------------------------------------------------------
constexpr int kKeyIdCount = 256;

struct MouseKey {
    const char* name;     // the canonical name (also what the settings and Customize show)
    int id;               // the id in KeyLatch
    uint8_t scancode;     // the scancode the game's key table gets for it
};

// The scancodes: the game's table byte_82825050 uses none of 0x80..0x86 (KBM-P0 P0d: free codes 67-9C 9E-B4 ...), and the four back
// paddles (0x7B-0x7E) and the camera jump (0x7F) are right below them (keyboard_native.cpp).
constexpr MouseKey kMouseKeys[] = {
    {"LMB", 0x01, 0x80},
    {"RMB", 0x02, 0x81},
    {"MMB", 0x04, 0x82},
    {"MB4", 0x05, 0x83},
    {"MB5", 0x06, 0x84},
    {"WheelUp", 0x07, 0x85},
    {"WheelDown", 0x0A, 0x86},
};
constexpr int kMouseKeyCount = int(sizeof(kMouseKeys) / sizeof(kMouseKeys[0]));
constexpr int kKeyWheelUp = 0x07;
constexpr int kKeyWheelDown = 0x0A;

inline const MouseKey* FindMouseKeyById(int id) {
    for (const MouseKey& k : kMouseKeys)
        if (k.id == id) return &k;
    return nullptr;
}

inline const MouseKey* FindMouseKeyByScancode(uint8_t scancode) {
    for (const MouseKey& k : kMouseKeys)
        if (k.scancode == scancode) return &k;
    return nullptr;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------
// The menu's buttons as keys of their own, for the mouse in the menus (menu_mouse.cpp, KBM-DESIGN.md phase P6): a click is ACCEPT, the right button BACK, the wheel and the hover's
// walk the arrows. The game's menu mapper (keyboard_native.cpp EnsureNavKeys) gets one entry per button pointing at the SAME output the pad's button writes, so what the pointer
// does is exactly what the button does (its sound, its transition, the form's own handler), and the keys of kb_ui_* are not involved: a click on empty space is no key. No keyboard
// produces these ids: 0x97..0x9C are Windows virtual-key numbers that nothing uses (0x88..0x8F are the gamepad navigation keys, 0x96 and 0xA0 the neighbours); the scancodes
// 0x87..0x8C follow the mouse keys' and are free in the game's table too (KBM-P0 P0d).
// ---------------------------------------------------------------------------------------------------------------------------------------------------------
enum class MenuKey : uint8_t { Accept = 0, Back, Up, Down, Left, Right };
constexpr int kMenuKeyCount = 6;

struct MenuKeyDef {
    const char* name;
    MenuKey key;
    int id;             // the id in KeyLatch
    uint8_t scancode;   // the scancode the game's key table gets for it
};
constexpr MenuKeyDef kMenuKeys[kMenuKeyCount] = {
    {"MenuAccept", MenuKey::Accept, 0x97, 0x87},
    {"MenuBack", MenuKey::Back, 0x98, 0x88},
    {"MenuUp", MenuKey::Up, 0x99, 0x89},
    {"MenuDown", MenuKey::Down, 0x9A, 0x8A},
    {"MenuLeft", MenuKey::Left, 0x9B, 0x8B},
    {"MenuRight", MenuKey::Right, 0x9C, 0x8C},
};

inline const MenuKeyDef* FindMenuKeyById(int id) {
    for (const MenuKeyDef& k : kMenuKeys)
        if (k.id == id) return &k;
    return nullptr;
}

inline int MenuKeyId(MenuKey key) { return kMenuKeys[int(key)].id; }

namespace detail {
inline char Low(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }
inline bool SameName(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (Low(a[i]) != Low(b[i])) return false;
    return true;
}
}  // namespace detail

// A name of a mouse key as the player writes it in a binding list, case-insensitive. The canonical names and a few spellings: LButton,
// RButton, MButton, XButton1, XButton2, X1, X2, Mouse1..Mouse5, MouseWheelUp, MouseWheelDown. Null when it is not one.
inline const MouseKey* FindMouseKeyByName(std::string_view name) {
    struct Alias {
        const char* name;
        int id;
    };
    constexpr Alias kAliases[] = {
        {"LMB", 0x01}, {"RMB", 0x02}, {"MMB", 0x04}, {"MB4", 0x05}, {"MB5", 0x06}, {"WheelUp", 0x07}, {"WheelDown", 0x0A},
        {"LButton", 0x01}, {"RButton", 0x02}, {"MButton", 0x04}, {"XButton1", 0x05}, {"XButton2", 0x06}, {"X1", 0x05}, {"X2", 0x06},
        {"Mouse1", 0x01}, {"Mouse2", 0x02}, {"Mouse3", 0x04}, {"Mouse4", 0x05}, {"Mouse5", 0x06},
        {"MouseWheelUp", 0x07}, {"MouseWheelDown", 0x0A},
    };
    for (const Alias& a : kAliases)
        if (detail::SameName(name, a.name)) return FindMouseKeyById(a.id);
    return nullptr;
}

// ---------------------------------------------------------------------------------------------------------------------------
// What the game thread reads: the held keys plus the ones tapped since the last poll, as a bit set over the ids.
// ---------------------------------------------------------------------------------------------------------------------------
struct KeySnapshot {
    std::array<uint64_t, 4> bits{};

    bool Test(int id) const { return id > 0 && id < kKeyIdCount && ((bits[size_t(id) >> 6] >> (id & 63)) & 1u) != 0; }
    bool Any() const { return (bits[0] | bits[1] | bits[2] | bits[3]) != 0; }
    // Calls fn(id) for every id that is set, ascending.
    template <typename Fn>
    void ForEach(Fn&& fn) const {
        for (size_t w = 0; w < bits.size(); ++w) {
            uint64_t word = bits[w];
            while (word) {
                int bit = 0;
                while (((word >> bit) & 1u) == 0) ++bit;
                word &= word - 1;
                fn(int(w * 64) + bit);
            }
        }
    }
};

class KeyLatch {
public:
    // ---- UI thread (window events) ----
    // A key or mouse button went down. `repeat` = the OS's auto-repeat of a key that was already down: it keeps the key held (it also
    // brings a key back that an overlay's swallowed key-up left stuck and ReleaseAll let go) but is not a new press.
    void Down(int id, bool repeat) {
        if (!Valid(id)) return;
        const uint64_t mask = Mask(id);
        const size_t w = Word(id);
        if (!repeat) {
            tapped_[w].fetch_or(mask, std::memory_order_acq_rel);
        }
        held_[w].fetch_or(mask, std::memory_order_acq_rel);
    }
    void Up(int id) {
        if (!Valid(id)) return;
        held_[Word(id)].fetch_and(~Mask(id), std::memory_order_acq_rel);
    }
    // A wheel notch: no up event follows, so it is down for exactly the next poll.
    void Pulse(int id) {
        if (!Valid(id)) return;
        tapped_[Word(id)].fetch_or(Mask(id), std::memory_order_acq_rel);
    }
    // The window lost the focus, or an overlay took the keyboard: nothing is down, nothing is waiting. (A key still held on the keyboard
    // comes back with its next auto-repeat.)
    void ReleaseAll() {
        for (size_t w = 0; w < held_.size(); ++w) {
            held_[w].store(0, std::memory_order_release);
            tapped_[w].store(0, std::memory_order_release);
        }
    }

    // ---- game thread, once per input poll ----
    // The keys held now plus the ones tapped since the last Poll; the taps are used up. A key tapped (down and up) between two polls is
    // reported once, by the poll after it.
    KeySnapshot Poll() {
        KeySnapshot s;
        for (size_t w = 0; w < s.bits.size(); ++w) {
            const uint64_t tapped = tapped_[w].exchange(0, std::memory_order_acq_rel);
            s.bits[w] = tapped | held_[w].load(std::memory_order_acquire);
        }
        return s;
    }

    // ---- any thread ----
    bool Held(int id) const { return Valid(id) && (held_[Word(id)].load(std::memory_order_acquire) & Mask(id)) != 0; }

private:
    static bool Valid(int id) { return id > 0 && id < kKeyIdCount - 1; }
    static size_t Word(int id) { return size_t(id) >> 6; }
    static uint64_t Mask(int id) { return uint64_t(1) << (id & 63); }

    std::array<std::atomic<uint64_t>, 4> held_{};
    std::array<std::atomic<uint64_t>, 4> tapped_{};
};

}  // namespace larecomp::kbm
