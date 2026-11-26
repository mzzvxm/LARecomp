// Check of which draws may wait in the background for their pipeline whatever the game's rule says (variant_wait.h). A program of its own: it needs no
// renderer, no ReXGlue and no GPU, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_variant_wait_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_variant_wait_test [<src/gpu folder>]
//
// Standalone: clang++ -std=c++20 -I <repo>/src -I <repo>/src/gpu variant_wait_test.cpp
//
// The checks of the ladder are on the header's model (LadderOf, NextStep); SourceChecks reads Scene::Prepare in scene.cpp and the failure paths in pipelines.cpp (the folder of
// this file, or the one given) and checks they walk the same ladder: every lookup of a key asks through the header, in the order the model has, and a lookup that ended with
// no pipeline (being made, or its build FAILED) takes the next rung through NextStep.

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <string_view>

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

void TestIdentityBloom() {
  const uint32_t feature = chain::FeatureBit(sp::kFeatureBloomGauss);
  for (uint32_t spec : {0u, 1u}) {
    const uint32_t bloom = chain::Word(feature, spec);
    const uint32_t other = chain::Word(chain::FeatureBit(sp::kFeatureLuminanceScale), spec);
    const uint32_t composed = chain::Word(feature | chain::FeatureBit(sp::kFeatureHeadlight), spec);
    Check(vw::BloomWord(bloom, true, true) == spec, "native-scale bloom keeps the guest specialization");
    Check(vw::BloomWord(bloom, false, true) == bloom, "scaled bloom keeps the fix");
    Check(vw::BloomWord(bloom, true, false) == bloom, "the A/B arm keeps native-scale bloom");
    Check(vw::BloomWord(other, true, true) == other, "the source scale of exposure is independent");
    Check(vw::BloomWord(composed, true, true) == composed, "other features survive in composed words");
  }
}

