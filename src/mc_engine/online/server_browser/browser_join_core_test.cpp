// A check that needs no game and no server: what a join does about an open pause menu, where the game is, the net menu's page and the
// search (browser_join_core.h). Build:
//   cmake --build <build dir> --target larecomp_browser_join_core_test, then run it; exit 0 = passed.
//
// The rules it pins down:
//   - a join never starts (the goto into the net menu) while the pause SCREEN is open, whatever the game's paused flag says (an online
//     session's pause menu leaves the flag at 0), and PM_Continue is asked for only while that screen is open;
//   - the settle after the close counts frames as well as milliseconds, and waits for the pause movie to go;
//   - the pause screen open on the page the join needs is the game's own start: nothing is closed, nothing is sent to (ChoosePath);
//   - a race, the garage and an online session refuse a join (RefuseStart); the title and the loads wait;
//   - the search is not entered on the frame after the goto into the net menu (PageWait), and a goto into the search that was dropped
//     is asked for again (SearchVerify).
// A fake game stands in for the pause screen: it is up from some frame, closes when asked (at once, late, or never) or when the player
// closes it, and the loops below are the shape of browser_join.cpp's kStarting step 0.
//
// LOCAL CHECK WITH THE GAME (one game at a time; the part this unit test cannot do):
//   1. larecomp_relay_server on 127.0.0.1 (--bind), a master and a Rate My Ride server on 127.0.0.1 too.
//   2. Game A, online on that relay (--mcla_online_enable=true --mcla_online_server=127.0.0.1:<port>), save mzlate, free roam.
//   3. Press Start (the pause menu), SYSTEM LINK, SERVER BROWSER, Cruise on this server. The log shows, in order:
//        [browser] join asked for: pause screen 1, paused flag 1, pause movie 1, activity free roam, session up 0
//        [browser] join <label> ...
//        [browser] the pause menu is open on the 'LAN' page: searching from it, the way the game does (no close, no goto)
//        [browser] goto(QuickMatchSearching) ...
//      and the relay's log shows the game hosting (REGISTER ... host=1) a few seconds later, the pause menu gone and the HUD back.
//   4. The same from XBOX LIVE > SERVER BROWSER (a Live room, or a System Link join from there: the page is not the one the join needs,
//      so the pause menu is closed first: "closing it before the join starts", then "the pause menu is closed; the join starts from free
//      roam", the goto, "[browser] the 'LAN' page ...", the search).
//   5. With --join_server=127.0.0.1:<port>,cruise: the relay's log must show the game HOSTING (host=1) or the join must say failed
//      (reason=no_session); "done" with no host line in the relay's log is the bug of ojnew-cli.
//   6. In a hosted session: pause, SERVER BROWSER, Join: refused with "You are already in an online session" (reason=in_session);
//      in a race: "A race is on" (reason=race_on).
//   7. (the measured crash, run ojnew-cli) --join_server=127.0.0.1:<port> and NO key: the game sits at the title screen ("PRESS START");
//      the join must not start (before the fix: an access violation at 0x1C on the Main XThread at the goto).
//
// The second part of the file (the player's car) walks the pure car test over a fake guest memory: the title screen's racer has
// no car entity (racer+48 = 0) and the join must wait, whatever else the world looks like.

#include <cstdint>
#include <cstdio>
#include <map>

#include "browser_join_core.h"

using larecomp::browser::PauseGate;

namespace {

int g_checks = 0;
int g_failed = 0;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        ++g_checks;                                                                    \
        if (!(cond)) {                                                                 \
            ++g_failed;                                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                \
        }                                                                              \
    } while (0)

using Step = PauseGate::Step;
using larecomp::browser::Activity;
using larecomp::browser::Look;
using larecomp::browser::PageWait;
using larecomp::browser::Refusal;

