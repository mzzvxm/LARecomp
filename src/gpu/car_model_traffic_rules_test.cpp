// Check of the traffic and the parked cars as walk sources (car_model_traffic_rules.h, car_model_parts_rt_rules.h; RT-MODEL-PARTS): the items of the renderer's ambient client (its
// vtable, the list's TWO levels (the client's model nodes, each with its items), the links, a cycle, the caps, a flat list, a foreign node), an item's car and model and the gates the draw's own tests make, the walk of the model's drawable at a lod (the order of the fallbacks),
// the raw palette colour, the pose block's root and bones by the render buffer, the hypotheses for a bone's placement (the transposed rows, the root, the bone alone, the car's own
// matrix) and the calibration against the draws, the traffic's count, and the racers' and the traffic's cars chosen apart and merged. The memory is made up. A program of its own: it
// needs no renderer, no ReXGlue and no game files, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_car_model_traffic_rules_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_car_model_traffic_rules_test

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "car_model_parts_rt_rules.h"
#include "car_model_parts_test_support.h"
#include "car_model_traffic_rules.h"
#include "car_model_walk_rules.h"

namespace cw = mcla::gpu::car_model_walk;
namespace crt = mcla::gpu::car_model_rt;
namespace cmp = mcla::gpu::car_model_parts;
namespace ci = mcla::gpu::car_rt_instances;
namespace tp = mcla::gpu::temporal;
using namespace parts_test;