void TestUiSharpFallback() {
  const uint32_t feature = chain::FeatureBit(sp::kFeatureUiSharp);
  for (uint32_t spec : {0u, 1u}) {
    Check(vw::CanUseUiSharpFallback(chain::Word(feature, spec), false), "both UI specializations may use a matching guest pipeline");
    Check(!vw::CanUseUiSharpFallback(chain::Word(feature, spec), true), "an attached plane must keep its existing fallback ladder");
    Check(!vw::CanUseUiSharpFallback(spec, false), "a guest draw without the optional shader stays mandatory where required");
    Check(!vw::CanUseUiSharpFallback(chain::Word(feature | chain::FeatureBit(sp::kFeatureHeadlight), spec), false),
          "a composed word cannot fall back by stripping UI sharpening alone");
  }
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
  const vw::Ladder road = vw::LadderOf({true, true, true});
  Check(road.count == 4 && road.rungs[0].extras.planes && !road.rungs[1].extras.planes &&
            road.rungs[1].extras.road_variant && !road.rungs[2].extras.road_variant &&
            road.rungs[2].extras.headlight && road.rungs[3].ask == vw::Ask::kGameRule,
        "road pipeline fallback keeps the headlight");
  Check(vw::NextStep({false, false, true}, false, false) == vw::Next::kFallBack &&
            vw::NextStep({false, false, true}, false, true) == vw::Next::kFallBack,
        "a standalone road variant falls back whether pending or failed");
  const uint32_t road_and_light = chain::Word(chain::FeatureBit(sp::kFeatureRoadMirrorBlur) | chain::FeatureBit(sp::kFeatureHeadlight), 1);
  const uint32_t headlight_twin = chain::Word(chain::FeatureBit(sp::kFeatureHeadlight), 1);
  Check(vw::RoadFeature(road_and_light) == sp::kFeatureRoadMirrorBlur && vw::RoadFeature(headlight_twin) == chain::kNoFeature &&
            vw::WordWithoutFeature(road_and_light, sp::kFeatureRoadMirrorBlur, [&](uint32_t word) { return word == headlight_twin; }) == headlight_twin,
        "removing the blur preserves the headlight and alpha test when the pack has that word");
  const uint32_t glow = chain::Word(chain::FeatureBit(sp::kFeatureGlowNeutral), 1);
  const uint32_t glow_feature = vw::RoadFeature(glow);
  Check(glow_feature == sp::kFeatureGlowNeutral &&
            vw::WordWithoutFeature(glow, glow_feature, [](uint32_t) { return false; }) == 1,
        "the reflected glow falls back to the game's shader with the same alpha test");
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

// How one lookup ended, as Scene::Prepare sees it: the pipeline, "being made", or a failed build (null and not pending).
enum class Result : uint8_t { kReady, kPending, kFailed };

// The walk Scene::Prepare does down the ladder of a draw whose full key has `full` extras; `results[i]` is how the lookup of rung i ends. What comes out: the rung the draw is
// drawn with (-1: none) and how it ends when no rung gave a pipeline (kWait: left out for now, kDrop: skipped for good).
struct Walked {
  int drawn_with = -1;
  vw::Next end = vw::Next::kDraw;
};
Walked Walk(const vw::Extras& full, const Result (&results)[3]) {
  const vw::Ladder ladder = vw::LadderOf(full);
  Walked out;
  for (size_t i = 0; i < ladder.count; ++i) {
    const vw::Next step = vw::NextStep(ladder.rungs[i].extras, results[i] == Result::kReady, results[i] == Result::kPending);
    out.end = step;
    if (step == vw::Next::kDraw) {
      out.drawn_with = int(i);
      return out;
    }
    if (step != vw::Next::kFallBack) {
      return out;
    }
  }
  return out;
}

void TestNextStep() {
  // The decision for one lookup.
  for (bool planes : {false, true}) {
    for (bool headlight : {false, true}) {
      const vw::Extras e{planes, headlight};
      const bool fb = vw::HasFallback(e);
      const std::string name = std::string("planes ") + (planes ? "yes" : "no") + ", headlight " + (headlight ? "yes" : "no");
      Check(vw::NextStep(e, true, false) == vw::Next::kDraw && vw::NextStep(e, true, true) == vw::Next::kDraw, name + ": a pipeline that is there is drawn with");
      Check(vw::NextStep(e, false, true) == (fb ? vw::Next::kFallBack : vw::Next::kWait), name + ": being made: the rung below, or left out for now when there is none");
      Check(vw::NextStep(e, false, false) == (fb ? vw::Next::kFallBack : vw::Next::kDrop),
            name + ": its build FAILED: the rung below, and the draw is dropped only when there is none (the game's own key)");
    }
  }
  // Every ladder and every outcome of its lookups. The first rung that has a pipeline draws; with none, the game's own key (the last rung) decides: being made leaves the draw
  // out for now, a failure drops it. A variant that failed or is being made never decides.
  size_t cases = 0;
  size_t bad = 0;
  const Result all[] = {Result::kReady, Result::kPending, Result::kFailed};
  for (bool planes : {false, true}) {
    for (bool headlight : {false, true}) {
      const vw::Extras full{planes, headlight};
      const size_t last = vw::LadderOf(full).count - 1;
      for (Result a : all) {
        for (Result b : all) {
          for (Result c : all) {
            const Result results[3] = {a, b, c};
            const Walked w = Walk(full, results);
            ++cases;
            int first_ready = -1;
            for (size_t i = 0; i <= last && first_ready < 0; ++i) {
              if (results[i] == Result::kReady) first_ready = int(i);
            }
            if (first_ready >= 0) {
              bad += !(w.drawn_with == first_ready && w.end == vw::Next::kDraw);
            } else if (results[last] == Result::kFailed) {
              bad += !(w.drawn_with < 0 && w.end == vw::Next::kDrop);
            } else {
              bad += !(w.drawn_with < 0 && w.end == vw::Next::kWait);
            }
          }
        }
      }
    }
  }
  Check(bad == 0 && cases == 4 * 27, "every ladder and every outcome of its lookups: the first rung with a pipeline draws, a variant that failed or is being made falls to the next, "
                                     "the draw is dropped only when the game's own key failed (" + std::to_string(cases) + " cases, " + std::to_string(bad) + " wrong)");
  // The case the stall review found: both variants failed, the game's own pipeline is there: it draws (before, the draw was left out for the whole session).
  const Result failed_variants[3] = {Result::kFailed, Result::kFailed, Result::kReady};
  const Walked a = Walk({true, true}, failed_variants);
  Check(a.drawn_with == 2 && a.end == vw::Next::kDraw, "planes + headlight both failed: the game's own pipeline draws");
  const Result mixed[3] = {Result::kFailed, Result::kReady, Result::kReady};
  Check(Walk({true, true}, mixed).drawn_with == 1, "the planes' variant failed, the key with the headlight is there: the draw takes it");
  const Result game_failed[3] = {Result::kFailed, Result::kFailed, Result::kFailed};
  Check(Walk({true, true}, game_failed).end == vw::Next::kDrop && Walk({false, false}, game_failed).end == vw::Next::kDrop, "the game's own key failing too: the draw is skipped");
}

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

size_t Count(const std::string& text, const std::string& what) {
  size_t n = 0;
  for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + what.size())) {
    ++n;
  }
  return n;
}

