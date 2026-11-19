#pragma once
// ===========================================================================
// MCLA GPU renderer -- the traffic's and the parked cars' own model data: the walk of an ambient car's render item down to its geometries, and its bones (RT-MODEL-PARTS)
// ===========================================================================
// The racers' cars are mcCarModel vehicles (car_model_walk_rules.h). The traffic and the parked cars are not: they are the items of the renderer's slot-9 client
// (mcAmbientCarRenderClass, the object whose vtable is 0x820D8C0C; perf/RT-MODEL-SURVEY.md section 3, gpu/traffic_shadow.h and traffic_shadow_pose.h, and the draw's asm in
// h11-work/rt-model-parts/asm/827914B8.txt, read again for this file). This header is that chain as pure functions over a memory reader (the `Mem` of
// car_model_walk_rules.h); car_model_traffic_rules_test.cpp feeds it made-up bytes.
//
// THE CHAIN   (offsets decimal; every word big endian)
//   renderer R = [0x8287E064];  client C = [R + 128 + 4 * 9]  (its first word is the vtable 0x820D8C0C)
//   THE LIST IS TWO LEVELS (found 2026-11-19 from the first check's "41 render items, 41 with no readable car"): C + 8 is the list of the client's MODEL nodes (one a car type: the
//              object whose vtable is 0x820D9084, vtbl+8 = sub_822E6CB8 = the generic "draw my children"), next at +12; a model node's +16 is its parent, the client; a model node's
//              +20 is the head of ITS ITEMS (the cars of that type), the next sibling of an item at +8 (sub_822E6CB8: `lwz r31,20(r3)` ... `lwz r31,8(r31)`, calling vtbl+8 of each
//              child = the item draw sub_82793360). The first version took the model nodes for items and read a car out of the client (+16) of each: every one failed.
//   item I     +8   the next item of the same model   +16  the car object (mcAmbientCar)         +20  the MODEL (the node it hangs under)          +24  -> the item's blocks table
//              +28  i32 lod: negative = outside the main view or deeper than 500 m (the draw skips it); 0..2; 2 = the coarsest (the body phase returns)
//              +32  float: the view depth + the car's radius (the lod update's frustum test)   +36 u8 enabled   +52 u16 colour index (into the model's palette)
//   car        +128 the world Matrix44, the update copy (rows right, up, back, position; row 3 at +176); the render thread's copy is at +(buffer + 4) * 64
//              +448 the car's type   +492 float radius   +500 u8 flags: 0x40 set, 0x20 and 0x80 clear for a car the draw takes (gpu/traffic_shadow.h AmbientRejectBits)
//   model M    +160 + 4 lod   -> a holder whose +0 is the lod's DRAWABLE (asm sub_827914B8: lwz r9,(40+lod)*4(r31); lwz r25,0(r9)); the drawable's +8 is the LOD object as for a
//              racer's part (L -> models -> geometries: the same walk, detail::WalkLod); a holder may carry the placed bits at +8 as a table entry does
//              +172 the type object (a shader group)   +236..+252 the paint palette: float scale at +240, an array of vec4 at +244 indexed by the item's colour index
//   THE POSE    block B = [[I + 24] + 4 * (buffer + 2)], buffer = [0x827E5640] (0..2); B + 16 a vec4: the car's ROOT translation; B + 32 + 48 b the bone b's 3 x 4 matrix, a row an
//               output component (x' = row0 . p + row0.w): p' = M p + t. The vertex shader skins by the bone then multiplies by gWorldViewProj with World = a translation by the root,
//               so the model-to-world of a geometry whose model's bone is b is  p_world = root + M_b p + t_b  (gpu/traffic_shadow_pose.h; in the row vector form the matrix
//               is the transpose of M with the translation row root + t_b).
// A traffic car is ONE drawable (the body and the four wheels as models with their own bones), so the wheels need no walk of their own; the driver is not in it.
//
// WHAT IS NOT VERIFIED (nothing here has run): the whole chain is read from the code and the traffic shadows' log; the bone matrices' freshness for an item the main view does
// not draw (its lod is negative), the meaning of the bone index (the draw may remap it: the racers' parts were direct), the position's type (float3 at offset 0 of the
// ambient vertex?) and the palette colour. The walker checks the placement against the draws of the traffic cars the camera does see (car_model_parts_rt_rules.h
// TrafficCalibration: three hypotheses) and places no traffic car until one fits within 0.35 m.
// ===========================================================================

