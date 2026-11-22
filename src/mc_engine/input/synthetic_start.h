#pragma once
//
// A press of the pad's Start button that nobody made on a pad. --join_server opens the game's pause menu with it, the way a player does
// (online/server_browser/join_server.cpp, PauseOpener in join_server_core.h).
//
// WHY. Every join that worked was made in a game whose pause menu had been opened (and the page of it used) before the join started; the
// --join_server runs never opened it, and the game dropped their search (JOIN-SERVER-ROOT.md). The pause menu is opened by the game's own
// reading of Start in free roam, so this is that reading: the pad poll the game makes (XamInputGetState, wheel/wheel_fake.cpp) gets the Start
// bit OR'd into the state it is about to read, for a few polls. The test harness's pad does the same at the same place, and the pause menu
// it opens is the one the loopback scenes joined from.
//
// It is put in AFTER the active-device rules have looked at the pad (the scope is constructed before InputActivityScope, so its destructor
// runs after that one's): a press the program made is not a controller's input, and must not turn a keyboard player's glyphs into the Xbox
// set. It is skipped for a poll that failed (no pad answered: the pad-loss window answers "not connected" on purpose, to make the game pause).
//
// Pure apart from the memory write: the clock is handed in and the guest memory is a plain byte pointer, so join_server_core_test.cpp checks
// it without a game.
//
#include <atomic>
#include <chrono>
#include <cstdint>

namespace larecomp::input {

constexpr uint16_t kXInputStart = 0x0010;  // XINPUT_GAMEPAD_START

// How long a requested press is held: at least this many polls of user 0 AND at least this long, whichever ends later. The game steps its
// pause check once per game update, which at a high frame rate is several frames; a press shorter than an update would not be seen (the
// harness's "press start 4f" is four game updates, and worked).
class SyntheticStart {
public:
    static constexpr int kMinPolls = 6;
    static constexpr int64_t kMinMs = 120;

    // Any thread. A press asked for while one is going on starts over.
    void Request(int64_t now_ms) {
        until_ms_.store(now_ms + kMinMs, std::memory_order_relaxed);
        polls_left_.store(kMinPolls, std::memory_order_release);
        requests_.fetch_add(1, std::memory_order_relaxed);
        pending_.store(true, std::memory_order_release);
    }

    // One poll of user 0 that found a pad: true = this poll shows Start held.
    bool OnPoll(int64_t now_ms) {
        int left = polls_left_.load(std::memory_order_acquire);
        while (left > 0 && !polls_left_.compare_exchange_weak(left, left - 1, std::memory_order_acq_rel)) {
        }
        if (left > 0) return true;
        if (now_ms < until_ms_.load(std::memory_order_relaxed)) return true;
        pending_.store(false, std::memory_order_release);  // over: the polls that follow do not even read the clock
        return false;
    }

    // A press was asked for and has not been seen to end: the only thing the common poll (nobody asked for anything) looks at.
    // (A request that arrives while the last poll clears the flag still has its polls counted, so it is not lost.)
    bool Pending() const { return pending_.load(std::memory_order_acquire) || polls_left_.load(std::memory_order_acquire) > 0; }

    // Whether a press is going on as of `now_ms` (what OnPoll would answer, without using up a poll).
    bool Held(int64_t now_ms) const {
        return polls_left_.load(std::memory_order_acquire) > 0 || now_ms < until_ms_.load(std::memory_order_relaxed);
    }
    int Requests() const { return requests_.load(std::memory_order_relaxed); }

    // The packet number a held poll carries: a counter that goes up with each one, far from the small numbers the SDK's own state has, so
    // that a game that skips a state whose packet number did not change sees every one of ours as new.
    uint32_t NextPacket() { return 0x7F000000u + (packet_.fetch_add(1, std::memory_order_relaxed) & 0x00FFFFFFu); }

private:
    std::atomic<int> polls_left_{0};
    std::atomic<int64_t> until_ms_{0};
    std::atomic<int> requests_{0};
    std::atomic<bool> pending_{false};
    std::atomic<uint32_t> packet_{1};
};

// The one instance (a local static of a type with no destructor to run: nothing to make immortal at exit).
inline SyntheticStart& Synthetic() {
    static SyntheticStart s;
    return s;
}

inline int64_t SyntheticNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Asks for a press of Start (any thread). It reaches the game with its next polls of user 0.
inline void PressPadStart() { Synthetic().Request(SyntheticNowMs()); }

// XINPUT_STATE in guest memory: dwPacketNumber (u32) at +0 and wButtons (u16) at +4, both big endian (the guest's byte order).
inline void PutStartInState(uint8_t* base, uint32_t state, uint32_t packet) {
    uint8_t* s = base + state;
    s[0] = uint8_t(packet >> 24);
    s[1] = uint8_t(packet >> 16);
    s[2] = uint8_t(packet >> 8);
    s[3] = uint8_t(packet);
    const uint16_t buttons = uint16_t(((s[4] << 8) | s[5]) | kXInputStart);
    s[4] = uint8_t(buttons >> 8);
    s[5] = uint8_t(buttons);
}

// What one poll does: user 0 (or the "any user" index 0xFF), a state to fill, a pad that answered (`result` 0), and a press that is going on.
// Returns whether it put Start in.
inline bool ApplySyntheticStart(SyntheticStart& press, int64_t now_ms, uint8_t* base, uint32_t user, uint32_t state, uint64_t result) {
    if (state == 0 || base == nullptr || result != 0) return false;
    const uint32_t u = user & 0xFF;
    if (u != 0 && u != 0xFF) return false;
    if (!press.OnPoll(now_ms)) return false;
    PutStartInState(base, state, press.NextPacket());
    return true;
}

// On the stack of the XamInputGetState wrapper (wheel/wheel_fake.cpp), constructed BEFORE InputActivityScope: the destructors run in the
// opposite order, so this one runs when the activity rules have already looked at the state the game is about to read.
class SyntheticStartScope {
public:
    SyntheticStartScope(uint8_t* base, uint32_t user, uint32_t state, uint64_t& result)
        : base_(base), user_(user), state_(state), result_(result) {}
    ~SyntheticStartScope() {
        if (!Synthetic().Pending()) return;  // the common poll: nobody asked for a press, or the last one is over
        ApplySyntheticStart(Synthetic(), SyntheticNowMs(), base_, user_, state_, result_);
    }
    SyntheticStartScope(const SyntheticStartScope&) = delete;
    SyntheticStartScope& operator=(const SyntheticStartScope&) = delete;

private:
    uint8_t* base_;
    uint32_t user_;
    uint32_t state_;
    uint64_t& result_;
};

}  // namespace larecomp::input
