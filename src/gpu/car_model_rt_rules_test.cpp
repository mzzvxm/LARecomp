// Check of the rules that build the ray traced scene's cars from the cars' own model data (car_model_rt_rules.h, RT-MODEL-CARS phase 2): the game's lod rule against
// the samples of the first check run, the part rules of the draw's loop, the five placement hypotheses and the calibration that picks one against the draws, the
// conversion of a placement into the scene's view space and the error between a walked instance and a draw's (the proof line's number), the mesh read from the
// car's buffers (16 and 32 bit indices, endian, lists and strips) and its key (cached per buffer address), the paint, the choice of the cars to stage and to place
// by class and by mode, the budgets of a car that is new and the cleanup when a car leaves. The memory is made up. A program of its own: it needs no renderer,
// no ReXGlue and no game files, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_car_model_rt_rules_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_car_model_rt_rules_test

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "car_model_rt_rules.h"

namespace cw = mcla::gpu::car_model_walk;
namespace crt = mcla::gpu::car_model_rt;
namespace ci = mcla::gpu::car_rt_instances;
namespace rr = mcla::gpu::car_rt_rules;
namespace tp = mcla::gpu::temporal;

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

bool Near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }

// A made-up guest: one flat byte image from `kBase`; every read is bounds-checked like the adapter's.
class FakeMemory {
 public:
  static constexpr uint32_t kBase = 0x40000000u;
  explicit FakeMemory(size_t size = 1u << 20) : bytes_(size, 0) {}
  uint32_t Alloc(uint32_t size, uint32_t align = 16) {
    next_ = (next_ + align - 1) / align * align;
    const uint32_t at = kBase + next_;
    next_ += size;
    if (next_ > bytes_.size()) {
      std::printf("FakeMemory: out of room\n");
      std::abort();
    }
    return at;
  }
  uint8_t* At(uint32_t va) { return bytes_.data() + (va - kBase); }
  void P32(uint32_t va, uint32_t v) {
    At(va)[0] = uint8_t(v >> 24);
    At(va)[1] = uint8_t(v >> 16);
    At(va)[2] = uint8_t(v >> 8);
    At(va)[3] = uint8_t(v);
  }
  void P16(uint32_t va, uint32_t v) {
    At(va)[0] = uint8_t(v >> 8);
    At(va)[1] = uint8_t(v);
  }
  void PF(uint32_t va, float f) {
    uint32_t bits;
    std::memcpy(&bits, &f, 4);
    P32(va, bits);
  }
  const uint8_t* Window(uint32_t va, uint32_t n) const {
    if (va < kBase || uint64_t(va - kBase) + n > bytes_.size()) {
      return nullptr;
    }
    return bytes_.data() + (va - kBase);
  }
  bool Word(uint32_t va, uint32_t* out) const {
    const uint8_t* p = Window(va, 4);
    if (!p) {
      return false;
    }
    *out = cw::Be32(p);
    return true;
  }
  bool Half(uint32_t va, uint32_t* out) const {
    const uint8_t* p = Window(va, 2);
    if (!p) {
      return false;
    }
    *out = cw::Be16(p);
    return true;
  }
  bool Byte(uint32_t va, uint32_t* out) const {
    const uint8_t* p = Window(va, 1);
    if (!p) {
      return false;
    }
    *out = *p;
    return true;
  }

 private:
  std::vector<uint8_t> bytes_;
  uint32_t next_ = 0;
};

tp::Mat4 Translation(double x, double y, double z) {
  tp::Mat4 m = tp::Identity();
  m.m[3][0] = x;
  m.m[3][1] = y;
  m.m[3][2] = z;
  return m;
}

// A rotation about y by `a` radians (row vectors) with the translation (x, y, z).
tp::Mat4 YawAt(double a, double x, double y, double z) {
  tp::Mat4 m = tp::Identity();
  m.m[0][0] = std::cos(a);
  m.m[0][2] = -std::sin(a);
  m.m[2][0] = std::sin(a);
  m.m[2][2] = std::cos(a);
  m.m[3][0] = x;
  m.m[3][1] = y;
  m.m[3][2] = z;
  return m;
}

void PutMatrix(FakeMemory& m, uint32_t va, const tp::Mat4& mat) {
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 3; ++c) {
      m.PF(va + 16u * uint32_t(r) + 4u * uint32_t(c), float(mat.m[r][c]));
    }
    m.PF(va + 16u * uint32_t(r) + 12u, std::numeric_limits<float>::quiet_NaN());  // the game leaves NaN in the fourth column
  }
}

// ---------------------------------------------------------------------------
// the game's lod rule, against the first check run's samples
// ---------------------------------------------------------------------------

void TestLodRule() {
  cw::LodRuleInput in;  // the retail thresholds
  in.threshold[0] = 15.0f;
  in.threshold[1] = 100.0f;
  const auto lod = [&](double distance, bool has_lod0, int32_t forced = -1, bool capped = false) {
    in.distance = distance;
    in.info_has_lod0 = has_lod0;
    in.forced = forced;
    in.capped = capped;
    return cw::GameBodyLod(in);
  };
  // The first run (ts-mw30-rt-offscreen, 999 results): every drawn geometry was in the slot the rule picks.
  Check(lod(3.3, false) == 1 && lod(3.7, false) == 1 && lod(4.2, false) == 1, "racers 3 to 4 m away, no lod 0: raised to 1 (the log: lods 1/1, matched in slot 1)");
  Check(lod(9.5, false) == 1, "a racer 9.5 m away, no lod 0: 1");
  Check(lod(55.9, false) == 1, "the car at 56 m (the 73 percent one): lod 1, the slot its draws were in");
  Check(lod(525.3, false) == 2 && lod(519.8, false) == 2, "cars 520 m away: lod 2 (the log: lods 2/2, four geometries of the body)");
  Check(lod(0.0, true) == 0 && lod(14.9, true) == 0, "a car with a lod 0, near: 0 (the player: lods 0/0 and matched in slot 0)");
  Check(lod(15.0, true) == 1 && lod(99.9, true) == 1 && lod(100.0, true) == 2 && lod(900.0, true) == 2, "the thresholds are <=: 15 m is lod 1, 100 m lod 2, never more than 2");
  Check(lod(2.0, false, 2) == 2 && lod(500.0, true, 0) == 0, "+5996 forces the lod whatever the distance says");
  Check(lod(500.0, true, -1, true) == 1 && lod(5.0, true, -1, true) == 0, "the global cap holds a far car at 1 and leaves a near one");
  // the parts' lod
  Check(cw::PartLodOf(2, 1, 0) == 1 && cw::PartLodOf(43, 1, 0) == 1 && cw::PartLodOf(44, 1, 0) == 0 && cw::PartLodOf(53, 2, 1) == 1,
        "parts under 44 are asked for the vehicle's lod, 44 and up for the second pass's (asm sub_8235D500: the part loop loads +6008 at part 44)");
  const std::array<int32_t, 3> from1 = cw::LodOrder(1);
  Check(from1[0] == 1 && from1[1] == 2 && from1[2] == 0, "the lods to try from 1: 1, then the coarser, then the finer");
  const std::array<int32_t, 3> from0 = cw::LodOrder(0), from2 = cw::LodOrder(2);
  Check(from0[0] == 0 && from0[1] == 1 && from0[2] == 2 && from2[0] == 2 && from2[1] == 1 && from2[2] == 0, "from 0 and from 2");
  Check(cw::LodOrder(-4)[0] == 0 && cw::LodOrder(9)[0] == 2, "a lod out of range is clamped");
}

