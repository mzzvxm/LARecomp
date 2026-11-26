#pragma once
// ===========================================================================
// MCLA GPU renderer -- the player's side mirrors: which draws of the car are the mirror glass (the rules that need no renderer)
// ===========================================================================
// The game has no mirror slot in a car's model (perf/RT-MODEL-SURVEY.md: the 55 part slots are doors, bumpers, lights ... and no mirror), and no shader of its own for the glass of a side
// mirror: the glass is one more small piece of the body's draws, shaded by a material that reads the environment map (the R34's two mirror glasses are draws of xBumpSpecAlpha, the
// shader xGrill and xGrillOpaque share, hash 8F31933E41A20C72, which the door handles' and the grille's draws also use). So a shader name cannot find it. Its SHAPE can: from the
// car dump of the R34 (ports\run\cardump2-run\car_dump_14104.obj, groups 55 and 59) a mirror glass is
//   - a small, nearly flat patch: 17 triangles, 0.017 square metres in all (12 cm by 16.5 cm), the triangles' normals within 6 degrees of one another (a slightly convex glass);
//   - on each side of the car, at the same height, a little above the door's belt line (y 18.71 to 18.83 on a car 17.84 to 19.16 high, 0.9 of the car's width from its middle);
//   - facing backwards and a little inwards (normal (0.91, 0.00, -0.41) on one side and (0.93, 0.00, 0.37) on the other in a world where the car faces -x), towards the driver.
// MeasurePatch takes the first (a mesh's own numbers, made once when the mesh is read from guest memory, car_rt_scene.cpp FinishStaged); IsPatchCandidate keeps the draws that are small
// and flat enough and are not a kind of part that has such patches by the dozen (paint panels, lights, tyres, the interior); ChoosePair takes, in the frame's view space, the
// two candidates that are a pair of mirrors: the same size, the same height, across the car from one another, facing the same way and tilted towards each other. Both must be there:
// one lone patch is never taken (a door handle, a badge or a vent is not a pair).
//
// SEE-THROUGH GLASS (kSeeThroughShaders): the windows of a car are glass a camera looks through (a cockpit camera sees the mirror through the side window: the window's draws come
// after the world and write no depth). The table's row says so (CAR_RT_FLAG_SEE_THROUGH), and the mirror's primary ray goes on through it.
//
// CHECK (nothing here ran in the game; one car's dump is all there is): that another car's mirror glass is such a patch (a mirror glass made of two-sided triangles has a resultant
// normal near zero and is not found; a mirror that is part of a larger mesh is not found either). The first log line of a pair ("[car mirror]") says what was found: area, normals,
// separation and height.
//
// This header includes nothing of the renderer: mirror_glass_rules_test.cpp builds on its own.
// ===========================================================================

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <vector>

