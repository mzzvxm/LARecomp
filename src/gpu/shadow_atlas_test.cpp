// Check of the shadow atlas's rules and the one shadow lookup (shadow_atlas_rules.h, shadow_provider.h, light_shadow_lookup.h, hlsl/light_shadow_lookup.hlsli; perf/LIGHT-UNIFY.md package
// LU-5 = step U5). A program of its own: it needs no renderer, no ReXGlue and no GPU, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_shadow_atlas_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_shadow_atlas_test [<hlsl/light_shadow_lookup.hlsli>]   (exit 0 = passed; the hlsli is found next to this file without an argument)
// or by hand: clang++ -std=c++23 -I src/gpu src/gpu/shadow_atlas_test.cpp -o shadow_atlas_test
//
//   LAYOUT      one pool is local_shadow::MakeAtlas / AtlasFor for every level of both shapes (tile positions, uv, bytes); several pools (the replay's and the parked lamps': live, static) are
//               sound for every level (no overlap, inside, contiguous slots); the memory table of perf/LIGHT-UNIFY.md 4.2 (replay 4 / 6 / 48 MiB; with the lamps 12 / 18 / 72); a fuzz of specs.
//   BUDGET      SpecFor takes the budget's own numbers (and the lamp rows while the budget's are reserved); FitLevel steps a level down when there is no room, asks nothing when nothing is new.
//   BARRIER     UseTracker: depth write before the pass, shader read for PIXEL AND COMPUTE after it, every time (the f427298a rule), a partial pass publishes too.
//   CLEARS      one clear of the whole texture when every pool is drawn every record, rectangles when a static pool exists.
//   SLOTS       StickySlots: a light keeps its slot, the spare slots of a fading light, a newcomer, the worst fading holder, the refusal; a fuzz of 4000 frames.
//   PUBLISHED   ShadowFrameBuilder = light_convert.h ShadowFrameFromLocal on random frames; RebindByIds = headlight_rules::RemapShadowViews + ShadowFrameFromLocal on random sets.
//   LOOKUP      ViewLit kSurface = light_shadow_filter.h ViewLit (the variant's CPU twin: HL-SHADOW-QUALITY), kAir = volumetrics_rules.h ShadowShare (the volume's), LightLit = light_laws.h CombineShadow; the
//               HLSL twin compiled as C++ gives the same numbers; the view table's builder; the edge rules of both modes.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

#include "headlight_rig_rules.h"
#include "light_budget.h"
#include "light_convert.h"
#include "light_frame.h"
#include "light_id.h"
#include "light_laws.h"
#include "light_shadow_lookup.h"
#include "local_shadow_rules.h"
#include "local_shadow_types.h"
#include "shadow_atlas_rules.h"
#include "shadow_provider.h"
#include "volumetrics_rules.h"
#include "volumetrics_types.h"

// ---------------------------------------------------------------------------
// the .hlsli as C++: a shim of what the file uses, in a namespace of its own so that the standard library's floor, min ... are not in the way
// ---------------------------------------------------------------------------
namespace hlsl_shim {
typedef unsigned int uint;
struct float4 {
  float x = 0, y = 0, z = 0, w = 0;
  float4() = default;
  float4(float a, float b, float c, float d) : x(a), y(b), z(c), w(d) {}
};
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
inline float saturate(float v) { return std::min(std::max(v, 0.0f), 1.0f); }
inline uint min(uint a, uint b) { return a < b ? a : b; }
inline int max(int a, int b) { return a > b ? a : b; }
// HLSL's min and max of two floats: the reference's std::min / std::max read the same two operands (a NaN is not a case the tests make)
inline float min(float a, float b) { return b < a ? b : a; }
inline float max(float a, float b) { return a < b ? b : a; }
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }
#include "hlsl/light_shadow_lookup.hlsli"
}  // namespace hlsl_shim

namespace lg = larecomp::gpu;
namespace sl = larecomp::gpu::shadow_lookup;
namespace sf = larecomp::gpu::shadow_filter;
namespace sa = mcla::gpu::shadow_atlas;
namespace ls = mcla::gpu::local_shadow;
namespace lb = mcla::gpu::light_budget;
namespace hr = mcla::gpu::headlight_rules;
namespace vol = mcla::gpu::volumetrics;
namespace ll = larecomp::gpu::light_laws;

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

const char* LevelName(lb::Level l) {
  switch (l) {
    case lb::Level::kMedium: return "medium";
    case lb::Level::kHigh: return "high";
    case lb::Level::kUltra: return "ultra";
    default: return "off";
  }
}
constexpr lb::Level kLevels[] = {lb::Level::kMedium, lb::Level::kHigh, lb::Level::kUltra};

// ---------------------------------------------------------------------------
// layout
// ---------------------------------------------------------------------------

void TestSinglePoolIsTheReplaysLayout() {
  // every view count and tile the replay has, through MakeAtlas: the columns, rows, size and every tile and its uv
  for (const uint32_t tile : {512u, 1024u}) {
    for (uint32_t views = 1; views <= ls::kMaxViews; ++views) {
      const ls::AtlasLayout want = ls::MakeAtlas(tile, views);
      sa::AtlasSpec spec;
      spec.Add({sa::ProviderId::kReplay, sa::PoolRole::kSingle, tile, views, false});
      const sa::Layout got = sa::MakeLayout(spec);
      const std::string at = " (" + std::to_string(views) + " views of " + std::to_string(tile) + ")";
      Check(got.valid() && got.pool_count == 1 && got.slot_count == views, "one pool: one pool of the views" + at);
      Check(got.width == want.width() && got.height == want.height(), "one pool: the size is MakeAtlas's" + at);
      Check(got.pools[0].cols == want.cols && got.pools[0].rows == want.rows, "one pool: the columns and rows are MakeAtlas's" + at);
      Check(sa::ColumnsFor(views) == want.cols, "ColumnsFor is MakeAtlas's rule" + at);
      Check(sa::ValidateLayout(got).empty(), "one pool: the layout is sound" + at);
      bool tiles_same = true, uv_same = true;
      for (uint32_t v = 0; v < views; ++v) {
        const ls::TileRect t = ls::TileAt(want, v);
        const sa::Rect r = got.SlotRect(0, v);
        tiles_same = tiles_same && t.x == r.x && t.y == r.y && t.size == r.size && got.GlobalSlotRect(v) == r;
        float a[4], b[4];
        ls::UvRect(want, v, a);
        sa::UvOf(got, r, b);
        uv_same = uv_same && std::memcmp(a, b, sizeof(a)) == 0;
      }
      Check(tiles_same, "one pool: every tile is TileAt's" + at);
      Check(uv_same, "one pool: every tile's uv is UvRect's, bit for bit" + at);
      Check(sa::Bytes(got) == ls::AtlasBytes(want), "one pool: the memory is AtlasBytes" + at);
    }
  }
  Check(sa::kTexelBytes == ls::kAtlasTexelBytes && sa::kTexelBytes == 8, "a texel of D32 float + S8 is 8 bytes");
}

void TestLevelsMakeTheReplaysAtlas() {
  // every level of both shapes, the debug views setting 0 / 1 / 2: the layout is ls::AtlasFor, and does not depend on anything but the level
  for (const ls::LevelTable table : {ls::LevelTable(lb::kThreeLevels), ls::LevelTable(lb::kTwoLevels)}) {
    for (const lb::Level level : kLevels) {
      for (int per_car = 0; per_car <= 2; ++per_car) {
        const ls::AtlasLayout want = ls::AtlasFor(level, per_car, table);
        const sa::Layout got = sa::LayoutForLevel(level, per_car, false, false, table);
        const std::string at = std::string(" (") + LevelName(level) + ", views " + std::to_string(per_car) + (table.size() == 3 ? ", three levels)" : ", two levels)");
        Check(got.width == want.width() && got.height == want.height(), "the level's layout is AtlasFor's size" + at);
        Check(got.slot_count == want.views && sa::Bytes(got) == ls::AtlasBytes(want), "the level's slots and bytes are AtlasFor's" + at);
        Check(got == sa::LayoutForLevel(level, per_car, false, false, table), "the same level always makes the same layout" + at);
        Check(sa::ValidateLayout(got).empty(), "the layout is sound" + at);
      }
    }
  }
  // the memory of the three level shape (local_shadow.h: medium 9 MiB, high 24, ultra 75: the tiles of HL-SHADOW-QUALITY, 768 / 1024 / 1280)
  CheckNear(sa::Mib(sa::Bytes(sa::LayoutForLevel(lb::Level::kMedium, 0))), 9.0, 1e-9, "medium holds 9 MiB");
  CheckNear(sa::Mib(sa::Bytes(sa::LayoutForLevel(lb::Level::kHigh, 0))), 24.0, 1e-9, "high holds 24 MiB");
  CheckNear(sa::Mib(sa::Bytes(sa::LayoutForLevel(lb::Level::kUltra, 0))), 75.0, 1e-9, "ultra holds 75 MiB");
  Check(sa::LayoutForLevel(lb::Level::kOff, 0).slot_count == 0 && !sa::LayoutForLevel(lb::Level::kOff, 0).valid(), "off makes no atlas");
}