void TestPartRules() {
  cw::DrawFlags f;
  Check(cw::LoopParts(f) == cw::kPartSlots, "no flag: all 55 parts");
  f.loop44_a = true;
  Check(cw::LoopParts(f) == 44, "the first byte cuts the loop to 44 parts");
  f = cw::DrawFlags{};
  f.loop44_b = true;
  Check(cw::LoopParts(f) == 44, "the second byte cuts it to 44 too");
  f.ghost = true;
  Check(cw::LoopParts(f) == cw::kPartSlots, "... unless the car is a ghost");
  f = cw::DrawFlags{};
  f.loop44_a = true;
  Check(!cw::PartDrawn(44, f) && !cw::PartDrawn(52, f) && cw::PartDrawn(43, f) && cw::PartDrawn(2, f), "with the loop at 44, the interior class (44 and up) is not drawn");
  f = cw::DrawFlags{};
  Check(cw::PartDrawn(44, f) && cw::PartDrawn(2, f), "with all 55 the interior is drawn");
  Check(!cw::PartDrawn(5, f) && (f.intercooler = true, cw::PartDrawn(5, f)), "the intercooler needs C+2205");
  f = cw::DrawFlags{};
  Check(cw::PartDrawn(6, f) && cw::PartDrawn(3, f), "the bumpers are drawn unless their byte says they are off");
  f.bumper_front_off = true;
  Check(!cw::PartDrawn(6, f) && cw::PartDrawn(3, f) && cw::PartDrawn(36, f) && !cw::PartDrawn(37, f), "front bumper off: the front sub-bumper shows, the rear one does not");
  f = cw::DrawFlags{};
  f.sub_bumpers = true;
  Check(cw::PartDrawn(36, f) && cw::PartDrawn(37, f), "the car object's byte lets both sub-bumpers be drawn");
}

// ---------------------------------------------------------------------------
// placement
// ---------------------------------------------------------------------------

void TestPlacement() {
  FakeMemory m;
  // an entry with an item array (3 bones) and a gWorld array (3 bones); bone 2 is the one asked for
  const uint32_t item = m.Alloc(64 * 3);
  const uint32_t worlds = m.Alloc(64 * 3);
  const tp::Mat4 a0 = Translation(100.0, 10.0, -50.0);
  const tp::Mat4 a2 = YawAt(0.5, 101.0, 11.0, -49.0);
  const tp::Mat4 b2 = YawAt(-0.2, 3.0, 4.0, 5.0);
  PutMatrix(m, item, a0);
  PutMatrix(m, item + 64, Translation(0, 0, 0));
  PutMatrix(m, item + 128, a2);
  PutMatrix(m, worlds + 128, b2);
  cw::PartEntry entry;
  entry.matrices = item;
  entry.world_matrices = worlds;
  const crt::BoneMatrices bone = crt::ReadBoneMatrices(m, entry, 2);
  Check(bone.item && bone.root && bone.world, "the item's bone, its first matrix and the gWorld array's bone are read");
  Check(Near(bone.a.m[3][0], 101.0) && Near(bone.a_root.m[3][1], 10.0) && Near(bone.b.m[3][2], 5.0) && bone.a.m[3][3] == 1.0 && bone.a.m[0][3] == 0.0,
        "the numbers are as put in; the fourth column (NaN in the game) is made (0, 0, 0, 1)");
  cw::PartEntry none;
  const crt::BoneMatrices missing = crt::ReadBoneMatrices(m, none, 2);
  Check(!missing.item && !missing.root && !missing.world, "an entry with no matrices reads none");
  Check(!crt::ReadBoneMatrices(m, entry, cw::kMaxMatrixBones).item, "a bone beyond the bound is not read");

  const tp::Mat4 entity = YawAt(1.0, 100.0, 10.0, -50.0);
  const crt::PoseSet set = crt::ComposePoses(bone, &entity);
  Check(set.ok[0] && set.ok[1] && set.ok[2] && set.ok[3] && set.ok[4], "all five hypotheses are made when every source is there");
  Check(crt::ComparePlacements(set.m[size_t(crt::Pose::kBoneWorld)], b2).Total() < 1e-6, "hypothesis 0 is the gWorld array's matrix");
  Check(crt::ComparePlacements(set.m[size_t(crt::Pose::kItemWorld)], a2).Total() < 1e-6, "hypothesis 1 is the item's matrix");
  tp::Mat4 palette = a2;
  palette.m[3][0] -= 100.0;
  palette.m[3][1] -= 10.0;
  palette.m[3][2] -= -50.0;
  Check(Near(crt::PaletteOf(a2, a0).m[3][0], 1.0) && Near(crt::PaletteOf(a2, a0).m[3][1], 1.0) && Near(crt::PaletteOf(a2, a0).m[3][2], 1.0),
        "the palette is the item's bone with the first bone's translation taken off (sub_823788C8)");
  Check(crt::ComparePlacements(set.m[size_t(crt::Pose::kPaletteBone)], tp::Mul(palette, b2)).Total() < 1e-6, "hypothesis 2 is the palette times the gWorld array's matrix");
  Check(crt::ComparePlacements(set.m[size_t(crt::Pose::kItemEntity)], tp::Mul(a2, entity)).Total() < 1e-6, "hypothesis 3 is the item's matrix times the entity's");
  Check(crt::ComparePlacements(set.m[size_t(crt::Pose::kPaletteEntity)], tp::Mul(palette, entity)).Total() < 1e-6, "hypothesis 4 is the palette times the entity's");
  const crt::PoseSet no_entity = crt::ComposePoses(bone, nullptr);
  Check(!no_entity.ok[3] && !no_entity.ok[4] && no_entity.ok[2], "with no entity matrix the two entity hypotheses are missing, the rest stay");
  crt::BoneMatrices only_world;
  only_world.world = true;
  only_world.b = b2;
  const crt::PoseSet partial = crt::ComposePoses(only_world, &entity);
  Check(partial.ok[0] && !partial.ok[1] && !partial.ok[2], "a source that was not read takes its hypotheses with it");

  // usable placements
  Check(crt::UsablePlacement(b2) && !crt::UsablePlacement(Translation(1e9, 0, 0)), "a placement is a car part's: finite, a believable size, a position under a million metres");
  tp::Mat4 big = b2;
  big.m[0][0] = big.m[0][1] = big.m[0][2] = 50.0;
  Check(!crt::UsablePlacement(big), "an axis 50 m long is no car part");
  tp::Mat4 nan = b2;
  nan.m[3][1] = std::numeric_limits<double>::quiet_NaN();
  Check(!crt::UsablePlacement(nan), "a placement with a NaN is none");

  // The error: the origins' distance and the largest difference of the 3x3.
  const crt::PlacementError e = crt::ComparePlacements(Translation(1, 2, 3), Translation(1, 2.5, 3));
  Check(Near(e.translation, 0.5) && Near(e.basis, 0.0), "the error of an offset placement is the offset");
  const crt::PlacementError e2 = crt::ComparePlacements(YawAt(0.1, 0, 0, 0), YawAt(0.0, 0, 0, 0));
  Check(Near(e2.translation, 0.0) && e2.basis > 0.09 && e2.basis < 0.11, "a rotated placement shows in the basis");
}

