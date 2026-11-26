#pragma once
// ===========================================================================
// MCLA GPU renderer -- the city as ray tracing structures: the streaming rules that need no renderer
// ===========================================================================
// city_rt_scene.h keeps the city's geometry (the city pack, src/citypack: 439 sectors and their far level of detail) as bottom level acceleration
// structures round the camera, one per sector, and adds them to the car scene's top level every frame. This header holds what needs no device, as pure
// functions on plain numbers, so that city_rt_rules_test.cpp builds on its own:
//
//   - the setting's levels and what each costs by graphics class (Budget): how far round the camera the sectors are built at their full detail, how far at the
//     far level of detail, the memory the structures may hold, the triangles built in one frame
//   - the streaming plan: from the camera and each sector's box and state, which sectors to read next, which to give back (with hysteresis, nearest first,
//     under the memory cap, never taking a sector nearer than the one that needs the room)
//   - the build schedule of a frame (triangles and bytes, nearest first, always at least one sector so that none starves)
//   - which structure of a sector the top level holds (its hi detail once built, else its lod, never both) and the instance's mask and id
//   - what a structure costs (an estimate, until the device says the real size)
//
// THE NUMBERS (perf/CITY-RT-SURVEY.md section 2): a sector is 50,000 triangles at the median, up to 262,000; the hi detail within 150 m is 0.28 to 0.5
// million triangles, within 300 m 0.85 to 1.5 million, within 600 m 2.1 to 2.9 million; the whole far level of detail is 383,000 triangles. A structure is
// 57 to 92 bytes a triangle (measured on the cars, PREFER_FAST_TRACE, no compaction), 96 here.
//
// This header includes nothing of the renderer except gpu_class.h (which includes none either).
// ===========================================================================

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "city_pack_state.h"
#include "gpu_class.h"
#include "rt_tier.h"

