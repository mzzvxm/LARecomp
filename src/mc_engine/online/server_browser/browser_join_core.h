#pragma once
//
// What a join does about an open pause menu, about where the game is, and about the net menu's page (browser_join.cpp). Pure (no SDK,
// no guest): browser_join_core_test.cpp compiles this header alone.
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
// WHAT THE FIRST FIX (e5c89c65) LEFT OPEN, found by reading the join and the game's own code again (2026-11-22; none of it could
// be run when this was written):
//   1. "The pause menu is up" was read from the game's paused flag alone. In an online session the pause menu does not pause the
//      game (a local pause), so the flag is 0 with the pause screen open: the join then went straight to the goto from inside the
//      pause screen, the crash path the fix was for. The pause SCREEN (mcPauseForm's open byte, form 71) is the fact; the flag and
//      the screen's movie only add to it (Look below).
//   2. PM_Continue (sub_8265F428) closes form 71 and goes to the in-game state from wherever the machine is. It was called whenever
//      the paused flag was set, also when the game was paused for another reason (the full map, a toolkit pause): there it moves
//      the machine out of a state it knows nothing about. It is asked for only while the pause screen is open.
//   3. After PM_Continue the join waited 300 ms of wall clock. The close unloads the pause movie through the game's own queue,
//      which advances per frame: on a slower PC 300 ms is a few frames, and the goto into the net menu (which loads the same
//      movie again) can meet the unload half done, the "UIObject that reload freed" of the first crash. The settle is counted in
//      frames as well as milliseconds and ends only when the pause movie is gone.
//   4. The net menu (LAN / Online) is a PAUSE page: the goto from free roam opens the pause movie again (measured, run
//      ojnew-pause: gotoWait + PAUSEMOVIE on the goto), and the search state was entered 17 ms later, on the next frame, while the
//      page was still loading. A player presses the row a few hundred milliseconds after the page is up. The search now waits for
//      the page (the machine in it, its movie up, a number of frames and a quarter of a second): PageWait.
//   5. Nothing checked WHERE the game is. A join from a race, the garage or from an online session the game is already in goes
//      into the net menu from states the game never does that from ("a goto from them leaves the machine half-built",
//      join_server.cpp); --join_server already waited for free roam, the browser did not. RefuseStart says no to those with a
//      sentence instead of a goto.
//   6. Closing the pause menu and going to the page again is not what the game does, and the logs of the loopback runs show what
//      it costs: with the pause screen open on SYSTEM LINK the join searched from the page (no goto), the game hosted and the pause
//      screen went by itself, the HUD back (ojold-pause); with the menu closed first and the page entered again, the SYSTEM LINK
//      page was left open over the world (ojnew-pause), and from plain free roam the search was not entered at all, the relay never
//      saw the game and --join_server still reported "done" (ojnew-cli). ChoosePath keeps the game's own start whenever it applies;
//      SearchVerify asks again for a search that was dropped; "done" needs a session (SystemLinkSessionUp).
//   7. A modal message box on screen. The game's first one after Start is up for about 9 s, past the moment the machine reaches
//      "Cruising", and a goto into the net menu under it never got its search going (the --join_server runs; join_server_core.h,
//      QuietGate). A join clicked in the F6 browser while the city loads reaches that moment too, so it waits for a box that is up
//      (QuietGate::Allows: none seen = no wait) and cancels with the reason message_box at its deadline.
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

// ------------------------------------------------------------------------------------------------------ where the game is
//
// The game's own state machine says where it is: free roam is "Cruising", a race "Racing...", the garage "Garage" (discord_rpc's
// RpcHook_StateActivate keeps the last of them; a pause menu in free roam is still free roam, the pause states are not among them).
// A join is started from free roam only: the net menu's goto is the game's own from there (the pause menu's MODES page opens it),
// and from a race or the garage it leaves the machine half-built. The title screen and the loads on the way to the city are other
// states again; a join that finds itself there waits (it has its 120 s), it does not fail.
enum class Activity {
    kOther,     // the title, a load, the credits, nothing known yet: wait for free roam
    kFreeRoam,  // "Cruising"
    kRace,      // "Racing..."
    kGarage,    // "Garage"
};

enum class Refusal {
    kNone,
    kRace,       // a race is on (also with its pause menu up)
    kGarage,     // the garage is open
    kInSession,  // the game is already in an online session (hosting or joined); sub_8226BE68 says so
};

// Whether a join that wants to start now must be refused, and why. `in_session`: the game's "session up at all" answer.
constexpr Refusal RefuseStart(Activity activity, bool in_session) {
    if (activity == Activity::kRace) return Refusal::kRace;
    if (activity == Activity::kGarage) return Refusal::kGarage;
    if (in_session) return Refusal::kInSession;
    return Refusal::kNone;
}

