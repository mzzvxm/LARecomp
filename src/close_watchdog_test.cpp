// Check of the close watchdog's decision (close_watchdog.h): when a close that has not finished is ended as hung. A program of its own: it needs no
// renderer, no ReXGlue, no driver and no threads (the watcher's polling is replayed here, on a clock of its own), and exits 0 when every check holds.
//
// The timelines are the logged closes of 2026-11-20 to 11-22 (VK-SLOW-CLOSE): the seven Vulkan closes whose renderer teardown took 4.4 to 4.7 s
// against 0.1 s normally and that the old fixed 5 s limit ended 0.3 s before they finished by themselves (onl3-race-vulkan, neon1-ab-vk-on, ...),
// and the hangs the watchdog is for, which must still end.
//
//   cmake --build <build dir> --target larecomp_close_watchdog_test, then run <build dir>/larecomp_close_watchdog_test (exit 0 = passed).

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "close_watchdog.h"

using namespace larecomp::closewatch;

namespace {

int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("FAILED: %s\n", what.c_str());
  }
}

// The renderer's shutdown enters `stage` at `at_ms` after the watchdog was armed.
struct Event {
  int64_t at_ms;
  const char* stage;
  bool driver_call;
};

struct Close {
  std::vector<Event> events;
  int64_t renderer_finished_ms = -1;  // Renderer::Shutdown returned here; -1 never
  int64_t process_ends_ms = -1;       // the close would finish by itself here; -1 never
};

RendererState StateAt(const Close& c, int64_t t) {
  RendererState r;
  const Event* last = nullptr;
  for (const Event& e : c.events) {
    if (e.at_ms <= t) last = &e;
  }
  if (!last) return r;  // not started
  r.started = true;
  r.stage = last->stage;
  r.in_driver_call = last->driver_call;
  r.quiet_ms = t - last->at_ms;
  if (c.renderer_finished_ms >= 0 && c.renderer_finished_ms <= t) {
    r.finished = true;
    r.stage = nullptr;
    r.in_driver_call = false;
    r.quiet_ms = t - c.renderer_finished_ms;
  }
  return r;
}

// Replays the watcher: it looks every kPollMs, asks ShouldEnd, and the process leaves by itself at process_ends_ms. Returns the elapsed ms at which the
// watchdog ended the process, or -1 when the close finished first.
int64_t Replay(const Close& c) {
  for (int64_t t = kPollMs; t < 120000; t += kPollMs) {
    if (c.process_ends_ms >= 0 && t > c.process_ends_ms) return -1;
    const RendererState r = StateAt(c, t);
    if (ShouldEnd(MakeLook(t, r.started, r.quiet_ms, r.in_driver_call))) return t;
  }
  return -2;  // never ended: a watchdog that does not end anything
}

const char* kStages[] = {"the render thread's stop", "the frame generation pacer's stop", "the final reports and the first idle wait", "the swap chain",
                         "the pipelines (the precache's join, the cache file, the pipelines' release)", "the second idle wait", "the bindless heap and the frames"};
const char* kDevice = "the device (the driver's own teardown)";

// A renderer shutdown that moves through its ordinary stages in the first 250 ms and then enters the driver's final teardown.
Close DriverClose(int64_t device_begins_ms, int64_t renderer_finished_ms, int64_t process_ends_ms) {
  Close c;
  int64_t t = 10;
  for (const char* stage : kStages) {
    c.events.push_back({t, stage, false});
    t += 20;
  }
  c.events.push_back({device_begins_ms, kDevice, true});
  c.renderer_finished_ms = renderer_finished_ms;
  c.process_ends_ms = process_ends_ms;
  return c;
}

void TestTheConstants() {
  Check(kQuietMs == 5000, "the quiet limit is the old 5 s: nothing is ended sooner than before");
  Check(kDriverCallQuietMs > 4700 + 400, "a driver call gets more than the slowest teardown seen (4.68 s) plus the 0.4 s the rest of a close needs");
  Check(kDriverCallQuietMs > kQuietMs, "a driver call gets more time than a stage of ours");
  Check(kCapMs > kDriverCallQuietMs, "the cap is beyond the driver-call limit");
  Check(kCapMs <= 15000, "the cap stays short enough for a player who closed the game");
  Check(kPollMs <= 100 && kPollMs >= 10, "the watcher looks about ten times a second");
}

void TestNothingIsEndedBeforeTheOldLimit() {
  for (int64_t elapsed = 0; elapsed < kQuietMs; elapsed += 50) {
    for (int64_t quiet : {int64_t(0), elapsed, int64_t(100000)}) {
      for (bool driver : {false, true}) {
        Look look;
        look.elapsed_ms = elapsed;
        look.quiet_ms = quiet;
        look.in_driver_call = driver;
        Check(!ShouldEnd(look), "before 5 s nothing is ended (elapsed " + std::to_string(elapsed) + ", quiet " + std::to_string(quiet) + ")");
      }
    }
  }
}

