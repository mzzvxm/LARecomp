// Check of the ray tracing benchmark's rules (rt_bench_rules.h): the synthetic scene, the workload's size, the plan from the warm-up's time, the rate from
// the measurement (with synthetic timings), the cache record and the start's action. A program of its own: no renderer, no GPU; exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_rt_bench_rules_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_rt_bench_rules_test
//
// The decision that turns a rate into a tier is rt_tier.h's ApplyBench, checked in rt_tier_test.cpp.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "rt_bench_rules.h"

namespace rb = mcla::gpu::rt_bench;

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

void CheckNear(double got, double want, double tolerance, const std::string& what) {
  ++g_checks;
  if (!(std::fabs(got - want) <= tolerance)) {
    ++g_failed;
    std::printf("FAILED: %s: got %g, want %g (+-%g)\n", what.c_str(), got, want, tolerance);
  }
}

// --- the scene ------------------------------------------------------------------------------------------------------------------

void CheckScene() {
  const rb::Scene scene = rb::MakeScene();
  // 576 boxes of 12 triangles and a ground of 2; 576 boxes of 8 vertices and a ground of 4.
  Check(scene.TriangleCount() == rb::kBoxGrid * rb::kBoxGrid * 12 + 2, "the structure's triangle count: " + std::to_string(scene.TriangleCount()));
  Check(scene.VertexCount() == rb::kBoxGrid * rb::kBoxGrid * 8 + 4, "the structure's vertex count: " + std::to_string(scene.VertexCount()));
  Check(scene.instances.size() == rb::kInstanceGrid * rb::kInstanceGrid, "one instance per cell of the top level's grid");
  Check(scene.TriangleCount() > 5000 && scene.TriangleCount() < 20000, "a small structure: thousands of triangles, not millions");
  // every index names a vertex
  bool in_range = true;
  for (const uint32_t i : scene.indices) {
    in_range = in_range && i < scene.VertexCount();
  }
  Check(in_range, "every index names a vertex");
  Check(scene.indices.size() % 3 == 0 && scene.positions.size() % 3 == 0, "triangle lists, float3 positions");
  // every vertex is inside the structure's footprint and under the tallest box
  float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
  for (size_t v = 0; v < scene.positions.size(); v += 3) {
    for (int k = 0; k < 3; ++k) {
      lo[k] = std::fmin(lo[k], scene.positions[v + k]);
      hi[k] = std::fmax(hi[k], scene.positions[v + k]);
    }
  }
  Check(lo[0] >= 0.0f && hi[0] <= rb::kStructureSide && lo[2] >= 0.0f && hi[2] <= rb::kStructureSide, "the structure fits its footprint");
  Check(lo[1] == 0.0f && hi[1] <= rb::kMaxBoxHeight && hi[1] >= rb::kMinBoxHeight, "the boxes stand between the ground and the height limit");
  // no degenerate triangle (a zero area one would be a hole in the structure's count and no hit)
  int degenerate = 0;
  for (size_t t = 0; t + 2 < scene.indices.size(); t += 3) {
    const float* a = &scene.positions[scene.indices[t] * 3];
    const float* b = &scene.positions[scene.indices[t + 1] * 3];
    const float* c = &scene.positions[scene.indices[t + 2] * 3];
    const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    const float e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    const float cross[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
    if (cross[0] * cross[0] + cross[1] * cross[1] + cross[2] * cross[2] < 1e-6f) {
      ++degenerate;
    }
  }
  Check(degenerate == 0, "no degenerate triangle (" + std::to_string(degenerate) + ")");
  // the instances tile the city: translations on the grid, identity rotation
  std::set<std::pair<int, int>> cells;
  bool identity = true;
  for (const auto& m : scene.instances) {
    identity = identity && m[0] == 1.0f && m[1] == 0.0f && m[2] == 0.0f && m[4] == 0.0f && m[5] == 1.0f && m[6] == 0.0f && m[8] == 0.0f && m[9] == 0.0f &&
               m[10] == 1.0f && m[7] == 0.0f;
    cells.insert({int(std::lround(m[3] / rb::kStructureSide)), int(std::lround(m[11] / rb::kStructureSide))});
  }
  Check(identity, "the instances are translations");
  Check(cells.size() == scene.instances.size(), "the instances are on different cells");
  // determinism: the same scene every time (a cached rate is for this scene)
  const rb::Scene again = rb::MakeScene();
  Check(again.positions == scene.positions && again.indices == scene.indices && again.instances == scene.instances, "the scene is the same every time");
  // the hash is a function of its input only
  Check(rb::Hash(1u) == rb::Hash(1u) && rb::Hash(1u) != rb::Hash(2u), "the hash differs for different input");
  Check(rb::Unit(rb::Hash(7u)) >= 0.0f && rb::Unit(rb::Hash(7u)) < 1.0f, "a hash's unit value is in [0, 1)");
}

void CheckWorkload() {
  Check(rb::kRaysPerDispatch == 512u * 512u * 4u, "a dispatch's rays: 1,048,576");
  Check(rb::kRaysPerDispatch == 1048576u, "a million rays a dispatch");
  Check(rb::kRaysPerRun == uint64_t(rb::kRaysPerDispatch) * rb::kDispatchesPerRun, "a run is kDispatchesPerRun dispatches");
  Check(rb::kRaysPerRun == 8388608ull, "8,388,608 rays a run");
  Check(rb::kThreadsX % rb::kGroup == 0 && rb::kThreadsY % rb::kGroup == 0, "the dispatch is whole thread groups");
  Check(rb::kCounterBytes == 16 && rb::kCounterWords == 4, "the counters' buffer is 16 bytes");
  Check(rb::kMinHitShare > 0.0 && rb::kMinHitShare < 0.5, "the hit share limit is a sensible fraction");
  Check(rb::kBenchVersion >= 1, "a benchmark version exists");
}

// --- the plan ------------------------------------------------------------------------------------------------------------------

void CheckPlan() {
  // A fast card: 1 ms for a million rays: a run is 8 ms, so the budget (150 ms) fits 18 runs, capped at kMaxRuns.
  {
    const rb::Plan p = rb::PlanRuns(1.0);
    Check(p.runs == rb::kMaxRuns && !p.slow, "1 ms warm-up: the most runs");
  }
  // The runs shrink as the card slows: the projected run is 8x the warm-up.
  Check(rb::PlanRuns(6.0).runs == 3 && !rb::PlanRuns(6.0).slow, "6 ms warm-up (a run of 48 ms): 3 runs fit");
  Check(rb::PlanRuns(7.0).runs == 2, "7 ms warm-up (a run of 56 ms): 2 runs fit");
  Check(rb::PlanRuns(10.0).runs == 1, "10 ms warm-up (a run of 80 ms): 1 run");
  Check(rb::PlanRuns(18.0).runs == 1 && !rb::PlanRuns(18.0).slow, "18 ms warm-up (a run of 144 ms): 1 run, within the budget");
  // Over the budget: the warm-up's own rate is the measurement, no run.
  {
    const rb::Plan p = rb::PlanRuns(19.0);
    Check(p.runs == 0 && p.slow, "19 ms warm-up (a run of 152 ms, over the budget): no run, slow");
    const rb::Plan q = rb::PlanRuns(500.0);
    Check(q.runs == 0 && q.slow, "500 ms warm-up: no run, slow");
  }
  // A clock that did not tick: nothing is planned and it is not "slow".
  {
    const rb::Plan p = rb::PlanRuns(0.0);
    Check(p.runs == 0 && !p.slow, "a warm-up of 0: no plan");
    const rb::Plan n = rb::PlanRuns(-1.0);
    Check(n.runs == 0 && !n.slow, "a negative warm-up: no plan");
  }
  // The whole thing stays within about a budget and a warm-up for every card speed.
  for (double warm = 0.05; warm < 100.0; warm *= 1.3) {
    const rb::Plan p = rb::PlanRuns(warm);
    const double total = warm + double(p.runs) * warm * double(rb::kDispatchesPerRun);
    Check(p.slow ? p.runs == 0 : (p.runs >= 1 && p.runs <= rb::kMaxRuns), "plan shape at a warm-up of " + std::to_string(warm));
    Check(total <= rb::kBudgetMs + warm + 1e-9 || p.runs == 1, "the plan's total at a warm-up of " + std::to_string(warm) + " is " + std::to_string(total));
  }
}

// --- the measurement --------------------------------------------------------------------------------------------------------------

rb::Measurement Runs(std::vector<double> ms, double hit_share = 0.6) {
  rb::Measurement m;
  m.warmup_ms = 2.0;
  m.run_ms = std::move(ms);
  m.rays = rb::kRaysPerRun;
  m.hits = uint64_t(double(rb::kRaysPerRun) * hit_share);
  return m;
}

void CheckSummarize() {
  // The best run: 8.39 M rays in 2 ms is 4194 M rays/s.
  {
    const rb::Result r = rb::Summarize(Runs({3.0, 2.0, 2.5}));
    Check(r.outcome == rb::Outcome::kOk && r.usable(), "timed runs: ok");
    CheckNear(r.mrays_per_s, 4194.304, 0.01, "the rate is rays / the fastest run");
    CheckNear(r.best_ms, 2.0, 1e-9, "the time is the fastest run's");
    Check(r.runs == 3, "the run count");
    CheckNear(r.hit_share, 0.6, 1e-3, "the hit share");
  }
  // One run is enough.
  {
    const rb::Result r = rb::Summarize(Runs({10.0}));
    Check(r.outcome == rb::Outcome::kOk && r.runs == 1, "one timed run is a measurement");
    CheckNear(r.mrays_per_s, 838.8608, 0.001, "its rate");
  }
  // No timed run: the warm-up alone is a slow measurement (of one dispatch's rays).
  {
    rb::Measurement m;
    m.warmup_ms = 100.0;
    m.rays = rb::kRaysPerDispatch;
    m.hits = rb::kRaysPerDispatch / 2;
    const rb::Result r = rb::Summarize(m);
    Check(r.outcome == rb::Outcome::kSlow && r.usable(), "the warm-up alone: slow, usable");
    CheckNear(r.mrays_per_s, 10.48576, 0.0001, "the warm-up's rate: a million rays in 100 ms");
    Check(r.runs == 0, "no timed runs");
  }
  // A failed step: failed, nothing usable.
  {
    rb::Measurement m = Runs({2.0});
    m.failed = true;
    const rb::Result r = rb::Summarize(m);
    Check(r.outcome == rb::Outcome::kFailed && !r.usable(), "a failed step: failed");
  }
  // The rays did not hit what they should: invalid, however fast.
  {
    Check(rb::Summarize(Runs({0.2}, 0.0)).outcome == rb::Outcome::kInvalid, "no hits: invalid");
    Check(rb::Summarize(Runs({0.2}, 0.10)).outcome == rb::Outcome::kInvalid, "10% hits: invalid");
    Check(rb::Summarize(Runs({0.2}, 0.24)).outcome == rb::Outcome::kInvalid, "24% hits: invalid");
    Check(rb::Summarize(Runs({0.2}, 0.26)).outcome == rb::Outcome::kOk, "26% hits: ok");
    Check(rb::Summarize(Runs({0.2}, 1.0)).outcome == rb::Outcome::kOk, "every ray hits: ok");
    rb::Measurement none;  // nothing counted
    none.run_ms = {1.0};
    Check(rb::Summarize(none).outcome == rb::Outcome::kInvalid, "no rays counted: invalid");
  }
  // A clock that did not tick: invalid, not an infinite rate.
  {
    Check(rb::Summarize(Runs({0.0, 0.0})).outcome == rb::Outcome::kInvalid, "all runs 0 ms: invalid");
    rb::Measurement m;
    m.warmup_ms = 0.0;
    m.rays = rb::kRaysPerDispatch;
    m.hits = rb::kRaysPerDispatch / 2;
    Check(rb::Summarize(m).outcome == rb::Outcome::kInvalid, "no runs and a warm-up of 0 ms: invalid");
  }
  // The share
  CheckNear(rb::HitShare(100, 25), 0.25, 1e-12, "HitShare");
  CheckNear(rb::HitShare(0, 0), 0.0, 1e-12, "HitShare of nothing is 0");
}

// --- the thresholds -------------------------------------------------------------------------------------------------------------------

void CheckThresholds() {
  Check(rb::kBenchThresholds.strong_mrays > 0.0, "the line is set (a first guess, SET FROM THE GFX COST PASS)");
  Check(rb::kBenchThresholds.band > 0.0 && rb::kBenchThresholds.band < 1.0, "the band is a share of the line");
  // The line's bottom edge must stay above what the slow path produces, so that a card that only ever completes a warm-up is under every line.
  const double warm_rate_limit = double(rb::kRaysPerDispatch) / (rb::kBudgetMs / double(rb::kDispatchesPerRun) * 1000.0);  // a warm-up at the slow limit
  Check(warm_rate_limit < rb::kBenchThresholds.strong_mrays * (1.0 - rb::kBenchThresholds.band), "the slow path's best rate is under the band's bottom edge");
}

// --- the cache record --------------------------------------------------------------------------------------------------------------------

rb::KeyFacts Key() {
  rb::KeyFacts k;
  k.vendor_id = 0x10DE;
  k.device_id = 0x2C02;
  k.driver_version = 32'0'15'7'0'0'0ull;
  k.vulkan = false;
  k.name = "NVIDIA GeForce RTX 5080";
  return k;
}

void CheckRecord() {
  const rb::KeyFacts key = Key();
  const rb::Result in = rb::Summarize(Runs({2.0, 1.5, 1.8}));
  const std::string text = rb::SerializeRecord(key, in);
  Check(text.find("larecomp rt bench 1\n") == 0, "the record starts with its magic line");
  Check(text.find("key=v1|d3d12|10de:2c02|") != std::string::npos && text.find("NVIDIA GeForce RTX 5080") != std::string::npos, "the key names API, ids and card");
  std::string cached_key;
  rb::Result out;
  Check(rb::ParseRecord(text, &cached_key, &out), "a record parses");
  Check(rb::KeyMatches(cached_key, key), "its key matches the facts it was written for");
  Check(out.outcome == in.outcome, "the outcome round-trips");
  CheckNear(out.mrays_per_s, in.mrays_per_s, 0.001, "the rate round-trips");
  CheckNear(out.best_ms, in.best_ms, 0.0001, "the time round-trips");
  CheckNear(out.hit_share, in.hit_share, 0.0001, "the hit share round-trips");
  Check(out.runs == in.runs, "the runs round-trip");
  // CRLF line endings (a file edited on Windows) parse the same.
  {
    std::string crlf;
    for (const char c : text) {
      if (c == '\n') crlf += '\r';
      crlf += c;
    }
    std::string k2;
    rb::Result r2;
    Check(rb::ParseRecord(crlf, &k2, &r2) && rb::KeyMatches(k2, key) && r2.outcome == in.outcome, "a record with CRLF parses");
  }
  // Another card, driver, API or benchmark version: the key differs, the benchmark runs again.
  {
    rb::KeyFacts other = key;
    other.driver_version += 1;
    Check(!rb::KeyMatches(cached_key, other), "a new driver version: another key");
    other = key;
    other.device_id = 0x2C05;
    Check(!rb::KeyMatches(cached_key, other), "another device id: another key");
    other = key;
    other.vulkan = true;
    Check(!rb::KeyMatches(cached_key, other), "another API: another key");
    other = key;
    other.name = "NVIDIA GeForce RTX 5070";
    Check(!rb::KeyMatches(cached_key, other), "another name: another key");
    other = key;
    other.vendor_id = 0x1002;
    Check(!rb::KeyMatches(cached_key, other), "another vendor: another key");
    // a name with a line break or the field separator cannot forge a key or break the record
    other = key;
    other.name = "A|B\nC\r";
    const std::string text2 = rb::SerializeRecord(other, in);
    std::string k3;
    rb::Result r3;
    Check(rb::ParseRecord(text2, &k3, &r3) && rb::KeyMatches(k3, other), "a name with a separator or a line break still makes a valid record");
    Check(rb::KeyText(other).find('\n') == std::string::npos && rb::KeyText(other).find('\r') == std::string::npos, "the key is one line");
  }
  // Every outcome that can be cached round-trips; "not run" is never a record.
  for (const rb::Outcome o : {rb::Outcome::kOk, rb::Outcome::kSlow, rb::Outcome::kInvalid, rb::Outcome::kFailed}) {
    rb::Result r;
    r.outcome = o;
    r.mrays_per_s = o == rb::Outcome::kOk || o == rb::Outcome::kSlow ? 100.0 : 0.0;
    std::string k;
    rb::Result back;
    Check(rb::ParseRecord(rb::SerializeRecord(key, r), &k, &back) && back.outcome == o, std::string("outcome '") + std::string(rb::OutcomeName(o)) + "' round-trips");
  }
  {
    rb::Result r;  // kNotRun
    std::string k;
    rb::Result back;
    Check(!rb::ParseRecord(rb::SerializeRecord(key, r), &k, &back), "a not-run record is refused");
  }
  // Garbage is refused: empty, another file, a missing field, a field that is not a number, a negative number.
  {
    std::string k;
    rb::Result r;
    Check(!rb::ParseRecord("", &k, &r), "empty text is no record");
    Check(!rb::ParseRecord("hello\nkey=x\n", &k, &r), "another file is no record");
    Check(!rb::ParseRecord("larecomp rt bench 2\nkey=x\noutcome=ok\nmrays=1\nbest_ms=1\nhit=1\nruns=1\n", &k, &r), "another version of the format is no record");
    std::string no_mrays = text;
    no_mrays.replace(no_mrays.find("mrays="), 6, "mraysX");
    Check(!rb::ParseRecord(no_mrays, &k, &r), "a missing field is no record");
    std::string bad_number = text;
    const size_t at = bad_number.find("mrays=") + 6;
    bad_number.replace(at, 4, "abcd");
    Check(!rb::ParseRecord(bad_number, &k, &r), "a field that is not a number is no record");
    std::string negative = text;
    negative.replace(negative.find("mrays=") + 6, 0, "-");
    Check(!rb::ParseRecord(negative, &k, &r), "a negative rate is no record");
    std::string nan = text;
    nan.replace(nan.find("mrays=") + 6, 4, "nan ");
    Check(!rb::ParseRecord(nan, &k, &r), "a nan is no record");
    std::string unknown_outcome = text;
    unknown_outcome.replace(unknown_outcome.find("outcome=ok"), 10, "outcome=zz");
    Check(!rb::ParseRecord(unknown_outcome, &k, &r), "an unknown outcome is no record");
  }
  // An extra line (a later version's field) does not hurt.
  {
    std::string k;
    rb::Result r;
    Check(rb::ParseRecord(text + "extra=1\n", &k, &r), "an unknown field is ignored");
  }
}

// --- the start's action --------------------------------------------------------------------------------------------------------------------

void CheckAction() {
  Check(rb::ActionFor("auto", false) == rb::Action::kRun, "auto, no cache: run");
  Check(rb::ActionFor("auto", true) == rb::Action::kUseCache, "auto, a valid cache: use it");
  Check(rb::ActionFor("force", true) == rb::Action::kRun, "force, a valid cache: run anyway");
  Check(rb::ActionFor("force", false) == rb::Action::kRun, "force, no cache: run");
  Check(rb::ActionFor("off", true) == rb::Action::kSkip, "off: skip, the cache is not read");
  Check(rb::ActionFor("off", false) == rb::Action::kSkip, "off, no cache: skip");
  Check(rb::ActionFor("", false) == rb::Action::kRun, "an unknown word is auto");
  Check(rb::ActionFor("AUTO", true) == rb::Action::kUseCache, "an unknown word is auto, with a cache");
}

}  // namespace

int main() {
  CheckScene();
  CheckWorkload();
  CheckPlan();
  CheckSummarize();
  CheckThresholds();
  CheckRecord();
  CheckAction();
  std::printf("rt_bench_rules_test: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
