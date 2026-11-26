#pragma once
// ===========================================================================
// MCLA GPU renderer -- the shadow atlas's rules: one atlas for every provider of shadow views (perf/LIGHT-UNIFY.md section 4 and step U5, package LU-5)
// ===========================================================================
// Pure: no renderer, no guest, no SDK, no GPU (shadow_atlas_test.cpp holds it to the replay's own rules, and light_project_test.cpp to a recorded drive). The renderer's half is
// shadow_atlas.{h,cpp}; the providers (shadow_provider.h) are the draw replay (local_shadow.h, the first), the street lamps' shadows (lamp_shadows.h: LAMP-SHADOWS-4, read by the deferred light pass,
// joined only while the lamps are shadowed) and the ray traced mask (CITY-RT CP-D, later: it makes no tiles at all, only channels).
//
//   POOLS AND LAYOUT   A provider asks for POOLS (PoolRequest): `slots` square tiles of `tile` texels, a role (single, live or static: ShadowLayer) and whether a slot STAYS with a light
//                      id (sticky). MakeLayout puts the pools into ONE texture: the largest tiles first, each pool a block of columns x rows, a narrower pool re-shaped to the width of the
//                      widest so that the texture has no empty band beside it. One pool = exactly local_shadow::MakeAtlas / AtlasFor (the headlight replay's layout today, bit for bit), and the
//                      layout depends on the LEVEL, never on the lights in view: it is not made again when a car comes into view (the rule AtlasLayout states).
//   BUDGETS            SpecFor(budget) = the pools of a LightBudget (light_budget.h: the replay's views and tile, the RESERVED lamp views, spare slots and tile), SpecForLevel(level) the same from a
//                      level; Bytes() the video memory the layout holds (D32 float + S8 is 8 bytes a texel: plume's RenderFormatSize); Fit* steps a level down before the atlas is made when
//                      the memory is not there (Ultra -> High -> Medium -> none): the plan is made for the level that fits.
//   THE ONE BARRIER    UseTracker: the texture's use (depth write, shader read) and the barriers a record pass needs: BeginWrite (to depth write, graphics) and Publish (to shader read for the
//                      PIXEL and the COMPUTE stages, always: the f427298a bug, a volumetric compute pass reading an atlas left in a graphics-only read state, lives in one function now).
//   CLEARS             ClearPlan: a pool that is drawn every frame (single, live) is cleared to 1 ("nothing drawn here") every record; a static pool only when it is invalid. All pools every
//                      record = ONE clear of the whole target, as the replay clears today.
//   SLOTS              StickySlots: a light id keeps its slot while it is wanted, and a light that left the pick keeps a spare slot while its shadow fades out; a newcomer takes a free slot, else
//                      the worst fading holder's, never a wanted holder's. The replay's pool is NOT sticky (a view is drawn anew every frame: slot k = view k, as today); the lamps' are.
//   PUBLISHED FORM     ShadowFrameBuilder: the provider hands its views with the id of the light each is of and its layer; Build() makes the ShadowFrame (views of a light consecutive, at most two,
//                      the same grouping as light_convert.h ShadowFrameFromLocal) and the ray traced masks' channels; RebindByIds moves a published frame to the next frame's lights BY ID (what
//                      the key remap of headlight_rules::RemapShadowViews + the carry do today).
//
// THE LOOKUP is light_shadow_lookup.h (one source for every reader). Namespace mcla::gpu::shadow_atlas.
// ===========================================================================

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

#include "light_budget.h"
#include "light_convert.h"
#include "light_frame.h"
#include "light_id.h"
#include "local_shadow_rules.h"
#include "local_shadow_types.h"

