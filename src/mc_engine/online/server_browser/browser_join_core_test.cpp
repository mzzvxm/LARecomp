// A check that needs no game and no server: the wait a join makes for an open pause menu (browser_join_core.h). Build:
//   cmake --build <build dir> --target larecomp_browser_join_core_test, then run it; exit 0 = passed.
//
// The rule it pins down: a join never starts (the goto into the net menu) while the game's paused flag is set. A fake game
// stands in for the pause screen: it is paused from some frame, closes when asked (at once, late, or never) or when the
// player closes it, and the loop below is the shape of browser_join.cpp's kStarting step 0.
//
// LOCAL TRIO CHECK (main session, one game at a time; the tour this unit test cannot do):
//   1. larecomp_relay_server on 127.0.0.1:31000 (+ master 31003, rmr 31002 as ONLINE.md 4.3 describes), a second game or a
//      recorded peer hosting a Cruise room as `Hoster`.
//   2. Game A, online on that relay (--mcla_online_enable=true --mcla_online_server=127.0.0.1:31000), save mzlate, free roam.
//   3. Press Start (the pause menu), F6 (the browser) over it, Join on `Hoster`'s room, or Cruise on this server.
//      Before the fix: the game crashed here. After it, the log shows, in order:
//        [browser] join <label> ...
//        [browser] the pause menu is open: closing it before the join starts (try 1)
//        [browser] the pause menu is closed; the join starts from free roam (paused 0)
//        [browser] goto(LAN) ...  /  [browser] the game is joining the picked room
//      and the pause menu is gone when the search starts.
//   4. The same from SYSTEM LINK > SERVER BROWSER (the machine sits in "LAN"), from XBOX LIVE > SERVER BROWSER ("Online": a LAN
//      join goes to "LAN" from there), and from a page of ours (EXTRAS) over the pause screen.
//   5. With --join_server=127.0.0.1:31000,cruise (join_server_core.h): open the pause menu while the city loads and leave it open.
//   A toolkit scene cannot press Start; a Director verb that opens and closes the pause menu (ui.pause) would make 3-5 a scene.

#include <cstdint>
#include <cstdio>

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

// Never paused: the join starts on the first look and nothing is closed.
void TestNeverPaused() {
    PauseGate g;
    CHECK(g.Update(false, 0) == Step::kProceed);
    CHECK(g.Update(false, 16) == Step::kProceed);
    CHECK(g.tries() == 0);
    CHECK(!g.SawPause());
}

// Paused: one close request at once, then wait; running again: wait out the settle, then go.
void TestCloseThenSettle() {
    PauseGate g;
    CHECK(g.Update(true, 1000) == Step::kClose);
    CHECK(g.tries() == 1);
    CHECK(g.Update(true, 1016) == Step::kWait);  // the request is still being carried out
    CHECK(g.Update(false, 1100) == Step::kWait);  // running, settling
    CHECK(g.Update(false, 1100 + PauseGate::kSettleMs - 1) == Step::kWait);
    CHECK(g.Update(false, 1100 + PauseGate::kSettleMs) == Step::kProceed);
    CHECK(g.tries() == 0);  // Reset: the next join starts clean
    CHECK(!g.SawPause());
    CHECK(g.Update(false, 2000) == Step::kProceed);
}

// A request that did nothing is repeated, three times, then only the player can close it.
void TestRetriesThenPlayer() {
    PauseGate g;
    int64_t now = 0;
    CHECK(g.Update(true, now) == Step::kClose);
    now += PauseGate::kRetryMs - 1;
    CHECK(g.Update(true, now) == Step::kWait);
    now += 1;
    CHECK(g.Update(true, now) == Step::kClose);
    CHECK(!g.NeedsPlayer(now));
    now += PauseGate::kRetryMs;
    CHECK(g.Update(true, now) == Step::kClose);
    CHECK(g.tries() == PauseGate::kMaxCloseTries);
    CHECK(!g.NeedsPlayer(now));  // the third request has not had its time yet
    now += PauseGate::kRetryMs;
    CHECK(g.NeedsPlayer(now));
    for (int i = 0; i < 100; ++i) {
        now += 16;
        CHECK(g.Update(true, now) == Step::kWait);  // never a fourth request, never a start
    }
    CHECK(g.tries() == PauseGate::kMaxCloseTries);
    // The player closes it: the join goes on after the settle.
    now += 16;
    CHECK(g.Update(false, now) == Step::kWait);
    now += PauseGate::kSettleMs;
    CHECK(g.Update(false, now) == Step::kProceed);
}