// The scene's view space: the walked instance and the draw's instance both through PackInstanceTransform; the error is the same in metres.
void TestViewError() {
  // the game's own view looks down -z; the scene flips z. A camera at (900, 20, -800) rotated 30 degrees: world to view, row vectors.
  const tp::Mat4 view = tp::Mul(Translation(-900.0, -20.0, 800.0), YawAt(0.5, 0, 0, 0));
  const tp::Mat4 drawn = YawAt(1.2, 905.0, 17.0, -812.0);
  tp::Mat4 walked = drawn;
  crt::PlacementError e;
  Check(crt::ViewError(walked, drawn, view, true, &e) && e.translation < 1e-5 && e.basis < 1e-5, "the same placement: no error in the view space");
  walked.m[3][0] += 0.03;
  Check(crt::ViewError(walked, drawn, view, true, &e) && Near(e.translation, 0.03, 1e-4), "a placement 3 cm off is 3 cm off in the view space too (a rigid view moves both alike)");
  walked = YawAt(1.2 + 0.02, 905.0, 17.0, -812.0);
  Check(crt::ViewError(walked, drawn, view, true, &e) && e.translation < 1e-5 && e.basis > 0.015 && e.basis < 0.025, "a rotation 0.02 rad off shows in the basis");
  tp::Mat4 bad = drawn;
  bad.m[0][0] = bad.m[1][1] = bad.m[2][2] = 0.0;
  Check(!crt::ViewError(bad, drawn, view, true, &e), "a placement with no volume is no instance: no number");
  // the conversion the first memory check lacked: world origin directly against a view-space matrix is far off, through the view it is exact
  float inst[3][4];
  Check(rr::PackInstanceTransform(drawn, view, true, inst), "the draw's instance");
  const double direct = std::sqrt(std::pow(drawn.m[3][0] - inst[0][3], 2) + std::pow(drawn.m[3][1] - inst[1][3], 2) + std::pow(drawn.m[3][2] - inst[2][3], 2));
  Check(direct > 100.0, "the world position and the instance's view-space translation are hundreds of metres apart: comparing them directly matches nothing");
}

// The calibration: the hypothesis that places the drawn cars' geometries where the draws put them is the pose.
crt::PoseSet SetOf(const tp::Mat4& truth_as, crt::Pose which, const tp::Mat4& decoy) {
  crt::PoseSet s;
  for (uint32_t i = 0; i < crt::kPoseCount; ++i) {
    s.ok[i] = true;
    s.m[i] = decoy;
  }
  s.m[size_t(which)] = truth_as;
  return s;
}

void TestCalibration() {
  crt::PoseCalibration c;
  const tp::Mat4 drawn = YawAt(0.7, 830.0, 17.0, -790.0);
  for (uint32_t i = 0; i < crt::PoseCalibration::kMinSamples - 1; ++i) {
    c.Add(SetOf(drawn, crt::Pose::kItemWorld, Translation(0, 0, 0)), drawn);
  }
  Check(!c.Decide() && !c.decided(), "fewer pairs than the minimum: not decided");
  c.Add(SetOf(drawn, crt::Pose::kItemWorld, Translation(0, 0, 0)), drawn);
  Check(c.Decide() && c.decided() && c.chosen() == crt::Pose::kItemWorld && c.fits(), "enough pairs: the hypothesis that equals the draws is the pose, and it fits");
  Check(c.tally(crt::Pose::kItemWorld).worst < 1e-9 && c.tally(crt::Pose::kBoneWorld).worst > 100.0, "the tallies say why: the others are hundreds of metres off");
  Check(!c.Describe().empty() && c.Describe().find("item matrix") != std::string::npos, "the description names the hypotheses");
  c.NewWindow();
  Check(c.decided() && c.chosen() == crt::Pose::kItemWorld && c.samples() == 0, "a new window forgets the tallies, not the choice");

  // none fits
  crt::PoseCalibration far;
  for (uint32_t i = 0; i < 30; ++i) {
    far.Add(SetOf(Translation(831.0, 17.0, -790.0), crt::Pose::kPaletteBone, Translation(0, 0, 0)), drawn);
  }
  Check(far.Decide() && far.chosen() == crt::Pose::kPaletteBone && !far.fits(), "the best hypothesis is a metre off: chosen as the least bad, but it does not fit: the scene places nothing by it");
  // an unusable placement is not counted
  crt::PoseCalibration skip;
  crt::PoseSet bad;
  bad.ok[0] = true;
  bad.m[0] = Translation(1e9, 0, 0);
  skip.Add(bad, drawn);
  Check(skip.samples() == 0, "a placement that is no car part's is not a pair");
  // two hypotheses close: the lower mean wins
  crt::PoseCalibration two;
  for (uint32_t i = 0; i < 30; ++i) {
    crt::PoseSet s;
    s.ok[size_t(crt::Pose::kBoneWorld)] = s.ok[size_t(crt::Pose::kPaletteBone)] = true;
    s.m[size_t(crt::Pose::kBoneWorld)] = Translation(830.0 + 0.05, 17.0, -790.0);
    s.m[size_t(crt::Pose::kPaletteBone)] = drawn;
    two.Add(s, drawn);
  }
  Check(two.Decide() && two.chosen() == crt::Pose::kPaletteBone, "the lower mean error wins");
}

// ---------------------------------------------------------------------------
// the mesh
// ---------------------------------------------------------------------------

struct Spec {
  uint32_t vertices = 4;
  uint32_t stride = 32;
  std::vector<uint32_t> indices = {0, 1, 2, 2, 1, 3};
  bool index32 = false;
  uint32_t endian = 1;
  uint32_t draw_mode = cw::kPrimitiveTriangleList;
  uint32_t primitive_count = 0;  // 0: indices / 3
  uint32_t decl_fvf = 0x0001;    // position only
  uint64_t decl_types = 6;       // position: float3
};