namespace mcla::gpu::shadow_atlas {

namespace lg = ::larecomp::gpu;
namespace lb = ::mcla::gpu::light_budget;
namespace ls = ::mcla::gpu::local_shadow;

// ---------------------------------------------------------------------------
// providers, pools, layout
// ---------------------------------------------------------------------------

enum class ProviderId : uint8_t {
  kReplay = 0,  // the headlight draw replay (local_shadow.h)
  kLamps = 1,   // the street lamps' shadows (lamp_shadows.h; joined only while they are shadowed)
  kRtMask = 2,  // the ray traced mask (CITY-RT CP-D): no tiles, it publishes channels
  kCount
};
inline constexpr uint32_t kProviderCount = uint32_t(ProviderId::kCount);
constexpr const char* ProviderName(ProviderId p) {
  switch (p) {
    case ProviderId::kReplay: return "headlight replay";
    case ProviderId::kLamps: return "street lamps";
    case ProviderId::kRtMask: return "ray traced mask";
    case ProviderId::kCount: break;
  }
  return "?";
}

// What a pool's tiles are of a light's view: kSingle one view that stands alone (a headlamp's), kLive the map of the moving casters, kStatic the map of the static ones (a street lamp's two
// layers). The same three as light_frame.h ShadowLayer.
enum class PoolRole : uint8_t { kSingle = 0, kLive = 1, kStatic = 2 };
constexpr lg::ShadowLayer LayerOf(PoolRole role) {
  return role == PoolRole::kStatic ? lg::ShadowLayer::kStatic : role == PoolRole::kLive ? lg::ShadowLayer::kLive : lg::ShadowLayer::kSingle;
}

struct PoolRequest {
  ProviderId provider = ProviderId::kReplay;
  PoolRole role = PoolRole::kSingle;
  uint32_t tile = 0;    // texels of a tile's side
  uint32_t slots = 0;   // tiles
  bool sticky = false;  // a slot stays with a light id (StickySlots)
  uint32_t band_slot = 0;  // logical slot of a separately packed tile
  bool manual_clear = false;
  friend constexpr bool operator==(const PoolRequest&, const PoolRequest&) = default;
};

inline constexpr uint32_t kMaxPools = 32;  // eight headlight tiles and ten live/static lamp pairs
inline constexpr uint32_t kMaxLegacyPools = 4;
inline constexpr uint32_t kMaxSlots = 32;  // the most tiles of an atlas (ultra: 6 + 10 + 10 would be 26)

struct AtlasSpec {
  std::array<PoolRequest, kMaxPools> pools{};
  uint32_t count = 0;
  bool compact = false;
  // A pool of no tiles (a provider that has nothing to ask for) is left out; false when the spec is full.
  bool Add(const PoolRequest& p) {
    if (p.tile == 0 || p.slots == 0) {
      return true;
    }
    if (count >= (compact ? kMaxPools : kMaxLegacyPools)) {
      return false;
    }
    pools[count++] = p;
    return true;
  }
  uint32_t SlotCount() const {
    uint32_t n = 0;
    for (uint32_t i = 0; i < count; ++i) {
      n += pools[i].slots;
    }
    return n;
  }
  friend bool operator==(const AtlasSpec& a, const AtlasSpec& b) {
    if (a.count != b.count || a.compact != b.compact) {
      return false;
    }
    for (uint32_t i = 0; i < a.count; ++i) {
      if (!(a.pools[i] == b.pools[i])) {
        return false;
      }
    }
    return true;
  }
};

struct Rect {
  uint32_t x = 0, y = 0, size = 0;
  bool valid() const { return size != 0; }
  friend constexpr bool operator==(const Rect&, const Rect&) = default;
};

// Columns of a block of `slots` tiles: one row up to three, 2 x 2 for four, three columns for five and six, four from seven (local_shadow::MakeAtlas' rule, without its cap at eight views).
constexpr uint32_t ColumnsFor(uint32_t slots) { return slots <= 3 ? slots : slots == 4 ? 2u : slots <= 6 ? 3u : 4u; }

struct PoolLayout {
  PoolRequest request;
  uint32_t cols = 0, rows = 0;
  uint32_t x0 = 0, y0 = 0;     // the block's corner in texels
  uint32_t first_slot = 0;     // the atlas-wide number of the pool's first tile (pools in layout order)
  friend constexpr bool operator==(const PoolLayout&, const PoolLayout&) = default;
};

struct Layout {
  uint32_t width = 0, height = 0;
  uint32_t pool_count = 0;
  uint32_t slot_count = 0;
  std::array<PoolLayout, kMaxPools> pools{};
  bool valid() const { return width != 0 && height != 0 && pool_count != 0; }
  // The tile `index` of pool `pool` (an index into `pools`: FindPool), in texels. An empty rect when either is out of range.
  Rect SlotRect(uint32_t pool, uint32_t index) const {
    if (pool >= pool_count || index >= pools[pool].request.slots || pools[pool].cols == 0) {
      return Rect{};
    }
    const PoolLayout& p = pools[pool];
    return Rect{p.x0 + (index % p.cols) * p.request.tile, p.y0 + (index / p.cols) * p.request.tile, p.request.tile};
  }
  // The tile with the atlas-wide number `slot`.
  Rect GlobalSlotRect(uint32_t slot) const {
    for (uint32_t i = 0; i < pool_count; ++i) {
      const PoolLayout& p = pools[i];
      if (slot >= p.first_slot && slot < p.first_slot + p.request.slots) {
        return SlotRect(i, slot - p.first_slot);
      }
    }
    return Rect{};
  }
  // The pool a provider's role has in this layout, or -1.
  int FindPool(ProviderId provider, PoolRole role, uint32_t band_slot = 0) const {
    for (uint32_t i = 0; i < pool_count; ++i) {
      if (pools[i].request.provider == provider && pools[i].request.role == role && pools[i].request.band_slot == band_slot) {
        return int(i);
      }
    }
    return -1;
  }
  // The smallest and the largest tile side (0 when empty).
  uint32_t SmallestTile() const {
    uint32_t t = 0;
    for (uint32_t i = 0; i < pool_count; ++i) {
      t = t == 0 ? pools[i].request.tile : std::min(t, pools[i].request.tile);
    }
    return t;
  }
  uint32_t LargestTile() const {
    uint32_t t = 0;
    for (uint32_t i = 0; i < pool_count; ++i) {
      t = std::max(t, pools[i].request.tile);
    }
    return t;
  }
  friend bool operator==(const Layout& a, const Layout& b) {
    if (a.width != b.width || a.height != b.height || a.pool_count != b.pool_count || a.slot_count != b.slot_count) {
      return false;
    }
    for (uint32_t i = 0; i < a.pool_count; ++i) {
      if (!(a.pools[i] == b.pools[i])) {
        return false;
      }
    }
    return true;
  }
};

// Compact layouts contain one square per pool. Try widths on the common 128-texel grid and choose the smallest occupied rectangle.
inline Layout MakeCompactLayout(const AtlasSpec& spec) {
  constexpr uint32_t quantum = 128;
  constexpr uint32_t limit = 64;  // 8192 texels per side
  if (spec.count == 0 || spec.count > kMaxPools) {
    return {};
  }
  std::array<uint32_t, kMaxPools> order{};
  uint32_t first_width = 0, last_width = 0;
  for (uint32_t i = 0; i < spec.count; ++i) {
    const PoolRequest& request = spec.pools[i];
    if (request.slots != 1 || request.tile == 0 || request.tile % quantum != 0 || request.tile >= limit * quantum) {
      return {};
    }
    const uint32_t side = request.tile / quantum;
    order[i] = i;
    first_width = std::max(first_width, side);
    last_width += side;
  }
  std::stable_sort(order.begin(), order.begin() + spec.count, [&](uint32_t a, uint32_t b) { return spec.pools[a].tile > spec.pools[b].tile; });
  Layout best;
  uint32_t best_area = limit * limit + 1;
  for (uint32_t width = first_width; width <= std::min(limit, last_width); ++width) {
    std::array<uint64_t, limit> occupied{};
    Layout fit;
    uint32_t right = 0, bottom = 0;
    for (uint32_t i = 0; i < spec.count; ++i) {
      const PoolRequest& request = spec.pools[order[i]];
      const uint32_t side = request.tile / quantum;
      bool placed = false;
      for (uint32_t y = 0; y + side <= limit && !placed; ++y) {
        uint64_t blocked = 0;
        for (uint32_t row = y; row < y + side; ++row) {
          blocked |= occupied[row];
        }
        for (uint32_t x = 0; x + side <= width; ++x) {
          const uint64_t mask = ((uint64_t{1} << side) - 1) << x;
          if ((blocked & mask) != 0) {
            continue;
          }
          for (uint32_t row = y; row < y + side; ++row) {
            occupied[row] |= mask;
          }
          fit.pools[fit.pool_count++] = {request, 1, 1, x * quantum, y * quantum, i};
          right = std::max(right, x + side);
          bottom = std::max(bottom, y + side);
          placed = true;
          break;
        }
      }
      if (!placed || right * bottom > best_area) {
        break;
      }
    }
    if (fit.pool_count == spec.count && (right * bottom < best_area ||
        (right * bottom == best_area && std::max(right, bottom) * quantum < std::max(best.width, best.height)))) {
      best_area = right * bottom;
      fit.width = right * quantum;
      fit.height = bottom * quantum;
      fit.slot_count = fit.pool_count;
      best = fit;
    }
  }
  return best;
}

// The tile as atlas uv: {u offset, v offset, u size, v size} (local_shadow::UvRect's arithmetic: the division is in double).
inline void UvOf(const Layout& layout, const Rect& r, float out[4]) {
  const double w = double(layout.width);
  const double h = double(layout.height);
  out[0] = w > 0 ? float(double(r.x) / w) : 0.0f;
  out[1] = h > 0 ? float(double(r.y) / h) : 0.0f;
  out[2] = w > 0 ? float(double(r.size) / w) : 0.0f;
  out[3] = h > 0 ? float(double(r.size) / h) : 0.0f;
}

// ONE TEXTURE for the pools of a spec. The largest tiles go first (stable among equals: the spec's order), each pool is a block of `cols` x `rows` tiles at the left edge, blocks stack
// downwards. The width is the widest block's natural width (ColumnsFor); a narrower pool takes as many columns as that width holds, so that its block has the fewest rows: ultra's 10
// lamp tiles of 512 under the replay's 3 x 2 tiles of 1024 are 6 x 2 (3072 x 1024), not 4 x 3 (2048 x 1536 in a 3072 wide texture). One pool: width = tile x ColumnsFor(slots),
// height = tile x rows = local_shadow::MakeAtlas, which is what the replay made until now.
inline Layout MakeLayout(const AtlasSpec& spec) {
  if (spec.compact) {
    return MakeCompactLayout(spec);
  }
  Layout out;
  std::array<uint32_t, kMaxPools> order{};
  uint32_t n = 0;
  for (uint32_t i = 0; i < spec.count && i < kMaxPools; ++i) {
    if (spec.pools[i].tile != 0 && spec.pools[i].slots != 0 && spec.pools[i].slots <= kMaxSlots) {
      order[n++] = i;
    }
  }
  if (n == 0) {
    return out;
  }
  std::stable_sort(order.begin(), order.begin() + n, [&](uint32_t a, uint32_t b) { return spec.pools[a].tile > spec.pools[b].tile; });
  uint32_t width = 0;
  for (uint32_t k = 0; k < n; ++k) {
    const PoolRequest& r = spec.pools[order[k]];
    width = std::max(width, r.tile * ColumnsFor(r.slots));
  }
  uint32_t y = 0;
  uint32_t first = 0;
  for (uint32_t k = 0; k < n; ++k) {
    PoolLayout& p = out.pools[k];
    p.request = spec.pools[order[k]];
    p.cols = std::min(p.request.slots, std::max(1u, width / p.request.tile));
    p.rows = (p.request.slots + p.cols - 1) / p.cols;
    p.x0 = 0;
    p.y0 = y;
    p.first_slot = first;
    y += p.rows * p.request.tile;
    first += p.request.slots;
  }
  out.pool_count = n;
  out.slot_count = first;
  out.width = width;
  out.height = y;
  return out;
}

// Every pool inside the texture, no two tiles overlapping, every tile on a whole number of its own texels. A text for the first thing wrong, empty when the layout is sound.
inline std::string ValidateLayout(const Layout& layout) {
  if (!layout.valid()) {
    return "empty";
  }
  for (uint32_t a = 0; a < layout.pool_count; ++a) {
    const PoolLayout& p = layout.pools[a];
    if (p.request.tile == 0 || p.request.slots == 0 || p.cols == 0 || p.rows == 0 || p.cols * p.rows < p.request.slots) {
      return "pool " + std::to_string(a) + " has no room for its tiles";
    }
    if (p.x0 + p.cols * p.request.tile > layout.width || p.y0 + p.rows * p.request.tile > layout.height) {
      return "pool " + std::to_string(a) + " is outside the texture";
    }
    for (uint32_t b = a + 1; b < layout.pool_count; ++b) {
      const PoolLayout& q = layout.pools[b];
      const bool apart = p.x0 + p.cols * p.request.tile <= q.x0 || q.x0 + q.cols * q.request.tile <= p.x0 || p.y0 + p.rows * p.request.tile <= q.y0 ||
                         q.y0 + q.rows * q.request.tile <= p.y0;
      if (!apart) {
        return "pools " + std::to_string(a) + " and " + std::to_string(b) + " overlap";
      }
    }
  }
  return {};
}

// ---------------------------------------------------------------------------
// memory
// ---------------------------------------------------------------------------

// D32 float + S8: 8 bytes a texel (plume's RenderFormatSize: 24 bits unused), a lower bound (the driver pads): local_shadow_rules.h kAtlasTexelBytes. CHECK: a D32-only atlas (4 bytes a
// texel, half of all of this) needs the depth plane read as it is on both backends; the format is one constant of the texture's creation (shadow_atlas.cpp) and this one.
inline constexpr uint64_t kTexelBytes = ls::kAtlasTexelBytes;
inline uint64_t Bytes(const Layout& layout, uint64_t texel_bytes = kTexelBytes) { return uint64_t(layout.width) * layout.height * texel_bytes; }
inline double Mib(uint64_t bytes) { return double(bytes) / (1024.0 * 1024.0); }

// ---------------------------------------------------------------------------
// budgets: the pools of a level
// ---------------------------------------------------------------------------

// The street lamps' shadow views a level wants, their spare slots (a lamp whose shadow fades out) and the tile (lamp_shadows_rules.h: 2 (+1) at medium, 4 (+1) at high, 8 (+2) at ultra,
// 512 texels: a texel is 5.4 cm straight below a 9 m lamp). light_budget.h keeps its own lamp numbers RESERVED (0); this is where the lamps' numbers are, so that the memory table and the layout
// are written once. The atlas grows by whole rows of its shelf: the lamps' 512 tiles fill 3 of 3 / 5 of 6 / 7 of 7.5 columns of the headlights' 768 / 1024 / 1280 tiles (12 / 24 / 60 MiB for 12 / 20 / 40
// MiB of tiles; the whole atlas 21 / 48 / 135 MiB with the headlights' 9 / 24 / 75). `live` pools: one tile for each view; `static` pools: the same count again when the static cache is on (a setting of that branch).
struct LampRow {
  lb::Level level = lb::Level::kOff;
  uint32_t views = 0;
  uint32_t spare = 0;
  uint32_t tile = 0;
};
inline constexpr LampRow kLampRows[] = {
    {lb::Level::kMedium, 2, 1, 512},
    {lb::Level::kHigh, 4, 1, 512},
    {lb::Level::kUltra, 8, 2, 512},
};
constexpr const LampRow* LampRowOf(lb::Level level) {
  for (const LampRow& r : kLampRows) {
    if (r.level == level) {
      return &r;
    }
  }
  return nullptr;
}

// The pools of a frame's budget. `lamps`: the street lamps' provider is registered (lamp_shadows.h joins while the lamps are shadowed), so its live (and, with `lamp_static`, static) pool is asked from the lamp row of the
// level, or from the budget's reserved numbers when they are set.
inline AtlasSpec SpecFor(const lb::LightBudget& b, lb::Level level, bool lamps = false, bool lamp_static = false) {
  AtlasSpec s;
  s.Add({ProviderId::kReplay, PoolRole::kSingle, b.shadow_tile, b.shadow_views, false});
  if (lamps) {
    uint32_t views = b.lamp_views + b.lamp_views_spare;
    uint32_t tile = b.lamp_tile;
    if (views == 0) {
      if (const LampRow* row = LampRowOf(level)) {
        views = row->views + row->spare;
        tile = row->tile;
      }
    }
    s.Add({ProviderId::kLamps, PoolRole::kLive, tile, views, true});
    if (lamp_static) {
      s.Add({ProviderId::kLamps, PoolRole::kStatic, tile, views, true});
    }
  }
  return s;
}

// The pools a level (and the debug setting of the views of a car) makes: the replay's alone unless the lamps' provider is there.
inline AtlasSpec SpecForLevel(lb::Level level, int views_per_car, bool lamps = false, bool lamp_static = false, ls::LevelTable table = ls::kLevelTable) {
  return SpecFor(lb::BudgetFor(true, level, views_per_car, lb::Level::kOff, table), level, lamps, lamp_static);
}

inline Layout LayoutForLevel(lb::Level level, int views_per_car, bool lamps = false, bool lamp_static = false, ls::LevelTable table = ls::kLevelTable) {
  return MakeLayout(SpecForLevel(level, views_per_car, lamps, lamp_static, table));
}

// The level that fits: `wanted` and then the levels below it, each asked `has_room(extra bytes)` (the bytes the layout holds more than `held_bytes`, what is made new). Off when none fits.
// `stepped` says the answer is lower than the wish. The plan (the cars and the views) is made for the answer, so the atlas always holds what the plan draws.
struct Fit {
  lb::Level level = lb::Level::kOff;
  uint64_t bytes = 0;
  bool stepped = false;
};
template <class HasRoom>
inline Fit FitLevel(lb::Level wanted, int views_per_car, bool lamps, bool lamp_static, uint64_t held_bytes, HasRoom&& has_room, ls::LevelTable table = ls::kLevelTable) {
  Fit fit;
  fit.level = lb::Level::kOff;
  if (wanted == lb::Level::kOff) {
    return fit;
  }
  for (int l = int(wanted); l >= int(lb::Level::kMedium); --l) {
    const lb::Level level = lb::Level(l);
    if (ls::Row(level, table) == nullptr) {
      continue;
    }
    const uint64_t bytes = Bytes(LayoutForLevel(level, views_per_car, lamps, lamps && lamp_static, table));
    const uint64_t extra = bytes > held_bytes ? bytes - held_bytes : 0;
    if (extra == 0 || has_room(extra)) {
      fit.level = ls::Effective(level, table);
      fit.bytes = bytes;
      fit.stepped = level != wanted;
      return fit;
    }
  }
  fit.stepped = true;
  return fit;
}

// The same with the lamps' live pool only (the first form: LU-5, kept for its callers and tests).
template <class HasRoom>
inline Fit FitLevel(lb::Level wanted, int views_per_car, bool lamps, uint64_t held_bytes, HasRoom&& has_room, ls::LevelTable table = ls::kLevelTable) {
  return FitLevel(wanted, views_per_car, lamps, false, held_bytes, std::forward<HasRoom>(has_room), table);
}

// ---------------------------------------------------------------------------
// the one barrier
// ---------------------------------------------------------------------------

enum class Use : uint8_t { kUnknown = 0, kDepthWrite = 1, kShaderRead = 2 };
inline constexpr uint8_t kStageGraphics = 1;  // plume::RenderBarrierStage::GRAPHICS
inline constexpr uint8_t kStageCompute = 2;   // plume::RenderBarrierStage::COMPUTE
// The stages that read a published atlas: the lit draws and the translucent ones (pixel) AND the volumetric light's inject (compute; later the deferred light pass). A transition to
// shader read names both, always: this constant is the rule (the f427298a bug: the atlas was left in a graphics-only read state while the volume's compute pass read it).
inline constexpr uint8_t kPublishStages = kStageGraphics | kStageCompute;

struct Barrier {
  bool needed = false;
  Use to = Use::kUnknown;
  uint8_t stages = 0;
};

class UseTracker {
 public:
  // Before the pass draws into the atlas: to depth write (graphics) unless it is already there.
  Barrier BeginWrite() {
    if (use_ == Use::kDepthWrite) {
      return Barrier{};
    }
    use_ = Use::kDepthWrite;
    return Barrier{true, Use::kDepthWrite, kStageGraphics};
  }
  // After the pass, whatever happened in it (a partial map is a map): to shader read for pixel and compute, every time.
  Barrier Publish() {
    use_ = Use::kShaderRead;
    return Barrier{true, Use::kShaderRead, kPublishStages};
  }
  // A new texture: its contents and layout are unknown.
  void Reset() { use_ = Use::kUnknown; }
  Use use() const { return use_; }

