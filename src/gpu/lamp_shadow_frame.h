#pragma once
// ===========================================================================
// MCLA GPU renderer -- the street lamps' shadow views of a frame, as the deferred light pass reads them (perf/LAMP-SHADOWS-2.md, package LAMP-SHADOWS-4)
// ===========================================================================
// Plain data (no renderer, no guest, no SDK): the lamp provider (lamp_shadows.h) fills it, local_lights_rules.h writes it into the pass's buffer, hlsl/local_light_ps.hlsl reads it and
// light_shadow_lookup.h's ONE lookup is run over it. It is NOT part of the frozen LightFrame's ShadowFrame (that holds 8 entries for the cars' headlamps; a lamp's pair of layers takes
// two entries and the level wants up to ten lamps), so a lamp's views live in a table of their own and the lamp's ShadowRef names an entry of THIS table:
//
//   ShadowRef::first_view of a light whose source is kLamp  =  kLampEntryBase + the row of its first entry in LampShadowFrame (the pass's combined entry space: 0..7 the headlights'
//                                                            table, 8.. the lamps'), view_count 1 or 2 as for a car (a lamp has ONE view: its static layer and, when anything that moves
//                                                            is in its cone, the live layer after it: the ONE combine rule multiplies the two: light_shadow_lookup.h LightLit).
//
// THE LAYERS. A street lamp's map is two tiles of the atlas: the STATIC one holds the world's casters that do not move (drawn once when the lamp takes its tile and added to as the main
// view shows more of the world, kept), the LIVE one the casters that do (cars, people), cleared and drawn again every frame (lamp_shadows_rules.h). The two entries share the lamp's matrix
// up to the tile's place in the atlas (WorldToShadow takes the tile's uv), so each is a LocalShadowView as it is: 96 bytes, field for field. A lamp whose cone holds nothing that moves publishes
// the static entry alone; with the cache off one entry (kSingle) holds everything.
//
// THE REACH. A lamp's shadow is made for the reach sqrt(8 I) clamped to 14..30 m (lamp_shadows_rules.h ShadowReach) and fades out over the last fifth of it (the map ends there; an edge
// at the map's far plane would show). The reach rides in the entry's `light_index` field as the bits of a float (LampReach / SetLampReach): nothing reads that field of a lamp's entry
// otherwise (it is the headlight rig's light index for the cars' views).
//
// THE GPU TABLE (GpuLampTable): a 32 byte header and kMaxLampEntries rows of the 96 byte view, appended to the pass's buffer after the headlights' table (local_lights_rules.h).
// ===========================================================================

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "light_frame.h"
#include "local_shadow_types.h"

namespace larecomp::gpu {

// Lamps that hold a tile at once: the level's wanted lamps and the spare tiles lamps fade out in (ultra: 8 + 2). Two entries each at most (static + live).
inline constexpr uint32_t kMaxLampSlots = 10;
inline constexpr uint32_t kMaxLampEntries = 2 * kMaxLampSlots;
// ShadowRef::first_view of a lamp's light is this plus its row in LampShadowFrame (the pass's combined entry space).
inline constexpr uint32_t kLampEntryBase = kMaxLightShadowViews;

struct LampShadowFrame {
  ShadowAtlasInfo atlas;  // the shared atlas's bindless slot and size (the same texture the headlights' views are in)
  std::array<LocalShadowView, kMaxLampEntries> views{};
  std::array<uint64_t, kMaxLampEntries> view_light{};      // the id of the lamp each entry is of (light_id.h LampId)
  std::array<ShadowLayer, kMaxLampEntries> view_layer{};
  uint32_t count = 0;

  // The row of the first entry of the lamp with this id, or -1.
  int32_t FindFirst(uint64_t id) const {
    for (uint32_t e = 0; e < count && e < kMaxLampEntries; ++e) {
      if (view_light[e] == id && id != kNoLightId) {
        return int32_t(e);
      }
    }
    return -1;
  }
  bool empty() const { return count == 0 || !atlas.valid; }
};

// The reach of a lamp's entry, in metres (see THE REACH above).
inline float LampReach(const LocalShadowView& v) { return std::bit_cast<float>(v.light_index); }
inline void SetLampReach(LocalShadowView* v, float metres) { v->light_index = std::bit_cast<uint32_t>(metres); }

// A shadow fades out from this share of its reach to the reach itself (a smoothstep of the distance from the lamp).
inline constexpr float kLampFadeFrom = 0.8f;

// ---------------------------------------------------------------------------
// the table the pass reads
// ---------------------------------------------------------------------------

struct alignas(16) GpuLampHeader {
  uint32_t count = 0;        // entries
  uint32_t atlas_slot = kNoLightSlot;
  uint32_t atlas_width = 0;
  uint32_t atlas_height = 0;
  uint32_t layers_lo = 0;    // two bits for each entry (ShadowLayer: 0 single, 1 static, 2 live): entries 0..15
  uint32_t layers_hi = 0;    // entries 16..31
  uint32_t reserved0 = 0;
  uint32_t reserved1 = 0;
};
struct GpuLampTable {
  GpuLampHeader header;
  std::array<LocalShadowView, kMaxLampEntries> views{};
};
static_assert(sizeof(GpuLampHeader) == 32 && alignof(GpuLampHeader) == 16, "the header is two float4");
static_assert(sizeof(LocalShadowView) == 96, "a view row is 96 bytes");
static_assert(sizeof(GpuLampTable) == 32 + 96 * kMaxLampEntries && offsetof(GpuLampTable, views) == 32, "header, then the rows");
static_assert(kMaxLampEntries <= 32, "two bits an entry in two words");
static_assert(std::is_trivially_copyable_v<GpuLampTable>);
inline constexpr uint32_t kGpuLampHeaderBytes = 32;
inline constexpr uint32_t kGpuLampViewBytes = 96;

// The table of a frame's lamp entries (the empty table when there is no valid atlas).
inline void BuildGpuLampTable(const LampShadowFrame& frame, GpuLampTable* out) {
  *out = GpuLampTable{};
  if (!frame.atlas.valid) {
    return;
  }
  const uint32_t n = frame.count < kMaxLampEntries ? frame.count : kMaxLampEntries;
  out->header.count = n;
  out->header.atlas_slot = frame.atlas.slot;
  out->header.atlas_width = frame.atlas.width;
  out->header.atlas_height = frame.atlas.height;
  for (uint32_t e = 0; e < n; ++e) {
    out->views[e] = frame.views[e];
    const uint32_t bits = uint32_t(frame.view_layer[e]) & 3u;
    (e < 16 ? out->header.layers_lo : out->header.layers_hi) |= bits << (2u * (e & 15u));
  }
}

}  // namespace larecomp::gpu
