// The lamps' checks (CITY-RT M3 package G2b): no game files, no renderer, no archive.
//   - the game's laws (the term, the reach, the cone, the key) held to the volumetric light's own (gpu/volumetrics_lamps.h, header only),
//   - the on hours,
//   - the table: build, the grid, every query against brute force (borders, negatives, huge and empty questions, a coarsened grid), the strongest lamps, a cell's emitter,
//   - the section: round trip, every damage (a flipped byte, a cut, an extension, a rewritten cell table with a fixed hash, a lamp in the wrong cell),
//   - the reader of lights.xlm on synthetic resources in the game's layout (plausibility, indices, damage),
//   - the merge with a simulated live read (the game's packing: 16 bit position steps, half floats), the rule's every branch, order independence and idempotence,
//   - SelectLamps.
// The lamps inside a pack built from a synthetic archive are checked in citypack_test.cpp (it has the archive's helpers). The golden counts on the real archive are the tool's
// `lamps --golden` (tools/citypack_tool), read only.
//
//   build: see src/citypack/CMakeLists.txt (larecomp_lamp_table_test); run with no argument (exit 0 = passed).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <string>
#include <vector>

#include "citypack/bytes.h"
#include "citypack/lamp_merge.h"
#include "citypack/lamp_parse.h"
#include "citypack/lamp_table.h"
#include "gpu/volumetrics_lamps.h"

using namespace larecomp::citypack;

namespace {

int g_failures = 0;
int g_checks = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    ++g_checks;                                                              \
    if (!(cond)) {                                                           \
      ++g_failures;                                                          \
      std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    }                                                                        \
  } while (0)

bool Near(float a, float b, float tolerance = 1e-5f) { return std::fabs(a - b) <= tolerance; }

// ---------------------------------------------------------------------------
// a deterministic generator and some lamps
// ---------------------------------------------------------------------------