constexpr Look kRoad{false, false, false};
constexpr Look kMenu{true, true, true};          // the pause screen up in a single-player game: the world is paused too
constexpr Look kSessionMenu{true, false, true};  // the pause screen up in an online session: a local pause, the game's flag is 0
constexpr Look kMapPause{false, true, false};    // paused with no pause screen (the full map, a toolkit pause)

// Never paused: the join starts on the first look and nothing is closed.
void TestNeverPaused() {
    PauseGate g;
    CHECK(g.Update(kRoad, 0) == Step::kProceed);
    CHECK(g.Update(kRoad, 16) == Step::kProceed);
    CHECK(g.tries() == 0);
    CHECK(!g.SawPause());
}

// The pause screen is up: it is left alone for a moment (it may have just been opened, its movie still loading), then one close
// request goes out, then wait; closed: wait out the settle (time AND frames), then go.
void TestCloseThenSettle() {
    PauseGate g;
    int64_t now = 1000;
    Step step = Step::kWait;
    int looks = 0;
    // The first looks: the screen is "fresh", no request yet.
    while ((step = g.Update(kMenu, now)) == Step::kWait) {
        CHECK(g.tries() == 0);
        now += 16;
        ++looks;
        CHECK(looks < 100);
    }
    CHECK(step == Step::kClose);
    CHECK(g.tries() == 1);
    CHECK(looks >= PauseGate::kOpenSettleFrames - 1);  // at least the frames
    CHECK(now - 1000 >= PauseGate::kOpenSettleMs);     // and the time
    now += 16;
    CHECK(g.Update(kMenu, now) == Step::kWait);  // the request is still being carried out
    // Closed: both the time and the frames of the settle count, and the movie must be gone.
    now += 16;
    CHECK(g.Update(kRoad, now) == Step::kWait);
    const int64_t closed_at = now;
    int frames_waited = 1;
    while ((step = g.Update(kRoad, now += 16)) == Step::kWait) {
        ++frames_waited;
        CHECK(frames_waited < 200);
    }
    CHECK(step == Step::kProceed);
    CHECK(now - closed_at >= PauseGate::kSettleMs);
    CHECK(frames_waited >= PauseGate::kSettleFrames - 1);
    CHECK(g.tries() == 0 && !g.SawPause());  // Reset: the next join starts clean
    CHECK(g.Update(kRoad, now + 16) == Step::kProceed);
}

// A slow PC: frames take 100 ms. The settle's frame count is what makes it longer than 300 ms of wall clock (the unload of the pause
// movie runs per frame): 20 frames at 100 ms = 2 s.
void TestSlowFramesSettleLonger() {
    PauseGate g;
    int64_t now = 0;
    while (g.Update(kMenu, now) != Step::kClose) now += 100;
    now += 100;
    CHECK(g.Update(kRoad, now) == Step::kWait);
    const int64_t closed_at = now;
    Step step = Step::kWait;
    while ((step = g.Update(kRoad, now += 100)) == Step::kWait) CHECK(now - closed_at < 10000);
    CHECK(step == Step::kProceed);
    CHECK(now - closed_at >= (PauseGate::kSettleFrames - 1) * 100);
}

// The pause movie lingering after the screen closed holds the start back, but not for ever.
void TestMovieHoldsThenGivesUp() {
    const Look movie_only{false, false, true};
    PauseGate g;
    int64_t now = 0;
    while (g.Update(kMenu, now) != Step::kClose) now += 16;
    now += 16;
    CHECK(g.Update(movie_only, now) == Step::kWait);  // the settle begins here
    const int64_t settle_from = now;
    // 60 frames = 960 ms: past the settle's time and frames, the movie is still there: still waiting.
    for (int i = 0; i < 60; ++i) CHECK(g.Update(movie_only, now += 16) == Step::kWait);
    CHECK(now - settle_from < PauseGate::kMovieWaitMs);
    // The movie goes: the join may start (the settle is long over).
    CHECK(g.Update(kRoad, now += 16) == Step::kProceed);
    // A movie that never goes: after kMovieWaitMs of settling the join goes on.
    PauseGate h;
    now = 0;
    while (h.Update(kMenu, now) != Step::kClose) now += 16;
    now += 16;
    CHECK(h.Update(movie_only, now) == Step::kWait);
    const int64_t from = now;
    Step st = Step::kWait;
    for (int i = 0; i < 1000 && st == Step::kWait; ++i) st = h.Update(movie_only, now += 16);
    CHECK(st == Step::kProceed);
    CHECK(now - from >= PauseGate::kMovieWaitMs);
}