 private:
  Use use_ = Use::kUnknown;
};

// ---------------------------------------------------------------------------
// clears
// ---------------------------------------------------------------------------

inline constexpr float kClearDepth = 1.0f;  // "nothing drawn here": local_shadow_rules.h DEPTH ("1 also where nothing was drawn (the clear value)")
inline constexpr uint8_t kClearStencil = 0;

enum class ClearWhen : uint8_t { kEveryRecord, kOnInvalidate };
constexpr ClearWhen ClearOf(PoolRole role) { return role == PoolRole::kStatic ? ClearWhen::kOnInvalidate : ClearWhen::kEveryRecord; }

// A block of texels (a pool's, whole): x, y, width, height.
struct Area {
  uint32_t x = 0, y = 0, w = 0, h = 0;
  friend constexpr bool operator==(const Area&, const Area&) = default;
};
struct ClearPlan {
  bool whole_target = false;        // one clear of the whole texture
  uint32_t rect_count = 0;          // else these rectangles (texels), one for each pool to clear
  std::array<Area, kMaxPools> rects{};
  std::array<uint32_t, kMaxPools> pools{};  // the pool each rectangle is of
};
// Which pools are cleared by a record pass: those drawn every record, and a static pool whose bit is set in `static_invalid` (bit = the pool's index in the layout). When every pool is
// cleared the whole texture is, in one clear (the replay's: one pool, one clear).
inline ClearPlan MakeClearPlan(const Layout& layout, uint32_t static_invalid = 0) {
  ClearPlan plan;
  bool all = layout.valid();
  for (uint32_t i = 0; i < layout.pool_count; ++i) {
    const PoolLayout& p = layout.pools[i];
    const bool clear = !p.request.manual_clear &&
                       (ClearOf(p.request.role) == ClearWhen::kEveryRecord || (static_invalid & (1u << i)) != 0);
    if (!clear) {
      all = false;
      continue;
    }
    plan.rects[plan.rect_count] = Area{p.x0, p.y0, p.cols * p.request.tile, p.rows * p.request.tile};
    plan.pools[plan.rect_count] = i;
    ++plan.rect_count;
  }
  plan.whole_target = all;
  if (all) {
    plan.rect_count = 0;
  }
  return plan;
}

// ---------------------------------------------------------------------------
// slots that stay with a light
// ---------------------------------------------------------------------------

inline constexpr uint32_t kNoSlot = 0xFFFFFFFFu;

class StickySlots {
 public:
  explicit StickySlots(uint32_t slots = 0) { Resize(slots); }
  // A new layout: nobody holds a slot any more (the texture was made again: whatever it held is gone).
  void Resize(uint32_t slots) {
    slots_ = std::min(slots, kMaxSlots);
    holder_.fill(0);
  }
  uint32_t slots() const { return slots_; }
  // One frame. `wanted[0 .. n)` = the ids that want a slot, best first; `fading[0 .. m)` = ids that left the pick and are still fading out, best first (they keep a slot they have and
  // there is room for). Out: `slot_of[i]` = the slot of wanted[i] or kNoSlot; `fresh[i]` = it was given this call (what a static map held for another light is not this light's: it must be
  // drawn again). Returns how many wanted ids hold a slot. A wanted id holding a slot is never moved or evicted; a free slot goes to a newcomer first, then the worst fading holder's.
  uint32_t Update(const uint64_t* wanted, uint32_t n, const uint64_t* fading, uint32_t m, uint32_t* slot_of, bool* fresh) {
    const auto in_list = [](const uint64_t* list, uint32_t count, uint64_t id) {
      for (uint32_t i = 0; i < count; ++i) {
        if (list[i] == id) {
          return true;
        }
      }
      return false;
    };
    for (uint32_t s = 0; s < slots_; ++s) {  // a holder that is neither wanted nor fading lets go
      if (holder_[s] != 0 && !in_list(wanted, n, holder_[s]) && !in_list(fading, m, holder_[s])) {
        holder_[s] = 0;
      }
    }
    uint32_t held = 0;
    for (uint32_t i = 0; i < n; ++i) {
      slot_of[i] = kNoSlot;
      if (fresh != nullptr) {
        fresh[i] = false;
      }
      if (wanted[i] == 0) {
        continue;
      }
      for (uint32_t s = 0; s < slots_; ++s) {
        if (holder_[s] == wanted[i]) {
          slot_of[i] = s;
          break;
        }
      }
    }
    for (uint32_t i = 0; i < n; ++i) {  // newcomers, in rank order
      if (slot_of[i] != kNoSlot || wanted[i] == 0) {
        continue;
      }
      uint32_t take = kNoSlot;
      for (uint32_t s = 0; s < slots_ && take == kNoSlot; ++s) {
        if (holder_[s] == 0) {
          take = s;
        }
      }
      if (take == kNoSlot) {  // the worst fading holder: the last of the fading list that holds a slot
        for (uint32_t k = m; k > 0 && take == kNoSlot; --k) {
          for (uint32_t s = 0; s < slots_; ++s) {
            if (holder_[s] == fading[k - 1] && !in_list(wanted, n, holder_[s])) {
              take = s;
              break;
            }
          }
        }
      }
      if (take == kNoSlot) {
        continue;  // every slot is a wanted light's: this one waits
      }
      holder_[take] = wanted[i];
      slot_of[i] = take;
      if (fresh != nullptr) {
        fresh[i] = true;
      }
    }
    for (uint32_t i = 0; i < n; ++i) {
      held += slot_of[i] != kNoSlot ? 1u : 0u;
    }
    return held;
  }
  uint32_t SlotOf(uint64_t id) const {
    for (uint32_t s = 0; s < slots_; ++s) {
      if (id != 0 && holder_[s] == id) {
        return s;
      }
    }
    return kNoSlot;
  }
  uint32_t Used() const {
    uint32_t n = 0;
    for (uint32_t s = 0; s < slots_; ++s) {
      n += holder_[s] != 0 ? 1u : 0u;
    }
    return n;
  }