namespace {

constexpr uint32_t kRendererAddress = 0x8287E064u;
constexpr uint32_t kBufferAddress = 0x827E5640u;

struct Item {
  uint32_t item = 0, car = 0, model = 0, blocks = 0, block = 0;
  uint32_t holders[3] = {};
  uint32_t drawables[3] = {};
};

// One traffic car: a car object, a model with three lods (each a drawable of a body model on bone 0 and four wheel models on bones 1..4: five geometries, the wheels smaller), a
// palette, and the pose block (root, five bones) of the render buffer.
Item MakeItem(FakeMemory& m, double x, double y, double z, int32_t lod = -1, uint32_t buffer = 1) {
  Item it;
  it.car = m.Alloc(520);
  PutIdentity(m, it.car + cw::kAmbCarMatrix, x, y, z);
  m.P32(it.car + cw::kAmbCarType, 0x50000000u + it.car);
  m.PF(it.car + cw::kAmbCarRadius, 2.5f);
  m.P8(it.car + cw::kAmbCarFlags, 0x40);
  PutIdentity(m, it.car + 64u * (buffer + 4u), x + 0.25, y, z);  // the render copy of the matrix, a quarter of a metre on

  it.model = m.Alloc(320);
  for (uint32_t l = 0; l < 3; ++l) {
    GeometrySpec body;
    body.vertices = 8 + l;
    body.indices = {0, 1, 2, 2, 1, 3, 4, 5, 6, 6, 5, 7};
    std::vector<std::vector<uint32_t>> models = {{MakeGeometry(m, body)}};
    std::vector<uint32_t> bones = {0};
    for (uint32_t wheel = 0; wheel < 4; ++wheel) {
      GeometrySpec spec;
      spec.vertices = 5;
      spec.x0 = 10.0f * float(wheel + 1);
      spec.indices = {0, 1, 2, 2, 3, 4};
      models.push_back({MakeGeometry(m, spec)});
      bones.push_back(1 + wheel);
    }
    it.drawables[l] = MakeDrawable(m, models, bones);
    it.holders[l] = MakeEntry(m, it.drawables[l]);
    m.P32(it.model + cw::kAmbModelLods + 4 * l, it.holders[l]);
  }
  const uint32_t palette = m.Alloc(16 * 8);
  for (uint32_t i = 0; i < 8; ++i) {
    m.PF(palette + 16 * i + 0, 0.1f * float(i));
    m.PF(palette + 16 * i + 4, 0.2f);
    m.PF(palette + 16 * i + 8, 0.3f);
    m.PF(palette + 16 * i + 12, 1.0f);
  }
  m.P32(it.model + cw::kAmbModelPalette, palette);
  m.PF(it.model + cw::kAmbModelScale, 0.5f);

  it.blocks = m.Alloc(64);
  it.block = m.Alloc(32 + 48 * 8);
  m.P32(it.blocks + 4 * (buffer + 2), it.block);
  m.PF(it.block + cw::kAmbBlockRoot + 0, float(x));
  m.PF(it.block + cw::kAmbBlockRoot + 4, float(y));
  m.PF(it.block + cw::kAmbBlockRoot + 8, float(z));
  for (uint32_t b = 0; b < 8; ++b) {
    // bone b: identity rotation, translation (0.1 b, 0.2 b, 0.3 b) from the root (a row an output component: the fourth float is the translation)
    for (uint32_t r = 0; r < 3; ++r) {
      for (uint32_t c = 0; c < 4; ++c) {
        float v = r == c ? 1.0f : 0.0f;
        if (c == 3) {
          v = 0.1f * float(r + 1) * float(b);
        }
        m.PF(it.block + cw::kAmbBlockBones + 48 * b + 16 * r + 4 * c, v);
      }
    }
  }

  it.item = m.Alloc(96);
  m.P32(it.item, cw::kAmbItemVtable);
  m.P32(it.item + cw::kAmbItemCar, it.car);
  m.P32(it.item + cw::kAmbItemModel, it.model);
  m.P32(it.item + cw::kAmbItemBlocks, it.blocks);
  m.P32(it.item + cw::kAmbItemLod, uint32_t(lod));
  m.PF(it.item + cw::kAmbItemDepth, 12.5f);
  m.P8(it.item + cw::kAmbItemEnabled, 1);
  m.P16(it.item + cw::kAmbItemColour, 3);
  return it;
}

// The client: the renderer, the client's vtable and the render buffer, and its list of MODEL NODES, one for each group of items (the items of a group hang under the model of the
// group's first item; an empty group is a model with no car: the game keeps a node for every type it has loaded). `MakeClient(items)` is one model for each item.
uint32_t MakeClientGroups(FakeMemory& m, const std::vector<std::vector<Item>>& groups, uint32_t buffer = 1, std::vector<uint32_t>* models_out = nullptr) {
  const uint32_t renderer = m.Alloc(400);
  const uint32_t client = m.Alloc(64);
  m.P32(kRendererAddress, renderer);
  m.P32(renderer + cw::kAmbRendererClients + 4 * cw::kAmbClientSlot, client);
  m.P32(client, cw::kAmbClientVtable);
  m.P32(kBufferAddress, buffer);
  std::vector<uint32_t> models;
  for (const std::vector<Item>& group : groups) {
    models.push_back(!group.empty() && group.front().model != 0 ? group.front().model : m.Alloc(320));
  }
  uint32_t head = 0;
  for (size_t g = groups.size(); g-- > 0;) {
    const uint32_t model = models[g];
    m.P32(model, cw::kAmbModelVtable);
    m.P32(model + cw::kAmbModelNext, head);
    m.P32(model + 16, client);  // the parent: the client
    uint32_t first = 0;
    for (size_t i = groups[g].size(); i-- > 0;) {
      m.P32(groups[g][i].item + cw::kAmbItemSibling, first);
      m.P32(groups[g][i].item + cw::kAmbItemModel, model);
      first = groups[g][i].item;
    }
    m.P32(model + cw::kAmbModelItems, first);
    head = model;
  }
  m.P32(client + cw::kAmbClientModels, head);
  if (models_out) {
    *models_out = models;
  }
  return client;
}

uint32_t MakeClient(FakeMemory& m, const std::vector<Item>& items, uint32_t buffer = 1) {
  std::vector<std::vector<Item>> groups;
  for (const Item& it : items) {
    groups.push_back({it});
  }
  return MakeClientGroups(m, groups, buffer);
}

void ItemTests() {
  {
    FakeMemory m;
    std::vector<Item> items = {MakeItem(m, 0, 0, 0), MakeItem(m, 5, 0, 0), MakeItem(m, 10, 0, 0)};
    const uint32_t client = MakeClient(m, items);
    cw::AmbientScan scan;
    const std::vector<uint32_t> list = cw::ListAmbientItems(m, &scan);
    Check(scan.renderer && scan.client && scan.client_object == client && scan.items == 3 && scan.models == 3 && !scan.truncated, "the client is found by its vtable and its three items are listed under three models");
    Check(scan.flat_items == 0 && scan.foreign_nodes == 0 && scan.mismatched == 0, "a list of models and items has no flat item, foreign node or mismatched item");
    Check(list.size() == 3 && list[0] == items[0].item && list[1] == items[1].item && list[2] == items[2].item, "in the game's order");
    // a cycle in the model list: the last model's next is the first
    std::vector<uint32_t> models;
    for (const Item& it : items) {
      models.push_back(it.model);
    }
    m.P32(models[2] + cw::kAmbModelNext, models[0]);
    cw::AmbientScan cyc;
    Check(cw::ListAmbientItems(m, &cyc).size() == 3 && cyc.models == 3, "a cycle in the list of models ends at the first repeat");
    m.P32(models[2] + cw::kAmbModelNext, 0);
    // a client of another kind
    m.P32(client, 0x82047B14u);
    cw::AmbientScan other;
    Check(cw::ListAmbientItems(m, &other).empty() && other.renderer && !other.client, "a client whose vtable is another's: nothing listed");
  }
  {
    FakeMemory m;
    cw::AmbientScan scan;
    Check(cw::ListAmbientItems(m, &scan).empty() && !scan.renderer, "no renderer: nothing, and it says so");
  }
  {
    // the items' cap: 130 models of 4 items
    FakeMemory m(1u << 22);
    std::vector<std::vector<Item>> groups;
    for (uint32_t g = 0; g < 130; ++g) {
      std::vector<Item> group;
      for (uint32_t i = 0; i < 4; ++i) {
        Item it;
        it.item = m.Alloc(32);
        group.push_back(it);
      }
      groups.push_back(group);
    }
    MakeClientGroups(m, groups);
    cw::AmbientScan scan;
    const std::vector<uint32_t> list = cw::ListAmbientItems(m, &scan);
    Check(list.size() == cw::kAmbMaxItems && scan.truncated, "a list of more items than the cap is cut and says so");
  }
  {
    // the models' cap
    FakeMemory m(1u << 22);
    std::vector<std::vector<Item>> groups(cw::kAmbMaxModels + 10);
    MakeClientGroups(m, groups);
    cw::AmbientScan scan;
    const std::vector<uint32_t> list = cw::ListAmbientItems(m, &scan);
    Check(list.empty() && scan.models == cw::kAmbMaxModels && scan.truncated, "more model nodes than the cap: cut and said");
  }
  {
    // the game's shape: models shared by several cars, a model with no car, the order of the models then of the cars under each, an item cycle, a mismatched item
    FakeMemory m;
    Item a = MakeItem(m, 0, 0, 0), b = MakeItem(m, 5, 0, 0), c = MakeItem(m, 10, 0, 0), d = MakeItem(m, 15, 0, 0);
    std::vector<uint32_t> models;
    const uint32_t client = MakeClientGroups(m, {{a, b}, {}, {c}, {d}}, 1, &models);
    cw::AmbientScan scan;
    std::vector<uint32_t> list = cw::ListAmbientItems(m, &scan);
    Check(scan.client_object == client && scan.models == 4 && scan.items == 4 && scan.mismatched == 0, "four model nodes (one with no car), four items");
    Check(list.size() == 4 && list[0] == a.item && list[1] == b.item && list[2] == c.item && list[3] == d.item, "the cars of one model, then the next model's");
    // the items of one model: a cycle in the siblings ends at the first repeat
    m.P32(b.item + cw::kAmbItemSibling, a.item);
    cw::AmbientScan cyc;
    Check(cw::ListAmbientItems(m, &cyc).size() == 4, "a cycle among one model's items ends at the first repeat");
    m.P32(b.item + cw::kAmbItemSibling, 0);
    // an item whose +20 is another model: listed, counted
    m.P32(d.item + cw::kAmbItemModel, models[0]);
    cw::AmbientScan mism;
    Check(cw::ListAmbientItems(m, &mism).size() == 4 && mism.mismatched == 1, "an item whose model word is another node is listed and counted");
    m.P32(d.item + cw::kAmbItemModel, models[3]);
    // the same car under two models is listed once
    m.P32(models[2] + cw::kAmbModelItems, a.item);
    cw::AmbientScan twice;
    Check(cw::ListAmbientItems(m, &twice).size() == 3, "an item reached twice is listed once");
  }
  {
    // the first version's reading, now refused: a node of the client's list that is no model (its first child does not point back, its vtable is another's) lists nothing
    FakeMemory m;
    const Item a = MakeItem(m, 0, 0, 0);
    const uint32_t client = MakeClientGroups(m, {{a}});
    const uint32_t foreign = m.Alloc(64);
    m.P32(foreign, 0x82047EE0u);                  // some other node class
    m.P32(foreign + cw::kAmbModelItems, a.car);   // a pointer that is no item's back pointer
    m.P32(client + cw::kAmbClientModels, foreign);
    cw::AmbientScan scan;
    Check(cw::ListAmbientItems(m, &scan).empty() && scan.foreign_nodes == 1 && scan.models == 1, "a node that is no model lists nothing and is counted");
  }
  {
    // a flat list (nodes that are items by their vtable): taken as items
    FakeMemory m;
    const Item a = MakeItem(m, 0, 0, 0), b = MakeItem(m, 5, 0, 0);
    const uint32_t client = MakeClientGroups(m, {{a}});
    m.P32(client + cw::kAmbClientModels, a.item);
    m.P32(a.item + cw::kAmbModelNext, b.item);
    m.P32(b.item + cw::kAmbModelNext, 0);
    cw::AmbientScan scan;
    const std::vector<uint32_t> list = cw::ListAmbientItems(m, &scan);
    Check(list.size() == 2 && scan.flat_items == 2 && list[0] == a.item && list[1] == b.item, "nodes that are items by their vtable are listed as items");
  }
  // reading an item
  {
    FakeMemory m;
    const Item it = MakeItem(m, 12, 3, -40, 1);
    MakeClient(m, {it});
    const cw::AmbientItem info = cw::ReadAmbientItem(m, it.item);
    Check(info.ok && info.Eligible() && info.car == it.car && info.model == it.model && info.blocks == it.blocks, "the item's car, model and blocks");
    Check(info.lod == 1 && info.Visible() && info.enabled && info.car_ok && info.colour_index == 3 && Near(info.depth, 12.5) && Near(info.radius, 2.5), "lod, enabled, flags, colour index, depth, radius");
    Check(Near(info.position[0], 12) && Near(info.position[1], 3) && Near(info.position[2], -40), "the position is the car's matrix row 3");
    Check(info.type == 0x50000000u + it.car, "the car's type");
    m.P32(it.item + cw::kAmbItemLod, 0xFFFFFFFFu);
    Check(!cw::ReadAmbientItem(m, it.item).Visible(), "a negative lod: the main view does not show it");
    m.P8(it.item + cw::kAmbItemEnabled, 0);
    Check(cw::ReadAmbientItem(m, it.item).ok && !cw::ReadAmbientItem(m, it.item).Eligible(), "disabled: read, not eligible");
    m.P8(it.item + cw::kAmbItemEnabled, 1);
    for (const uint32_t flags : {0x60u, 0xC0u, 0x00u, 0x20u, 0x80u}) {
      m.P8(it.car + cw::kAmbCarFlags, flags);
      Check(!cw::ReadAmbientItem(m, it.item).Eligible(), "the car's flags must have 0x40 and neither 0x20 nor 0x80: " + std::to_string(flags));
    }
    m.P8(it.car + cw::kAmbCarFlags, 0x40);
    Check(cw::ReadAmbientItem(m, it.item).Eligible(), "0x40 alone is a car the draw takes");
    m.P8(it.car + cw::kAmbCarFlags, 0x5F);
    Check(cw::ReadAmbientItem(m, it.item).Eligible(), "other bits do not matter");
    m.P8(it.car + cw::kAmbCarFlags, 0x40);
    // a matrix that is not a pose
    m.PF(it.car + cw::kAmbCarMatrix + 0, std::nanf(""));
    const cw::AmbientItem bad = cw::ReadAmbientItem(m, it.item);
    Check(!bad.ok && !bad.why.empty() && !bad.Eligible(), "a car whose matrix is NaN is not a pose");
    PutIdentity(m, it.car + cw::kAmbCarMatrix, 1e7, 0, 0);
    Check(!cw::ReadAmbientItem(m, it.item).ok, "a position a million metres out is not a pose");
    PutIdentity(m, it.car + cw::kAmbCarMatrix, 0, 0, 0);
    m.PF(it.car + cw::kAmbCarMatrix + 0, 0.0f);
    Check(!cw::ReadAmbientItem(m, it.item).ok, "an axis with no length is not a pose");
    // no car, no model
    m.P32(it.item + cw::kAmbItemCar, 0);
    Check(!cw::ReadAmbientItem(m, it.item).ok, "no car");
    m.P32(it.item + cw::kAmbItemCar, it.car);
    m.P32(it.item + cw::kAmbItemModel, 0);
    Check(!cw::ReadAmbientItem(m, it.item).ok, "no model");
    Check(!cw::ReadAmbientItem(m, 0).ok, "no item");
  }
  // the cheap first look: the car's position alone
  {
    FakeMemory m;
    const Item it = MakeItem(m, 12, 3, -40, 1);
    double at[3];
    Check(cw::ReadAmbientPosition(m, it.item, at) && Near(at[0], 12) && Near(at[1], 3) && Near(at[2], -40), "the position of the car by the item, without reading the rest");
    m.PF(it.car + cw::kAmbCarMatrix + 48, std::nanf(""));
    Check(!cw::ReadAmbientPosition(m, it.item, at), "a NaN position is not read");
    m.PF(it.car + cw::kAmbCarMatrix + 48, 2.0e7f);
    Check(!cw::ReadAmbientPosition(m, it.item, at), "a position beyond a million metres is not read");
    m.P32(it.item + cw::kAmbItemCar, 0);
    Check(!cw::ReadAmbientPosition(m, it.item, at) && !cw::ReadAmbientPosition(m, 0, at), "no car, no item: not read");
  }
}

void WalkTests() {
  FakeMemory m;
  const Item it = MakeItem(m, 20, 1, 5, -1);
  MakeClient(m, {it});
  cw::AmbientItem info = cw::ReadAmbientItem(m, it.item);
  cw::Options options;
  options.production = true;
  options.min_lod = 1;
  cw::CarWalk walk;
  cw::WalkAmbient(m, info, options, &walk);
  Check(walk.ok && walk.vehicle == it.item && walk.entity == it.car && walk.model_set == it.model, "the walk keys on the item and knows the car and the model");
  Check(walk.position_read && Near(walk.position[0], 20) && walk.visible == 0 && walk.has_entity_matrix, "position, not visible, the car's matrix");
  Check(walk.body_lod == 1 && walk.parts.size() == 1 && walk.parts[0].part == cw::kModelPart && walk.parts[0].lod == 1 && walk.parts[0].in_use && walk.parts[0].placed,
        "the reflection's lod 1 is taken: one entry, the model part");
  Check(walk.geometries.size() == 5 && walk.in_use.geometries == 5 && walk.all.geometries == 5, "five geometries: the body and four wheels, all in use");
  std::vector<uint32_t> bones;
  bool all_model = true;
  for (const cw::Geometry& g : walk.geometries) {
    bones.push_back(g.bone);
    all_model = all_model && g.part == cw::kModelPart && g.lod == 1 && g.in_use && g.read && g.vb_ok && g.ib_ok;
  }
  Check(bones == std::vector<uint32_t>({0, 1, 2, 3, 4}) && all_model, "bones 0..4, every geometry of the model part at lod 1");
  Check(cw::BonesNeeded(walk) == 5, "the pose block needs five bones");
  Check(walk.in_use.triangles == 4 + 4 * 2 && walk.lod_tally[1].geometries == 5 && walk.lod_tally[0].geometries == 0, "tallies by lod: five geometries at lod 1");
  // picks: the whole model, rigid, with bones
  Check(crt::PickGeometries(walk, 0, 0).size() == 5, "PickGeometries takes all five");
  Check(crt::PickGeometries(walk, 7, 0).size() == 1, "a smallest part of 7 indices leaves the wheels (6) out and keeps the body (12)");

  // the lod fallbacks: wanted 1: 1, then the finer 0, then the coarser 2
  const auto taken = [&](int32_t item_lod, int32_t min_lod, std::vector<uint32_t> missing) {
    FakeMemory mm;
    const Item i2 = MakeItem(mm, 0, 0, 0, item_lod);
    for (const uint32_t l : missing) {
      mm.P32(i2.model + cw::kAmbModelLods + 4 * l, 0);
    }
    MakeClient(mm, {i2});
    cw::Options o;
    o.production = true;
    o.min_lod = min_lod;
    cw::CarWalk w;
    cw::WalkAmbient(mm, cw::ReadAmbientItem(mm, i2.item), o, &w);
    return w.ok ? w.body_lod : -1;
  };
  Check(taken(-1, 1, {}) == 1, "not visible, the floor 1: lod 1");
  Check(taken(0, 1, {}) == 1, "the main view's lod 0 raised to the floor 1");
  Check(taken(2, 0, {}) == 2, "the main view's own lod 2 is taken where the floor allows it");
  Check(taken(-1, 0, {}) == 0, "not visible, the floor 0: the finest");
  Check(taken(-1, 1, {1}) == 0, "lod 1 missing: the finer 0 before the coarser 2");
  Check(taken(-1, 1, {1, 0}) == 2, "lods 1 and 0 missing: 2");
  Check(taken(-1, 1, {0, 1, 2}) == -1, "no lod at all: the walk is not ok");
  Check(taken(-1, 2, {2}) == 1, "wanted 2 and missing: the finer 1");
  {
    // an unplaced holder is walked all the same (the draw does not test it); the flag is told
    FakeMemory mm;
    const Item i2 = MakeItem(mm, 0, 0, 0, -1);
    mm.P32(i2.holders[1] + 8, 0);
    MakeClient(mm, {i2});
    cw::Options o;
    o.production = true;
    cw::CarWalk w;
    cw::WalkAmbient(mm, cw::ReadAmbientItem(mm, i2.item), o, &w);
    Check(w.ok && w.body_lod == 1 && !w.parts[0].placed && w.geometries.size() == 5, "a holder without the placed bits is walked, and says it is not placed");
  }
  {
    // a model whose lod holds nothing readable
    FakeMemory mm;
    const Item i2 = MakeItem(mm, 0, 0, 0, -1);
    mm.P32(i2.drawables[1] + 8, 0);
    MakeClient(mm, {i2});
    cw::Options o;
    o.production = true;
    cw::CarWalk w;
    cw::WalkAmbient(mm, cw::ReadAmbientItem(mm, i2.item), o, &w);
    Check(w.ok && w.body_lod == 0, "a lod whose LOD object is null falls to the next");
  }
  {
    // an item that is not ok
    cw::AmbientItem bad;
    bad.item = 0x1234;
    bad.why = "item+16 holds no car";
    cw::CarWalk w;
    cw::WalkAmbient(m, bad, options, &w);
    Check(!w.ok && w.why == "item+16 holds no car" && w.vehicle == 0x1234, "a bad item: not ok, with the reason");
  }

  // the raw palette colour
  const cw::AmbientColour colour = cw::ReadAmbientColour(m, info);
  Check(colour.read && Near(colour.rgba[0], 0.3, 1e-5) && Near(colour.rgba[1], 0.2, 1e-5) && Near(colour.rgba[3], 1.0) && Near(colour.scale, 0.5), "palette entry 3: (0.3, 0.2, 0.3, 1) x 0.5");
  info.colour_index = 9999;
  Check(!cw::ReadAmbientColour(m, info).read, "a colour index past the palette is not read");
  info.colour_index = 3;
  m.P32(it.model + cw::kAmbModelPalette, 0);
  Check(!cw::ReadAmbientColour(m, info).read, "no palette: not read");
}

void PoseTests() {
  FakeMemory m;
  const Item it = MakeItem(m, 100, 2, -50, -1, 1);
  MakeClient(m, {it}, 1);
  const cw::AmbientItem info = cw::ReadAmbientItem(m, it.item);
  const cw::TrafficPoseData pose = cw::ReadTrafficPose(m, info, 5);
  Check(pose.block_ok && pose.buffer == 1 && pose.block == it.block && pose.Usable() && pose.bones.size() == 5, "the block of buffer 1: (buffer + 2) * 4 in the item's table; five bones");
  Check(Near(pose.root[0], 100) && Near(pose.root[1], 2) && Near(pose.root[2], -50), "the root");
  Check(Near(pose.bones[3].m[0][3], 0.3, 1e-5) && Near(pose.bones[3].m[1][3], 0.6, 1e-5) && Near(pose.bones[3].m[2][3], 0.9, 1e-5) && Near(pose.bones[3].m[1][1], 1.0), "bone 3's 3x4 matrix (48 bytes each)");
  Check(pose.car_render_ok && Near(pose.car_render.m[3][0], 100.25), "the car's render-copy matrix at +(buffer + 4) * 64");
  // another buffer: another block
  {
    FakeMemory mm;
    const Item i2 = MakeItem(mm, 0, 0, 0, -1, 2);
    MakeClient(mm, {i2}, 2);
    const cw::TrafficPoseData p2 = cw::ReadTrafficPose(mm, cw::ReadAmbientItem(mm, i2.item), 2);
    Check(p2.block_ok && p2.buffer == 2 && p2.block == i2.block && p2.bones.size() == 2, "buffer 2 reads the block at (2 + 2) * 4");
    mm.P32(kBufferAddress, 1);  // the table has no block for buffer 1 in this world
    Check(!cw::ReadTrafficPose(mm, cw::ReadAmbientItem(mm, i2.item), 2).block_ok, "a buffer whose block is null: not usable");
    mm.P32(kBufferAddress, 7);
    Check(!cw::ReadTrafficPose(mm, cw::ReadAmbientItem(mm, i2.item), 2).block_ok, "a buffer index above 3 is no buffer");
  }
  Check(!cw::ReadTrafficPose(m, info, 0).block_ok, "no bones asked for: not usable");
  Check(cw::ReadTrafficPose(m, info, 200).bones.size() == cw::kAmbMaxBones, "the bones are capped at the palette's 64");
  {
    cw::AmbientItem no_blocks = info;
    no_blocks.blocks = 0;
    Check(!cw::ReadTrafficPose(m, no_blocks, 5).block_ok, "no blocks table: not usable");
    m.PF(it.block + cw::kAmbBlockRoot, std::nanf(""));
    Check(!cw::ReadTrafficPose(m, info, 5).block_ok, "a NaN root: not usable");
    m.PF(it.block + cw::kAmbBlockRoot, 100.0f);
  }

  // PlaceTraffic: a turned bone. Rows are output components: x' = z, y' = y, z' = -x (a quarter turn about y), translation (1, 2, 3), root (100, 2, -50).
  cw::TrafficPoseData turned = pose;
  const float rows[3][4] = {{0, 0, 1, 1}, {0, 1, 0, 2}, {-1, 0, 0, 3}};
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 4; ++c) {
      turned.bones[2].m[r][c] = rows[r][c];
    }
  }
  const auto transform = [](const tp::Mat4& w, const double p[3], double out[3]) {
    for (int c = 0; c < 3; ++c) {
      out[c] = p[0] * w.m[0][c] + p[1] * w.m[1][c] + p[2] * w.m[2][c] + w.m[3][c];
    }
  };
  tp::Mat4 placed;
  Check(cw::PlaceTraffic(turned, 2, cw::TrafficPose::kRootBone, &placed), "root + bone: placed");
  const double p[3] = {1, 0, 0};
  double q[3];
  transform(placed, p, q);
  Check(Near(q[0], 100 + 0 + 1) && Near(q[1], 2 + 0 + 2) && Near(q[2], -50 + (-1) + 3), "a point (1,0,0): root + M p + t = (101, 4, -48): the row-vector matrix is the transpose with the root added");
  const double p2[3] = {0, 0, 1};
  transform(placed, p2, q);
  Check(Near(q[0], 102) && Near(q[1], 4) && Near(q[2], -47), "a point (0,0,1): M p = (1, 0, 0): (102, 4, -47)");
  Check(Near(placed.m[0][3], 0) && Near(placed.m[3][3], 1), "the fourth column is (0, 0, 0, 1)");
  tp::Mat4 alone;
  Check(cw::PlaceTraffic(turned, 2, cw::TrafficPose::kBoneOnly, &alone) && Near(alone.m[3][0], 1) && Near(alone.m[3][1], 2) && Near(alone.m[3][2], 3), "bone alone: no root");
  tp::Mat4 car;
  Check(cw::PlaceTraffic(turned, 99, cw::TrafficPose::kCarMatrix, &car) && Near(car.m[3][0], 100.25), "the car's matrix ignores the bone");
  Check(!cw::PlaceTraffic(turned, 5, cw::TrafficPose::kRootBone, &placed), "bone 5 is not in a five-bone table");
  cw::TrafficPoseData nocar = turned;
  nocar.car_render_ok = false;
  Check(!cw::PlaceTraffic(nocar, 0, cw::TrafficPose::kCarMatrix, &car), "no render-copy matrix: the car hypothesis has nothing");
  cw::TrafficPoseData nan_pose = turned;
  nan_pose.bones[0].m[0][0] = std::nanf("");
  Check(!cw::PlaceTraffic(nan_pose, 0, cw::TrafficPose::kRootBone, &placed), "a NaN in the bone: not placed");
  cw::TrafficPoseData unusable;
  Check(!cw::PlaceTraffic(unusable, 0, cw::TrafficPose::kRootBone, &placed), "no pose data: not placed");
  Check(cw::kDefaultTrafficPose == cw::TrafficPose::kRootBone, "the shadow pose's recipe is the default");
}