// A request that did nothing is repeated, three times, then only the player can close it.
void TestRetriesThenPlayer() {
    PauseGate g;
    int64_t now = 0;
    while (g.Update(kMenu, now) != Step::kClose) now += 16;
    CHECK(g.tries() == 1);
    now += PauseGate::kRetryMs - 1;
    CHECK(g.Update(kMenu, now) == Step::kWait);
    now += 1;
    CHECK(g.Update(kMenu, now) == Step::kClose);
    CHECK(!g.NeedsPlayer(now));
    now += PauseGate::kRetryMs;
    CHECK(g.Update(kMenu, now) == Step::kClose);
    CHECK(g.tries() == PauseGate::kMaxCloseTries);
    CHECK(!g.NeedsPlayer(now));  // the third request has not had its time yet
    now += PauseGate::kRetryMs;
    CHECK(g.NeedsPlayer(now));
    for (int i = 0; i < 100; ++i) {
        now += 16;
        CHECK(g.Update(kMenu, now) == Step::kWait);  // never a fourth request, never a start
    }
    CHECK(g.tries() == PauseGate::kMaxCloseTries);
    // The player closes it: the join goes on after the settle.
    now += 16;
    CHECK(g.Update(kRoad, now) == Step::kWait);
    Step step = Step::kWait;
    for (int i = 0; i < 200 && step == Step::kWait; ++i) step = g.Update(kRoad, now += 16);
    CHECK(step == Step::kProceed);
}

// The first fix's gap: in an online session the pause screen is up with the game's flag at 0. That is a pause menu all the same:
// it is closed, never gone past.
void TestSessionPauseIsStillAPause() {
    PauseGate g;
    int64_t now = 0;
    Step step = Step::kWait;
    for (int i = 0; i < 200 && step == Step::kWait; ++i) step = g.Update(kSessionMenu, now += 16);
    CHECK(step == Step::kClose);  // not kProceed: the flag alone said "running"
    CHECK(g.SawPause());
}

// Paused with no pause screen (the full map, a toolkit pause): nothing of ours to close. PM_Continue is never asked for, the player
// is asked, and the join goes on after the pause lifts.
void TestPausedWithoutPauseScreen() {
    PauseGate g;
    int64_t now = 0;
    for (int i = 0; i < 400; ++i) {
        CHECK(g.Update(kMapPause, now += 16) == Step::kWait);
        CHECK(g.tries() == 0);
    }
    CHECK(g.NeedsPlayer(now));
    Step step = Step::kWait;
    for (int i = 0; i < 200 && step == Step::kWait; ++i) step = g.Update(kRoad, now += 16);
    CHECK(step == Step::kProceed);
}

// The player closes the pause screen on their own while the join waits: no request of ours at all.
void TestPlayerClosesFirst() {
    PauseGate g;
    int64_t now = 0;
    CHECK(g.Update(kMenu, now) == Step::kWait);  // fresh
    now += 16;
    CHECK(g.Update(kRoad, now) == Step::kWait);  // gone already
    CHECK(g.tries() == 0);
    Step step = Step::kWait;
    for (int i = 0; i < 200 && step == Step::kWait; ++i) step = g.Update(kRoad, now += 16);
    CHECK(step == Step::kProceed);
    CHECK(g.tries() == 0);
}

