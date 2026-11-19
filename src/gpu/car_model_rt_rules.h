#pragma once
// ===========================================================================
// MCLA GPU renderer -- the ray traced scene's cars built from the cars' own model data: the rules that need no renderer (RT-MODEL-CARS phase 2)
// ===========================================================================
// PHASE 1 (car_model_walk_rules.h, car_model_walk.h) reads every car's model data from guest memory: the part table, each part's drawable, its geometries and
// their vertex and index buffers, the bones, the paint, and proved on the first check run that what it reads is what the game draws (97 to 100 percent of the
// in-use geometries are draws of the frame; every culled car has resident, valid geometry). PHASE 2 builds the ray traced scene from it: a car the camera does
// not draw this frame gets its triangles from the walk, its place from the walk's part matrices and its colour from its paint. This header is what needs no
// device, as pure functions on plain numbers and on a memory reader, so that car_model_rt_rules_test.cpp builds on its own:
//
//   - PLACEMENT. A geometry is placed by the matrices of its model's bone: the part's item matrices (V+6960+4p -> +20, what the palette is made from) and the
//     array the draw sets gWorld from (V+5740+4p). The asm says the draw's vertex shader makes [p 1] * palette(bone) * gWorld and that the palette is the item's
//     bone matrix with the first bone's translation taken off, but not for certain what gWorld is for a part, so FIVE hypotheses (Pose) are made for every
//     geometry and the walker CHECKS THEM AGAINST THE DRAWS of the frame for the cars the camera does draw (the player's included): the hypothesis with the
//     smallest error against the draws' own model-to-world matrices (PoseCalibration) places the cars that are not drawn. The error is the proof line.
//   - THE VIEW SPACE. The scene's top level is in the frame's VIEW space (camera-relative: car_rt_scene_rules.h PackInstanceTransform); the walk's matrices are
//     WORLD space (the game's own, like the vehicle list). A walked instance goes through the same PackInstanceTransform as a draw's, with the same view and the
//     same z flip: ViewError compares the two instances of one geometry in view space, in metres. The first check run's memory path compared world with view
//     directly and matched 0 cars; this is the conversion it lacked.
//   - THE MESH. ReadGeometryMesh reads a geometry's positions (float3 at offset 0 of the vertex, the declared stride, big endian) and indices (16 or 32 bit, the
//     header's endian, a list or a strip) into the vectors the scene stages (car_rt_scene.h), cached per buffer address (GeometryKey: the same model's cars share
//     the keys, so a second car of a model costs instances only).
//   - THE PAINT. PaintColour: region 0's colour A (the base colour of the gradient), the mean of A and B where the car has a gradient.
//   - WHICH CARS, HOW MUCH. The class's caps (ci::Limits: High 4 cars within 14 m, Mid 2 within 10 m) ranked by the distance to the player, a walked car
//     only where no drawn car is (the draw path keeps the cars it draws) and, by the mode, where no remembered car is; the per-frame budgets of a car that is new
//     (triangles and KiB read, triangles built); the cleanup when a car leaves (WalkedSet).
//
// Matrices are row vectors (v' = v M) as in temporal_math.h. This header reads guest memory only through the `Mem` of car_model_walk_rules.h.
//
// WHAT IS NOT VERIFIED (nothing here has run; the check script h11-work/rt-model-cars-2/check-model-rt.ps1 answers each):
//   CHECK 1  which of the five placement hypotheses is the game's (the PLACEMENT table of the first run says; until a hypothesis fits the draws within kFitMetres no walked
//            car is placed), and that the part matrices of a car nobody draws are as fresh as a drawn car's (survey CHECK 3: the proof compares drawn cars only);
//   CHECK 2  the paint colour as the shader shows it (PaintColourOf: region 0's colour A, the mean with B for a gradient; the numbers are the game's constants, 0..1);
//   CHECK 3  the draw loop's per-part tests (PartDrawn: asm only) and the meaning of the two bytes that cut it to 44 parts (the walk logs them: `flags=`, `loop=`);
//   CHECK 4  that a draw's primitive is a list or a strip by the geometry's counts (the loader's draw-mode number 3 is a list; strips are read by counts alone);
//   NOT DONE the wheels (the wheel manager's drawables are not in the model set: a walked car has no tyres), the driver, traffic and parked cars (not vehicles: the
//            ambient chain of the survey, section 3), a glass / chrome flag per part (every walked part is an opaque body of the others' mask).
// ===========================================================================

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "car_dump_rules.h"
#include "car_model_walk_rules.h"
#include "car_rt_instances_rules.h"
#include "car_rt_memory_rules.h"
#include "car_rt_scene_rules.h"
#include "temporal_math.h"