// The first look finds the menu up: the request goes out at once (no waiting a frame with a goto pending), and a Reset
// (a new join) forgets everything.
void TestResetBetweenJoins() {
    PauseGate g;
    CHECK(g.Update(true, 0) == Step::kClose);
    CHECK(g.SawPause());
    g.Reset();
    CHECK(!g.SawPause() && g.tries() == 0);
    CHECK(g.Update(false, 5) == Step::kProceed);
}

// The menu comes back during the settle (the player pressed Start again): back to closing, the tries kept.
void TestReopenedDuringSettle() {
    PauseGate g;
    CHECK(g.Update(true, 0) == Step::kClose);
    CHECK(g.Update(false, 100) == Step::kWait);
    CHECK(g.Update(true, 150) == Step::kWait);  // the first request is still inside its retry window
    CHECK(g.tries() == 1);
    CHECK(g.Update(true, 150 + PauseGate::kRetryMs) == Step::kClose);
    CHECK(g.tries() == 2);
    CHECK(g.Update(false, 2000) == Step::kWait);
    CHECK(g.Update(false, 2000 + PauseGate::kSettleMs) == Step::kProceed);
}

// The fake game: paused from `pause_from`, closes `close_after_ms` after a close request (-1 = ignores them), and the player
// closes it at `player_at` (-1 = never). Returns the frame the join started, or -1; `started_while_paused` is the bug.
struct Run {
    int start_frame = -1;
    bool started_while_paused = false;
    int close_requests = 0;
};

Run Simulate(int pause_from, int close_after_ms, int player_at, int frames) {
    PauseGate gate;
    Run run;
    bool paused = false;
    int64_t close_at = -1;
    for (int f = 0; f < frames; ++f) {
        const int64_t now = int64_t(f) * 16;
        if (f == pause_from) paused = true;
        if (close_at >= 0 && now >= close_at) {
            paused = false;
            close_at = -1;
        }
        if (player_at >= 0 && f == player_at) paused = false;
        const Step step = gate.Update(paused, now);
        if (step == Step::kClose) {
            ++run.close_requests;
            if (close_after_ms >= 0) close_at = now + close_after_ms;
        } else if (step == Step::kProceed) {
            run.start_frame = f;
            run.started_while_paused = paused;
            return run;
        }
    }
    return run;
}

void TestSimulated() {
    {  // the pause menu is up before the join asks: the game closes it in one frame
        const Run r = Simulate(0, 16, -1, 600);
        CHECK(r.start_frame > 0 && !r.started_while_paused && r.close_requests == 1);
    }
    {  // a slow close (the menu movie unloads over half a second)
        const Run r = Simulate(0, 500, -1, 600);
        CHECK(r.start_frame > 0 && !r.started_while_paused && r.close_requests == 1);
    }
    {  // a close that takes two retries
        const Run r = Simulate(0, 3200, -1, 600);
        CHECK(r.start_frame > 0 && !r.started_while_paused && r.close_requests >= 2);
    }
    {  // a game that ignores every request, the player closes it a minute in: still no start while paused
        const Run r = Simulate(0, -1, 3600, 6000);
        CHECK(r.start_frame > 3600 && !r.started_while_paused && r.close_requests == PauseGate::kMaxCloseTries);
    }
    {  // a game that ignores every request and a player who never does: the join never starts
        const Run r = Simulate(0, -1, -1, 6000);
        CHECK(r.start_frame < 0 && r.close_requests == PauseGate::kMaxCloseTries);
    }
    {  // the pause menu opens after the join began waiting for something else: closed all the same
        const Run r = Simulate(40, 16, -1, 600);
        CHECK(r.start_frame >= 0 && !r.started_while_paused);
    }
    {  // never paused: starts on the first frame
        const Run r = Simulate(-1, 16, -1, 10);
        CHECK(r.start_frame == 0 && r.close_requests == 0);
    }
}

}  // namespace

int main() {
    TestNeverPaused();
    TestCloseThenSettle();
    TestRetriesThenPlayer();
    TestResetBetweenJoins();
    TestReopenedDuringSettle();
    TestSimulated();
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