#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

#include "car_model_walk_rules.h"
#include "temporal_math.h"

namespace mcla::gpu::car_model_walk {

inline constexpr uint32_t kAmbRenderer = 0x8287E064u;
inline constexpr uint32_t kAmbRendererClients = 128;
inline constexpr uint32_t kAmbClientSlot = 9;
inline constexpr uint32_t kAmbClientVtable = 0x820D8C0Cu;
inline constexpr uint32_t kAmbClientModels = 8;      // client + 8: the list of the model nodes
inline constexpr uint32_t kAmbModelNext = 12;        // a model node's next in the client's list
inline constexpr uint32_t kAmbModelItems = 20;       // a model node's first item
inline constexpr uint32_t kAmbItemSibling = 8;       // an item's next item under the same model
inline constexpr uint32_t kAmbModelVtable = 0x820D9084u;  // the model node's vtable (the retail image): the discriminator of a node
inline constexpr uint32_t kAmbItemVtable = 0x820D9254u;   // the item's vtable (vtbl+8 = the item draw sub_82793360)
inline constexpr uint32_t kAmbItemCar = 16;
inline constexpr uint32_t kAmbItemModel = 20;
inline constexpr uint32_t kAmbItemBlocks = 24;
inline constexpr uint32_t kAmbItemLod = 28;
inline constexpr uint32_t kAmbItemDepth = 32;
inline constexpr uint32_t kAmbItemEnabled = 36;
inline constexpr uint32_t kAmbItemColour = 52;
inline constexpr uint32_t kAmbCarMatrix = 128;
inline constexpr uint32_t kAmbCarType = 448;
inline constexpr uint32_t kAmbCarRadius = 492;
inline constexpr uint32_t kAmbCarFlags = 500;
inline constexpr uint32_t kAmbModelLods = 160;       // + 4 lod
inline constexpr uint32_t kAmbModelScale = 240;
inline constexpr uint32_t kAmbModelPalette = 244;
inline constexpr uint32_t kAmbBufferGlobal = 0x827E5640u;
inline constexpr uint32_t kAmbBlockRoot = 16;
inline constexpr uint32_t kAmbBlockBones = 32;
inline constexpr uint32_t kAmbBoneBytes = 48;
inline constexpr uint32_t kAmbMaxBones = 64;          // the palette holds 64 bones (c64..c255, three registers a bone)
inline constexpr uint32_t kAmbMaxItems = 512;
inline constexpr uint32_t kAmbMaxModels = 256;        // the most model nodes walked (the retail game has about forty)
inline constexpr uint32_t kAmbPaletteEntries = 256;   // the most colour indices read

// ---------------------------------------------------------------------------
// the items
// ---------------------------------------------------------------------------

struct AmbientScan {
  bool renderer = false, client = false;
  uint32_t client_object = 0;
  uint32_t models = 0;          // the client's model nodes walked
  uint32_t items = 0;           // items listed (under all the models)
  uint32_t flat_items = 0;      // nodes of the client's own list that are items themselves (a flat list: none expected)
  uint32_t foreign_nodes = 0;   // nodes that are neither a model nor an item (left out)
  uint32_t mismatched = 0;      // items whose +20 (their model) is not the node they hang under (listed all the same: the item draw reads +20)
  bool truncated = false;       // a cap was hit
};

// The items of the traffic client, in the game's order: the client's model nodes, and under each the items of that model. A list read while the game changes it gives an odd
// line at worst: bounded by kAmbMaxModels, kAmbMaxItems and by readable memory, and a node or an item seen twice ends its list.
template <class Mem>
std::vector<uint32_t> ListAmbientItems(const Mem& mem, AmbientScan* scan) {
  std::vector<uint32_t> items;
  uint32_t renderer = 0, client = 0, vtable = 0;
  if (!mem.Word(kAmbRenderer, &renderer) || !Plausible(renderer)) {
    return items;
  }
  scan->renderer = true;
  if (!mem.Word(renderer + kAmbRendererClients + 4u * kAmbClientSlot, &client) || !Plausible(client) || !mem.Word(client, &vtable) || vtable != kAmbClientVtable) {
    return items;
  }
  scan->client = true;
  scan->client_object = client;
  uint32_t node = 0;
  if (!mem.Word(client + kAmbClientModels, &node)) {
    return items;
  }
  std::unordered_set<uint32_t> seen_nodes, seen_items;
  const auto add_item = [&](uint32_t item, uint32_t model) {
    uint32_t back = 0;
    if (model != 0 && mem.Word(item + kAmbItemModel, &back) && back != model) {
      ++scan->mismatched;
    }
    items.push_back(item);
    ++scan->items;
  };
  while (Plausible(node) && seen_nodes.insert(node).second) {
    if (scan->models >= kAmbMaxModels) {
      scan->truncated = true;
      break;
    }
    ++scan->models;
    uint32_t node_vtable = 0, first = 0, next = 0;
    mem.Word(node, &node_vtable);
    if (node_vtable == kAmbItemVtable) {
      // a node that is an item itself: taken as one (and no children walked)
      if (seen_items.insert(node).second) {
        if (items.size() >= kAmbMaxItems) {
          scan->truncated = true;
          break;
        }
        ++scan->flat_items;
        add_item(node, 0);
      }
    } else if (!mem.Word(node + kAmbModelItems, &first)) {
      ++scan->foreign_nodes;
    } else {
      // the model's items; a node whose first child does not point back to it is not a model node (nothing of its list is taken)
      uint32_t back = 0;
      const bool model = first == 0 || (Plausible(first) && mem.Word(first + kAmbItemModel, &back) && back == node) || node_vtable == kAmbModelVtable;
      if (!model) {
        ++scan->foreign_nodes;
      } else {
        for (uint32_t item = first; Plausible(item) && seen_items.insert(item).second;) {
          if (items.size() >= kAmbMaxItems) {
            scan->truncated = true;
            break;
          }
          add_item(item, node);
          uint32_t sibling = 0;
          if (!mem.Word(item + kAmbItemSibling, &sibling)) {
            break;
          }
          item = sibling;
        }
      }
    }
    if (scan->truncated || !mem.Word(node + kAmbModelNext, &next)) {
      break;
    }
    node = next;
  }
  return items;
}

// Why an item could not be read (the walker counts them: the check's first look at a list that gave no car was "41 with no readable car" and nothing to say which link failed).
enum class AmbientFail : uint8_t { kNone, kItem, kCar, kModel, kMatrixRead, kMatrixPose };
inline constexpr uint32_t kAmbientFailCount = 6;
inline const char* AmbientFailName(AmbientFail f) {
  switch (f) {
    case AmbientFail::kNone: return "readable";
    case AmbientFail::kItem: return "no item";
    case AmbientFail::kCar: return "item+16 is no car pointer";
    case AmbientFail::kModel: return "item+20 is no model pointer";
    case AmbientFail::kMatrixRead: return "the car's matrix is not readable";
    case AmbientFail::kMatrixPose: return "the car's matrix is not a pose";
  }
  return "?";
}

// What the item and its car say before a walk.
struct AmbientItem {
  bool ok = false;               // the chain item -> car and item -> model was readable and the car's matrix is a car's pose
  AmbientFail fail = AmbientFail::kNone;
  std::string why;
  uint32_t item = 0, car = 0, model = 0, blocks = 0, type = 0;
  int32_t lod = -1;              // item+28: negative = the main view does not show the car
  bool enabled = false;          // item+36
  uint32_t car_flags = 0;        // car+500 (a byte)
  bool car_ok = false;           // bit 0x40 set, 0x20 and 0x80 clear
  uint32_t colour_index = 0;     // item+52
  float depth = 0.0f;
  float radius = 0.0f;           // car+492
  temporal::Mat4 matrix;         // car+128: the update copy
  double position[3] = {};
  bool Eligible() const { return ok && enabled && car_ok; }
  bool Visible() const { return lod >= 0; }
};

// True when the rows are a car's pose: finite, a position within a million metres, three axes about a metre long that span a volume (car_rt_memory_rules.h UsableRows says
// the same of a vehicle's; this copy keeps this header free of the memory rules).
inline bool UsableCarMatrix(const temporal::Mat4& m) {
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 3; ++c) {
      if (!std::isfinite(m.m[r][c])) {
        return false;
      }
    }
  }
  for (int c = 0; c < 3; ++c) {
    if (std::fabs(m.m[3][c]) > 1.0e6) {
      return false;
    }
  }
  for (int r = 0; r < 3; ++r) {
    const double length = std::sqrt(m.m[r][0] * m.m[r][0] + m.m[r][1] * m.m[r][1] + m.m[r][2] * m.m[r][2]);
    if (!(length > 0.25 && length < 4.0)) {
      return false;
    }
  }
  return true;
}