// A geometry object as the walk reads it (the same layout as car_model_walk_rules_test.cpp).
uint32_t MakeGeometry(FakeMemory& m, const Spec& s, uint32_t* vdata_out = nullptr, uint32_t* idata_out = nullptr) {
  const uint32_t vdata = m.Alloc(s.vertices * s.stride);
  for (uint32_t v = 0; v < s.vertices; ++v) {
    m.PF(vdata + v * s.stride + 0, 1.0f + float(v));
    m.PF(vdata + v * s.stride + 4, 0.5f * float(v));
    m.PF(vdata + v * s.stride + 8, -2.0f);
  }
  const uint32_t index_bytes = uint32_t(s.indices.size()) * (s.index32 ? 4u : 2u);
  const uint32_t idata = m.Alloc(index_bytes);
  for (uint32_t i = 0; i < s.indices.size(); ++i) {
    if (s.index32) {
      // 8in32: the guest holds them as written, the endian field says how to swap: here the data is the big-endian number (endian 0 = none)
      m.P32(idata + 4 * i, s.indices[i]);
    } else {
      m.P16(idata + 2 * i, s.indices[i]);
    }
  }
  const uint32_t decl = m.Alloc(16);
  m.P32(decl, s.decl_fvf);
  m.P32(decl + 8, uint32_t(s.decl_types >> 32));
  m.P32(decl + 12, uint32_t(s.decl_types));
  const uint32_t vh = m.Alloc(32);
  m.P32(vh + 24, (vdata & 0x1FFFFFFFu) | 3u);
  m.P32(vh + 28, 0x10000002u | ((s.vertices * s.stride) & cw::kFetchSizeMask));
  const uint32_t ih = m.Alloc(32);
  m.P32(ih + 0, (s.index32 ? 0x80000000u : 0u) | (s.endian << 29));
  m.P32(ih + 24, idata & 0x1FFFFFFFu);
  m.P32(ih + 28, index_bytes);
  const uint32_t vb = m.Alloc(32);
  m.P16(vb + 4, s.vertices);
  m.P32(vb + 8, vdata);
  m.P32(vb + 12, s.stride);
  m.P32(vb + 16, decl);
  m.P32(vb + 24, vdata);
  m.P32(vb + 28, vh);
  const uint32_t ib = m.Alloc(16);
  m.P32(ib + 4, uint32_t(s.indices.size()));
  m.P32(ib + 8, idata);
  m.P32(ib + 12, ih);
  const uint32_t g = m.Alloc(64);
  m.P32(g + 12, vb);
  m.P32(g + 28, ib);
  m.P32(g + 44, uint32_t(s.indices.size()));
  m.P32(g + 48, s.primitive_count != 0 ? s.primitive_count : uint32_t(s.indices.size() / 3));
  m.P16(g + 52, s.vertices);
  m.P16(g + 54, s.draw_mode);
  if (vdata_out) {
    *vdata_out = vdata;
  }
  if (idata_out) {
    *idata_out = idata;
  }
  return g;
}

cw::Geometry ReadIt(const FakeMemory& m, uint32_t g) {
  cw::Geometry geometry;
  cw::ReadGeometry(m, g, &geometry);
  geometry.in_use = true;
  geometry.bone = 0;
  return geometry;
}

void TestMesh() {
  FakeMemory m;
  std::vector<float> positions;
  std::vector<uint32_t> indices;
  crt::MeshRead info;

  // a 16-bit list
  Spec list;
  const uint32_t g1 = MakeGeometry(m, list);
  cw::Geometry geometry = ReadIt(m, g1);
  Check(crt::ReadGeometryMesh(m, geometry, &positions, &indices, &info) == crt::MeshProblem::kNone, "a 16-bit triangle list reads");
  Check(positions.size() == 12 && Near(positions[0], 1.0) && Near(positions[3], 2.0) && Near(positions[4], 0.5) && Near(positions[11], -2.0), "the positions: float3 at offset 0 of each 32-byte vertex");
  Check(indices == std::vector<uint32_t>({0, 1, 2, 2, 1, 3}) && info.triangles == 2 && info.vertices == 4 && !info.strip && !info.guessed, "the indices as written, two triangles");
  Check(info.bytes == 4u * 32u + 6u * 2u, "the bytes read are the vertices and the indices");

  // 32-bit indices, endian 2 (8in32): the guest holds big-endian numbers, the header's field swaps them
  Spec wide;
  wide.index32 = true;
  wide.endian = 2;
  const uint32_t g2 = MakeGeometry(m, wide);
  geometry = ReadIt(m, g2);
  Check(geometry.Index32() && geometry.IndexBytes() == 4, "a 32-bit index buffer by the header's bit 31");
  Check(crt::ReadGeometryMesh(m, geometry, &positions, &indices, &info) == crt::MeshProblem::kNone && indices == std::vector<uint32_t>({0, 1, 2, 2, 1, 3}),
        "32-bit big-endian indices read through the header's endian field");
  // endian 0: no swap, the bytes are as the host reads them (little endian)
  Spec raw;
  raw.index32 = true;
  raw.endian = 0;
  uint32_t raw_data = 0;
  const uint32_t g3 = MakeGeometry(m, raw, nullptr, &raw_data);
  for (uint32_t i = 0; i < 6; ++i) {
    const uint32_t v = raw.indices[i];
    m.At(raw_data + 4 * i)[0] = uint8_t(v);
    m.At(raw_data + 4 * i)[1] = uint8_t(v >> 8);
    m.At(raw_data + 4 * i)[2] = uint8_t(v >> 16);
    m.At(raw_data + 4 * i)[3] = uint8_t(v >> 24);
  }
  geometry = ReadIt(m, g3);
  Check(crt::ReadGeometryMesh(m, geometry, &positions, &indices, &info) == crt::MeshProblem::kNone && indices == std::vector<uint32_t>({0, 1, 2, 2, 1, 3}), "endian 0: no swap");

  // a strip: 5 indices, 3 triangles by the primitive count; the winding alternates; a restart index ends a run
  Spec strip;
  strip.vertices = 6;
  strip.indices = {0, 1, 2, 3, 4};
  strip.draw_mode = 5;
  strip.primitive_count = 3;
  const uint32_t g4 = MakeGeometry(m, strip);
  geometry = ReadIt(m, g4);
  Check(crt::ReadGeometryMesh(m, geometry, &positions, &indices, &info) == crt::MeshProblem::kNone && info.strip && indices == std::vector<uint32_t>({0, 1, 2, 2, 1, 3, 2, 3, 4}),
        "a strip is expanded, odd triangles flipped (counts: n = primitives + 2)");
  Spec restart;
  restart.vertices = 8;
  restart.indices = {0, 1, 2, 0xFFFF, 4, 5, 6};
  restart.draw_mode = 5;
  restart.primitive_count = 5;
  const uint32_t g5 = MakeGeometry(m, restart);
  geometry = ReadIt(m, g5);
  Check(crt::ReadGeometryMesh(m, geometry, &positions, &indices, &info) == crt::MeshProblem::kNone && info.strip && indices == std::vector<uint32_t>({0, 1, 2, 4, 5, 6}),
        "a restart index (0xFFFF) ends the run");

  // neither
  Spec odd;
  odd.indices = {0, 1, 2, 3};
  odd.draw_mode = 9;
  odd.primitive_count = 7;
  const uint32_t g6 = MakeGeometry(m, odd);
  geometry = ReadIt(m, g6);
  Check(crt::ReadGeometryMesh(m, geometry, &positions, &indices, &info) == crt::MeshProblem::kPrimitive && positions.empty() && indices.empty(), "four indices that are no list and no strip by their counts: refused, nothing read");
  Spec guess;
  guess.indices = {0, 1, 2, 3, 2, 1};
  guess.draw_mode = 9;
  guess.primitive_count = 7;
  const uint32_t g7 = MakeGeometry(m, guess);
  geometry = ReadIt(m, g7);
  Check(crt::ReadGeometryMesh(m, geometry, &positions, &indices, &info) == crt::MeshProblem::kNone && info.guessed && indices.size() == 6, "a multiple of three that the counts do not explain is read as a list, and said so");

  // a position that is not a float3 at 0
  Spec packed;
  packed.decl_types = 1;  // Half2
  const uint32_t g8 = MakeGeometry(m, packed);
  geometry = ReadIt(m, g8);
  Check(crt::ReadGeometryMesh(m, geometry, &positions, &indices, &info) == crt::MeshProblem::kPosition, "a position that is not float3 is refused");
  Spec thin;
  thin.stride = 8;
  const uint32_t g9 = MakeGeometry(m, thin);
  geometry = ReadIt(m, g9);
  Check(crt::ReadGeometryMesh(m, geometry, &positions, &indices, &info) == crt::MeshProblem::kPosition, "a stride under 12 cannot hold a float3");

  // unreadable data / no buffers
  geometry = ReadIt(m, g1);
  geometry.vb_data = 0x00020000u;
  Check(crt::ReadGeometryMesh(m, geometry, &positions, &indices, &info) == crt::MeshProblem::kUnreadable, "a vertex range that is not guest memory is refused");
  geometry = ReadIt(m, g1);
  geometry.ib_ok = false;
  Check(crt::ReadGeometryMesh(m, geometry, &positions, &indices, &info) == crt::MeshProblem::kNoBuffers, "no index buffer: refused");
  geometry = ReadIt(m, g1);
  geometry.vb_stride = 1u << 28;
  geometry.vb_w7 = 0;
  Check(crt::ReadGeometryMesh(m, geometry, &positions, &indices, &info) == crt::MeshProblem::kTooBig, "a buffer beyond the bound is refused");
  // the fetch window caps the vertex count
  geometry = ReadIt(m, g1);
  geometry.vb_count = 1000;
  geometry.vertex_count = 1000;
  Check(crt::ReadGeometryMesh(m, geometry, &positions, &indices, &info) == crt::MeshProblem::kNone && info.vertices == 4, "the fetch window's size caps the vertex count");

  // the key: per buffer address; the same buffers are the same key, any other is another; never 0
  geometry = ReadIt(m, g1);
  const cw::Geometry again = ReadIt(m, g1);
  const cw::Geometry other = ReadIt(m, g2);
  Check(crt::GeometryKey(geometry) == crt::GeometryKey(again) && crt::GeometryKey(geometry) != crt::GeometryKey(other) && crt::GeometryKey(geometry) != 0,
        "the key follows the buffers' addresses: two cars of one model name the same geometry, another geometry is another key");
  cw::Geometry resized = geometry;
  resized.index_count += 3;
  Check(crt::GeometryKey(resized) != crt::GeometryKey(geometry), "the key follows the counts");

  // cost before reading
  uint32_t triangles = 0;
  uint64_t bytes = 0;
  crt::CostOf(geometry, &triangles, &bytes);
  Check(triangles == 2 && bytes == 4u * 32u + 12u, "the cost of a geometry before it is read: its triangles and its guest bytes");
}