namespace mcla::gpu::car_mirror {

// ---------------------------------------------------------------------------
// the numbers
// ---------------------------------------------------------------------------

// A mesh with more triangles than this is never a mirror glass (the R34's has 17; the check costs a pass over the mesh's triangles, so a big mesh is left at once).
inline constexpr uint32_t kMaxPatchTriangles = 160;
inline constexpr float kMinPatchArea = 0.005f;   // m2: under it a badge, a door handle, a screw
inline constexpr float kMaxPatchArea = 0.07f;    // m2: over it a window or a panel (the R34's mirror glass is 0.017)
inline constexpr float kMaxPatchExtent = 0.40f;  // m: the longest side of the mesh's box
inline constexpr float kMinFlat = 0.92f;         // the resultant of the area weighted triangle normals (1: all the same way, 0: a closed box); the R34's glass is above 0.99

// The pair (view space, metres): across the car from one another, the same height, facing the same way and tilted towards each other.
inline constexpr float kMinSeparation = 1.2f;    // the mirrors of a car 1.9 m wide are about 1.8 m apart
inline constexpr float kMaxSeparation = 2.9f;
inline constexpr float kMaxHeightDifference = 0.12f;
inline constexpr float kMaxUpComponent = 0.55f;  // |normal . up|: the glass is nearer the vertical than the horizontal
inline constexpr float kMaxAcrossComponent = 0.75f;  // |normal . the line from mirror to mirror|: it faces along the car, only tilted across it
inline constexpr float kMinSameWay = 0.35f;      // normal . normal: both face the front or both the back
inline constexpr float kMinInward = -0.15f;      // the normal's component towards the other mirror: a glass tilts towards the driver, never away
inline constexpr float kMinAreaRatio = 0.6f;     // the smaller glass over the larger

// The material classes a candidate may not have (car_rt_material_rules.h Class: paint 1, tyre 3, light 5, interior 6; the test checks the numbers): a paint panel, a lamp lens, a tyre or a
// piece of the interior can be a small flat patch too, and is not a mirror. Glass 2, chrome 4, other 7 and the shaders the table does not know (0) are.
inline constexpr uint8_t kClassNone = 0;
inline constexpr uint8_t kClassPaint = 1;
inline constexpr uint8_t kClassGlass = 2;
inline constexpr uint8_t kClassTyre = 3;
inline constexpr uint8_t kClassChrome = 4;
inline constexpr uint8_t kClassLight = 5;
inline constexpr uint8_t kClassInterior = 6;
inline constexpr uint8_t kClassOther = 7;

// The pixel shaders of the glass a camera looks THROUGH (the runtime's own identities, the same as car_rt_material_rules.h kShaders: xCarGlass::PS_GlassFront, PS_InteriorGlass,
// PS_InteriorGrime). xBlackOpaqueGlass is opaque and not in the list: it blocks the view as it does in the game.
inline constexpr std::array<uint64_t, 3> kSeeThroughShaders = {0x4594B239C3B63D42ull, 0xFD705246ABE541BCull, 0x4ECA6B7EB23F204Cull};

inline bool IsSeeThroughShader(uint64_t ps) {
  for (const uint64_t id : kSeeThroughShaders) {
    if (id == ps) {
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// a mesh's patch numbers
// ---------------------------------------------------------------------------

struct PatchStats {
  bool valid = false;
  uint32_t triangles = 0;
  float area = 0.0f;       // m2, the sum of the triangles' areas
  float centroid[3] = {};  // the area weighted centre of the triangles, in the mesh's model space
  float normal[3] = {};    // the unit mean normal, turned to face the way the mesh's vertex normals do (the way the winding does where there are none), model space
  float flat = 0.0f;       // the resultant of the area weighted triangle normals: |sum(area n)| / sum(area)
  float extent = 0.0f;     // the longest side of the mesh's box
};

// The numbers of a mesh: `positions` float3 a vertex, `indices` a triangle list, `normals` float3 a vertex (or empty). A mesh over kMaxPatchTriangles, over kMaxPatchExtent across, or
// with no area is not valid (nothing is computed for the rest of it).
inline PatchStats MeasurePatch(const std::vector<float>& positions, const std::vector<uint32_t>& indices, const std::vector<float>& normals) {
  PatchStats out;
  const size_t vertices = positions.size() / 3;
  const size_t triangles = indices.size() / 3;
  if (triangles < 2 || triangles > kMaxPatchTriangles || vertices < 3) {
    return out;
  }
  float lo[3] = {positions[0], positions[1], positions[2]};
  float hi[3] = {lo[0], lo[1], lo[2]};
  for (size_t v = 0; v < vertices; ++v) {
    for (int c = 0; c < 3; ++c) {
      const float x = positions[v * 3 + c];
      if (!std::isfinite(x)) {
        return out;
      }
      lo[c] = std::min(lo[c], x);
      hi[c] = std::max(hi[c], x);
    }
  }
  float extent = 0.0f;
  for (int c = 0; c < 3; ++c) {
    extent = std::max(extent, hi[c] - lo[c]);
  }
  if (!(extent <= kMaxPatchExtent)) {
    return out;
  }
  double sum_n[3] = {0.0, 0.0, 0.0};
  double sum_c[3] = {0.0, 0.0, 0.0};
  double sum_a = 0.0;
  double vertex_n[3] = {0.0, 0.0, 0.0};
  const bool have_normals = normals.size() >= vertices * 3;
  for (size_t t = 0; t < triangles; ++t) {
    const uint32_t i0 = indices[t * 3], i1 = indices[t * 3 + 1], i2 = indices[t * 3 + 2];
    if (i0 >= vertices || i1 >= vertices || i2 >= vertices) {
      return out;
    }
    const float* p0 = &positions[size_t(i0) * 3];
    const float* p1 = &positions[size_t(i1) * 3];
    const float* p2 = &positions[size_t(i2) * 3];
    const double u[3] = {double(p1[0]) - p0[0], double(p1[1]) - p0[1], double(p1[2]) - p0[2]};
    const double v[3] = {double(p2[0]) - p0[0], double(p2[1]) - p0[1], double(p2[2]) - p0[2]};
    const double cross[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
    const double area = 0.5 * std::sqrt(cross[0] * cross[0] + cross[1] * cross[1] + cross[2] * cross[2]);
    if (!(area > 1e-12)) {
      continue;  // a degenerate triangle: no area, no direction
    }
    // area * unit normal = half the cross product
    for (int c = 0; c < 3; ++c) {
      sum_n[c] += 0.5 * cross[c];
      sum_c[c] += area * (double(p0[c]) + p1[c] + p2[c]) / 3.0;
    }
    sum_a += area;
    if (have_normals) {
      for (const uint32_t i : {i0, i1, i2}) {
        for (int c = 0; c < 3; ++c) {
          vertex_n[c] += normals[size_t(i) * 3 + c] * area;
        }
      }
    }
  }
  const double n_length = std::sqrt(sum_n[0] * sum_n[0] + sum_n[1] * sum_n[1] + sum_n[2] * sum_n[2]);
  if (!(sum_a > 0.0) || !(n_length > 0.0)) {
    return out;
  }
  double unit[3] = {sum_n[0] / n_length, sum_n[1] / n_length, sum_n[2] / n_length};
  if (have_normals && unit[0] * vertex_n[0] + unit[1] * vertex_n[1] + unit[2] * vertex_n[2] < 0.0) {
    for (double& c : unit) {
      c = -c;  // the mesh's own normals face the other way from its winding: they are the authority on which side is the glass's face
    }
  }
  out.valid = true;
  out.triangles = uint32_t(triangles);
  out.area = float(sum_a);
  out.extent = extent;
  out.flat = float(std::min(1.0, n_length / sum_a));
  for (int c = 0; c < 3; ++c) {
    out.centroid[c] = float(sum_c[c] / sum_a);
    out.normal[c] = float(unit[c]);
  }
  return out;
}

// Whether a draw of this mesh, of this material class, may be one of the mirror glasses.
inline bool IsPatchCandidate(const PatchStats& s, uint8_t material_class) {
  if (!s.valid || !(s.area >= kMinPatchArea) || !(s.area <= kMaxPatchArea) || !(s.extent <= kMaxPatchExtent) || !(s.flat >= kMinFlat)) {
    return false;
  }
  switch (material_class) {
    case kClassPaint:
    case kClassTyre:
    case kClassLight:
    case kClassInterior:
      return false;
    default:
      return true;
  }
}

// ---------------------------------------------------------------------------
// the pair, in the frame's view space
// ---------------------------------------------------------------------------

struct Candidate {
  uint32_t item = 0;       // the instance's place in the list (List::items)
  float centre[3] = {};    // view space
  float normal[3] = {};    // view space, unit, the way the glass faces
  float area = 0.0f;
};

// A mesh's patch placed by an instance's 3x4 (the rows of the matrix that takes a model-space point to the frame's view space, the translation in the last column). The normal is
// turned as a vector (a mirrored matrix, the right mirror made from the left one's mesh, reflects it as it reflects the glass).
inline Candidate Place(uint32_t item, const float transform[3][4], const PatchStats& s) {
  Candidate c;
  c.item = item;
  c.area = s.area;
  float n[3];
  for (int r = 0; r < 3; ++r) {
    c.centre[r] = transform[r][0] * s.centroid[0] + transform[r][1] * s.centroid[1] + transform[r][2] * s.centroid[2] + transform[r][3];
    n[r] = transform[r][0] * s.normal[0] + transform[r][1] * s.normal[1] + transform[r][2] * s.normal[2];
  }
  const float length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
  for (int r = 0; r < 3; ++r) {
    c.normal[r] = length > 1e-9f ? n[r] / length : 0.0f;
  }
  return c;
}

inline float Dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// Whether two candidates are a pair of mirrors; `up` is the world's up in view space (unit). On success `*score` ranks the pair (bigger: more alike as a mirrored pair: the same size, and the normals
// mirror images of one another).
inline bool IsMirrorPair(const Candidate& a, const Candidate& b, const float up[3], float* score = nullptr) {
  const float d[3] = {b.centre[0] - a.centre[0], b.centre[1] - a.centre[1], b.centre[2] - a.centre[2]};
  const float distance = std::sqrt(Dot3(d, d));
  if (!(distance >= kMinSeparation) || !(distance <= kMaxSeparation)) {
    return false;
  }
  const float line[3] = {d[0] / distance, d[1] / distance, d[2] / distance};  // from a to b
  if (std::fabs(Dot3(a.centre, up) - Dot3(b.centre, up)) > kMaxHeightDifference) {
    return false;
  }
  if (std::fabs(Dot3(a.normal, up)) > kMaxUpComponent || std::fabs(Dot3(b.normal, up)) > kMaxUpComponent) {
    return false;
  }
  if (std::fabs(Dot3(a.normal, line)) > kMaxAcrossComponent || std::fabs(Dot3(b.normal, line)) > kMaxAcrossComponent) {
    return false;
  }
  const float same_way = Dot3(a.normal, b.normal);
  if (!(same_way >= kMinSameWay)) {
    return false;
  }
  // tilted towards each other: a's normal has a share along a -> b, b's along b -> a
  if (!(Dot3(a.normal, line) >= kMinInward) || !(-Dot3(b.normal, line) >= kMinInward)) {
    return false;
  }
  const float larger = std::max(a.area, b.area);
  const float smaller = std::min(a.area, b.area);
  if (!(larger > 0.0f) || !(smaller / larger >= kMinAreaRatio)) {
    return false;
  }
  if (score) {
    // How alike the two are as a MIRRORED PAIR, not as two patches that face the same way: the right glass is the left one reflected in the plane half way between them (perpendicular to the line
    // from one to the other), so a's normal reflected in that plane is b's: 1 for twins. `same_way` ranked another small flat part of the R34 that faces the way a faces (the instance 103 the
    // far LOD's logs name, 1.25 m from a, area 0.0138 against 0.0165, same_way 0.92) above the real twin, whose normal is a's tilted the other way across the car (same_way 0.70, score 1.70 against 1.76).
    const float along = Dot3(a.normal, line);
    const float reflected[3] = {a.normal[0] - 2.0f * along * line[0], a.normal[1] - 2.0f * along * line[1], a.normal[2] - 2.0f * along * line[2]};
    *score = smaller / larger + Dot3(reflected, b.normal);
  }
  return true;
}

struct Pick {
  uint32_t count = 0;      // 0 or 2
  uint32_t item[2] = {};   // the two instances (List::items)
  float score = 0.0f;
};

// The best pair of the candidates, or none. At most a few dozen candidates a frame (the player's small flat draws), so every pair is looked at.
inline Pick ChoosePair(const std::vector<Candidate>& candidates, const float up[3]) {
  Pick best;
  for (size_t i = 0; i < candidates.size(); ++i) {
    for (size_t j = i + 1; j < candidates.size(); ++j) {
      float score = 0.0f;
      if (IsMirrorPair(candidates[i], candidates[j], up, &score) && (best.count == 0 || score > best.score)) {
        best.count = 2;
        best.item[0] = candidates[i].item;
        best.item[1] = candidates[j].item;
        best.score = score;
      }
    }
  }
  return best;
}

}  // namespace mcla::gpu::car_mirror
