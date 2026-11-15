#pragma once
// The pure parts of the always-on native keyboard (keyboard_native.cpp) and of the neutral pad: no SDK, no guest memory, no
// window, so larecomp_kbm_input_test (kbm_input_test.cpp) can run them without the game.
//
//  KeyLatch        the keys and the mouse as one table of 256 ids, fed by WINDOW EVENTS (key down / key up, mouse button, wheel) on the UI
//                  thread and read once per input poll on the game thread. Nothing is polled from the OS: no GetAsyncKeyState, no focus
//                  call, so it works the same on Windows, Linux and the Steam Deck. A key that goes down and up again between two polls
//                  is not lost: it is held for exactly one poll (the "pressed since the last poll" latch). Idea of the audit INPUT-EVENTS-AUDIT.md item 2.
//  mouse keys      the five mouse buttons and the two wheel directions as keys with scancodes of their own, so the game's own Options >
//                  Controller > Customize can bind them and kb_ui_* / kb_drive_* can name them (LMB, RMB, MMB, MB4, MB5, WheelUp, WheelDown).
//  NeutralPadRule  what user 0's pad is answered while the SDK says "not connected" (the rule is written down below).
//
// KeyLatch is lock free (a few atomics); NeutralPadRule takes a mutex (two calls a frame).

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
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
            presses_.fetch_add(1, std::memory_order_acq_rel);
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
        presses_.fetch_add(1, std::memory_order_acq_rel);
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
    // How many times a key or button went down (auto-repeat excluded) and the wheel turned, since the start. The neutral pad rule watches it
    // to see "the player pressed something".
    uint32_t PressCount() const { return presses_.load(std::memory_order_acquire); }

private:
    static bool Valid(int id) { return id > 0 && id < kKeyIdCount - 1; }
    static size_t Word(int id) { return size_t(id) >> 6; }
    static uint64_t Mask(int id) { return uint64_t(1) << (id & 63); }

    std::array<std::atomic<uint64_t>, 4> held_{};
    std::array<std::atomic<uint64_t>, 4> tapped_{};
    std::atomic<uint32_t> presses_{0};
};

// ---------------------------------------------------------------------------------------------------------------------------
// The neutral pad
//
// The game opens its pause menu on its own when user 0's pad stops answering (KBM-P0 P0c: in a race, with the pad "not connected" for
// 10 s, the plain pause menu appears at once, no message, and stays; the title waits at PRESS START). A player who has only a keyboard
// would meet that all the time, so the guest is told that a pad is connected and at rest. The rule, in full:
//
//   * the SDK answers "connected": that answer stands, and a pad has been seen.
//   * the SDK answers "not connected" and NO pad has been seen since the start (or since the player took the keyboard over, below):
//     the guest is told "connected, neutral": nothing pauses, the keys play.
//   * the SDK answers "not connected" and a pad WAS here (it was unplugged, ran flat, lost its Bluetooth): the guest is told the truth, so the
//     game pauses as it does in other PC games and the player sees why.
//   * from then on, the first key or mouse button the player presses (a Down or a wheel notch, not an auto-repeat) is the player choosing
//     the keyboard: the neutral pad takes the place of the lost one, the game sees a pad again and the Enter that picks CONTINUE
//     resumes it. If a real pad comes back, it stands again, and losing it again is a new pause.
//
// Which key counts is the caller's: it passes KeyLatch::PressCount(), and the rule only compares it with the count at the moment the pad
// was lost. Guest users other than 0 and the wheel are not the rule's business.
// ---------------------------------------------------------------------------------------------------------------------------
enum class PadAnswer {
    kSdk,      // the SDK's own answer (connected, or the truth: not connected)
    kNeutral,  // "connected, gamepad, nothing pressed"
};

enum class PadSeen {
    kNever,  // no pad since the start: a keyboard player
    kHere,   // a pad is connected
    kGone,   // a pad was here and is not: the game has been told
};

class NeutralPadRule {
public:
    PadAnswer Decide(bool sdk_connected, uint32_t key_presses) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (sdk_connected) {
            seen_ = PadSeen::kHere;
            return PadAnswer::kSdk;
        }
        switch (seen_) {
            case PadSeen::kNever:
                return PadAnswer::kNeutral;
            case PadSeen::kHere:
                seen_ = PadSeen::kGone;
                lost_at_ = key_presses;
                return PadAnswer::kSdk;
            case PadSeen::kGone:
                if (key_presses != lost_at_) {
                    seen_ = PadSeen::kNever;  // the player took the keyboard over
                    return PadAnswer::kNeutral;
                }
                return PadAnswer::kSdk;
        }
        return PadAnswer::kSdk;
    }

    PadSeen State() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return seen_;
    }

private:
    mutable std::mutex mutex_;
    PadSeen seen_ = PadSeen::kNever;
    uint32_t lost_at_ = 0;
};

}  // namespace larecomp::kbm