// ---------------------------------------------------------------------------
// the picks, the paint
// ---------------------------------------------------------------------------

cw::Geometry MakeSimple(uint32_t indices, bool in_use = true) {
  cw::Geometry g;
  g.read = g.vb_ok = g.ib_ok = in_use ? true : true;
  g.in_use = in_use;
  g.index_count = indices;
  g.vertex_count = indices;
  g.vb_stride = 32;
  g.bone = 0;
  g.vb_w6 = 0x1000u + indices * 4;
  return g;
}

void TestPicks() {
  cw::CarWalk car;
  car.geometries.push_back(MakeSimple(300));
  car.geometries.push_back(MakeSimple(12));          // under the minimum
  car.geometries.push_back(MakeSimple(900));
  car.geometries.push_back(MakeSimple(600, false));  // not in use
  cw::Geometry skinned = MakeSimple(450);
  skinned.palette_count = 5;
  car.geometries.push_back(skinned);
  cw::Geometry no_bone = MakeSimple(450);
  no_bone.bone = cw::kNoBone;
  car.geometries.push_back(no_bone);
  car.geometries.push_back(MakeSimple(150));
  const std::vector<uint32_t> picks = crt::PickGeometries(car, 18, 0);
  Check(picks == std::vector<uint32_t>({2, 0, 6}), "the in-use rigid geometries of at least 18 indices, the largest first (not the small, the unused, the skinned or the unplaced)");
  Check(crt::PickGeometries(car, 18, 2) == std::vector<uint32_t>({2, 0}), "at most N of them: the largest");
  Check(crt::PickGeometries(car, 200, 0) == std::vector<uint32_t>({2, 0}), "the class's minimum (Mid: 45) drops the small ones");

  cw::Paint paint;
  Check(!crt::PaintColourOf(paint).known, "a paint that was not read is not known");
  paint.read = true;
  paint.a[0] = 0.063f;
  paint.a[1] = 0.349f;
  paint.a[2] = 0.071f;
  paint.a[3] = std::numeric_limits<float>::quiet_NaN();  // the game's alpha is NaN
  paint.b[0] = 0.514f;
  paint.b[1] = 0.541f;
  paint.b[2] = 0.0f;
  paint.mode = 0;
  crt::PaintColour c = crt::PaintColourOf(paint);
  Check(c.known && Near(c.rgb[0], 0.063, 1e-6) && Near(c.rgb[1], 0.349, 1e-6) && Near(c.rgb[2], 0.071, 1e-6), "region 0's colour A (the log's green car), the NaN alpha ignored");
  paint.mode = 2;
  c = crt::PaintColourOf(paint);
  Check(c.known && Near(c.rgb[0], 0.2885, 1e-5) && Near(c.rgb[1], 0.445, 1e-5), "a gradient: the mean of A and B");
  paint.a[1] = std::numeric_limits<float>::quiet_NaN();
  Check(!crt::PaintColourOf(paint).known, "a colour that is not a number is not known");
  paint.a[1] = 3.0f;
  paint.mode = 0;
  Check(Near(crt::PaintColourOf(paint).rgb[1], 1.0), "a colour is clamped to 0..1");
  Check(crt::Describe(crt::PaintColour{}) == "not known" && crt::Describe(c).find("0.") != std::string::npos, "the words");
}

