// Check of the rules that choose where the sun shafts are added (shafts_anchor.h). A program of its own: it
// needs no renderer, no ReXGlue and no GPU, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_shafts_anchor_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_shafts_anchor_test

#include <cstdint>
#include <cstdio>
#include <string>

#include "shafts_anchor.h"

using namespace mcla::gpu::shafts_anchor;

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

// One frame in which the sky was taken, the way shafts.cpp runs it: choose, then report.
Anchor Frame(Policy& policy, bool force, bool copy_seen) {
  const Anchor used = policy.Choose(force);
  policy.EndOfFrame(used, copy_seen);
  return used;
}

void FreshPolicy() {
  Policy policy;
  Check(policy.Choose(false) == Anchor::kEndOfScene, "a new policy adds at the end of the scene");
  Check(policy.Choose(true) == Anchor::kMidScene, "the debug switch forces the mid-scene point");
  Check(!policy.fallen_back() && policy.fallbacks() == 0 && policy.returns() == 0, "a new policy has not fallen back");
}

void CopyThatComesNeverFallsBack() {
  Policy policy;
  for (int i = 0; i < 1000; ++i) {
    Check(Frame(policy, false, true) == Anchor::kEndOfScene, "frame with the copy stays at the end of the scene");
  }
  Check(!policy.fallen_back(), "1000 frames with the copy: no fall-back");
}

void MissesFallBackAfterThree() {
  Policy policy;
  Check(Frame(policy, false, false) == Anchor::kEndOfScene, "first miss still uses the end of the scene");
  Check(Frame(policy, false, false) == Anchor::kEndOfScene, "second miss still uses the end of the scene");
  Check(!policy.fallen_back(), "two misses are not enough");
  Check(Frame(policy, false, false) == Anchor::kEndOfScene, "the third miss's own frame still used the end of the scene");
  Check(policy.fallen_back() && policy.fallbacks() == 1, "three misses in a row fall back");
  Check(Frame(policy, false, false) == Anchor::kMidScene, "the frame after uses the mid-scene point");
  Check(policy.Choose(false) == Anchor::kMidScene, "and keeps using it");
}

void AHitBetweenMissesResetsTheCount() {
  Policy policy;
  for (int round = 0; round < 50; ++round) {
    Frame(policy, false, false);
    Frame(policy, false, false);
    Frame(policy, false, true);  // a copy in between: the count starts again
  }
  Check(!policy.fallen_back(), "2 misses and a hit, over and over, never fall back");
}

void FallBackReturnsAfterThirtyHits() {
  Policy policy;
  for (uint32_t i = 0; i < Policy::kMissesToFallBack; ++i) {
    Frame(policy, false, false);
  }
  Check(policy.fallen_back(), "fallen back");
  for (uint32_t i = 0; i + 1 < Policy::kHitsToReturn; ++i) {
    Check(Frame(policy, false, true) == Anchor::kMidScene, "watching hits use the mid-scene point");
  }
  Check(policy.fallen_back() && policy.returns() == 0, "one hit short of the return");
  Frame(policy, false, true);
  Check(!policy.fallen_back() && policy.returns() == 1, "kHitsToReturn hits in a row return to the end of the scene");
  Check(policy.Choose(false) == Anchor::kEndOfScene, "and the choice follows");
}

void AMissWhileWatchingResetsTheHits() {
  Policy policy;
  for (uint32_t i = 0; i < Policy::kMissesToFallBack; ++i) {
    Frame(policy, false, false);
  }
  for (uint32_t i = 0; i + 1 < Policy::kHitsToReturn; ++i) {
    Frame(policy, false, true);
  }
  Frame(policy, false, false);  // the 30th is a miss: the count of hits starts again
  Frame(policy, false, true);
  Check(policy.fallen_back(), "a miss among the hits keeps the fall-back");
  for (uint32_t i = 1; i < Policy::kHitsToReturn; ++i) {
    Frame(policy, false, true);
  }
  Check(!policy.fallen_back(), "kHitsToReturn clean hits after it return");
}

