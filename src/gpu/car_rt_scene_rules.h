#pragma once
// ===========================================================================
// MCLA GPU renderer -- the player's car as ray tracing structures: the rules that need no renderer
// ===========================================================================
// car_rt_scene.h builds one bottom level structure per mesh of the player's car and, every frame, one top level structure with one
// instance per draw. This header holds what needs no device, as pure functions on plain numbers, so that
// car_rt_scene_rules_test.cpp builds on its own:
//
//   - the instance transform: a draw's model-to-world matrix (row vectors, as the game's) and the frame's camera into the 3x4 matrix
//     (column vectors) an instance of the top level takes, in the frame's VIEW space (x right, y up, z forward = the AO's linear depth t),
//     and the decision whether the camera matrix the frame carries has z forward already or backward as the game's own view matrix has it
//   - the instance masks and the row of the per-instance table the shaders read
//   - the clean-up of a mesh read from guest memory (triangles that name no vertex, positions that are not numbers, normals that are no
//     unit vectors, smooth normals made from the triangles where the declaration had none)
//   - a range allocator with coalescing (the shared position, normal, index and acceleration structure buffers)
//   - the eviction policy of the meshes (not seen for a few hundred frames, or the memory cap) and the per-frame build budget
//   - the check pass's statistics (the words the shader counts into, as numbers)
//
// This header includes nothing of the renderer.
// ===========================================================================

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <map>
#include <vector>

#include "car_dump_rules.h"
#include "temporal_math.h"