 private:
  uint32_t slots_ = 0;
  std::array<uint64_t, kMaxSlots> holder_{};
};

// ---------------------------------------------------------------------------
// the published form
// ---------------------------------------------------------------------------

// What the atlas publishes (the LightFrame's ShadowFrame) and the ray traced masks' channels (no tile, no view: ShadowRef kind kRtMask).
inline constexpr uint32_t kMaxMasks = 16;
struct MaskRef {
  uint64_t light = lg::kNoLightId;
  uint8_t channel = 0;
  float weight = 1.0f;
};
struct Published {
  lg::ShadowFrame frame;
  std::array<MaskRef, kMaxMasks> masks{};
  uint32_t mask_count = 0;
};

// The providers hand their views here, in the order they made them: AddView(view, the id of the light it is of, its layer). Build() makes the ShadowFrame exactly as light_convert.h
// ShadowFrameFromLocal does (the views of a light consecutive: the valid views of the first light first, at most two of them; then the next light's; then the views that count for
// nobody, in their order). Layers: a kStatic entry and its kLive twin are consecutive entries of one light and count as ONE of its two (the lamps' provider hands them as a pair).
class ShadowFrameBuilder {
 public:
  void Clear() {
    count_ = 0;
    masks_ = {};
    mask_count_ = 0;
  }
  bool AddView(const lg::LocalShadowView& view, uint64_t light, lg::ShadowLayer layer = lg::ShadowLayer::kSingle) {
    if (count_ >= lg::kMaxLightShadowViews) {
      return false;
    }
    views_[count_] = view;
    light_[count_] = light;
    layer_[count_] = layer;
    ++count_;
    return true;
  }
  bool AddMask(uint64_t light, uint8_t channel, float weight) {
    if (mask_count_ >= kMaxMasks || light == lg::kNoLightId) {
      return false;
    }
    masks_[mask_count_++] = MaskRef{light, channel, weight};
    return true;
  }
  uint32_t view_count() const { return count_; }
  Published Build(const lg::ShadowAtlasInfo& atlas) const {
    Published out;
    out.frame.atlas = atlas;
    std::array<bool, lg::kMaxLightShadowViews> taken{};
    std::array<uint64_t, lg::kMaxLightShadowViews> grouped{};
    uint32_t grouped_count = 0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < count_; ++i) {
      if (taken[i] || views_[i].valid == 0 || light_[i] == lg::kNoLightId) {
        continue;
      }
      const uint64_t light = light_[i];
      if (std::find(grouped.begin(), grouped.begin() + grouped_count, light) != grouped.begin() + grouped_count) {
        continue;  // a third view of a light that has two: it counts for nobody
      }
      grouped[grouped_count++] = light;
      uint32_t in_group = 0;
      for (uint32_t j = i; j < count_ && in_group < 2; ++j) {
        if (!taken[j] && views_[j].valid != 0 && light_[j] == light) {
          taken[j] = true;
          out.frame.views[n] = views_[j];
          out.frame.view_light[n] = light;
          out.frame.view_layer[n] = layer_[j];
          ++n;
          ++in_group;
        }
      }
    }
    for (uint32_t i = 0; i < count_; ++i) {
      if (!taken[i]) {
        out.frame.views[n] = views_[i];
        out.frame.view_light[n] = lg::kNoLightId;
        out.frame.view_layer[n] = lg::ShadowLayer::kSingle;
        ++n;
      }
    }
    out.frame.count = n;
    out.masks = masks_;
    out.mask_count = mask_count_;
    return out;
  }