void TestTheCapEndsEverything() {
  for (int64_t elapsed : {kCapMs, kCapMs + 1, kCapMs + 5000}) {
    for (bool driver : {false, true}) {
      Look look;
      look.elapsed_ms = elapsed;
      look.quiet_ms = 0;  // the renderer just moved
      look.in_driver_call = driver;
      Check(ShouldEnd(look), "at the cap a close is ended whatever the renderer does (elapsed " + std::to_string(elapsed) + ")");
    }
  }
  Look just_under;
  just_under.elapsed_ms = kCapMs - 1;
  just_under.quiet_ms = 0;
  Check(!ShouldEnd(just_under), "one ms under the cap, with the renderer moving, the close is still waited for");
}

void TestMakeLook() {
  Look l = MakeLook(7000, false, 123, true);
  Check(l.quiet_ms == 7000, "a renderer that has not started: the quiet time runs from the arming");
  Check(!l.in_driver_call, "a renderer that has not started is in no driver call, whatever it says");
  l = MakeLook(7000, true, 300, true);
  Check(l.quiet_ms == 300 && l.in_driver_call && l.elapsed_ms == 7000, "a running renderer: its own quiet time and its driver-call flag");
  l = MakeLook(2000, true, 9000, false);
  Check(l.quiet_ms == 2000, "a quiet time longer than the close itself is cut to it (a clock read a little late)");
}

void TestTheSlowDriverCloseIsLeftToFinish() {
  // onl3-race-vulkan 11-22 19:14:57: the device teardown from 0.3 s to 4.65 s, the title's terminate 3 ms later, the old watchdog at 5.007 s, the
  // process would have finished about 0.35 s after the renderer.
  Check(Replay(DriverClose(300, 4650, 5050)) == -1, "onl3-race-vulkan (renderer done at 4.65 s, close done at 5.05 s): not ended");
  // neon1-ab-vk-on 11-21 12:55:57: renderer shut down 4.767 s after the quit line, the old watchdog 5.018 s.
  Check(Replay(DriverClose(200, 4767, 5100)) == -1, "neon1-ab-vk-on (renderer done at 4.77 s): not ended");
  // neon1-ab-vk-off2: 4.447 s; the same for 4.35 (onl3's gap) and a teardown of 3.96 (gw1-gb-night-vk-m8, which the old watchdog did not end).
  Check(Replay(DriverClose(200, 4447, 4800)) == -1, "neon1-ab-vk-off2 (renderer done at 4.45 s): not ended");
  Check(Replay(DriverClose(200, 3960, 4300)) == -1, "gw1-gb-night-vk-m8 (renderer done at 3.96 s): not ended");
  // A teardown well past anything seen (7 s) is still a call that returned.
  Check(Replay(DriverClose(300, 7000, 7400)) == -1, "a 7 s teardown (past the slowest seen) is not ended either");
}

void TestADriverCallThatNeverReturnsIsEnded() {
  const int64_t ended = Replay(DriverClose(300, -1, -1));
  Check(ended >= 300 + kDriverCallQuietMs && ended <= 300 + kDriverCallQuietMs + kPollMs, "a driver call that never returns: ended " + std::to_string(kDriverCallQuietMs) + " ms after it began, got " + std::to_string(ended));
  Check(ended <= kCapMs, "a hung driver call is ended under the cap");
  // A call that begins late (the stages before it took 4.5 s, each moving) still ends under the cap: 4.5 s + 8 s would be 12.5 s.
  const int64_t late = Replay(DriverClose(4500, -1, -1));
  Check(late == kCapMs, "a driver call that begins at 4.5 s and hangs is ended by the cap, got " + std::to_string(late));
}

void TestAStageOfOursThatHangsEndsAfterFiveSecondsOfQuiet() {
  // The first idle wait that waits for a GPU that never answers: the stage began at 1.0 s.
  Close c;
  c.events.push_back({10, kStages[0], false});
  c.events.push_back({1000, kStages[2], false});
  const int64_t ended = Replay(c);
  Check(ended == 1000 + kQuietMs, "a stage of ours that hangs from 1.0 s: ended 5 s after it began (6.0 s), got " + std::to_string(ended));
  // The same stage inside the driver's flag would get 8 s: the flag is the only thing that differs.
  Close d = c;
  d.events.back().driver_call = true;
  Check(Replay(d) == 1000 + kDriverCallQuietMs, "the same stage flagged as a driver call: 8 s");
}

void TestNoRendererMeansTheOldFiveSeconds() {
  // Xenia, a modal error box on the UI thread (the 2026-11-18 run), a close that stuck before the renderer's shutdown began.
  Close c;
  const int64_t ended = Replay(c);
  Check(ended == kQuietMs, "no renderer shutdown: ended at exactly the old 5 s, got " + std::to_string(ended));
}