namespace mcla::gpu::car_rt_rules {

// ---------------------------------------------------------------------------
// limits
// ---------------------------------------------------------------------------

// Instances of the top level and rows of the table: the player's car is about 195 rigid draws (perf/CAR-RT-SPIKE.md section 10).
inline constexpr uint32_t kMaxInstances = 1024;
// A mesh beyond these is not built (one draw of a car has a few thousand triangles).
inline constexpr uint32_t kMaxMeshVertices = 1u << 20;
inline constexpr uint32_t kMaxMeshTriangles = 400000;
// The memory the structures, the shared buffers and their growth may hold in all (bytes).
inline constexpr uint64_t kMemoryCapBytes = 64ull << 20;
// A mesh that no draw of the frame names for this many frames is given back; one seen in the last kProtectFrames is never taken for the
// cap's sake (it is in the frame being recorded or the one before).
inline constexpr uint64_t kEvictAfterFrames = 300;
inline constexpr uint64_t kProtectFrames = 2;
// A mesh whose read or build failed is tried again after this many frames.
inline constexpr uint64_t kRetryFailedFrames = 600;
// Triangles read from guest memory in one frame (the listener runs inside a draw's call), and triangles whose bottom level structure is
// built in one frame at the trigger. A car is about 91,000; the rest waits for the next frame, where the draws that need it are left out.
inline constexpr uint32_t kStageTrianglesPerFrame = 150000;
inline constexpr uint32_t kBuildTrianglesPerFrame = 150000;
// Acceleration structure placement (D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT, and Vulkan's scratch alignment).
inline constexpr uint64_t kStructureAlignment = 256;

// ---------------------------------------------------------------------------
// masks and the per-instance table
// ---------------------------------------------------------------------------
// hlsl/car_rt_common.hlsli CAR_RT_MASK_*: the instance mask a ray's own mask is tested against.
inline constexpr uint32_t kMaskBody = 0x01;    // the opaque rigid parts
inline constexpr uint32_t kMaskGlass = 0x02;   // drawn after the world: glass, lights and other translucent parts
inline constexpr uint32_t kMaskTyres = 0x04;
inline constexpr uint32_t kMaskDriver = 0x08;  // skinned: not in the first version
// Another car (an opponent, traffic, a parked car; car_rt_instances_rules.h): hit by the effects' rays against the scene, never by the
// primary ray that finds the player's car at a pixel.
inline constexpr uint32_t kMaskOthers = 0x10;
// Another car's CHROME (the tailpipes, the badges and the trim, drawn by xChrome::PS_Chrome: car_rt_instances_rules.h OtherMask). An instance of such a draw
// carries this bit AND kMaskOthers, so every ray that hits another car still hits it; a ray whose mask is this bit alone finds another car's chrome and
// nothing else. The reflection pass's primary ray takes it (with the player's body) to find another car's chrome at a pixel (car_chrome_rt_rules.h).
inline constexpr uint32_t kMaskOtherChrome = 0x20;
// The table's flags word (z): the mask in the low 8 bits, then these (CAR_RT_FLAG_*).
inline constexpr uint32_t kFlagAfterWorld = 0x100;

// The tyres' vertex shader (xWheelRubber VS_ParallaxRubber2: no palette, gWorld alone; car_dump_rules.h kKnownWorldOnlyVs).
inline constexpr uint64_t kTyreVs = car_dump::kKnownWorldOnlyVs[0];
static_assert(kTyreVs == 0xEDDAF3E3986AE099ull);

// Whether "drawn after the frame's first world draw" tells glass from the rest in this frame. Where every draw of the car was drawn after
// the first world draw (the car dump's frame 13460: the whole car came after the world in that view) or none was, the rule says nothing
// and every draw that is no tyre is a body.
inline bool GlassRuleInformative(size_t draws_after_world, size_t draws) {
  return draws_after_world > 0 && draws_after_world < draws;
}

inline uint32_t InstanceMask(bool tyre, bool after_world, bool glass_rule_informative) {
  if (tyre) {
    return kMaskTyres;
  }
  return (after_world && glass_rule_informative) ? kMaskGlass : kMaskBody;
}

// One row of the table the shaders read (StructuredBuffer<uint4>, hlsl/car_rt_common.hlsli CarRtInstance).
struct InstanceRow {
  uint32_t first_index = 0;   // the mesh's first index in the shared index buffer (uint32 units)
  uint32_t first_vertex = 0;  // the mesh's first vertex in the shared normal buffer (vertices, 12 bytes each)
  uint32_t flags = 0;         // the mask, kFlagAfterWorld
  uint32_t draw = 0;          // the draw's place in car_frame::Frame::draws
};

inline InstanceRow MakeRow(uint32_t first_index, uint32_t first_vertex, uint32_t mask, bool after_world, uint32_t draw) {
  InstanceRow row;
  row.first_index = first_index;
  row.first_vertex = first_vertex;
  row.flags = (mask & 0xFFu) | (after_world ? kFlagAfterWorld : 0u);
  row.draw = draw;
  return row;
}

// ---------------------------------------------------------------------------
// the instance transform
// ---------------------------------------------------------------------------

// Which side of the camera the player's car is on in the matrix `view` (row vectors, world to view): +1 when it is in front along +z (the
// AO's view space: z forward), -1 when it is along -z (the game's own view matrix: the camera looks down -z, c14 is BACKWARD), 0 when it
// cannot be told (the car is within half a metre of the camera's plane). The player's car is in front of the camera in the game's view, the
// pause menu's and the garage's. The determinant says nothing: it only tells the world's handedness against the view's.
inline int ViewZSign(const temporal::Mat4& view, const double world_position[3]) {
  const double z = world_position[0] * view.m[0][2] + world_position[1] * view.m[1][2] + world_position[2] * view.m[2][2] + view.m[3][2];
  if (!std::isfinite(z) || std::fabs(z) < 0.5) {
    return 0;
  }
  return z > 0.0 ? 1 : -1;
}

inline double Determinant3(const temporal::Mat4& m) {
  return m.m[0][0] * (m.m[1][1] * m.m[2][2] - m.m[1][2] * m.m[2][1]) - m.m[0][1] * (m.m[1][0] * m.m[2][2] - m.m[1][2] * m.m[2][0]) +
         m.m[0][2] * (m.m[1][0] * m.m[2][1] - m.m[1][1] * m.m[2][0]);
}

// The 3x4 matrix (column vectors, as DXR and Vulkan take it: out[i][j] is the weight of input j in output i, j = 3 the translation) that
// takes a point of a mesh's model space into the view space: (p, 1) * model_to_world * view, with the view's z negated where it looks down
// -z (`flip_z`). Both are affine row-vector matrices. False (nothing written) when the result is not finite, is not affine (a perspective
// column), or has no volume.
//
// The matrix is rigid with the scale the game's own bone matrices carry (the squash and stretch of the car body may scale a part): the
// shaders' normal is made with the cofactor matrix (hlsl/car_rt_common.hlsli CarRtHitNormal), which is right for any invertible matrix.
inline bool PackInstanceTransform(const temporal::Mat4& model_to_world, const temporal::Mat4& view, bool flip_z, float out[3][4]) {
  temporal::Mat4 t = temporal::Mul(model_to_world, view);
  if (flip_z) {
    for (int r = 0; r < 4; ++r) {
      t.m[r][2] = -t.m[r][2];
    }
  }
  if (!temporal::Finite(t)) {
    return false;
  }
  constexpr double kAffine = 1e-4;
  if (std::fabs(t.m[0][3]) > kAffine || std::fabs(t.m[1][3]) > kAffine || std::fabs(t.m[2][3]) > kAffine ||
      std::fabs(t.m[3][3] - 1.0) > kAffine) {
    return false;
  }
  if (!(std::fabs(Determinant3(t)) > 1e-9)) {
    return false;
  }
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 4; ++j) {
      out[i][j] = float(t.m[j][i]);
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// a mesh read from guest memory
// ---------------------------------------------------------------------------

// Area-weighted smooth normals of a triangle list: the sum of the face normals (their length is twice the area) of the triangles a vertex
// belongs to, made unit length. A vertex no triangle names gets (0, 0, 1). The winding is the mesh's own; the shaders turn the normal to
// face the ray, so its sign does not matter.
inline void SmoothNormals(const std::vector<float>& positions, const std::vector<uint32_t>& indices, std::vector<float>* normals) {
  const size_t vertices = positions.size() / 3;
  std::vector<double> sum(vertices * 3, 0.0);
  for (size_t t = 0; t + 3 <= indices.size(); t += 3) {
    const uint32_t a = indices[t], b = indices[t + 1], c = indices[t + 2];
    if (a >= vertices || b >= vertices || c >= vertices) {
      continue;
    }
    double e1[3], e2[3];
    for (int k = 0; k < 3; ++k) {
      e1[k] = double(positions[size_t(b) * 3 + k]) - double(positions[size_t(a) * 3 + k]);
      e2[k] = double(positions[size_t(c) * 3 + k]) - double(positions[size_t(a) * 3 + k]);
    }
    const double n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
    for (int k = 0; k < 3; ++k) {
      sum[size_t(a) * 3 + k] += n[k];
      sum[size_t(b) * 3 + k] += n[k];
      sum[size_t(c) * 3 + k] += n[k];
    }
  }
  normals->assign(vertices * 3, 0.0f);
  for (size_t v = 0; v < vertices; ++v) {
    const double x = sum[v * 3], y = sum[v * 3 + 1], z = sum[v * 3 + 2];
    const double length = std::sqrt(x * x + y * y + z * z);
    if (length > 1e-20 && std::isfinite(length)) {
      (*normals)[v * 3] = float(x / length);
      (*normals)[v * 3 + 1] = float(y / length);
      (*normals)[v * 3 + 2] = float(z / length);
    } else {
      (*normals)[v * 3 + 2] = 1.0f;
    }
  }
}

struct MeshClean {
  uint32_t triangles_in = 0;
  uint32_t triangles_dropped = 0;  // naming a vertex that is not there, a repeated vertex, or a position that is not a number
  bool normals_made = false;       // none came with the mesh (or the wrong number): smooth normals from the triangles
  uint32_t normals_replaced = 0;   // given normals that were no numbers or had no length, replaced by the smooth normal
};

// Makes a mesh fit for the structures: `positions` (x y z a vertex) keeps its vertices, `indices` keeps the whole triangles that name
// vertices of it, all different and with finite positions, `normals` ends with one unit vector per vertex.
inline MeshClean CleanMesh(std::vector<float>* positions, std::vector<uint32_t>* indices, std::vector<float>* normals) {
  MeshClean report;
  const size_t vertices = positions->size() / 3;
  positions->resize(vertices * 3);
  std::vector<uint8_t> finite(vertices, 1);
  for (size_t v = 0; v < vertices; ++v) {
    finite[v] = std::isfinite((*positions)[v * 3]) && std::isfinite((*positions)[v * 3 + 1]) && std::isfinite((*positions)[v * 3 + 2]);
  }
  const size_t triangles = indices->size() / 3;
  report.triangles_in = uint32_t(triangles);
  size_t kept = 0;
  for (size_t t = 0; t < triangles; ++t) {
    const uint32_t a = (*indices)[t * 3], b = (*indices)[t * 3 + 1], c = (*indices)[t * 3 + 2];
    if (a >= vertices || b >= vertices || c >= vertices || a == b || b == c || a == c || !finite[a] || !finite[b] || !finite[c]) {
      continue;
    }
    (*indices)[kept * 3] = a;
    (*indices)[kept * 3 + 1] = b;
    (*indices)[kept * 3 + 2] = c;
    ++kept;
  }
  report.triangles_dropped = uint32_t(triangles - kept);
  indices->resize(kept * 3);
  std::vector<float> smooth;
  if (normals->size() != vertices * 3) {
    SmoothNormals(*positions, *indices, normals);
    report.normals_made = true;
    return report;
  }
  for (size_t v = 0; v < vertices; ++v) {
    float* n = normals->data() + v * 3;
    const double length = std::sqrt(double(n[0]) * n[0] + double(n[1]) * n[1] + double(n[2]) * n[2]);
    if (length > 1e-6 && std::isfinite(length)) {
      n[0] = float(n[0] / length);
      n[1] = float(n[1] / length);
      n[2] = float(n[2] / length);
      continue;
    }
    if (smooth.empty()) {
      SmoothNormals(*positions, *indices, &smooth);
    }
    n[0] = smooth[v * 3];
    n[1] = smooth[v * 3 + 1];
    n[2] = smooth[v * 3 + 2];
    ++report.normals_replaced;
  }
  return report;
}

// ---------------------------------------------------------------------------
// the shared buffers
// ---------------------------------------------------------------------------

// First fit over a set of free ranges with coalescing. The unit is the caller's (bytes, vertices, indices).
class RangeAllocator {
 public:
  void Reset(uint64_t capacity) {
    free_.clear();
    capacity_ = capacity;
    used_ = 0;
    if (capacity != 0) {
      free_[0] = capacity;
    }
  }
  // The capacity grows to `capacity`; what is added is free.
  void Grow(uint64_t capacity) {
    if (capacity <= capacity_) {
      return;
    }
    Insert(capacity_, capacity - capacity_);
    capacity_ = capacity;
  }
  bool Alloc(uint64_t size, uint64_t alignment, uint64_t* offset) {
    if (size == 0) {
      return false;
    }
    const uint64_t align = std::max<uint64_t>(alignment, 1);
    for (auto it = free_.begin(); it != free_.end(); ++it) {
      const uint64_t begin = it->first, length = it->second;
      const uint64_t start = (begin + align - 1) / align * align;
      if (start < begin || start + size > begin + length) {
        continue;
      }
      free_.erase(it);
      if (start > begin) {
        free_[begin] = start - begin;
      }
      if (start + size < begin + length) {
        free_[start + size] = begin + length - (start + size);
      }
      used_ += size;
      *offset = start;
      return true;
    }
    return false;
  }
  void Free(uint64_t offset, uint64_t size) {
    if (size == 0 || offset + size > capacity_) {
      return;
    }
    Insert(offset, size);
    used_ -= std::min(used_, size);
  }
  uint64_t capacity() const { return capacity_; }
  uint64_t used() const { return used_; }
  uint64_t largest_free() const {
    uint64_t best = 0;
    for (const auto& [offset, length] : free_) {
      best = std::max(best, length);
    }
    return best;
  }
  // The end of the last range in use: everything beyond it is free (what a growth has to copy).
  uint64_t high_water() const {
    if (!free_.empty()) {
      const auto last = std::prev(free_.end());
      if (last->first + last->second == capacity_) {
        return last->first;
      }
    }
    return capacity_;
  }
  size_t free_ranges() const { return free_.size(); }

 private:
  void Insert(uint64_t offset, uint64_t size) {
    auto next = free_.lower_bound(offset);
    if (next != free_.begin()) {
      auto previous = std::prev(next);
      if (previous->first + previous->second == offset) {
        offset = previous->first;
        size += previous->second;
        free_.erase(previous);
      }
    }
    if (next != free_.end() && offset + size == next->first) {
      size += next->second;
      free_.erase(next);
    }
    free_[offset] = size;
  }

  std::map<uint64_t, uint64_t> free_;  // offset -> length
  uint64_t capacity_ = 0;
  uint64_t used_ = 0;
};

// The capacity a buffer grows to when `needed` does not fit: double, at least `needed` rounded up to `step`, never over `limit`.
inline uint64_t GrownCapacity(uint64_t capacity, uint64_t needed, uint64_t step, uint64_t limit) {
  const uint64_t wanted = std::max(capacity * 2, (needed + step - 1) / step * step);
  return std::min(std::max(wanted, needed), std::max(limit, needed));
}

// ---------------------------------------------------------------------------
// eviction
// ---------------------------------------------------------------------------

struct MeshUse {
  uint64_t key = 0;
  uint64_t bytes = 0;      // what it holds in all: positions, normals, indices, its structure
  uint64_t last_seen = 0;  // the last frame a draw of the car named it
};

struct EvictionRequest {
  uint64_t frame = 0;
  uint64_t cap_bytes = kMemoryCapBytes;
  uint64_t need_bytes = 0;  // what the meshes about to be added will hold
  uint64_t unseen_frames = kEvictAfterFrames;
  uint64_t protect_frames = kProtectFrames;
};

struct EvictionPlan {
  std::vector<uint64_t> keys;
  bool fits = true;  // after the evictions the needed bytes are inside the cap
};

// The meshes to give back: first every one not seen for `unseen_frames`, then, while the resident bytes and the needed ones are over the
// cap, the least recently seen among those not seen in the last `protect_frames` (the larger first at a tie).
inline EvictionPlan PlanEviction(const std::vector<MeshUse>& meshes, const EvictionRequest& request) {
  EvictionPlan plan;
  uint64_t resident = 0;
  for (const MeshUse& m : meshes) {
    resident += m.bytes;
  }
  const auto age = [&](const MeshUse& m) { return request.frame > m.last_seen ? request.frame - m.last_seen : 0; };
  std::vector<const MeshUse*> rest;
  for (const MeshUse& m : meshes) {
    if (age(m) > request.unseen_frames) {
      plan.keys.push_back(m.key);
      resident -= m.bytes;
    } else {
      rest.push_back(&m);
    }
  }
  if (resident + request.need_bytes > request.cap_bytes) {
    std::vector<const MeshUse*> candidates;
    for (const MeshUse* m : rest) {
      if (age(*m) > request.protect_frames) {
        candidates.push_back(m);
      }
    }
    std::sort(candidates.begin(), candidates.end(), [](const MeshUse* a, const MeshUse* b) {
      return a->last_seen != b->last_seen ? a->last_seen < b->last_seen : a->bytes > b->bytes;
    });
    for (const MeshUse* m : candidates) {
      if (resident + request.need_bytes <= request.cap_bytes) {
        break;
      }
      plan.keys.push_back(m->key);
      resident -= m->bytes;
    }
  }
  plan.fits = resident + request.need_bytes <= request.cap_bytes;
  return plan;
}

// How many of the pending meshes (their triangle counts, in order) are taken in one frame: until the budget is reached, at least one.
inline size_t TakeWithinBudget(const std::vector<uint32_t>& triangles, uint64_t budget) {
  uint64_t sum = 0;
  size_t taken = 0;
  for (uint32_t t : triangles) {
    if (taken != 0 && sum + t > budget) {
      break;
    }
    sum += t;
    ++taken;
  }
  return taken;
}

// Whether the totals changed enough from the last time they were logged to say them again: the first time, then a change of a quarter
// or of a mebibyte, whichever is larger.
inline bool TotalsChangedMuch(uint64_t last_logged, uint64_t now) {
  if (last_logged == ~uint64_t(0)) {
    return true;
  }
  const uint64_t difference = now > last_logged ? now - last_logged : last_logged - now;
  return difference >= std::max<uint64_t>(1ull << 20, last_logged / 4);
}

// ---------------------------------------------------------------------------
// the check pass's statistics (hlsl/car_rt_check_cs.hlsl)
// ---------------------------------------------------------------------------
// The shader counts into 20 words (and then holds the PROBE's, below): for each vehicle pixel of the AO's linear depth image that the scene is meant to hold (see the scope
// below) a ray from the view-space origin through the pixel is traced and its committed hit distance (the view-space z of the hit, as the ray's direction
// has z = 1) is compared with the pixel's own distance. A vehicle pixel the scene is not meant to hold is only counted (kStatOutside).
inline constexpr uint32_t kStatProbe = 20;     // the first word of the probe's
inline constexpr uint32_t kProbeCars = 4;      // the first cars' records the probe reads back
inline constexpr uint32_t kProbeWordsPerCar = 8;  // a car's two rows (CarRow, CarPaintRow) as the shader reads them: centre xyz, radius, paint rgb, flags, as bits
inline constexpr uint32_t kStatsWords = kStatProbe + kProbeCars * kProbeWordsPerCar;  // 52: a multiple of four: the counter buffer is a multiple of 16 bytes
inline constexpr uint32_t kStatVehicle = 0;    // vehicle pixels of the image the scene is meant to hold (the scope below)
inline constexpr uint32_t kStatCovered = 1;    // ... whose ray hit a part of the car
inline constexpr uint32_t kStatErrorSum = 2;   // the sum of |hit - image| / image over the covered, in units of kErrorUnit
inline constexpr uint32_t kStatNearer = 3;     // covered pixels whose hit is nearer than the image's by more than kNearFarTolerance
inline constexpr uint32_t kStatFarther = 4;    // ... farther
inline constexpr uint32_t kStatHistogram = 5;  // 8 words: the covered pixels by |hit - image| / image, below each of kBucketEdges
inline constexpr uint32_t kBuckets = 8;
inline constexpr uint32_t kStatGlass = 13;     // hits on an instance of kMaskGlass
inline constexpr uint32_t kStatTyres = 14;     // ... of kMaskTyres
inline constexpr uint32_t kStatBody = 15;      // ... of kMaskBody
inline constexpr uint32_t kStatOutside = 16;   // vehicle pixels the scene is NOT meant to hold: cars outside the frame's selection (not in the scene by design)
inline constexpr uint32_t kStatOthers = 17;    // of kStatVehicle's: the pixels in another kept car's box (the rest are in the player's box)
// words 18 and 19 are not used
static_assert(kStatHistogram + kBuckets == kStatGlass);
static_assert(kStatOthers < kStatProbe);
static_assert(kStatsWords % 4 == 0);
inline constexpr double kBucketEdges[kBuckets] = {0.0025, 0.005, 0.01, 0.02, 0.05, 0.10, 0.25, 1e30};
inline constexpr double kErrorUnit = 1000.0;
inline constexpr double kNearFarTolerance = 0.02;

// THE SCOPE: which vehicle pixels the scene is meant to hold. The scene keeps the player's car and the nearest few other cars of the frame (by the
// class: High up to 4 within 14 m, Mid up to 2 within 10 m; none with the cars 0 switch), and leaves the driver out; a vehicle pixel of any other car
// is not a miss but not in the scene by design. The depth image says only "vehicle" (one bit), not which car, so the shader places the pixel's
// view-space position (the ray's direction times the image's distance) in the boxes of the kept cars: the player's car's box (the transformed mesh
// boxes of its listed instances, grown on the host) and each kept other car's box (the same, from the top level's table, grown in the shader), both
// by kScopeBoxMargin. A pixel outside every box is of a car outside the selection. A box is an axis aligned box in view space, so a pixel of an
// unkept car right beside a kept one can fall in the kept car's box (it is then counted, and may show as a miss: the check errs on the strict side).
inline constexpr float kScopeBoxMargin = 0.15f;  // what every car's box is grown by (the player's on the host, the other cars' in the shader)
inline constexpr uint32_t kScopeMaxOtherBoxes = 8;  // car_rt_instances_rules.h kMaxCars
inline constexpr uint32_t kScopeOutside = 0;
inline constexpr uint32_t kScopePlayer = 1;
inline constexpr uint32_t kScopeOther = 2;
// bits of the check's scope flags (the push block's)
inline constexpr uint32_t kScopeFlagPlayerBox = 1u << 0;  // the player's car's box is known

struct ScopeBox {
  float lo[3] = {0, 0, 0};
  float hi[3] = {0, 0, 0};
};

// The host's mirror of the shader's ScopeOf: `p` is the pixel's view-space position; `player` is the player's box as the host pushes it (grown by the
// margin already), null when no mesh of the car had a box, so that a pixel in no other car's box counts as the player's; the other cars' boxes are as the
// scene has them (the margin is added here, as the shader adds it).
inline uint32_t ScopeOf(const float p[3], const ScopeBox* player, const ScopeBox* others, uint32_t other_count) {
  const auto inside = [&](const ScopeBox& b, float margin) {
    for (int c = 0; c < 3; ++c) {
      if (!(p[c] >= b.lo[c] - margin && p[c] <= b.hi[c] + margin)) {
        return false;
      }
    }
    return true;
  };
  if (player && inside(*player, 0.0f)) {
    return kScopePlayer;
  }
  for (uint32_t i = 0; i < other_count && i < kScopeMaxOtherBoxes; ++i) {
    if (inside(others[i], kScopeBoxMargin)) {
      return kScopeOther;
    }
  }
  return player ? kScopeOutside : kScopePlayer;
}

struct CheckSummary {
  uint64_t vehicle = 0;    // the vehicle pixels the scene is meant to hold: the player's car and the other cars the frame kept
  uint64_t outside = 0;    // more vehicle pixels of cars outside the selection (the class's radius and count, the cars 0 switch): not in the scene by design
  uint64_t others = 0;     // of `vehicle`: in another kept car's box
  uint64_t covered = 0;
  uint64_t missed = 0;     // vehicle pixels no part of a car in the scene covers: the driver, anything not built yet
  double coverage = 0.0;   // covered / vehicle
  double mean_error = 0.0;  // mean |hit - image| / image over the covered
  double p50 = 0.0;        // the upper edge of the histogram's bucket holding the median and the 95th percentile (1e30: the last)
  double p95 = 0.0;
  double within_1pct = 0.0;  // the share of covered pixels within 1%
  double nearer = 0.0;       // ... with a hit nearer than the image by more than 2%
  double farther = 0.0;
  uint64_t glass = 0, tyres = 0, body = 0;
  uint64_t other = 0;      // covered pixels none of the three classes counts: the other cars' parts (and the driver, where it is built)
};

inline CheckSummary DecodeCheckStats(const uint32_t words[kStatsWords]) {
  CheckSummary s;
  s.vehicle = words[kStatVehicle];
  s.outside = words[kStatOutside];
  s.others = std::min(words[kStatOthers], words[kStatVehicle]);
  s.covered = std::min(words[kStatCovered], words[kStatVehicle]);
  s.missed = s.vehicle - s.covered;
  s.coverage = s.vehicle ? double(s.covered) / double(s.vehicle) : 0.0;
  s.glass = words[kStatGlass];
  s.tyres = words[kStatTyres];
  s.body = words[kStatBody];
  const uint64_t classed = s.glass + s.tyres + s.body;
  s.other = s.covered > classed ? s.covered - classed : 0;
  if (s.covered == 0) {
    return s;
  }
  const double covered = double(s.covered);
  s.mean_error = double(words[kStatErrorSum]) / kErrorUnit / covered;
  uint64_t seen = 0;
  bool have50 = false, have95 = false;
  for (uint32_t b = 0; b < kBuckets; ++b) {
    seen += words[kStatHistogram + b];
    if (!have50 && double(seen) >= 0.5 * covered) {
      s.p50 = kBucketEdges[b];
      have50 = true;
    }
    if (!have95 && double(seen) >= 0.95 * covered) {
      s.p95 = kBucketEdges[b];
      have95 = true;
    }
  }
  s.within_1pct = double(uint64_t(words[kStatHistogram]) + words[kStatHistogram + 1] + words[kStatHistogram + 2]) / covered;
  s.nearer = double(words[kStatNearer]) / covered;
  s.farther = double(words[kStatFarther]) / covered;
  return s;
}


// ---------------------------------------------------------------------------
// the probe: the paint as the shader reads it (RT-MODEL-CARS-4)
// ---------------------------------------------------------------------------
// The first check showed the neighbour beside the door as a dark silhouette in no colour. The CPU wrote the paint into the table (car_rt_instances_rules.h CarPaintRow), the shader
// reads it with CarRtLoadCar (hlsl/car_rt_common.hlsli): this proves the second half. The check pass's thread (0, 0) copies the first kProbeCars cars' two rows, as the shader's
// own loads see them, into the words from kStatProbe (the push block says where the records start and how many there are), and the host's readback decodes them and compares each
// with the record it wrote for that frame (car_rt_instances_rules.h CarRecordLog): the same bits or not.
struct ProbeCar {
  bool read = false;     // the slot is one of the frame's records
  float centre[3] = {};  // CarRow as the shader read it
  float radius = 0.0f;
  float paint[3] = {};   // CarPaintRow
  uint32_t flags = 0;    // bit 0 the colour is known (car_rt_instances_rules.h kPaintKnown)
};

inline ProbeCar DecodeProbe(const uint32_t words[kStatsWords], uint32_t slot, uint32_t car_count) {
  ProbeCar p;
  if (slot >= kProbeCars || slot >= car_count) {
    return p;
  }
  const uint32_t* w = words + kStatProbe + slot * kProbeWordsPerCar;
  float f[kProbeWordsPerCar];
  static_assert(sizeof(f) == sizeof(uint32_t) * kProbeWordsPerCar);
  std::memcpy(f, w, sizeof(f));
  p.read = true;
  for (int c = 0; c < 3; ++c) {
    p.centre[c] = f[c];
    p.paint[c] = f[4 + c];
  }
  p.radius = f[3];
  p.flags = w[7];
  return p;
}

// Whether the shader read exactly the bits the host wrote for the car (the row the host sent: its radius, centre and paint; a car with no paint writes zeros and flags 0).
inline bool ProbeMatches(const ProbeCar& read, const float centre[3], float radius, bool has_paint, const float paint[3]) {
  if (!read.read) {
    return false;
  }
  const auto same = [](float a, float b) { return std::memcmp(&a, &b, sizeof(float)) == 0; };
  for (int c = 0; c < 3; ++c) {
    if (!same(read.centre[c], centre[c]) || !same(read.paint[c], has_paint ? paint[c] : 0.0f)) {
      return false;
    }
  }
  return same(read.radius, radius) && read.flags == (has_paint ? 1u : 0u);
}

}  // namespace mcla::gpu::car_rt_rules