// The cheap first look at an item: only its car's position (the update copy's row 3), so that the walker can leave the cars far from the player alone without reading the rest.
// False when the item, its car or the position is not readable or not a point of the world.
template <class Mem>
bool ReadAmbientPosition(const Mem& mem, uint32_t item, double out[3]) {
  uint32_t car = 0;
  if (!Plausible(item) || !mem.Word(item + kAmbItemCar, &car) || !Plausible(car)) {
    return false;
  }
  const uint8_t* t = mem.Window(car + kAmbCarMatrix + kMatrixTranslation, 12);
  if (!t) {
    return false;
  }
  float p[3];
  LoadFloats(t, 3, p);
  for (int c = 0; c < 3; ++c) {
    if (!std::isfinite(p[c]) || std::fabs(p[c]) > 1.0e6f) {
      return false;
    }
    out[c] = double(p[c]);
  }
  return true;
}

template <class Mem>
AmbientItem ReadAmbientItem(const Mem& mem, uint32_t item) {
  AmbientItem out;
  out.item = item;
  if (!Plausible(item)) {
    out.fail = AmbientFail::kItem;
    out.why = "no item";
    return out;
  }
  uint32_t value = 0;
  if (!mem.Word(item + kAmbItemCar, &out.car) || !Plausible(out.car)) {
    out.fail = AmbientFail::kCar;
    out.why = "item+16 holds no car";
    return out;
  }
  if (!mem.Word(item + kAmbItemModel, &out.model) || !Plausible(out.model)) {
    out.fail = AmbientFail::kModel;
    out.why = "item+20 holds no model";
    return out;
  }
  mem.Word(item + kAmbItemBlocks, &out.blocks);
  if (mem.Word(item + kAmbItemLod, &value)) {
    out.lod = int32_t(value);
  }
  if (mem.Byte(item + kAmbItemEnabled, &value)) {
    out.enabled = value != 0;
  }
  if (mem.Half(item + kAmbItemColour, &value)) {
    out.colour_index = value;
  }
  if (const uint8_t* d = mem.Window(item + kAmbItemDepth, 4)) {
    out.depth = BeFloat(d);
  }
  if (mem.Byte(out.car + kAmbCarFlags, &value)) {
    out.car_flags = value;
    out.car_ok = (value & 0x40u) != 0 && (value & 0x20u) == 0 && (value & 0x80u) == 0;
  }
  mem.Word(out.car + kAmbCarType, &out.type);
  if (const uint8_t* d = mem.Window(out.car + kAmbCarRadius, 4)) {
    out.radius = BeFloat(d);
  }
  if (!ReadMatrix(mem, out.car + kAmbCarMatrix, &out.matrix)) {
    out.fail = AmbientFail::kMatrixRead;
    out.why = "the car's matrix is not readable";
    return out;
  }
  if (!UsableCarMatrix(out.matrix)) {
    out.fail = AmbientFail::kMatrixPose;
    out.why = "the car's matrix is not a pose";
    return out;
  }
  for (int c = 0; c < 3; ++c) {
    out.position[c] = out.matrix.m[3][c];
  }
  out.ok = true;
  return out;
}