// ---------------------------------------------------------------------------
// which cars, how much
// ---------------------------------------------------------------------------

crt::CarInfo Info(uint32_t vehicle, double distance, bool visible = false, bool drawn = false, double x = 0.0, uint32_t parts = 100) {
  crt::CarInfo c;
  c.vehicle = vehicle;
  c.distance = distance;
  c.visible = visible;
  c.drawn = drawn;
  c.parts = parts;
  c.position[0] = x + double(vehicle) * 5.0;  // the cars stand 5 m apart sideways: the same-car tolerance is 3 m
  c.position[2] = -distance;
  return c;
}

void TestChoose() {
  const ci::Limits high = ci::ClassLimits(mcla::gpu::gpu_class::GpuClass::kHigh);
  const ci::Limits mid = ci::ClassLimits(mcla::gpu::gpu_class::GpuClass::kMid);
  std::vector<crt::CarInfo> cars = {Info(1, 0.0), Info(2, 12.0), Info(3, 3.5), Info(4, 6.0, true), Info(5, 9.0), Info(6, 30.0), Info(7, 5.0, false, true), Info(8, 4.0), Info(9, 8.0)};
  cars[0].is_player = true;
  // stage: not the player, not visible, not drawn, within the radius (14 m), nearest first, the class's count and a spare
  std::vector<uint32_t> staged = crt::ChooseStaged(cars, high);
  // the cars at 3.5, 4, 8, 9 and 12 m are indices 2, 7, 8, 4, 1; the one at 6 m is visible, the one at 5 m is drawn, the one at 30 m is out of the radius
  Check(staged == std::vector<uint32_t>({2, 7, 8, 4, 1}), "High stages the nearest 4 cars within 14 m the main view does not show, and a spare, nearest first");
  staged = crt::ChooseStaged(cars, mid);
  Check(staged.size() == 3 && staged[0] == 2 && staged[1] == 7 && staged[2] == 8, "Mid: 2 cars within 10 m and a spare");
  Check(crt::ChooseStaged(cars, ci::Limits{}).empty(), "no class limits: none");
  cars[7].parts = 0;
  staged = crt::ChooseStaged(cars, high);
  Check(std::find(staged.begin(), staged.end(), 7u) == staged.end(), "a car with no part to bring is not staged");
  cars[7].parts = 100;

  // place: not drawn (the draw path keeps those), within the radius, at most the class's count
  const std::vector<ci::OtherCar> none;
  std::vector<uint32_t> placed = crt::ChoosePlaced(cars, high, crt::Mode::kModelFirst, none);
  Check(placed == std::vector<uint32_t>({2, 7, 3, 8}), "High places the 4 nearest cars within 14 m that the frame does not draw (the one at 6 m is visible by the game's flag but has no draw found: placed)");
  Check(std::find(placed.begin(), placed.end(), 5u) == placed.end() && std::find(placed.begin(), placed.end(), 6u) == placed.end(), "the car at 30 m is beyond the radius, the one the frame draws is the draw path's");
  placed = crt::ChoosePlaced(cars, mid, crt::Mode::kModelFirst, none);
  Check(placed == std::vector<uint32_t>({2, 7}), "Mid places 2");
  Check(crt::ChoosePlaced(cars, high, crt::Mode::kOff, none).empty(), "mode off: none");
  // the cars the camera draws are the draw path's: a visible car whose draws the frame did make is `drawn`, one that is only visible (no draw found) is placed
  cars[7].drawn = true;
  placed = crt::ChoosePlaced(cars, high, crt::Mode::kModelFirst, none);
  Check(std::find(placed.begin(), placed.end(), 7u) == placed.end(), "a drawn car is never placed from the walk");
  cars[7].drawn = false;

  // gaps: a walked car where a remembered car stands is left to its kit; model first drops the kit's car instead
  ci::OtherCar kit;
  kit.stencil_id = crt::kWalkedIdBase - 1;
  kit.remembered = true;
  kit.Add(Translation(15.0, 0.0, -3.5), 3.5);  // at car 2's place (vehicle 3: x 15, z -3.5), its centre rises by kCentreRise
  std::vector<ci::OtherCar> remembered = {kit};
  placed = crt::ChoosePlaced(cars, high, crt::Mode::kGaps, remembered);
  Check(placed == std::vector<uint32_t>({7, 3, 8, 4}), "gaps: the car a kit places is left out, the next one takes its place in the count");
  placed = crt::ChoosePlaced(cars, high, crt::Mode::kModelFirst, remembered);
  Check(placed[0] == 2u, "model first: the walked car is placed whatever the kit does");
  const std::vector<uint32_t> drop = crt::RememberedCovered(remembered, cars, placed);
  Check(drop == std::vector<uint32_t>({0}), "... and the kit's car beside it is dropped");
  // paint for a remembered car: the nearest walked car within 3 m that has one
  cars[2].paint.known = true;
  cars[2].paint.rgb[0] = 0.1f;
  cars[2].paint.rgb[1] = 0.2f;
  cars[2].paint.rgb[2] = 0.3f;
  float rgb[3] = {};
  Check(crt::PaintForRemembered(kit, cars, rgb) && Near(rgb[0], 0.1, 1e-6) && Near(rgb[2], 0.3, 1e-6), "a remembered car takes the paint of the walked car at its place");
  ci::OtherCar far_kit;
  far_kit.Add(Translation(40.0, 0.0, -40.0), 5.0);
  Check(!crt::PaintForRemembered(far_kit, cars, rgb), "no walked car with a paint near it: none");
  ci::OtherCar empty;
  Check(!crt::PaintForRemembered(empty, cars, rgb), "a car with no draws has no place");

  // paint for a car the camera draws (RT-MODEL-CARS-3): the nearest walked car that is not the player's, within 3 m of the draws' mean origin
  std::vector<crt::CarInfo> row;
  const auto car_at = [](double x, double z, bool player, bool painted, float r) {
    crt::CarInfo c;
    c.is_player = player;
    c.position[0] = x;
    c.position[1] = 0.0;
    c.position[2] = z;
    c.paint.known = painted;
    c.paint.rgb[0] = r;
    c.paint.rgb[1] = r;
    c.paint.rgb[2] = r;
    return c;
  };
  row.push_back(car_at(0.0, 0.0, true, true, 0.9f));    // the player's, with a paint
  row.push_back(car_at(3.4, 0.0, false, true, 0.2f));   // the neighbour on the right
  row.push_back(car_at(-4.2, 0.5, false, true, 0.6f));  // the neighbour on the left
  ci::OtherCar right, left, between, unpainted;
  right.Add(Translation(3.3, 0.0, 0.2), 3.3);
  left.Add(Translation(-4.1, 0.0, 0.4), 4.1);
  between.Add(Translation(0.0, 0.0, 0.0), 0.0);  // at the player's place: within 3 m of nobody else
  Check(crt::PaintForDrawn(right, row, rgb) && Near(rgb[0], 0.2, 1e-6), "a drawn car takes the paint of the walked car at its place (the right one)");
  Check(crt::PaintForDrawn(left, row, rgb) && Near(rgb[0], 0.6, 1e-6), "... and the left one the left one's");
  Check(!crt::PaintForDrawn(between, row, rgb), "the player's own car is never a drawn car's paint");
  row.push_back(car_at(3.5, 0.0, false, false, 0.0f));  // a car with no paint, nearer to the right neighbour's draws than the painted one
  unpainted.Add(Translation(3.5, 0.0, 0.0), 3.5);
  Check(!crt::PaintForDrawn(unpainted, row, rgb), "the nearest walked car decides: one with no paint does not borrow its neighbour's");
  Check(!crt::PaintForDrawn(empty, row, rgb), "a car with no draws has no place");
  ci::OtherCar far;
  far.Add(Translation(30.0, 0.0, 30.0), 40.0);
  Check(!crt::PaintForDrawn(far, row, rgb), "no walked car within 3 m: none");
}