// the calibration
void CalibrationTests() {
  FakeMemory m;
  const Item it = MakeItem(m, 100, 2, -50, -1, 1);
  MakeClient(m, {it}, 1);
  const cw::TrafficPoseData pose = cw::ReadTrafficPose(m, cw::ReadAmbientItem(m, it.item), 5);
  const auto feed = [&](cmp::TrafficCalibration* cal, cw::TrafficPose truth, uint32_t pairs, double error = 0.0) {
    for (uint32_t i = 0; i < pairs; ++i) {
      tp::Mat4 drawn;
      Check(cw::PlaceTraffic(pose, i % 5, truth, &drawn), "the draw's matrix is made");
      drawn.m[3][1] += error;
      cal->Add(pose, i % 5, drawn);
    }
  };
  {
    cmp::TrafficCalibration cal;
    Check(!cal.decided() && !cal.fits() && cal.chosen() == cw::TrafficPose::kRootBone, "nothing decided: the default");
    feed(&cal, cw::TrafficPose::kRootBone, 20);
    Check(cal.samples() == 20 && cal.Decide() && cal.fits() && cal.chosen() == cw::TrafficPose::kRootBone, "draws of root + bone: it fits and is chosen");
    Check(cal.Describe().find("root + bone matrix") != std::string::npos, "the description names the hypotheses");
  }
  {
    cmp::TrafficCalibration cal;
    feed(&cal, cw::TrafficPose::kBoneOnly, 20);
    Check(cal.Decide() && cal.chosen() == cw::TrafficPose::kBoneOnly && cal.fits(), "draws with the root already in the palette: bone alone");
  }
  {
    cmp::TrafficCalibration cal;
    feed(&cal, cw::TrafficPose::kCarMatrix, 20);
    Check(cal.Decide() && cal.chosen() == cw::TrafficPose::kCarMatrix && cal.fits(), "draws at the car's own matrix: that one");
  }
  {
    cmp::TrafficCalibration cal;
    feed(&cal, cw::TrafficPose::kRootBone, 11);
    Check(!cal.Decide() && !cal.decided(), "eleven pairs are too few (the minimum is twelve)");
  }
  {
    cmp::TrafficCalibration cal;
    feed(&cal, cw::TrafficPose::kRootBone, 20, 2.0);
    Check(cal.Decide() && cal.decided() && !cal.fits(), "two metres off everywhere: decided, no fit: no traffic car is placed");
    cal.NewWindow();
    Check(cal.samples() == 0 && cal.decided(), "a new window keeps the choice");
  }
}