void TestBudgetSpec() {
  for (const lb::Level level : kLevels) {
    const lb::LightBudget b = lb::BudgetFor(true, level, 0, lb::Level::kOff, ls::kLevelTable);
    const sa::AtlasSpec s = sa::SpecFor(b, level);
    Check(s.count == 1 && s.pools[0].provider == sa::ProviderId::kReplay && s.pools[0].tile == b.shadow_tile && s.pools[0].slots == b.shadow_views && !s.pools[0].sticky,
          std::string("the replay's pool is the budget's views and tile (") + LevelName(level) + ")");
    Check(s == sa::SpecForLevel(level, 0), std::string("SpecForLevel is SpecFor of the level's budget (") + LevelName(level) + ")");
  }
  // the lamps' provider (parked): the budget's reserved numbers when they are set, else the rows of the level
  lb::LightBudget b = lb::BudgetFor(true, lb::Level::kHigh, 0, lb::Level::kOff, ls::kLevelTable);
  sa::AtlasSpec s = sa::SpecFor(b, lb::Level::kHigh, true, false);
  Check(s.count == 2 && s.pools[1].provider == sa::ProviderId::kLamps && s.pools[1].role == sa::PoolRole::kLive && s.pools[1].tile == 512 && s.pools[1].slots == 5 && s.pools[1].sticky,
        "the lamps' live pool at high: 4 views and 1 spare of 512, sticky");
  s = sa::SpecFor(b, lb::Level::kHigh, true, true);
  Check(s.count == 3 && s.pools[2].role == sa::PoolRole::kStatic && s.pools[2].slots == 5, "with the static cache a static pool of the same size follows");
  b.lamp_views = 3;
  b.lamp_views_spare = 2;
  b.lamp_tile = 256;
  s = sa::SpecFor(b, lb::Level::kHigh, true, false);
  Check(s.pools[1].tile == 256 && s.pools[1].slots == 5, "the budget's own lamp numbers win over the rows once they are set");
  s = sa::SpecFor(lb::BudgetFor(true, lb::Level::kOff, 0, lb::Level::kOff, ls::kLevelTable), lb::Level::kOff, true, false);
  Check(s.count == 0, "off: no pool at all (the lamp row of off is none)");
  Check(sa::LampRowOf(lb::Level::kUltra) != nullptr && sa::LampRowOf(lb::Level::kUltra)->views == 8 && sa::LampRowOf(lb::Level::kUltra)->spare == 2 && sa::LampRowOf(lb::Level::kOff) == nullptr,
        "the lamp rows are 2 (+1), 4 (+1), 8 (+2) of 512");
  // an AtlasSpec that is full refuses a fifth pool; a pool of no tiles is left out
  sa::AtlasSpec full;
  for (int i = 0; i < 4; ++i) {
    Check(full.Add({sa::ProviderId::kLamps, sa::PoolRole::kLive, 512, 1, false}), "four pools fit");
  }
  Check(!full.Add({sa::ProviderId::kLamps, sa::PoolRole::kLive, 512, 1, false}) && full.count == 4, "a fifth does not");
  sa::AtlasSpec empty;
  Check(empty.Add({sa::ProviderId::kRtMask, sa::PoolRole::kSingle, 0, 0, false}) && empty.count == 0 && !sa::MakeLayout(empty).valid(), "a provider that asks for nothing (the ray traced mask) adds no pool");
}

void TestMemoryTable() {
  // perf/LIGHT-UNIFY.md 4.2: with the lamps' live pool, the figures the doc counted (4 / 6 / 48 for the replay before HL-SHADOW-QUALITY, now 9 / 24 / 75; the rows of the lamps rounded up to a whole row)
  struct Row {
    lb::Level level;
    double replay, with_lamps, with_static;
  };
  const Row rows[] = {{lb::Level::kMedium, 9.0, 15.0, 21.0}, {lb::Level::kHigh, 24.0, 36.0, 48.0}, {lb::Level::kUltra, 75.0, 105.0, 135.0}};
  for (const Row& r : rows) {
    const std::string at = std::string(" (") + LevelName(r.level) + ")";
    const sa::Layout a = sa::LayoutForLevel(r.level, 0, false);
    const sa::Layout b = sa::LayoutForLevel(r.level, 0, true, false);
    const sa::Layout c = sa::LayoutForLevel(r.level, 0, true, true);
    CheckNear(sa::Mib(sa::Bytes(a)), r.replay, 1e-9, "the replay alone" + at);
    CheckNear(sa::Mib(sa::Bytes(b)), r.with_lamps, 1e-9, "with the lamps' live tiles, rounded up to a whole row" + at);
    CheckNear(sa::Mib(sa::Bytes(c)), r.with_static, 1e-9, "with their static twins" + at);
    Check(sa::ValidateLayout(b).empty() && sa::ValidateLayout(c).empty(), "the layouts with the lamps are sound" + at);
    Check(b.pool_count == 2 && c.pool_count == 3, "pools" + at);
    Check(b.FindPool(sa::ProviderId::kReplay, sa::PoolRole::kSingle) >= 0 && b.FindPool(sa::ProviderId::kLamps, sa::PoolRole::kLive) >= 0 && b.FindPool(sa::ProviderId::kLamps, sa::PoolRole::kStatic) < 0 &&
              c.FindPool(sa::ProviderId::kLamps, sa::PoolRole::kStatic) >= 0,
          "each pool is found by its provider and role" + at);
    Check(sa::Bytes(b) >= sa::Bytes(a) + 3 * 512ull * 512 * 8, "the lamps' tiles are inside the bytes" + at);
  }
  // ultra with the lamps: the large tiles first, the lamps' 512 tiles in seven columns under them (3840 x 3584: the 1280 tiles are three columns wide), not four columns in a texture three tiles wide
  const sa::Layout u = sa::LayoutForLevel(lb::Level::kUltra, 0, true, false);
  Check(u.width == 3840 && u.height == 3584 && u.pools[0].request.provider == sa::ProviderId::kReplay && u.pools[1].request.provider == sa::ProviderId::kLamps && u.pools[1].cols == 7 &&
            u.pools[1].rows == 2 && u.pools[1].y0 == 2560,
        "ultra with the lamps: 6 x 1280 tiles at the top, the lamps' 10 tiles of 512 in 7 columns (the width holds seven) below");
  Check(u.SmallestTile() == 512 && u.LargestTile() == 1280, "the two tile classes");
}

void TestLayoutFuzz() {
  Lcg rng;
  uint32_t made = 0;
  for (int n = 0; n < 3000; ++n) {
    sa::AtlasSpec spec;
    const uint32_t pools = 1 + rng.Int(4);
    for (uint32_t i = 0; i < pools; ++i) {
      static const uint32_t kTiles[] = {128, 256, 512, 1024};
      spec.Add({sa::ProviderId(rng.Int(3)), sa::PoolRole(rng.Int(3)), kTiles[rng.Int(4)], 1 + rng.Int(12), rng.Int(2) != 0});
    }
    const sa::Layout layout = sa::MakeLayout(spec);
    if (!layout.valid()) {
      continue;
    }
    ++made;
    const std::string bad = sa::ValidateLayout(layout);
    Check(bad.empty(), "fuzz: sound layout: " + bad);
    // every tile of every pool: inside, whole texels, no two the same, no two overlapping, the atlas-wide numbers contiguous
    std::vector<sa::Rect> all;
    bool inside = true, contiguous = true;
    uint32_t first = 0;
    for (uint32_t p = 0; p < layout.pool_count; ++p) {
      contiguous = contiguous && layout.pools[p].first_slot == first;
      first += layout.pools[p].request.slots;
      for (uint32_t s = 0; s < layout.pools[p].request.slots; ++s) {
        const sa::Rect r = layout.SlotRect(p, s);
        inside = inside && r.valid() && r.x + r.size <= layout.width && r.y + r.size <= layout.height && r.x % r.size == 0 && layout.GlobalSlotRect(layout.pools[p].first_slot + s) == r;
        all.push_back(r);
      }
    }
    Check(inside && contiguous && all.size() == layout.slot_count && layout.slot_count == spec.SlotCount(), "fuzz: tiles inside the texture, numbered one after the other");
    bool apart = true;
    for (size_t a = 0; a < all.size() && apart; ++a) {
      for (size_t b = a + 1; b < all.size(); ++b) {
        const bool sep = all[a].x + all[a].size <= all[b].x || all[b].x + all[b].size <= all[a].x || all[a].y + all[a].size <= all[b].y || all[b].y + all[b].size <= all[a].y;
        apart = apart && sep;
      }
    }
    Check(apart, "fuzz: no two tiles overlap");
    Check(!layout.SlotRect(layout.pool_count, 0).valid() && !layout.GlobalSlotRect(layout.slot_count).valid(), "fuzz: a tile past the end is empty");
  }
  Check(made > 2500, "fuzz: most specs make a layout (" + std::to_string(made) + ")");
  // a layout that is wrong is found
  sa::Layout l = sa::LayoutForLevel(lb::Level::kHigh, 0, true, false);
  l.pools[1].y0 = 0;
  Check(!sa::ValidateLayout(l).empty(), "validation finds two pools that overlap");
  l = sa::LayoutForLevel(lb::Level::kHigh, 0, true, false);
  l.height -= 512;
  Check(!sa::ValidateLayout(l).empty(), "validation finds a pool outside the texture");
  Check(sa::ValidateLayout(sa::Layout{}) == "empty", "validation finds an empty layout");
}

// ---------------------------------------------------------------------------
// fitting a level to the memory
// ---------------------------------------------------------------------------