struct Rng {
  uint64_t s;
  explicit Rng(uint64_t seed) : s(seed) {}
  uint64_t Next() {
    s += 0x9E3779B97F4A7C15ull;
    uint64_t z = s;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
  float Unit() { return float(Next() >> 40) / float(1u << 24); }
  float Range(float lo, float hi) { return lo + (hi - lo) * Unit(); }
  uint32_t Below(uint32_t n) { return uint32_t(Next() % n); }
};

// The city's rectangle as the grid builder has it (x -3467 .. 2853, y -31 .. 79, z -2358 .. 2105.8): where the game's packing steps live.
constexpr float kBoxLo[3] = {-3467.0f, -31.0f, -2358.0f};
constexpr float kBoxHi[3] = {2853.0f, 79.0f, 2105.8f};

// n file lights inside the packing box, in file order (file_index = the position), a few on cell borders, the types and values of the real file's population.
std::vector<Lamp> RandomFileLamps(uint32_t n, uint64_t seed, bool borders = true) {
  Rng rng(seed);
  static const float kIntensity[] = {20, 20, 20, 10, 15, 12, 0, 40, 8, 30, 5, 25, 16, 60};
  std::vector<Lamp> out(n);
  for (uint32_t i = 0; i < n; ++i) {
    Lamp& l = out[i];
    l.pos[0] = rng.Range(-3460.0f, 2850.0f);
    l.pos[1] = rng.Range(-30.0f, 78.0f);
    l.pos[2] = rng.Range(-2350.0f, 2100.0f);
    if (borders && rng.Below(20) == 0) l.pos[0] = std::floor(l.pos[0] / 64.0f) * 64.0f;  // exactly on a cell border
    if (borders && rng.Below(20) == 0) l.pos[2] = std::floor(l.pos[2] / 64.0f) * 64.0f;
    for (float& c : l.colour) c = rng.Unit();
    l.intensity = kIntensity[rng.Below(uint32_t(std::size(kIntensity)))];
    if (rng.Below(15) == 0) {  // a spot
      l.spot = 1;
      static const float kCone[] = {40, 55, 65, 80, 100, 110, 120, 130, 144.7f};
      static const float kSpread[] = {0, 10, 20, 30, 40, 50, 60};
      l.cone_deg = kCone[rng.Below(uint32_t(std::size(kCone)))];
      l.spread_deg = kSpread[rng.Below(uint32_t(std::size(kSpread)))];
      l.dir[0] = 3e-6f;
      l.dir[1] = -1.0f;
      l.dir[2] = 2e-4f;
    } else {
      l.cone_deg = 360.0f;
      l.spread_deg = rng.Below(10) == 0 ? 1.0f : 0.0f;
    }
    l.on = 1;
    l.source = LampSource::kFile;
    l.file_index = i;
  }
  return out;
}

bool SameBits(const Lamp& a, const Lamp& b, bool with_flags = true) {
  Lamp x = a, y = b;
  if (!with_flags) x.flags = y.flags = 0;
  return std::memcmp(&x, &y, sizeof(Lamp)) == 0;  // Lamp has no padding that matters: every member is written by the same code on both sides
}

// ---------------------------------------------------------------------------
// the game's laws, against the volumetric light's
// ---------------------------------------------------------------------------

void TestLaws() {
  namespace vol = mcla::gpu::volumetrics;
  for (const float intensity : {0.0f, 0.5f, 5.0f, 20.0f, 55.0f, 70.0f, 100.0f, 470.0f}) {
    CHECK(Near(LampReach(intensity), vol::GameReach(intensity)));
    CHECK(Near(LampPlateau(intensity), intensity > 0 ? std::sqrt(intensity) : 0.0f));
    for (const float dist : {0.0f, 0.5f, 3.0f, 7.4f, 7.5f, 8.0f, 15.0f, 44.7f, 100.0f, 300.0f}) CHECK(LampTerm(intensity, dist) == vol::GameLampTerm(intensity, dist));
  }
  CHECK(LampTerm(55.0f, 7.4f) == 1.0f && LampTerm(55.0f, 8.0f) < 1.0f && Near(LampTerm(55.0f, 2.0f * std::sqrt(55.0f)), 0.25f));  // the plateau, then I / d^2
  CHECK(LampTerm(-1.0f, 3.0f) == 0.0f && LampTerm(0.0f, 0.0f) == 0.0f);
  CHECK(Near(LampReach(20.0f), 44.72136f, 1e-3f));  // the mod loader's "20 reaches 44.7 m"

  for (const float cone : {0.0f, 40.0f, 65.0f, 100.0f, 125.0f, 144.7f, 180.0f, 359.0f, 360.0f}) {
    for (const float spread : {-50.5f, -10.0f, -1.0f, 0.0f, 1.0f, 20.0f, 60.0f, 400.0f}) {
      float o1, i1, o2, i2;
      LampConeCosines(cone, spread, &o1, &i1);
      vol::ConeCosines(cone, spread, &o2, &i2);
      CHECK(o1 == o2 && i1 == i2);
    }
  }
  // the Los Angeles street lamp (cone 125, spread -50.5): full inside 37.25 degrees of the axis and gone at 62.5
  Lamp street;
  street.spot = 1;
  street.cone_deg = 125.0f;
  street.spread_deg = -50.5f;
  street.intensity = 55.0f;
  const auto at_angle = [&](float degrees) {
    const float a = degrees * 3.14159265f / 180.0f;
    const float to[3] = {std::sin(a), -std::cos(a), 0.0f};  // the axis is straight down
    return LampConeFactor(street, to);
  };
  CHECK(Near(at_angle(0.0f), 1.0f) && Near(at_angle(37.0f), 1.0f, 1e-4f) && at_angle(50.0f) < 1.0f && at_angle(50.0f) > 0.0f && Near(at_angle(63.0f), 0.0f, 1e-3f) && at_angle(120.0f) == 0.0f);
  CHECK(at_angle(40.0f) > at_angle(50.0f) && at_angle(50.0f) > at_angle(60.0f));  // monotone
  Lamp omni;
  const float any[3] = {0.0f, 1.0f, 0.0f};
  CHECK(LampConeFactor(omni, any) == 1.0f);
  Lamp wide = street;
  wide.cone_deg = 360.0f;
  CHECK(LampConeFactor(wide, any) == 1.0f);       // 360 degrees is no cone
  Lamp no_axis = street;
  no_axis.dir[0] = no_axis.dir[1] = no_axis.dir[2] = 0.0f;
  CHECK(LampConeFactor(no_axis, any) == 1.0f);

  // the key: the same number as the volumetric light's (and, by its own test, the lamp shadows')
  Rng rng(7);
  for (int i = 0; i < 200; ++i) {
    mcla::gpu::vol_lamps::GameLamp g;
    for (float& p : g.pos) p = rng.Range(-3500.0f, 2900.0f);
    Lamp l;
    std::memcpy(l.pos, g.pos, sizeof(l.pos));
    CHECK(LampKeyOf(l) == vol::LampKey(g));
  }
  Lamp a, b;
  a.pos[0] = 10.0f;
  b.pos[0] = 10.1f;  // the same eighth of a metre
  CHECK(LampKeyOf(a) == LampKeyOf(b));
  b.pos[0] = 10.2f;
  CHECK(LampKeyOf(a) != LampKeyOf(b));
}

// ---------------------------------------------------------------------------
// the on hours
// ---------------------------------------------------------------------------

void TestHours() {
  const LampHours h;  // on 18, off 6, margin 0.1
  CHECK(!StreetLampOn(h, 12.0f, 0.0f) && !StreetLampOn(h, 17.8f, 0.0f) && StreetLampOn(h, 17.95f, 0.0f) && StreetLampOn(h, 19.0f, 0.0f) && StreetLampOn(h, 0.0f, 0.0f));
  CHECK(StreetLampOn(h, 5.9f, 0.0f) && StreetLampOn(h, 6.05f, 0.0f) && !StreetLampOn(h, 6.2f, 0.0f) && !StreetLampOn(h, 8.0f, 0.0f));
  CHECK(StreetLampOn(h, 6.2f, 0.25f) && !StreetLampOn(h, 6.4f, 0.25f) && StreetLampOn(h, 17.7f, 0.25f) && !StreetLampOn(h, 17.6f, 0.25f));  // the jitter widens both edges
  LampHours late;
  late.on_hour = 19.5f;
  late.off_hour = 5.0f;
  CHECK(!StreetLampOn(late, 19.0f, 0.0f) && StreetLampOn(late, 19.45f, 0.0f) && StreetLampOn(late, 5.05f, 0.0f) && !StreetLampOn(late, 5.5f, 0.0f));

  std::array<int, 16> steps{};
  for (uint64_t key = 0; key < 4000; ++key) {
    const float j = LampJitterHours(key * 0x9E3779B97F4A7C15ull);
    CHECK(j >= 0.0f && j <= 15.0f * 16.0f * kLampJitterHoursPerStep + 1e-6f);
    CHECK(LampJitterHours(key) == LampJitterHours(key));
    ++steps[size_t(std::lround(j / (16.0f * kLampJitterHoursPerStep)))];
  }
  int used = 0;
  for (const int n : steps) used += n != 0 ? 1 : 0;
  CHECK(used == 16);  // all sixteen steps occur

  CHECK(EstimateNightFactor(h, 12.0f) == 0.0f && EstimateNightFactor(h, 20.0f) == 1.0f && EstimateNightFactor(h, 3.0f) == 1.0f && EstimateNightFactor(h, 18.0f) == 1.0f);
  CHECK(Near(EstimateNightFactor(h, 17.75f), 0.5f) && EstimateNightFactor(h, 17.0f) == 0.0f && Near(EstimateNightFactor(h, 6.25f), 0.5f) && EstimateNightFactor(h, 7.0f) == 0.0f);

  Lamp fill;
  Lamp street;
  street.source = LampSource::kStreetLamp;
  street.spot = 1;
  CHECK(LampActivity(fill, h, 22.0f) == 1.0f && LampActivity(fill, h, 12.0f) == 0.0f && Near(LampActivity(fill, h, 12.0f, 0.4f), 0.4f) && LampActivity(fill, h, 12.0f, 3.0f) == 1.0f);
  CHECK(LampActivity(street, h, 22.0f) == 1.0f && LampActivity(street, h, 12.0f) == 0.0f && LampActivity(street, h, 12.0f, 1.0f) == 0.0f);  // the window, not the night factor
  fill.on = 0;
  street.on = 0;
  CHECK(LampActivity(fill, h, 22.0f) == 0.0f && LampActivity(street, h, 22.0f) == 0.0f);
}

// ---------------------------------------------------------------------------
// the table
// ---------------------------------------------------------------------------

std::vector<uint32_t> BruteNear(const LampTable& t, float x, float z, float r) {
  std::vector<uint32_t> out;
  if (!(r >= 0.0f)) return out;
  for (uint32_t i = 0; i < t.size(); ++i) {
    const double dx = double(t[i].pos[0]) - double(x), dz = double(t[i].pos[2]) - double(z);
    if (dx * dx + dz * dz <= double(r) * double(r)) out.push_back(i);
  }
  return out;
}
std::vector<uint32_t> BruteSphere(const LampTable& t, const float p[3], float r) {
  std::vector<uint32_t> out;
  if (!(r >= 0.0f)) return out;
  for (uint32_t i = 0; i < t.size(); ++i) {
    double d2 = 0.0;
    for (int a = 0; a < 3; ++a) d2 += (double(t[i].pos[a]) - double(p[a])) * (double(t[i].pos[a]) - double(p[a]));
    if (d2 <= double(r) * double(r)) out.push_back(i);
  }
  return out;
}
std::vector<uint32_t> BruteBox(const LampTable& t, const float lo[3], const float hi[3]) {
  std::vector<uint32_t> out;
  for (uint32_t i = 0; i < t.size(); ++i) {
    bool in = true;
    for (int a = 0; a < 3; ++a) in = in && t[i].pos[a] >= lo[a] && t[i].pos[a] <= hi[a];
    if (in) out.push_back(i);
  }
  return out;
}

std::vector<LampPick> BruteStrongest(const LampTable& t, const float p[3], float radius, uint32_t n, const LampQuery& q) {
  std::vector<LampPick> all;
  for (const uint32_t i : BruteSphere(t, p, radius)) {
    const Lamp& l = t[i];
    float d[3] = {p[0] - l.pos[0], p[1] - l.pos[1], p[2] - l.pos[2]};
    const float dist = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    float to[3] = {0.0f, -1.0f, 0.0f};
    if (dist > 1e-4f) {
      for (int a = 0; a < 3; ++a) to[a] = d[a] / dist;
    }
    const float act = q.hour < 0.0f ? (l.on ? 1.0f : 0.0f) : LampActivity(l, q.hours != nullptr ? *q.hours : t.hours(), q.hour, q.night_factor);
    const float score = std::max({l.colour[0], l.colour[1], l.colour[2], 0.0f}) * LampTerm(l.intensity, dist) * LampConeFactor(l, to) * act;
    if (score > 0.0f) all.push_back({i, score, dist});
  }
  std::stable_sort(all.begin(), all.end(), [](const LampPick& a, const LampPick& b) { return a.score > b.score || (a.score == b.score && a.index < b.index); });
  if (all.size() > n) all.resize(n);
  return all;
}

void TestTable() {
  Rng rng(11);
  std::vector<Lamp> lamps = RandomFileLamps(3000, 5);
  // some street lamps and live fills among them, so that the sources and the file indices are mixed
  for (uint32_t i = 0; i < 300; ++i) {
    Lamp l = lamps[i];
    l.pos[0] += 3.0f;
    l.source = i % 2 ? LampSource::kStreetLamp : LampSource::kLiveFill;
    l.file_index = kNoLampIndex;
    lamps.push_back(l);
  }
  // lights that must not enter: not a number, infinite, far beyond a city
  Lamp bad;
  bad.pos[0] = std::nanf("");
  lamps.push_back(bad);
  bad.pos[0] = 0.0f;
  bad.pos[2] = INFINITY;
  lamps.push_back(bad);
  bad.pos[2] = 0.0f;
  bad.pos[1] = 5.0e6f;
  lamps.push_back(bad);

  const LampTable t = LampTable::Build(lamps);
  CHECK(t.size() == 3300 && t.dropped() == 3 && !t.empty());
  CHECK(t.grid().cell_m == kDefaultLampCellMetres && std::fmod(t.grid().origin_x, 64.0f) == 0.0f && std::fmod(t.grid().origin_z, 64.0f) == 0.0f);
  CHECK(t.grid().nx * t.grid().nz <= kMaxLampCells && t.grid().nx >= 90 && t.grid().nz >= 60);
  // every lamp is under the cell it is in; the cells add up to the table
  uint32_t total = 0;
  for (uint32_t cz = 0; cz < t.grid().nz; ++cz) {
    for (uint32_t cx = 0; cx < t.grid().nx; ++cx) {
      uint32_t first = 0, last = 0;
      t.CellRange(cx, cz, &first, &last);
      CHECK(first <= last && last <= t.size());
      total += last - first;
      for (uint32_t i = first; i < last; ++i) {
        uint32_t ax = 0, az = 0;
        CHECK(t.CellOf(t[i].pos[0], t[i].pos[2], &ax, &az) && ax == cx && az == cz);
      }
    }
  }
  CHECK(total == t.size());
  uint32_t cx = 0, cz = 0;
  CHECK(!t.CellOf(1.0e5f, 0.0f, &cx, &cz) && !t.CellOf(0.0f, -1.0e5f, &cx, &cz) && !t.CellOf(std::nanf(""), 0.0f, &cx, &cz) && t.CellOf(0.0f, 0.0f, &cx, &cz));
  uint32_t f = 0, l = 0;
  t.CellRange(t.grid().nx, 0, &f, &l);
  CHECK(f == 0 && l == 0);  // a cell outside the grid is empty

  // the same lights in another order are the same table, byte for byte
  {
    std::vector<Lamp> shuffled = lamps;
    for (size_t i = shuffled.size() - 1; i > 0; --i) std::swap(shuffled[i], shuffled[rng.Below(uint32_t(i + 1))]);
    CHECK(LampTable::Build(shuffled).Encode() == t.Encode());
  }
  // file indices
  CHECK(t.FindByFileIndex(0) != kNoLampIndex && t[t.FindByFileIndex(1234)].file_index == 1234 && t.FindByFileIndex(3000) == kNoLampIndex && t.FindByFileIndex(kNoLampIndex) == kNoLampIndex);

  // the queries, against brute force
  std::vector<uint32_t> got;
  for (int k = 0; k < 300; ++k) {
    const float x = rng.Range(-4000.0f, 3300.0f), z = rng.Range(-2800.0f, 2500.0f), y = rng.Range(-40.0f, 90.0f);
    const float r = k % 7 == 0 ? 0.0f : rng.Range(0.0f, k % 3 == 0 ? 900.0f : 140.0f);
    t.Near(x, z, r, &got);
    CHECK(got == BruteNear(t, x, z, r));
    CHECK(std::is_sorted(got.begin(), got.end()));
    const float p[3] = {x, y, z};
    t.NearSphere(p, r, &got);
    CHECK(got == BruteSphere(t, p, r));
    const float lo[3] = {x - r, y - 20.0f, z - 0.5f * r}, hi[3] = {x + 0.7f * r, y + 20.0f, z + r};
    t.InBox(lo, hi, &got);
    CHECK(got == BruteBox(t, lo, hi));
  }
  // lamps exactly on cell borders and exactly r away
  for (uint32_t i = 0; i < 40; ++i) {
    const Lamp& lamp = t[rng.Below(t.size())];
    t.Near(lamp.pos[0], lamp.pos[2], 0.0f, &got);
    CHECK(!got.empty());  // the lamp itself, at distance 0 (inclusive)
    CHECK(got == BruteNear(t, lamp.pos[0], lamp.pos[2], 0.0f));
    t.Near(lamp.pos[0] + 64.0f, lamp.pos[2], 64.0f, &got);  // the lamp is exactly r away (inclusive)
    CHECK(got == BruteNear(t, lamp.pos[0] + 64.0f, lamp.pos[2], 64.0f));
  }
  // odd questions
  t.Near(0.0f, 0.0f, 1.0e30f, &got);
  CHECK(got.size() == t.size());
  t.Near(0.0f, 0.0f, -1.0f, &got);
  CHECK(got.empty());
  t.Near(0.0f, 0.0f, std::nanf(""), &got);
  CHECK(got.empty());
  t.Near(std::nanf(""), 0.0f, 10.0f, &got);
  CHECK(got.empty());
  t.Near(1.0e8f, 1.0e8f, 100.0f, &got);
  CHECK(got.empty());
  const float pn[3] = {std::nanf(""), 0.0f, 0.0f};
  t.NearSphere(pn, 10.0f, &got);
  CHECK(got.empty());
  const float inverted_lo[3] = {10, 10, 10}, inverted_hi[3] = {0, 0, 0};
  t.InBox(inverted_lo, inverted_hi, &got);
  CHECK(got.empty());
  const float all_lo[3] = {-1e30f, -1e30f, -1e30f}, all_hi[3] = {1e30f, 1e30f, 1e30f};
  t.InBox(all_lo, all_hi, &got);
  CHECK(got.size() == t.size());

  // the strongest lamps
  for (int k = 0; k < 120; ++k) {
    const float p[3] = {rng.Range(-3400.0f, 2800.0f), rng.Range(-20.0f, 70.0f), rng.Range(-2300.0f, 2000.0f)};
    LampQuery q;
    if (k % 3 == 1) q.hour = 22.0f;
    if (k % 3 == 2) {
      q.hour = 12.0f;
      q.night_factor = 0.6f;
    }
    const uint32_t n = k % 5 == 0 ? 1 : (k % 5 == 1 ? 8 : 40);
    const float radius = rng.Range(20.0f, 300.0f);
    const auto have = t.Strongest(p, radius, n, q);
    const auto want = BruteStrongest(t, p, radius, n, q);
    CHECK(have.size() == want.size());
    for (size_t i = 0; i < have.size() && i < want.size(); ++i) CHECK(have[i].index == want[i].index && have[i].score == want[i].score && Near(have[i].distance, want[i].distance));
    for (size_t i = 1; i < have.size(); ++i) CHECK(have[i - 1].score >= have[i].score);
  }
  const float p0[3] = {0, 0, 0};
  CHECK(t.Strongest(p0, 300.0f, 0).empty());
  {
    // by day the fill lights give nothing (night factor 0) and the street lamps are off: no lamp is picked; at night the street lamps count by their cone
    LampQuery day;
    day.hour = 12.0f;
    CHECK(t.Strongest(p0, 5000.0f, 10, day).empty());
  }

  // a cell's emitter
  {
    uint32_t any = t.size() / 2;
    uint32_t ex = 0, ez = 0;
    CHECK(t.CellOf(t[any].pos[0], t[any].pos[2], &ex, &ez));
    const CellEmitter e = t.EmitterOfCell(ex, ez);
    uint32_t first = 0, last = 0;
    t.CellRange(ex, ez, &first, &last);
    CHECK(e.count == last - first && e.count >= 1);
    double radiance[3] = {0, 0, 0}, centre[3] = {0, 0, 0};
    float reach = 0.0f;
    for (uint32_t i = first; i < last; ++i) {
      for (int a = 0; a < 3; ++a) {
        radiance[a] += t[i].colour[a] * t[i].intensity;
        centre[a] += t[i].pos[a];
      }
      reach = std::max(reach, LampReach(t[i].intensity));
    }
    for (int a = 0; a < 3; ++a) CHECK(Near(e.radiance[a], float(radiance[a]), 1e-3f) && Near(e.centre[a], float(centre[a] / double(e.count)), 1e-3f));
    CHECK(e.reach_m == reach);
    CHECK(t.EmitterOfCell(t.grid().nx, 0).count == 0 && t.EmitterOfCell(0, t.grid().nz).count == 0);
  }

  const LampStats st = t.Stats();
  CHECK(st.count == t.size() && st.by_source[0] == 3000 && st.by_source[1] == 150 && st.by_source[2] == 150 && st.spots + st.omni == st.count);
  CHECK(st.bounds_min[0] == t.bounds_min()[0] && st.intensity_max == 60.0f);
}

void TestTableEdges() {
  // empty
  const LampTable empty = LampTable::Build({});
  std::vector<uint32_t> got = {1, 2, 3};
  empty.Near(0, 0, 100, &got);
  CHECK(empty.empty() && got.empty() && empty.size() == 0 && empty.Stats().count == 0);
  const float p[3] = {0, 0, 0};
  CHECK(empty.Strongest(p, 100.0f, 4).empty() && empty.EmitterOfCell(0, 0).count == 0 && empty.FindByFileIndex(0) == kNoLampIndex);
  LampTable back;
  std::string error;
  const std::vector<uint8_t> empty_bytes = empty.Encode();
  CHECK(LampTable::Decode(empty_bytes.data(), empty_bytes.size(), &back, &error) && back.empty() && back.Encode() == empty_bytes);
  const LampTable none;  // default constructed
  none.Near(0, 0, 1, &got);
  CHECK(got.empty());
  const std::vector<uint8_t> none_bytes = none.Encode();
  CHECK(LampTable::Decode(none_bytes.data(), none_bytes.size(), &back, &error));

  // one lamp
  Lamp one;
  one.pos[0] = -70.5f;
  one.pos[1] = 3.0f;
  one.pos[2] = 64.0f;
  one.intensity = 20.0f;
  const LampTable t1 = LampTable::Build({one});
  t1.Near(-70.5f, 64.0f, 0.0f, &got);
  CHECK(t1.size() == 1 && got == std::vector<uint32_t>{0});
  t1.Near(-70.5f, 64.01f, 0.0f, &got);
  CHECK(got.empty());
  CHECK(t1.grid().origin_x == -128.0f && t1.grid().origin_z == 64.0f && t1.grid().nx == 1 && t1.grid().nz == 1);

  // a city far bigger than the cell budget: the cell doubles until the grid fits, the queries stay right
  std::vector<Lamp> wide;
  Rng rng(3);
  for (int i = 0; i < 500; ++i) {
    Lamp l;
    l.pos[0] = rng.Range(-4.0e5f, 4.0e5f);
    l.pos[2] = rng.Range(-4.0e5f, 4.0e5f);
    wide.push_back(l);
  }
  const LampTable tw = LampTable::Build(wide);
  CHECK(tw.grid().nx * tw.grid().nz <= kMaxLampCells && tw.grid().cell_m > kDefaultLampCellMetres);
  for (int k = 0; k < 40; ++k) {
    const float x = rng.Range(-4.5e5f, 4.5e5f), z = rng.Range(-4.5e5f, 4.5e5f), r = rng.Range(0.0f, 1.0e5f);
    tw.Near(x, z, r, &got);
    CHECK(got == BruteNear(tw, x, z, r));
  }
  LampTable round;
  const std::vector<uint8_t> wide_bytes = tw.Encode();
  CHECK(LampTable::Decode(wide_bytes.data(), wide_bytes.size(), &round, &error) && round.Encode() == wide_bytes);

  // a cell smaller than a metre is a metre
  CHECK(LampTable::Build({one}, 0.01f).grid().cell_m == 1.0f);
}

// ---------------------------------------------------------------------------
// the section
// ---------------------------------------------------------------------------

void Rehash(std::vector<uint8_t>* bytes) {
  const uint64_t h = Fnv1a64(bytes->data() + kLampHeaderBytes, bytes->size() - kLampHeaderBytes);
  for (int i = 0; i < 8; ++i) (*bytes)[80 + size_t(i)] = uint8_t(h >> (8 * i));
}

void TestSection() {
  std::vector<Lamp> lamps = RandomFileLamps(1500, 21);
  for (uint32_t i = 0; i < 60; ++i) {
    Lamp l = lamps[i];
    l.source = i % 3 == 0 ? LampSource::kStreetLamp : LampSource::kLiveFill;
    l.flags = kLampSeenLive;
    l.file_index = kNoLampIndex;
    l.on = i % 5 == 0 ? 0 : 1;
    lamps.push_back(l);
  }
  const LampTable t = LampTable::Build(lamps, 64.0f, kLampTableFromMods | kLampTableHasLive);
  const std::vector<uint8_t> bytes = t.Encode();
  CHECK(bytes.size() == ((kLampHeaderBytes + (size_t(t.grid().cells()) + 1) * 4 + 15) & ~size_t(15)) + size_t(t.size()) * kLampRecordBytes);
  CHECK(bytes.size() % 16 == (size_t(t.size()) * kLampRecordBytes) % 16);

  LampTable back;
  std::string error;
  CHECK(LampTable::Decode(bytes.data(), bytes.size(), &back, &error));
  CHECK(back.size() == t.size() && back.flags() == t.flags() && back.grid().nx == t.grid().nx && back.grid().nz == t.grid().nz && back.grid().origin_x == t.grid().origin_x &&
        back.grid().cell_m == t.grid().cell_m);
  bool same = true;
  for (uint32_t i = 0; i < t.size(); ++i) same = same && SameBits(t[i], back[i]);
  CHECK(same);
  CHECK(back.Encode() == bytes && back.ContentHash() == t.ContentHash());
  for (int a = 0; a < 3; ++a) CHECK(back.bounds_min()[a] == t.bounds_min()[a] && back.bounds_max()[a] == t.bounds_max()[a]);
  CHECK(back.FindByFileIndex(700) == t.FindByFileIndex(700));
  std::vector<uint32_t> a, b;
  t.Near(0, 0, 400, &a);
  back.Near(0, 0, 400, &b);
  CHECK(a == b);

  // the lamp hours travel with the section: the defaults, or what a reader set
  {
    CHECK(t.hours().on_hour == 18.0f && t.hours().off_hour == 6.0f && t.hours().margin_hours == 0.1f && back.hours().on_hour == 18.0f && back.hours().off_hour == 6.0f &&
          back.hours().margin_hours == 0.1f);
    LampTable custom = LampTable::Build(lamps);
    custom.set_hours(LampHours{19.5f, 5.0f, 0.2f});
    const std::vector<uint8_t> custom_bytes = custom.Encode();
    LampTable read_back;
    CHECK(LampTable::Decode(custom_bytes.data(), custom_bytes.size(), &read_back, &error) && read_back.hours().on_hour == 19.5f && read_back.hours().off_hour == 5.0f &&
          read_back.hours().margin_hours == 0.2f && read_back.Encode() == custom_bytes);
    // hours that are not hours are refused (they are in the header, outside the hash)
    std::vector<uint8_t> bad_hours = custom_bytes;
    bad_hours[76] = 0x00;
    bad_hours[77] = 0x00;
    bad_hours[78] = 0xC0;
    bad_hours[79] = 0x7F;  // a NaN on hour
    LampTable unchanged = LampTable::Build({lamps[0]});
    CHECK(!LampTable::Decode(bad_hours.data(), bad_hours.size(), &unchanged, &error) && error.find("hours") != std::string::npos);
    bad_hours = custom_bytes;
    bad_hours[88] = 0x00;
    bad_hours[89] = 0x00;
    bad_hours[90] = 0xF0;
    bad_hours[91] = 0x41;  // an off hour of 30
    CHECK(!LampTable::Decode(bad_hours.data(), bad_hours.size(), &unchanged, &error) && error.find("hours") != std::string::npos);
    // the strongest lamps follow the table's hours unless the query brings its own
    Lamp street;
    street.pos[1] = 8.0f;
    street.colour[0] = street.colour[1] = street.colour[2] = 1.0f;
    street.intensity = 55.0f;
    street.spot = 1;
    street.cone_deg = 125.0f;
    street.spread_deg = -50.5f;
    street.source = LampSource::kStreetLamp;
    LampTable one = LampTable::Build({street});
    const float below[3] = {0.0f, 0.0f, 0.0f};
    LampQuery evening;
    evening.hour = 19.0f;
    CHECK(one.Strongest(below, 50.0f, 1, evening).size() == 1);  // 18 / 6: lit at 19
    one.set_hours(LampHours{19.5f, 5.0f, 0.1f});
    CHECK(one.Strongest(below, 50.0f, 1, evening).empty());      // 19.5 / 5: not yet
    const LampHours early;
    evening.hours = &early;
    CHECK(one.Strongest(below, 50.0f, 1, evening).size() == 1);  // the query's own hours win
  }

  // every damage is refused with a reason, and leaves `out` alone
  LampTable keep = LampTable::Build({lamps[0]});
  const uint64_t keep_hash = keep.ContentHash();
  const auto refused = [&](const std::vector<uint8_t>& damaged, const char* expect_word) {
    std::string why;
    const bool ok = LampTable::Decode(damaged.data(), damaged.size(), &keep, &why);
    const bool good = !ok && keep.ContentHash() == keep_hash && why.find("lamp section") != std::string::npos && (expect_word == nullptr || why.find(expect_word) != std::string::npos);
    if (!good) std::fprintf(stderr, "  (decode said %s: \"%s\")\n", ok ? "ok" : "no", why.c_str());
    return good;
  };
  {
    std::vector<uint8_t> d = bytes;
    d[bytes.size() / 2] ^= 0x01;
    CHECK(refused(d, "hash"));  // a flipped byte in a record
    d = bytes;
    d[kLampHeaderBytes + 6] ^= 0x10;
    CHECK(refused(d, "hash"));  // ... in the cell table
    d = bytes;
    d.pop_back();
    CHECK(refused(d, "size"));  // cut by a byte
    d = bytes;
    d.push_back(0);
    CHECK(refused(d, "size"));  // one too long
    d.assign(bytes.begin(), bytes.begin() + 50);
    CHECK(refused(d, "header"));
    d = bytes;
    d[0] = 'X';
    CHECK(refused(d, "magic"));
    d = bytes;
    d[4] = 2;
    CHECK(refused(d, "version"));
    d = bytes;
    d[12] = 60;
    CHECK(refused(d, "size"));  // another record size
    d = bytes;
    d[11] = 0x7F;
    CHECK(refused(d, "more lamps"));  // a count no table has
    d = bytes;
    d[16] = d[17] = d[18] = 0x7F;
    CHECK(refused(d, "cells"));
    // damage with the hash made right again: only the checks behind the hash can see it
    const uint32_t cells = t.grid().cells();
    d = bytes;
    d[kLampHeaderBytes + size_t(cells) * 4] += 1;  // the table's end is not the lamp count
    Rehash(&d);
    CHECK(refused(d, "add up"));
    d = bytes;
    for (int i = 0; i < 4; ++i) d[kLampHeaderBytes + 4 + size_t(i)] = uint8_t(t.size() >> (8 * i));  // the second cell starts at the last lamp: the third is behind it
    Rehash(&d);
    CHECK(refused(d, "backwards"));
    d = bytes;
    const uint32_t records_at = (kLampHeaderBytes + uint32_t((size_t(t.grid().cells()) + 1) * 4) + 15u) & ~15u;
    d[records_at + 0] = 0x00;
    d[records_at + 1] = 0x24;
    d[records_at + 2] = 0xF4;
    d[records_at + 3] = 0x48;  // the first lamp's x is 500,000: a number, and outside its cell
    Rehash(&d);
    CHECK(refused(d, "cell"));
    d = bytes;
    for (size_t i = 0; i < kLampRecordBytes; ++i) std::swap(d[records_at + i], d[records_at + size_t(t.size() - 1) * kLampRecordBytes + i]);  // the first and the last lamp trade places
    Rehash(&d);
    CHECK(refused(d, "cell"));
    d = bytes;
    d[records_at + 4 * 12] = 7;  // a type byte that is not 0 or 1
    Rehash(&d);
    CHECK(refused(d, "type"));
    d = bytes;
    d[records_at + 4] = 0;
    d[records_at + 5] = 0;
    d[records_at + 6] = 0xC0;
    d[records_at + 7] = 0x7F;  // the first lamp's y is a NaN
    Rehash(&d);
    CHECK(refused(d, "number"));
  }
  CHECK(!LampTable::Decode(nullptr, 0, &keep, &error));
}

// ---------------------------------------------------------------------------
// lights.xlm
// ---------------------------------------------------------------------------

struct RawLight {
  float pos[3];
  float colour[3];
  float dir[3];
  float intensity, cone, spread;
  uint8_t type, on;
};

void PutBE32(std::vector<uint8_t>& d, size_t at, uint32_t v) {
  d[at] = uint8_t(v >> 24);
  d[at + 1] = uint8_t(v >> 16);
  d[at + 2] = uint8_t(v >> 8);
  d[at + 3] = uint8_t(v);
}
void PutBEF(std::vector<uint8_t>& d, size_t at, float v) {
  uint32_t b;
  std::memcpy(&b, &v, 4);
  PutBE32(d, at, b);
}

// A lights.xlm in the game's layout: the list at root +0x50 {pointer, count, capacity}, the lights of 96 bytes from 0x80, the 0xCDCDCDCD and NaN fill of the real file.
RscImage MakeLightsImage(const std::vector<RawLight>& lights, uint32_t count_override = 0xFFFFFFFFu, uint32_t pointer_override = 0) {
  RscImage image;
  image.data.assign(0x80 + lights.size() * 96 + 0x200, 0);
  image.virtual_size = uint32_t(image.data.size());
  PutBE32(image.data, 0x50, pointer_override != 0 ? pointer_override : 0x50000080u);
  const uint32_t count = count_override != 0xFFFFFFFFu ? count_override : uint32_t(lights.size());
  image.data[0x54] = uint8_t(count >> 8);
  image.data[0x55] = uint8_t(count);
  image.data[0x56] = uint8_t(count >> 8);
  image.data[0x57] = uint8_t(count);
  for (size_t i = 0; i < lights.size(); ++i) {
    const size_t o = 0x80 + i * 96;
    for (int a = 0; a < 3; ++a) {
      PutBEF(image.data, o + 4 * size_t(a), lights[i].pos[a]);
      PutBEF(image.data, o + 16 + 4 * size_t(a), lights[i].colour[a]);
      PutBEF(image.data, o + 32 + 4 * size_t(a), lights[i].dir[a]);
    }
    PutBE32(image.data, o + 12, 0x7FC00001u);
    PutBEF(image.data, o + 64, lights[i].intensity);
    PutBEF(image.data, o + 68, lights[i].cone);
    PutBEF(image.data, o + 72, lights[i].spread);
    PutBEF(image.data, o + 76, 0.0f);
    PutBEF(image.data, o + 80, 1.0f);
    PutBE32(image.data, o + 84, 0xCDCDCDCDu);
    image.data[o + 88] = lights[i].type;
    image.data[o + 89] = lights[i].on;
  }
  return image;
}

void TestParse() {
  std::vector<RawLight> raw(6);
  raw[0] = {{1.5f, 2.5f, 3.5f}, {0.25f, 0.5f, 0.75f}, {0, -1, 0}, 20.0f, 360.0f, 0.0f, 0, 1};
  raw[1] = {{-10.0f, 4.0f, 8.0f}, {1, 1, 1}, {3e-6f, -1, 2e-4f}, 70.0f, 125.0f, -50.5f, 1, 1};
  raw[2] = {{std::nanf(""), 0, 0}, {1, 1, 1}, {0, -1, 0}, 20.0f, 360.0f, 0.0f, 0, 1};    // not a place
  raw[3] = {{5, 5, 5}, {1, 1, 1}, {0, -1, 0}, -4.0f, 360.0f, 0.0f, 0, 1};                 // a negative intensity
  raw[4] = {{6, 6, 6}, {1, 1, 200.0f}, {0, -1, 0}, 20.0f, 360.0f, 0.0f, 0, 1};            // a colour of 200
  raw[5] = {{7, 7, 7}, {0, 0, 0}, {0, -1, 0}, 0.0f, 90.0f, 30.0f, 1, 0};                  // fine: a spot, switched off, intensity 0
  const RscImage image = MakeLightsImage(raw);
  std::vector<Lamp> out;
  LampParseStats st;
  std::string error;
  CHECK(ParseLightsResource(image, &out, &st, &error));
  CHECK(st.listed == 6 && st.kept == 3 && st.skipped == 3 && st.spots == 2 && st.omni == 1 && st.off == 1 && out.size() == 3);
  CHECK(out[0].file_index == 0 && out[1].file_index == 1 && out[2].file_index == 5);  // the file's own index survives the lights that were left out
  CHECK(out[0].pos[0] == 1.5f && out[0].pos[1] == 2.5f && out[0].pos[2] == 3.5f && out[0].colour[2] == 0.75f && out[0].intensity == 20.0f && out[0].cone_deg == 360.0f && !out[0].is_spot() &&
        out[0].on == 1 && out[0].source == LampSource::kFile);
  CHECK(out[1].is_spot() && out[1].cone_deg == 125.0f && out[1].spread_deg == -50.5f && out[1].dir[1] == -1.0f && out[1].dir[2] == 2e-4f && out[1].intensity == 70.0f);
  CHECK(out[2].on == 0 && out[2].is_spot() && out[2].intensity == 0.0f);

  // damage: no list, an absurd count, a pointer outside, a list that runs past the segment, a cut image
  CHECK(!ParseLightsResource(MakeLightsImage(raw, 0), &out, &st, &error) && out.empty() && !error.empty());
  CHECK(!ParseLightsResource(MakeLightsImage(raw, 60000), &out, &st, &error));
  CHECK(!ParseLightsResource(MakeLightsImage(raw, 0xFFFFFFFFu, 0x50F00000u), &out, &st, &error));
  CHECK(!ParseLightsResource(MakeLightsImage(raw, 0xFFFFFFFFu, 0x40000080u), &out, &st, &error));
  CHECK(!ParseLightsResource(MakeLightsImage(raw, 5000), &out, &st, &error));  // 5,000 lights of 96 bytes do not fit the 0x300 bytes
  RscImage cut = MakeLightsImage(raw);
  cut.data.resize(0x40);
  CHECK(!ParseLightsResource(cut, &out, &st, &error));
  std::vector<RawLight> junk(3, RawLight{{std::nanf(""), 0, 0}, {1, 1, 1}, {0, -1, 0}, 20.0f, 360.0f, 0.0f, 0, 1});
  CHECK(!ParseLightsResource(MakeLightsImage(junk), &out, &st, &error) && st.skipped == 3);  // nothing plausible in it: no table
  CHECK(!ParseLightsResource(RscImage{}, &out, &st, &error));

  // the census of a file of the real file's population
  std::vector<Lamp> lamps = RandomFileLamps(2000, 77, false);
  LampParseStats parse;
  parse.listed = parse.kept = 2000;
  for (const Lamp& l : lamps) (l.spot ? parse.spots : parse.omni) += 1;
  const LampCensus c = CensusOfLamps(lamps, parse);
  uint32_t buckets = 0;
  for (const uint32_t n : c.by_intensity) buckets += n;
  CHECK(buckets == 2000 && c.most_common_intensity == 20.0f && c.most_common_intensity_count > 400 && c.table.count == 2000);
  const std::string report = LampCensusReport(c);
  CHECK(report.find("lights listed: 2000") != std::string::npos && report.find("DIFFERENT") != std::string::npos);
  CHECK(LampCensusReport(LampCensus{}).find("not in the archive") != std::string::npos);
}

// ---------------------------------------------------------------------------
// the merge, on a simulated live read
// ---------------------------------------------------------------------------

// What the game hands over for a file light after packing and unpacking (the +8 list): the library's model of it.
LiveLight Packed(const Lamp& l, bool street = false) { return SimulateGamePacking(l, street); }

void TestPackingModel() {
  CHECK(RoundThroughHalf(0.0f) == 0.0f && RoundThroughHalf(1.0f) == 1.0f && RoundThroughHalf(-50.5f) == -50.5f && RoundThroughHalf(125.0f) == 125.0f && RoundThroughHalf(470.0f) == 470.0f);
  CHECK(RoundThroughHalf(0.1f) != 0.1f && std::fabs(RoundThroughHalf(0.1f) - 0.1f) < 0.1f * 0.0005f);  // a half float's 11 bits: 0.05 per cent
  CHECK(std::isnan(RoundThroughHalf(std::nanf(""))) && std::isinf(RoundThroughHalf(INFINITY)));
  Lamp l;
  l.pos[0] = 1234.567f;
  l.pos[1] = 12.345f;
  l.pos[2] = -777.777f;
  const LiveLight v = SimulateGamePacking(l);
  CHECK(std::fabs(v.pos[0] - l.pos[0]) <= 0.5f * (kBoxHi[0] - kBoxLo[0]) / 65535.0f + 1e-3f && std::fabs(v.pos[2] - l.pos[2]) <= 0.5f * (kBoxHi[2] - kBoxLo[2]) / 65535.0f + 1e-3f);
  CHECK(std::fabs(v.pos[1] - l.pos[1]) <= 0.5f * (kBoxHi[1] - kBoxLo[1]) / 65535.0f + 1e-3f);
  l.pos[0] = 9999.0f;  // outside the box: clamped to its edge
  CHECK(SimulateGamePacking(l).pos[0] == kBoxHi[0]);
  CHECK(SimulateGamePacking(l, true).street && !SimulateGamePacking(l).street);
  const PackingBox wide = {{-4000.0f, -50.0f, -3000.0f}, {4000.0f, 400.0f, 3000.0f}};
  l.pos[0] = 0.0f;
  CHECK(std::fabs(SimulateGamePacking(l, false, wide).pos[0] - 0.0f) <= 0.5f * 8000.0f / 65535.0f + 1e-3f);
}

std::vector<LiveLight> StreetLamps(uint32_t n, uint64_t seed) {
  Rng rng(seed);
  std::vector<LiveLight> out;
  for (uint32_t i = 0; i < n; ++i) {
    Lamp l;
    l.pos[0] = rng.Range(-3400.0f, 2800.0f);
    l.pos[1] = rng.Range(-20.0f, 60.0f);
    l.pos[2] = rng.Range(-2300.0f, 2000.0f);
    l.colour[0] = 1.0f;
    l.colour[1] = 0.9f;
    l.colour[2] = 0.7f;
    l.intensity = rng.Below(2) ? 55.0f : 70.0f;
    l.spot = 1;
    l.cone_deg = 125.0f;
    l.spread_deg = -50.5f;
    l.dir[0] = -0.07f;
    l.dir[1] = -0.99f;
    l.dir[2] = 0.13f;
    out.push_back(Packed(l, true));
  }
  return out;
}

void TestMerge() {
  const uint32_t n = 2000;
  std::vector<Lamp> file_lamps = RandomFileLamps(n, 99, false);
  for (uint32_t i = 0; i < 20; ++i) {  // a few on cell borders, inside the packing's box
    file_lamps[20 + i].pos[0] = 64.0f * float(int(i) - 10);
    file_lamps[40 + i].pos[2] = 64.0f * float(int(i) - 10);
  }
  // two lights at one pole (same place, other colour), and two that are the same in every number
  file_lamps[11].pos[0] = file_lamps[10].pos[0];
  file_lamps[11].pos[1] = file_lamps[10].pos[1];
  file_lamps[11].pos[2] = file_lamps[10].pos[2];
  file_lamps[12] = file_lamps[13];
  file_lamps[12].file_index = 12;
  const LampTable file = LampTable::Build(file_lamps);

  std::vector<LiveLight> fills;
  for (const Lamp& l : file_lamps) fills.push_back(Packed(l));
  const std::vector<LiveLight> street = StreetLamps(800, 5);
  std::vector<LiveLight> live = fills;
  live.insert(live.end(), street.begin(), street.end());

  MergeReport rep;
  const LampTable merged = MergeLive(file, live, MergeOptions{}, &rep);
  CHECK(rep.file_lights == n && rep.live_fill == n && rep.live_street == 800 && rep.matched == n && rep.matched_by_ordinal == n && rep.live_only_fill == 0 && rep.file_only == 0 &&
        rep.street_added == 800 && rep.merged == n + 800 && merged.size() == n + 800);
  CHECK(rep.worst_match_metres < 0.12 && rep.worst_match_metres > 0.0 && rep.mean_match_metres < rep.worst_match_metres && rep.worst_match_metres < MergeOptions{}.match_metres);
  CHECK(merged.flags() == kLampTableHasLive && merged.grid().cell_m == file.grid().cell_m);
  CHECK(rep.Summary().find("2000 in the file") != std::string::npos && rep.Summary().find("800 street lamps added") != std::string::npos);
  // the file's exact records survive (marked as seen), the street lamps are the game's, nothing else was added
  uint32_t seen = 0, street_n = 0, other = 0;
  for (uint32_t i = 0; i < merged.size(); ++i) {
    const Lamp& l = merged[i];
    if (l.source == LampSource::kFile) {
      CHECK((l.flags & kLampSeenLive) != 0 && l.file_index < n);
      CHECK(SameBits(l, [&] { Lamp x = file_lamps[l.file_index]; x.flags = kLampSeenLive; return x; }()));
      ++seen;
    } else if (l.source == LampSource::kStreetLamp) {
      CHECK(l.is_spot() && l.cone_deg == 125.0f && l.spread_deg == -50.5f && l.file_index == kNoLampIndex);
      ++street_n;
    } else {
      ++other;
    }
  }
  CHECK(seen == n && street_n == 800 && other == 0);
  CHECK(merged.FindByFileIndex(1234) != kNoLampIndex && merged[merged.FindByFileIndex(1234)].file_index == 1234);

  // the same table whatever order the live lights come in
  {
    std::vector<LiveLight> shuffled = live;
    Rng rng(31);
    for (size_t i = shuffled.size() - 1; i > 0; --i) std::swap(shuffled[i], shuffled[rng.Below(uint32_t(i + 1))]);
    MergeReport r2;
    const LampTable m2 = MergeLive(file, shuffled, MergeOptions{}, &r2);
    CHECK(m2.Encode() == merged.Encode());
    CHECK(r2.matched == n && r2.matched_by_ordinal < n);  // the ordinal hint misses, the nearest candidate finds each
  }
  // merging what a merge hands over changes nothing
  {
    const LampTable again = MergeLive(file, LiveViewOf(merged));
    CHECK(again.Encode() == merged.Encode());
  }
  // no live lights: the file's table
  {
    MergeReport r0;
    const LampTable m0 = MergeLive(file, {}, MergeOptions{}, &r0);
    CHECK(m0.Encode() == file.Encode() && m0.flags() == 0 && r0.file_only == n && r0.matched == 0 && r0.merged == n);
  }
  // an incomplete read: 100 file lights were not seen, 50 live fills the file does not have
  {
    std::vector<LiveLight> part;
    for (size_t i = 0; i < fills.size(); ++i) {
      if (i >= 500 && i < 600) continue;
      part.push_back(fills[i]);
    }
    Rng rng(8);
    for (int i = 0; i < 50; ++i) {
      Lamp l = file_lamps[rng.Below(n)];
      l.pos[0] = rng.Range(-3000.0f, 2000.0f);  // a place the file has no light at
      l.pos[2] = rng.Range(-2000.0f, 1900.0f);
      l.intensity = 33.0f;
      part.push_back(Packed(l));
    }
    MergeReport r;
    const LampTable m = MergeLive(file, part, MergeOptions{}, &r);
    CHECK(r.matched == n - 100 && r.file_only == 100 && r.live_only_fill == 50 && r.street_added == 0 && m.size() == n + 50);
    uint32_t unseen = 0, extra = 0;
    for (const Lamp& l : m.lamps()) {
      if (l.source == LampSource::kFile && (l.flags & kLampSeenLive) == 0) ++unseen;
      if (l.source == LampSource::kLiveFill) {
        ++extra;
        CHECK(l.intensity == 33.0f && l.file_index == kNoLampIndex && (l.flags & kLampSeenLive) != 0);
      }
    }
    CHECK(unseen == 100 && extra == 50);
    const LampTable again = MergeLive(file, LiveViewOf(m));
    CHECK(again.Encode() == m.Encode());
  }
  // the tolerances: 0.25 m (and the packing's error) matches, 0.45 m does not; a spot is not an omni light; 2 per cent of intensity is another light, 0.5 per cent is not
  {
    Lamp one;
    one.pos[0] = 100.0f;
    one.pos[1] = 5.0f;
    one.pos[2] = 200.0f;
    one.colour[0] = 0.5f;
    one.colour[1] = 0.25f;
    one.colour[2] = 0.125f;
    one.intensity = 40.0f;
    one.file_index = 0;
    const LampTable t1 = LampTable::Build({one});
    const auto merged_with = [&](const LiveLight& v) {
      MergeReport r;
      MergeLive(t1, {v}, MergeOptions{}, &r);
      return r;
    };
    LiveLight base = Packed(one);
    CHECK(merged_with(base).matched == 1);
    LiveLight v = base;
    v.pos[0] += 0.25f;
    CHECK(merged_with(v).matched == 1);
    v.pos[0] = base.pos[0] + 0.45f;
    CHECK(merged_with(v).matched == 0 && merged_with(v).live_only_fill == 1 && merged_with(v).file_only == 1);
    v = base;
    v.pos[1] += 0.40f;
    CHECK(merged_with(v).matched == 0);
    v = base;
    v.spot = 1;
    CHECK(merged_with(v).matched == 0);
    v = base;
    v.intensity = 40.0f * 1.02f;
    CHECK(merged_with(v).matched == 0);
    v.intensity = 40.0f * 1.005f;
    CHECK(merged_with(v).matched == 1);
    v = base;
    v.colour[1] += 0.02f;
    CHECK(merged_with(v).matched == 0);
    v = base;
    v.cone_deg = 359.0f;
    CHECK(merged_with(v).matched == 0);
    v = base;
    v.spread_deg = 3.0f;
    CHECK(merged_with(v).matched == 0);
    // a light of intensity 0 matches an intensity 0
    Lamp zero = one;
    zero.intensity = 0.0f;
    MergeReport rz;
    MergeLive(LampTable::Build({zero}), {Packed(zero)}, MergeOptions{}, &rz);
    CHECK(rz.matched == 1);
    // two live lights for one file light: one matches, the other is the game's own
    MergeReport r2;
    const LampTable m2 = MergeLive(t1, {base, base}, MergeOptions{}, &r2);
    CHECK(r2.matched == 1 && r2.live_only_fill == 1 && m2.size() == 2);
  }
  // a street lamp that coincides with a file spot is still a street lamp: the game lights it twice, so does the table
  {
    Lamp spot;
    spot.pos[0] = 50.0f;
    spot.pos[2] = 60.0f;
    spot.spot = 1;
    spot.cone_deg = 125.0f;
    spot.spread_deg = -50.5f;
    spot.intensity = 55.0f;
    spot.file_index = 0;
    MergeReport r;
    const LampTable m = MergeLive(LampTable::Build({spot}), {Packed(spot, false), Packed(spot, true)}, MergeOptions{}, &r);
    CHECK(r.matched == 1 && r.street_added == 1 && m.size() == 2 && m.Stats().by_source[0] == 1 && m.Stats().by_source[2] == 1);
  }
  // a merge into a table with an empty file: the live lights are all there is
  {
    MergeReport r;
    const LampTable m = MergeLive(LampTable::Build({}), {Packed(file_lamps[0]), street[0]}, MergeOptions{}, &r);
    CHECK(m.size() == 2 && r.live_only_fill == 1 && r.street_added == 1 && m.flags() == kLampTableHasLive);
  }
}

// ---------------------------------------------------------------------------
// what a consumer takes from the table
// ---------------------------------------------------------------------------

void TestSelect() {
  std::vector<Lamp> lamps;
  const auto add = [&](float x, LampSource source) {
    Lamp l;
    l.pos[0] = x;
    l.pos[1] = 8.0f;
    l.pos[2] = 0.0f;
    l.intensity = 55.0f;
    l.source = source;
    if (source == LampSource::kStreetLamp) l.spot = 1;
    lamps.push_back(l);
  };
  for (const float x : {0.0f, 100.0f, 250.0f, 350.0f, 450.0f, 700.0f}) add(x, LampSource::kStreetLamp);
  add(10.0f, LampSource::kFile);
  add(400.0f, LampSource::kFile);
  const LampTable t = LampTable::Build(lamps);
  LampSelection s;
  s.eye[1] = 1.0f;
  s.radius_m = 500.0f;
  std::vector<uint32_t> out;
  const auto xs = [&] {
    std::vector<float> x;
    for (const uint32_t i : out) x.push_back(t[i].pos[0]);
    std::sort(x.begin(), x.end());
    return x;
  };
  SelectLamps(t, s, &out);
  CHECK((xs() == std::vector<float>{0, 10, 100, 250, 350, 400, 450}));  // the list is stale: every lamp within 500 m of the eye (700 is out)
  s.live_list_fresh = true;
  SelectLamps(t, s, &out);
  CHECK((xs() == std::vector<float>{10, 350, 400, 450}));  // the game's list has the street lamps within 300 m; the fill lights and the far street lamps stay
  CHECK(std::is_sorted(out.begin(), out.end()));
  s.live_radius_m = 400.0f;
  SelectLamps(t, s, &out);
  CHECK((xs() == std::vector<float>{10, 400, 450}));
  s.hour = 12.0f;  // by day: the fill lights have no night factor, the street lamps are off
  SelectLamps(t, s, &out);
  CHECK(out.empty());
  s.hour = 23.0f;
  SelectLamps(t, s, &out);
  CHECK((xs() == std::vector<float>{10, 400, 450}));
  s.hour = 12.0f;
  s.night_factor = 0.5f;  // a frame's own night factor lights the fill lights (the street lamps follow the window)
  SelectLamps(t, s, &out);
  CHECK((xs() == std::vector<float>{10, 400}));
  // the table's hours decide unless the selection has its own
  LampTable late = t;
  late.set_hours(LampHours{23.5f, 5.0f, 0.1f});
  s.hour = 23.0f;
  s.night_factor = -1.0f;
  SelectLamps(late, s, &out);
  CHECK(out.empty());  // 23.5 / 5: it is not night yet for any of them
  const LampHours evening_hours;
  s.hours = &evening_hours;
  SelectLamps(late, s, &out);
  CHECK((xs() == std::vector<float>{10, 400, 450}));
}

}  // namespace

int main() {
  TestLaws();
  TestHours();
  TestTable();
  TestTableEdges();
  TestSection();
  TestParse();
  TestPackingModel();
  TestMerge();
  TestSelect();
  if (g_failures != 0) {
    std::fprintf(stderr, "lamp_table_test: %d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
  }
  std::printf("lamp_table_test: %d checks passed\n", g_checks);
  return 0;
}