// Where a join may start from: free roam. (Anything else that is not refused is waited for.)
constexpr bool AtFreeRoam(Activity activity) { return activity == Activity::kFreeRoam; }

// ------------------------------------------------------------------------------------------------------ the pause menu
//
// What one look at the game shows about the pause menu.
struct Look {
    bool pause_screen = false;  // the pause screen (mcPauseForm, form 71) is open: the thing PM_Continue closes
    bool paused = false;        // the game's paused flag (Game_IsPaused): the world stands still, whoever paused it
    bool pause_movie = false;   // the UI's current movie is PAUSEMOVIE: the pause pages (and the net menu) draw in it
};

class PauseGate {
public:
    enum class Step {
        kProceed,  // the game runs (and has settled): start the join
        kClose,    // ask the game to close the pause menu now (only ever returned while the pause screen is open)
        kWait,     // closing, settling, or waiting for the player
    };

    // A close request that did not close the screen is repeated this often, at most kMaxCloseTries times in all; after that only
    // the player can close it, and the join waits (NeedsPlayer).
    static constexpr int kMaxCloseTries = 3;
    static constexpr int64_t kRetryMs = 2000;
    // A pause screen found open is given this long (and this many looks) before the first request: it may have been opened a
    // moment ago, with its movie still loading, and the close would meet that load.
    static constexpr int64_t kOpenSettleMs = 300;
    static constexpr int kOpenSettleFrames = 10;
    // After the pause is gone (our close or the player's): the close's own transition (HUD back, the movie unloaded through the
    // game's per-frame queue) gets this much time AND this many frames, whichever is longer, and the pause movie must be gone,
    // or kMovieWaitMs must have passed.
    static constexpr int64_t kSettleMs = 300;
    static constexpr int kSettleFrames = 20;
    static constexpr int64_t kMovieWaitMs = 1500;

    // Once per frame while a join waits to start.
    Step Update(const Look& look, int64_t now_ms) {
        ++frames_;
        const bool blocked = look.pause_screen || look.paused;
        last_blocked_ = blocked;
        last_screen_ = look.pause_screen;
        if (blocked) {
            seen_ = true;
            settle_ms_ = -1;
            if (look.pause_screen) {
                if (screen_ms_ < 0) {
                    screen_ms_ = now_ms;
                    screen_frame_ = frames_;
                }
                const bool fresh = now_ms - screen_ms_ < kOpenSettleMs || frames_ - screen_frame_ < kOpenSettleFrames;
                if (fresh) return Step::kWait;
                if (tries_ < kMaxCloseTries && (tries_ == 0 || now_ms >= next_try_ms_)) {
                    ++tries_;
                    next_try_ms_ = now_ms + kRetryMs;
                    return Step::kClose;
                }
            }
            return Step::kWait;  // paused with no pause screen (the map, a toolkit pause): nothing of ours to close
        }
        screen_ms_ = -1;  // the screen is gone (a later one is "fresh" again)
        if (!seen_) {
            if (!look.pause_movie) return Step::kProceed;  // it never was up
            seen_ = true;                                  // the player's own close is going on right now: let it finish
        }
        // Up before, running now (our close or the player's): let the transition finish.
        if (settle_ms_ < 0) {
            settle_ms_ = now_ms;
            settle_frame_ = frames_;
        }
        const bool movie_gone = !look.pause_movie || now_ms - settle_ms_ >= kMovieWaitMs;
        if (now_ms - settle_ms_ < kSettleMs || frames_ - settle_frame_ < kSettleFrames || !movie_gone) return Step::kWait;
        Reset();
        return Step::kProceed;
    }

    // Close requests sent so far.
    int tries() const { return tries_; }
    // The player has to do it: our requests are used up and the pause screen is still open, or the game is paused with no pause
    // screen to close. The sentence for the player is "close the pause menu".
    bool NeedsPlayer(int64_t now_ms) const {
        if (!seen_ || !last_blocked_) return false;
        if (!last_screen_) return true;
        return tries_ >= kMaxCloseTries && now_ms >= next_try_ms_;
    }
    // Whether the pause menu was seen up since the last Reset (a join that waited on it).
    bool SawPause() const { return seen_; }

    void Reset() { *this = PauseGate{}; }

private:
    int tries_ = 0;
    int64_t next_try_ms_ = 0;
    int64_t settle_ms_ = -1;
    int64_t screen_ms_ = -1;
    int frames_ = 0;
    int settle_frame_ = 0;
    int screen_frame_ = 0;
    bool seen_ = false;
    bool last_blocked_ = false;
    bool last_screen_ = false;
};