void TestFit() {
  const uint64_t ultra = sa::Bytes(sa::LayoutForLevel(lb::Level::kUltra, 0));
  const uint64_t high = sa::Bytes(sa::LayoutForLevel(lb::Level::kHigh, 0));
  const uint64_t medium = sa::Bytes(sa::LayoutForLevel(lb::Level::kMedium, 0));
  int asked = 0;
  uint64_t last = 0;
  const auto room = [&](uint64_t limit) {
    return [&, limit](uint64_t extra) {
      ++asked;
      last = extra;
      return extra <= limit;
    };
  };
  sa::Fit f = sa::FitLevel(lb::Level::kUltra, 0, false, 0, room(~0ull));
  Check(f.level == lb::Level::kUltra && f.bytes == ultra && !f.stepped && asked == 1 && last == ultra, "room for ultra: ultra, asked once for all of it");
  asked = 0;
  f = sa::FitLevel(lb::Level::kUltra, 0, false, 0, room(high));
  Check(f.level == lb::Level::kHigh && f.bytes == high && f.stepped && asked == 2, "no room for ultra: high");
  asked = 0;
  f = sa::FitLevel(lb::Level::kUltra, 0, false, 0, room(medium));
  Check(f.level == lb::Level::kMedium && f.bytes == medium && f.stepped && asked == 3, "no room for high either: medium");
  asked = 0;
  f = sa::FitLevel(lb::Level::kUltra, 0, false, 0, room(1));
  Check(f.level == lb::Level::kOff && f.bytes == 0 && f.stepped && asked == 3, "no room for any: off");
  asked = 0;
  f = sa::FitLevel(lb::Level::kUltra, 0, false, ultra, room(0));
  Check(f.level == lb::Level::kUltra && !f.stepped && asked == 0, "what is held already is not asked for again");
  asked = 0;
  f = sa::FitLevel(lb::Level::kUltra, 0, false, high, room(ultra - high));
  Check(f.level == lb::Level::kUltra && last == ultra - high, "only what is new is asked: ultra over a held high asks the difference");
  asked = 0;
  f = sa::FitLevel(lb::Level::kOff, 0, false, 0, room(~0ull));
  Check(f.level == lb::Level::kOff && !f.stepped && asked == 0, "off stays off and asks nothing");
  f = sa::FitLevel(lb::Level::kMedium, 0, false, 0, room(1));
  Check(f.level == lb::Level::kOff && f.stepped, "medium that does not fit is off");
  // with the lamps' pools in, the answer counts them
  const uint64_t high_lamps = sa::Bytes(sa::LayoutForLevel(lb::Level::kHigh, 0, true, false));
  f = sa::FitLevel(lb::Level::kUltra, 0, true, 0, room(high_lamps));
  Check(f.level == lb::Level::kHigh && f.bytes == high_lamps, "the lamps' tiles count toward the fit");
}

// ---------------------------------------------------------------------------
// the one barrier, the clears
// ---------------------------------------------------------------------------

void TestBarrier() {
  Check((sa::kPublishStages & sa::kStageGraphics) != 0 && (sa::kPublishStages & sa::kStageCompute) != 0, "the publish names the pixel AND the compute stage");
  sa::UseTracker t;
  Check(t.use() == sa::Use::kUnknown, "a new texture is of unknown use");
  sa::Barrier b = t.BeginWrite();
  Check(b.needed && b.to == sa::Use::kDepthWrite && b.stages == sa::kStageGraphics, "before the pass: to depth write for graphics");
  b = t.BeginWrite();
  Check(!b.needed, "already writable: no second barrier");
  b = t.Publish();
  Check(b.needed && b.to == sa::Use::kShaderRead && b.stages == (sa::kStageGraphics | sa::kStageCompute) && t.use() == sa::Use::kShaderRead, "after the pass: to shader read for pixel and compute");
  b = t.BeginWrite();
  Check(b.needed && b.to == sa::Use::kDepthWrite, "the next frame's pass takes it back to depth write");
  // a pass that stops half way (the upload ring was full) still publishes: the map is partial, never unreadable
  b = t.Publish();
  Check(b.needed && b.stages == sa::kPublishStages, "a partial pass publishes with both stages too");
  b = t.Publish();
  Check(b.needed && b.stages == sa::kPublishStages, "publishing twice names both stages twice (never elided)");
  t.Reset();
  Check(t.use() == sa::Use::kUnknown && t.BeginWrite().needed, "a texture made again starts unknown");
  // every pass, from every state, ends published for both: the rule over all sequences of four calls
  for (uint32_t seq = 0; seq < 16; ++seq) {
    sa::UseTracker u;
    sa::Barrier last{};
    for (uint32_t k = 0; k < 4; ++k) {
      const bool publish = ((seq >> k) & 1u) != 0;
      last = publish ? u.Publish() : u.BeginWrite();
      if (publish) {
        Check(last.stages == sa::kPublishStages, "any publish names both stages (sequence " + std::to_string(seq) + ")");
      }
    }
  }
}

void TestClears() {
  Check(sa::kClearDepth == 1.0f && sa::kClearStencil == 0, "a clear is depth 1: nothing drawn here");
  const sa::Layout replay = sa::LayoutForLevel(lb::Level::kHigh, 0, false);
  sa::ClearPlan p = sa::MakeClearPlan(replay);
  Check(p.whole_target && p.rect_count == 0, "the replay alone: one clear of the whole texture");
  const sa::Layout live = sa::LayoutForLevel(lb::Level::kHigh, 0, true, false);
  p = sa::MakeClearPlan(live);
  Check(p.whole_target && p.rect_count == 0, "a live pool is drawn every record too: still one clear");
  const sa::Layout with_static = sa::LayoutForLevel(lb::Level::kHigh, 0, true, true);
  p = sa::MakeClearPlan(with_static);
  Check(!p.whole_target && p.rect_count == 2, "with a static pool: rectangles, the static pool is left");
  const int static_pool = with_static.FindPool(sa::ProviderId::kLamps, sa::PoolRole::kStatic);
  bool has_static = false;
  for (uint32_t i = 0; i < p.rect_count; ++i) {
    has_static = has_static || int(p.pools[i]) == static_pool;
    const sa::PoolLayout& pl = with_static.pools[p.pools[i]];
    Check(p.rects[i] == sa::Area{pl.x0, pl.y0, pl.cols * pl.request.tile, pl.rows * pl.request.tile}, "the rectangle is the pool's whole block");
  }
  Check(!has_static, "the static pool's map is kept");
  p = sa::MakeClearPlan(with_static, 1u << static_pool);
  Check(p.whole_target && p.rect_count == 0, "an invalid static pool is cleared with the rest: one clear");
  p = sa::MakeClearPlan(sa::Layout{});
  Check(!p.whole_target && p.rect_count == 0, "an empty layout clears nothing");
}

// ---------------------------------------------------------------------------
// slots that stay with a light
// ---------------------------------------------------------------------------

void TestStickySlots() {
  sa::StickySlots s(4);
  uint64_t wanted[4] = {11, 22, 33, 0};
  uint32_t slot[5];
  bool fresh[5];
  uint32_t held = s.Update(wanted, 3, nullptr, 0, slot, fresh);
  Check(held == 3 && slot[0] == 0 && slot[1] == 1 && slot[2] == 2 && fresh[0] && fresh[1] && fresh[2], "three lights take the first three slots, all new");
  const uint32_t before[3] = {slot[0], slot[1], slot[2]};
  uint64_t shuffled[3] = {33, 11, 22};
  held = s.Update(shuffled, 3, nullptr, 0, slot, fresh);
  Check(held == 3 && slot[0] == before[2] && slot[1] == before[0] && slot[2] == before[1] && !fresh[0] && !fresh[1] && !fresh[2], "a new rank changes no slot and nothing is new");
  // a light leaves the pick but fades: it keeps its slot, a newcomer takes the free one
  uint64_t now[3] = {33, 11, 44};
  uint64_t fading[1] = {22};
  held = s.Update(now, 3, fading, 1, slot, fresh);
  Check(held == 3 && slot[0] == 2 && slot[1] == 0 && slot[2] == 3 && fresh[2] && s.SlotOf(22) == 1, "the fading light keeps its slot; the newcomer takes the free one");
  // all four slots held, a new light wants one: the fading light's goes
  uint64_t now2[4] = {33, 11, 44, 55};
  held = s.Update(now2, 4, fading, 1, slot, fresh);
  Check(held == 4 && slot[3] == 1 && fresh[3] && s.SlotOf(22) == sa::kNoSlot, "a newcomer takes the worst fading holder's slot");
  // no fading light to take from: the newcomer waits, nobody that is wanted is moved
  uint64_t now3[5] = {33, 11, 44, 55, 66};
  held = s.Update(now3, 5, nullptr, 0, slot, fresh);
  Check(held == 4 && slot[4] == sa::kNoSlot && !fresh[4] && slot[0] == 2 && slot[1] == 0 && slot[2] == 3 && slot[3] == 1, "every slot wanted: the fifth waits and no one is moved");
  // a holder that is neither wanted nor fading lets go, and the slot is the next newcomer's
  uint64_t now4[3] = {33, 11, 66};
  held = s.Update(now4, 3, nullptr, 0, slot, fresh);
  Check(held == 3 && s.SlotOf(44) == sa::kNoSlot && s.SlotOf(55) == sa::kNoSlot && slot[2] != sa::kNoSlot && fresh[2], "who is gone lets go; the newcomer gets a freed slot");
  // the worst fading holder is the last of the fading list
  sa::StickySlots t(3);
  uint64_t a3[3] = {1, 2, 3};
  t.Update(a3, 3, nullptr, 0, slot, fresh);
  uint64_t w[1] = {9};
  uint64_t f3[3] = {2, 3, 1};  // best first: 1 is the worst
  uint32_t out[1];
  bool fr[1];
  t.Update(w, 1, f3, 3, out, fr);
  Check(out[0] == t.SlotOf(9) && t.SlotOf(1) == sa::kNoSlot && t.SlotOf(2) != sa::kNoSlot && t.SlotOf(3) != sa::kNoSlot, "the last of the fading list loses its slot first");
  // a resize takes everything back
  t.Resize(2);
  Check(t.Used() == 0 && t.slots() == 2 && t.SlotOf(2) == sa::kNoSlot, "a new layout: nobody holds a slot");
  // the id 0 is nobody
  uint64_t zero[2] = {0, 5};
  uint32_t z[2];
  held = t.Update(zero, 2, nullptr, 0, z, nullptr);
  Check(held == 1 && z[0] == sa::kNoSlot && z[1] != sa::kNoSlot, "the id 0 is no light");
}