namespace mcla::gpu::car_model_rt {

namespace cw = car_model_walk;
namespace tp = temporal;
namespace ci = car_rt_instances;
namespace rr = car_rt_rules;

// ---------------------------------------------------------------------------
// the numbers
// ---------------------------------------------------------------------------

// The most cars the walk puts in the scene at once (the class's count is at most ci::kMaxCars; the IDs of walked cars follow the remembered cars').
inline constexpr uint32_t kMaxWalkedCars = 8;
// The stencil ID of a walked car in the instance list: after the remembered cars' (car_rt_memory_rules.h kRememberedIdBase + kMaxKits).
inline constexpr uint32_t kWalkedIdBase = car_rt_memory::kRememberedIdBase + car_rt_memory::kMaxKits;
static_assert(kWalkedIdBase + kMaxWalkedCars <= ci::kIdSpace, "the instance list's slot table has room for the walked cars' IDs");
// The smallest part (in indices) a walked car brings: the car's smallest parts show in no reflection and every geometry costs an instance and a structure.
inline constexpr uint32_t kMinPartIndices = 18;
// How far a walked car and a remembered car may be apart (their positions, metres) to be the same car.
inline constexpr double kSameCarMetres = 3.0;
// Frames a walked car may be out of the scene before its meshes are given back.
inline constexpr uint64_t kLeaveFrames = 120;
// A walk's cached structure is made again this often (frames), and at once when the lod, the model set or the part variants change.
inline constexpr uint64_t kRewalkFrames = 90;
// A car is staged (its triangles read from guest memory) while its +116 says the main view does not draw it; and this many cars beyond the class's count are
// kept staged so that a car that comes near is ready.
inline constexpr uint32_t kStageSpareCars = 1;

// How the scene takes the walked cars (mcla_gpu_car_rt_cars_model).
enum class Mode : int {
  kOff = 0,        // no walked cars
  kGaps = 1,       // a walked car only where no remembered car stands (the kits are the better source where they exist)
  kModelFirst = 2  // a walked car wherever it is; a remembered car beside it is dropped
};

// ---------------------------------------------------------------------------
// placement: the hypotheses
// ---------------------------------------------------------------------------

enum class Pose : uint8_t {
  kBoneWorld,      // B[b]: the array the draw sets gWorld from (V+5740+4p), the bone's matrix as it is
  kItemWorld,      // A[b]: the item's matrix as it is (V+6960+4p -> +20)
  kPaletteBone,    // palette(A)[b] * B[b]: the shader's own recipe if gWorld is the bone's matrix: the item's bone with the first bone's translation off, then gWorld
  kItemEntity,     // A[b] * E: the item's matrix is local to the car (E the entity's world matrix)
  kPaletteEntity,  // palette(A)[b] * E
};
inline constexpr uint32_t kPoseCount = 5;

inline const char* PoseName(Pose pose) {
  switch (pose) {
    case Pose::kBoneWorld: return "gWorld array B[bone]";
    case Pose::kItemWorld: return "item matrix A[bone]";
    case Pose::kPaletteBone: return "palette(A)[bone] * B[bone]";
    case Pose::kItemEntity: return "A[bone] * entity";
    case Pose::kPaletteEntity: return "palette(A)[bone] * entity";
  }
  return "?";
}

// The matrices of one bone of one part entry, as the guest holds them.
struct BoneMatrices {
  bool item = false, root = false, world = false;
  tp::Mat4 a;       // A[b]
  tp::Mat4 a_root;  // A[0]
  tp::Mat4 b;       // B[b]
};

// Reads the bone's matrices of an entry (the item's array, its first matrix, the gWorld array). A source that is not readable stays false.
template <class Mem>
BoneMatrices ReadBoneMatrices(const Mem& mem, const cw::PartEntry& entry, uint32_t bone) {
  BoneMatrices out;
  if (bone >= cw::kMaxMatrixBones) {
    return out;
  }
  if (cw::Plausible(entry.matrices)) {
    out.item = cw::ReadMatrix(mem, entry.matrices + 64u * bone, &out.a);
    out.root = cw::ReadMatrix(mem, entry.matrices, &out.a_root);
  }
  if (cw::Plausible(entry.world_matrices)) {
    out.world = cw::ReadMatrix(mem, entry.world_matrices + 64u * bone, &out.b);
  }
  return out;
}

// The five hypotheses for one bone: `ok[i]` false where a source was missing.
struct PoseSet {
  std::array<bool, kPoseCount> ok{};
  std::array<tp::Mat4, kPoseCount> m;
};

// A with the first bone's translation taken off (what the palette holds: sub_823788C8).
inline tp::Mat4 PaletteOf(const tp::Mat4& a, const tp::Mat4& root) {
  tp::Mat4 p = a;
  for (int c = 0; c < 3; ++c) {
    p.m[3][c] -= root.m[3][c];
  }
  return p;
}

inline PoseSet ComposePoses(const BoneMatrices& b, const tp::Mat4* entity) {
  PoseSet s;
  if (b.world) {
    s.ok[size_t(Pose::kBoneWorld)] = true;
    s.m[size_t(Pose::kBoneWorld)] = b.b;
  }
  if (b.item) {
    s.ok[size_t(Pose::kItemWorld)] = true;
    s.m[size_t(Pose::kItemWorld)] = b.a;
  }
  if (b.item && b.root && b.world) {
    s.ok[size_t(Pose::kPaletteBone)] = true;
    s.m[size_t(Pose::kPaletteBone)] = tp::Mul(PaletteOf(b.a, b.a_root), b.b);
  }
  if (b.item && entity) {
    s.ok[size_t(Pose::kItemEntity)] = true;
    s.m[size_t(Pose::kItemEntity)] = tp::Mul(b.a, *entity);
  }
  if (b.item && b.root && entity) {
    s.ok[size_t(Pose::kPaletteEntity)] = true;
    s.m[size_t(Pose::kPaletteEntity)] = tp::Mul(PaletteOf(b.a, b.a_root), *entity);
  }
  return s;
}

// A placement is usable when it is finite, affine and a car part's size (a squash may scale a part a little).
inline bool UsablePlacement(const tp::Mat4& m) {
  if (!tp::Finite(m)) {
    return false;
  }
  for (int r = 0; r < 3; ++r) {
    const double length = std::sqrt(m.m[r][0] * m.m[r][0] + m.m[r][1] * m.m[r][1] + m.m[r][2] * m.m[r][2]);
    if (!(length > 0.1 && length < 10.0)) {
      return false;
    }
  }
  for (int c = 0; c < 3; ++c) {
    if (std::fabs(m.m[3][c]) > 1.0e6) {
      return false;
    }
  }
  return true;
}

// The error of a placement against a draw's model-to-world, in metres: the distance between their origins, and the largest difference of the 3x3 (a number
// of metres where the part is a metre across).
struct PlacementError {
  double translation = 0.0;
  double basis = 0.0;
  double Total() const { return translation + basis; }
};

inline PlacementError ComparePlacements(const tp::Mat4& walked, const tp::Mat4& drawn) {
  PlacementError e;
  e.translation = car_rt_memory::Distance(walked.m[3], drawn.m[3]);
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      e.basis = std::max(e.basis, std::fabs(walked.m[r][c] - drawn.m[r][c]));
    }
  }
  return e;
}