// the self-check of a pose block against the car's own matrix (no draw)
void SelfCheckTests() {
  FakeMemory m;
  const Item it = MakeItem(m, 100, 2, -50, -1, 1);
  MakeClient(m, {it}, 1);
  const cw::AmbientItem info = cw::ReadAmbientItem(m, it.item);
  const cw::TrafficPoseData pose = cw::ReadTrafficPose(m, info, 5);
  {
    const cmp::TrafficSelfCheck c = cmp::CheckTrafficBlock(pose, cw::TrafficPose::kRootBone, pose.car_render);
    Check(c.placed && c.ok && Near(c.metres, 0.25, 1e-4) && c.basis < 1e-5, "the body bone at the root: the car's own render-copy matrix (a quarter of a metre on in this world) is where the block puts the body");
    const cmp::TrafficSelfCheck u = cmp::CheckTrafficBlock(pose, cw::TrafficPose::kRootBone, info.matrix);
    Check(u.ok && Near(u.metres, 0.0, 1e-4), "... and the update copy the same here");
  }
  {
    tp::Mat4 moved = pose.car_render;
    moved.m[3][0] += 1.0;
    Check(cmp::CheckTrafficBlock(pose, cw::TrafficPose::kRootBone, moved).ok, "1.25 m between the block and the car is within the check's 1.5 m (a car that moved between the two copies)");
    moved.m[3][0] += 1.0;
    const cmp::TrafficSelfCheck far = cmp::CheckTrafficBlock(pose, cw::TrafficPose::kRootBone, moved);
    Check(far.placed && !far.ok && far.metres > cmp::kSelfCheckMetres, "2.25 m between them: not the car's block (a stale block)");
  }
  {
    tp::Mat4 turned = tp::Identity();
    turned.m[0][0] = 0.0;
    turned.m[0][2] = 1.0;
    turned.m[2][0] = -1.0;
    turned.m[2][2] = 0.0;
    for (int c = 0; c < 3; ++c) {
      turned.m[3][c] = pose.car_render.m[3][c];
    }
    const cmp::TrafficSelfCheck t = cmp::CheckTrafficBlock(pose, cw::TrafficPose::kRootBone, turned);
    Check(t.placed && !t.ok && t.basis > cmp::kSelfCheckBasis, "a car a quarter turn from the body bone: not accepted (the rows are read the wrong way, or the block is another car's)");
  }
  {
    // the block without the root: the root-less hypothesis puts the body at the model's origin, far from the car
    const cmp::TrafficSelfCheck alone = cmp::CheckTrafficBlock(pose, cw::TrafficPose::kBoneOnly, pose.car_render);
    Check(alone.placed && !alone.ok && alone.metres > 50.0, "the root-less hypothesis puts the body far from the car: not accepted");
    tp::Mat4 nan = pose.car_render;
    nan.m[3][1] = std::nan("");
    Check(!cmp::CheckTrafficBlock(pose, cw::TrafficPose::kRootBone, nan).ok, "a NaN car matrix: not accepted");
    const cw::TrafficPoseData none;
    Check(!cmp::CheckTrafficBlock(none, cw::TrafficPose::kRootBone, pose.car_render).placed, "no pose block: nothing to check");
  }
}