// ------------------------------------------------------------------------------------------------- which way a join starts
//
// What the game does for its OWN online joins: the player is on the pause menu's SYSTEM LINK (or XBOX LIVE) page, which the game loaded
// and has had up for seconds, and presses QUICK CRUISE; the search is entered from that page, and when it ends the game closes the
// pause screen itself. A join started with the pause screen open on the very page it needs does the same (measured, run ojold-pause:
// "already in 'LAN'; searching from it" > the game hosted > "screen -> none" > the HUD). It is the one start that makes no transition of
// ours: no close, no goto. Every other start (the pause screen open on another page, or not open: free roam, --join_server) has to
// bring the machine to the page itself, from where the game never goes there: PM_Continue first when the screen is open, a settle,
// the goto, the wait for the page (PageWait), and an end state that is not the game's (measured, runs ojnew-pause and ojnew-cli: the
// SYSTEM LINK page left open over the world, and in the free-roam run the search was not entered at all).
enum class Path {
    kInPage,        // the pause screen is open on the page the join needs: search from it, touch nothing else
    kFromFreeRoam,  // anything else: close the pause screen if it is open, settle, go to the page, wait for it, search
};

constexpr Path ChoosePath(bool pause_screen_open, bool in_target_page) {
    return pause_screen_open && in_target_page ? Path::kInPage : Path::kFromFreeRoam;
}

// ------------------------------------------------------------------------------------------------ the search was entered
//
// The goto into "QuickMatchSearching" says nothing useful about whether the machine moved (its return value is the last vfunc's: 1 in
// the runs where the search started, 0 in the free-roam run where it did not and the relay never saw the game, ojnew-cli). So the join
// looks: a moment after the goto the machine must be in the search state, or no longer in the page it started from (it moved on: a
// session was made or found). Still in the page and not in the search = the goto was dropped: ask again, twice at most, then say so.
class SearchVerify {
public:
    enum class Verdict {
        kWait,    // too early to tell
        kOk,      // in the search, or past it: nothing to do
        kRetry,   // dropped: send the goto again now
        kGiveUp,  // dropped three times: the join goes on to its deadline, the caller logs it
    };
    static constexpr int64_t kCheckAfterMs = 600;
    static constexpr int kCheckAfterFrames = 15;
    static constexpr int kMaxRetries = 2;

    // The goto was just sent (also after a retry).
    void Start(int64_t now_ms) {
        start_ms_ = now_ms;
        frames_ = 0;
        active_ = true;
    }
    bool Active() const { return active_; }
    int retries() const { return retries_; }

    Verdict Update(bool in_search, bool in_page, int64_t now_ms) {
        if (!active_) return Verdict::kOk;
        ++frames_;
        if (in_search || !in_page) {
            active_ = false;
            return Verdict::kOk;
        }
        if (now_ms - start_ms_ < kCheckAfterMs || frames_ < kCheckAfterFrames) return Verdict::kWait;
        if (retries_ < kMaxRetries) {
            ++retries_;
            Start(now_ms);
            return Verdict::kRetry;
        }
        active_ = false;
        return Verdict::kGiveUp;
    }

private:
    int64_t start_ms_ = 0;
    int frames_ = 0;
    int retries_ = 0;
    bool active_ = false;
};

// ---------------------------------------------------------------------------------------------------------- the net menu's page
//
// The goto into the net menu loads a pause page (measured, run ojnew-pause: the goto(LAN) of 21:19:51.067 brought the pause movie
// back at 51.083 and the search was entered at 51.084). The search state is entered from the page by a player a few hundred
// milliseconds after it showed. This is that wait: the machine reports being in the page, the page's movie is the current one, and
// at least kMinFrames frames and kMinMs have passed since the goto. A page that never reports (the walk that reads "in the state"
// is the game's own, read from the outside) is not waited for longer than kGiveUpMs: then the search goes on and the caller logs it.
class PageWait {
public:
    static constexpr int kMinFrames = 12;
    static constexpr int64_t kMinMs = 250;
    static constexpr int64_t kGiveUpMs = 4000;

    // The goto was just made.
    void Start(int64_t now_ms) {
        start_ms_ = now_ms;
        frames_ = 0;
        skip_ = false;
        gave_up_ = false;
    }
    // The machine was in the page already (nothing was loaded): nothing to wait for.
    void Skip() { skip_ = true; }

    // Once per frame: whether the search may be entered now.
    bool Ready(bool in_page, bool page_shown, int64_t now_ms) {
        ++frames_;
        if (skip_) return true;
        if (now_ms - start_ms_ >= kGiveUpMs) {
            gave_up_ = true;
            return true;
        }
        return frames_ >= kMinFrames && now_ms - start_ms_ >= kMinMs && in_page && page_shown;
    }
    // Ready() said yes because the time ran out, not because the page was up.
    bool GaveUp() const { return gave_up_; }

private:
    int64_t start_ms_ = 0;
    int frames_ = 0;
    bool skip_ = false;
    bool gave_up_ = false;
};

}  // namespace larecomp::browser