// Scene::Prepare walks the ladder by hand (three lookups of the pipeline, each asking through variant_wait::AskFor): a fourth lookup that asks the game's rule alone, or a
// rung moved, would bring the render thread's builds back without any check above noticing.
void SourceChecks(const std::string& gpu) {
  const std::string scene = ReadFile(gpu + "/scene.cpp");
  Check(!scene.empty(), "scene.cpp is readable in " + gpu);
  const size_t begin = scene.find("const SkipAsk ask{this, &d};");
  const size_t end = scene.find("pipeline_memo_key_ = pipeline_key;", begin);
  Check(begin != std::string::npos && end != std::string::npos && begin < end, "Scene::Prepare's lookup of the pipeline is where the check expects it");
  if (begin == std::string::npos || end == std::string::npos || begin >= end) {
    return;
  }
  const std::string lookup = scene.substr(begin, end - begin);
  Check(scene.find("#include \"variant_wait.h\"") != std::string::npos, "scene.cpp includes variant_wait.h");
  Check(Count(lookup, "pipelines_->Get(") == 4, "the ladder has four lookups including the road blur fallback");
  Check(Count(lookup, "pipelines_->Get(pipeline_key, inputs, ask_for(pipeline_key), &pending)") == 4, "every lookup uses ask_for");
  // A lookup that ended with no pipeline takes the rung below when it was being made OR when its build failed: the rungs' conditions go through NextStep, none looks at `pending` alone.
  Check(lookup.find("const auto falls_back = [&]() { return variant_wait::NextStep(extras_of(pipeline_key), d.pipeline != nullptr, pending) == variant_wait::Next::kFallBack; };") != std::string::npos,
        "the rungs ask variant_wait::NextStep whether the lookup that just ended falls back (being made, or a failed build)");
  Check(Count(lookup, "falls_back()") == 3 && Count(lookup, "!d.pipeline && pending") == 0, "the fallback rungs handle pending and failed pipelines");
  Check(Count(lookup, "pipelines_->NoteVariantFailedDraw();") == 3, "a draw that fell back because the build failed is counted at every rung");
  Check(scene.find("pipeline_memo_ = d.pipeline;\n    } else if (pending) {", begin) != std::string::npos,
        "after the last rung only a build under way leaves the draw out for now (a failure of the game's own key is Skip::kPipeline)");
  Check(Count(lookup, "&deferral") == 1 && lookup.find("? &waits : &deferral;") != std::string::npos, "the game's rule is used in one place: ask_for's answer for a key with no extra");
  Check(lookup.find("const Deferral waits{&Scene::AlwaysMaySkip, nullptr, sync_builds::Kind::kGuestDraw, true};") != std::string::npos,
        "the deferral of a key with a fallback always says pending, counts as a guest draw and says it has a fallback (a failed build is final for it)");
  Check(lookup.find("variant_wait::CanUseUiSharpFallback(key.ps_variant, d.gbuffer_attached)") != std::string::npos &&
            lookup.find("larecomp::settings::mcla_gpu_ui_sharp_background.Get()") != std::string::npos &&
            lookup.find("ui_fallback.key = key;\n        ui_fallback.key.ps_variant = chain::SpecOf(key.ps_variant);") != std::string::npos &&
            lookup.find("return &ui_deferral;") != std::string::npos,
        "the UI fallback requires the switch and changes only the pixel word, preserving alpha specialization");
  Check(lookup.find("if (ui_fallback.used)") < lookup.find("pipeline_key = ui_fallback.key;") &&
            lookup.find("pipeline_key = ui_fallback.key;") < lookup.find("} else if (d.pipeline) {"),
        "the fallback uses the guest constants and leaves the original pipeline out of the draw memo");
  Check(lookup.find("variant_wait::Extras{d.gbuffer_attached, variant_wait::HasHeadlight(key.ps_variant),") != std::string::npos &&
            lookup.find("variant_wait::RoadFeature(key.ps_variant) != chain::kNoFeature") != std::string::npos &&
            lookup.find("variant_wait::AskFor(extras_of(key)) == variant_wait::Ask::kAlways") != std::string::npos,
        "ask_for reads the planes and pixel features of the key being requested");
  const size_t first = lookup.find("pipelines_->Get(");
  const size_t planes = lookup.find("if (falls_back() && d.gbuffer_attached) {");
  const size_t road = lookup.find("if (falls_back() && road_feature != chain::kNoFeature) {");
  const size_t headlight = lookup.find("if (falls_back() && variant_wait::HasHeadlight(pipeline_key.ps_variant)) {");
  Check(headlight != std::string::npos && first < planes && planes < road && road < headlight,
        "the ladder tries the full key, then removes planes, the road variant and headlight in order");
  Check(lookup.find("variant_wait::WordWithoutFeature(pipeline_key.ps_variant, road_feature,") != std::string::npos,
        "the road variant's fallback strips the feature carried by its word");
  const size_t glow_start = scene.find("constexpr uint32_t glow_feature");
  if (glow_start != std::string::npos && glow_start < begin) {
    const auto select_glow = std::string_view(scene).substr(glow_start, begin - glow_start);
    Check(select_glow.find("mcla_gpu_glow_neutral.Get()") != std::string_view::npos &&
              select_glow.find("== SideView::kReflection") != std::string_view::npos &&
              select_glow.find("std::has_single_bit(d.ps->texture_fetch_mask)") != std::string_view::npos &&
              select_glow.find("fetch.format == xe::TextureFormat::k_DXT1") != std::string_view::npos,
          "the live glow fix is scoped to one BC1 fetch in the road reflection view");
    Check(select_glow.find("chain::SpecOf(pipeline_key.ps_variant)") != std::string_view::npos &&
              select_glow.find("d.ps->HasWord(word)") != std::string_view::npos,
          "the draw selects only an available variant with matching alpha-test state");
  } else {
    Check(false, "the reflected glow word is selected before the pipeline lookup");
  }
  Check(lookup.find("variant_wait::WordWithoutHeadlight(pipeline_key.ps_variant,") != std::string::npos, "the headlight's fallback takes the word the header names");
  Check(scene.find("variant pipelines failed {} (draws that fell back {})") != std::string::npos && scene.find("ps.variant_failed, ps.variant_fallback_draws") != std::string::npos,
        "the scene interval line counts the variant builds that failed and the draws that took the game's own pipeline");

  // Pipelines: every path that ends a build with no pipeline books it through LogFailure with whether the asker has a fallback, so the books, the log and the answer agree.
  const std::string pipelines = ReadFile(gpu + "/pipelines.cpp");
  Check(!pipelines.empty(), "pipelines.cpp is readable in " + gpu);
  Check(Count(pipelines, "LogFailure(") == 4, "LogFailure books every failed build: its definition and three calls (the render thread's build, an adopted request, a precache entry)");
  Check(pipelines.find("LogFailure(key, vs_result, ps_result, miss.HasFallback());") != std::string::npos,
        "the render thread's build reports a failure with whether the draw has a fallback");
  Check(pipelines.find("LogFailure(key, vs_result, ps_result, r.fallback);") != std::string::npos, "an adopted background request reports it with what its asker said");
  Check(pipelines.find("LogFailure(key, p.vs_result, p.ps_result, true);") != std::string::npos && pipelines.find("p.vs_result = vs_result;") != std::string::npos,
        "a precache entry that failed on a worker keeps its error and is reported, not made again, for a draw that has a fallback");
  Check(pipelines.find("if (fallback) {") != std::string::npos && pipelines.find("++stats_.variant_failed;") != std::string::npos, "a failed key with a fallback is logged once and counted");
  Check(pipelines.find("miss.ready_fallback = fallback != nullptr;") != std::string::npos &&
            pipelines.find("if (miss.ready_fallback)") != std::string::npos,
        "the UI variant is eligible to defer only after an original pipeline is found");
  Check(pipelines.find("if (!deferral || !deferral->ready_fallback || sync_builds::CaptureActive())") != std::string::npos &&
            pipelines.find("const auto alias = aliases_.find(key);\n  return alias != aliases_.end() ? alias->second : nullptr;") != std::string::npos,
        "captures use the exact pipeline and the UI fallback lookup only reads already adopted pipelines");
  Check(pipelines.find("used->second.UsedIn(frame_)") != std::string::npos &&
            pipelines.find("NoteReadyFallback(key, *deferral->ready_fallback, true);") != std::string::npos,
        "each key that fell back while pending stays on the same pipeline for that guest frame");
}

}  // namespace

int main(int argc, char** argv) {
  std::string gpu = argc > 1 ? argv[1] : std::string();
  if (gpu.empty()) {
    std::string here = __FILE__;
    for (char& c : here) {
      if (c == '\\') c = '/';
    }
    const size_t slash = here.rfind('/');
    gpu = slash == std::string::npos ? std::string(".") : here.substr(0, slash);
  }
  TestFallback();
  TestIdentityBloom();
  TestUiSharpFallback();
  TestTheStencilWritingLitDraw();
  TestLadders();
  TestWordWithoutHeadlight();
  TestNextStep();
  SourceChecks(gpu);
  std::printf("variant_wait_test: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