void TestStickyFuzz() {
  Lcg rng;
  const uint32_t kSlots = 6;
  sa::StickySlots s(kSlots);
  std::array<uint64_t, 12> universe{};
  for (uint32_t i = 0; i < universe.size(); ++i) {
    universe[i] = 100 + i;
  }
  std::array<uint32_t, 12> last_slot;
  last_slot.fill(sa::kNoSlot);
  uint64_t moved = 0, wrong = 0;
  for (int frame = 0; frame < 4000; ++frame) {
    std::vector<uint64_t> wanted, fading;
    for (uint64_t id : universe) {
      const float r = rng.Next();
      if (r < 0.35f && wanted.size() < kSlots) {
        wanted.push_back(id);
      } else if (r < 0.5f) {
        fading.push_back(id);
      }
    }
    for (size_t i = wanted.size(); i > 1; --i) {
      std::swap(wanted[i - 1], wanted[rng.Int(uint32_t(i))]);
    }
    std::array<uint32_t, 16> slot{};
    std::array<bool, 16> fresh{};
    // the slots the wanted ones held before this frame
    std::vector<uint32_t> held_before;
    for (uint64_t id : wanted) {
      held_before.push_back(s.SlotOf(id));
    }
    const uint32_t held = s.Update(wanted.data(), uint32_t(wanted.size()), fading.data(), uint32_t(fading.size()), slot.data(), fresh.data());
    uint32_t count = 0;
    std::array<bool, kSlots> used{};
    for (size_t i = 0; i < wanted.size(); ++i) {
      if (slot[i] == sa::kNoSlot) {
        continue;  // refused: checked below (up to kSlots wanted, every one holds a slot)
      }
      ++count;
      wrong += slot[i] >= kSlots || used[slot[i]] ? 1u : 0u;
      if (slot[i] < kSlots) {
        used[slot[i]] = true;
      }
      wrong += s.SlotOf(wanted[i]) != slot[i] ? 1u : 0u;
      if (held_before[i] != sa::kNoSlot) {
        moved += held_before[i] != slot[i] ? 1u : 0u;      // a wanted holder never moves
        wrong += fresh[i] ? 1u : 0u;                          // and is not new
      } else {
        wrong += !fresh[i] ? 1u : 0u;                         // a light that had no slot has a new one
      }
    }
    wrong += count != held ? 1u : 0u;
    wrong += wanted.size() <= kSlots && count != wanted.size() ? 1u : 0u;  // up to kSlots wanted: every one holds a slot (fading holders give way)
    // nobody neither wanted nor fading holds a slot
    for (uint64_t id : universe) {
      const bool listed = std::find(wanted.begin(), wanted.end(), id) != wanted.end() || std::find(fading.begin(), fading.end(), id) != fading.end();
      wrong += !listed && s.SlotOf(id) != sa::kNoSlot ? 1u : 0u;
    }
    // no two ids in one slot
    std::array<int, kSlots> owners{};
    for (uint64_t id : universe) {
      const uint32_t sl = s.SlotOf(id);
      if (sl != sa::kNoSlot && sl < kSlots) {
        ++owners[sl];
      }
    }
    for (int o : owners) {
      wrong += o > 1 ? 1u : 0u;
    }
  }
  Check(moved == 0, "fuzz: a wanted light that holds a slot never moves (" + std::to_string(moved) + ")");
  Check(wrong == 0, "fuzz: slots are unique, wanted lights up to the slot count all hold one, a leaver lets go (" + std::to_string(wrong) + " faults)");
}

// ---------------------------------------------------------------------------
// the published form
// ---------------------------------------------------------------------------

lg::LocalShadowView MakeView(Lcg* rng, uint32_t light_index, bool valid = true) {
  lg::LocalShadowView v{};
  for (int i = 0; i < 16; ++i) {
    v.world_to_shadow[i] = rng->Range(-2.0f, 2.0f);
  }
  for (int i = 0; i < 4; ++i) {
    v.atlas_rect[i] = rng->Next();
  }
  v.depth_bias = 0.002f;
  v.texel_size = 1.0f / 512.0f;
  v.light_index = light_index;
  v.valid = valid ? 1u : 0u;
  return v;
}

lg::ShadowAtlasInfo MakeInfo(uint32_t slot = 41) {
  lg::ShadowAtlasInfo a;
  a.valid = true;
  a.slot = slot;
  a.sampler = 2;
  a.width = 1536;
  a.height = 512;
  a.tile_small = 512;
  return a;
}

uint64_t CarId(uint32_t key) { return lg::light_id::CarLampId(key, lg::light_id::CarLamp::kPair); }

void TestBuilderIsShadowFrameFromLocal() {
  Lcg rng;
  uint32_t grouped_frames = 0, with_nobody = 0;
  for (int n = 0; n < 4000; ++n) {
    const uint32_t lights = 1 + rng.Int(4);
    std::array<uint64_t, 4> ids{};
    for (uint32_t i = 0; i < lights; ++i) {
      ids[i] = CarId(10 + i * 7);
    }
    lg::LocalShadowFrame local;
    const uint32_t views = rng.Int(9);
    for (uint32_t v = 0; v < views; ++v) {
      local.views[local.count++] = MakeView(&rng, rng.Int(lights), rng.Next() < 0.85f);
    }
    const lg::ShadowAtlasInfo atlas = MakeInfo();
    const lg::ShadowFrame legacy = lg::ShadowFrameFromLocal(local, atlas, ids.data(), lights);
    sa::ShadowFrameBuilder b;
    for (uint32_t v = 0; v < local.count; ++v) {
      b.AddView(local.views[v], ids[local.views[v].light_index]);
    }
    const sa::Published made = b.Build(atlas);
    const std::string diff = sa::DiffShadowFrames(legacy, made.frame);
    Check(diff.empty(), "the builder makes ShadowFrameFromLocal's frame: " + diff);
    for (uint32_t e = 0; e < made.frame.count; ++e) {
      with_nobody += made.frame.view_light[e] == lg::kNoLightId ? 1u : 0u;
    }
    grouped_frames += made.frame.count != 0 ? 1u : 0u;
  }
  Check(grouped_frames > 3000 && with_nobody > 100, "the random frames had views and views that count for nobody (" + std::to_string(grouped_frames) + ", " + std::to_string(with_nobody) + ")");
  // layers and masks
  Lcg r2;
  sa::ShadowFrameBuilder b;
  b.AddView(MakeView(&r2, 0), CarId(1));
  b.AddView(MakeView(&r2, 0), 77, lg::ShadowLayer::kStatic);
  b.AddView(MakeView(&r2, 0), 77, lg::ShadowLayer::kLive);
  b.AddView(MakeView(&r2, 1), CarId(1));
  b.AddMask(CarId(1), 3, 0.5f);
  b.AddMask(lg::kNoLightId, 1, 1.0f);
  const sa::Published p = b.Build(MakeInfo());
  Check(p.frame.count == 4 && p.frame.view_light[0] == CarId(1) && p.frame.view_light[1] == CarId(1) && p.frame.view_light[2] == 77 && p.frame.view_light[3] == 77, "views of a light are consecutive, a light's first view first");
  Check(p.frame.view_layer[2] == lg::ShadowLayer::kStatic && p.frame.view_layer[3] == lg::ShadowLayer::kLive, "a static entry is followed by its live twin");
  Check(p.mask_count == 1 && p.masks[0].light == CarId(1) && p.masks[0].channel == 3, "a mask of no light is refused, the other kept");
  // a ninth view is refused
  sa::ShadowFrameBuilder full;
  for (uint32_t i = 0; i < lg::kMaxLightShadowViews; ++i) {
    Check(full.AddView(MakeView(&r2, 0), CarId(i + 1)), "eight views fit");
  }
  Check(!full.AddView(MakeView(&r2, 0), CarId(9)), "a ninth does not");
  // a view of a light with no id is a view that counts for nobody: the tail
  sa::ShadowFrameBuilder tail;
  tail.AddView(MakeView(&r2, 0), lg::kNoLightId);
  tail.AddView(MakeView(&r2, 1), CarId(2));
  const sa::Published tp = tail.Build(MakeInfo());
  Check(tp.frame.count == 2 && tp.frame.view_light[0] == CarId(2) && tp.frame.view_light[1] == lg::kNoLightId, "a view with no light goes to the tail");
}

