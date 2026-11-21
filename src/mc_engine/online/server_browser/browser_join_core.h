#pragma once
//
// What a join does about an open pause menu (browser_join.cpp). Pure (no SDK, no guest): browser_join_core_test.cpp compiles
// this header alone.
//
// WHY. A join drives the game's own menu machine: it goes to the net menu ("LAN" / "Online"), sets the options the confirm
// screen would set, and goes to the search. That works from free roam, where nothing of the pause screen is loaded. With the
// pause menu open (the server browser is opened from it: SYSTEM LINK > SERVER BROWSER, or F6 over it) the machine is already
// inside the pause screen's own states, and a goto from there tears down the pause screen's UI objects while the game still
// holds them: Mz, 2026-11-21, "if you press join while in pause menu, the game crashes". The same row of the code already had
// to skip the goto when the machine sat in the target state (it read a freed UIObject, guest 0x50).
//
// So a join does not start while the pause menu is up: it closes it first the way the game's own PM_Continue does, waits until
// the game runs again plus a short settle, and only then starts from free roam, the path that works. This class is that wait,
// with the clock handed in so it can be checked without a game.
//
#include <cstdint>

namespace larecomp::browser {

class PauseGate {
public:
    enum class Step {
        kProceed,  // the game runs (and has settled): start the join
        kClose,    // ask the game to close the pause menu now
        kWait,     // closing, or waiting for the player to
    };

    // A close request that did not unpause the game is repeated this often, at most kMaxCloseTries times in all; after
    // that only the player can close it, and the join waits (NeedsPlayer).
    static constexpr int kMaxCloseTries = 3;
    static constexpr int64_t kRetryMs = 1500;
    // After the game is running again: the close's own transition (HUD back, menu movie freed) is given this long.
    static constexpr int64_t kSettleMs = 300;

    // Once per frame while a join waits to start. `paused`: the game's paused flag is set (the pause menu holds the game).
    Step Update(bool paused, int64_t now_ms) {
        if (paused) {
            seen_paused_ = true;
            settle_until_ms_ = -1;
            if (tries_ < kMaxCloseTries && (tries_ == 0 || now_ms >= next_try_ms_)) {
                ++tries_;
                next_try_ms_ = now_ms + kRetryMs;
                return Step::kClose;
            }
            return Step::kWait;
        }
        if (!seen_paused_) return Step::kProceed;  // it never was up
        // Up before, running now (our close or the player's): let the transition finish.
        if (settle_until_ms_ < 0) settle_until_ms_ = now_ms + kSettleMs;
        if (now_ms < settle_until_ms_) return Step::kWait;
        Reset();
        return Step::kProceed;
    }

    // Close requests sent so far.
    int tries() const { return tries_; }
    // Our requests are used up and the game is still paused: the sentence for the player is "close the pause menu".
    bool NeedsPlayer(int64_t now_ms) const { return seen_paused_ && tries_ >= kMaxCloseTries && now_ms >= next_try_ms_; }
    // Whether the pause menu was seen up since the last Reset (a join that waited on it).
    bool SawPause() const { return seen_paused_; }

    void Reset() { *this = PauseGate{}; }

private:
    int tries_ = 0;
    int64_t next_try_ms_ = 0;
    int64_t settle_until_ms_ = -1;
    bool seen_paused_ = false;
};

}  // namespace larecomp::browser