// THE PROOF in the scene's own space: the instance of a placement and the instance of a draw, both through the frame's view (PackInstanceTransform: the
// camera-relative top level, z flipped where the camera looks down -z), compared as the 3x4 matrices the top level takes: the origins' distance and the largest
// difference of the 3x3. False where either could not be made an instance.
inline bool ViewError(const tp::Mat4& walked, const tp::Mat4& drawn, const tp::Mat4& view, bool flip_z, PlacementError* out) {
  float a[3][4], b[3][4];
  if (!rr::PackInstanceTransform(walked, view, flip_z, a) || !rr::PackInstanceTransform(drawn, view, flip_z, b)) {
    return false;
  }
  double t = 0.0, basis = 0.0;
  for (int r = 0; r < 3; ++r) {
    const double d = double(a[r][3]) - double(b[r][3]);
    t += d * d;
    for (int c = 0; c < 3; ++c) {
      basis = std::max(basis, std::fabs(double(a[r][c]) - double(b[r][c])));
    }
  }
  out->translation = std::sqrt(t);
  out->basis = basis;
  return true;
}

// The calibration: which hypothesis places the geometries of the cars the camera draws where the draws put them. Every matched (geometry, draw) pair adds its
// error for each hypothesis; the one with the lowest mean error (translation plus basis) over at least kMinSamples pairs is the pose, if it is within kFitMetres.
class PoseCalibration {
 public:
  static constexpr uint32_t kMinSamples = 24;
  static constexpr double kFitMetres = 0.35;  // a mean error over this: no hypothesis fits (the default stays and the log says so)

  struct Tally {
    uint32_t n = 0;
    double sum = 0.0, basis_sum = 0.0, worst = 0.0;
    double Mean() const { return n != 0 ? (sum + basis_sum) / double(n) : 1.0e9; }
  };

  void Add(const PoseSet& walked, const tp::Mat4& drawn) {
    for (uint32_t i = 0; i < kPoseCount; ++i) {
      if (!walked.ok[i] || !UsablePlacement(walked.m[i])) {
        continue;
      }
      const PlacementError e = ComparePlacements(walked.m[i], drawn);
      Tally& t = tally_[i];
      ++t.n;
      t.sum += e.translation;
      t.basis_sum += e.basis;
      t.worst = std::max(t.worst, e.translation);
    }
  }
  // Decides from what was added. Returns true when a pose was chosen (also again: the choice follows the data until `Freeze`).
  bool Decide() {
    uint32_t best = kPoseCount;
    double best_mean = 1.0e18;
    for (uint32_t i = 0; i < kPoseCount; ++i) {
      if (tally_[i].n >= kMinSamples && tally_[i].Mean() < best_mean) {
        best = i;
        best_mean = tally_[i].Mean();
      }
    }
    if (best == kPoseCount) {
      return false;
    }
    fits_ = best_mean <= kFitMetres;
    chosen_ = Pose(best);
    decided_ = true;
    return true;
  }
  Pose chosen() const { return chosen_; }
  bool decided() const { return decided_; }
  bool fits() const { return decided_ && fits_; }
  const Tally& tally(Pose pose) const { return tally_[size_t(pose)]; }
  uint32_t samples() const {
    uint32_t n = 0;
    for (const Tally& t : tally_) {
      n = std::max(n, t.n);
    }
    return n;
  }
  // The window's tallies go; the choice stays until the next Decide (a new window decides again).
  void NewWindow() { tally_ = {}; }

  std::string Describe() const {
    std::string out;
    for (uint32_t i = 0; i < kPoseCount; ++i) {
      const Tally& t = tally_[i];
      out += std::format("{}{}: {}", out.empty() ? "" : "; ", PoseName(Pose(i)),
                         t.n == 0 ? std::string("no pair") : std::format("{} pairs, mean {:.3f} m + basis {:.3f}, worst {:.3f} m", t.n, t.sum / double(t.n), t.basis_sum / double(t.n), t.worst));
    }
    return out;
  }

 private:
  std::array<Tally, kPoseCount> tally_{};
  Pose chosen_ = Pose::kPaletteBone;
  bool decided_ = false;
  bool fits_ = false;
};

// The pose the walk uses before the calibration has decided: the shader's own recipe as the asm reads it.
inline constexpr Pose kDefaultPose = Pose::kPaletteBone;