void TestRebindIsTheKeyRemap() {
  Lcg rng;
  uint64_t views_moved = 0, dropped = 0;
  for (int n = 0; n < 6000; ++n) {
    // the set the views were made for: up to four cars with distinct keys, in some order
    std::array<uint32_t, 6> pool = {5, 6, 7, 8, 9, 10};
    for (size_t i = pool.size(); i > 1; --i) {
      std::swap(pool[i - 1], pool[rng.Int(uint32_t(i))]);
    }
    const uint32_t old_count = 1 + rng.Int(4);
    std::array<uint32_t, 4> old_keys{};
    std::array<uint64_t, 4> old_ids{};
    for (uint32_t i = 0; i < old_count; ++i) {
      old_keys[i] = pool[i];
      old_ids[i] = CarId(pool[i]);
    }
    lg::LocalShadowFrame local;
    const uint32_t views = rng.Int(9);
    for (uint32_t v = 0; v < views && local.count < local.views.size(); ++v) {
      // views come a light's two together, as the replay publishes them
      const uint32_t light = rng.Int(old_count);
      local.views[local.count++] = MakeView(&rng, light, rng.Next() < 0.9f);
      if (rng.Next() < 0.4f && local.count < local.views.size()) {
        local.views[local.count++] = MakeView(&rng, light, rng.Next() < 0.9f);
      }
    }
    const lg::ShadowAtlasInfo atlas = MakeInfo();
    // the set that reads them: other cars, another order, some gone, some new
    for (size_t i = pool.size(); i > 1; --i) {
      std::swap(pool[i - 1], pool[rng.Int(uint32_t(i))]);
    }
    const uint32_t new_count = 1 + rng.Int(4);
    std::array<uint32_t, 4> new_keys{};
    std::array<uint64_t, 4> new_ids{};
    for (uint32_t i = 0; i < new_count; ++i) {
      new_keys[i] = pool[i];
      new_ids[i] = CarId(pool[i]);
    }
    // legacy: the rig's key remap of the replay's frame, then the manager's conversion with the new set's ids
    const lg::LocalShadowFrame moved = hr::RemapShadowViews(local, old_keys.data(), old_count, new_keys.data(), new_count);
    lg::ShadowFrame legacy;
    if (moved.count != 0) {
      legacy = lg::ShadowFrameFromLocal(moved, atlas, new_ids.data(), new_count);
    } else {
      legacy = lg::ShadowFrame{};
      legacy.atlas = atlas;
    }
    // the atlas: the published frame of the old set (ids), read by id for the new set
    sa::ShadowFrameBuilder b;
    for (uint32_t v = 0; v < local.count; ++v) {
      b.AddView(local.views[v], old_ids[local.views[v].light_index]);
    }
    const sa::Published made = b.Build(atlas);
    const lg::ShadowFrame rebound = sa::RebindByIds(made.frame, new_ids.data(), new_count);
    const std::string diff = sa::DiffShadowFrames(legacy, rebound);
    Check(diff.empty(), "RebindByIds makes what the key remap and the conversion make: " + diff);
    views_moved += moved.count;
    dropped += local.count - moved.count;
  }
  Check(views_moved > 3000 && dropped > 1000, "the sets had views that moved and views that were dropped (" + std::to_string(views_moved) + ", " + std::to_string(dropped) + ")");
  // a view whose light is not in the new set is dropped, a third view of a light is not read, invalid views never move
  Lcg r2;
  sa::ShadowFrameBuilder b;
  b.AddView(MakeView(&r2, 0), CarId(1));
  b.AddView(MakeView(&r2, 0), CarId(1));
  b.AddView(MakeView(&r2, 1), CarId(2));
  const sa::Published p = b.Build(MakeInfo());
  const uint64_t only_two[1] = {CarId(2)};
  const lg::ShadowFrame r = sa::RebindByIds(p.frame, only_two, 1);
  Check(r.count == 1 && r.view_light[0] == CarId(2) && r.views[0].light_index == 0 && r.atlas.slot == 41, "only the car that is still there keeps its view, as light 0 of the new set");
  const lg::ShadowFrame none = sa::RebindByIds(p.frame, nullptr, 0);
  Check(none.count == 0 && none.atlas.valid && none.atlas.slot == 41, "no lights: no views, the atlas is still named");
}

// every part of a published frame, changed on purpose, is found by the comparison the manager's self-check makes
void TestDiffFindsEveryField() {
  Lcg rng;
  sa::ShadowFrameBuilder b;
  b.AddView(MakeView(&rng, 0), CarId(1));
  b.AddView(MakeView(&rng, 0), CarId(1));
  b.AddView(MakeView(&rng, 1), CarId(2));
  const lg::ShadowFrame base = b.Build(MakeInfo()).frame;
  Check(sa::DiffShadowFrames(base, base).empty(), "a frame is the same as itself");
  const auto found = [&](const std::string& what, const std::function<void(lg::ShadowFrame*)>& mutate) {
    lg::ShadowFrame m = base;
    mutate(&m);
    Check(!sa::DiffShadowFrames(base, m).empty(), "the comparison finds " + what);
  };
  found("a view dropped", [](lg::ShadowFrame* m) { --m->count; });
  found("an atlas that is not valid", [](lg::ShadowFrame* m) { m->atlas.valid = false; });
  found("another atlas slot", [](lg::ShadowFrame* m) { m->atlas.slot += 1; });
  found("another sampler", [](lg::ShadowFrame* m) { m->atlas.sampler += 1; });
  found("another atlas width", [](lg::ShadowFrame* m) { m->atlas.width += 1; });
  found("another atlas height", [](lg::ShadowFrame* m) { m->atlas.height += 1; });
  found("another small tile", [](lg::ShadowFrame* m) { m->atlas.tile_small += 1; });
  found("another large tile", [](lg::ShadowFrame* m) { m->atlas.tile_large += 1; });
  found("another light for a view", [](lg::ShadowFrame* m) { m->view_light[1] = CarId(9); });
  found("another layer", [](lg::ShadowFrame* m) { m->view_layer[2] = lg::ShadowLayer::kLive; });
  found("two views in the other order", [](lg::ShadowFrame* m) { std::swap(m->views[0], m->views[2]); });
  for (int k = 0; k < 16; ++k) {
    found("a matrix entry " + std::to_string(k), [k](lg::ShadowFrame* m) { m->views[1].world_to_shadow[k] += 1.0f; });
  }
  for (int k = 0; k < 4; ++k) {
    found("a rectangle entry " + std::to_string(k), [k](lg::ShadowFrame* m) { m->views[0].atlas_rect[k] += 0.25f; });
  }
  found("the bias", [](lg::ShadowFrame* m) { m->views[0].depth_bias += 0.001f; });
  found("the texel size", [](lg::ShadowFrame* m) { m->views[0].texel_size += 0.001f; });
  found("the light index", [](lg::ShadowFrame* m) { m->views[2].light_index += 1; });
  found("the valid flag", [](lg::ShadowFrame* m) { m->views[2].valid = 0; });
  found("a view past the count", [](lg::ShadowFrame* m) { m->views[5].valid = 1; });
  // and the rebind is not a copy: a view of a light that left, or moved to another place, is not the same frame
  const uint64_t two[2] = {CarId(2), CarId(1)};
  const lg::ShadowFrame swapped = sa::RebindByIds(base, two, 2);
  Check(!sa::DiffShadowFrames(base, swapped).empty() && swapped.views[0].light_index == 1 && swapped.views[2].light_index == 0, "a light that moved in the set has its views' light_index moved with it");
}

void TestInfoAndMasks() {
  const sa::Layout one = sa::LayoutForLevel(lb::Level::kUltra, 0);
  lg::ShadowAtlasInfo i = sa::InfoOf(one, 7, 2, true);
  Check(i.valid && i.slot == 7 && i.sampler == 2 && i.width == 3840 && i.height == 2560 && i.tile_small == 1280 && i.tile_large == 0, "one tile size: tile_small only, as the manager's AtlasOf made it");
  const sa::Layout two = sa::LayoutForLevel(lb::Level::kUltra, 0, true, false);
  i = sa::InfoOf(two, 7, 2, true);
  Check(i.tile_small == 512 && i.tile_large == 1280, "two sizes: small 512, large 1280");
  Check(!sa::InfoOf(two, 7, 2, false).valid && !sa::InfoOf(sa::Layout{}, 7, 2, true).valid, "no valid atlas without a layout or the flag");
  // the ray traced masks bind to lights that have no atlas views
  lg::LightFrame frame;
  frame.count = 2;
  frame.lights[0].id = CarId(1);
  frame.lights[1].id = CarId(2);
  frame.lights[1].shadow.kind = lg::ShadowKind::kAtlas;
  sa::Published p;
  p.masks[0] = {CarId(1), 5, 0.75f};
  p.masks[1] = {CarId(2), 6, 1.0f};
  p.masks[2] = {12345, 1, 1.0f};
  p.mask_count = 3;
  sa::BindMaskRefs(&frame, p);
  Check(frame.lights[0].shadow.kind == lg::ShadowKind::kRtMask && frame.lights[0].shadow.mask_channel == 5 && frame.lights[0].shadow.weight == 0.75f, "a light with no atlas view reads its mask channel");
  Check(frame.lights[1].shadow.kind == lg::ShadowKind::kAtlas, "a light that has atlas views keeps them");
}

// a provider that asks for nothing, one that asks for tiles: the interface is usable by both
class MaskOnlyProvider final : public mcla::gpu::ShadowProvider {
 public:
  sa::ProviderId provider_id() const override { return sa::ProviderId::kRtMask; }
  void RequestPools(sa::AtlasSpec*) const override {}
  void CollectViews(sa::ShadowFrameBuilder* b) const override { b->AddMask(CarId(1), 2, 1.0f); }
};
class TilesProvider final : public mcla::gpu::ShadowProvider {
 public:
  sa::ProviderId provider_id() const override { return sa::ProviderId::kReplay; }
  void RequestPools(sa::AtlasSpec* spec) const override { spec->Add({sa::ProviderId::kReplay, sa::PoolRole::kSingle, 512, 3, false}); }
  void OnLayout(const sa::Layout& l) override { layout = l; }
  void CollectViews(sa::ShadowFrameBuilder* b) const override {
    lg::LocalShadowView v{};
    v.valid = 1;
    b->AddView(v, CarId(3));
  }
  sa::Layout layout;
};
void TestProviderInterface() {
  MaskOnlyProvider masks;
  TilesProvider tiles;
  mcla::gpu::ShadowProvider* providers[2] = {&tiles, &masks};
  sa::AtlasSpec spec;
  sa::ShadowFrameBuilder builder;
  for (mcla::gpu::ShadowProvider* p : providers) {
    p->RequestPools(&spec);
  }
  const sa::Layout layout = sa::MakeLayout(spec);
  for (mcla::gpu::ShadowProvider* p : providers) {
    p->OnLayout(layout);
    p->CollectViews(&builder);
  }
  const sa::Published pub = builder.Build(sa::InfoOf(layout, 3, 2, true));
  Check(spec.count == 1 && layout.slot_count == 3 && tiles.layout == layout, "the tile provider's pool is laid out and it is told");
  Check(pub.frame.count == 1 && pub.frame.view_light[0] == CarId(3) && pub.mask_count == 1 && pub.masks[0].channel == 2, "one published frame holds a tile provider's view and a mask provider's channel");
  Check(std::string(sa::ProviderName(sa::ProviderId::kLamps)) == "street lamps" && sa::kProviderCount == 3, "the three providers have names");
}