// reading an item: why it failed
void FailTests() {
  FakeMemory m;
  const Item it = MakeItem(m, 1, 2, 3, 0);
  MakeClient(m, {it});
  Check(cw::ReadAmbientItem(m, it.item).fail == cw::AmbientFail::kNone && cw::ReadAmbientItem(m, it.item).ok, "an item that reads has no failure");
  Check(cw::ReadAmbientItem(m, 0).fail == cw::AmbientFail::kItem, "no item");
  m.P32(it.item + cw::kAmbItemCar, 0);
  Check(cw::ReadAmbientItem(m, it.item).fail == cw::AmbientFail::kCar, "no car pointer");
  m.P32(it.item + cw::kAmbItemCar, it.car);
  m.P32(it.item + cw::kAmbItemModel, 0);
  Check(cw::ReadAmbientItem(m, it.item).fail == cw::AmbientFail::kModel, "no model pointer");
  m.P32(it.item + cw::kAmbItemModel, it.model);
  m.PF(it.car + cw::kAmbCarMatrix + 0, std::nanf(""));
  Check(cw::ReadAmbientItem(m, it.item).fail == cw::AmbientFail::kMatrixRead, "a NaN in the matrix: not readable as a matrix");
  m.PF(it.car + cw::kAmbCarMatrix + 0, 0.0f);
  Check(cw::ReadAmbientItem(m, it.item).fail == cw::AmbientFail::kMatrixPose, "an axis of no length: not a pose");
  // the first version's mistake: the client read for a car (its +128 is no matrix)
  const uint32_t client = m.Alloc(600);
  m.P32(it.item + cw::kAmbItemCar, client);
  Check(!cw::ReadAmbientItem(m, it.item).ok, "a car pointer that is the client: refused (nothing there is a pose)");
  Check(std::string(cw::AmbientFailName(cw::AmbientFail::kMatrixPose)).find("pose") != std::string::npos && cw::kAmbientFailCount == 6, "the failures have names");
}