// The screen is already closing when the join looks (its movie still up, the flag already 0): not a start yet.
void TestMovieOnlyFirstLook() {
    PauseGate g;
    const Look closing{false, false, true};
    CHECK(g.Update(closing, 0) == Step::kWait);
    CHECK(g.SawPause());
}

// A new join forgets everything.
void TestResetBetweenJoins() {
    PauseGate g;
    int64_t now = 0;
    while (g.Update(kMenu, now) != Step::kClose) now += 16;
    CHECK(g.SawPause());
    g.Reset();
    CHECK(!g.SawPause() && g.tries() == 0);
    CHECK(g.Update(kRoad, now + 5) == Step::kProceed);
}

// The menu comes back during the settle (the player pressed Start again): back to closing, the tries kept.
void TestReopenedDuringSettle() {
    PauseGate g;
    int64_t now = 0;
    while (g.Update(kMenu, now) != Step::kClose) now += 16;
    now += 16;
    CHECK(g.Update(kRoad, now) == Step::kWait);
    now += 16;
    CHECK(g.Update(kMenu, now) == Step::kWait);  // the screen is "fresh" again and the first request is inside its retry window
    CHECK(g.tries() == 1);
    Step step = Step::kWait;
    for (int i = 0; i < 400 && step != Step::kClose; ++i) step = g.Update(kMenu, now += 16);
    CHECK(step == Step::kClose);
    CHECK(g.tries() == 2);
    for (int i = 0; i < 200 && step != Step::kProceed; ++i) step = g.Update(kRoad, now += 16);
    CHECK(step == Step::kProceed);
}

// The fake game: pause screen up from `pause_from`, closes `close_after_ms` after a close request (-1 = ignores them), the player
// closes it at frame `player_at` (-1 = never), `session` = the flag stays 0 (a local pause). Returns the frame the join started,
// or -1; `started_with_pause` is the bug; `close_requests` how often PM_Continue was asked for.
struct Run {
    int start_frame = -1;
    bool started_with_pause = false;
    int close_requests = 0;
};

Run Simulate(int pause_from, int close_after_ms, int player_at, int frames, bool session, int frame_ms = 16) {
    PauseGate gate;
    Run run;
    bool screen = false;
    int64_t close_at = -1;
    int64_t movie_gone_at = -1;
    for (int f = 0; f < frames; ++f) {
        const int64_t now = int64_t(f) * frame_ms;
        if (f == pause_from) screen = true;
        if (close_at >= 0 && now >= close_at) {
            screen = false;
            close_at = -1;
            movie_gone_at = now + 3 * frame_ms;  // the movie is unloaded a few frames later
        }
        if (player_at >= 0 && f == player_at && screen) {
            screen = false;
            movie_gone_at = now + 3 * frame_ms;
        }
        Look look;
        look.pause_screen = screen;
        look.paused = screen && !session;
        look.pause_movie = screen || (movie_gone_at >= 0 && now < movie_gone_at);
        const Step step = gate.Update(look, now);
        if (step == Step::kClose) {
            ++run.close_requests;
            if (!screen) run.started_with_pause = true;  // a request with no screen up: PM_Continue on a closed form
            if (close_after_ms >= 0) close_at = now + close_after_ms;
        } else if (step == Step::kProceed) {
            run.start_frame = f;
            run.started_with_pause = run.started_with_pause || screen;
            return run;
        }
    }
    return run;
}