void TheDebugSwitchLearnsNothing() {
  Policy policy;
  for (int i = 0; i < 100; ++i) {
    Check(Frame(policy, true, false) == Anchor::kMidScene, "forced frame uses the mid-scene point");
  }
  Check(!policy.fallen_back() && policy.fallbacks() == 0, "forced frames without the copy do not count as misses");
  // Switched off again: the end of the scene is tried at once.
  Check(policy.Choose(false) == Anchor::kEndOfScene, "switch off: back to the end of the scene");
  // A fall-back, then the switch on and off: the fall-back stays until the hits come.
  for (uint32_t i = 0; i < Policy::kMissesToFallBack; ++i) {
    Frame(policy, false, false);
  }
  Check(policy.Choose(true) == Anchor::kMidScene && policy.Choose(false) == Anchor::kMidScene,
        "fallen back: both choices are mid-scene");
}

void ForcedFramesStillCountHitsWhenFallenBack() {
  Policy policy;
  for (uint32_t i = 0; i < Policy::kMissesToFallBack; ++i) {
    Frame(policy, false, false);
  }
  for (uint32_t i = 0; i < Policy::kHitsToReturn; ++i) {
    Frame(policy, true, true);  // the switch is on while the copy keeps coming
  }
  Check(!policy.fallen_back() && policy.returns() == 1, "the copy seen in forced frames returns the policy too");
  Check(policy.Choose(true) == Anchor::kMidScene, "the switch still forces the mid-scene point");
}

void FallsBackAgainAfterAReturn() {
  Policy policy;
  for (int cycle = 0; cycle < 3; ++cycle) {
    for (uint32_t i = 0; i < Policy::kMissesToFallBack; ++i) {
      Frame(policy, false, false);
    }
    Check(policy.fallen_back(), "fallen back in cycle");
    for (uint32_t i = 0; i < Policy::kHitsToReturn; ++i) {
      Frame(policy, false, true);
    }
    Check(!policy.fallen_back(), "returned in cycle");
  }
  Check(policy.fallbacks() == 3 && policy.returns() == 3, "three fall-backs and three returns counted");
}

void TallyAddsUp() {
  Tally tally;
  for (int i = 0; i < 600; ++i) {
    tally.Frame();
  }
  Check(!tally.Worth(), "an interval with no sky is not worth a line");
  tally.Sky(Anchor::kEndOfScene, true, true);
  tally.Sky(Anchor::kEndOfScene, true, true);
  tally.Sky(Anchor::kEndOfScene, false, false);  // lost: no copy
  tally.Sky(Anchor::kMidScene, true, false);     // fall-back: composited, copy not seen
  tally.Sky(Anchor::kMidScene, true, true);      // fall-back with the copy watched
  tally.Sky(Anchor::kMidScene, false, true);     // the passes could not be recorded
  Check(tally.Worth(), "an interval with sky is worth a line");
  Check(tally.frames == 600 && tally.sky == 6, "frames and sky counted");
  Check(tally.end_of_scene == 2 && tally.mid_scene == 2 && tally.lost == 2, "by place: 2, 2 and 2 lost");
  Check(tally.sky == tally.end_of_scene + tally.mid_scene + tally.lost, "sky = end + mid + lost");
  Check(tally.copies_missing == 2, "two frames never saw the copy");
  tally.Clear();
  Check(tally.frames == 0 && tally.sky == 0 && !tally.Worth(), "clear empties the tally");
}

void Names() {
  Check(std::string(AnchorName(Anchor::kEndOfScene)).find("end-of-scene") != std::string::npos, "end name");
  Check(std::string(AnchorName(Anchor::kMidScene)).find("mid-scene") != std::string::npos, "mid name");
}

}  // namespace

int main() {
  FreshPolicy();
  CopyThatComesNeverFallsBack();
  MissesFallBackAfterThree();
  AHitBetweenMissesResetsTheCount();
  FallBackReturnsAfterThirtyHits();
  AMissWhileWatchingResetsTheHits();
  TheDebugSwitchLearnsNothing();
  ForcedFramesStillCountHitsWhenFallenBack();
  FallsBackAgainAfterAReturn();
  TallyAddsUp();
  Names();
  std::printf("shafts_anchor_test: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