// ---------------------------------------------------------------------------
// budgets, caching, cleanup
// ---------------------------------------------------------------------------

void TestBudgets() {
  const ci::Limits high = ci::ClassLimits(mcla::gpu::gpu_class::GpuClass::kHigh);
  crt::StageBudget spent;
  Check(crt::MayStage(spent, 500000, 100u << 20, high), "the first mesh of a frame is always taken, however large");
  spent.triangles = 500000;
  spent.bytes = 1;
  Check(!crt::MayStage(spent, 1, 1, high), "after it the budget (60000 triangles on High) is spent");
  spent = {};
  spent.triangles = 30000;
  spent.bytes = 700u * 1024u;
  Check(crt::MayStage(spent, 20000, 60u * 1024u, high), "within both limits");
  Check(!crt::MayStage(spent, 40000, 1, high), "over the triangles");
  Check(!crt::MayStage(spent, 1, 100u * 1024u, high), "over the 768 KiB of guest data");
  const ci::Limits mid = ci::ClassLimits(mcla::gpu::gpu_class::GpuClass::kMid);
  spent.triangles = 20000;
  spent.bytes = 10;
  Check(crt::MayStage(spent, 10000, 10, mid) && !crt::MayStage(spent, 10001, 10, mid), "Mid: 30000 triangles");

  // the build plan: the total budget as the scene's, the walked meshes within their own
  std::vector<crt::BuildItem> items = {{50000, false}, {30000, true}, {30000, true}, {40000, false}, {20000, true}};
  std::vector<uint8_t> plan = crt::PlanBuilds(items, 150000, 40000);
  Check(plan == std::vector<uint8_t>({1, 1, 0, 1, 0}), "the player's 50000 and the first walked mesh go, the second walked one waits (the walked budget 40000), the next draw's goes, the total (150000) stops the rest");
  plan = crt::PlanBuilds({{200000, true}, {10, true}}, 150000, 40000);
  Check(plan == std::vector<uint8_t>({1, 0}), "the first mesh of a frame is always built, however large");
  plan = crt::PlanBuilds({{10, true}, {200000, false}}, 150000, 40000);
  Check(plan == std::vector<uint8_t>({1, 0}), "a large mesh after a small one is held by the total budget");
  Check(crt::PlanBuilds({}, 1, 1).empty(), "no staged mesh, no plan");

  // the structure cache
  crt::WalkSignature a;
  a.model_set = 0x1000;
  a.body_lod = 1;
  a.part_lod = 1;
  a.variants = 77;
  a.flags = 0;
  a.min_lod = 1;
  a.walked_frame = 100;
  crt::WalkSignature now = a;
  Check(crt::WalkStillGood(a, now, 150), "the same car a few frames later: the cached walk stands");
  Check(!crt::WalkStillGood(a, now, 100 + crt::kRewalkFrames), "after kRewalkFrames it is walked again");
  now.body_lod = 2;
  Check(!crt::WalkStillGood(a, now, 120), "a lod change makes it walked again at once");
  now = a;
  now.variants = 78;
  Check(!crt::WalkStillGood(a, now, 120), "so does a part variant that changed");
  now = a;
  now.min_lod = 0;
  Check(!crt::WalkStillGood(a, now, 120), "and the lod floor (the car came into view)");
  now = a;
  now.model_set = 0;
  Check(!crt::WalkStillGood(now, now, 120), "no model set: never good");

  // the cleanup when a car leaves
  crt::WalkedSet set;
  std::vector<crt::WalkedSet::Placed> placed(2);
  placed[0].vehicle = 1;
  placed[0].keys = {10, 11, 12};
  placed[1].vehicle = 2;
  placed[1].keys = {12, 13};  // 12 is shared (one model)
  Check(set.Update(1000, placed).empty() && set.cars() == 2, "two cars placed: nothing to give back");
  placed.erase(placed.begin());  // car 1 leaves (car 2 is still placed)
  Check(set.Update(1000 + crt::kLeaveFrames, placed).empty(), "car 1 is gone for less than kLeaveFrames: its meshes stay");
  std::vector<uint64_t> give = set.Update(1000 + crt::kLeaveFrames + 1, placed);
  Check(give == std::vector<uint64_t>({10, 11}) && set.cars() == 1, "after kLeaveFrames car 1's meshes go back, except the one car 2 still names");
  Check(set.Update(1000 + crt::kLeaveFrames + 2, placed).empty(), "and only once");
  give = set.Update(1000 + 2 * crt::kLeaveFrames + 5, {});
  Check(give == std::vector<uint64_t>({12, 13}) && set.cars() == 0, "when car 2 leaves too, all its meshes go");
  set.Update(5000, placed);
  set.Clear();
  Check(set.cars() == 0, "Clear forgets everyone");
}

// ---------------------------------------------------------------------------
// which car a draw is of
// ---------------------------------------------------------------------------

// A walk with `n` geometries whose buffers are numbered from `first` (two cars of one model have the same `first`: the same buffers).
cw::CarWalk TwinWalk(uint32_t first, uint32_t n) {
  cw::CarWalk w;
  w.ok = true;
  for (uint32_t i = 0; i < n; ++i) {
    cw::Geometry g;
    g.read = g.vb_ok = g.ib_ok = true;
    g.in_use = true;
    g.vb_w6 = ((first + i) * 0x10000u) | 3u;
    g.vb_w7 = 0x10000002u | (((first + i) * 64u) & cw::kFetchSizeMask);
    g.ib_header = 0x40000000u + (first + i) * 0x100u;
    g.ib_w6 = 0x1000u + (first + i) * 0x40u;
    g.index_count = 30;
    w.geometries.push_back(g);
  }
  return w;
}

