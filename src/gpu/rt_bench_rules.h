#pragma once
// ===========================================================================
// MCLA GPU renderer -- the ray tracing micro-benchmark: the rules (no GPU, no renderer)
// ===========================================================================
// The RT tier (rt_tier.h: none / weak / strong) is found from the graphics API and a table of cards. A table cannot know a laptop's power
// limit, a card it has no row for, or what a driver does with inline ray queries, so the renderer may also MEASURE: once, on the first
// start (and again whenever the card or its driver changes), a short compute dispatch traces rays through a small synthetic city on the
// device's own ray query path and says how many million rays a second the card traces. rt_bench.cpp does the GPU work; this file is every
// decision around it, as plain functions that rt_bench_rules_test.cpp checks without a device:
//   - the synthetic scene (MakeScene: a grid of boxes on a ground plane, instanced in a grid) and the workload's size (kRaysPerRun, ...)
//   - the plan: one warm-up dispatch of a million rays is timed, and from its time the number of timed runs is chosen so that the whole
//     thing takes about kBudgetMs on a fast card and never more than that plus one warm-up on a slow one (PlanRuns)
//   - the measurement and the throughput (Summarize): the fastest timed run (the others carry clocks ramping and the OS), or the warm-up when
//     it was too slow to go on, and a hit-share check that proves the rays traced something (a scene that missed everything is fast and
//     meaningless)
//   - the cache record (SerializeRecord, ParseRecord, KeyMatches): written to the settings folder next to larecomp.toml, valid for one
//     card + driver + API + benchmark version, so the benchmark runs again after any of them changes and not otherwise
// The decision that turns a throughput into a tier (it needs the tier type) is rt_tier.h's ApplyBench; the numbers it compares with are the
// constants of kBenchThresholds below.
//
// THE THRESHOLDS ARE NOT MEASURED YET. kBenchThresholds, kRaysPerRun's scene and every other number marked "SET FROM THE GFX COST PASS" are
// first guesses from the design files' throughput estimates (perf/CITY-RT-M2.md section 8: closest-hit with shading about 2.5 to 3.5 G rays/s on an
// RTX 5080, 0.8 to 1.1 on an RTX 3060, 0.4 to 0.6 on an RTX 2060 or an RX 6600; this synthetic scene has no shading and a small structure, so its
// numbers are higher). The band around the line is wide (kBenchThresholds.band) so that a wrong guess rarely overrides the table's row. The first
// runs on the test PCs log the number ("[gpu] rt bench: ... M rays/s"); the GFX cost pass then sets the line from the cost of the RT effects.
// ===========================================================================

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace mcla::gpu::rt_bench {

// Bump when the scene, the shader or the way the rate is measured changes: a cached number of an older benchmark is not this benchmark's.
inline constexpr uint32_t kBenchVersion = 1;

// --- the synthetic scene --------------------------------------------------------------------------------------------------

inline constexpr uint32_t kBoxGrid = 24;        // boxes along each side of one structure
inline constexpr float kBoxPitch = 10.0f;       // metres between box centres
inline constexpr uint32_t kInstanceGrid = 4;    // instances of the structure along each side of the top level
inline constexpr float kStructureSide = kBoxGrid * kBoxPitch;                  // 240 m
inline constexpr float kCitySide = kStructureSide * kInstanceGrid;             // 960 m
inline constexpr float kMinBoxHeight = 4.0f;
inline constexpr float kMaxBoxHeight = 40.0f;

struct Scene {
  std::vector<float> positions;    // x y z per vertex, one structure (the bottom level)
  std::vector<uint32_t> indices;   // three per triangle
  std::vector<std::array<float, 12>> instances;  // row-major 3x4 transform of each instance of the structure
  uint32_t TriangleCount() const { return uint32_t(indices.size() / 3); }
  uint32_t VertexCount() const { return uint32_t(positions.size() / 3); }
};

// A small integer hash (Wang-style), so the scene is the same on every machine and every start.
constexpr uint32_t Hash(uint32_t x) {
  x = (x ^ 61u) ^ (x >> 16);
  x *= 9u;
  x = x ^ (x >> 4);
  x *= 0x27d4eb2du;
  x = x ^ (x >> 15);
  return x;
}

// A float in [0, 1) from a hash.
constexpr float Unit(uint32_t h) { return float(h >> 8) * (1.0f / 16777216.0f); }

// One structure: kBoxGrid x kBoxGrid boxes (a random footprint of 4 to 8 m and a random height of kMinBoxHeight to kMaxBoxHeight) standing on a
// ground quad, then kInstanceGrid x kInstanceGrid instances of it side by side. About 7,000 triangles in the bottom level, 16 instances, so
// the traversal crosses the top level and the bottom level as the car and city passes do.
inline Scene MakeScene() {
  Scene scene;
  const auto add_vertex = [&](float x, float y, float z) {
    scene.positions.push_back(x);
    scene.positions.push_back(y);
    scene.positions.push_back(z);
    return uint32_t(scene.positions.size() / 3 - 1);
  };
  const auto add_triangle = [&](uint32_t a, uint32_t b, uint32_t c) {
    scene.indices.push_back(a);
    scene.indices.push_back(b);
    scene.indices.push_back(c);
  };
  // The ground, one quad under the whole structure.
  {
    const uint32_t v0 = add_vertex(0.0f, 0.0f, 0.0f);
    const uint32_t v1 = add_vertex(kStructureSide, 0.0f, 0.0f);
    const uint32_t v2 = add_vertex(kStructureSide, 0.0f, kStructureSide);
    const uint32_t v3 = add_vertex(0.0f, 0.0f, kStructureSide);
    add_triangle(v0, v2, v1);
    add_triangle(v0, v3, v2);
  }
  for (uint32_t gz = 0; gz < kBoxGrid; ++gz) {
    for (uint32_t gx = 0; gx < kBoxGrid; ++gx) {
      const uint32_t h = Hash(gz * kBoxGrid + gx + 1u);
      const float half_x = 2.0f + 2.0f * Unit(Hash(h ^ 0x68bc21ebu));
      const float half_z = 2.0f + 2.0f * Unit(Hash(h ^ 0x02e5be93u));
      const float height = kMinBoxHeight + (kMaxBoxHeight - kMinBoxHeight) * Unit(Hash(h ^ 0x967a889bu));
      const float cx = (float(gx) + 0.5f) * kBoxPitch;
      const float cz = (float(gz) + 0.5f) * kBoxPitch;
      uint32_t c[8];
      for (uint32_t k = 0; k < 8; ++k) {
        c[k] = add_vertex(cx + ((k & 1u) ? half_x : -half_x), (k & 2u) ? height : 0.0f, cz + ((k & 4u) ? half_z : -half_z));
      }
      // Six faces, two triangles each (the corner k has bit 0 = +x, bit 1 = +y, bit 2 = +z).
      const uint32_t faces[6][4] = {{0, 1, 3, 2}, {4, 6, 7, 5}, {0, 2, 6, 4}, {1, 5, 7, 3}, {0, 4, 5, 1}, {2, 3, 7, 6}};
      for (const auto& f : faces) {
        add_triangle(c[f[0]], c[f[1]], c[f[2]]);
        add_triangle(c[f[0]], c[f[2]], c[f[3]]);
      }
    }
  }
  for (uint32_t iz = 0; iz < kInstanceGrid; ++iz) {
    for (uint32_t ix = 0; ix < kInstanceGrid; ++ix) {
      std::array<float, 12> m = {1.0f, 0.0f, 0.0f, float(ix) * kStructureSide, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, float(iz) * kStructureSide};
      scene.instances.push_back(m);
    }
  }
  return scene;
}

// --- the workload -----------------------------------------------------------------------------------------------------------

inline constexpr uint32_t kGroup = 8;              // the shader's thread group is 8 x 8
inline constexpr uint32_t kThreadsX = 512;         // threads along each side of a dispatch
inline constexpr uint32_t kThreadsY = 512;
inline constexpr uint32_t kRaysPerThread = 4;
inline constexpr uint32_t kRaysPerDispatch = kThreadsX * kThreadsY * kRaysPerThread;  // 1,048,576
inline constexpr uint32_t kDispatchesPerRun = 8;
inline constexpr uint64_t kRaysPerRun = uint64_t(kRaysPerDispatch) * kDispatchesPerRun;  // 8,388,608
inline constexpr float kRayLength = 200.0f;  // metres: a ray that finds nothing within it is a miss

// The counters the shader writes (a 16-byte buffer): rays traced, rays that hit.
inline constexpr uint32_t kCounterRays = 0;
inline constexpr uint32_t kCounterHits = 1;
inline constexpr uint32_t kCounterWords = 4;
inline constexpr uint32_t kCounterBytes = kCounterWords * 4;

// Rays from inside the city pointing in random directions: about every ray that points down meets the ground within kRayLength and the ones
// that point sideways meet a box, so well over a third hit. Below kMinHitShare the scene or its transforms went wrong (the TLAS is empty or
// misplaced) and the time means nothing.
inline constexpr double kMinHitShare = 0.25;

// --- the plan -----------------------------------------------------------------------------------------------------------------

// SET FROM THE GFX COST PASS (the benchmark's own cost; the line it measures against is kBenchThresholds below).
inline constexpr double kBudgetMs = 150.0;  // the timed runs, together, aim for this on any card
inline constexpr uint32_t kMaxRuns = 3;     // and never more than this many

struct Plan {
  uint32_t runs = 0;  // timed runs of kDispatchesPerRun dispatches each; 0: none, the warm-up alone is the measurement
  bool slow = false;  // even one run would take over kBudgetMs (under about 56 M rays/s: an integrated or a software device): the warm-up's rate is taken
};

// `warmup_ms`: the time of the one warm-up dispatch (kRaysPerDispatch rays, the first use of the pipeline: a little pessimistic on purpose). A
// warm-up that took no time (a clock that did not tick) plans no run either: Summarize calls that invalid.
inline Plan PlanRuns(double warmup_ms) {
  Plan plan;
  if (!(warmup_ms > 0.0)) {
    return plan;
  }
  const double run_ms = warmup_ms * double(kDispatchesPerRun);  // one run, projected from the warm-up
  if (run_ms > kBudgetMs) {
    plan.slow = true;
    return plan;
  }
  plan.runs = uint32_t(std::clamp(std::floor(kBudgetMs / run_ms), 1.0, double(kMaxRuns)));
  return plan;
}

// --- the measurement ------------------------------------------------------------------------------------------------------------

enum class Outcome : uint8_t {
  kNotRun,    // the benchmark did not run (off, no ray tracing, integrated GPU, or nothing asked)
  kOk,        // a rate was measured
  kSlow,      // the warm-up alone was over the limit: a rate from the warm-up (a lower bound: it includes the first-use costs)
  kInvalid,   // it ran and the rays did not hit what they should (the hit share): no rate is trusted
  kFailed,    // a step could not be made (the shader, the structures, the pipeline) or the device was lost
};

constexpr std::string_view OutcomeName(Outcome o) {
  switch (o) {
    case Outcome::kOk: return "ok";
    case Outcome::kSlow: return "slow";
    case Outcome::kInvalid: return "invalid";
    case Outcome::kFailed: return "failed";
    default: return "not run";
  }
}

struct Measurement {
  double warmup_ms = 0.0;         // the one warm-up dispatch
  std::vector<double> run_ms;     // each timed run (kRaysPerRun rays)
  uint64_t rays = 0;              // the shader's own counters of the last timed run (or of the warm-up when there was none)
  uint64_t hits = 0;
  bool failed = false;            // a step failed (the benchmark made no measurement)
};

struct Result {
  Outcome outcome = Outcome::kNotRun;
  double mrays_per_s = 0.0;   // million rays a second
  double best_ms = 0.0;       // the time that rate was computed from (a run of kRaysPerRun rays, or the warm-up's kRaysPerDispatch)
  double hit_share = 0.0;
  uint32_t runs = 0;
  // Usable for the decision: a rate that can be compared with a line.
  bool usable() const { return (outcome == Outcome::kOk || outcome == Outcome::kSlow) && mrays_per_s > 0.0; }
};

inline double HitShare(uint64_t rays, uint64_t hits) {
  return rays != 0 ? double(hits) / double(rays) : 0.0;
}

// The rate: the fastest timed run (the clocks have ramped by then and the OS has had its slices elsewhere); with no timed run, the warm-up.
inline Result Summarize(const Measurement& m) {
  Result r;
  if (m.failed) {
    r.outcome = Outcome::kFailed;
    return r;
  }
  r.hit_share = HitShare(m.rays, m.hits);
  if (m.rays == 0 || r.hit_share < kMinHitShare) {
    r.outcome = Outcome::kInvalid;
    return r;
  }
  if (!m.run_ms.empty()) {
    const double best = *std::min_element(m.run_ms.begin(), m.run_ms.end());
    if (!(best > 0.0)) {
      r.outcome = Outcome::kInvalid;
      return r;
    }
    r.outcome = Outcome::kOk;
    r.best_ms = best;
    r.runs = uint32_t(m.run_ms.size());
    r.mrays_per_s = double(kRaysPerRun) / (best * 1000.0);  // rays per millisecond / 1000 = million rays per second
    return r;
  }
  if (!(m.warmup_ms > 0.0)) {
    r.outcome = Outcome::kInvalid;
    return r;
  }
  r.outcome = Outcome::kSlow;
  r.best_ms = m.warmup_ms;
  r.mrays_per_s = double(kRaysPerDispatch) / (m.warmup_ms * 1000.0);
  return r;
}

// --- the thresholds ---------------------------------------------------------------------------------------------------------------

// The line between weak and strong, in million rays a second on THIS scene. SET FROM THE GFX COST PASS: the number below sits between the
// estimates for an RTX 3060 and an RTX 5080 (a few thousand against about ten thousand on this unshaded scene).
// `band` is the share either side of the line inside which the table's row stands whatever the benchmark says: a card is moved up only when it
// is clearly above the line and down only when it is clearly below, so the guess above cannot flip a card the table knew, and two starts of the
// same card that measure a little differently stay in the same tier.
struct Thresholds {
  double strong_mrays = 0.0;
  double band = 0.0;
};
inline constexpr Thresholds kBenchThresholds = {5000.0, 0.40};  // SET FROM THE GFX COST PASS

// --- the cache record ---------------------------------------------------------------------------------------------------------------

// What a cached result is valid for. Any change in it runs the benchmark again.
struct KeyFacts {
  uint32_t vendor_id = 0;
  uint32_t device_id = 0;      // 0 when the API did not say
  uint64_t driver_version = 0;
  bool vulkan = false;
  std::string name;
};

inline std::string KeyText(const KeyFacts& f) {
  char head[96];
  std::snprintf(head, sizeof(head), "v%u|%s|%04x:%04x|drv %llu|", kBenchVersion, f.vulkan ? "vulkan" : "d3d12", f.vendor_id, f.device_id,
                static_cast<unsigned long long>(f.driver_version));
  std::string key = head;
  for (const char c : f.name) {
    key += (c == '\n' || c == '\r' || c == '|') ? ' ' : c;  // one line, and no field separator in it
  }
  return key;
}

inline constexpr std::string_view kRecordMagic = "larecomp rt bench 1";

inline std::string SerializeRecord(const KeyFacts& facts, const Result& r) {
  char body[256];
  std::snprintf(body, sizeof(body), "outcome=%s\nmrays=%.3f\nbest_ms=%.4f\nhit=%.4f\nruns=%u\n", std::string(OutcomeName(r.outcome)).c_str(),
                r.mrays_per_s, r.best_ms, r.hit_share, r.runs);
  return std::string(kRecordMagic) + "\nkey=" + KeyText(facts) + "\n" + body;
}

namespace detail {

inline Outcome OutcomeOf(std::string_view text) {
  if (text == "ok") return Outcome::kOk;
  if (text == "slow") return Outcome::kSlow;
  if (text == "invalid") return Outcome::kInvalid;
  if (text == "failed") return Outcome::kFailed;
  return Outcome::kNotRun;
}

// The value of "name=" in a record, or false.
inline bool Field(std::string_view text, std::string_view name, std::string* out) {
  std::size_t pos = 0;
  while (pos < text.size()) {
    std::size_t end = text.find('\n', pos);
    if (end == std::string_view::npos) {
      end = text.size();
    }
    std::string_view line = text.substr(pos, end - pos);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    if (line.size() > name.size() && line.substr(0, name.size()) == name && line[name.size()] == '=') {
      *out = std::string(line.substr(name.size() + 1));
      return true;
    }
    pos = end + 1;
  }
  return false;
}

inline bool Number(const std::string& text, double* out) {
  if (text.empty()) {
    return false;
  }
  char* end = nullptr;
  const double v = std::strtod(text.c_str(), &end);
  if (end == text.c_str() || *end != '\0' || !std::isfinite(v)) {
    return false;
  }
  *out = v;
  return true;
}

}  // namespace detail

// A record the file may hold: its key and its result. False for anything that is not a record of this format (a missing file reads as empty
// text), a field that is not a number, or an outcome that was never cached (kNotRun).
inline bool ParseRecord(std::string_view text, std::string* key, Result* result) {
  std::string magic;
  const std::size_t first = text.find('\n');
  magic = std::string(text.substr(0, first));
  if (!magic.empty() && magic.back() == '\r') {
    magic.pop_back();
  }
  if (magic != kRecordMagic) {
    return false;
  }
  std::string outcome, mrays, best, hit, runs;
  if (!detail::Field(text, "key", key) || !detail::Field(text, "outcome", &outcome) || !detail::Field(text, "mrays", &mrays) ||
      !detail::Field(text, "best_ms", &best) || !detail::Field(text, "hit", &hit) || !detail::Field(text, "runs", &runs)) {
    return false;
  }
  Result r;
  r.outcome = detail::OutcomeOf(outcome);
  double m = 0.0, b = 0.0, h = 0.0, n = 0.0;
  if (r.outcome == Outcome::kNotRun || !detail::Number(mrays, &m) || !detail::Number(best, &b) || !detail::Number(hit, &h) ||
      !detail::Number(runs, &n) || m < 0.0 || b < 0.0 || h < 0.0 || n < 0.0 || n > 1000.0) {
    return false;
  }
  r.mrays_per_s = m;
  r.best_ms = b;
  r.hit_share = h;
  r.runs = uint32_t(n);
  *result = r;
  return true;
}

// Whether a parsed record is the answer for these facts.
inline bool KeyMatches(const std::string& cached_key, const KeyFacts& facts) {
  return cached_key == KeyText(facts);
}

// What to do at start, from the setting (mcla_gpu_rt_bench: auto | off | force) and the cache: run the benchmark, use the cached result, or
// neither.
enum class Action : uint8_t { kSkip, kUseCache, kRun };

inline Action ActionFor(std::string_view setting, bool cache_valid) {
  if (setting == "off") return Action::kSkip;
  if (setting == "force") return Action::kRun;
  return cache_valid ? Action::kUseCache : Action::kRun;
}

}  // namespace mcla::gpu::rt_bench