void TestSimulated() {
    for (const bool session : {false, true}) {
        {  // the pause menu is up before the join asks: the game closes it in one frame
            const Run r = Simulate(0, 16, -1, 600, session);
            CHECK(r.start_frame > 0 && !r.started_with_pause && r.close_requests == 1);
        }
        {  // a slow close (the menu movie unloads over half a second)
            const Run r = Simulate(0, 500, -1, 600, session);
            CHECK(r.start_frame > 0 && !r.started_with_pause && r.close_requests == 1);
        }
        {  // a close that takes two retries
            const Run r = Simulate(0, 4200, -1, 900, session);
            CHECK(r.start_frame > 0 && !r.started_with_pause && r.close_requests >= 2);
        }
        {  // a game that ignores every request, the player closes it a minute in: still no start with the pause up
            const Run r = Simulate(0, -1, 3600, 6000, session);
            CHECK(r.start_frame > 3600 && !r.started_with_pause && r.close_requests == PauseGate::kMaxCloseTries);
        }
        {  // a game that ignores every request and a player who never does: the join never starts
            const Run r = Simulate(0, -1, -1, 6000, session);
            CHECK(r.start_frame < 0 && r.close_requests == PauseGate::kMaxCloseTries);
        }
        {  // the pause menu opens after the join began waiting for something else: closed all the same
            const Run r = Simulate(40, 16, -1, 600, session);
            CHECK(r.start_frame >= 0 && !r.started_with_pause);
        }
        {  // never paused: starts on the first frame
            const Run r = Simulate(-1, 16, -1, 10, session);
            CHECK(r.start_frame == 0 && r.close_requests == 0);
        }
        {  // a slow PC (100 ms frames): the same invariant
            const Run r = Simulate(0, 300, -1, 600, session, 100);
            CHECK(r.start_frame > 0 && !r.started_with_pause && r.close_requests == 1);
        }
    }
}

// --------------------------------------------------------------------------------------------------- where the game is
void TestRefusals() {
    // free roam, no session: fine
    CHECK(larecomp::browser::RefuseStart(Activity::kFreeRoam, false) == Refusal::kNone);
    CHECK(larecomp::browser::AtFreeRoam(Activity::kFreeRoam));
    // a race: refused, also with a session flag that happens to be set
    CHECK(larecomp::browser::RefuseStart(Activity::kRace, false) == Refusal::kRace);
    CHECK(larecomp::browser::RefuseStart(Activity::kRace, true) == Refusal::kRace);
    CHECK(larecomp::browser::RefuseStart(Activity::kGarage, false) == Refusal::kGarage);
    // already in a session
    CHECK(larecomp::browser::RefuseStart(Activity::kFreeRoam, true) == Refusal::kInSession);
    // the title / a load: not refused, not a place to start from either (the join waits)
    CHECK(larecomp::browser::RefuseStart(Activity::kOther, false) == Refusal::kNone);
    CHECK(!larecomp::browser::AtFreeRoam(Activity::kOther));
    CHECK(!larecomp::browser::AtFreeRoam(Activity::kRace) && !larecomp::browser::AtFreeRoam(Activity::kGarage));
}

// ------------------------------------------------------------------------------------------------- which way a join starts
void TestChoosePath() {
    using larecomp::browser::ChoosePath;
    using larecomp::browser::Path;
    CHECK(ChoosePath(true, true) == Path::kInPage);         // SYSTEM LINK page open, a System Link join: the game's own start
    CHECK(ChoosePath(true, false) == Path::kFromFreeRoam);  // the pause screen on another page: close it, go to the page
    CHECK(ChoosePath(false, false) == Path::kFromFreeRoam); // free roam
    CHECK(ChoosePath(false, true) == Path::kFromFreeRoam);  // the machine in the page with no pause screen (a page left open): not the game's own start
}