// ---------------------------------------------------------------------------
// the walked car's parts and their placements
// ---------------------------------------------------------------------------

// One geometry of a walked car as the scene takes it.
struct Part {
  uint32_t geometry = 0;   // into CarWalk::geometries
  uint64_t key = 0;        // the mesh key (GeometryKey)
  uint32_t indices = 0;
  uint32_t triangles = 0;
  tp::Mat4 world;          // model to world (the placement of the chosen pose)
  uint16_t part = 0;       // the part slot (cw::kBodyPart for the body), for the log
};

// The parts a car brings: its in-use geometries with a vertex and an index buffer, rigid (no bone palette: a skinned geometry is blended per vertex), of
// `min_indices` or more, the largest first, at most `max_parts` of them.
inline std::vector<uint32_t> PickGeometries(const cw::CarWalk& car, uint32_t min_indices, uint32_t max_parts) {
  std::vector<uint32_t> picks;
  for (uint32_t g = 0; g < car.geometries.size(); ++g) {
    const cw::Geometry& geometry = car.geometries[g];
    if (!geometry.in_use || !geometry.read || !geometry.vb_ok || !geometry.ib_ok || geometry.palette_count != 0 || geometry.index_count < std::max<uint32_t>(min_indices, 3u) ||
        geometry.vertex_count == 0 || geometry.vb_stride < 12 || geometry.bone == cw::kNoBone) {
      continue;
    }
    picks.push_back(g);
  }
  std::stable_sort(picks.begin(), picks.end(), [&](uint32_t a, uint32_t b) { return car.geometries[a].index_count > car.geometries[b].index_count; });
  if (max_parts != 0 && picks.size() > max_parts) {
    picks.resize(max_parts);
  }
  return picks;
}

// The mesh key of a geometry: its buffers' addresses and sizes (the GPU headers' two words, the physical address), strides, counts and formats. The cars of one
// model name the same buffers, so they share the key. Marked as the walk's (a key of the draw path is another number all the same: a fingerprint of the bytes).
inline uint64_t GeometryKey(const cw::Geometry& g) {
  uint64_t h = 0xCBF29CE484222325ull ^ 0x57414C4B4D455348ull;  // "WALKMESH"
  const auto mix = [&](uint64_t v) {
    h ^= v;
    h *= 0x100000001B3ull;
    h ^= h >> 29;
  };
  mix(cw::PhysicalOf(g.vb_w6 & ~cw::kFetchTypeMask));
  mix(g.vb_w7 & cw::kFetchSizeMask);
  mix(g.vb_stride);
  mix(cw::PhysicalOf(g.ib_w6));
  mix(g.ib_w7);
  mix(g.ib_w0 & 0xE0000000u);
  mix(g.index_count);
  mix(g.vertex_count);
  mix(g.draw_mode);
  mix(uint32_t(g.decl.offset[0] + 1));
  return h != 0 ? h : 1ull;
}

// ---------------------------------------------------------------------------
// the mesh
// ---------------------------------------------------------------------------

enum class MeshProblem : uint8_t {
  kNone,
  kNoBuffers,     // a buffer's header or object was not readable
  kPosition,      // the declaration's position is not a float3 at offset 0 (or the stride is under 12)
  kTooBig,
  kUnreadable,    // a range is not committed guest memory
  kPrimitive,     // neither a list nor a strip by its counts
  kNoTriangles,   // nothing left
};

inline const char* MeshProblemName(MeshProblem p) {
  switch (p) {
    case MeshProblem::kNone: return "none";
    case MeshProblem::kNoBuffers: return "no buffers";
    case MeshProblem::kPosition: return "the position is not a float3 at offset 0";
    case MeshProblem::kTooBig: return "too big";
    case MeshProblem::kUnreadable: return "guest memory not readable";
    case MeshProblem::kPrimitive: return "neither a triangle list nor a strip";
    case MeshProblem::kNoTriangles: return "no triangles";
  }
  return "?";
}

struct MeshRead {
  uint32_t vertices = 0;
  uint32_t triangles = 0;       // before the scene's cleaning (degenerate and out-of-range ones are dropped there)
  uint64_t bytes = 0;           // guest bytes read (vertices and indices)
  bool strip = false;
  bool guessed = false;         // the counts said neither a list nor a strip but a multiple of three: read as a list
};

