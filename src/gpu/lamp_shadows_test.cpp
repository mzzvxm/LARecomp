// Check of the street lamps' shadows, second try (lamp_shadows_rules.h, lamp_shadow_frame.h, lamp_shadow_lookup.h, hlsl/lamp_shadow_lookup.hlsli; perf/LAMP-SHADOWS-2.md, package
// LAMP-SHADOWS-4). A program of its own: it needs no renderer, no ReXGlue and no GPU, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_lamp_shadows_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_lamp_shadows_test [<src/gpu folder>]   (exit 0 = passed; the folder is found next to this file without an argument)
// or by hand: clang++ -std=c++23 -I src/gpu -I src src/gpu/lamp_shadows_test.cpp -o lamp_shadows_test
//
//   SOURCES     FromGameLamp / FromTableLamp (a mock of citypack::Lamp) make the same lamp, ids are light_id.h's LampId and the old KeyOf, Usable's refusals, the reach (light_laws.h).
//   GRID        the game's half float: known values (1500.7 -> 1500, -2961.4 -> -2960), truncation toward zero against an independent bit model, at most 2 m off over the city, idempotent.
//   LEVELS      the budgets are shadow_atlas_rules.h's lamp rows, the pools are what SpecFor asks for the lamps, the memory (high 10 / 20 MiB, ultra 20 / 40 MiB), override rules.
//   PICK        the nearest N by what a lamp adds to the view, hysteresis (two equal lamps do not trade), fades in scene seconds (the same at 30 and 144 FPS), tiles that stay, spares, cuts.
//   CASTERS     movers against the static world (the prop families' skinned shaders are static), the draw identity.
//   CACHE       DecideStatic's rules (first build, new lamp, fresh atlas, epoch, full set, refresh), PlanLampPass (cone cull, layers, only the new ids, the caps, cache off), CommitStatic.
//   VIEW        FitLampView holds a street lamp's cone from the lamp, the matrices map a point under the lamp into the lamp's tile.
//   FRAME       BuildFrame (static + live entries, single entry with the cache off, capacity), the pass's light (both modes), the GPU table.
//   LOOKUP      LampLit on a fake atlas (lit, shadowed, edge fade, weight, layers multiplied, the entries below the lamps' base); the HLSL twin compiled as C++ gives the same numbers bit for bit.

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "lamp_shadow_frame.h"
#include "lamp_shadow_lookup.h"
#include "lamp_shadows_rules.h"
#include "light_convert.h"
#include "light_frame.h"
#include "light_id.h"
#include "light_laws.h"
#include "local_lights_rules.h"