cw::DrawKey DrawOf(const cw::CarWalk& w, uint32_t geometry, uint32_t id, double x, double y, double z) {
  cw::DrawKey d;
  const cw::Geometry& g = w.geometries[geometry];
  d.id = id;
  d.vertex_d0 = g.vb_w6;
  d.vertex_d1 = g.vb_w7;
  d.indexed = true;
  d.index_object = g.ib_header;
  d.index_address = g.ib_w6;
  d.has_world = true;
  d.world = Translation(x, y, z);
  return d;
}

void TestAssign() {
  // two cars of one model (the same buffers), 10 m apart: car A is drawn under ID 3, car B is culled
  const cw::CarWalk a = TwinWalk(1, 4);
  const cw::CarWalk b = TwinWalk(1, 4);
  const cw::CarWalk other = TwinWalk(50, 3);  // another model, far away
  const std::vector<const cw::CarWalk*> cars = {&a, &b, &other};
  const std::vector<std::array<double, 3>> positions = {{100.0, 0.0, 0.0}, {110.0, 0.0, 0.0}, {300.0, 0.0, 0.0}};
  std::vector<cw::DrawKey> draws;
  for (uint32_t g = 0; g < 4; ++g) {
    draws.push_back(DrawOf(a, g, 3, 100.5, 0.5, 0.3));
  }
  crt::Assignment as = crt::AssignDraws(cars, positions, draws);
  Check(as.drawn.size() == 3 && as.drawn[0] == 1 && as.drawn[1] == 0 && as.drawn[2] == 0, "the twin whose draws the frame made is drawn, its twin 10 m away (the same buffers) is not, the other model is not");
  bool all_a = true;
  for (uint32_t d = 0; d < 4; ++d) {
    all_a = all_a && as.draws[d].car == 0 && as.draws[d].geometry == int32_t(d);
  }
  Check(all_a && as.ids == 1 && as.ids_assigned == 1, "every draw is car A's geometry of the same number (not the first owner of the buffers, which is A here by luck: see the next check)");
  // the same draws made at B's place: B is the drawn one (MatchFrame would have given them to A, the first owner)
  draws.clear();
  for (uint32_t g = 0; g < 4; ++g) {
    draws.push_back(DrawOf(a, g, 5, 110.2, 0.4, -0.3));
  }
  as = crt::AssignDraws(cars, positions, draws);
  Check(as.drawn[0] == 0 && as.drawn[1] == 1, "draws at the second twin's place are the second twin's: it is drawn, the first is not");
  Check(as.draws[0].car == 1 && as.draws[3].car == 1 && as.draws[3].geometry == 3, "each draw names the twin it is of");
  // both twins drawn, two IDs
  draws.clear();
  for (uint32_t g = 0; g < 4; ++g) {
    draws.push_back(DrawOf(a, g, 3, 100.5, 0.5, 0.3));
    draws.push_back(DrawOf(a, g, 5, 110.2, 0.4, -0.3));
  }
  as = crt::AssignDraws(cars, positions, draws);
  Check(as.drawn[0] == 1 && as.drawn[1] == 1 && as.ids == 2 && as.ids_assigned == 2 && as.draws[0].car == 0 && as.draws[1].car == 1, "two IDs, two twins: both are drawn, each by its own draws");
  // RT-MODEL-CARS-3: the debug comparison (car_model_walk::MatchFrame) with each draw's owner: the twin 10 m away that the frame does not draw is not credited with its twin's draws
  // (the first check run's 88.7 percent car: a racer 56 m away of the neighbour's model, 19 part variants different, scored against the neighbour's draws)
  {
    draws.clear();
    for (uint32_t g = 0; g < 4; ++g) {
      draws.push_back(DrawOf(a, g, 3, 100.5, 0.5, 0.3));
    }
    const crt::Assignment one = crt::AssignDraws(cars, positions, draws);
    std::vector<int32_t> owner(draws.size(), -1);
    for (size_t d = 0; d < draws.size(); ++d) {
      owner[d] = one.draws[d].car;
    }
    const cw::FrameMatch plain = cw::MatchFrame(cars, draws);
    const cw::FrameMatch owned = cw::MatchFrame(cars, draws, &owner);
    Check(plain.cars[0].drawn && plain.cars[1].drawn, "by the buffers alone both twins look drawn");
    Check(owned.cars[0].drawn && !owned.cars[1].drawn && owned.cars[0].matched == 4 && owned.cars[1].matched == 0 && owned.cars[1].draws == 0 && owned.cars[1].ids.empty(),
          "with the owners only the twin the draws are of is drawn: the other matched nothing");
    Check(owned.draws_matched == 4 && owned.draws_vertex_only == 0, "the four draws are matched once");
  }
  // an ID far from every candidate is nobody's
  draws.clear();
  draws.push_back(DrawOf(a, 0, 7, 160.0, 0.0, 0.0));
  as = crt::AssignDraws(cars, positions, draws);
  Check(!as.drawn[0] && !as.drawn[1] && as.ids == 1 && as.ids_assigned == 0 && as.draws[0].car == -1, "an ID whose draws are 50 m from both twins is given to neither");
  // a draw with no world matrix and one owner: the owner; with two owners: nobody (nothing says which)
  draws.clear();
  cw::DrawKey blind = DrawOf(other, 1, 9, 0, 0, 0);
  blind.has_world = false;
  draws.push_back(blind);
  as = crt::AssignDraws(cars, positions, draws);
  Check(as.drawn[2] == 1 && as.draws[0].car == 2 && as.draws[0].geometry == 1, "no matrix, one owner: the owner");
  cw::DrawKey blind2 = DrawOf(a, 1, 9, 0, 0, 0);
  blind2.has_world = false;
  draws.assign(1, blind2);
  as = crt::AssignDraws(cars, positions, draws);
  Check(!as.drawn[0] && !as.drawn[1] && as.ids_assigned == 0, "no matrix and two owners (twins): nothing says which, so neither is called drawn");
  // a draw that names no walked geometry
  draws.assign(1, DrawOf(a, 0, 2, 100.0, 0.0, 0.0));
  draws[0].vertex_d0 = 0x77770003u;
  as = crt::AssignDraws(cars, positions, draws);
  Check(as.ids == 0 && !as.drawn[0] && as.draws[0].car == -1, "a draw of buffers no walked car has is nobody's");
  // an indexed draw of the right vertices with another index buffer is not that geometry
  draws.assign(1, DrawOf(a, 0, 2, 100.0, 0.0, 0.0));
  draws[0].index_object = 0x5555u;
  draws[0].index_address = 0x6666u;
  as = crt::AssignDraws(cars, positions, draws);
  Check(as.ids == 0 && !as.drawn[0], "the vertex window alone is not enough: the index buffer must be the geometry's");
}

}  // namespace

int main() {
  TestLodRule();
  TestPartRules();
  TestPlacement();
  TestViewError();
  TestCalibration();
  TestMesh();
  TestPicks();
  TestChoose();
  TestBudgets();
  TestAssign();
  std::printf("car_model_rt_rules_test: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