// Reads a geometry's triangles: positions x y z a vertex (the first `n` vertices of the buffer, n the larger of the geometry's and the buffer's count, within the
// fetch window's size) and indices as a triangle list. A strip is expanded (its winding alternates; degenerate triangles are the scene's cleaning's to drop).
template <class Mem>
MeshProblem ReadGeometryMesh(const Mem& mem, const cw::Geometry& g, std::vector<float>* positions, std::vector<uint32_t>* indices, MeshRead* info) {
  positions->clear();
  indices->clear();
  *info = MeshRead{};
  if (!g.read || !g.vb_ok || !g.ib_ok || g.index_count == 0) {
    return MeshProblem::kNoBuffers;
  }
  if (g.vb_stride < 12 || (g.decl.read && (!g.decl.PositionIsFloat3() || g.decl.offset[0] != 0))) {
    return MeshProblem::kPosition;
  }
  uint32_t vertices = std::max(g.vertex_count, g.vb_count);
  if (const uint32_t window = g.vb_w7 & cw::kFetchSizeMask; window != 0) {
    vertices = std::min(vertices, window / g.vb_stride);
  }
  const uint64_t vertex_bytes = uint64_t(vertices) * g.vb_stride;
  const uint64_t index_bytes = uint64_t(g.index_count) * g.IndexBytes();
  if (vertices == 0 || vertex_bytes > cw::kMaxBufferBytes || index_bytes > cw::kMaxBufferBytes || g.index_count > rr::kMaxMeshTriangles * 3u) {
    return MeshProblem::kTooBig;
  }
  const uint8_t* vertex_data = mem.Window(g.vb_data, uint32_t(vertex_bytes));
  const uint8_t* index_data = mem.Window(g.ib_data, uint32_t(index_bytes));
  if (!vertex_data || !index_data) {
    return MeshProblem::kUnreadable;
  }
  // list or strip: by the counts (the draw-mode field's number is the loader's, 3 for a list), whichever the geometry's own primitive count agrees with
  const uint32_t n = g.index_count;
  bool strip = false;
  if (g.draw_mode == cw::kPrimitiveTriangleList || (g.primitive_count != 0 && n == g.primitive_count * 3u)) {
    strip = false;
  } else if (g.primitive_count != 0 && n == g.primitive_count + 2u) {
    strip = true;
  } else if (n % 3u == 0) {
    info->guessed = true;
  } else {
    return MeshProblem::kPrimitive;
  }
  positions->resize(size_t(vertices) * 3);
  for (uint32_t v = 0; v < vertices; ++v) {
    cw::LoadFloats(vertex_data + size_t(v) * g.vb_stride, 3, positions->data() + size_t(v) * 3);
  }
  // the indices, swapped by the header's endian, in chunks
  std::vector<uint32_t> raw(n);
  {
    const uint32_t unit = g.IndexBytes();
    uint8_t chunk[4096];
    uint32_t done = 0;
    while (done < n) {
      const uint32_t take = std::min<uint32_t>(n - done, uint32_t(sizeof(chunk)) / unit);
      car_dump::SwapEndian(chunk, index_data + size_t(done) * unit, take * unit, g.IndexEndian());
      for (uint32_t i = 0; i < take; ++i) {
        if (unit == 4) {
          uint32_t v;
          std::memcpy(&v, chunk + size_t(i) * 4, 4);
          raw[done + i] = v;
        } else {
          uint16_t v;
          std::memcpy(&v, chunk + size_t(i) * 2, 2);
          raw[done + i] = v;
        }
      }
      done += take;
    }
  }
  if (!strip) {
    indices->assign(raw.begin(), raw.begin() + std::ptrdiff_t(n - n % 3u));
  } else {
    const uint32_t restart = g.Index32() ? 0xFFFFFFFFu : 0xFFFFu;
    uint32_t run = 0;
    for (uint32_t i = 0; i < n; ++i) {
      if (raw[i] == restart) {
        run = 0;
        continue;
      }
      if (run >= 2) {
        const uint32_t a = raw[i - 2], b = raw[i - 1], c = raw[i];
        if ((run & 1u) == 0) {
          indices->insert(indices->end(), {a, b, c});
        } else {
          indices->insert(indices->end(), {b, a, c});
        }
      }
      ++run;
    }
  }
  info->vertices = vertices;
  info->triangles = uint32_t(indices->size() / 3);
  info->bytes = vertex_bytes + index_bytes;
  info->strip = strip;
  return info->triangles == 0 ? MeshProblem::kNoTriangles : MeshProblem::kNone;
}

// ---------------------------------------------------------------------------
// which car a draw is of
// ---------------------------------------------------------------------------
// car_model_walk::MatchFrame says which geometry a draw is by its buffers, and the cars of one model name the SAME buffers: it gives a draw to the first car that owns
// them, and every car of the model looks drawn when one of them is. The walk needs the car: the one the draw belongs to. A draw has the stencil ID the game gave its car
// for this frame, and its own model-to-world, so: the draws of one ID are one car's; the car is, among the cars that own a geometry some draw of the ID matched, the one
// nearest to the mean of the draws' origins (the draws' origins are about the car's root, as car_rt_memory_rules.h's match takes them). A car none of whose IDs are
// assigned to it is not drawn, however many of its geometries a twin's draws hit.
struct DrawOwner {
  int32_t car = -1;       // into the cars given: the car the draw is of
  int32_t geometry = -1;  // ... and its geometry the draw's buffers are
};

struct Assignment {
  std::vector<DrawOwner> draws;  // by the draws given
  std::vector<uint8_t> drawn;    // by the cars given: some draw is of it
  uint32_t ids = 0;              // stencil IDs with a matched draw
  uint32_t ids_assigned = 0;     // ... that were given to a car
};

// How far the mean origin of an ID's draws may be from a car (metres) for the ID to be that car's. A car's parts' origins are within about 2 m of its root.
inline constexpr double kDrawnMatchMetres = 6.0;