void TestARendererThatFinishedAndThenSomethingHangs() {
  Close early;
  early.events.push_back({10, kStages[0], false});
  early.renderer_finished_ms = 200;
  Check(Replay(early) == 200 + kQuietMs, "the renderer was done at 0.2 s and the close still hangs: ended 5 s after it finished");
  Close late = DriverClose(300, 4900, -1);
  Check(Replay(late) == 4900 + kQuietMs, "the renderer was done at 4.9 s and the rest hangs: ended 5 s after it finished (9.9 s)");
  Close later = DriverClose(300, 8000, -1);
  Check(Replay(later) == kCapMs, "a renderer done at 8 s and a rest that hangs: the cap (12 s)");
}

void TestARendererThatKeepsMovingIsCapped() {
  Close c;
  int64_t t = 0;
  for (int i = 0; i < 40; ++i) {
    c.events.push_back({t, kStages[i % 7], false});
    t += 3000;
  }
  Check(Replay(c) == kCapMs, "a renderer that changes stage every 3 s for ever: ended at the cap");
}

void TestOnceEndedStaysEnded() {
  // With the renderer's state frozen, more elapsed time never turns an end back into a wait.
  for (bool driver : {false, true}) {
    for (int64_t quiet = 0; quiet <= 20000; quiet += 250) {
      bool ended = false;
      for (int64_t elapsed = quiet; elapsed <= 30000; elapsed += 250) {
        Look look;
        look.elapsed_ms = elapsed;
        look.quiet_ms = quiet;
        look.in_driver_call = driver;
        const bool now = ShouldEnd(look);
        if (ended && !now) {
          Check(false, "an ended close came back to waiting (quiet " + std::to_string(quiet) + ", elapsed " + std::to_string(elapsed) + ")");
          return;
        }
        ended = ended || now;
      }
    }
  }
  Check(true, "once ended it stays ended");
}

void TestTheLineKeepsItsWords() {
  // gb_neon_report.py QUIT_SLOW_RX = '\[quit\] the process was still running (\d+) s' and chain-quit1.ps1's 'still running 5 s after'.
  const std::string prefix = "the process was still running ";
  RendererState none;
  RendererState finished;
  finished.started = true;
  finished.finished = true;
  finished.quiet_ms = 2100;
  RendererState in_stage;
  in_stage.started = true;
  in_stage.stage = "the swap chain";
  in_stage.quiet_ms = 5100;
  RendererState in_driver;
  in_driver.started = true;
  in_driver.stage = kDevice;
  in_driver.in_driver_call = true;
  in_driver.quiet_ms = 4700;
  for (const RendererState* r : {&none, &finished, &in_stage, &in_driver}) {
    const std::string text = EndText(5000, *r);
    Check(text.rfind("the process was still running 5 s after the window was asked to close; exiting", 0) == 0, "the first words of the line at 5 s: " + text);
  }
  Check(EndText(5099, none).rfind(prefix + "5 s ", 0) == 0, "5.099 s is still 5 s (whole seconds, rounded down)");
  Check(EndText(8300, in_driver).rfind(prefix + "8 s after", 0) == 0, "a watchdog that waited for a driver call says 8 s, not 5");
  Check(EndText(12000, in_driver).rfind(prefix + "12 s after", 0) == 0, "the cap says 12 s");
  Check(EndText(5000, none).find("had not begun") != std::string::npos, "no renderer shutdown: the line says so");
  Check(EndText(5000, finished).find("had finished 2.1 s before") != std::string::npos, "a finished renderer: how long before");
  Check(EndText(5000, in_stage).find("'the swap chain', a stage of ours, for 5.1 s") != std::string::npos, "a stage of ours: its name and its time");
  Check(EndText(8000, in_driver).find("'the device (the driver's own teardown)', one call into the graphics driver, which returns by itself, for 4.7 s") != std::string::npos,
        "a driver call: its name and its time");
  RendererState nameless;
  nameless.started = true;
  Check(EndText(5000, nameless).find("'its first stage'") != std::string::npos, "a stage that was not named yet does not crash the line");
}

}  // namespace

int main() {
  TestTheConstants();
  TestNothingIsEndedBeforeTheOldLimit();
  TestTheCapEndsEverything();
  TestMakeLook();
  TestTheSlowDriverCloseIsLeftToFinish();
  TestADriverCallThatNeverReturnsIsEnded();
  TestAStageOfOursThatHangsEndsAfterFiveSecondsOfQuiet();
  TestNoRendererMeansTheOldFiveSeconds();
  TestARendererThatFinishedAndThenSomethingHangs();
  TestARendererThatKeepsMovingIsCapped();
  TestOnceEndedStaysEnded();
  TestTheLineKeepsItsWords();
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