// ------------------------------------------------------------------------------------------------ the search was entered
void TestSearchVerify() {
    using larecomp::browser::SearchVerify;
    using V = SearchVerify::Verdict;
    {  // entered at once: ok on the first look, and no more looks after it
        SearchVerify v;
        v.Start(0);
        CHECK(v.Active());
        CHECK(v.Update(true, true, 16) == V::kOk);
        CHECK(!v.Active());
        CHECK(v.Update(false, true, 5000) == V::kOk);  // inactive: nothing more is asked
        CHECK(v.retries() == 0);
    }
    {  // the search was entered and has already moved on (hosting: the page is gone too)
        SearchVerify v;
        v.Start(0);
        CHECK(v.Update(false, false, 16) == V::kOk);
    }
    {  // the measured free-roam case: still in the page, never in the search. Wait first, ask again twice, then give up.
        SearchVerify v;
        int64_t now = 0;
        v.Start(now);
        int retries = 0;
        V last = V::kWait;
        for (int i = 0; i < 2000 && last != V::kGiveUp; ++i) {
            now += 16;
            last = v.Update(false, true, now);
            if (last == V::kRetry) ++retries;
            CHECK(last != V::kOk);
        }
        CHECK(last == V::kGiveUp);
        CHECK(retries == SearchVerify::kMaxRetries && v.retries() == SearchVerify::kMaxRetries);
        CHECK(!v.Active());
        CHECK(now >= 3 * SearchVerify::kCheckAfterMs);  // three tries, each given its time
    }
    {  // the first look is too early to tell
        SearchVerify v;
        v.Start(0);
        CHECK(v.Update(false, true, 16) == V::kWait);
        CHECK(v.Update(false, true, SearchVerify::kCheckAfterMs - 1) == V::kWait);
    }
    {  // slow frames: the frame count holds the verdict back as well
        SearchVerify v;
        v.Start(0);
        CHECK(v.Update(false, true, 5000) == V::kWait);  // one frame, much time
    }
    {  // taken after one retry
        SearchVerify v;
        int64_t now = 0;
        v.Start(now);
        V last = V::kWait;
        for (int i = 0; i < 200 && last != V::kRetry; ++i) last = v.Update(false, true, now += 16);
        CHECK(last == V::kRetry);
        CHECK(v.Update(true, true, now += 16) == V::kOk);
        CHECK(v.retries() == 1 && !v.Active());
    }
}

// --------------------------------------------------------------------------------------------------- the net menu's page
void TestPageWait() {
    {  // the page comes up at once: still no search before the frames and the time of a player's pause
        PageWait w;
        w.Start(1000);
        int frames = 0;
        int64_t now = 1000;
        bool ready = false;
        while (!ready && frames < 200) {
            now += 16;
            ++frames;
            ready = w.Ready(true, true, now);
        }
        CHECK(ready && !w.GaveUp());
        CHECK(frames >= PageWait::kMinFrames);
        CHECK(now - 1000 >= PageWait::kMinMs);
    }
    {  // the measured case: the search was entered one frame after the goto. Never again.
        PageWait w;
        w.Start(0);
        CHECK(!w.Ready(true, true, 16));
        CHECK(!w.Ready(true, true, 32));
    }
    {  // the machine is in the page but its movie is not up: wait
        PageWait w;
        w.Start(0);
        bool ready = false;
        for (int i = 1; i < 100 && !ready; ++i) ready = w.Ready(true, false, i * 16);
        CHECK(!ready);
        CHECK(w.Ready(true, true, 100 * 16));
    }
    {  // the page never reports: go on after the give-up time, and say so
        PageWait w;
        w.Start(0);
        bool ready = false;
        int64_t now = 0;
        while (!ready && now < 20000) ready = w.Ready(false, false, now += 16);
        CHECK(ready && w.GaveUp());
        CHECK(now >= PageWait::kGiveUpMs);
    }
    {  // already in the page when the join began: nothing to wait for
        PageWait w;
        w.Start(0);
        w.Skip();
        CHECK(w.Ready(false, false, 16));
        CHECK(!w.GaveUp());
    }
}

// ------------------------------------------------------------------------------------------------ the player's car
// A fake guest memory: the words that exist (a read of any other address fails, as an unmapped page does).
struct FakeMemory {
    std::map<uint32_t, uint32_t> words;
    int reads = 0;
    bool operator()(uint32_t ea, uint32_t* out) {
        ++reads;
        const auto it = words.find(ea);
        if (it == words.end()) return false;
        *out = it->second;
        return true;
    }
};