namespace mcla::gpu::city_rt {

// ---------------------------------------------------------------------------
// the setting and the budgets
// ---------------------------------------------------------------------------

// What the cvar says (mcla_gpu_rt_shadows): auto, off, high, ultra. A text the list does not know counts as auto.
enum class Choice : uint8_t { kAuto, kOff, kHigh, kUltra };
// What runs.
enum class Level : uint8_t { kOff, kHigh, kUltra };

constexpr Choice ParseChoice(std::string_view text) {
  if (text == "off") return Choice::kOff;
  if (text == "high") return Choice::kHigh;
  if (text == "ultra") return Choice::kUltra;
  return Choice::kAuto;
}

constexpr std::string_view LevelName(Level level) {
  return level == Level::kOff ? "off" : level == Level::kHigh ? "high" : "ultra";
}

constexpr Level LevelOfCell(std::string_view cell) { return cell == "high" ? Level::kHigh : cell == "ultra" ? Level::kUltra : Level::kOff; }

// The level a choice comes to on a class ALONE (no ray tracing tier: LevelForRung below is the one the renderer and the menu use). `auto` is gpu_class.h's cell for the class, which is older than the
// tier's table and says off for Mid; a hand set level runs on any class that has ray queries (the class rule says what auto picks, not what the player may choose), except Low: a shared memory GPU does
// not run it at all.
constexpr Level LevelFor(gpu_class::GpuClass cls, Choice choice) {
  if (cls == gpu_class::GpuClass::kLow) return Level::kOff;
  switch (choice) {
    case Choice::kOff:
      return Level::kOff;
    case Choice::kHigh:
      return Level::kHigh;
    case Choice::kUltra:
      return Level::kUltra;
    default:
      return LevelOfCell(gpu_class::Cell(gpu_class::Effect::kRtShadows, cls));
  }
}

// What `auto` comes to on this device, for the pause menu's "AUTO (HIGH)" / "AUTO (OFF)": the class's cell where the device can trace (ray queries, the pass made:
// gpu.h CityRtShadowsAvailable), else off: a High class card without ray queries (RDNA 1, a GTX 1080 Ti is Mid) would otherwise say HIGH and do nothing.
constexpr Level AutoLevelFor(gpu_class::GpuClass cls, bool available) { return available ? LevelFor(cls, Choice::kAuto) : Level::kOff; }

// The ray tracing tier's say (rt_tier.h): a card that does not trace in hardware (the tier none: a GTX 10 or 16 series card runs DXR in its shaders, far too slowly; the Low class is none too) runs
// nothing, whatever the class and the choice. `auto` is rt_tier.h's cell for the rung (kRtShadows: off, high, high for none, weak, strong), NOT gpu_class.h's per-class cell (off, off, high), which said
// off for a Mid class card that traces in hardware: the two tables disagreed, and the rung's table is the one the launcher's text and the other ray traced effects read, so it decides (a weak card runs
// it, with the Mid budget below). A hand set level is the player's, on any rung that traces. The budgets follow the tier too: the caller reads BudgetFor with gpu_class::RtClass (a weak card of the High
// class gets the Mid budget).
constexpr Level LevelForRung(gpu_class::GpuClass cls, gpu_class::RtTier tier, Choice choice) {
  const gpu_class::RtTier rung = gpu_class::RtRung(cls, tier);
  if (tier == gpu_class::RtTier::kNone) return Level::kOff;
  if (choice == Choice::kAuto) return LevelOfCell(gpu_class::RtCell(gpu_class::RtEffect::kRtShadows, rung));
  return LevelFor(cls, choice);
}
constexpr Level AutoLevelForRung(gpu_class::GpuClass cls, gpu_class::RtTier tier, bool available) { return available ? LevelForRung(cls, tier, Choice::kAuto) : Level::kOff; }

// A mesh the worker read WITHOUT the hit blob (the plan asked before the hit data's pool existed) must not be built once the pool is there: its structure would be geometry only, the sector would have
// no hit data, and the read that does bring the blob (the plan asks again after the pool's epoch) is dropped because the sector is resident. It is given back to be read again, with the blob. Where the
// pool is not ready a mesh without the blob is the right one (M1's), and a read that asked for the blob and found none keeps its mesh (the file's, not the order of events).
constexpr bool ReadBeforePool(bool pool_ready, bool read_with_hit) { return pool_ready && !read_with_hit; }

struct Budget {
  float hi_radius = 0.0f;           // metres from the camera to a sector's box: built at its full detail within it
  float far_radius = 0.0f;          // ... at the far level of detail within it (the long shadows of a low sun: casters up to a kilometre away)
  float hysteresis = 0.0f;          // metres added to a radius before a built sector is given back (no building and giving back at a boundary)
  uint64_t memory_cap = 0;          // bytes the structures (hi and lod) may hold
  uint32_t build_triangles = 0;     // triangles built into structures in one frame (a sector bigger than this is built alone)
  uint32_t loads_in_flight = 0;     // pack reads being decoded at once
  uint64_t upload_bytes = 0;        // bytes staged for the structures' builds in one frame
  uint64_t pool_cap = 0;            // CITY-RT M2 H3: bytes of the hit data's pool (city_hit_pool_rules.h) the sectors may hold; 0 = no hit data, the pool is not counted
  bool on() const { return hi_radius > 0.0f; }
};

inline constexpr uint64_t kMiB = 1ull << 20;
// Structure bytes a triangle, PREFER_FAST_TRACE without compaction (the cars' measure was 57 to 92: car_rt_scene.h).
inline constexpr uint64_t kStructureBytesPerTriangle = 96;

constexpr uint64_t EstimateStructureBytes(uint32_t triangles) { return uint64_t(triangles) * kStructureBytesPerTriangle; }

// The class's budget at a level. Mid (an RT card with under 5.5 GiB) at high: the sectors within 150 m, about 45 to 65 MB of structures; High at high: 300 m,
// 120 to 180 MB; High at ultra: 600 m, 240 to 320 MB. The caps leave room for the player's cars (car_rt_scene_rules.h kMemoryCapBytes is 64 MiB of its own).
constexpr Budget BudgetFor(gpu_class::GpuClass cls, Level level) {
  if (level == Level::kOff || cls == gpu_class::GpuClass::kLow) return Budget{};
  if (level == Level::kUltra && cls == gpu_class::GpuClass::kHigh) return Budget{600.0f, 3000.0f, 100.0f, 512 * kMiB, 200000, 2, 14 * kMiB};
  if (cls == gpu_class::GpuClass::kHigh) return Budget{300.0f, 2000.0f, 60.0f, 256 * kMiB, 120000, 2, 12 * kMiB};
  if (level == Level::kUltra) return Budget{300.0f, 2000.0f, 60.0f, 192 * kMiB, 100000, 1, 8 * kMiB};  // a Mid card asked for ultra by hand
  return Budget{150.0f, 1200.0f, 30.0f, 96 * kMiB, 60000, 1, 6 * kMiB};
}

// mcla_gpu_city_rt_radius (debug, metres, 0 = the class's): the hi detail radius, with the far radius following it (at least 4 times, never under the class's own).
constexpr Budget WithRadius(Budget budget, float radius) {
  if (radius <= 0.0f || !budget.on()) return budget;
  budget.hi_radius = std::clamp(radius, 25.0f, 1500.0f);
  budget.far_radius = std::max(budget.far_radius, budget.hi_radius * 4.0f);
  budget.hysteresis = std::max(15.0f, budget.hi_radius * 0.2f);
  return budget;
}

// ---------------------------------------------------------------------------
// the instance: mask and id
// ---------------------------------------------------------------------------

// The instance mask of every city structure. The car scene's masks are 0x01 to 0x40 (car_rt_scene_rules.h; 0x20 and 0x40 are the other cars' chrome and paint
// sub-masks), so 0x80 is the one bit left in the instance mask's eight: the city (hi and lod alike, never both of one sector) is ONE class, and a pass that wants
// the city and the cars traces with kMaskCity | the cars' parts. The survey's finer split (hi, lod, foliage) is not available in eight bits; the geometry's
// opaque flag (opaque and alpha tested triangles are two geometries of a structure) and the instance id say the rest. None of the car passes traces 0x80.
inline constexpr uint32_t kMaskCity = 0x80;
// InstanceID (24 bits): the city's ids have bit 23 set so that a car's table row (a small number) is never mistaken for one; then the sector and the detail.
inline constexpr uint32_t kInstanceIdFlag = 0x800000u;
enum class Detail : uint8_t { kHi = 0, kLod = 1 };
constexpr uint32_t InstanceId(uint32_t sector, Detail detail) { return kInstanceIdFlag | ((sector & 0x3FFFFu) << 1) | (detail == Detail::kLod ? 1u : 0u); }
constexpr bool IsCityInstance(uint32_t instance_id) { return (instance_id & kInstanceIdFlag) != 0; }
constexpr uint32_t SectorOf(uint32_t instance_id) { return (instance_id >> 1) & 0x3FFFFu; }
constexpr Detail DetailOf(uint32_t instance_id) { return (instance_id & 1u) != 0 ? Detail::kLod : Detail::kHi; }

// ---------------------------------------------------------------------------
// sectors and the streaming plan
// ---------------------------------------------------------------------------

struct SectorInfo {
  float box_min[3] = {0, 0, 0};  // the game's box of the sector (the pack's aabb)
  float box_max[3] = {0, 0, 0};
  uint32_t hi_triangles = 0;     // 0: the sector has no hi mesh
  uint32_t lod_triangles = 0;    // 0: no lod mesh
  uint64_t hi_pool = 0;          // CITY-RT M2 H3: the estimate of the sector detail's pool block (city_hit_pool_rules.h EstimateBlockBytes); counted only while the budget has a pool_cap
  uint64_t lod_pool = 0;
  bool has_hi() const { return hi_triangles != 0; }
  bool has_lod() const { return lod_triangles != 0; }
};

enum class Residency : uint8_t {
  kNone,      // nothing: not read, or given back
  kLoading,   // the pack is being read and decoded (or the mesh waits to be built)
  kResident,  // the structure is built (or recorded to be)
  kFailed,    // a read or a build failed: not tried again before `retry_frame`
};

struct SectorState {
  Residency hi = Residency::kNone;
  Residency lod = Residency::kNone;
  uint64_t hi_bytes = 0;         // what the structure holds (the device's size once built, the estimate while loading)
  uint64_t lod_bytes = 0;
  uint64_t hi_retry_frame = 0;   // for kFailed
  uint64_t lod_retry_frame = 0;
  uint64_t hi_pool_held = 0;     // what the sector detail's pool block holds (the estimate while loading, the block's real size once built); 0 with no hit data
  uint64_t lod_pool_held = 0;
};

// The distance from a point to a box (0 inside): the same measure the game's own sector cull uses (sub_822D7D00: the box's distance), so the ring is the one the
// game draws its hi detail by.
inline float DistanceToBox(const double p[3], const float lo[3], const float hi[3]) {
  double sum = 0.0;
  for (int a = 0; a < 3; ++a) {
    const double below = double(lo[a]) - p[a];
    const double above = p[a] - double(hi[a]);
    const double d = std::max(std::max(below, above), 0.0);
    sum += d * d;
  }
  return float(std::sqrt(sum));
}

struct Request {
  uint32_t sector = 0;
  Detail detail = Detail::kHi;
  float distance = 0.0f;
  bool operator==(const Request& o) const { return sector == o.sector && detail == o.detail; }
};

struct Plan {
  std::vector<Request> loads;      // to read next, in the order to read them
  std::vector<Request> evictions;  // to give back now
  // what the plan saw, for the log and the tests
  uint32_t wanted_hi = 0;
  uint32_t wanted_lod = 0;
  uint64_t resident_bytes = 0;
  uint64_t resident_pool_bytes = 0;  // the pool blocks of what is built (0 with no pool cap)
  bool over_cap = false;           // something wanted could not be had for the cap
  bool over_pool_cap = false;      // ... and the pool's cap was the one in the way (it can also be both)
};

struct PlanInput {
  double camera[3] = {0, 0, 0};    // world position
  uint64_t frame = 0;
  Budget budget;
  uint32_t loading = 0;            // reads in flight now (they count against loads_in_flight and their estimated bytes against the cap)
};

// Which sectors to read and give back. Pure: the same inputs give the same plan.
//   - hi detail is wanted within hi_radius (within hi_radius + hysteresis once it is built or loading), the lod within far_radius (+ hysteresis);
//   - a lod mesh is read before any hi mesh (it is a few hundred triangles; it keeps a sector in the top level while its hi detail is built) and the nearest
//     of each first; at most `loads_in_flight - loading` reads are asked for;
//   - what is built and no longer wanted is given back; over the memory cap the farthest of the unwanted go first, then a wanted sector that is farther than the one
//     that needs the room (never a nearer one), and a load that finds no room is not asked for (over_cap).
inline Plan PlanStreaming(const PlanInput& in, const std::vector<SectorInfo>& sectors, const std::vector<SectorState>& states);

// ---------------------------------------------------------------------------
// the frame's builds
// ---------------------------------------------------------------------------

struct BuildItem {
  uint32_t triangles = 0;
  uint64_t upload_bytes = 0;   // positions and indices of the mesh, staged
  float distance = 0.0f;       // to the camera: nearest first
};

// Which of the meshes waiting to be built are built in this frame: nearest first while the frame's triangles and staged bytes last, and always the nearest one
// even when it alone is over them (a sector of 262,000 triangles must not starve behind a budget of 60,000). The result has one flag a mesh, in the input's order.
inline std::vector<uint8_t> PlanBuilds(const std::vector<BuildItem>& items, uint32_t triangle_budget, uint64_t upload_budget);

// ---------------------------------------------------------------------------
// the top level's content
// ---------------------------------------------------------------------------

// Which structure of a sector the top level holds: the hi detail when it is built, else the lod when that is, else none. Never both (a sector's lod model is
// the same buildings at a coarser level: both in the top level would shadow the surface from itself).
inline bool PickInstance(const SectorState& state, Detail* detail) {
  if (state.hi == Residency::kResident) {
    *detail = Detail::kHi;
    return true;
  }
  if (state.lod == Residency::kResident) {
    *detail = Detail::kLod;
    return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// the implementation (inline: this header is the test's whole build)
// ---------------------------------------------------------------------------

inline Plan PlanStreaming(const PlanInput& in, const std::vector<SectorInfo>& sectors, const std::vector<SectorState>& states) {
  Plan plan;
  const Budget& b = in.budget;
  if (!b.on() || sectors.size() != states.size()) {
    // switched off: everything built goes back
    for (uint32_t i = 0; i < states.size(); ++i) {
      if (states[i].hi == Residency::kResident) plan.evictions.push_back({i, Detail::kHi, 0.0f});
      if (states[i].lod == Residency::kResident) plan.evictions.push_back({i, Detail::kLod, 0.0f});
    }
    return plan;
  }
  struct Candidate {
    uint32_t sector;
    Detail detail;
    float distance;
    uint64_t bytes;
    uint64_t pool;  // the pool block's bytes (0 without a pool cap)
    bool resident;  // built (false: still loading)
    bool wanted;
  };
  const bool pooled = b.pool_cap != 0;
  std::vector<Candidate> load_hi, load_lod;
  std::vector<Candidate> held;  // resident or loading, wanted or not
  uint64_t held_bytes = 0;
  uint64_t held_pool = 0;
  for (uint32_t i = 0; i < sectors.size(); ++i) {
    const SectorInfo& s = sectors[i];
    const SectorState& st = states[i];
    const float d = DistanceToBox(in.camera, s.box_min, s.box_max);
    const auto consider = [&](Detail detail, bool has, Residency residency, uint64_t bytes, uint64_t pool_held, uint64_t retry, uint32_t triangles, uint64_t pool_estimate, float radius) {
      const bool present = residency == Residency::kResident || residency == Residency::kLoading;
      const bool wanted = has && d <= (present ? radius + b.hysteresis : radius);
      if (wanted) (detail == Detail::kHi ? plan.wanted_hi : plan.wanted_lod) += 1;
      if (present) {
        const uint64_t pool = pooled ? pool_held : 0;
        held.push_back({i, detail, d, bytes, pool, residency == Residency::kResident, wanted});
        held_bytes += bytes;
        held_pool += pool;
        if (residency == Residency::kResident) {
          plan.resident_bytes += bytes;
          plan.resident_pool_bytes += pool;
        }
      } else if (wanted && (residency == Residency::kNone || (residency == Residency::kFailed && in.frame >= retry))) {
        (detail == Detail::kHi ? load_hi : load_lod).push_back({i, detail, d, EstimateStructureBytes(triangles), pooled ? pool_estimate : 0, false, true});
      }
    };
    consider(Detail::kHi, s.has_hi(), st.hi, st.hi_bytes, st.hi_pool_held, st.hi_retry_frame, s.hi_triangles, s.hi_pool, b.hi_radius);
    consider(Detail::kLod, s.has_lod(), st.lod, st.lod_bytes, st.lod_pool_held, st.lod_retry_frame, s.lod_triangles, s.lod_pool, b.far_radius);
  }
  const auto nearest_first = [](const Candidate& a, const Candidate& c) { return a.distance != c.distance ? a.distance < c.distance : a.sector < c.sector; };
  std::sort(load_hi.begin(), load_hi.end(), nearest_first);
  std::sort(load_lod.begin(), load_lod.end(), nearest_first);

  // 1. What is built and no longer wanted goes back. A read in flight is not cancelled: when it arrives and is still unwanted, the next plan gives it back.
  for (Candidate& c : held) {
    if (c.wanted || !c.resident) continue;
    plan.evictions.push_back({c.sector, c.detail, c.distance});
    held_bytes -= std::min(held_bytes, c.bytes);
    held_pool -= std::min(held_pool, c.pool);
    c.resident = false;
    c.wanted = false;
    c.bytes = 0;
    c.pool = 0;
  }

  // 2. The reads: the lod first, then the hi detail, nearest first, within the reads in flight and the cap. A wanted hi structure farther than the one that
  // needs the room can be traded for it (farthest first); the lod structures are the safety net and are never traded.
  uint32_t slots = b.loads_in_flight > in.loading ? b.loads_in_flight - in.loading : 0;
  std::vector<size_t> tradable;
  for (size_t k = 0; k < held.size(); ++k) {
    if (held[k].wanted && held[k].resident && held[k].detail == Detail::kHi) tradable.push_back(k);
  }
  std::sort(tradable.begin(), tradable.end(), [&](size_t a, size_t c) { return held[a].distance > held[c].distance; });
  size_t next_tradable = 0;
  const auto ask = [&](const std::vector<Candidate>& list) {
    for (const Candidate& c : list) {
      if (slots == 0) return;
      // Room: the structures' memory and, with hit data, the pool's. Either may be the one that is short; a victim gives back both.
      const auto short_of_room = [&] { return held_bytes + c.bytes > b.memory_cap || (pooled && held_pool + c.pool > b.pool_cap); };
      while (short_of_room() && next_tradable < tradable.size() && held[tradable[next_tradable]].distance > c.distance) {
        const Candidate& victim = held[tradable[next_tradable++]];
        plan.evictions.push_back({victim.sector, victim.detail, victim.distance});
        held_bytes -= std::min(held_bytes, victim.bytes);
        held_pool -= std::min(held_pool, victim.pool);
      }
      if (short_of_room()) {
        plan.over_cap = true;
        plan.over_pool_cap = plan.over_pool_cap || (pooled && held_pool + c.pool > b.pool_cap);
        continue;  // no room for this one; a smaller or nearer one behind it may still fit
      }
      plan.loads.push_back({c.sector, c.detail, c.distance});
      held_bytes += c.bytes;
      held_pool += c.pool;
      --slots;
    }
  };
  ask(load_lod);
  ask(load_hi);
  return plan;
}

inline std::vector<uint8_t> PlanBuilds(const std::vector<BuildItem>& items, uint32_t triangle_budget, uint64_t upload_budget) {
  std::vector<uint8_t> built(items.size(), 0);
  std::vector<size_t> order(items.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t c) { return items[a].distance < items[c].distance; });
  uint64_t triangles = 0, bytes = 0;
  bool first = true;
  for (const size_t i : order) {
    const BuildItem& item = items[i];
    if (!first && (triangles + item.triangles > triangle_budget || bytes + item.upload_bytes > upload_budget)) continue;  // a smaller one behind may still fit
    triangles += item.triangles;
    bytes += item.upload_bytes;
    built[i] = 1;
    first = false;
  }
  return built;
}

// ---------------------------------------------------------------------------
// where a ray traced shadow may be applied
// ---------------------------------------------------------------------------
// The RECEIVER of a shadow is the surface the depth image shows, which is the geometry the GAME drew there: its hi detail within `game_hi_radius` of the camera
// (300 m times lod_city_scale, the owner's auto is 2x) and its lod beyond. A ray from a receiver that the pack holds at another level of detail starts under or
// above the structure's surface (the lod of a building is a few metres off its hi detail) and the pixel is shadowed by its own surface: acne. So a shadow is
// applied only within a range where the structures are the hi detail the game drew: the radius of the hi ring, the game's hi radius, and the distance of the
// nearest sector whose hi structure is wanted but not built yet (until it is, the pixels that could be in it, and everything beyond it, keep the game's shadows).
// The casters beyond are the lod, which is all a caster needs.
inline float SafeReceiverRange(const PlanInput& in, const std::vector<SectorInfo>& sectors, const std::vector<SectorState>& states, float game_hi_radius) {
  float range = std::min(in.budget.hi_radius, game_hi_radius);
  if (!in.budget.on() || sectors.size() != states.size()) return 0.0f;
  for (uint32_t i = 0; i < sectors.size(); ++i) {
    if (!sectors[i].has_hi() || states[i].hi == Residency::kResident) continue;
    const float d = DistanceToBox(in.camera, sectors[i].box_min, sectors[i].box_max);
    if (d < range) range = d;
  }
  return std::max(range, 0.0f);
}

}  // namespace mcla::gpu::city_rt