// ---------------------------------------------------------------------------
// the .hlsli as C++: a shim of what the files use, in a namespace of its own so that the standard library's floor, min ... are not in the way
// ---------------------------------------------------------------------------
namespace hlsl_shim {
typedef unsigned int uint;
struct float4 {
  float x = 0, y = 0, z = 0, w = 0;
  float4() = default;
  float4(float a, float b, float c, float d) : x(a), y(b), z(c), w(d) {}
};
// float3 and what the plane lookup (HL-SHADOW-QUALITY: light_shadow_lookup.hlsli LslPlaneLit) uses of it; the same subset shadow_atlas_test.cpp's shim gives the headlights' reader
struct float3 {
  float x = 0, y = 0, z = 0;
  float3() = default;
  float3(float a, float b, float c) : x(a), y(b), z(c) {}
};
inline float3 operator-(const float3& a, const float3& b) { return float3(a.x - b.x, a.y - b.y, a.z - b.z); }
inline float3 operator*(const float3& a, float k) { return float3(a.x * k, a.y * k, a.z * k); }
inline float dot(const float3& a, const float3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float3 cross(const float3& a, const float3& b) { return float3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
inline float length(const float3& a) { return std::sqrt(dot(a, a)); }
inline float floor(float v) { return std::floor(v); }
inline float abs(float v) { return std::fabs(v); }
inline uint min(uint a, uint b) { return a < b ? a : b; }
inline int max(int a, int b) { return a > b ? a : b; }
// HLSL's min and max of two floats: the reference's std::min / std::max read the same two operands (a NaN is not a case the tests make)
inline float min(float a, float b) { return b < a ? b : a; }
inline float max(float a, float b) { return a < b ? b : a; }
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }
inline float saturate(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
inline float sqrt(float v) { return std::sqrt(v); }
inline float asfloat(uint v) { return std::bit_cast<float>(v); }
#define LSL_MAX_VIEWS 28
#include "hlsl/lamp_shadow_lookup.hlsli"
}  // namespace hlsl_shim

namespace lg = larecomp::gpu;
namespace sl = larecomp::gpu::shadow_lookup;
namespace ls = mcla::gpu::local_shadow;
namespace lb = mcla::gpu::light_budget;
namespace sa = mcla::gpu::shadow_atlas;
namespace tp = mcla::gpu::temporal;
namespace la = mcla::gpu::lamp_shadows;
namespace vl = mcla::gpu::vol_lamps;
namespace ll = larecomp::gpu::light_laws;
namespace lp = larecomp::gpu::local_lights;
namespace gc = mcla::gpu::gpu_class;

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
void CheckNear(double got, double want, double tol, const std::string& what) {
  ++g_checks;
  if (!(std::fabs(got - want) <= tol)) {
    ++g_failed;
    std::printf("FAILED: %s: got %.9g, want %.9g (tolerance %.3g)\n", what.c_str(), got, want, tol);
  }
}

struct Lcg {
  uint32_t state = 20261121u;
  float Next() {  // 0 to 1
    state = state * 1664525u + 1013904223u;
    return float(state >> 8) / float(1u << 24);
  }
  uint32_t Int(uint32_t n) { return uint32_t(Next() * float(n)) % std::max(n, 1u); }
  float Range(float a, float b) { return a + (b - a) * Next(); }
};

// a street lamp of Los Angeles: I 55 to 100, cone 125, spread -50.5, pointing down, 9 m up
vl::GameLamp StreetLamp(float x, float z, float intensity = 70.0f) {
  vl::GameLamp g;
  g.pos[0] = x;
  g.pos[1] = 9.0f;
  g.pos[2] = z;
  g.colour[0] = 1.0f;
  g.colour[1] = 0.82f;
  g.colour[2] = 0.55f;
  g.dir[0] = 0.0f;
  g.dir[1] = -1.0f;
  g.dir[2] = 0.0f;
  g.intensity = intensity;
  g.cone_deg = 125.0f;
  g.spread_deg = -50.5f;
  g.scale = 1.0f;
  g.type = 1;
  g.on = 1;
  return g;
}

la::Lamp Lamp(float x, float z, float intensity = 70.0f) { return la::FromGameLamp(StreetLamp(x, z, intensity)); }

// ---------------------------------------------------------------------------
// the sources
// ---------------------------------------------------------------------------

// citypack::Lamp's fields, as a mock
struct MockTableLamp {
  float pos[3] = {0, 0, 0};
  float colour[3] = {1, 1, 1};
  float dir[3] = {0.0f, -1.0f, 0.0f};
  float intensity = 0.0f;
  float cone_deg = 360.0f;
  float spread_deg = 0.0f;
  uint8_t spot = 0;
  uint8_t on = 1;
};

uint64_t OldKeyOf(const float pos[3]) {  // lamp_shadow_block.h KeyOf / volumetrics_lamps.h LampKey / citypack LampKeyOf
  uint64_t h = 0x9E3779B97F4A7C15ull;
  for (int i = 0; i < 3; ++i) {
    const int64_t q = int64_t(std::floor(double(pos[i]) * 8.0));
    h ^= uint64_t(q) + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
  }
  return h;
}

void TestSources() {
  const vl::GameLamp g = StreetLamp(885.32f, -756.95f, 70.0f);
  const la::Lamp a = la::FromGameLamp(g);
  Check(a.origin == la::Origin::kLiveList && a.intensity == 70.0f && a.cone_deg == 125.0f && a.spread_deg == -50.5f && a.type == 1 && a.on == 1 && a.scale == 1.0f, "the list's lamp");
  Check(a.id == OldKeyOf(g.pos) && a.id == lg::light_id::LampId(g.pos), "a lamp's id is the old key, bit for bit");
  MockTableLamp t;
  for (int i = 0; i < 3; ++i) {
    t.pos[i] = g.pos[i];
    t.colour[i] = g.colour[i];
  }
  t.intensity = 70.0f;
  t.cone_deg = 125.0f;
  t.spread_deg = -50.5f;
  t.spot = 1;
  const la::Lamp b = la::FromTableLamp(t, 0.6f);
  Check(b.origin == la::Origin::kTable && b.id == a.id && b.scale == 0.6f && b.type == 1 && b.intensity == 70.0f, "the table's lamp is the same lamp at the place (and the strength given)");
  Check(la::Usable(a) && la::Usable(b), "a street lamp is usable");
  // refusals
  la::Lamp x = a;
  x.type = 0;
  Check(!la::Usable(x), "an omni light has no axis: no view");
  x = a;
  x.on = 0;
  Check(!la::Usable(x), "a lamp that is off");
  x = a;
  x.intensity = 0.0f;
  Check(!la::Usable(x), "no intensity");
  x = a;
  x.scale = 0.0f;
  Check(!la::Usable(x), "faded out by the game");
  x = a;
  x.cone_deg = 360.0f;
  Check(!la::Usable(x), "a cone of 360 is a point");
  x = a;
  x.dir[0] = x.dir[1] = x.dir[2] = 0.0f;
  Check(!la::Usable(x), "no axis");
  x = a;
  x.pos[1] = std::nanf("");
  Check(!la::Usable(x), "a number a light cannot have");
  // the reach
  CheckNear(la::ShadowReach(55.0f), std::sqrt(8.0 * 55.0), 1e-5, "I 55 reaches sqrt(8 I) = 21 m");
  CheckNear(la::ShadowReach(100.0f), std::sqrt(8.0 * 100.0), 1e-5, "I 100 reaches 28 m");
  CheckNear(la::ShadowReach(1.0f), 14.0, 1e-9, "a weak lamp is kept at 14 m");
  CheckNear(la::ShadowReach(10000.0f), 30.0, 1e-9, "a strong one at 30 m");
  CheckNear(la::ShadowReach(0.0f), 14.0, 1e-9, "no intensity: the least");
  for (float i = 0.0f; i < 300.0f; i += 7.3f) {
    Check(la::ShadowReach(i) == double(ll::LampShadowReach(i)), "the reach is light_laws.h's");
  }
}

// ---------------------------------------------------------------------------
// the game's half float
// ---------------------------------------------------------------------------

// an independent model: the float's value cut to 10 mantissa bits toward zero (and the half's range)
float TruncateTo10Bits(float v) {
  if (!(v == v) || std::fabs(v) < 6.103515625e-5f) {
    return 0.0f;
  }
  const float a = std::fabs(v);
  const int e = int(std::floor(std::log2(a)));
  const double step = std::ldexp(1.0, e - 10);
  const double cut = std::floor(double(a) / step) * step;
  return float(v < 0.0f ? -cut : cut);
}

void TestGrid() {
  CheckNear(la::GamePosition(1500.7f), 1500.0, 0, "1500.7 is stored as 1500");
  CheckNear(la::GamePosition(-2961.4f), -2960.0, 0, "-2961.4 is stored as -2960 (toward zero)");
  CheckNear(la::GamePosition(0.0f), 0.0, 0, "zero");
  CheckNear(la::GamePosition(1.0e-6f), 0.0, 0, "below the smallest normal half: zero");
  CheckNear(la::GamePosition(70000.0f), 65504.0, 0, "clamped");
  CheckNear(la::GamePosition(885.32f), 885.0, 0, "885.32 -> 885 (the lamp the night probe logged)");
  CheckNear(la::GamePosition(-756.95f), -756.5, 0, "-756.95 -> -756.5");
  Lcg rng;
  for (int n = 0; n < 20000; ++n) {
    const float v = rng.Range(-3500.0f, 3500.0f);
    const float g = la::GamePosition(v);
    CheckNear(g, TruncateTo10Bits(v), 0, "the game's half float is the value cut to 10 mantissa bits");
    Check(std::fabs(g) <= std::fabs(v) + 1e-6f && std::fabs(g - v) <= 2.0f + 1e-6f, "never farther from zero, at most 2 m off over the city");
    Check(la::GamePosition(g) == g, "idempotent");
    Check((g < 0.0f) == (v < 0.0f) || g == 0.0f, "the sign is kept");
  }
  const float pos[3] = {885.32f, 25.68f, -756.95f};
  float grid[3];
  la::GridPosition(pos, grid);
  Check(grid[0] == 885.0f && grid[1] == 25.68f && grid[2] == -756.5f, "the lamp as the grid has it: x and z through the half float, y as it is");
  Check(la::HalfBitsToFloat(la::GameHalfBits(0.5f)) == 0.5f && la::HalfBitsToFloat(la::GameHalfBits(-3.0f)) == -3.0f, "exact numbers survive");
}

// ---------------------------------------------------------------------------
// the levels
// ---------------------------------------------------------------------------

void TestLevels() {
  const lb::Level levels[] = {lb::Level::kMedium, lb::Level::kHigh, lb::Level::kUltra};
  const uint32_t wanted[] = {2, 4, 8};
  const uint32_t spare[] = {1, 1, 2};
  for (int i = 0; i < 3; ++i) {
    const la::Budget b = la::BudgetFor(levels[i], 0, true);
    Check(b.on() && b.wanted == wanted[i] && b.spare == spare[i] && b.tile == 512 && b.cache, "the level's row: 2 (+1) / 4 (+1) / 8 (+2) lamps of 512");
    const sa::LampRow* row = sa::LampRowOf(levels[i]);
    Check(row != nullptr && b.wanted == row->views && b.spare == row->spare && b.tile == row->tile, "the row is shadow_atlas_rules.h's");
    Check(b.tiles() <= la::kMaxSlots, "the tiles fit the frame's slots");
    // the pools are what SpecFor asks for the lamps
    sa::AtlasSpec mine;
    mine.Add({sa::ProviderId::kReplay, sa::PoolRole::kSingle, lb::BudgetFor(true, levels[i], 0, lb::Level::kOff, ls::kLevelTable).shadow_tile,
              lb::BudgetFor(true, levels[i], 0, lb::Level::kOff, ls::kLevelTable).shadow_views, false});
    la::RequestPools(b, &mine);
    const sa::AtlasSpec want = sa::SpecForLevel(levels[i], 0, true, true);
    Check(mine == want, "the provider's pools are SpecForLevel(level, lamps, static)'s");
    const sa::Layout layout = sa::MakeLayout(mine);
    Check(layout.valid() && sa::ValidateLayout(layout).empty(), "the layout is sound");
    // The atlas texture grows by whole ROWS of its shelf layout (shadow_atlas_rules.h MakeLayout: a pool is a block of `cols` x `rows` tiles in the width the widest pool makes): the headlights'
    // tiles are 768 / 1024 / 1280 now (HL-SHADOW-QUALITY), so the lamps' 512 tiles fill 3 / 5 of 6 / 7 of 7.5 columns: the growth is the pools' tiles or more, never less, and at most the tiles
    // plus a row's slack for each pool (ultra: 10 tiles in rows of 7 = 14 slots a pool).
    const uint64_t lamp_bytes = sa::Bytes(sa::MakeLayout(want)) - sa::Bytes(sa::LayoutForLevel(levels[i], 0));
    Check(la::PoolBytes(b) <= lamp_bytes && lamp_bytes <= la::PoolBytes(b) * 3 / 2, "the atlas grew by the pools' tiles plus at most the shelf's row slack (1.5x)");
    // the live pool and the static pool have the same number of slots, so a lamp's tile k has a twin
    const int live = layout.FindPool(sa::ProviderId::kLamps, sa::PoolRole::kLive);
    const int stat = layout.FindPool(sa::ProviderId::kLamps, sa::PoolRole::kStatic);
    Check(live >= 0 && stat >= 0 && layout.pools[uint32_t(live)].request.slots == b.tiles() && layout.pools[uint32_t(stat)].request.slots == b.tiles() &&
              layout.pools[uint32_t(live)].request.sticky && layout.pools[uint32_t(stat)].request.sticky,
          "a live pool and a static pool of the same size, both sticky");
  }
  CheckNear(sa::Mib(la::PoolBytes(la::BudgetFor(lb::Level::kHigh, 0, true))), 20.0, 1e-9, "high holds 20 MiB for the lamps with the static layers (10 without)");
  CheckNear(sa::Mib(la::PoolBytes(la::BudgetFor(lb::Level::kHigh, 0, false))), 10.0, 1e-9, "high without the static layers: 10 MiB");
  CheckNear(sa::Mib(la::PoolBytes(la::BudgetFor(lb::Level::kUltra, 0, true))), 40.0, 1e-9, "ultra: 40 MiB with them");
  CheckNear(sa::Mib(la::PoolBytes(la::BudgetFor(lb::Level::kMedium, 0, true))), 12.0, 1e-9, "medium: 12 MiB");
  Check(!la::BudgetFor(lb::Level::kOff, 0, true).on(), "off: no lamps");
  Check(!la::BudgetFor(lb::Level::kHigh, -1, true).on(), "the debug override -1: no lamp views");
  const la::Budget o3 = la::BudgetFor(lb::Level::kHigh, 3, true);
  Check(o3.wanted == 3 && o3.spare == 1, "an override of 3 lamps keeps a spare");
  const la::Budget o1 = la::BudgetFor(lb::Level::kHigh, 1, true);
  Check(o1.wanted == 1 && o1.spare == 0, "one lamp has no spare");
  const la::Budget o99 = la::BudgetFor(lb::Level::kMedium, 99, true);
  Check(o99.tiles() <= la::kMaxSlots && o99.wanted >= 1, "a huge override is cut to the frame's slots");
  sa::AtlasSpec none;
  la::RequestPools(la::Budget{}, &none);
  Check(none.count == 0, "no budget, no pools");
  sa::AtlasSpec nocache;
  la::RequestPools(la::BudgetFor(lb::Level::kHigh, 0, false), &nocache);
  Check(nocache.count == 1 && nocache.pools[0].role == sa::PoolRole::kLive, "the cache off asks for the live pool alone");
  Check(la::Describe(la::BudgetFor(lb::Level::kHigh, 0, true)).find("4 lamps (+1 spare)") != std::string::npos, "the log's text");
}

// ---------------------------------------------------------------------------
// the pick
// ---------------------------------------------------------------------------

la::View SimpleView(double x, double z) {
  la::View v;
  v.have_eye = true;
  v.eye[0] = x;
  v.eye[1] = 1.5;
  v.eye[2] = z;
  return v;  // no frustum: every lamp counts fully
}

// A view-projection looking along +z from (x, 1.5, z): a plain symmetric perspective of 70 degrees, row vectors (the game's), world -> clip.
tp::Mat4 LookAlongZ(double x, double z) {
  tp::Mat4 m = tp::Identity();
  const double f = 1.0 / std::tan(35.0 * 3.14159265358979 / 180.0);
  const double n = 0.1, fa = 600.0;
  // translate then project: clip.x = f * (px - x), clip.y = f * (py - 1.5), clip.z = (fa/(fa-n)) * (pz - z) - fa*n/(fa-n), clip.w = pz - z
  tp::Mat4 t = tp::Identity();
  t.m[3][0] = -x;
  t.m[3][1] = -1.5;
  t.m[3][2] = -z;
  tp::Mat4 p{};
  p.m[0][0] = f;
  p.m[1][1] = f;
  p.m[2][2] = fa / (fa - n);
  p.m[3][2] = -fa * n / (fa - n);
  p.m[2][3] = 1.0;
  m = tp::Mul(t, p);
  return m;
}

std::vector<la::Lamp> Street(int count, float spacing, float x = 0.0f, float z0 = 10.0f) {
  std::vector<la::Lamp> out;
  for (int i = 0; i < count; ++i) {
    out.push_back(Lamp(x, z0 + spacing * float(i)));
  }
  return out;
}

void TestPick() {
  const la::Budget high = la::BudgetFor(lb::Level::kHigh, 0, true);
  la::Picker picker;
  la::PickResult r;
  la::PickParams params;
  params.budget = high;
  params.dt = 0.0f;
  const std::vector<la::Lamp> street = Street(12, 18.0f);
  picker.Update(street, SimpleView(0.0, 0.0), params, &r);
  Check(r.count == 4 && r.wanted == 4 && r.fading == 0, "high picks 4 lamps");
  std::set<uint64_t> got;
  for (uint32_t i = 0; i < r.count; ++i) {
    got.insert(r.entries[i].id);
    Check(r.entries[i].weight == 1.0f && r.entries[i].fresh && r.entries[i].wanted, "the first pick is at full strength and fresh");
  }
  std::set<uint64_t> want;
  for (int i = 0; i < 4; ++i) {
    want.insert(street[size_t(i)].id);
  }
  Check(got == want, "the four nearest lamps");
  Check(r.usable == 5 && r.listed == 12, "the census counts the list and the lamps within 90 m that light the view");
  std::set<uint32_t> tiles;
  for (uint32_t i = 0; i < r.count; ++i) {
    tiles.insert(r.entries[i].tile);
  }
  Check(tiles.size() == 4 && *tiles.rbegin() < high.tiles(), "four different tiles inside the budget");
  // the camera steps on: tiles stay with their lamps, the newcomer takes a free tile, the one that left fades out in its tile
  std::map<uint64_t, uint32_t> tile_of;
  for (uint32_t i = 0; i < r.count; ++i) {
    tile_of[r.entries[i].id] = r.entries[i].tile;
  }
  params.dt = 1.0f / 60.0f;
  picker.Update(street, SimpleView(0.0, 60.0), params, &r);  // the lamp at z 10 is out of the four nearest, the one at z 82 is in
  for (uint32_t i = 0; i < r.count; ++i) {
    const auto f = tile_of.find(r.entries[i].id);
    if (f != tile_of.end()) {
      Check(r.entries[i].tile == f->second, "a lamp keeps its tile while it is tracked");
    }
  }
  Check(r.wanted == 4 && r.fading == 1 && r.count == 5 && r.count <= high.tiles(), "a lamp that left the pick fades out in the spare tile");
  bool fading_weight_ok = false;
  for (uint32_t i = 0; i < r.count; ++i) {
    if (!r.entries[i].wanted) {
      fading_weight_ok = r.entries[i].weight > 0.0f && r.entries[i].weight < 1.0f;
    }
  }
  Check(fading_weight_ok, "its weight steps down, it does not drop");
  // a newcomer fades in
  bool in_ok = false;
  for (uint32_t i = 0; i < r.count; ++i) {
    if (r.entries[i].fresh) {
      in_ok = r.entries[i].weight > 0.0f && r.entries[i].weight < 1.0f && r.entries[i].wanted;
    }
  }
  Check(in_ok, "a lamp that takes a tile fades in");
}

// the same fade in the same seconds whatever the frame rate
float SecondsToFull(float fps) {
  la::Picker picker;
  la::PickResult r;
  la::PickParams params;
  params.budget = la::BudgetFor(lb::Level::kMedium, 0, true);
  const std::vector<la::Lamp> two = {Lamp(0, 10), Lamp(0, 28)};
  const std::vector<la::Lamp> one = {Lamp(0, 10)};
  params.dt = 0.0f;
  picker.Update(one, SimpleView(0, 0), params, &r);
  params.dt = 1.0f / fps;
  float t = 0.0f;
  float weight_of_new = 0.0f;
  for (int i = 0; i < 2000 && weight_of_new < 1.0f; ++i) {
    picker.Update(two, SimpleView(0, 0), params, &r);
    t += params.dt;
    for (uint32_t k = 0; k < r.count; ++k) {
      if (r.entries[k].id == two[1].id) {
        weight_of_new = r.entries[k].weight;
      }
    }
  }
  return t;
}

void TestPickClock() {
  const float t30 = SecondsToFull(30.0f), t60 = SecondsToFull(60.0f), t144 = SecondsToFull(144.0f);
  CheckNear(t30, 0.35, 0.05, "a shadow fades in in 0.35 s at 30 FPS");
  CheckNear(t60, 0.35, 0.03, "... at 60 FPS");
  CheckNear(t144, 0.35, 0.02, "... at 144 FPS");
  // a stall is not a jump: one step of a second moves a lamp by a quarter second's worth
  la::Picker picker;
  la::PickResult r;
  la::PickParams params;
  params.budget = la::BudgetFor(lb::Level::kMedium, 0, true);
  params.dt = 0.0f;
  picker.Update({Lamp(0, 10)}, SimpleView(0, 0), params, &r);
  params.dt = 1.0f;
  picker.Update({Lamp(0, 10), Lamp(0, 28)}, SimpleView(0, 0), params, &r);
  float w = 0.0f;
  for (uint32_t k = 0; k < r.count; ++k) {
    if (r.entries[k].fresh) {
      w = r.entries[k].weight;
    }
  }
  CheckNear(w, std::min(1.0, 0.25 / double(la::kFadeSeconds)), 1e-5, "a stall of a second fades by a quarter second's worth");
  // a cut starts everyone at full weight
  params.reset = true;
  params.dt = 1.0f / 60.0f;
  picker.Update({Lamp(0, 500), Lamp(0, 510)}, SimpleView(0, 500), params, &r);
  bool all_full = r.count >= 2;
  for (uint32_t k = 0; k < r.count; ++k) {
    all_full = all_full && (!r.entries[k].wanted || r.entries[k].weight == 1.0f);
  }
  Check(all_full, "a cut (the camera jumped) starts the picked lamps at full strength");
}

void TestPickHysteresis() {
  // two lamps equally good for the one tile: the one that holds it keeps it (the keep bonus) until the rival is 1.3 times better
  la::Picker picker;
  la::PickResult r;
  la::PickParams params;
  params.budget = la::BudgetFor(lb::Level::kMedium, 1, true);  // one lamp
  params.dt = 1.0f / 60.0f;
  const la::Lamp a = Lamp(-10.0f, 20.0f), b = Lamp(10.0f, 20.0f);
  const std::vector<la::Lamp> list = {a, b};
  picker.Update(list, SimpleView(-1.0, 0.0), params, &r);  // a is nearer
  const uint64_t holder = r.entries[0].id;
  Check(holder == a.id, "the nearer lamp holds the tile");
  int swaps = 0;
  uint64_t last = holder;
  for (int i = 0; i < 400; ++i) {
    const double x = std::sin(double(i) * 0.05) * 1.2;  // the eye wobbles between the two a little: the scores cross
    picker.Update(list, SimpleView(x, 0.0), params, &r);
    uint64_t now = 0;
    for (uint32_t k = 0; k < r.count; ++k) {
      if (r.entries[k].wanted) {
        now = r.entries[k].id;
      }
    }
    if (now != last) {
      ++swaps;
      last = now;
    }
  }
  Check(swaps == 0, "two lamps of the same worth do not trade the tile back and forth");
  // a lamp that is much better does take it (the lamp that held the one tile fades out first: there is no spare tile for a lamp of one)
  for (int i = 0; i < 40; ++i) {
    picker.Update({a, b, Lamp(-1.0f, 6.0f)}, SimpleView(-1.0, 0.0), params, &r);
  }
  bool near_wins = false;
  for (uint32_t k = 0; k < r.count; ++k) {
    near_wins = near_wins || (r.entries[k].wanted && r.entries[k].lamp.pos[2] == 6.0f);
  }
  Check(near_wins, "a lamp that is much nearer takes the tile");
}

void TestPickFilters() {
  la::Picker picker;
  la::PickResult r;
  la::PickParams params;
  params.budget = la::BudgetFor(lb::Level::kHigh, 0, true);
  // omni lights, lamps that are off and lamps beyond 90 m are no candidates
  std::vector<la::Lamp> list;
  la::Lamp omni = Lamp(0, 10);
  omni.type = 0;
  list.push_back(omni);
  la::Lamp off = Lamp(2, 12);
  off.on = 0;
  list.push_back(off);
  list.push_back(Lamp(0, 200));
  list.push_back(Lamp(1, 14));
  picker.Update(list, SimpleView(0, 0), params, &r);
  Check(r.count == 1 && r.usable == 1 && r.entries[0].lamp.pos[2] == 14.0f, "omni, off and far lamps are left out");
  // two records of one place count once
  picker.Reset();
  picker.Update({Lamp(0, 10), Lamp(0, 10)}, SimpleView(0, 0), params, &r);
  Check(r.count == 1, "one place, one lamp");
  // a lamp BEHIND the camera still counts (it lights the road in front of it, as far as its shadow reaches); one far behind does not
  la::View v;
  v.have_eye = true;
  v.eye[0] = 0.0;
  v.eye[1] = 1.5;
  v.eye[2] = 0.0;
  v.have_vp = true;
  v.view_projection = LookAlongZ(0.0, 0.0);
  const double behind_near[3] = {0.0, 9.0, -8.0};
  const double behind_far[3] = {0.0, 9.0, -60.0};
  const double ahead[3] = {0.0, 9.0, 20.0};
  CheckNear(la::FrustumShare(v.view_projection, ahead, 21.0), 1.0, 1e-9, "a lamp in the view");
  Check(la::FrustumShare(v.view_projection, behind_near, 21.0) > 0.0 && la::FrustumShare(v.view_projection, behind_near, 21.0) < 1.0, "a lamp behind the camera counts less");
  CheckNear(la::FrustumShare(v.view_projection, behind_far, 21.0), 0.0, 1e-9, "one far behind it, not at all");
  Check(la::Score(Lamp(0, 20), v) > la::Score(Lamp(0, -20), v), "the lamp in view beats the same lamp as far behind");
  Check(la::Score(Lamp(0, 20), v) > la::Score(Lamp(0, 60), v), "nearer is more");
  Check(la::Score(Lamp(0, 95), v) == 0.0, "past 90 m: nothing");
  // no tile free: wanted lamps wait while a fading lamp holds the spare
  picker.Reset();
  params.budget = la::BudgetFor(lb::Level::kMedium, 0, true);
  const std::vector<la::Lamp> five = Street(5, 12.0f);
  params.dt = 0.0f;
  picker.Update(five, SimpleView(0, 0), params, &r);
  Check(r.count == 2 && r.wanted == 2, "medium picks 2");
  Check(picker.tracked() == 2, "two tracks");
}

// ---------------------------------------------------------------------------
// the casters and the cache
// ---------------------------------------------------------------------------

void TestCasters() {
  Check(la::IsDynamic(ls::Kind::kVehicle, 1ull), "a vehicle's part moves");
  Check(!la::IsDynamic(ls::Kind::kOpaque, 1ull) && !la::IsDynamic(ls::Kind::kAlphaTested, 1ull), "the opaque world is static");
  Check(la::IsDynamic(ls::Kind::kSkinned, 0x1234ull), "a skinned shader that is not a prop's moves (a person)");
  for (const ls::NamedVs& v : la::kStaticSkinnedVertexShaders) {
    Check(!la::IsDynamic(ls::Kind::kSkinned, v.key), std::string("the prop family's skinned shader is static: ") + v.name);
  }
  bool sorted = true;
  for (size_t i = 1; i < std::size(la::kStaticSkinnedVertexShaders); ++i) {
    sorted = sorted && la::kStaticSkinnedVertexShaders[i - 1].key < la::kStaticSkinnedVertexShaders[i].key;
  }
  Check(sorted && std::size(la::kStaticSkinnedVertexShaders) == 15, "the identities are sorted, fifteen of them");
  la::DrawKey k;
  k.vs = 7;
  k.count = 36;
  k.origin[0] = 10.0;
  const uint64_t base = la::StaticDrawId(k);
  Check(base != 0 && base == la::StaticDrawId(k), "a draw's id is stable and never 0");
  la::DrawKey m = k;
  m.origin[0] = 10.03;
  Check(la::StaticDrawId(m) == base, "a hair's move (under a thirty second of a metre) is the same draw");
  m.origin[0] = 10.2;
  Check(la::StaticDrawId(m) != base, "a prop that was moved is another draw");
  m = k;
  m.index_offset = 64;
  Check(la::StaticDrawId(m) != base, "another mesh is another draw");
  std::set<uint64_t> ids;
  Lcg rng;
  for (int i = 0; i < 5000; ++i) {
    la::DrawKey q;
    q.vs = rng.Int(40);
    q.first = rng.Int(5000);
    q.count = rng.Int(900);
    q.origin[0] = rng.Range(-300.0f, 300.0f);
    q.origin[2] = rng.Range(-300.0f, 300.0f);
    ids.insert(la::StaticDrawId(q));
  }
  Check(ids.size() > 4990, "random draws rarely collide");
}

la::CasterFact Fact(uint32_t index, double x, double y, double z, bool mover = false, bool vehicle = false) {
  la::CasterFact f;
  f.index = index;
  f.origin[0] = x;
  f.origin[1] = y;
  f.origin[2] = z;
  f.mover = mover;
  f.vehicle = vehicle;
  return f;
}

ls::ViewSpec SpecOf(const la::Lamp& lamp) {
  ls::Light light;
  ls::ViewSpec spec;
  Check(la::MakeReplayLight(lamp, 0, &light) && la::FitLampView(light, &spec), "the lamp makes a view");
  return spec;
}

void TestDecideStatic() {
  la::StaticLayer layer;
  Check(la::DecideStatic(layer, 5, 100, 0, false) == la::Build::kRebuild, "a layer never built is built");
  layer.valid = true;
  layer.id = 5;
  layer.built = 100;
  layer.epoch = 0;
  Check(la::DecideStatic(layer, 5, 101, 0, false) == la::Build::kAdd, "a steady layer adds");
  Check(la::DecideStatic(layer, 6, 101, 0, false) == la::Build::kRebuild, "a lamp that takes the tile gets a clean layer");
  Check(la::DecideStatic(layer, 5, 101, 0, true) == la::Build::kRebuild, "a new atlas holds nothing");
  Check(la::DecideStatic(layer, 5, 101, 0, false, 1) == la::Build::kRebuild, "the world changed (the epoch)");
  Check(la::DecideStatic(layer, 5, 100 + la::kRefreshFrames - 1, 0, false) == la::Build::kAdd, "not yet time");
  Check(la::DecideStatic(layer, 5, 100 + la::kRefreshFrames, 0, false) == la::Build::kRebuild, "the safety net: the layer is built again after two minutes");
  Check(la::DecideStatic(layer, 5, 100 + la::kRefreshFrames, 3, false) == la::Build::kAdd, "... staggered by tile so that the lamps do not all rebuild in one frame");
  for (uint32_t i = 0; i < 3200; ++i) {
    layer.drawn.Insert(0x1000000ull + i * 7919ull);
  }
  Check(layer.drawn.Full() && la::DecideStatic(layer, 5, 101, 0, false) == la::Build::kRebuild, "a full set is built again");
  layer.Invalidate();
  Check(!layer.valid && layer.drawn.size() == 0 && !layer.drawn.Contains(0x1000000ull), "invalidated: nothing remembered");
}

void TestPlanLampPass() {
  const la::Lamp lamp = Lamp(0.0f, 0.0f, 70.0f);  // at (0, 9, 0), pointing down
  la::PassInput in;
  in.spec = SpecOf(lamp);
  in.id = lamp.id;
  in.frame = 10;
  in.tile = 0;
  const auto ident = [](uint32_t i) { return 0x100ull + i; };
  // facts: 3 poles under the lamp (static), a car under it (mover, vehicle), a person (mover), a building 80 m off (outside), a pole behind the lamp's near plane region
  std::vector<la::CasterFact> facts = {Fact(10, 2, 0, 3), Fact(11, -3, 0, 1), Fact(12, 5, 0, -4), Fact(20, 1, 0, 1, true, true), Fact(21, -2, 0, -2, true, false), Fact(30, 0, -200, 0)};
  la::StaticLayer layer;
  la::PassPlan plan;
  la::PlanLampPass(layer, in, facts.data(), uint32_t(facts.size()), ident, &plan);
  Check(plan.rebuild() && plan.cache, "the first pass builds the layer");
  Check(plan.world_in_view == 3 && plan.movers_in_view == 2, "three world draws and two movers are in the cone; a draw 200 m below the lamp (past the far plane) is not");
  Check(plan.static_draws.size() == 3 && plan.live_draws.size() == 2 && plan.new_ids.size() == 3, "the static pass draws the world, the live pass the movers, the ids to remember are the world's");
  Check(std::find(plan.static_draws.begin(), plan.static_draws.end(), 30u) == plan.static_draws.end(), "the far building is in neither");
  la::CommitStatic(&layer, plan, lamp.id, 10, 0, false);
  Check(layer.valid && layer.id == lamp.id && layer.drawn.size() == 3 && layer.built == 10, "the layer holds what was drawn");
  // the next frame: nothing new
  in.frame = 11;
  la::PlanLampPass(layer, in, facts.data(), uint32_t(facts.size()), ident, &plan);
  Check(!plan.rebuild() && plan.static_draws.empty() && plan.known == 3 && plan.live_draws.size() == 2, "a steady frame draws the movers only");
  // a new pole comes into the main view: only it is added
  facts.push_back(Fact(13, 4, 0, 6));
  in.frame = 12;
  la::PlanLampPass(layer, in, facts.data(), uint32_t(facts.size()), ident, &plan);
  Check(!plan.rebuild() && plan.static_draws.size() == 1 && plan.static_draws[0] == 13u && plan.new_ids.size() == 1 && plan.known == 3, "a pole that is new is added to the layer");
  // not committed (the pass failed): the next plan asks for it again
  la::PlanLampPass(layer, in, facts.data(), uint32_t(facts.size()), ident, &plan);
  Check(plan.static_draws.size() == 1, "what was not recorded is not remembered");
  la::CommitStatic(&layer, plan, lamp.id, 12, 0, false);
  la::PlanLampPass(layer, in, facts.data(), uint32_t(facts.size()), ident, &plan);
  Check(plan.static_draws.empty() && plan.known == 4, "recorded: remembered");
  // the camera turns away: the poles are not in the frame any more; the layer still holds them (no shrink rule: the replay sees the main view only)
  std::vector<la::CasterFact> only_movers = {facts[3], facts[4]};
  in.frame = 13;
  la::PlanLampPass(layer, in, only_movers.data(), uint32_t(only_movers.size()), ident, &plan);
  Check(!plan.rebuild() && plan.static_draws.empty() && plan.live_draws.size() == 2, "draws that leave the main view do not rebuild the layer (it keeps what it had)");
  // a partial pass forgets the layer
  la::CommitStatic(&layer, plan, lamp.id, 13, 0, true);
  Check(!layer.valid && layer.drawn.size() == 0, "a partial pass: the layer is built again");
  // the world epoch
  la::PlanLampPass(layer, in, facts.data(), uint32_t(facts.size()), ident, &plan);
  la::CommitStatic(&layer, plan, lamp.id, 14, 7, false);
  in.world_epoch = 7;
  in.frame = 15;
  la::PlanLampPass(layer, in, facts.data(), uint32_t(facts.size()), ident, &plan);
  Check(!plan.rebuild(), "the same epoch adds");
  in.world_epoch = 8;
  la::PlanLampPass(layer, in, facts.data(), uint32_t(facts.size()), ident, &plan);
  Check(plan.rebuild() && plan.static_draws.size() == 4, "a new epoch (a sector streamed) rebuilds with every world draw of the view");
  // the cache off: everything into the one tile
  la::PassInput nocache = in;
  nocache.cache = false;
  la::PlanLampPass(layer, nocache, facts.data(), uint32_t(facts.size()), ident, &plan);
  Check(!plan.cache && plan.static_draws.empty() && plan.new_ids.empty() && plan.live_draws.size() == 6, "the cache off: all six draws of the cone go to the one tile");
  // the draw cap keeps the nearest
  std::vector<la::CasterFact> many;
  for (uint32_t i = 0; i < 1000; ++i) {
    many.push_back(Fact(i, double(i % 20) - 10.0, 0.0, double(i % 7) - 3.0 + double(i / 200)));
  }
  la::PassInput capped = in;
  capped.max_draws = 100;
  capped.world_epoch = 0;
  la::StaticLayer fresh;
  la::PlanLampPass(fresh, capped, many.data(), uint32_t(many.size()), ident, &plan);
  Check(plan.static_draws.size() == 100 && plan.over_budget == plan.world_in_view - 100, "the cap keeps 100 draws and counts the rest");
  bool in_draw_order = std::is_sorted(plan.static_draws.begin(), plan.static_draws.end());
  Check(in_draw_order, "in draw order");
  // adds per frame are capped
  la::PassInput lots = in;
  lots.max_draws = 2000;
  lots.world_epoch = 0;
  la::StaticLayer built;
  built.valid = true;
  built.id = lamp.id;
  built.built = 1;
  la::PlanLampPass(built, lots, many.data(), uint32_t(many.size()), ident, &plan);
  Check(!plan.rebuild() && plan.static_draws.size() <= la::kStaticAddsPerFrame && plan.static_draws.size() > 0, "a layer adds at most kStaticAddsPerFrame draws in a frame");
  // a mover's reach is a car's, a world draw's is 40 m
  std::vector<la::CasterFact> edge = {Fact(1, 0, 9.0 - (in.spec.far_m + 10.0), 0, true), Fact(2, 0, 9.0 - (in.spec.far_m + 10.0), 0, false)};
  la::PlanLampPass(fresh, in, edge.data(), uint32_t(edge.size()), ident, &plan);
  Check(plan.movers_in_view == 0 && plan.world_in_view == 1, "a mover 10 m past the far plane is out, a world draw (a long wall) is not");
}

// ---------------------------------------------------------------------------
// the view
// ---------------------------------------------------------------------------

void TestView() {
  const la::Lamp lamp = Lamp(100.0f, -50.0f, 70.0f);
  ls::Light light;
  ls::ViewSpec spec;
  Check(la::MakeReplayLight(lamp, 3, &light), "a street lamp is a light for the replay");
  Check(light.index == 3 && std::fabs(light.dir[1] + 1.0) < 1e-6 && std::fabs(light.range - la::ShadowReach(70.0f)) < 1e-4, "its axis is down, its range the shadow reach");
  Check(la::FitLampView(light, &spec), "the view is fitted");
  CheckNear(spec.eye[0], 100.0, 1e-9, "the eye is the lamp (x)");
  CheckNear(spec.eye[1], 9.0, 1e-9, "the eye is the lamp (y)");
  Check(spec.dir[1] < -0.999, "it looks down");
  Check(std::fabs(spec.near_m - ls::kNearMeters) < 1e-9 && std::fabs(spec.lamps_m) < 1e-9, "the near plane is 1.5 m below the lamp (its housing and arm cast nothing)");
  CheckNear(spec.far_m, la::ShadowReach(70.0f), 1e-3, "the far plane is the shadow reach");
  const double fov_deg = spec.fov * 180.0 / 3.14159265358979;
  // the view holds the cone up to the cap (HL-SHADOW-QUALITY: the beam's 38 degrees are held to 34; the lamp's 62.5 to kConeCapDegrees = 55) and the margin, not the cone's whole 125 degrees
  CheckNear(fov_deg, 2.0 * (la::kConeCapDegrees + ls::kFovMarginDegrees), 1e-6, "as wide as the capped cone with the margin on each side (114 degrees)");
  Check(fov_deg < 125.0 && std::fabs(light.shadow_half - ls::Rad(la::kConeCapDegrees)) < 1e-9, "narrower than the lamp's own cone: its half angle is the cap's");
  // the points inside the capped cone are in the view; the ground at the cap's edge is in it, the ground at the cone's own edge (62.5 degrees) is outside (lit: no shadow there)
  const double under[3] = {100.0, 0.0, -50.0};
  const double edge[3] = {100.0 + 9.0 * std::tan(54.0 * 3.14159265358979 / 180.0), 0.0, -50.0};
  const double rim[3] = {100.0 + 9.0 * std::tan(62.0 * 3.14159265358979 / 180.0), 0.0, -50.0};
  Check(ls::SphereInView(spec, under, 0.1) && ls::SphereInView(spec, edge, 0.1), "the ground under the lamp and at the cap's edge are in the view");
  (void)rim;  // (the replay's cone cull is the frustum's DIAGONAL, conservative: the rim's place is judged by the matrix below)
  const double behind[3] = {100.0, 20.0, -50.0};
  Check(!ls::SphereInView(spec, behind, 0.5), "a point above the lamp is not");
  // the clip and the replay matrices exist, and a point under the lamp lands inside the tile at a depth between 0 and 1
  tp::Mat4 clip;
  Check(ls::LightClip(spec, true, &clip) && tp::Finite(clip), "the lamp's clip matrix");
  sa::Layout layout = sa::MakeLayout([&] {
    sa::AtlasSpec s;
    la::RequestPools(la::BudgetFor(lb::Level::kHigh, 0, true), &s);
    return s;
  }());
  const int live = layout.FindPool(sa::ProviderId::kLamps, sa::PoolRole::kLive);
  const int stat = layout.FindPool(sa::ProviderId::kLamps, sa::PoolRole::kStatic);
  Check(live >= 0 && stat >= 0, "pools");
  for (uint32_t tile = 0; tile < 5; ++tile) {
    const sa::Rect r = layout.SlotRect(uint32_t(stat), tile);
    float uv[4];
    sa::UvOf(layout, r, uv);
    float m[16];
    ls::WorldToShadow(clip, uv, 1.0, 1.0, m);
    float rect[4];
    ls::PublishedRect(uv, rect);
    double u = 0, v = 0, d = 0;
    Check(ls::MapPoint(m, under, &u, &v, &d), "a point under the lamp is in front of it");
    Check(u > rect[0] && u < rect[2] && v > rect[1] && v < rect[3], "... lands inside its tile (" + std::to_string(tile) + ")");
    CheckNear(u, 0.5 * (rect[0] + rect[2]), 0.02 * uv[2], "... at the middle of it");
    Check(d > 0.0 && d < 1.0, "... with a depth between 0 and 1");
    // the ground at the cap's edge lands inside the tile, the ground at the cone's own rim (62 degrees) outside it: a receiver outside the rectangle is lit, so the last 7 degrees of the cone have no shadow
    const double cap_edge[3] = {100.0 + 9.0 * std::tan(54.0 * 3.14159265358979 / 180.0), 0.0, -50.0};
    const double cone_rim[3] = {100.0 + 9.0 * std::tan(62.0 * 3.14159265358979 / 180.0), 0.0, -50.0};
    double eu = 0, ev = 0, ed = 0, ru = 0, rv = 0, rd = 0;
    Check(ls::MapPoint(m, cap_edge, &eu, &ev, &ed) && eu > rect[0] && eu < rect[2] && ev > rect[1] && ev < rect[3], "the ground at the cap's edge (54 degrees) is inside the tile (" + std::to_string(tile) + ")");
    Check(!ls::MapPoint(m, cone_rim, &ru, &rv, &rd) || ru < rect[0] || ru > rect[2] || rv < rect[1] || rv > rect[3], "the ground at the cone's own rim (62 degrees) is outside it (lit, no shadow)");
  }
}

// ---------------------------------------------------------------------------
// the published frame and the pass's light
// ---------------------------------------------------------------------------

la::PublishInput InputFor(const sa::Layout* layout, bool cache) {
  la::PublishInput in;
  in.layout = layout;
  in.live_pool = layout->FindPool(sa::ProviderId::kLamps, sa::PoolRole::kLive);
  in.static_pool = layout->FindPool(sa::ProviderId::kLamps, sa::PoolRole::kStatic);
  in.cache = cache;
  in.atlas_slot = 17;
  in.sampler = 3;
  in.atlas_valid = true;
  return in;
}

la::Published PublishedLamp(const la::Lamp& lamp, uint32_t tile, bool has_live, float weight = 1.0f) {
  la::Published p;
  ls::Light light;
  ls::ViewSpec spec;
  la::MakeReplayLight(lamp, 0, &light);
  la::FitLampView(light, &spec);
  ls::LightClip(spec, true, &p.clip);
  p.id = lamp.id;
  p.tile = tile;
  p.weight = weight;
  p.valid = true;
  p.has_live = has_live;
  p.reach = float(la::ShadowReach(lamp.intensity));
  return p;
}

void TestFrame() {
  sa::AtlasSpec spec;
  la::RequestPools(la::BudgetFor(lb::Level::kHigh, 0, true), &spec);
  const sa::Layout layout = sa::MakeLayout(spec);
  la::PublishInput in = InputFor(&layout, true);
  const la::Lamp l0 = Lamp(0, 10), l1 = Lamp(0, 30), l2 = Lamp(0, 50);
  std::vector<la::Published> lamps = {PublishedLamp(l0, 0, true), PublishedLamp(l1, 3, false, 0.5f), PublishedLamp(l2, 1, true)};
  lg::LampShadowFrame frame;
  std::vector<la::LampRef> refs;
  la::BuildFrame(lamps, in, &frame, &refs);
  Check(frame.count == 5, "a lamp with live casters has two entries, one without has one: 2 + 1 + 2");
  Check(frame.atlas.valid && frame.atlas.slot == 17 && frame.atlas.sampler == 3 && frame.atlas.width == layout.width && frame.atlas.height == layout.height, "the atlas is named");
  Check(refs[0].first_view == lg::kLampEntryBase + 0 && refs[0].view_count == 2 && refs[1].first_view == lg::kLampEntryBase + 2 && refs[1].view_count == 1 &&
            refs[2].first_view == lg::kLampEntryBase + 3 && refs[2].view_count == 2,
        "the refs name the entries in the pass's combined space, consecutive");
  Check(frame.view_layer[0] == lg::ShadowLayer::kStatic && frame.view_layer[1] == lg::ShadowLayer::kLive && frame.view_layer[2] == lg::ShadowLayer::kStatic &&
            frame.view_layer[3] == lg::ShadowLayer::kStatic && frame.view_layer[4] == lg::ShadowLayer::kLive,
        "static, then its live twin");
  Check(frame.view_light[0] == l0.id && frame.view_light[1] == l0.id && frame.view_light[2] == l1.id && frame.view_light[3] == l2.id, "each entry says its lamp");
  Check(frame.FindFirst(l1.id) == 2 && frame.FindFirst(12345) == -1, "FindFirst");
  CheckNear(refs[1].weight, 0.5, 0, "the weight rides on the ref");
  CheckNear(lg::LampReach(frame.views[0]), la::ShadowReach(70.0f), 1e-4, "the reach rides on the entry");
  const int live_pool = in.live_pool, static_pool = in.static_pool;
  // the static entry's rectangle is the static tile of the lamp's tile, the live entry's the live tile
  const sa::Rect s0 = layout.SlotRect(uint32_t(static_pool), 0), v0 = layout.SlotRect(uint32_t(live_pool), 0);
  float uv[4];
  sa::UvOf(layout, s0, uv);
  Check(std::fabs(frame.views[0].atlas_rect[0] - uv[0]) < 1e-7f && std::fabs(frame.views[0].atlas_rect[2] - (uv[0] + uv[2])) < 1e-7f, "the static entry is in the static tile");
  sa::UvOf(layout, v0, uv);
  Check(std::fabs(frame.views[1].atlas_rect[0] - uv[0]) < 1e-7f && std::fabs(frame.views[1].atlas_rect[1] - uv[1]) < 1e-7f, "the live entry is in the live tile");
  Check(frame.views[0].valid == 1 && frame.views[0].texel_size == 1.0f / 512.0f && frame.views[0].depth_bias == ls::kDepthBias, "valid, one texel of 512, the bias");
  // the two entries of a lamp differ only by the tile
  Check(std::memcmp(frame.views[0].world_to_shadow, frame.views[1].world_to_shadow, sizeof(float) * 16) != 0, "... and so by the matrix's offset");
  // the cache off: one single entry in the live pool
  la::PublishInput nocache = InputFor(&layout, false);
  la::BuildFrame(lamps, nocache, &frame, &refs);
  Check(frame.count == 3 && frame.view_layer[0] == lg::ShadowLayer::kSingle && refs[0].view_count == 1, "the cache off: one single entry a lamp");
  // invalid lamps are skipped
  lamps[1].valid = false;
  la::BuildFrame(lamps, in, &frame, &refs);
  Check(frame.count == 4 && refs[1].view_count == 0, "a lamp with no view is not published");
  // capacity: ten lamps with live casters fill 20 entries; an eleventh pair does not fit
  sa::AtlasSpec big;
  la::RequestPools(la::BudgetFor(lb::Level::kUltra, 0, true), &big);
  const sa::Layout big_layout = sa::MakeLayout(big);
  la::PublishInput big_in = InputFor(&big_layout, true);
  std::vector<la::Published> ten;
  for (uint32_t i = 0; i < 10; ++i) {
    ten.push_back(PublishedLamp(Lamp(0.0f, 10.0f + 20.0f * float(i)), i, true));
  }
  la::BuildFrame(ten, big_in, &frame, &refs);
  Check(frame.count == 20, "ten lamps with two entries each fill the table");
  ten.push_back(PublishedLamp(Lamp(0.0f, 400.0f), 3, true));
  la::BuildFrame(ten, big_in, &frame, &refs);
  Check(frame.count == 20 && refs[10].view_count == 0, "a lamp that does not fit is left out whole (never half)");
  // no layout: empty
  la::PublishInput none;
  la::BuildFrame(ten, none, &frame, &refs);
  Check(frame.count == 0 && frame.empty(), "no layout, no frame");
}

void TestPassLight() {
  const la::Lamp lamp = Lamp(885.32f, -756.95f, 70.0f);
  la::LampRef ref;
  ref.id = lamp.id;
  ref.first_view = lg::kLampEntryBase + 4;
  ref.view_count = 2;
  ref.weight = 0.7f;
  ref.reach = 23.0f;
  const lg::Light sub = la::MakePassLight(lamp, ref, la::LampMode::kSubtract);
  Check(sub.pos[0] == 885.0f && sub.pos[2] == -756.5f && sub.pos[1] == 9.0f, "subtract: the lamp where the grid has it");
  Check((sub.flags & lg::kLightFlagGameLit) != 0 && sub.roles == lg::kRoleMaskGameLit, "subtract: the game lights it (the pass takes a share away)");
  CheckNear(sub.range, la::ShadowReach(70.0f) + la::kPassRangeMargin, 1e-4, "subtract: the range is the shadow's reach plus a margin (not the 45 m of the lamp's own light)");
  Check(sub.range < ll::LampRange(70.0f), "... much less than the lamp's light reaches (the scissor is small)");
  Check(sub.shadow.kind == lg::ShadowKind::kAtlas && sub.shadow.first_view == lg::kLampEntryBase + 4 && sub.shadow.view_count == 2 && sub.shadow.weight == 0.7f && sub.shadow.reach == 23.0f,
        "the ShadowRef names the entries, the weight and the reach");
  Check(sub.source == lg::LightSource::kLamp && sub.id == lamp.id && sub.law == lg::LightLaw::kGameLamp && sub.kind == lg::LightKind::kSpot, "a spot lamp of the game's law, id = the lamp's");
  CheckNear(sub.law_a, 70.0, 0, "the plateau is I");
  CheckNear(sub.color[0], 70.0, 1e-4, "its colour is rgb x I x the game's fade");
  const lg::Light own = la::MakePassLight(lamp, ref, la::LampMode::kOwn);
  Check(own.pos[0] == 885.32f && own.pos[2] == -756.95f, "own: the lamp's own place");
  Check((own.flags & lg::kLightFlagGameLit) == 0 && (own.roles & lg::kRoleMaskSurface) != 0, "own: the pass adds it");
  CheckNear(own.range, ll::LampRange(70.0f), 1e-4, "own: the lamp's whole reach");
  const lg::Light bare = la::MakePassLight(lamp, la::LampRef{}, la::LampMode::kSubtract);
  Check(bare.shadow.kind == lg::ShadowKind::kNone && bare.shadow.view_count == 0, "no entries: no atlas ref");
  // the lamp's colour carries the game's fade
  la::Lamp faded = lamp;
  faded.scale = 0.5f;
  CheckNear(la::MakePassLight(faded, ref, la::LampMode::kSubtract).color[0], 35.0, 1e-4, "half faded: half the colour");
}

// ---------------------------------------------------------------------------
// the lookup
// ---------------------------------------------------------------------------

// a fake atlas: a grid of depths, 1 where nothing is drawn, read with the index clamped
struct FakeAtlas {
  uint32_t width = 0, height = 0;
  std::vector<float> d;
  FakeAtlas(uint32_t w, uint32_t h) : width(w), height(h), d(size_t(w) * h, 1.0f) {}
  float At(int x, int y) const {
    x = std::clamp(x, 0, int(width) - 1);
    y = std::clamp(y, 0, int(height) - 1);
    return d[size_t(y) * width + size_t(x)];
  }
  // a block of texels (the uv rectangle), all at `depth`
  void Fill(const float rect[4], float depth) {
    for (uint32_t y = uint32_t(rect[1] * float(height)); y < uint32_t(rect[3] * float(height)); ++y) {
      for (uint32_t x = uint32_t(rect[0] * float(width)); x < uint32_t(rect[2] * float(width)); ++x) {
        d[size_t(y) * width + x] = depth;
      }
    }
  }
};

struct World {
  sa::Layout layout;
  lg::LampShadowFrame frame;
  std::vector<la::LampRef> refs;
  la::Lamp lamp;
};

World MakeWorld(bool has_live) {
  World w;
  sa::AtlasSpec spec;
  la::RequestPools(la::BudgetFor(lb::Level::kHigh, 0, true), &spec);
  w.layout = sa::MakeLayout(spec);
  w.lamp = Lamp(0.0f, 0.0f, 70.0f);
  la::PublishInput in = InputFor(&w.layout, true);
  la::BuildFrame({PublishedLamp(w.lamp, 0, has_live)}, in, &w.frame, &w.refs);
  return w;
}

lg::ShadowRef RefOf(const la::LampRef& r) {
  lg::ShadowRef ref;
  ref.kind = lg::ShadowKind::kAtlas;
  ref.first_view = uint8_t(r.first_view);
  ref.view_count = uint8_t(r.view_count);
  ref.weight = r.weight;
  ref.reach = r.reach;
  return ref;
}

void TestLookup() {
  const World w = MakeWorld(true);
  FakeAtlas atlas(w.layout.width, w.layout.height);
  const lg::ShadowRef ref = RefOf(w.refs[0]);
  const float lamp_pos[3] = {0.0f, 9.0f, 0.0f};
  const auto depth = [&](int x, int y) { return atlas.At(x, y); };
  // the point (3, 0, 2) under the lamp: where is it in the static tile?
  const float p[3] = {3.0f, 0.0f, 2.0f};
  double u = 0, v = 0, dz = 0;
  Check(ls::MapPoint(w.frame.views[0].world_to_shadow, std::array<double, 3>{3.0, 0.0, 2.0}.data(), &u, &v, &dz), "the point maps");
  // nothing drawn: lit
  CheckNear(sl::LampLit(w.frame, ref, lamp_pos, p, sl::Mode::kSurface, depth), 1.0, 0, "an empty map: lit");
  // a caster in the static map in front of the point (stored depth smaller than the receiver's): in shadow
  const float static_rect[4] = {float(u) - 0.01f, float(v) - 0.01f, float(u) + 0.01f, float(v) + 0.01f};
  atlas.Fill(static_rect, float(dz) * 0.5f);
  CheckNear(sl::LampLit(w.frame, ref, lamp_pos, p, sl::Mode::kSurface, depth), 0.0, 1e-6, "a static caster in front: in shadow");
  // the weight blends it away
  lg::ShadowRef half = ref;
  half.weight = 0.25f;
  CheckNear(sl::LampLit(w.frame, half, lamp_pos, p, sl::Mode::kSurface, depth), 0.75, 1e-6, "a shadow at weight 0.25 darkens a quarter");
  half.weight = 0.0f;
  CheckNear(sl::LampLit(w.frame, half, lamp_pos, p, sl::Mode::kSurface, depth), 1.0, 0, "a shadow at weight 0 is nothing");
  // a point beside the caster is lit
  const float q[3] = {-6.0f, 0.0f, -5.0f};
  CheckNear(sl::LampLit(w.frame, ref, lamp_pos, q, sl::Mode::kSurface, depth), 1.0, 1e-6, "a point beside it is lit");
  // the live layer multiplies: a live caster over another point
  double u2 = 0, v2 = 0, dz2 = 0;
  Check(ls::MapPoint(w.frame.views[1].world_to_shadow, std::array<double, 3>{-5.0, 0.0, 4.0}.data(), &u2, &v2, &dz2), "the second point maps in the live tile");
  const float live_rect[4] = {float(u2) - 0.01f, float(v2) - 0.01f, float(u2) + 0.01f, float(v2) + 0.01f};
  atlas.Fill(live_rect, float(dz2) * 0.5f);
  const float p2[3] = {-5.0f, 0.0f, 4.0f};
  CheckNear(sl::LampLit(w.frame, ref, lamp_pos, p2, sl::Mode::kSurface, depth), 0.0, 1e-6, "a live caster in front: in shadow");
  CheckNear(sl::LampLit(w.frame, ref, lamp_pos, p, sl::Mode::kSurface, depth), 0.0, 1e-6, "the static shadow is still there (the layers multiply)");
  // the edge fade: the same shadowed point moved out to the reach is lit again
  const float lamp_far[3] = {0.0f, 9.0f, 0.0f};
  const float reach = lg::LampReach(w.frame.views[0]);
  CheckNear(sl::LampEdgeFade(0.0f, reach), 1.0, 0, "no fade near the lamp");
  CheckNear(sl::LampEdgeFade(0.8f * reach, reach), 1.0, 1e-6, "no fade up to four fifths of the reach");
  CheckNear(sl::LampEdgeFade(reach, reach), 0.0, 1e-6, "gone at the reach");
  CheckNear(sl::LampEdgeFade(0.9f * reach, reach), 0.5, 1e-6, "half way through the fade: half");
  CheckNear(sl::LampEdgeFade(100.0f, reach), 0.0, 0, "beyond: gone");
  CheckNear(sl::LampEdgeFade(100.0f, 0.0f), 1.0, 0, "no reach known: no fade");
  const float far_p[3] = {3.0f, 0.0f, 2.0f};
  float lamp_away[3] = {far_p[0] + 100.0f, 9.0f, far_p[2]};  // the lamp is "far" from the point: only the edge fade sees it
  CheckNear(sl::LampLit(w.frame, ref, lamp_away, far_p, sl::Mode::kSurface, depth), 1.0, 1e-6, "a shadow at the reach is faded out entirely");
  (void)lamp_far;
  // an entry below the lamps' base, or past the frame, or no views: lit
  lg::ShadowRef low = ref;
  low.first_view = 2;
  CheckNear(sl::LampLit(w.frame, low, lamp_pos, p, sl::Mode::kSurface, depth), 1.0, 0, "an entry of the headlights' table is not a lamp's: lit");
  lg::ShadowRef past = ref;
  past.first_view = uint8_t(lg::kLampEntryBase + 15);
  CheckNear(sl::LampLit(w.frame, past, lamp_pos, p, sl::Mode::kSurface, depth), 1.0, 0, "an entry past the frame: lit");
  lg::ShadowRef none = ref;
  none.view_count = 0;
  CheckNear(sl::LampLit(w.frame, none, lamp_pos, p, sl::Mode::kSurface, depth), 1.0, 0, "no views: lit");
  none = ref;
  none.kind = lg::ShadowKind::kRtMask;
  CheckNear(sl::LampLit(w.frame, none, lamp_pos, p, sl::Mode::kSurface, depth), 1.0, 0, "a ray traced mask is not this lookup's");
  // a light the frame has no live entry for: one view only
  const World solo = MakeWorld(false);
  Check(solo.refs[0].view_count == 1 && solo.frame.count == 1, "a lamp with nothing that moves has one entry");
  CheckNear(sl::LampLit(solo.frame, RefOf(solo.refs[0]), lamp_pos, p, sl::Mode::kSurface, depth), 0.0, 1e-6, "a lone static entry shadows by itself");
}

// the HLSL twin
namespace shim_state {
const FakeAtlas* atlas = nullptr;
std::array<hlsl_shim::LslView, 28> views{};
lg::GpuLampTable table;
}  // namespace shim_state

}  // namespace

namespace {
hlsl_shim::LslView ToLsl(const lg::LocalShadowView& v) {
  hlsl_shim::LslView o;
  o.m0 = hlsl_shim::float4(v.world_to_shadow[0], v.world_to_shadow[1], v.world_to_shadow[2], v.world_to_shadow[3]);
  o.m1 = hlsl_shim::float4(v.world_to_shadow[4], v.world_to_shadow[5], v.world_to_shadow[6], v.world_to_shadow[7]);
  o.m2 = hlsl_shim::float4(v.world_to_shadow[8], v.world_to_shadow[9], v.world_to_shadow[10], v.world_to_shadow[11]);
  o.m3 = hlsl_shim::float4(v.world_to_shadow[12], v.world_to_shadow[13], v.world_to_shadow[14], v.world_to_shadow[15]);
  o.rect = hlsl_shim::float4(v.atlas_rect[0], v.atlas_rect[1], v.atlas_rect[2], v.atlas_rect[3]);
  o.bias = v.depth_bias;
  o.texel = v.texel_size;
  o.light_index = v.light_index;
  o.valid = v.valid;
  return o;
}
}  // namespace

// the seams, defined as the pass would (the hlsli declared them): entries below the base are the headlights' table (empty here), the others the lamps'
namespace hlsl_shim {
float LslTexel(uint, int ix, int iy) { return shim_state::atlas->At(ix, iy); }
uint LslAtlasWidth(uint) { return shim_state::atlas->width; }
uint LslAtlasHeight(uint) { return shim_state::atlas->height; }
LslView LslLoadView(uint entry) { return shim_state::views[entry]; }
uint LslLayerOf(uint entry) {
  if (entry < uint(LSL_LAMP_ENTRY_BASE)) {
    return 0u;
  }
  const uint e = entry - uint(LSL_LAMP_ENTRY_BASE);
  return LslLayerBits(e < 16u ? shim_state::table.header.layers_lo : shim_state::table.header.layers_hi, e & 15u);
}
}  // namespace hlsl_shim

namespace {

void TestHlslTwin(const std::string& gpu_dir) {
  Check(LSL_LAMP_ENTRY_BASE == int(lg::kLampEntryBase) && LSL_LAMP_FADE_FROM == lg::kLampFadeFrom && LSL_MAX_VIEWS == int(lg::kLampEntryBase + lg::kMaxLampEntries),
        "the twin's base, fade and room (8 + 20 entries)");
  std::ifstream in(gpu_dir + "/hlsl/lamp_shadow_lookup.hlsli", std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string text = ss.str();
  Check(!text.empty(), "the lamp hlsli is found");
  Check(text.find("static const") == std::string::npos && text.find("LslLampLit") != std::string::npos && text.find("LARECOMP_LAMP_SHADOW_LOOKUP_HLSLI") != std::string::npos, "the twin's guard and entry point, no static const");
  Check(sizeof(hlsl_shim::LslView) >= 96, "the row has the 96 bytes' fields");
  Lcg rng;
  uint64_t checked = 0;
  for (int n = 0; n < 2500; ++n) {
    World w;
    sa::AtlasSpec spec;
    la::RequestPools(la::BudgetFor(lb::Level::kHigh, 0, true), &spec);
    w.layout = sa::MakeLayout(spec);
    la::PublishInput pin = InputFor(&w.layout, true);
    std::vector<la::Published> lamps;
    std::vector<la::Lamp> lamp_data;
    const uint32_t count = 1 + rng.Int(5);
    for (uint32_t i = 0; i < count; ++i) {
      lamp_data.push_back(Lamp(rng.Range(-40.0f, 40.0f), rng.Range(-40.0f, 40.0f), rng.Range(30.0f, 100.0f)));
      lamps.push_back(PublishedLamp(lamp_data.back(), i, rng.Next() < 0.6f, rng.Next() < 0.5f ? 1.0f : rng.Next()));
    }
    la::BuildFrame(lamps, pin, &w.frame, &w.refs);
    FakeAtlas atlas(w.layout.width, w.layout.height);
    for (size_t k = 0; k < atlas.d.size(); k += 1 + rng.Int(7)) {
      atlas.d[k] = rng.Next() < 0.5f ? 1.0f : rng.Range(0.0f, 1.0f);  // a scatter of casters
    }
    shim_state::atlas = &atlas;
    lg::BuildGpuLampTable(w.frame, &shim_state::table);
    for (auto& e : shim_state::views) {
      e = hlsl_shim::LslView{};
    }
    for (uint32_t e = 0; e < w.frame.count; ++e) {
      shim_state::views[lg::kLampEntryBase + e] = ToLsl(w.frame.views[e]);
    }
    const uint32_t k = rng.Int(count);
    if (w.refs[k].view_count == 0) {
      continue;
    }
    const lg::ShadowRef ref = RefOf(w.refs[k]);
    const float lp[3] = {lamp_data[k].pos[0], lamp_data[k].pos[1], lamp_data[k].pos[2]};
    float p[3] = {lp[0] + rng.Range(-25.0f, 25.0f), rng.Range(-1.0f, 4.0f), lp[2] + rng.Range(-25.0f, 25.0f)};
    // the receiver's unit normal (the pass gives the depth plane's quad normal: HL-SHADOW-QUALITY's plane lookup needs it): random, every second sample the facing-the-lamp default
    float nrm[3] = {rng.Range(-1.0f, 1.0f), rng.Range(0.05f, 1.0f), rng.Range(-1.0f, 1.0f)};
    const float nlen = std::sqrt(nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2]);
    for (float& c : nrm) {
      c /= nlen;
    }
    for (const sl::Mode mode : {sl::Mode::kSurface, sl::Mode::kAir}) {
      const auto depth = [&](int x, int y) { return atlas.At(x, y); };
      const float want = sl::LampLit(w.frame, ref, lp, p, nrm, mode, depth);
      const float got = hlsl_shim::LslLampLit(uint32_t(ref.kind), ref.first_view, ref.view_count, ref.weight, lp[0], lp[1], lp[2], p[0], p[1], p[2], nrm[0], nrm[1], nrm[2], 0u, uint32_t(mode));
      Check(std::memcmp(&want, &got, sizeof(float)) == 0, "the twin's lamp lookup is the reference's, bit for bit");
      ++checked;
    }
  }
  Check(checked > 3000, "most random lamps were looked up in both modes");
  // the table's layer bits: entry e of the frame at bits 2e of lo (e < 16) or hi
  lg::LampShadowFrame f;
  f.atlas.valid = true;
  f.atlas.slot = 5;
  f.atlas.width = 2048;
  f.atlas.height = 1024;
  f.count = 20;
  for (uint32_t e = 0; e < 20; ++e) {
    f.views[e].valid = 1;
    f.view_layer[e] = (e % 2 == 0) ? lg::ShadowLayer::kStatic : lg::ShadowLayer::kLive;
  }
  lg::GpuLampTable t;
  lg::BuildGpuLampTable(f, &t);
  Check(t.header.count == 20 && t.header.atlas_slot == 5 && t.header.atlas_width == 2048 && t.header.atlas_height == 1024, "the table's header");
  bool bits = true;
  for (uint32_t e = 0; e < 20; ++e) {
    const uint32_t word = e < 16 ? t.header.layers_lo : t.header.layers_hi;
    bits = bits && ((word >> (2 * (e & 15))) & 3u) == (e % 2 == 0 ? 1u : 2u);
  }
  Check(bits, "two bits an entry across two words");
  shim_state::table = t;
  Check(hlsl_shim::LslLayerOf(8 + 0) == 1u && hlsl_shim::LslLayerOf(8 + 1) == 2u && hlsl_shim::LslLayerOf(8 + 17) == 2u && hlsl_shim::LslLayerOf(3) == 0u, "the seam reads the right word");
  f.atlas.valid = false;
  lg::BuildGpuLampTable(f, &t);
  Check(t.header.count == 0 && t.header.atlas_slot == lg::kNoLightSlot, "no valid atlas: an empty table that names no texture");
  Check(sizeof(lg::GpuLampTable) == 32 + 96 * 20, "the table's size");
}

// THE PASS: the per-pixel twin (subtract, own, no table, debug), the buffer with the lamps' table, the shader's text.
std::string ReadAll(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}
bool Has(const std::string& text, const std::string& what) { return text.find(what) != std::string::npos; }

void TestPass(const std::string& gpu_dir) {
  const World w = MakeWorld(true);
  FakeAtlas atlas(w.layout.width, w.layout.height);
  const auto depth = [&](int x, int y) { return atlas.At(x, y); };
  // a shadowed receiver (static caster in front of it)
  const float p[3] = {3.0f, 0.0f, 2.0f};
  double u = 0, v = 0, dz = 0;
  ls::MapPoint(w.frame.views[0].world_to_shadow, std::array<double, 3>{3.0, 0.0, 2.0}.data(), &u, &v, &dz);
  const float rect[4] = {float(u) - 0.01f, float(v) - 0.01f, float(u) + 0.01f, float(v) + 0.01f};
  atlas.Fill(rect, float(dz) * 0.5f);
  const float lit_p[3] = {-8.0f, 0.0f, -6.0f};
  const float normal[3] = {0.0f, 1.0f, 0.0f};
  const float albedo[3] = {0.5f, 0.5f, 0.5f};
  lp::Units units;
  units.night = 1.0f;
  units.exposure = 16.0f;
  units.gain = 1.0f;
  const lg::Light sub = la::MakePassLight(w.lamp, w.refs[0], la::LampMode::kSubtract);
  const lg::Light own = la::MakePassLight(w.lamp, w.refs[0], la::LampMode::kOwn);
  float term[3], out[3];
  lp::PixelLight(sub, units, p, normal, albedo, 1.0f, term);
  Check(term[0] > 0.0f && term[1] > 0.0f && term[2] > 0.0f, "the lamp lights the receiver (the term is over 0)");
  // subtract: in shadow the whole term is taken away (negative), lit: nothing
  Check(lp::PixelLampShadowed(sub, units, &w.frame, true, 0, p, normal, albedo, 1.0f, depth, out), "subtract: a shadowed pixel changes");
  for (int i = 0; i < 3; ++i) {
    CheckNear(out[i], -term[i], 1e-5 * std::fabs(term[i]) + 1e-9, "subtract: the pixel loses exactly the lamp's term in full shadow (what the grid added)");
  }
  Check(!lp::PixelLampShadowed(sub, units, &w.frame, true, 0, lit_p, normal, albedo, 1.0f, depth, out), "subtract: a lit pixel is left alone (the pass draws nothing: discard)");
  // own: in shadow nothing, lit the term
  Check(!lp::PixelLampShadowed(own, units, &w.frame, true, 0, p, normal, albedo, 1.0f, depth, out), "own: a shadowed pixel gets no light");
  float own_term[3];
  lp::PixelLight(own, units, lit_p, normal, albedo, 1.0f, own_term);
  Check(lp::PixelLampShadowed(own, units, &w.frame, true, 0, lit_p, normal, albedo, 1.0f, depth, out) && out[0] == own_term[0] && out[1] == own_term[1] && out[2] == own_term[2],
        "own: a lit pixel gets the whole term");
  // the weight fades the shadow in
  la::LampRef half = w.refs[0];
  half.weight = 0.5f;
  const lg::Light sub_half = la::MakePassLight(w.lamp, half, la::LampMode::kSubtract);
  lp::PixelLampShadowed(sub_half, units, &w.frame, true, 0, p, normal, albedo, 1.0f, depth, out);
  CheckNear(out[0], -0.5 * term[0], 1e-5 * std::fabs(term[0]), "a shadow at weight 0.5 takes half of the term");
  // no table: a game lamp has nothing to take away, an owned lamp keeps its light
  Check(!lp::PixelLampShadowed(sub, units, &w.frame, false, 0, p, normal, albedo, 1.0f, depth, out), "subtract with no table: nothing");
  Check(lp::PixelLampShadowed(own, units, &w.frame, false, 0, p, normal, albedo, 1.0f, depth, out) && out[0] > 0.0f, "own with no table: the lamp's light, unshadowed");
  Check(!lp::PixelLampShadowed(sub, units, nullptr, true, 0, p, normal, albedo, 1.0f, depth, out), "no frame: nothing to take away");
  // a pixel the lamp does not light takes no lookup and no change
  const float far_p[3] = {500.0f, 0.0f, 500.0f};
  Check(!lp::PixelLampShadowed(sub, units, &w.frame, true, 0, far_p, normal, albedo, 1.0f, depth, out), "out of the lamp's reach: nothing");
  // an emissive surface (lit weight 0) takes nothing
  Check(!lp::PixelLampShadowed(sub, units, &w.frame, true, 0, p, normal, albedo, 0.0f, depth, out), "lit weight 0 (emissive): nothing");
  // a light that is not a street lamp is the headlights' path
  lg::Light head = sub;
  head.source = lg::LightSource::kHeadlight;
  head.flags = 0;
  Check(lp::PixelLampShadowed(head, units, &w.frame, true, 0, p, normal, albedo, 1.0f, depth, out), "another source is not looked up here");
  // the debug pictures
  Check(lp::PixelLampShadowed(sub, units, &w.frame, true, lp::kDebugVisibility, p, normal, albedo, 1.0f, depth, out) && out[0] > 0.0f && out[1] == 0.0f, "debug 1: red in the shadow");
  Check(lp::PixelLampShadowed(sub, units, &w.frame, true, lp::kDebugVisibility, lit_p, normal, albedo, 1.0f, depth, out) && out[0] == 0.0f && out[1] > 0.0f, "debug 1: green where lit");
  Check(lp::PixelLampShadowed(sub, units, &w.frame, true, lp::kDebugLamps, p, normal, albedo, 1.0f, depth, out) && (out[0] > 0.0f || out[1] > 0.0f || out[2] > 0.0f), "debug 2: a colour");

  // debug 3: the units check takes the whole term of a game lamp away, in the light and in the shadow, with or without a table; an owned lamp adds nothing
  float lit_term[3];
  lp::PixelLight(sub, units, lit_p, normal, albedo, 1.0f, lit_term);
  Check(lp::PixelLampShadowed(sub, units, &w.frame, true, lp::kDebugRemove, lit_p, normal, albedo, 1.0f, depth, out) && out[0] == -lit_term[0] && out[1] == -lit_term[1] && out[2] == -lit_term[2],
        "debug 3: a lit pixel loses the whole term");
  Check(lp::PixelLampShadowed(sub, units, nullptr, false, lp::kDebugRemove, lit_p, normal, albedo, 1.0f, depth, out) && out[0] == -lit_term[0], "debug 3: with no table too");
  Check(!lp::PixelLampShadowed(own, units, &w.frame, true, lp::kDebugRemove, lit_p, normal, albedo, 1.0f, depth, out), "debug 3: an owned lamp adds nothing");
  Check(!lp::PixelLampShadowed(sub, units, &w.frame, true, lp::kDebugRemove, far_p, normal, albedo, 1.0f, depth, out), "debug 3: out of reach, nothing");

  // RT-LIGHTS-D1: the ray traced visibility on the lamp's path. THE IDENTITY first: with the flag, the table and every visibility 1 the pixel is bit for bit what it was without the ray, in the shadow, in the
  // light, for a lamp the game lights and for one the pass owns; a visibility that is not a number is a lit one; a lamp the game lights is only ever taken away from.
  {
    const auto lit_term_of = [](const lg::Light& l, const lp::Units& un, const float* pos, const float* n, const float* alb, int i) {
      float t[3];
      lp::PixelLight(l, un, pos, n, alb, 1.0f, t);
      return double(t[i]);
    };
    lg::Light sub_rt = sub;
    sub_rt.flags |= lg::kLightFlagRtVisibility;
    sub_rt.shadow.mask_channel = 4;
    lg::Light own_rt = own;
    own_rt.flags |= lg::kLightFlagRtVisibility;
    own_rt.shadow.mask_channel = 1;
    const float* receivers[2] = {p, lit_p};
    for (const float* r : receivers) {
      for (int game = 0; game < 2; ++game) {
        const lg::Light& base = game ? sub : own;
        const lg::Light& with_rt = game ? sub_rt : own_rt;
        float plain[3] = {-1, -1, -1}, same[3] = {-2, -2, -2}, nan_v[3] = {-3, -3, -3};
        const bool a = lp::PixelLampShadowed(base, units, &w.frame, true, 0, r, normal, albedo, 1.0f, depth, plain);
        const bool b = lp::PixelLampShadowed(with_rt, units, &w.frame, true, 0, r, normal, albedo, 1.0f, depth, same, nullptr, true, 1.0f);
        const bool c = lp::PixelLampShadowed(with_rt, units, &w.frame, true, 0, r, normal, albedo, 1.0f, depth, nan_v, nullptr, true, std::nanf(""));
        Check(a == b && a == c, "RT identity: the pixel is drawn (or not) as without the ray");
        if (a) {
          Check(std::memcmp(plain, same, sizeof(plain)) == 0 && std::memcmp(plain, nan_v, sizeof(plain)) == 0, "RT identity: every visibility 1 (or not a number) is the pixel bit for bit");
        }
        // the flag with no table in the buffer (kFlagRtMask clear: rt_ref false) is the lamp as it was
        float off[3] = {-4, -4, -4};
        const bool d = lp::PixelLampShadowed(with_rt, units, &w.frame, true, 0, r, normal, albedo, 1.0f, depth, off, nullptr, false, 0.0f);
        Check(d == a && (!a || std::memcmp(plain, off, sizeof(plain)) == 0), "RT off: the flag with no table changes nothing");
      }
    }
    // the product with the atlas: a lit receiver (the atlas says 1) at V = 0.5 loses half of a game lamp's term, an owned lamp keeps half; a shadowed one is in the shadow whatever V says
    float half_sub[3], half_own[3];
    Check(lp::PixelLampShadowed(sub_rt, units, &w.frame, true, 0, lit_p, normal, albedo, 1.0f, depth, half_sub, nullptr, true, 0.5f), "a game lamp at V 0.5 on a lit receiver takes half away");
    for (int i = 0; i < 3; ++i) {
      CheckNear(half_sub[i], -0.5 * lit_term_of(sub, units, lit_p, normal, albedo, i), 1e-5, "... exactly half of the term, negative");
      Check(half_sub[i] <= 0.0f, "... and never positive");
    }
    Check(lp::PixelLampShadowed(own_rt, units, &w.frame, true, 0, lit_p, normal, albedo, 1.0f, depth, half_own, nullptr, true, 0.5f), "an owned lamp at V 0.5 keeps half");
    for (int i = 0; i < 3; ++i) {
      CheckNear(half_own[i], 0.5 * lit_term_of(own, units, lit_p, normal, albedo, i), 1e-5, "... half of the term");
    }
    float shadow_out[3];
    Check(lp::PixelLampShadowed(sub_rt, units, &w.frame, true, 0, p, normal, albedo, 1.0f, depth, shadow_out, nullptr, true, 0.5f), "a shadowed receiver of a game lamp changes");
    for (int i = 0; i < 3; ++i) {
      CheckNear(shadow_out[i], -lit_term_of(sub, units, p, normal, albedo, i), 1e-5, "... in full: the atlas's shadow times the ray's is still the whole term");
    }
    Check(!lp::PixelLampShadowed(own_rt, units, &w.frame, true, 0, p, normal, albedo, 1.0f, depth, shadow_out, nullptr, true, 0.5f), "an owned lamp in the atlas's shadow gets no light whatever V says");
    // a lamp that has ONLY a channel (no views of the atlas): the ray decides alone; a game lamp is only taken away from
    lg::Light sub_only = sub_rt;
    sub_only.shadow.kind = lg::ShadowKind::kNone;
    sub_only.shadow.view_count = 0;
    sub_only.shadow.first_view = 0;
    lg::Light own_only = own_rt;
    own_only.shadow.kind = lg::ShadowKind::kNone;
    own_only.shadow.view_count = 0;
    own_only.shadow.first_view = 0;
    float only[3];
    Check(!lp::PixelLampShadowed(sub_only, units, &w.frame, true, 0, lit_p, normal, albedo, 1.0f, depth, only, nullptr, true, 1.0f), "a game lamp with only a channel and V 1: nothing is drawn (the game's pixel)");
    Check(!lp::PixelLampShadowed(sub_only, units, nullptr, false, 0, lit_p, normal, albedo, 1.0f, depth, only, nullptr, true, 1.0f), "... whatever the atlas tables are");
    Check(lp::PixelLampShadowed(sub_only, units, nullptr, false, 0, lit_p, normal, albedo, 1.0f, depth, only, nullptr, true, 0.25f), "... at V 0.25 it takes three quarters away");
    for (int i = 0; i < 3; ++i) {
      CheckNear(only[i], -0.75 * lit_term_of(sub, units, lit_p, normal, albedo, i), 1e-5, "... exactly");
      Check(only[i] <= 0.0f, "... and never positive");
    }
    Check(lp::PixelLampShadowed(own_only, units, nullptr, false, 0, lit_p, normal, albedo, 1.0f, depth, only, nullptr, true, 0.25f), "an owned lamp with only a channel keeps a quarter");
    for (int i = 0; i < 3; ++i) {
      CheckNear(only[i], 0.25 * lit_term_of(own, units, lit_p, normal, albedo, i), 1e-5, "... exactly");
    }
    // zero negative pixels: no visibility, however odd, makes a game lamp add light, or take more than its term
    const float odd[] = {-3.0f, -0.0f, 0.0f, 0.2f, 1.0f, 7.0f, std::nanf(""), INFINITY, -INFINITY};
    for (float v : odd) {
      float o[3] = {0, 0, 0};
      lp::PixelLampShadowed(sub_only, units, nullptr, false, 0, lit_p, normal, albedo, 1.0f, depth, o, nullptr, true, v);
      for (int i = 0; i < 3; ++i) {
        Check(o[i] <= 0.0f && o[i] >= -lit_term_of(sub, units, lit_p, normal, albedo, i) * (1.0f + 1e-6f), "a game lamp never adds light and never takes more than its term, whatever V");
      }
    }
    // the headlights' path: a light we own with a channel is multiplied by it (with the atlas's visibility where it has views); with V 1 it is the light unchanged
    lg::Light head_rt = head;
    head_rt.flags |= lg::kLightFlagRtVisibility;
    head_rt.shadow.mask_channel = 0;
    head_rt.shadow.kind = lg::ShadowKind::kNone;
    float head_plain[3], head_one[3], head_half[3];
    lg::ShadowFrame no_frame;
    Check(lp::PixelLightShadowed(head, units, &no_frame, false, lit_p, normal, albedo, 1.0f, depth, head_plain) &&
              lp::PixelLightShadowed(head_rt, units, &no_frame, false, lit_p, normal, albedo, 1.0f, depth, head_one, nullptr, true, 1.0f) && std::memcmp(head_plain, head_one, sizeof(head_plain)) == 0,
          "a headlight with a channel at V 1 is the headlight, bit for bit");
    Check(lp::PixelLightShadowed(head_rt, units, &no_frame, false, lit_p, normal, albedo, 1.0f, depth, head_half, nullptr, true, 0.5f), "at V 0.5 it is half");
    for (int i = 0; i < 3; ++i) {
      CheckNear(head_half[i], 0.5 * head_plain[i], 1e-5 * std::fabs(head_plain[i]) + 1e-9, "... of its term");
    }
    Check(!lp::PixelLightShadowed(head_rt, units, &no_frame, false, lit_p, normal, albedo, 1.0f, depth, head_half, nullptr, true, 0.0f), "at V 0 the pass draws nothing for it");
  }

  // the buffer: the lamps' table is in it only when a selected lamp names entries of it
  lp::Selection selection;
  selection.lights[0] = sub;
  selection.count = 1;
  Check(lp::LampTableNeeded(selection, &w.frame), "a selected lamp with entries needs the table");
  lg::Light no_ref = sub;
  no_ref.shadow = lg::ShadowRef{};
  lp::Selection none;
  none.lights[0] = no_ref;
  none.count = 1;
  Check(!lp::LampTableNeeded(none, &w.frame), "a lamp with no views does not");
  lg::Light stale = sub;
  stale.shadow.first_view = uint8_t(lg::kLampEntryBase + 15);
  lp::Selection st;
  st.lights[0] = stale;
  st.count = 1;
  Check(!lp::LampTableNeeded(st, &w.frame), "a ref past the table's count does not");
  lg::Light headlight = sub;
  headlight.source = lg::LightSource::kHeadlight;
  lp::Selection hl;
  hl.lights[0] = headlight;
  hl.count = 1;
  Check(!lp::LampTableNeeded(hl, &w.frame), "only a street lamp reads it");
  lg::LampShadowFrame invalid = w.frame;
  invalid.atlas.valid = false;
  Check(!lp::LampTableNeeded(selection, &invalid) && !lp::LampTableNeeded(selection, nullptr), "no valid atlas, no frame: no table");

  lp::PassInputs in;
  in.units = units;
  in.width = 1920;
  in.height = 1080;
  in.lamp_shadow = &w.frame;
  in.debug = lp::kDebugVisibility;
  std::vector<uint8_t> bytes(lp::kBufferAlloc, 0xAB);
  lp::BuildBuffer(in, selection, bytes.data());
  lp::PassHeader h;
  std::memcpy(&h, bytes.data(), sizeof(h));
  Check((h.flags & lp::kFlagLampShadow) != 0 && (h.flags & lp::kFlagShadow) == 0 && h.debug == lp::kDebugVisibility && h.count == 1,
        "the header says the lamps' table is there (and not the headlights'), the debug word, one light");
  lg::GpuLampTable want_table;
  lg::BuildGpuLampTable(w.frame, &want_table);
  Check(std::memcmp(bytes.data() + lp::kLampOffset, &want_table, sizeof(want_table)) == 0, "the lamps' table is BuildGpuLampTable's, byte for byte, at kLampOffset");
  bool zero = true;
  for (uint32_t i = lp::kShadowOffset; i < lp::kLampOffset; ++i) {
    zero = zero && bytes[i] == 0;
  }
  Check(zero, "the headlights' region is empty");
  lg::GpuLight row;
  std::memcpy(&row, bytes.data() + lp::kLightsOffset, sizeof(row));
  const lg::Light back = lg::UnpackGpuLight(row);
  Check(back.source == lg::LightSource::kLamp && back.shadow.first_view == sub.shadow.first_view && back.shadow.view_count == sub.shadow.view_count && back.shadow.kind == lg::ShadowKind::kAtlas &&
            (back.flags & lg::kLightFlagGameLit) != 0 && back.shadow.weight == sub.shadow.weight,
        "the GPU row carries the lamp, its entries, its weight and the game's flag");
  in.lamp_shadow = nullptr;
  std::vector<uint8_t> bare(lp::kBufferAlloc, 0xCD);
  lp::BuildBuffer(in, selection, bare.data());
  std::memcpy(&h, bare.data(), sizeof(h));
  bool region_zero = true;
  for (uint32_t i = lp::kShadowOffset; i < lp::kBufferAlloc; ++i) {
    region_zero = region_zero && bare[i] == 0;
  }
  Check((h.flags & lp::kFlagLampShadow) == 0 && region_zero, "without a lamp frame the flag is clear and the tables' region is zero");

  // the shader's text keeps to what the twin says
  const std::string ps = ReadAll(gpu_dir + "/hlsl/local_light_ps.hlsl");
  const std::string hlsli = ReadAll(gpu_dir + "/hlsl/local_light.hlsli");
  Check(!ps.empty() && !hlsli.empty(), "the shader and its twin are readable");
  Check(Has(ps, "#define LSL_MAX_VIEWS 28") && Has(ps, "#include \"lamp_shadow_lookup.hlsli\"") && Has(ps, "LpLampLayersLo") && Has(ps, "LpLampLayersHi"),
        "the pass raises the lookup's room to both tables and includes the lamps' lookup");
  Check(Has(ps, "if (entry >= uint(LSL_LAMP_ENTRY_BASE))") && Has(ps, "at = uint(LP_LAMP_VIEWS_OFFSET) + (entry - uint(LSL_LAMP_ENTRY_BASE)) * uint(LP_SHADOW_VIEW_BYTES);"),
        "the view seam reads the lamps' table from the combined entry space's base");
  // RT-LIGHTS-D1: a lamp that has only a ray traced channel (no atlas views) is looked at too: `|| rt_ref`; the atlas's lookup runs inside it for a lamp that has views
  Check(Has(ps, "l.source == uint(LL_SOURCE_LAMP) && ((l.shadow_kind == uint(LL_SHADOW_ATLAS) && l.view_count != 0u) || rt_ref)") &&
            Has(ps, "LslLampLit(l.shadow_kind, l.first_view, l.view_count, l.shadow_weight, l.pos.x, l.pos.y, l.pos.z, world.x, world.y, world.z, lamp_normal.x, lamp_normal.y, lamp_normal.z, lamp_head.y, uint(LSL_MODE_SURFACE))") &&
            Has(ps, "const float3 lamp_normal = LpShadowNormal(g, p, world, normal);"),
        "the shader looks a street lamp up through the lamps' lookup, with the depth plane's geometric normal as the headlights");
  Check(Has(ps, "add = game_lit ? -add * (1.0f - lamp_vis) : add * lamp_vis;") && Has(ps, "(l.flags & uint(LL_FLAG_GAME_LIT)) != 0u"),
        "a game lamp's shadowed share is taken away, an owned lamp's light is multiplied");
  // INTEGRATION-TRIAL: the lookups are inside LpShadowed (called by LpOneLight once the term is known), the lamps' before the headlights' path
  const size_t lit_at = ps.find("float3 add = NgFinite3(LpPixelLight(l, g, world, normal, albedo, lit));");
  const size_t lamp_at = ps.find("LslLampLit(");
  const size_t head_at = ps.find("add *= LslLightLit(");
  const size_t call_at = ps.find("return LpShadowed(l, g, flags, p, world, normal, add);");
  Check(lit_at != std::string::npos && lamp_at != std::string::npos && head_at != std::string::npos && call_at != std::string::npos && lamp_at < head_at && lit_at < call_at,
        "the lamp's lookup runs after the term is known (LpOneLight calls LpShadowed once it is) and before the headlights' path");
  Check(Has(ps, "LP_OFF_DEBUG") && Has(ps, "LP_DEBUG_VISIBILITY") && Has(ps, "LP_DEBUG_LAMPS") && Has(ps, "LP_DEBUG_REMOVE") && Has(ps, "return float4(-add, 1.0f);"), "the debug pictures and the units check are there");
  Check(Has(ps, "if ((flags & uint(LP_FLAG_LAMP_SHADOW)) == 0u") && Has(ps, "if (!rt_ref) {\n          if (game_lit) {\n            return float4(0.0f, 0.0f, 0.0f, 0.0f);"),
        "a game lamp with no table takes nothing (and with a ray traced channel the ray's visibility alone decides)");
}

// ---------------------------------------------------------------------------
// the port to the pass of the headlights (feat/lamp-shadows-pass: gb-h2 + the sharper headlight shadows)
// ---------------------------------------------------------------------------

// 4 x 4 inverse by Gauss-Jordan with partial pivoting (the test's own: the view's world -> atlas matrix is not the game's).
bool Invert4(const double a[4][4], double out[4][4]) {
  double m[4][8];
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      m[i][j] = a[i][j];
      m[i][4 + j] = i == j ? 1.0 : 0.0;
    }
  }
  for (int c = 0; c < 4; ++c) {
    int best = c;
    for (int r = c + 1; r < 4; ++r) {
      best = std::fabs(m[r][c]) > std::fabs(m[best][c]) ? r : best;
    }
    if (std::fabs(m[best][c]) < 1e-14) {
      return false;
    }
    for (int j = 0; j < 8; ++j) {
      std::swap(m[c][j], m[best][j]);
    }
    const double inv = 1.0 / m[c][c];
    for (int j = 0; j < 8; ++j) {
      m[c][j] *= inv;
    }
    for (int r = 0; r < 4; ++r) {
      if (r != c) {
        const double f = m[r][c];
        for (int j = 0; j < 8; ++j) {
          m[r][j] -= f * m[c][j];
        }
      }
    }
  }
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      out[i][j] = m[i][4 + j];
    }
  }
  return true;
}