inline Assignment AssignDraws(const std::vector<const cw::CarWalk*>& cars, const std::vector<std::array<double, 3>>& positions, const std::vector<cw::DrawKey>& draws) {
  Assignment out;
  out.draws.assign(draws.size(), DrawOwner{});
  out.drawn.assign(cars.size(), 0);
  struct Owner {
    uint32_t car, geometry;
  };
  std::unordered_map<uint64_t, std::vector<Owner>> windows;
  for (uint32_t c = 0; c < cars.size(); ++c) {
    for (uint32_t g = 0; g < cars[c]->geometries.size(); ++g) {
      const cw::Geometry& geometry = cars[c]->geometries[g];
      if (geometry.vb_ok) {
        windows[cw::detail::VertexWindowKey(geometry.vb_w6, geometry.vb_w7)].push_back({c, g});
      }
    }
  }
  // each draw's owners (the geometries its buffers are), by ID
  struct Hit {
    uint32_t draw;
    std::vector<Owner> owners;
  };
  std::unordered_map<uint32_t, std::vector<Hit>> by_id;
  for (uint32_t d = 0; d < draws.size(); ++d) {
    const cw::DrawKey& draw = draws[d];
    const auto found = windows.find(cw::detail::VertexWindowKey(draw.vertex_d0, draw.vertex_d1));
    if (found == windows.end()) {
      continue;
    }
    Hit hit;
    hit.draw = d;
    for (const Owner& owner : found->second) {
      const cw::Geometry& g = cars[owner.car]->geometries[owner.geometry];
      const bool index_ok = draw.indexed && g.ib_ok && ((draw.index_object != 0 && draw.index_object == g.ib_header) || cw::PhysicalOf(draw.index_address) == cw::PhysicalOf(g.ib_w6));
      if (index_ok) {
        hit.owners.push_back(owner);
      }
    }
    if (!hit.owners.empty()) {
      by_id[draw.id].push_back(std::move(hit));
    }
  }
  out.ids = uint32_t(by_id.size());
  for (const auto& [id, hits] : by_id) {
    // the mean origin of the ID's draws (all of them, matched or not: the car's other parts are drawn under the same ID)
    double sum[3] = {};
    uint32_t n = 0;
    for (const cw::DrawKey& draw : draws) {
      if (draw.id == id && draw.has_world) {
        for (int c = 0; c < 3; ++c) {
          sum[c] += draw.world.m[3][c];
        }
        ++n;
      }
    }
    // the cars that own a geometry of one of its draws
    std::vector<uint32_t> candidates;
    for (const Hit& hit : hits) {
      for (const Owner& owner : hit.owners) {
        if (std::find(candidates.begin(), candidates.end(), owner.car) == candidates.end()) {
          candidates.push_back(owner.car);
        }
      }
    }
    int32_t chosen = -1;
    if (candidates.size() == 1 && n == 0) {
      chosen = int32_t(candidates[0]);  // one car owns them and nothing says otherwise
    } else if (n != 0) {
      const double centre[3] = {sum[0] / n, sum[1] / n, sum[2] / n};
      double best = kDrawnMatchMetres;
      for (const uint32_t c : candidates) {
        const double d = car_rt_memory::Distance(centre, positions[c].data());
        if (d <= best) {
          best = d;
          chosen = int32_t(c);
        }
      }
    }
    if (chosen < 0) {
      continue;
    }
    ++out.ids_assigned;
    out.drawn[size_t(chosen)] = 1;
    for (const Hit& hit : hits) {
      for (const Owner& owner : hit.owners) {
        if (int32_t(owner.car) == chosen) {
          out.draws[hit.draw] = DrawOwner{chosen, int32_t(owner.geometry)};
          break;
        }
      }
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// the paint
// ---------------------------------------------------------------------------

struct PaintColour {
  bool known = false;
  float rgb[3] = {};
};

// The colour a car's paint shows in the reflection's hidden hits: region 0's colour A, the base of the paint's gradient (the vertex shader makes
// lerp(DL2, DL3, f) with DL2 = A and DL3 = B), and the mean of A and B where the car's gradient mode is not 0 (the paint then runs from A to B over the body).
// CHECK: the colours are the game's constants as they are (linear 0..1: the first check run's R34-like car reads (0.063, 0.349, 0.071) = a dark green); the
// shader shows them darkened by the reflection's own shade like any hidden part.
inline PaintColour PaintColourOf(const cw::Paint& region0) {
  PaintColour out;
  if (!region0.read) {
    return out;
  }
  float c[3];
  for (int k = 0; k < 3; ++k) {
    c[k] = region0.a[k];
    if (!std::isfinite(c[k])) {
      return out;
    }
  }
  if (region0.mode != 0) {
    for (int k = 0; k < 3; ++k) {
      if (!std::isfinite(region0.b[k])) {
        return out;
      }
      c[k] = 0.5f * (c[k] + region0.b[k]);
    }
  }
  for (int k = 0; k < 3; ++k) {
    out.rgb[k] = std::clamp(c[k], 0.0f, 1.0f);
  }
  out.known = true;
  return out;
}

// ---------------------------------------------------------------------------
// which cars, how much
// ---------------------------------------------------------------------------

// What the scene knows of a walked car in a frame (the walker fills it).
struct CarInfo {
  uint32_t vehicle = 0;
  bool is_player = false;
  bool drawn = false;            // some draw of the frame is one of its geometries
  bool visible = false;          // the game's own flag (+116): the main view shows it
  double position[3] = {};
  double distance = 0.0;         // metres from the player's vehicle
  PaintColour paint;
  uint32_t parts = 0;            // geometries it would bring
};

// The cars a frame stages (reads the triangles of): near the player (the radius and the class's count plus a spare), not the player's, and not shown by the
// main view this frame (the game's visibility flag): the draw path covers the ones it draws. Nearest first.
inline std::vector<uint32_t> ChooseStaged(const std::vector<CarInfo>& cars, const ci::Limits& limits) {
  std::vector<uint32_t> order;
  for (uint32_t i = 0; i < cars.size(); ++i) {
    const CarInfo& c = cars[i];
    if (!c.is_player && !c.visible && !c.drawn && c.parts != 0 && std::isfinite(c.distance) && c.distance <= limits.radius) {
      order.push_back(i);
    }
  }
  std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return cars[a].distance < cars[b].distance; });
  const size_t most = std::min<size_t>(size_t(limits.cars) + kStageSpareCars, kMaxWalkedCars);
  if (order.size() > most) {
    order.resize(most);
  }
  return order;
}

// The cars a frame places, nearest first: not the player's, not drawn (the draw path keeps the cars it draws), within the radius, at most the class's count.
// `remembered` are the cars the kits place this frame (their centres): in kGaps a walked car within kSameCarMetres of one is the same car and is left to its kit.
inline std::vector<uint32_t> ChoosePlaced(const std::vector<CarInfo>& cars, const ci::Limits& limits, Mode mode, const std::vector<ci::OtherCar>& remembered) {
  std::vector<uint32_t> order;
  if (mode == Mode::kOff || !limits.On()) {
    return order;
  }
  for (uint32_t i = 0; i < cars.size(); ++i) {
    const CarInfo& c = cars[i];
    if (c.is_player || c.drawn || c.parts == 0 || !(std::isfinite(c.distance) && c.distance <= limits.radius)) {
      continue;
    }
    if (mode == Mode::kGaps) {
      bool covered = false;
      for (const ci::OtherCar& r : remembered) {
        double centre[3];
        if (r.Centre(centre)) {
          centre[1] -= ci::kCentreRise;
          covered = covered || car_rt_memory::Distance(centre, c.position) <= kSameCarMetres;
        }
      }
      if (covered) {
        continue;
      }
    }
    order.push_back(i);
  }
  std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return cars[a].distance < cars[b].distance; });
  if (order.size() > std::min<size_t>(limits.cars, kMaxWalkedCars)) {
    order.resize(std::min<size_t>(limits.cars, kMaxWalkedCars));
  }
  return order;
}