// ---------------------------------------------------------------------------
// the walk of one item's model
// ---------------------------------------------------------------------------

// The lods to try for a wanted lod: the wanted one, then the FINER ones (the coarsest is the body phase's skip lod: a billboard-grade mesh), then the coarser.
inline std::array<int32_t, kLods> TrafficLodOrder(int32_t wanted) {
  wanted = std::clamp<int32_t>(wanted, 0, int32_t(kLods) - 1);
  std::array<int32_t, kLods> order{};
  uint32_t n = 0;
  order[n++] = wanted;
  for (int32_t l = wanted - 1; l >= 0; --l) {
    order[n++] = l;
  }
  for (int32_t l = wanted + 1; l < int32_t(kLods); ++l) {
    order[n++] = l;
  }
  return order;
}

// What the walk of a model found at a lod (for the log and the cache's signature).
struct AmbientModelRead {
  uint32_t holder = 0, drawable = 0, lod_object = 0, flags = 0;
  bool placed = false;           // the holder's +8 carries the placed bits (soft: the draw does not test it)
  uint32_t models = 0;
};

// The model's drawable at `lod` (its holder, the drawable, the LOD object): false when the chain is not readable or the LOD object holds no models.
template <class Mem>
bool ReadAmbientLod(const Mem& mem, uint32_t model, int32_t lod, AmbientModelRead* out) {
  *out = AmbientModelRead{};
  if (lod < 0 || lod >= int32_t(kLods)) {
    return false;
  }
  if (!mem.Word(model + kAmbModelLods + 4u * uint32_t(lod), &out->holder) || !Plausible(out->holder) || !mem.Word(out->holder + kEntryDrawable, &out->drawable) ||
      !Plausible(out->drawable) || !mem.Word(out->drawable + kDrawableLod, &out->lod_object) || !Plausible(out->lod_object)) {
    return false;
  }
  if (mem.Word(out->holder + kEntryFlags, &out->flags)) {
    out->placed = (out->flags & kPlacedMask) == kPlacedMask;
  }
  const uint8_t* l = mem.Window(out->lod_object, 20);
  if (!l) {
    return false;
  }
  out->models = Be16(l + kLodModelCount);
  return out->models != 0;
}

