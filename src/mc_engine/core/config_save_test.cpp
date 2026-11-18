// Check of what a settings save does with a cvar (config_save.h: DecideCvarSave). A program of its own: it needs no game, no ReXGlue and
// no cvar system (the decision is a plain function of the facts), and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_config_save_test      (CMakeLists.txt)
//   <build dir>/larecomp_config_save_test
//
// The compile by hand (the msvc-target clang of labuild, include path src/mc_engine):
//   clang++ -std=c++23 -Isrc/mc_engine src/mc_engine/core/config_save_test.cpp -o config_save_test.exe

#include <cstdio>
#include <string>

#include "config_save.h"

using larecomp::CvarSaveAction;
using larecomp::CvarSaveFacts;
using larecomp::DecideCvarSave;

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

const char* Name(CvarSaveAction a) {
  switch (a) {
    case CvarSaveAction::kSkip: return "skip";
    case CvarSaveAction::kPutDefault: return "put-default";
    case CvarSaveAction::kWriteLive: return "write-live";
    case CvarSaveAction::kWriteBeforeScene: return "write-before-scene";
  }
  return "?";
}

void Expect(const CvarSaveFacts& f, CvarSaveAction want, const std::string& what) {
  const CvarSaveAction got = DecideCvarSave(f);
  Check(got == want, what + ": got " + Name(got) + ", want " + Name(want));
}

// No scene: the rules the save always had.
void TestNoScene() {
  CvarSaveFacts f;
  Expect(f, CvarSaveAction::kSkip, "a cvar nobody changed is not written");
  f.changed_this_session = true;
  Expect(f, CvarSaveAction::kWriteLive, "a cvar changed this session is written, live");
  f.debug_only = true;
  Expect(f, CvarSaveAction::kSkip, "a debug-only cvar changed this session is not written");
  f.debug_only = false;
  f.is_command = true;
  Expect(f, CvarSaveAction::kSkip, "a command is never written");
  f.is_command = false;
  f.session_only = true;
  Expect(f, CvarSaveAction::kPutDefault, "a session-only cvar is put back to the default in the file, changed or not");
  f.changed_this_session = false;
  Expect(f, CvarSaveAction::kPutDefault, "a session-only cvar nobody changed is the same");
  f.is_command = true;
  Expect(f, CvarSaveAction::kSkip, "a command beats session-only");
}

// A scene holds the cvar (changed_this_session is then true: the scene's SetFlagByName made it "run time").
void TestScene() {
  CvarSaveFacts f;
  f.held_by_scene = true;
  f.changed_this_session = true;

  // The cvar was at its default, in the file or on the command line before the scene: the scene's value is not written, nothing is.
  f.changed_before_scene = false;
  Expect(f, CvarSaveAction::kSkip, "a cvar the scene took from the default, the file or the command line is not written");
  f.debug_only = true;
  Expect(f, CvarSaveAction::kSkip, "and a debug-only one neither");
  f.debug_only = false;

  // The player had changed it before the scene: the save writes the player's value, not the scene's.
  f.changed_before_scene = true;
  Expect(f, CvarSaveAction::kWriteBeforeScene, "a cvar the player had changed before the scene is written with the value from before");
  f.debug_only = true;
  Expect(f, CvarSaveAction::kSkip, "unless it is debug-only");
  f.debug_only = false;

  // The held flag is what counts: a scene cvar whose source went back (the command line value restored) is not "held" any more.
  f.held_by_scene = false;
  f.changed_this_session = false;
  f.changed_before_scene = false;
  Expect(f, CvarSaveAction::kSkip, "after the scene ends, a restored command-line value (source put back) is not written");

  // Session-only and command rules come first, held or not.
  f.held_by_scene = true;
  f.changed_this_session = true;
  f.session_only = true;
  Expect(f, CvarSaveAction::kPutDefault, "a session-only cvar a scene holds is still put back to the default");
  f.session_only = false;
  f.is_command = true;
  Expect(f, CvarSaveAction::kSkip, "a command a scene holds is not written");
}

// Every combination of the six facts: the scene's value (held, and not changed before) is never the answer kWriteLive, and
// kWriteBeforeScene only comes with a hold.
void TestAll() {
  int writes_live_held = 0;
  int before_without_hold = 0;
  int live_for_unchanged_before = 0;
  for (int bits = 0; bits < 64; ++bits) {
    CvarSaveFacts f;
    f.is_command = bits & 1;
    f.session_only = bits & 2;
    f.changed_this_session = bits & 4;
    f.debug_only = bits & 8;
    f.held_by_scene = bits & 16;
    f.changed_before_scene = bits & 32;
    const CvarSaveAction a = DecideCvarSave(f);
    if (a == CvarSaveAction::kWriteLive && f.held_by_scene) ++writes_live_held;
    if (a == CvarSaveAction::kWriteBeforeScene && !f.held_by_scene) ++before_without_hold;
    if (f.held_by_scene && !f.changed_before_scene && !f.is_command && !f.session_only && a != CvarSaveAction::kSkip) ++live_for_unchanged_before;
    if (a == CvarSaveAction::kWriteLive || a == CvarSaveAction::kWriteBeforeScene) {
      Check(!f.is_command && !f.session_only && !f.debug_only, "only a plain, non-debug cvar is ever written");
    }
  }
  Check(writes_live_held == 0, "a held cvar's live value (the scene's) is never written");
  Check(before_without_hold == 0, "the value from before the scene is only used for a held cvar");
  Check(live_for_unchanged_before == 0, "a held cvar that was not the player's before the scene is never written");
}

}  // namespace

int main() {
  TestNoScene();
  TestScene();
  TestAll();
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