// What the replay would store in a lamp's tile for ONE flat receiver (the plane n . p = d): the depth of the plane at the CENTRE of every texel of the view's rectangle, as a rasteriser interpolates
// it (not a splat of points: those are off the centre by up to half a texel, which the plane lookup's margin is not made for).
void RasterisePlane(const lg::LocalShadowView& v, uint32_t atlas_w, uint32_t atlas_h, const double n[3], double d, FakeAtlas* atlas) {
  double m[4][4];
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      m[i][j] = double(v.world_to_shadow[4 * i + j]);
    }
  }
  double inv[4][4];
  if (!Invert4(m, inv)) {
    return;
  }
  const auto unproject = [&](double u, double w, double z, double out[3]) {
    double h[4];
    const double in[4] = {u, w, z, 1.0};
    for (int i = 0; i < 4; ++i) {
      h[i] = inv[i][0] * in[0] + inv[i][1] * in[1] + inv[i][2] * in[2] + inv[i][3] * in[3];
    }
    for (int i = 0; i < 3; ++i) {
      out[i] = h[i] / h[3];
    }
  };
  const uint32_t x0 = uint32_t(std::ceil(double(v.atlas_rect[0]) * atlas_w)), x1 = uint32_t(std::floor(double(v.atlas_rect[2]) * atlas_w));
  const uint32_t y0 = uint32_t(std::ceil(double(v.atlas_rect[1]) * atlas_h)), y1 = uint32_t(std::floor(double(v.atlas_rect[3]) * atlas_h));
  for (uint32_t y = y0; y < y1; ++y) {
    for (uint32_t x = x0; x < x1; ++x) {
      const double u = (double(x) + 0.5) / atlas_w, w = (double(y) + 0.5) / atlas_h;
      double p0[3], p1[3];
      unproject(u, w, 0.0, p0);
      unproject(u, w, 0.5, p1);
      const double dir[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
      const double den = n[0] * dir[0] + n[1] * dir[1] + n[2] * dir[2];
      if (std::fabs(den) < 1e-12) {
        continue;
      }
      const double t = (d - (n[0] * p0[0] + n[1] * p0[1] + n[2] * p0[2])) / den;
      const double hit[3] = {p0[0] + t * dir[0], p0[1] + t * dir[1], p0[2] + t * dir[2]};
      double ru = 0, rv = 0, rz = 0;
      if (t > 0.0 && ls::MapPoint(v.world_to_shadow, hit, &ru, &rv, &rz) && rz > 0.0 && rz < 1.0) {
        atlas->d[size_t(y) * atlas_w + x] = std::min(atlas->d[size_t(y) * atlas_w + x], float(rz));
      }
    }
  }
}