// Walks the traffic car of `info`: the geometries of the model's lod nearest the wanted one (`options.min_lod`, raised to the item's own lod where the main view shows the
// car) into `out`, one PartEntry (part kModelPart). The CarWalk's vehicle is the ITEM (the object the walker keys on), its entity the car object; kind-specific fields: position
// (the car's matrix row 3), visible (the item's lod is not negative), body_lod (the lod taken). Writes nothing in the guest.
template <class Mem>
void WalkAmbient(const Mem& mem, const AmbientItem& info, const Options& options, CarWalk* out) {
  *out = CarWalk{};
  out->vehicle = info.item;
  if (!info.ok) {
    out->why = info.why.empty() ? "the item is not readable" : info.why;
    return;
  }
  out->entity = info.car;
  out->model_set = info.model;
  out->visible = info.Visible() ? 1u : 0u;
  out->enabled = info.enabled ? 1u : 0u;
  out->position_read = true;
  for (int c = 0; c < 3; ++c) {
    out->position[c] = float(info.position[c]);
  }
  out->entity_matrix = info.matrix;
  out->has_entity_matrix = true;

  const int32_t wanted = std::clamp<int32_t>(std::max<int32_t>(info.lod, options.min_lod), 0, int32_t(kLods) - 1);
  AmbientModelRead pick;
  int32_t picked = -1;
  for (const int32_t candidate : TrafficLodOrder(wanted)) {
    if (ReadAmbientLod(mem, info.model, candidate, &pick)) {
      picked = candidate;
      break;
    }
  }
  if (picked < 0) {
    out->why = "no lod of the model is readable";
    return;
  }
  out->ok = true;
  out->body_lod = picked;
  out->part_lod = picked;
  PartEntry entry;
  entry.part = uint16_t(kModelPart);
  entry.variant = 0;
  entry.lod = uint8_t(picked);
  entry.entry = pick.holder;
  entry.drawable = pick.drawable;
  entry.lod_object = pick.lod_object;
  entry.flags = pick.flags;
  entry.state = kStateLoaded;
  entry.count = 1;
  entry.loaded = true;
  entry.placed = pick.placed;
  entry.valid = true;
  entry.in_use = true;
  entry.models = pick.models;
  entry.first_geometry = 0;
  entry.geometries = detail::WalkLod(mem, pick.lod_object, entry, 0, out);
  ++out->entries_present;
  ++out->entries_loaded;
  ++out->entries_valid;
  out->parts.push_back(entry);
  for (const Geometry& g : out->geometries) {
    ++out->all.geometries;
    out->all.vertices += g.vertex_count;
    out->all.triangles += g.index_count / 3;
    ++out->lod_tally[std::min<uint32_t>(g.lod, kLods - 1)].geometries;
    out->lod_tally[std::min<uint32_t>(g.lod, kLods - 1)].vertices += g.vertex_count;
    out->lod_tally[std::min<uint32_t>(g.lod, kLods - 1)].triangles += g.index_count / 3;
    out->skinned += g.palette_count != 0;
    ++out->in_use.geometries;
    out->in_use.vertices += g.vertex_count;
    out->in_use.triangles += g.index_count / 3;
  }
  if (out->geometries.empty()) {
    out->why = "the lod holds no geometry";
  }
}