 private:
  std::array<lg::LocalShadowView, lg::kMaxLightShadowViews> views_{};
  std::array<uint64_t, lg::kMaxLightShadowViews> light_{};
  std::array<lg::ShadowLayer, lg::kMaxLightShadowViews> layer_{};
  uint32_t count_ = 0;
  std::array<MaskRef, kMaxMasks> masks_{};
  uint32_t mask_count_ = 0;
};

// A published frame read for the lights of the next frame: `ids[i]` = the id of rig light i of the frame that reads (`count` of them). A view goes to the light that has its id (at most two a
// light; views of a light that is gone, invalid views and the views that counted for nobody are dropped), light_index is that light's place in `ids` (what headlight_rules::RemapShadowViews
// does by the car's key: a car's lamp id is made from its key, so the two are the same moves), the atlas as it was published. The views stay grouped by light, in the order they had.
inline lg::ShadowFrame RebindByIds(const lg::ShadowFrame& kept, const uint64_t* ids, uint32_t count) {
  lg::ShadowFrame out;
  out.atlas = kept.atlas;
  std::array<uint32_t, lg::kMaxLightShadowViews> taken{};
  const uint32_t in_count = std::min<uint32_t>(kept.count, lg::kMaxLightShadowViews);
  uint32_t n = 0;
  for (uint32_t e = 0; e < in_count; ++e) {
    const lg::LocalShadowView& view = kept.views[e];
    const uint64_t owner = kept.view_light[e];
    if (view.valid == 0 || owner == lg::kNoLightId) {
      continue;
    }
    uint32_t k = count;
    for (uint32_t i = 0; i < count && i < lg::kMaxLightShadowViews; ++i) {
      if (ids[i] == owner) {
        k = i;
        break;
      }
    }
    if (k >= count || k >= lg::kMaxLightShadowViews || taken[k] >= 2 || n >= lg::kMaxLightShadowViews) {
      continue;
    }
    ++taken[k];
    out.views[n] = view;
    out.views[n].light_index = k;
    out.view_light[n] = owner;
    out.view_layer[n] = kept.view_layer[e];
    ++n;
  }
  out.count = n;
  return out;
}