// ---------------------------------------------------------------------------
// the lookup
// ---------------------------------------------------------------------------

// A stored depth for texel (ix, iy) of a test atlas: a quarter of the texels are "nothing drawn" (1), the rest are depths in [0, 1); the index is clamped into the atlas as a sampler's is.
struct TestAtlas {
  uint32_t width = 1536, height = 512, seed = 1;
  float At(int ix, int iy) const {
    ix = std::clamp(ix, 0, int(width) - 1);
    iy = std::clamp(iy, 0, int(height) - 1);
    uint32_t h = uint32_t(ix) * 73856093u ^ uint32_t(iy) * 19349663u ^ seed * 83492791u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return (h & 3u) == 0 ? 1.0f : float(h >> 8 & 0xFFFFu) / 65536.0f;
  }
};

// A real perspective view of a test tile: it looks along +z from the origin, w = z, and
//   su = ox + fx x / z    sv = oy + fy y / z    sz = A (1 - n / z)   (A = f / (f - n): the replay's depth, 0 at the near plane n, 1 at the far plane f)
// with the tile's centre (ox, 0.5) and a field of 70 degrees (tan 35 degrees = 0.7). Row 3 is the unit axis and row 2 is A times it, as light_shadow_filter.h reads a view. Tile k of three in an
// atlas of 1536 x 512.
constexpr float kTestNear = 0.1f, kTestFar = 100.0f;
constexpr float kTestA = kTestFar / (kTestFar - kTestNear);
lg::LocalShadowView TileView(uint32_t tile_index, float bias, bool valid = true) {
  lg::LocalShadowView v{};
  const float u0 = float(tile_index) / 3.0f, u1 = float(tile_index + 1) / 3.0f;
  const float fx = 0.5f * (u1 - u0) / 0.7f, fy = 0.5f / 0.7f;
  v.world_to_shadow[0] = fx;
  v.world_to_shadow[2] = 0.5f * (u0 + u1);
  v.world_to_shadow[5] = fy;
  v.world_to_shadow[6] = 0.5f;
  v.world_to_shadow[10] = kTestA;
  v.world_to_shadow[11] = -kTestA * kTestNear;
  v.world_to_shadow[14] = 1.0f;
  v.atlas_rect[0] = u0;
  v.atlas_rect[1] = 0.0f;
  v.atlas_rect[2] = u1;
  v.atlas_rect[3] = 1.0f;
  v.depth_bias = bias;
  v.texel_size = 1.0f / 512.0f;
  v.light_index = tile_index;
  v.valid = valid ? 1u : 0u;
  return v;
}

// A view whose depth is the constant `depth` for every point at z = 1 (row 2 = (0, 0, 0, depth), w = z): the edges of the depth range exactly (sz = depth / 1), which a perspective's own arithmetic
// does not hold to the last bit. The world point (x, y, 1) is at su = ox + fx x, sv = oy + fy y.
lg::LocalShadowView FlatDepthView(uint32_t tile_index, float bias, float depth) {
  lg::LocalShadowView v = TileView(tile_index, bias);
  v.world_to_shadow[10] = 0.0f;
  v.world_to_shadow[11] = depth;
  return v;
}

// The world point of the view `v` (TileView) that is at the tile's atlas place (su, sv) with the depth sz: the perspective inverted.
void PointAt(const lg::LocalShadowView& v, float su, float sv, float sz, float p[3]) {
  const float z = kTestA * kTestNear / (kTestA - sz);
  p[0] = (su - v.world_to_shadow[2]) * z / v.world_to_shadow[0];
  p[1] = (sv - v.world_to_shadow[6]) * z / v.world_to_shadow[5];
  p[2] = z;
}

// A random point in or around the tile: mostly inside (so the kernels run), some outside the rectangle, some outside the depth range.
void RandomPoint(Lcg* rng, const lg::LocalShadowView& v, float p[3]) {
  const float u0 = v.atlas_rect[0], u1 = v.atlas_rect[2];
  const float su = u0 + (u1 - u0) * rng->Range(-0.08f, 1.08f);
  const float sv = rng->Range(-0.05f, 1.05f);
  const float sz = rng->Next() < 0.05f ? rng->Range(-0.2f, 1.2f) : rng->Range(0.02f, 0.98f);
  PointAt(v, su, sv, sz, p);
}

// A random unit normal of a surface: any direction (the lookup does not care which side faces the light; the plane is the same).
void RandomNormal(Lcg* rng, float n[3]) {
  const float z = rng->Range(-1.0f, 1.0f);
  const float a = rng->Range(0.0f, 6.2831853f);
  const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
  n[0] = r * std::cos(a);
  n[1] = z;
  n[2] = r * std::sin(a);
}

// light_shadow_filter.h ViewLit (the variant's CPU twin, held to the variant's HLSL text by headlight_test.cpp) behind the one lookup's own rule for an invalid view: the surface mode.
float ReferenceSurface(const lg::LocalShadowView& v, const float p[3], const float normal[3], const TestAtlas& atlas) {
  if (v.valid == 0) {
    return 1.0f;
  }
  const auto gather = [&](int ix, int iy) {
    sf::Quad q;
    q.x = atlas.At(ix, iy + 1);
    q.y = atlas.At(ix + 1, iy + 1);
    q.z = atlas.At(ix + 1, iy);
    q.w = atlas.At(ix, iy);
    return q;
  };
  return sf::ViewLit(v, p, normal, gather);
}

void TestSurfaceModeIsTheVariantsTwin() {
  Lcg rng;
  uint64_t inside = 0, soft = 0, lit_all = 0, dark_all = 0, faced = 0;
  for (int n = 0; n < 30000; ++n) {
    TestAtlas atlas;
    atlas.seed = 1 + rng.Int(50);
    const lg::LocalShadowView v = TileView(rng.Int(3), rng.Next() < 0.5f ? 0.002f : rng.Range(0.0f, 0.02f), rng.Next() < 0.97f);
    float p[3];
    RandomPoint(&rng, v, p);
    float normal[3];
    RandomNormal(&rng, normal);
    const float want = ReferenceSurface(v, p, normal, atlas);
    const float got = sl::ViewLit(v, p, normal, atlas.width, atlas.height, sl::Mode::kSurface, [&](int x, int y) { return atlas.At(x, y); });
    Check(std::memcmp(&want, &got, sizeof(float)) == 0, "the surface mode is light_shadow_filter.h ViewLit behind the same rule for an invalid view, bit for bit");
    // the overload without a normal looks at a receiver that faces the light: the view's axis, reversed
    const float facing[3] = {-v.world_to_shadow[12], -v.world_to_shadow[13], -v.world_to_shadow[14]};
    const float want_facing = ReferenceSurface(v, p, facing, atlas);
    const float got_facing = sl::ViewLit(v, p, atlas.width, atlas.height, sl::Mode::kSurface, [&](int x, int y) { return atlas.At(x, y); });
    faced += std::memcmp(&want_facing, &got_facing, sizeof(float)) == 0 ? 1u : 0u;
    inside += v.valid != 0 && want != 1.0f ? 1u : 0u;
    soft += got > 0.0f && got < 1.0f ? 1u : 0u;
    lit_all += got == 1.0f ? 1u : 0u;
    dark_all += got < 0.25f ? 1u : 0u;  // mostly in shadow (the contact hardening kernel leaves an exact 0 rare on random maps)
  }
  Check(faced == 30000, "no normal: a receiver that faces the light (the view's axis reversed), bit for bit");
  Check(inside > 5000 && soft > 500 && lit_all > 3000 && dark_all > 500, "the random points reach the kernel, its soft part, lit areas and (mostly) shadows (" + std::to_string(inside) + ", " + std::to_string(soft) + ", " +
                                                                                  std::to_string(lit_all) + ", " + std::to_string(dark_all) + ")");
}

void TestAirModeIsTheVolumesTwin() {
  Lcg rng;
  uint64_t inside = 0, soft = 0;
  for (int n = 0; n < 30000; ++n) {
    TestAtlas atlas;
    atlas.seed = 1 + rng.Int(50);
    const lg::LocalShadowView v = TileView(rng.Int(3), rng.Next() < 0.5f ? 0.002f : rng.Range(0.0f, 0.02f), rng.Next() < 0.97f);
    float p[3];
    RandomPoint(&rng, v, p);
    const vol::VolShadowView vv = lg::ToVolShadowView<vol::VolShadowView>(v);
    const float want = vol::ShadowShare(vv, p, int(atlas.width), int(atlas.height), [&](int x, int y) { return atlas.At(x, y); });
    const float got = sl::ViewLit(v, p, atlas.width, atlas.height, sl::Mode::kAir, [&](int x, int y) { return atlas.At(x, y); });
    Check(std::memcmp(&want, &got, sizeof(float)) == 0, "the air mode is volumetrics_rules.h ShadowShare, bit for bit");
    inside += want != 1.0f ? 1u : 0u;
    soft += want > 0.0f && want < 1.0f ? 1u : 0u;
  }
  Check(inside > 5000 && soft > 2000, "the random points reach the bilinear weights (" + std::to_string(inside) + ", " + std::to_string(soft) + ")");
}