// The paint palette's colour of the item (the raw vec4 of the model's array at the item's colour index, and the model's scale): for the log. What the draw makes of it
// (sub_82790A28) is not known, so the scene does not use it; CHECK against a picture.
struct AmbientColour {
  bool read = false;
  float scale = 0.0f;
  float rgba[4] = {};
};

template <class Mem>
AmbientColour ReadAmbientColour(const Mem& mem, const AmbientItem& info) {
  AmbientColour out;
  uint32_t palette = 0;
  if (!mem.Word(info.model + kAmbModelPalette, &palette) || !Plausible(palette) || info.colour_index >= kAmbPaletteEntries) {
    return out;
  }
  const uint8_t* p = mem.Window(palette + 16u * info.colour_index, 16);
  const uint8_t* s = mem.Window(info.model + kAmbModelScale, 4);
  if (!p || !s) {
    return out;
  }
  LoadFloats(p, 4, out.rgba);
  out.scale = BeFloat(s);
  out.read = std::isfinite(out.scale) && std::isfinite(out.rgba[0]) && std::isfinite(out.rgba[1]) && std::isfinite(out.rgba[2]);
  return out;
}

// ---------------------------------------------------------------------------
// the pose
// ---------------------------------------------------------------------------

// A bone as the block holds it: three rows of four floats.
struct AmbientBone {
  float m[3][4] = {};
};

struct TrafficPoseData {
  bool block_ok = false;                 // the block, its root and bone 0 were readable and finite
  uint32_t buffer = 0, block = 0;
  double root[3] = {};
  std::vector<AmbientBone> bones;        // 0 .. the highest bone a geometry uses
  bool car_render_ok = false;            // the car's render-copy matrix (car + (buffer + 4) * 64), the matrix hypothesis kCarMatrix uses
  temporal::Mat4 car_render;
  bool Usable() const { return block_ok && !bones.empty(); }
};

// The highest bone index of a walk's geometries plus one (at most kAmbMaxBones).
inline uint32_t BonesNeeded(const CarWalk& walk) {
  uint32_t n = 0;
  for (const Geometry& g : walk.geometries) {
    if (g.bone != kNoBone && g.bone < kAmbMaxBones) {
      n = std::max(n, g.bone + 1);
    }
  }
  return n;
}