// In kModelFirst the kits' cars that a placed walked car stands on are dropped: returns the indices into `remembered` to drop.
inline std::vector<uint32_t> RememberedCovered(const std::vector<ci::OtherCar>& remembered, const std::vector<CarInfo>& cars, const std::vector<uint32_t>& placed) {
  std::vector<uint32_t> drop;
  for (uint32_t k = 0; k < remembered.size(); ++k) {
    double centre[3];
    if (!remembered[k].Centre(centre)) {
      continue;
    }
    centre[1] -= ci::kCentreRise;
    for (const uint32_t i : placed) {
      if (car_rt_memory::Distance(centre, cars[i].position) <= kSameCarMetres) {
        drop.push_back(k);
        break;
      }
    }
  }
  return drop;
}

// The paint of a remembered car (the kit places the draws of a car that was seen once: its colour is not in the history now): the nearest walked car within
// kSameCarMetres, if its paint is known.
inline bool PaintForRemembered(const ci::OtherCar& car, const std::vector<CarInfo>& cars, float rgb[3]) {
  double centre[3];
  if (!car.Centre(centre)) {
    return false;
  }
  centre[1] -= ci::kCentreRise;
  double best = kSameCarMetres;
  bool found = false;
  for (const CarInfo& c : cars) {
    const double d = car_rt_memory::Distance(centre, c.position);
    if (c.paint.known && d <= best) {
      best = d;
      found = true;
      for (int k = 0; k < 3; ++k) {
        rgb[k] = c.paint.rgb[k];
      }
    }
  }
  return found;
}

// The paint of a car the camera draws (RT-MODEL-CARS-3). The game draws a car near the player whenever the sphere round it reaches the view, so a neighbour beside the door
// is DRAWN (its draws are in the scene as an ordinary other car, its +116 flag says visible) while the camera, standing beside the door, sees none of it: the history
// holds no colour of it and its hidden hits were a miss. The walk knows the paint of every car it walks, drawn or not: the walked car nearest to the drawn car's draws
// (their mean origin, as PaintForRemembered takes a kit's), within kSameCarMetres, and not the player's (a car beside the player is another car), if its paint is known.
inline bool PaintForDrawn(const ci::OtherCar& car, const std::vector<CarInfo>& cars, float rgb[3]) {
  double centre[3];
  if (!car.Centre(centre)) {
    return false;
  }
  centre[1] -= ci::kCentreRise;
  double best = kSameCarMetres;
  const CarInfo* found = nullptr;
  for (const CarInfo& c : cars) {
    if (c.is_player) {
      continue;
    }
    const double d = car_rt_memory::Distance(centre, c.position);
    if (d <= best) {  // the nearest walked car decides, whether or not its paint is known (a car with no paint must not borrow its neighbour's)
      best = d;
      found = &c;
    }
  }
  if (!found || !found->paint.known) {
    return false;
  }
  for (int k = 0; k < 3; ++k) {
    rgb[k] = found->paint.rgb[k];
  }
  return true;
}

// ---------------------------------------------------------------------------
// the budgets of a car that is new
// ---------------------------------------------------------------------------

