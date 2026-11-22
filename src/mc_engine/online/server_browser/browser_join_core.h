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

// ---------------------------------------------------------------------------------------------------------- the player's car
//
// WHY (measured, 2026-11-21, run ojnew-cli: --join_server=127.0.0.1:41000, no pause menu anywhere). The join waited for "the
// city", and the front-end city behind the TITLE screen ("PRESS START") passed that test: the player's racer exists there, the
// chain the online menus' fill reads (racer+880 -> +16 -> +48) is there, but the racer has NO CAR yet (racer+48, the car entity, is
// 0). The join went to the "Online" state, whose onenter runs the flash command setMenuCam; its slot picker (sub_82654960) probes
// the shot with sub_82654738, which loads racer+48 and then reads entity+28 without a null check (`lwz r31,28(r8)` with r8 = 0):
// an access violation, "unhandled title read of 0x0000001C" on the Main XThread, mcla.exe+0x240536D. The stack: GotoState >
// the HSM goto (sub_82224678) > sub_82691300 > sub_8268DD70 > sub_8268CD08 > sub_82220FE0 (flash command 13) > sub_82654960 >
// sub_82654738.
//
// So the join starts only when the player's car exists: racer+48 -> entity, entity+28 -> matrix (the pose chain the Director's
// car registry reads, cinematic/cars.cpp ReadCarMatrix), and the matrix is readable. The test is pure: the caller hands in how
// a guest word is read (and says no for memory that is not mapped), so a unit test can walk it over a fake memory.
constexpr uint32_t kRacerEntityAt = 48;
constexpr uint32_t kEntityMatrixAt = 28;
constexpr uint32_t kMatrixRowsAt = 16;  // the 16 floats of the pose start here (the picker's probe loads them)

constexpr bool PlausibleGuestPtr(uint32_t ea) { return ea >= 0x10000u && ea < 0xFFFF0000u; }

// `read_word(address, &value)` returns false when the 4 bytes at `address` cannot be read.
template <typename ReadWord>
bool PlayerCarReady(uint32_t racer, ReadWord read_word) {
    if (!PlausibleGuestPtr(racer)) return false;
    uint32_t entity = 0, matrix = 0, row = 0;
    if (!read_word(racer + kRacerEntityAt, &entity) || !PlausibleGuestPtr(entity)) return false;
    if (!read_word(entity + kEntityMatrixAt, &matrix) || !PlausibleGuestPtr(matrix)) return false;
    return read_word(matrix + kMatrixRowsAt, &row);
}

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