template <class Mem>
TrafficPoseData ReadTrafficPose(const Mem& mem, const AmbientItem& info, uint32_t bones_needed) {
  TrafficPoseData out;
  uint32_t buffer = 0;
  if (!mem.Word(kAmbBufferGlobal, &buffer) || buffer > 3u) {
    return out;
  }
  out.buffer = buffer;
  if (buffer <= 2u) {
    out.car_render_ok = ReadMatrix(mem, info.car + 64u * (buffer + 4u), &out.car_render) && UsableCarMatrix(out.car_render);
  }
  uint32_t blocks = 0, block = 0;
  if (!Plausible(info.blocks)) {
    return out;
  }
  blocks = info.blocks;
  if (!mem.Word(blocks + 4u * (buffer + 2u), &block) || !Plausible(block)) {
    return out;
  }
  out.block = block;
  const uint8_t* root = mem.Window(block + kAmbBlockRoot, 12);
  if (!root || bones_needed == 0) {
    return out;
  }
  float r[3];
  LoadFloats(root, 3, r);
  for (int c = 0; c < 3; ++c) {
    if (!std::isfinite(r[c]) || std::fabs(r[c]) > 1.0e6f) {
      return out;
    }
    out.root[c] = double(r[c]);
  }
  const uint32_t n = std::min(bones_needed, kAmbMaxBones);
  const uint8_t* b = mem.Window(block + kAmbBlockBones, kAmbBoneBytes * n);
  if (!b) {
    return out;
  }
  out.bones.resize(n);
  for (uint32_t i = 0; i < n; ++i) {
    for (int row = 0; row < 3; ++row) {
      for (int col = 0; col < 4; ++col) {
        const float v = BeFloat(b + kAmbBoneBytes * i + 16u * uint32_t(row) + 4u * uint32_t(col));
        out.bones[i].m[row][col] = v;
      }
    }
  }
  out.block_ok = true;
  return out;
}

// The hypotheses for the model-to-world of a traffic geometry whose model's bone is b.
enum class TrafficPose : uint8_t {
  kRootBone,    // p_world = root + M_b p + t_b (the vertex shader's own recipe as the shadow pose's notes read it)
  kBoneOnly,    // p_world = M_b p + t_b (the palette already holds the root)
  kCarMatrix,   // the car's own render-copy matrix for every bone (the bind pose at the car's place; the wheels do not turn)
};
inline constexpr uint32_t kTrafficPoseCount = 3;

inline const char* TrafficPoseName(TrafficPose pose) {
  switch (pose) {
    case TrafficPose::kRootBone: return "root + bone matrix";
    case TrafficPose::kBoneOnly: return "bone matrix alone";
    case TrafficPose::kCarMatrix: return "the car's own matrix";
  }
  return "?";
}

inline constexpr TrafficPose kDefaultTrafficPose = TrafficPose::kRootBone;

// The model-to-world of a geometry whose model's bone is `bone` under a hypothesis (row vector form: v' = v M, row 3 the translation); false where the data it needs is
// missing, the bone is not in the block's table or the matrix is not finite.
inline bool PlaceTraffic(const TrafficPoseData& pose, uint32_t bone, TrafficPose hypothesis, temporal::Mat4* out) {
  if (hypothesis == TrafficPose::kCarMatrix) {
    if (!pose.car_render_ok) {
      return false;
    }
    *out = pose.car_render;
    return true;
  }
  if (!pose.Usable() || bone >= pose.bones.size()) {
    return false;
  }
  const AmbientBone& b = pose.bones[bone];
  temporal::Mat4 m = temporal::Identity();
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      m.m[c][r] = double(b.m[r][c]);  // the bone's rows are output components: the row vector matrix is the transpose
    }
    m.m[3][r] = double(b.m[r][3]) + (hypothesis == TrafficPose::kRootBone ? pose.root[r] : 0.0);
  }
  if (!temporal::Finite(m)) {
    return false;
  }
  *out = m;
  return true;
}

}  // namespace mcla::gpu::car_model_walk