// The per-frame budget of the walked cars' meshes, in the class's numbers (ci::Limits: triangles read and KiB of guest data read for a new car, 60,000 and 768 KiB
// on High, 30,000 and 384 KiB on Mid): the first mesh of a frame is always taken, so a mesh larger than the budget is not left out for ever.
struct StageBudget {
  uint64_t triangles = 0;
  uint64_t bytes = 0;
};

inline bool MayStage(const StageBudget& spent, uint32_t triangles, uint64_t bytes, const ci::Limits& limits) {
  const bool first = spent.triangles == 0 && spent.bytes == 0;
  if (first) {
    return true;
  }
  return spent.triangles + triangles <= limits.stage_triangles && (limits.content_bytes == 0 || spent.bytes + bytes <= limits.content_bytes);
}

// What a geometry will cost to read, before reading: its guest bytes (vertices and indices) and its triangles.
inline void CostOf(const cw::Geometry& g, uint32_t* triangles, uint64_t* bytes) {
  *triangles = g.index_count / 3;
  *bytes = uint64_t(std::max(g.vertex_count, g.vb_count)) * g.vb_stride + uint64_t(g.index_count) * g.IndexBytes();
}

// One staged mesh waiting to be built: its triangles and whether it is a walked car's.
struct BuildItem {
  uint32_t triangles = 0;
  bool walked = false;
};

// How many of the staged meshes (in order) a frame builds: the total budget as the scene's (rr::TakeWithinBudget: at least one), and the walked meshes
// within `walked_budget` triangles of their own, so that a new car of the walk never starves the player's car (the first walked mesh of a frame is always taken).
// Returns, for each item, whether it is built this frame; a walked mesh over its budget waits (the items after it that fit are still taken).
inline std::vector<uint8_t> PlanBuilds(const std::vector<BuildItem>& items, uint64_t total_budget, uint64_t walked_budget) {
  std::vector<uint8_t> take(items.size(), 0);
  uint64_t total = 0, walked = 0;
  bool any = false, any_walked = false;
  for (size_t i = 0; i < items.size(); ++i) {
    const BuildItem& it = items[i];
    if (any && total + it.triangles > total_budget) {
      break;
    }
    if (it.walked && any_walked && walked + it.triangles > walked_budget) {
      continue;
    }
    take[i] = 1;
    total += it.triangles;
    any = true;
    if (it.walked) {
      walked += it.triangles;
      any_walked = true;
    }
  }
  return take;
}

// ---------------------------------------------------------------------------
// the walked cars the scene holds, and the cleanup when one leaves
// ---------------------------------------------------------------------------

// Which meshes each walked car names, and when it last was placed: a car that has not been placed for kLeaveFrames gives its meshes back (the ones no other
// placed car names); a mesh of a car that is still near is kept by the scene's own use stamps.
class WalkedSet {
 public:
  // `placed`: the cars placed this frame with their keys.
  struct Placed {
    uint32_t vehicle = 0;
    std::vector<uint64_t> keys;
  };
  // Returns the keys to give back this frame.
  std::vector<uint64_t> Update(uint64_t frame, const std::vector<Placed>& placed) {
    for (const Placed& p : placed) {
      Entry& e = cars_[p.vehicle];
      e.last = frame;
      e.keys = p.keys;
    }
    std::vector<uint64_t> give;
    std::unordered_set<uint64_t> in_use;
    for (const auto& [vehicle, e] : cars_) {
      if (frame - e.last <= kLeaveFrames) {
        in_use.insert(e.keys.begin(), e.keys.end());
      }
    }
    for (auto it = cars_.begin(); it != cars_.end();) {
      if (frame - it->second.last > kLeaveFrames) {
        for (const uint64_t k : it->second.keys) {
          if (!in_use.contains(k)) {
            give.push_back(k);
          }
        }
        it = cars_.erase(it);
      } else {
        ++it;
      }
    }
    std::sort(give.begin(), give.end());
    give.erase(std::unique(give.begin(), give.end()), give.end());
    return give;
  }
  size_t cars() const { return cars_.size(); }
  void Clear() { cars_.clear(); }

 private:
  struct Entry {
    uint64_t last = 0;
    std::vector<uint64_t> keys;
  };
  std::unordered_map<uint32_t, Entry> cars_;
};

// Whether a vehicle's cached walk is still good (the structure only: the matrices are read every frame): the same model set, the same lods, the same variants
// signature, and not older than kRewalkFrames.
struct WalkSignature {
  uint32_t model_set = 0;
  int32_t body_lod = -2, part_lod = -2;
  uint64_t variants = 0;       // a hash of the part variants
  uint32_t flags = 0;          // the draw flags as bits
  int32_t min_lod = -1;        // the lod floor the walk was made with
  uint64_t walked_frame = 0;
  bool operator==(const WalkSignature& o) const {
    return model_set == o.model_set && body_lod == o.body_lod && part_lod == o.part_lod && variants == o.variants && flags == o.flags && min_lod == o.min_lod;
  }
};

inline bool WalkStillGood(const WalkSignature& cached, const WalkSignature& now, uint64_t frame) {
  return cached == now && cached.model_set != 0 && frame >= cached.walked_frame && frame - cached.walked_frame < kRewalkFrames;
}

// ---------------------------------------------------------------------------
// the words of the logs
// ---------------------------------------------------------------------------

inline std::string Describe(const PaintColour& p) {
  return p.known ? std::format("({:.3f}, {:.3f}, {:.3f})", p.rgb[0], p.rgb[1], p.rgb[2]) : std::string("not known");
}

}  // namespace mcla::gpu::car_model_rt