void TestPortToThePass(const std::string& gpu_dir) {
  // 1. THE SETTING: -1 follows the LIGHT SHADOWS row while the pass runs, 0 is off, 1 is on (the pass then runs for the lamps)
  Check(!la::LampsEnabled(la::kSettingAuto, false) && la::LampsEnabled(la::kSettingAuto, true), "auto: on exactly while the deferred light pass runs for anything else");
  Check(!la::LampsEnabled(0, false) && !la::LampsEnabled(0, true), "0: off whatever the pass does");
  Check(la::LampsEnabled(1, false) && la::LampsEnabled(1, true), "1: on whatever the pass's own settings say");
  Check(la::kSettingAuto == -1, "auto is -1");

  // 2. THE ROW AND THE CLASSES: ONE LIGHT SHADOWS row (Q3), Auto by class (Low off, Mid medium, High ultra), the lamps' count by the level
  struct ClassCase {
    gc::GpuClass cls;
    const char* name;
    uint32_t wanted;
    uint32_t spare;
    double atlas_mib_max;  // the whole atlas (headlights' replay + the lamps' live and static pools) in MiB
  };
  const ClassCase cases[] = {{gc::GpuClass::kLow, "Low", 0, 0, 0.0}, {gc::GpuClass::kMid, "Mid", 2, 1, 22.0}, {gc::GpuClass::kHigh, "High", 8, 2, 136.0}};
  for (const ClassCase& c : cases) {
    const ls::Choice choice = ls::Resolve(c.cls, "auto");
    const la::Budget b = la::BudgetFor(choice.level, 0, true);
    Check(b.on() == (c.wanted != 0) && b.wanted == c.wanted && b.spare == c.spare,
          std::string("Auto on the ") + c.name + " class: " + std::to_string(c.wanted) + " lamps (+" + std::to_string(c.spare) + ")");
    if (c.wanted != 0) {
      const sa::Layout layout = sa::MakeLayout(sa::SpecForLevel(choice.level, 0, true, true));
      Check(layout.valid() && sa::Mib(sa::Bytes(layout)) <= c.atlas_mib_max,
            std::string("the ") + c.name + " class's whole atlas (headlight maps + lamp pools) is within " + std::to_string(int(c.atlas_mib_max)) + " MiB");
    }
  }
  Check(!la::BudgetFor(lb::Level::kOff, 0, true).on(), "LIGHT SHADOWS off: no lamp has a tile");
  Check(la::BudgetFor(lb::Level::kMedium, -1, true).wanted == 0 && la::BudgetFor(lb::Level::kMedium, 10, true).wanted <= la::kMaxSlots,
        "the lamps override: -1 none, 1..10 at most the largest level's slots");

  // 3. THE QUALITY OF THE HEADLIGHTS' SHADOWS (HL-SHADOW-QUALITY) FOR A LAMP: its views carry the texel size and the lookup takes the receiver's plane, so a wall seen from the side (a plane the lamp's map
  // sees at a grazing angle) is not shadowed by its own texels, and a blocker still shadows. A lamp 9 m up, the wall x = 5 m (normal -x), looked at from the lamp.
  {
    const World w = MakeWorld(false);
    Check(w.frame.count == 1 && std::fabs(w.frame.views[0].texel_size - 1.0f / 512.0f) < 1e-9f, "a lamp's view row carries its texel size (the plane lookup's tile width): 1 / 512");
    const lg::ShadowRef ref = RefOf(w.refs[0]);
    const float lamp_pos[3] = {0.0f, 9.0f, 0.0f};
    FakeAtlas atlas(w.layout.width, w.layout.height);
    const double wall_n[3] = {-1.0, 0.0, 0.0};
    RasterisePlane(w.frame.views[0], w.layout.width, w.layout.height, wall_n, -5.0, &atlas);  // -x = -5
    const auto depth = [&](int x, int y) { return atlas.At(x, y); };
    Lcg rng;
    int with_normal_lit = 0, without_normal_lit = 0, tried = 0;
    for (int i = 0; i < 400; ++i) {
      // a point of the wall between 1 and 6 m up and 4 m to each side (the view holds it: within 55 degrees of the lamp's axis)
      const float p[3] = {5.0f, rng.Range(1.0f, 6.0f), rng.Range(-4.0f, 4.0f)};
      double u = 0, v = 0, z = 0;
      if (!ls::MapPoint(w.frame.views[0].world_to_shadow, std::array<double, 3>{double(p[0]), double(p[1]), double(p[2])}.data(), &u, &v, &z) || !(z > 0.0 && z < 1.0)) {
        continue;
      }
      ++tried;
      const float normal[3] = {-1.0f, 0.0f, 0.0f};
      with_normal_lit += sl::LampLit(w.frame, ref, lamp_pos, p, normal, sl::Mode::kSurface, depth) > 0.999f ? 1 : 0;
      without_normal_lit += sl::LampLit(w.frame, ref, lamp_pos, p, sl::Mode::kSurface, depth) > 0.999f ? 1 : 0;
    }
    Check(tried > 300, "most wall points are in the lamp's view");
    Check(with_normal_lit * 100 >= tried * 99,
          "with the receiver's plane a wall seen at a grazing angle is not shadowed by its own texels (" + std::to_string(with_normal_lit) + " of " + std::to_string(tried) + " lit)");
    std::printf("INFO port: the wall's own texels leave %d of %d points lit with the plane, %d without it (facing the lamp)\n", with_normal_lit, tried, without_normal_lit);
    // a blocker in front of the wall (a car's side at x = 3 m, nearer to the lamp in its map than the wall) shadows the wall point behind it
    const double car_n[3] = {-1.0, 0.0, 0.0};
    FakeAtlas blocked = atlas;
    RasterisePlane(w.frame.views[0], w.layout.width, w.layout.height, car_n, -3.0, &blocked);
    const float p[3] = {5.0f, 3.0f, 0.5f};
    const float normal[3] = {-1.0f, 0.0f, 0.0f};
    const auto depth_blocked = [&](int x, int y) { return blocked.At(x, y); };
    CheckNear(sl::LampLit(w.frame, ref, lamp_pos, p, normal, sl::Mode::kSurface, depth_blocked), 0.0, 1e-6, "a blocker nearer to the lamp than the wall shadows the wall point (with the plane lookup too)");
  }

  // 4. THE PASS'S SHADER TEXT FOLLOWS THE HEADLIGHTS' (the lamp branch takes the depth plane's geometric normal and the one lookup; no lookup of its own)
  const std::string ps = ReadAll(gpu_dir + "/hlsl/local_light_ps.hlsl");
  const std::string lamp_hlsli = ReadAll(gpu_dir + "/hlsl/lamp_shadow_lookup.hlsli");
  const std::string lookup_hlsli = ReadAll(gpu_dir + "/hlsl/light_shadow_lookup.hlsli");
  Check(!ps.empty() && !lamp_hlsli.empty() && !lookup_hlsli.empty(), "the shaders are readable");
  Check(Has(lamp_hlsli, "LslLightMean(shadow_kind, first_view, view_count, px, py, pz, nx, ny, nz, slot, mode)") && Has(lookup_hlsli, "LslPlaneLit(v, px, py, pz, nx, ny, nz, slot)") &&
            Has(lookup_hlsli, "LslBilinear"),
        "the lamps' lookup is the ONE lookup (the headlights' plane lookup with the receiver's normal); the air mode stays the volume's");
  Check(Has(ps, "const float3 lamp_normal = LpShadowNormal(g, p, world, normal);") && Has(ps, "const float3 shadow_normal = LpShadowNormal(g, p, world, normal);"),
        "lamps and headlights take the SAME geometric normal");

  // 5. THE WIRING: auto follows the pass, the replay and the pass ask the provider
  const std::string cpp = ReadAll(gpu_dir + "/lamp_shadows.cpp");
  const std::string lights_cpp = ReadAll(gpu_dir + "/local_lights.cpp");
  const std::string shadow_cpp = ReadAll(gpu_dir + "/local_shadow.cpp");
  const std::string settings = ReadAll(gpu_dir + "/../settings/generated/settings_gpu.h");
  Check(Has(cpp, "bool LampShadows::Enabled() const") && Has(cpp, "la::LampsEnabled(int(larecomp::settings::mcla_gpu_lamp_shadows.Get()), pass_runs_)") && Has(cpp, "on_ = Enabled();"),
        "the provider's switch is LampsEnabled(setting, whether the pass runs)");
  Check(Has(lights_cpp, "lamp_shadows_->SetPassRuns(hp::PassOn(route, st::mcla_gpu_local_lights.Get()));"), "the pass tells the provider whether it runs without the lamps");
  Check(Has(cpp, "\"the street lamps are shadowed\"") && Has(cpp, "if (on_ != enabled_logged_) {"), "the log says once when the lamps are switched on or off (the gate's auto runs read it)");
  Check(Has(shadow_cpp, "const bool lamps_on = lamps_ != nullptr && lamps_->Enabled();"), "the replay records into the shared atlas while the lamps are enabled (the one atlas)");
  Check(Has(cpp, "std::clamp<int>(int(st::mcla_gpu_lamp_shadows_view.Get()), 0, 3)"), "the debug view 3 (the units check) reaches the pass: its clamp is 0..3");
  // (the needles are built in two pieces: tools/check_settings_exposure.py reads a whole REX_SETTING(...) line of any source as a definition of the setting)
  const std::string define = std::string("REX_SET") + "TING(i32, ";
  Check(Has(settings, define + "mcla_gpu_lamp_shadows, -1,") && Has(settings, "rex::kDebugOnly, rex::Range{-1, 1});"),
        "the setting's default is -1 (auto: it follows LIGHT SHADOWS), a debug-only setting with no row of its own");
  Check(Has(settings, define + "mcla_gpu_lamp_shadows_mode, 0,") && Has(settings, define + "mcla_gpu_lamp_shadows_cache, 1,"), "subtract mode and the static layers stay the defaults");

  // 6. THE NOISE RULE (AGENT-RULE-NOISE): the lamps' shadow has no stochastic term, so there is no pattern to vary: the lookup is deterministic, the fade is in scene seconds. A noise source added later
  // must come with its per-frame variation and its twin in noise_vary_test.cpp (perf/scripts/noise_guard_units.py), so this fails until it does.
  const size_t lamp_from = ps.find("// A STREET LAMP's light with views of the lamps' table");
  const size_t lamp_to = ps.find("// The shadow: only for a light that has views of the atlas");
  Check(lamp_from != std::string::npos && lamp_to != std::string::npos && lamp_from < lamp_to, "the lamp branch of the pass is found");
  const std::string lamp_branch = lamp_from != std::string::npos && lamp_to != std::string::npos && lamp_from < lamp_to ? ps.substr(lamp_from, lamp_to - lamp_from) : std::string();
  const std::string files[] = {lamp_hlsli, lookup_hlsli, lamp_branch};
  const char* const noise_words[] = {"noise", "Noise", "dither", "Dither", "hash", "Hash", "rand", "Rand", "sin(", "interleaved", "BlueNoise", "frame_index", "frame_serial", "NoiseIndex"};
  bool no_noise = true;
  for (const std::string& f : files) {
    std::istringstream lines(f);
    std::string line;
    while (std::getline(lines, line)) {
      const size_t cut = line.find("//");  // the headers say "no noise in it" in comments: code only
      const std::string code = cut == std::string::npos ? line : line.substr(0, cut);
      for (const char* word : noise_words) {
        if (code.find(word) != std::string::npos) {
          no_noise = false;
          std::printf("FAILED: a noise word '%s' in code of the lamps' shadow path: %s\n", word, line.c_str());
        }
      }
    }
  }
  Check(no_noise, "AGENT-RULE-NOISE: the lamps' shadow path (the lookup, the edge fade, the pass's lamp branch) has no noise, dither or hash: nothing to vary, nothing to slide");
}