// The atlas as the lights' description says it: the texture's slot and sampler, its size and the tile sizes (the smallest in `tile_small`, a larger one in `tile_large`; one size only:
// `tile_small`, as LightManager's AtlasOf made it from the replay's one tile).
inline lg::ShadowAtlasInfo InfoOf(const Layout& layout, uint32_t slot, uint32_t sampler, bool valid) {
  lg::ShadowAtlasInfo info;
  info.valid = valid && layout.valid();
  info.slot = slot;
  info.sampler = sampler;
  info.width = layout.width;
  info.height = layout.height;
  info.tile_small = layout.SmallestTile();
  info.tile_large = layout.LargestTile() != layout.SmallestTile() ? layout.LargestTile() : 0u;
  return info;
}

// Every light the masks name reads its channel: its mask_channel and the flag kLightFlagRtVisibility (RT-LIGHTS-D1: the ray traced visibility's way into the deferred light pass). THE PRODUCT RULE (the survey's
// O2: neither caster set is complete): a light that also has atlas views KEEPS them, its kind and its weight stay, and the light pass multiplies the two visibilities; a light with no atlas views takes kind
// kRtMask and the mask's weight. (Before D1 the mask won only where there were no views.)
inline void BindMaskRefs(lg::LightFrame* frame, const Published& published) {
  for (uint32_t m = 0; m < published.mask_count && m < kMaxMasks; ++m) {
    const int32_t slot = frame->Find(published.masks[m].light);
    if (slot < 0) {
      continue;
    }
    lg::Light& l = frame->lights[uint32_t(slot)];
    l.shadow.mask_channel = published.masks[m].channel;
    l.flags |= lg::kLightFlagRtVisibility;
    if (l.shadow.kind == lg::ShadowKind::kNone) {
      l.shadow.kind = lg::ShadowKind::kRtMask;
      l.shadow.weight = published.masks[m].weight;
    }
  }
}