void TestModeEdges() {
  // the two live copies disagree on exactly these: a receiver at depth exactly 0 or 1. The surface mode looks it up, the air mode calls it lit. (A stored 1 is lit in both.)
  // The views here have a constant depth (FlatDepthView: sz = depth for every point at z = 1), so that the edges of the range are exact: the world point (0, 0, 1) is the tile's centre.
  const float centre[3] = {0.0f, 0.0f, 1.0f};
  const lg::LocalShadowView at_far_view = FlatDepthView(1, 0.002f, 1.0f);
  const lg::LocalShadowView at_near_view = FlatDepthView(1, 0.002f, 0.0f);
  const lg::LocalShadowView inside_view = FlatDepthView(1, 0.002f, 0.5f);
  // a texel pattern that is a shadow at any depth that matters: stored 0.1 everywhere around the place
  const auto depth = [&](int, int) { return 0.1f; };
  Check(sl::ViewLit(at_far_view, centre, 1536, 512, sl::Mode::kSurface, depth) == 0.0f, "surface: a receiver at depth 1 is looked up (the variant's `s.z > 1.0`)");
  Check(sl::ViewLit(at_far_view, centre, 1536, 512, sl::Mode::kAir, depth) == 1.0f, "air: a receiver at depth 1 is lit (the volume's `depth < 1.0`)");
  Check(sl::ViewLit(at_near_view, centre, 1536, 512, sl::Mode::kSurface, depth) == 1.0f, "surface: depth 0, stored 0.1 >= 0 - bias: lit by the compare");
  Check(sl::ViewLit(at_near_view, centre, 1536, 512, sl::Mode::kAir, depth) == 1.0f, "air: depth 0 is lit");
  Check(sl::ViewLit(inside_view, centre, 1536, 512, sl::Mode::kSurface, depth) == 0.0f && sl::ViewLit(inside_view, centre, 1536, 512, sl::Mode::kAir, depth) == 0.0f, "stored 0.1 in front of 0.5: shadow in both");
  // beyond the range in depth (the surface mode's [0, 1] is inclusive): lit in both
  const lg::LocalShadowView beyond_view = FlatDepthView(1, 0.002f, 1.5f);
  const lg::LocalShadowView before_view = FlatDepthView(1, 0.002f, -0.5f);
  Check(sl::ViewLit(beyond_view, centre, 1536, 512, sl::Mode::kSurface, depth) == 1.0f && sl::ViewLit(beyond_view, centre, 1536, 512, sl::Mode::kAir, depth) == 1.0f, "a depth past the far plane is lit");
  Check(sl::ViewLit(before_view, centre, 1536, 512, sl::Mode::kSurface, depth) == 1.0f && sl::ViewLit(before_view, centre, 1536, 512, sl::Mode::kAir, depth) == 1.0f, "a depth before the near plane is lit");
  // behind the view, outside the rectangle, invalid: lit
  lg::LocalShadowView behind = inside_view;
  behind.world_to_shadow[15] = -1000.0f;
  Check(sl::ViewLit(behind, centre, 1536, 512, sl::Mode::kSurface, depth) == 1.0f && sl::ViewLit(behind, centre, 1536, 512, sl::Mode::kAir, depth) == 1.0f, "a point behind the view is lit");
  const float outside[3] = {(0.1f - inside_view.world_to_shadow[2]) / inside_view.world_to_shadow[0], 0.0f, 1.0f};
  Check(sl::ViewLit(inside_view, outside, 1536, 512, sl::Mode::kSurface, depth) == 1.0f && sl::ViewLit(inside_view, outside, 1536, 512, sl::Mode::kAir, depth) == 1.0f, "a point outside the tile's rectangle is lit");
  lg::LocalShadowView invalid = inside_view;
  invalid.valid = 0;
  Check(sl::ViewLit(invalid, centre, 1536, 512, sl::Mode::kSurface, depth) == 1.0f && sl::ViewLit(invalid, centre, 1536, 512, sl::Mode::kAir, depth) == 1.0f, "an invalid view is lit");
  // a stored 1 is nothing drawn: lit in both whatever the bias
  const auto nothing = [](int, int) { return 1.0f; };
  Check(sl::ViewLit(inside_view, centre, 1536, 512, sl::Mode::kSurface, nothing) == 1.0f && sl::ViewLit(inside_view, centre, 1536, 512, sl::Mode::kAir, nothing) == 1.0f, "nothing drawn there: lit");
  // the surface mode fades the shadow out over the last 5 percent of the tile (light_shadow_filter.h kBorderFade): at the tile's edge a full shadow is gone, a share inside it is not
  {
    const float edge[3] = {(inside_view.atlas_rect[0] + 0.001f - inside_view.world_to_shadow[2]) / inside_view.world_to_shadow[0], 0.0f, 1.0f};
    const float near_edge[3] = {(inside_view.atlas_rect[0] + 0.5f * sf::kBorderFade * (inside_view.atlas_rect[2] - inside_view.atlas_rect[0]) - inside_view.world_to_shadow[2]) / inside_view.world_to_shadow[0], 0.0f, 1.0f};
    const float at_edge = sl::ViewLit(inside_view, edge, 1536, 512, sl::Mode::kSurface, depth);
    const float half_way = sl::ViewLit(inside_view, near_edge, 1536, 512, sl::Mode::kSurface, depth);
    Check(at_edge > 0.9f && half_way > 0.3f && half_way < 0.7f && sl::ViewLit(inside_view, centre, 1536, 512, sl::Mode::kSurface, depth) == 0.0f,
          "the surface mode's shadow fades out over the last 5 percent of the tile (edge " + std::to_string(at_edge) + ", half way " + std::to_string(half_way) + ")");
  }
  // the air mode never reads a texel of another tile: the neighbours are poison
  std::vector<uint8_t> read_outside(1, 0);
  const auto poison = [&](int x, int y) {
    const bool in_tile = x >= 512 && x <= 1023 && y >= 0 && y <= 511;
    read_outside[0] |= in_tile ? 0 : 1;
    return in_tile ? 0.5f : -1.0f;
  };
  Lcg rng;
  const lg::LocalShadowView poison_view = FlatDepthView(1, 0.002f, 0.9f);
  for (int n = 0; n < 2000; ++n) {
    float p[3] = {rng.Range(-1.4f, 1.4f), rng.Range(-1.4f, 1.4f), 1.0f};
    sl::ViewLit(poison_view, p, 1536, 512, sl::Mode::kAir, poison);
  }
  Check(read_outside[0] == 0, "the air mode's taps stay inside the view's tile, at its edges too");
}

void TestLightLitIsTheCombineRule() {
  Lcg rng;
  uint64_t two_views = 0, pairs = 0, weighted = 0;
  for (int n = 0; n < 8000; ++n) {
    TestAtlas atlas;
    atlas.seed = 1 + rng.Int(40);
    lg::ShadowFrame frame;
    frame.atlas = MakeInfo();
    // a light with 1 or 2 views (one per headlamp), or a static + live pair (a lamp), possibly one of them invalid
    lg::ShadowRef ref;
    ref.kind = rng.Next() < 0.9f ? lg::ShadowKind::kAtlas : (rng.Next() < 0.5f ? lg::ShadowKind::kRtMask : lg::ShadowKind::kNone);
    ref.first_view = uint8_t(rng.Int(3));
    ref.view_count = uint8_t(rng.Int(3));
    ref.weight = rng.Next() < 0.5f ? 1.0f : rng.Next();
    const bool pair = rng.Next() < 0.3f;
    for (uint32_t e = 0; e < 8; ++e) {
      frame.views[e] = TileView(e % 3, 0.002f, rng.Next() < 0.92f);
    }
    frame.count = 8;
    if (pair) {
      frame.view_layer[ref.first_view] = lg::ShadowLayer::kStatic;
      frame.view_layer[ref.first_view + 1] = lg::ShadowLayer::kLive;
      ref.view_count = 2;
      ++pairs;
    }
    float p[3];
    RandomPoint(&rng, frame.views[ref.first_view % 8], p);
    float normal[3];
    RandomNormal(&rng, normal);
    for (const sl::Mode mode : {sl::Mode::kSurface, sl::Mode::kAir}) {
      const auto depth = [&](int x, int y) { return atlas.At(x, y); };
      float visibility[lg::kMaxLightShadowViews] = {};
      for (uint32_t e = 0; e < 8; ++e) {
        visibility[e] = sl::ViewLit(frame.views[e], p, normal, atlas.width, atlas.height, mode, depth);
      }
      const float want = ll::CombineShadow(frame, ref, visibility);
      const float got = sl::LightLit(frame, ref, p, normal, mode, depth);
      CheckNear(got, want, 1e-6, "LightLit is light_laws.h CombineShadow over the lookups");
      if (ref.weight >= 1.0f && ref.kind == lg::ShadowKind::kAtlas && ref.view_count == 2 && !pair) {
        ++two_views;
      }
      weighted += ref.weight < 1.0f && ref.kind == lg::ShadowKind::kAtlas ? 1u : 0u;
    }
  }
  Check(two_views > 300 && pairs > 1500 && weighted > 2000, "the random lights had two views, static + live pairs and weights (" + std::to_string(two_views) + ", " + std::to_string(pairs) + ", " + std::to_string(weighted) + ")");
  // weight 1 is the mean itself: the live readers' `seen / views`, with no rounding of 1 + (m - 1)
  Check(sl::Blend(0.3f, 1.0f) == 0.3f && sl::Blend(0.3f, 0.0f) == 1.0f && std::fabs(sl::Blend(0.3f, 0.5f) - 0.65f) < 1e-6f, "the blend is exact at weight 1 and lit at weight 0");
  // the volume's mean of two views: 0.5 * (a + b), the variant's seen / views: the same number
  for (int n = 0; n < 1000; ++n) {
    const float a = rng.Next(), b = rng.Next();
    const float live = 0.5f * (a + b);
    const float variant = (0.0f + a + b) / 2.0f;
    const float shared = (a + b) / float(2);
    Check(std::memcmp(&live, &variant, sizeof(float)) == 0 && std::memcmp(&live, &shared, sizeof(float)) == 0, "the mean of two views is one number whichever way a reader writes it");
  }
}

