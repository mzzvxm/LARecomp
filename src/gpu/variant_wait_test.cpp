// Check of which draws may wait in the background for their pipeline whatever the game's rule says (variant_wait.h). A program of its own: it needs no
// renderer, no ReXGlue and no GPU, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_variant_wait_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_variant_wait_test
//
// Standalone: clang++ -std=c++20 -I <repo>/src -I <repo>/src/gpu variant_wait_test.cpp

#include <cstdint>
#include <cstdio>
#include <set>
#include <string>

#include "shaderpack/variant_chain.h"
#include "sync_builds.h"
#include "variant_wait.h"

namespace vw = mcla::gpu::variant_wait;
namespace chain = larecomp::shaderpack::chain;
namespace sp = larecomp::shaderpack;

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

// What Pipelines::MaySkip does with the deferral a rung asks (pipelines.cpp): the asker's answer, and a take (sync_builds.h) turns every "pending" into a wait.
// `rule` is the game's rule (Scene::MaySkipForPipeline).
bool MayLeaveOut(vw::Ask ask, bool rule, bool take_active) {
  const bool answer = ask == vw::Ask::kAlways ? true : rule;
  return mcla::gpu::sync_builds::Decide(take_active, answer).may_skip;
}

void TestFallback() {
  Check(!vw::HasFallback(false, false), "the game's own key has no fallback");
  Check(vw::HasFallback(true, false), "a key with the sidecar's planes has one (the draw without them)");
  Check(vw::HasFallback(false, true), "a key with the headlight has one (the draw without it)");
  Check(vw::HasFallback(true, true), "a key with both has one");
  Check(vw::AskFor({false, false}) == vw::Ask::kGameRule, "no extras: the game's rule decides");
  Check(vw::AskFor({true, false}) == vw::Ask::kAlways && vw::AskFor({false, true}) == vw::Ask::kAlways && vw::AskFor({true, true}) == vw::Ask::kAlways,
        "any extra: pending is always the answer");
}

// The case the runs showed: a lit world draw writes the stencil, the game's rule says no, and the draw has the headlight and/or the planes.
void TestTheStencilWritingLitDraw() {
  const bool rule = false;  // Scene::MaySkipForPipeline on a stencil-writing draw (and on a 1-sample one)
  const vw::Ladder both = vw::LadderOf({true, true});
  Check(both.count == 3, "planes + headlight: three rungs");
  Check(MayLeaveOut(both.rungs[0].ask, rule, false), "full key: made in the background although the rule refuses (the first night, the G-buffer's first frames)");
  Check(MayLeaveOut(both.rungs[1].ask, rule, false), "the draw's own key still has the headlight: also in the background");
  Check(!MayLeaveOut(both.rungs[2].ask, rule, false), "the game's own shader keeps the game's rule: a stencil writer is not left out");
  // a take (capture mode) never leaves a draw out and waits for each pipeline, as before: the rung's answer does not matter
  for (const vw::Rung& r : both.rungs) {
    Check(!MayLeaveOut(r.ask, rule, true) && !MayLeaveOut(r.ask, true, true), "a take: nothing is left out on any rung");
  }
}

void TestLadders() {
  struct Case {
    vw::Extras full;
    size_t rungs;
    bool asks[3];  // true: kAlways
  };
  const Case cases[] = {
      {{false, false}, 1, {false, false, false}},
      {{true, false}, 2, {true, false, false}},
      {{false, true}, 2, {true, false, false}},
      {{true, true}, 3, {true, true, false}},
  };
  for (const Case& c : cases) {
    const vw::Ladder ladder = vw::LadderOf(c.full);
    bool ok = ladder.count == c.rungs;
    for (size_t i = 0; ok && i < ladder.count; ++i) {
      ok = (ladder.rungs[i].ask == vw::Ask::kAlways) == c.asks[i];
    }
    Check(ok, std::string("ladder of planes ") + (c.full.planes ? "yes" : "no") + ", headlight " + (c.full.headlight ? "yes" : "no") + ": " + std::to_string(c.rungs) + " rungs, only the last asks the game's rule");
    // the last rung never has an extra, and every rung above it has one
    Check(!vw::HasFallback(ladder.rungs[ladder.count - 1].extras), "the last rung is the game's own shader");
    bool above = true;
    for (size_t i = 0; i + 1 < ladder.count; ++i) {
      above = above && vw::HasFallback(ladder.rungs[i].extras);
    }
    Check(above, "every rung above the last has a fallback");
  }
  // the planes go before the headlight (the draw's own key still has the headlight)
  const vw::Ladder both = vw::LadderOf({true, true});
  Check(both.rungs[1].extras.headlight && !both.rungs[1].extras.planes, "second rung of planes + headlight: no planes, headlight kept");
  Check(!both.rungs[2].extras.headlight && !both.rungs[2].extras.planes, "third rung: neither");
}

void TestWordWithoutHeadlight() {
  const uint32_t headlight = chain::SoloWord(sp::kFeatureHeadlight);                                       // 1024
  const uint32_t headlight_alpha = chain::Word(chain::FeatureBit(sp::kFeatureHeadlight), 1);               // 1025
  const uint32_t composed = chain::Word(chain::FeatureBit(sp::kFeatureHeadlight) | chain::FeatureBit(sp::kFeatureGBuffer));  // 3072
  const uint32_t gbuffer = chain::SoloWord(sp::kFeatureGBuffer);                                           // 2048
  const auto none = [](uint32_t) { return false; };
  const auto all = [](uint32_t) { return true; };
  Check(headlight == 1024 && headlight_alpha == 1025 && composed == 3072 && gbuffer == 2048, "the words the logs and the starter list name (1024, 1025, 3072, 2048)");
  Check(vw::HasHeadlight(headlight) && vw::HasHeadlight(headlight_alpha) && vw::HasHeadlight(composed), "the headlight words have the headlight");
  Check(!vw::HasHeadlight(gbuffer) && !vw::HasHeadlight(0) && !vw::HasHeadlight(1), "the G-buffer's word and the game's own have none");
  Check(vw::WordWithoutHeadlight(headlight, none) == 0, "headlight alone: the game's own shader");
  Check(vw::WordWithoutHeadlight(headlight_alpha, none) == 1, "its alpha test twin: the game's own alpha test shader (spec 1)");
  Check(vw::WordWithoutHeadlight(composed, all) == gbuffer, "headlight + G-buffer: the G-buffer's word when the pack has it");
  Check(vw::WordWithoutHeadlight(composed, none) == 0, "headlight + G-buffer: the game's own shader when the pack lacks the G-buffer's alone");
  Check(vw::WordWithoutHeadlight(chain::Word(chain::FeatureBit(sp::kFeatureHeadlight) | chain::FeatureBit(sp::kFeatureGBuffer), 1), none) == 1,
        "composed alpha test twin: the spec bit stays");
  // a word the pack must have is asked for by its own number
  std::set<uint32_t> asked;
  vw::WordWithoutHeadlight(composed, [&](uint32_t w) {
    asked.insert(w);
    return true;
  });
  Check(asked.size() == 1 && *asked.begin() == gbuffer, "the pack is asked for the word that is left, once");
  // a word that has no headlight is not changed
  Check(vw::WordWithoutHeadlight(gbuffer, all) == gbuffer && vw::WordWithoutHeadlight(0, none) == 0, "no headlight: the word is the same");
}

}  // namespace

int main() {
  TestFallback();
  TestTheStencilWritingLitDraw();
  TestLadders();
  TestWordWithoutHeadlight();
  std::printf("variant_wait_test: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