// A whole night: a street of lamps, a camera driving along it, the pick, the passes' plans and the published frames hold together frame after frame.
void TestDrive() {
  const la::Budget budget = la::BudgetFor(lb::Level::kUltra, 0, true);
  la::Picker picker;
  la::PickResult r;
  std::vector<la::StaticLayer> layers(budget.tiles());
  sa::AtlasSpec spec;
  la::RequestPools(budget, &spec);
  const sa::Layout layout = sa::MakeLayout(spec);
  la::PublishInput in = InputFor(&layout, true);
  const std::vector<la::Lamp> street = Street(40, 25.0f, 0.0f, 0.0f);
  uint32_t rebuilds = 0, adds_only = 0, max_entries = 0;
  uint64_t frame = 1000;
  std::vector<la::CasterFact> facts;
  Lcg rng;
  for (uint32_t i = 0; i < 600; ++i) {
    facts.push_back(Fact(i, rng.Range(-20.0f, 20.0f), 0.0, rng.Range(-20.0f, 1000.0f), rng.Next() < 0.1f, false));
  }
  for (int step = 0; step < 900; ++step, ++frame) {  // 15 seconds at 60 FPS, driving 20 m/s along the street
    la::PickParams params;
    params.budget = budget;
    params.dt = step == 0 ? 0.0f : 1.0f / 60.0f;
    la::View view;
    view.have_eye = true;
    view.eye[2] = 20.0 * double(step) / 60.0;
    view.eye[1] = 1.5;
    view.have_vp = true;
    view.view_projection = LookAlongZ(0.0, view.eye[2]);
    picker.Update(street, view, params, &r);
    std::vector<la::Published> pub;
    std::set<uint32_t> tiles;
    for (uint32_t k = 0; k < r.count; ++k) {
      const la::Entry& e = r.entries[k];
      Check(tiles.insert(e.tile).second, "no two lamps share a tile");
      Check(e.tile < budget.tiles(), "a tile inside the budget");
      la::PassInput pi;
      pi.spec = SpecOf(e.lamp);
      pi.id = e.id;
      pi.frame = frame;
      pi.tile = e.tile;
      la::PassPlan plan;
      la::PlanLampPass(layers[e.tile], pi, facts.data(), uint32_t(facts.size()), [](uint32_t i) { return 0x5000ull + i; }, &plan);
      if (e.fresh) {
        Check(plan.rebuild(), "a lamp that just took a tile builds its static layer");
      }
      rebuilds += plan.rebuild() ? 1u : 0u;
      adds_only += plan.rebuild() ? 0u : 1u;
      la::CommitStatic(&layers[e.tile], plan, e.id, frame, 0, false);
      la::Published p = PublishedLamp(e.lamp, e.tile, !plan.live_draws.empty(), e.weight);
      pub.push_back(p);
    }
    lg::LampShadowFrame out;
    std::vector<la::LampRef> refs;
    la::BuildFrame(pub, in, &out, &refs);
    max_entries = std::max(max_entries, out.count);
    Check(out.count <= lg::kMaxLampEntries, "the table never overflows");
    for (const la::LampRef& ref : refs) {
      if (ref.view_count != 0) {
        Check(ref.first_view >= lg::kLampEntryBase && ref.first_view + ref.view_count <= lg::kLampEntryBase + out.count, "every ref names entries that exist");
      }
    }
  }
  Check(rebuilds > 8 && adds_only > rebuilds * 3, "most lamp passes of a drive are adds, the rebuilds are the lamps that took a tile");
  Check(max_entries > 8 && max_entries <= 20, "ultra uses many entries");
}

}  // namespace

int main(int argc, char** argv) {
  std::string gpu_dir = argc > 1 ? argv[1] : "";
  if (gpu_dir.empty()) {
    std::string self = __FILE__;
    const size_t cut = self.find_last_of("/\\");
    gpu_dir = cut == std::string::npos ? std::string(".") : self.substr(0, cut);
  }
  TestSources();
  TestGrid();
  TestLevels();
  TestPick();
  TestPickClock();
  TestPickHysteresis();
  TestPickFilters();
  TestCasters();
  TestDecideStatic();
  TestPlanLampPass();
  TestView();
  TestFrame();
  TestPassLight();
  TestLookup();
  TestHlslTwin(gpu_dir);
  TestPass(gpu_dir);
  TestPortToThePass(gpu_dir);
  TestDrive();
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