using larecomp::browser::kEntityMatrixAt;
using larecomp::browser::kMatrixRowsAt;
using larecomp::browser::kRacerEntityAt;
using larecomp::browser::PlayerCarReady;

constexpr uint32_t kRacer = 0x40100000, kEntity = 0x40200000, kMatrix = 0x40300000;

bool Ready(FakeMemory& m, uint32_t racer = kRacer) {
    return PlayerCarReady(racer, [&](uint32_t ea, uint32_t* out) { return m(ea, out); });
}

// The measured crash (ojnew-cli, the title screen): the racer is there, racer+48 is 0. The join must not start.
void TestTitleScreenHasNoCar() {
    FakeMemory m;
    m.words[kRacer + kRacerEntityAt] = 0;
    CHECK(!Ready(m));
    CHECK(m.reads == 1);  // it stops at the null entity, it never reads entity+28 (guest 0x1C, the fault address)
    CHECK(!Ready(m, 0));  // no racer at all
    FakeMemory none;      // the racer's memory is not readable
    CHECK(!Ready(none));
}

void TestCarChain() {
    FakeMemory m;
    m.words[kRacer + kRacerEntityAt] = kEntity;
    CHECK(!Ready(m));  // an entity without a matrix word (not readable)
    m.words[kEntity + kEntityMatrixAt] = 0;
    CHECK(!Ready(m));  // the entity has no matrix yet
    m.words[kEntity + kEntityMatrixAt] = kMatrix;
    CHECK(!Ready(m));  // the matrix is not readable
    m.words[kMatrix + kMatrixRowsAt] = 0x3F800000;
    CHECK(Ready(m));   // the car is on the road: the join may start
}

// Junk where a pointer should be (the 0xCDCDCDCD fill, a tiny value, the top of the address space): no start, no read of it.
void TestJunkPointers() {
    for (const uint32_t junk : {0xFFFFFFFFu, 0xFFFF0000u, 0x00000001u, 0x0000FFFFu}) {
        FakeMemory m;
        m.words[kRacer + kRacerEntityAt] = junk;
        CHECK(!Ready(m));
        CHECK(m.reads == 1);
    }
    FakeMemory m;
    m.words[kRacer + kRacerEntityAt] = kEntity;
    m.words[kEntity + kEntityMatrixAt] = 0x00000010;
    CHECK(!Ready(m));
    CHECK(m.reads == 2);
}

// The title screen turns into free roam: the same fake game, frame by frame. The join waits, then starts once, with the car.
void TestStartPressed() {
    FakeMemory m;
    m.words[kRacer + kRacerEntityAt] = 0;
    int started_at = -1;
    bool started_without_car = false;
    for (int f = 0; f < 200 && started_at < 0; ++f) {
        if (f == 120) {  // Start pressed, the city loaded, the car spawned
            m.words[kRacer + kRacerEntityAt] = kEntity;
            m.words[kEntity + kEntityMatrixAt] = kMatrix;
            m.words[kMatrix + kMatrixRowsAt] = 0;
        }
        if (Ready(m)) {
            started_at = f;
            started_without_car = m.words[kRacer + kRacerEntityAt] == 0;
        }
    }
    CHECK(started_at == 120);
    CHECK(!started_without_car);
}

}  // namespace

int main() {
    TestTitleScreenHasNoCar();
    TestCarChain();
    TestJunkPointers();
    TestStartPressed();
    TestNeverPaused();
    TestCloseThenSettle();
    TestSlowFramesSettleLonger();
    TestMovieHoldsThenGivesUp();
    TestRetriesThenPlayer();
    TestSessionPauseIsStillAPause();
    TestPausedWithoutPauseScreen();
    TestPlayerClosesFirst();
    TestMovieOnlyFirstLook();
    TestResetBetweenJoins();
    TestReopenedDuringSettle();
    TestSimulated();
    TestRefusals();
    TestChoosePath();
    TestSearchVerify();
    TestPageWait();
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