// the cars chosen
crt::CarInfo Car(uint32_t id, bool traffic, double distance, uint32_t parts = 10) {
  crt::CarInfo c;
  c.vehicle = id;
  c.traffic = traffic;
  c.distance = distance;
  c.parts = parts;
  c.position[0] = distance;
  return c;
}

void SelectionTests() {
  const ci::Limits high = ci::ClassLimits(mcla::gpu::gpu_class::GpuClass::kHigh);
  const ci::Limits mid = ci::ClassLimits(mcla::gpu::gpu_class::GpuClass::kMid);
  Check(cmp::TrafficCarsFor(high, -1) == 4 && cmp::TrafficCarsFor(mid, -1) == 2, "the traffic's count is the class's by default");
  Check(cmp::TrafficCarsFor(high, 0) == 0 && cmp::TrafficCarsFor(high, 3) == 3 && cmp::TrafficCarsFor(high, 99) == ci::kMaxCars, "0 off, N asks, at most the setting's maximum");
  Check(cmp::TrafficCarsFor(ci::Limits{}, -1) == 0 && cmp::TrafficCarsFor(ci::Limits{}, 3) == 0, "no other cars: no traffic");

  std::vector<crt::CarInfo> cars = {
      Car(1, false, 3), Car(2, false, 6), Car(3, false, 9), Car(4, false, 11), Car(5, false, 13),          // racers (indices 0..4)
      Car(11, true, 2), Car(12, true, 5), Car(13, true, 7), Car(14, true, 10), Car(15, true, 12), Car(16, true, 13.5),  // traffic (5..10)
  };
  crt::CarInfo player = Car(99, false, 0);
  player.is_player = true;
  crt::CarInfo drawn = Car(98, true, 1);
  drawn.drawn = true;
  crt::CarInfo shown = Car(97, true, 1.5);
  shown.visible = true;
  shown.parts = 0;  // the walk makes no parts for a car the main view shows
  crt::CarInfo far = Car(96, true, 30);
  crt::CarInfo empty = Car(95, true, 4, 0);
  cars.insert(cars.end(), {player, drawn, shown, far, empty});  // indices 11..15

  const auto vehicles = [&](const std::vector<uint32_t>& picks) {
    std::vector<uint32_t> out;
    for (const uint32_t i : picks) {
      out.push_back(cars[i].vehicle);
    }
    return out;
  };
  using V = std::vector<uint32_t>;
  Check(vehicles(crt::ChooseStaged(cars, high)) == V({1, 2, 3, 4, 5}), "racers staged: the class's four and a spare; no traffic");
  Check(vehicles(crt::ChooseStaged(cars, high, true, 4)) == V({11, 12, 13, 14, 15}), "traffic staged: its four and a spare; not the drawn, the shown, the far or the one with no parts");
  Check(vehicles(crt::ChooseStaged(cars, high, true, 2)) == V({11, 12, 13}), "a traffic count of two: two and a spare");
  Check(vehicles(cmp::ChooseStagedAll(cars, high, 4)) == V({11, 1, 12, 2, 13, 3, 14, 4, 15, 5}), "merged by distance: ten cars staged");
  Check(vehicles(cmp::ChooseStagedAll(cars, high, 0)) == V({1, 2, 3, 4, 5}), "no traffic count: the racers only (as before)");

  const std::vector<ci::OtherCar> none;
  Check(vehicles(crt::ChoosePlaced(cars, high, crt::Mode::kGaps, none)) == V({1, 2, 3, 4}), "racers placed: the class's four, no traffic");
  Check(vehicles(crt::ChoosePlaced(cars, high, crt::Mode::kGaps, none, true, 3)) == V({11, 12, 13}), "traffic placed: its three");
  Check(vehicles(cmp::ChoosePlacedAll(cars, high, crt::Mode::kGaps, none, 3)) == V({11, 1, 12, 2, 13, 3, 4}), "both, nearest first: seven");
  Check(vehicles(cmp::ChoosePlacedAll(cars, high, crt::Mode::kGaps, none, 4)).size() == 8, "four and four: eight, the stencil IDs' room");
  Check(vehicles(cmp::ChoosePlacedAll(cars, high, crt::Mode::kGaps, none, 8)).size() == crt::kMaxWalkedCars, "more than that is cut at the walked cars' IDs");
  Check(vehicles(cmp::ChoosePlacedAll(cars, high, crt::Mode::kGaps, none, 0)) == V({1, 2, 3, 4}), "traffic count 0: the racers only (as before)");
  Check(cmp::ChoosePlacedAll(cars, high, crt::Mode::kOff, none, 4).empty(), "mode off: nothing");
  Check(cmp::ChoosePlacedAll(cars, ci::Limits{}, crt::Mode::kGaps, none, 4).empty(), "the other cars off: nothing");
  // the radius is the class's: the mid class reaches 10 m
  Check(vehicles(cmp::ChoosePlacedAll(cars, mid, crt::Mode::kGaps, none, 2)) == V({11, 1, 12, 2}), "mid class: 2 racers and 2 traffic cars within 10 m");
  // a remembered car (a kit) stands where traffic car 11 is: kGaps leaves it to the kit, kModelFirst does not
  std::vector<ci::OtherCar> kits(1);
  kits[0].remembered = true;
  kits[0].stencil_id = 24;
  kits[0].draws = 1;
  kits[0].sum[0] = 2.0;
  kits[0].sum[1] = 0.0 - ci::kCentreRise;
  kits[0].sum[2] = 0.0;
  Check(vehicles(crt::ChoosePlaced(cars, high, crt::Mode::kGaps, kits, true, 3)) == V({12, 13, 14}), "a kit at the nearest traffic car's place leaves that car to the kit (mode 1)");
  Check(vehicles(crt::ChoosePlaced(cars, high, crt::Mode::kModelFirst, kits, true, 3)) == V({11, 12, 13}), "mode 2: the model data first");

  // the signature includes the wheel ids / the car type
  crt::WalkSignature a, b;
  a.model_set = b.model_set = 0x1000;
  a.walked_frame = b.walked_frame = 10;
  a.wheels = 7;
  b.wheels = 8;
  Check(!(a == b) && !crt::WalkStillGood(a, b, 20), "another wheel / type: the cached walk is not good");
  b.wheels = 7;
  Check(a == b && crt::WalkStillGood(a, b, 20) && !crt::WalkStillGood(a, b, 10 + crt::kRewalkFrames), "the same: good until the rewalk frames");
}

}  // namespace

int main() {
  ItemTests();
  WalkTests();
  PoseTests();
  CalibrationTests();
  SelfCheckTests();
  FailTests();
  SelectionTests();
  std::printf("car_model_traffic_rules_test: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