// Two views the same bit for bit, field by field (a LocalShadowView has no padding: its size is the sum of its fields, light_convert.h asserts 96).
inline bool SameViewBits(const lg::LocalShadowView& a, const lg::LocalShadowView& b) {
  for (int i = 0; i < 16; ++i) {
    if (std::bit_cast<uint32_t>(a.world_to_shadow[i]) != std::bit_cast<uint32_t>(b.world_to_shadow[i])) {
      return false;
    }
  }
  for (int i = 0; i < 4; ++i) {
    if (std::bit_cast<uint32_t>(a.atlas_rect[i]) != std::bit_cast<uint32_t>(b.atlas_rect[i])) {
      return false;
    }
  }
  return std::bit_cast<uint32_t>(a.depth_bias) == std::bit_cast<uint32_t>(b.depth_bias) && std::bit_cast<uint32_t>(a.texel_size) == std::bit_cast<uint32_t>(b.texel_size) &&
         a.light_index == b.light_index && a.valid == b.valid;
}

// A text for the first thing that differs between two ShadowFrames (field by field: the structs have padding), empty when they are the same.
inline std::string DiffShadowFrames(const lg::ShadowFrame& a, const lg::ShadowFrame& b) {
  if (a.count != b.count) {
    return "the views are " + std::to_string(a.count) + " against " + std::to_string(b.count);
  }
  if (a.atlas.valid != b.atlas.valid || a.atlas.slot != b.atlas.slot || a.atlas.sampler != b.atlas.sampler || a.atlas.width != b.atlas.width || a.atlas.height != b.atlas.height ||
      a.atlas.tile_small != b.atlas.tile_small || a.atlas.tile_large != b.atlas.tile_large) {
    return "the atlas differs (slot " + std::to_string(a.atlas.slot) + " / " + std::to_string(b.atlas.slot) + ", " + std::to_string(a.atlas.width) + "x" + std::to_string(a.atlas.height) +
           " / " + std::to_string(b.atlas.width) + "x" + std::to_string(b.atlas.height) + ", tile " + std::to_string(a.atlas.tile_small) + " / " + std::to_string(b.atlas.tile_small) + ")";
  }
  for (uint32_t e = 0; e < lg::kMaxLightShadowViews; ++e) {
    if (!SameViewBits(a.views[e], b.views[e])) {
      return "view " + std::to_string(e) + " differs in its bytes";
    }
    if (a.view_light[e] != b.view_light[e]) {
      return "view " + std::to_string(e) + " is of another light";
    }
    if (a.view_layer[e] != b.view_layer[e]) {
      return "view " + std::to_string(e) + " is of another layer";
    }
  }
  return {};
}

}  // namespace mcla::gpu::shadow_atlas