// ---------------------------------------------------------------------------
// the HLSL twin as C++
// ---------------------------------------------------------------------------

namespace shim_state {
const TestAtlas* atlas = nullptr;
std::array<hlsl_shim::LslView, 8> views{};
uint32_t layers = 0;
}  // namespace shim_state

}  // namespace

// the seams, defined as the including shader would (the hlsli declared them)
namespace hlsl_shim {
float LslTexel(uint, int ix, int iy) { return shim_state::atlas->At(ix, iy); }
uint LslAtlasWidth(uint) { return shim_state::atlas->width; }
uint LslAtlasHeight(uint) { return shim_state::atlas->height; }
LslView LslLoadView(uint entry) { return shim_state::views[entry]; }
uint LslLayerOf(uint entry) { return LslLayerBits(shim_state::layers, entry); }
}  // namespace hlsl_shim

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

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void TestHlslTwin(const std::string& hlsli_path) {
  // the numbers
  Check(LSL_MAX_VIEWS == int(lg::kMaxLightShadowViews) && LSL_VIEW_BYTES == int(sl::kGpuShadowViewBytes) && LSL_HEADER_BYTES == int(sl::kGpuShadowHeaderBytes), "the twin's sizes are the table's");
  Check(LSL_MODE_SURFACE == int(sl::Mode::kSurface) && LSL_MODE_AIR == int(sl::Mode::kAir), "the twin's modes");
  Check(LSL_PCF_MIN_RADIUS == sf::kPcfMinRadius && LSL_PCF_MAX_RADIUS == sf::kPcfMaxRadius && LSL_LAMP_RADIUS == sf::kLampRadius && LSL_MAX_SLOPE == sf::kMaxSlope && LSL_SLOPE_BIAS == sf::kSlopeBias &&
            LSL_BORDER_FADE == sf::kBorderFade && LSL_DEGENERATE == sf::kDegenerate,
        "the twin's plane lookup has light_shadow_filter.h's numbers");
  Check(LSL_LAYER_SINGLE == int(lg::ShadowLayer::kSingle) && LSL_LAYER_STATIC == int(lg::ShadowLayer::kStatic) && LSL_LAYER_LIVE == int(lg::ShadowLayer::kLive), "the twin's layers are ShadowLayer's");
  Check(LSL_SHADOW_NONE == int(lg::ShadowKind::kNone) && LSL_SHADOW_ATLAS == int(lg::ShadowKind::kAtlas) && LSL_SHADOW_RT_MASK == int(lg::ShadowKind::kRtMask), "the twin's shadow kinds are ShadowKind's");
  Check(sizeof(hlsl_shim::LslView) >= 96, "the twin's view row has the 96 bytes' fields");
  // the text keeps to the subset both languages read and to its own names
  const std::string text = ReadFile(hlsli_path);
  Check(!text.empty(), "the hlsli is found (" + hlsli_path + ")");
  Check(text.find("static const") == std::string::npos, "no `static const` in the twin");
  Check(text.find("LslLightLit") != std::string::npos && text.find("LARECOMP_LIGHT_SHADOW_LOOKUP_HLSLI") != std::string::npos, "the twin has its guard and its entry point");
  // the functions against the C++ reference on random views, atlases and lights
  Lcg rng;
  uint64_t checked = 0;
  for (int n = 0; n < 20000; ++n) {
    TestAtlas atlas;
    atlas.seed = 1 + rng.Int(60);
    shim_state::atlas = &atlas;
    lg::ShadowFrame frame;
    frame.atlas = MakeInfo();
    frame.count = 3;
    lg::ShadowRef ref;
    ref.kind = lg::ShadowKind::kAtlas;
    ref.first_view = uint8_t(rng.Int(2));
    ref.view_count = uint8_t(1 + rng.Int(2));
    ref.weight = rng.Next() < 0.5f ? 1.0f : rng.Next();
    for (uint32_t e = 0; e < 3; ++e) {
      frame.views[e] = TileView(e, rng.Next() < 0.5f ? 0.002f : rng.Range(0.0f, 0.02f), rng.Next() < 0.95f);
      shim_state::views[e] = ToLsl(frame.views[e]);
    }
    if (rng.Next() < 0.25f && ref.first_view == 0) {
      frame.view_layer[0] = lg::ShadowLayer::kStatic;
      frame.view_layer[1] = lg::ShadowLayer::kLive;
      ref.view_count = 2;
    }
    shim_state::layers = 0;
    sl::GpuShadowBuffer table;
    sl::BuildGpuShadowBuffer(frame, &table);
    shim_state::layers = table.header.layers;
    float p[3];
    RandomPoint(&rng, frame.views[rng.Int(3)], p);
    float nrm[3];
    RandomNormal(&rng, nrm);
    for (const sl::Mode mode : {sl::Mode::kSurface, sl::Mode::kAir}) {
      const auto depth = [&](int x, int y) { return atlas.At(x, y); };
      const float view_want = sl::ViewLit(frame.views[ref.first_view % 3], p, nrm, atlas.width, atlas.height, mode, depth);
      const float view_got = hlsl_shim::LslViewLit(shim_state::views[ref.first_view % 3], p[0], p[1], p[2], nrm[0], nrm[1], nrm[2], 0u, uint32_t(mode));
      Check(std::memcmp(&view_want, &view_got, sizeof(float)) == 0, "the twin's view lookup is the reference's, bit for bit");
      const float want = sl::LightLit(frame, ref, p, nrm, mode, depth);
      const float got = hlsl_shim::LslLightLit(uint32_t(ref.kind), ref.first_view, ref.view_count, ref.weight, p[0], p[1], p[2], nrm[0], nrm[1], nrm[2], 0u, uint32_t(mode));
      Check(std::memcmp(&want, &got, sizeof(float)) == 0, "the twin's light lookup is the reference's, bit for bit");
      ++checked;
    }
  }
  Check(checked == 40000, "every random light was looked up in both modes");
  // a light with no atlas views, or none counted, is lit
  TestAtlas atlas;
  shim_state::atlas = &atlas;
  Check(hlsl_shim::LslLightLit(uint32_t(lg::ShadowKind::kRtMask), 0, 2, 1.0f, 0.5f, 0.5f, 0.5f, 0.0f, 1.0f, 0.0f, 0u, 0u) == 1.0f &&
            hlsl_shim::LslLightLit(uint32_t(lg::ShadowKind::kAtlas), 0, 0, 1.0f, 0.5f, 0.5f, 0.5f, 0.0f, 1.0f, 0.0f, 0u, 0u) == 1.0f,
        "a light with no atlas views is lit");
  // the header's two bits an entry
  Check(hlsl_shim::LslLayerBits(0b100100u, 1) == 1u && hlsl_shim::LslLayerBits(0b100100u, 2) == 2u && hlsl_shim::LslLayerBits(0b100100u, 0) == 0u, "two bits an entry");
}

void TestGpuShadowBuffer() {
  lg::ShadowFrame f;
  f.atlas = MakeInfo(9);
  f.atlas.tile_large = 1024;
  Lcg rng;
  f.count = 3;
  f.views[0] = MakeView(&rng, 0);
  f.views[1] = MakeView(&rng, 1);
  f.views[2] = MakeView(&rng, 1);
  f.view_layer[1] = lg::ShadowLayer::kStatic;
  f.view_layer[2] = lg::ShadowLayer::kLive;
  sl::GpuShadowBuffer b;
  sl::BuildGpuShadowBuffer(f, &b);
  Check(b.header.count == 3 && b.header.atlas_slot == 9 && b.header.atlas_width == 1536 && b.header.atlas_height == 512 && b.header.tile_small == 512 && b.header.tile_large == 1024,
        "the header says the atlas");
  Check(b.header.layers == ((1u << 2) | (2u << 4)), "two bits an entry: 0 single, 1 static, 2 live");
  Check(std::memcmp(&b.views[1], &f.views[1], sizeof(lg::LocalShadowView)) == 0 && std::memcmp(&b.views[2], &f.views[2], sizeof(lg::LocalShadowView)) == 0, "the rows are the views, byte for byte");
  f.atlas.valid = false;
  sl::BuildGpuShadowBuffer(f, &b);
  Check(b.header.count == 0 && b.header.atlas_slot == lg::kNoLightSlot, "no valid atlas: an empty table that names no texture");
  static_assert(sizeof(sl::GpuShadowBuffer) == 32 + 96 * 8, "table size");
}

}  // namespace

int main(int argc, char** argv) {
  std::string hlsli = argc > 1 ? argv[1] : "";
  if (hlsli.empty()) {
    std::string self = __FILE__;
    const size_t cut = self.find_last_of("/\\");
    hlsli = (cut == std::string::npos ? std::string(".") : self.substr(0, cut)) + "/hlsl/light_shadow_lookup.hlsli";
  }
  TestSinglePoolIsTheReplaysLayout();
  TestLevelsMakeTheReplaysAtlas();
  TestBudgetSpec();
  TestMemoryTable();
  TestLayoutFuzz();
  TestFit();
  TestBarrier();
  TestClears();
  TestStickySlots();
  TestStickyFuzz();
  TestBuilderIsShadowFrameFromLocal();
  TestRebindIsTheKeyRemap();
  TestDiffFindsEveryField();
  TestInfoAndMasks();
  TestProviderInterface();
  TestSurfaceModeIsTheVariantsTwin();
  TestAirModeIsTheVolumesTwin();
  TestModeEdges();
  TestLightLitIsTheCombineRule();
  TestHlslTwin(hlsli);
  TestGpuShadowBuffer();
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
