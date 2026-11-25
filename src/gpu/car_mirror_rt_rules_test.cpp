// Check of the ray traced side mirrors' rules (car_mirror_patch_rules.h and car_mirror_rt_rules.h): the patch numbers of a mesh (a gently curved glass the size of a mirror, and the things that must
// not be taken for one: a box, a panel, a badge, a mesh of hundreds of triangles, one with bad indices), the candidates by material class, the pair of mirrors chosen among decoys in any pose of
// the car (a rotated and moved car, the world's up in view space), the setting and the class's cell, the lens from the frame's two matrices, the mirrors' rectangles on the picture, the pixel's
// arithmetic, and that hlsl/car_mirror_rt_cs.hlsl, hlsl/car_mirror_apply_ps.hlsl, hlsl/car_rt_common.hlsli, car_rt_scene_rules.h, the host (car_mirror_rt.cpp, car_mirror_rt.h), host_precache.h, the
// CMake file and the settings hold the headers' numbers. A program of its own: it needs no renderer, no ReXGlue and no game files, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_car_mirror_rt_rules_test, then run src/gpu/larecomp_car_mirror_rt_rules_test [<src dir>]
//   (the src folder is the one that holds gpu and settings; by default the one this file is in)

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "car_mirror_rt_rules.h"

namespace cm = mcla::gpu::car_mirror;
namespace ci = mcla::gpu::car_rt_instances;
namespace rr = mcla::gpu::car_rt_rules;
namespace tp = mcla::gpu::temporal;
using mcla::gpu::gpu_class::GpuClass;

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

bool Near(double a, double b, double tolerance = 1e-5) { return std::fabs(a - b) <= tolerance * std::max(1.0, std::fabs(b)); }

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}
bool Has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

// ---------------------------------------------------------------------------
// meshes
// ---------------------------------------------------------------------------

struct Mesh {
  std::vector<float> positions;
  std::vector<uint32_t> indices;
  std::vector<float> normals;
};

void Normalize(float v[3]) {
  const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  for (int c = 0; c < 3; ++c) {
    v[c] /= l;
  }
}

// A rectangular glass: `w` x `h` metres in its own plane (u along x, v along y), n x m quads of two triangles, bulging by `bulge` metres at the middle (a slightly convex glass), facing +z. The
// vertex normals follow the bulge (or are empty when `with_normals` is false); `flip_winding` reverses every triangle (the winding says the glass faces -z, the normals still say +z).
Mesh Glass(float w, float h, int n, int m, float bulge, bool with_normals, bool flip_winding) {
  Mesh mesh;
  auto height = [&](float u, float v) {
    const float x = (u / w) * 2.0f, y = (v / h) * 2.0f;
    return bulge * (1.0f - 0.5f * (x * x + y * y));
  };
  for (int j = 0; j <= m; ++j) {
    for (int i = 0; i <= n; ++i) {
      const float u = (float(i) / n - 0.5f) * w, v = (float(j) / m - 0.5f) * h;
      mesh.positions.insert(mesh.positions.end(), {u, v, height(u, v)});
      if (with_normals) {
        const float e = 1e-3f;
        float nrm[3] = {-(height(u + e, v) - height(u - e, v)) / (2 * e), -(height(u, v + e) - height(u, v - e)) / (2 * e), 1.0f};
        Normalize(nrm);
        mesh.normals.insert(mesh.normals.end(), {nrm[0], nrm[1], nrm[2]});
      }
    }
  }
  for (int j = 0; j < m; ++j) {
    for (int i = 0; i < n; ++i) {
      const uint32_t a = uint32_t(j * (n + 1) + i), b = a + 1, c = a + uint32_t(n + 1), d = c + 1;
      if (flip_winding) {
        mesh.indices.insert(mesh.indices.end(), {a, c, b, b, c, d});
      } else {
        mesh.indices.insert(mesh.indices.end(), {a, b, c, b, d, c});
      }
    }
  }
  return mesh;
}

// A closed box: no resultant normal.
Mesh Box(float sx, float sy, float sz) {
  Mesh mesh;
  for (int k = 0; k < 8; ++k) {
    mesh.positions.insert(mesh.positions.end(), {(k & 1 ? 0.5f : -0.5f) * sx, (k & 2 ? 0.5f : -0.5f) * sy, (k & 4 ? 0.5f : -0.5f) * sz});
  }
  const uint32_t faces[12][3] = {{0, 2, 1}, {1, 2, 3}, {4, 5, 6}, {5, 7, 6}, {0, 1, 4}, {1, 5, 4}, {2, 6, 3}, {3, 6, 7}, {0, 4, 2}, {2, 4, 6}, {1, 3, 5}, {3, 7, 5}};
  for (const auto& f : faces) {
    mesh.indices.insert(mesh.indices.end(), {f[0], f[1], f[2]});
  }
  return mesh;
}

// ---------------------------------------------------------------------------
// a car's pose: a rotation (about the vertical, then a pitch) and a translation, as the 3x4 of an instance
// ---------------------------------------------------------------------------

struct Pose {
  float m[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
};

Pose MakePose(float yaw, float pitch, float tx, float ty, float tz, bool mirrored = false) {
  Pose p;
  const float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
  // R = Rpitch (about x) * Ryaw (about y), column vectors
  const float ry[3][3] = {{cy, 0, sy}, {0, 1, 0}, {-sy, 0, cy}};
  const float rp[3][3] = {{1, 0, 0}, {0, cp, -sp}, {0, sp, cp}};
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      float v = 0.0f;
      for (int k = 0; k < 3; ++k) {
        v += rp[r][k] * ry[k][c];
      }
      p.m[r][c] = v;
    }
  }
  if (mirrored) {
    for (int r = 0; r < 3; ++r) {
      p.m[r][0] = -p.m[r][0];  // x -> -x before the rotation
    }
  }
  p.m[0][3] = tx;
  p.m[1][3] = ty;
  p.m[2][3] = tz;
  return p;
}

// a patch's numbers made up for a position and a facing in the car's space, then placed into the view by the pose
cm::Candidate PlaceStats(uint32_t item, const Pose& pose, const float car_centre[3], const float car_normal[3], float area) {
  cm::PatchStats s;
  s.valid = true;
  s.triangles = 17;
  s.area = area;
  s.flat = 0.999f;
  s.extent = 0.17f;
  for (int c = 0; c < 3; ++c) {
    s.centroid[c] = car_centre[c];
    s.normal[c] = car_normal[c];
  }
  return cm::Place(item, pose.m, s);
}

}  // namespace

int main(int argc, char** argv) {
  std::string src = argc > 1 ? argv[1] : std::string();
  if (src.empty()) {
    src = __FILE__;
    for (int up = 0; up < 2; ++up) {
      const size_t cut = src.find_last_of("/\\");
      if (cut != std::string::npos) {
        src = src.substr(0, cut);
      }
    }
  }
  const std::string gpu = src + "/gpu";

  // =========================================================================
  // the patch numbers
  // =========================================================================
  {
    // a mirror's glass: 12 cm by 16.5 cm, bulging 1.2 cm, 8 x 2 quads
    const Mesh glass = Glass(0.165f, 0.12f, 4, 2, 0.012f, true, false);
    const cm::PatchStats s = cm::MeasurePatch(glass.positions, glass.indices, glass.normals);
    Check(s.valid && s.triangles == 16, "a gently convex glass is measured: valid, its triangles");
    Check(s.area > 0.0195f && s.area < 0.0215f, "its area is the glass's (0.0198 plus the bulge)");
    Check(s.flat > 0.98f, "a gently curved glass is flat: resultant above 0.98");
    Check(s.normal[2] > 0.99f && std::fabs(s.normal[0]) < 0.05f, "its normal faces +z");
    Check(s.extent > 0.16f && s.extent < 0.17f, "its extent is its longest side");
    Check(std::fabs(s.centroid[2]) < 0.012f && std::fabs(s.centroid[0]) < 1e-4f && std::fabs(s.centroid[1]) < 1e-4f, "its centroid is in the middle, within the bulge");
    Check(cm::IsPatchCandidate(s, cm::kClassOther) && cm::IsPatchCandidate(s, cm::kClassChrome) && cm::IsPatchCandidate(s, cm::kClassGlass) && cm::IsPatchCandidate(s, cm::kClassNone),
          "a glass of a part that can be one (other, chrome, glass, a shader the table does not know) is a candidate");
    Check(!cm::IsPatchCandidate(s, cm::kClassPaint) && !cm::IsPatchCandidate(s, cm::kClassLight) && !cm::IsPatchCandidate(s, cm::kClassTyre) && !cm::IsPatchCandidate(s, cm::kClassInterior),
          "a patch of paint, a lamp, a tyre or the interior is not");

    // the winding says -z, the vertex normals say +z: the normals are the authority
    const Mesh flipped = Glass(0.165f, 0.12f, 4, 2, 0.012f, true, true);
    const cm::PatchStats f = cm::MeasurePatch(flipped.positions, flipped.indices, flipped.normals);
    Check(f.valid && f.normal[2] > 0.99f, "the vertex normals decide which side faces: a reversed winding with +z normals faces +z");
    const cm::PatchStats no_normals = cm::MeasurePatch(flipped.positions, flipped.indices, {});
    Check(no_normals.valid && no_normals.normal[2] < -0.99f, "with no vertex normals the winding decides (reversed: -z)");
    Check(Near(f.area, s.area, 1e-4), "the area does not depend on the winding");
  }
  {
    // what is not a mirror glass
    const Mesh box = Box(0.15f, 0.12f, 0.1f);
    const cm::PatchStats b = cm::MeasurePatch(box.positions, box.indices, {});
    Check((!b.valid || b.flat < 0.05f) && !cm::IsPatchCandidate(b, cm::kClassOther), "a closed box has no resultant normal: not flat (or not measured), not a candidate");
    const Mesh panel = Glass(0.6f, 0.4f, 4, 2, 0.0f, true, false);
    Check(!cm::MeasurePatch(panel.positions, panel.indices, panel.normals).valid, "a panel 60 cm across is too big to be measured");
    const Mesh badge = Glass(0.05f, 0.04f, 2, 1, 0.0f, true, false);
    const cm::PatchStats bs = cm::MeasurePatch(badge.positions, badge.indices, badge.normals);
    Check(bs.valid && !cm::IsPatchCandidate(bs, cm::kClassOther), "a badge is measured and is under the area (0.002)");
    const Mesh window = Glass(0.38f, 0.3f, 4, 2, 0.0f, true, false);
    const cm::PatchStats ws = cm::MeasurePatch(window.positions, window.indices, window.normals);
    Check(ws.valid && ws.area > cm::kMaxPatchArea && !cm::IsPatchCandidate(ws, cm::kClassOther), "a quarter window is over the area");
    const Mesh many = Glass(0.2f, 0.2f, 12, 8, 0.0f, true, false);  // 192 triangles
    Check(!cm::MeasurePatch(many.positions, many.indices, many.normals).valid, "a mesh over kMaxPatchTriangles is left at once");
    Mesh bad = Glass(0.165f, 0.12f, 2, 1, 0.0f, true, false);
    bad.indices[3] = 9999;
    Check(!cm::MeasurePatch(bad.positions, bad.indices, bad.normals).valid, "an index out of range makes the mesh invalid");
    Mesh nan = Glass(0.165f, 0.12f, 2, 1, 0.0f, true, false);
    nan.positions[4] = std::numeric_limits<float>::quiet_NaN();
    Check(!cm::MeasurePatch(nan.positions, nan.indices, nan.normals).valid, "a coordinate that is not finite makes the mesh invalid");
    Check(!cm::MeasurePatch({}, {}, {}).valid, "an empty mesh is invalid");
    // a sphere-ish fan (a knob): normals spread, not flat
    Mesh knob;
    knob.positions = {0, 0, 0.05f, 0.05f, 0, 0, 0, 0.05f, 0, -0.05f, 0, 0, 0, -0.05f, 0};
    knob.indices = {0, 1, 2, 0, 2, 3, 0, 3, 4, 0, 4, 1};
    const cm::PatchStats k = cm::MeasurePatch(knob.positions, knob.indices, {});
    Check(k.valid && k.flat < 0.8f && !cm::IsPatchCandidate(k, cm::kClassOther), "a pyramid is not flat");
  }

  // =========================================================================
  // the pair
  // =========================================================================
  {
    // a car 4.6 m long and 1.95 m wide with its origin at its middle on the ground, facing -x, y up (the shape of the one car dump there is, ports\run\cardump2-run, in the car's own axes): the two mirror
    // glasses 0.9 m from the middle, a little over the door's belt line, 0.0165 m2 each, facing backwards and tilted towards the driver
    const Pose world;
    const float left_c[3] = {-0.34f, 0.97f, 0.90f}, left_n[3] = {0.91f, 0.01f, -0.41f};
    const float right_c[3] = {-0.38f, 0.97f, -0.88f}, right_n[3] = {0.93f, 0.00f, 0.36f};
    cm::Candidate a = PlaceStats(10, world, left_c, left_n, 0.0165f);
    cm::Candidate b = PlaceStats(11, world, right_c, right_n, 0.0165f);
    const float up[3] = {0, 1, 0};
    float score = 0.0f;
    Check(cm::IsMirrorPair(a, b, up, &score) && score > 1.0f, "the R34's two glasses are a pair");
    Check(cm::IsMirrorPair(b, a, up), "... in either order");

    // decoys of the same car: the licence plate (rear, on the middle), a chrome patch and a black glass on the middle line, the interior trim of the two doors (they face each other)
    const float plate_c[3] = {2.33f, 0.50f, -0.06f}, plate_n[3] = {1.0f, -0.06f, -0.02f};
    const float chrome_c[3] = {-0.28f, 1.22f, 0.01f}, chrome_n[3] = {0.97f, -0.14f, -0.18f};
    const float black_c[3] = {-0.54f, 0.92f, 0.02f}, black_n[3] = {0.94f, 0.16f, -0.31f};
    const float trim_a_c[3] = {-0.48f, 0.47f, 0.68f}, trim_a_n[3] = {-0.03f, 0.01f, -1.0f};
    const float trim_b_c[3] = {-0.51f, 0.47f, -0.66f}, trim_b_n[3] = {0.02f, 0.01f, 1.0f};
    std::vector<cm::Candidate> all = {PlaceStats(1, world, plate_c, plate_n, 0.044f), PlaceStats(2, world, chrome_c, chrome_n, 0.0095f), a,
                                      PlaceStats(3, world, black_c, black_n, 0.0056f),   PlaceStats(4, world, trim_a_c, trim_a_n, 0.0426f),
                                      PlaceStats(5, world, trim_b_c, trim_b_n, 0.0425f), b};
    const cm::Pick pick = cm::ChoosePair(all, up);
    Check(pick.count == 2 && ((pick.item[0] == 10 && pick.item[1] == 11) || (pick.item[0] == 11 && pick.item[1] == 10)), "among the decoys the pair is the two glasses");
    // every other pair of the decoys is refused
    int others = 0;
    for (size_t i = 0; i < all.size(); ++i) {
      for (size_t j = i + 1; j < all.size(); ++j) {
        if (cm::IsMirrorPair(all[i], all[j], up) && !((all[i].item == 10 && all[j].item == 11) || (all[i].item == 11 && all[j].item == 10))) {
          ++others;
        }
      }
    }
    Check(others == 0, "no other pair of the decoys is a pair");
    // the decoy the far LOD's logs show (2026-11-24, MIRROR-RT-GATE: instance 103 beside the glass 51, 1.25 m from it, 0.0138 against 0.0165 m2): another small flat part at the glass's height that faces the way
    // the left glass faces. By the loose rules it IS a pair with the left glass; the real twin must score higher, so the choice is the glasses whatever order the draws come in
    {
      const float hub_c[3] = {-0.34f, 0.93f, -0.35f}, hub_n[3] = {0.99f, 0.12f, 0.0f};
      const cm::Candidate hub = PlaceStats(7, world, hub_c, hub_n, 0.0138f);
      float hub_score = 0.0f, real_score = 0.0f;
      Check(cm::IsMirrorPair(a, hub, up, &hub_score) && cm::IsMirrorPair(a, b, up, &real_score), "the part that faces the way the left glass does passes the loose rules, as the logs show");
      Check(real_score > hub_score + 0.1f, "... and the real twin scores clearly higher (a mirrored pair, not two patches facing alike)");
      const cm::Pick p1 = cm::ChoosePair({a, hub, b}, up);
      const cm::Pick p2 = cm::ChoosePair({hub, b, a}, up);
      const cm::Pick p3 = cm::ChoosePair({b, a, hub}, up);
      auto real = [](const cm::Pick& p) { return p.count == 2 && p.item[0] + p.item[1] == 21 && p.item[0] != p.item[1]; };
      Check(real(p1) && real(p2) && real(p3), "ChoosePair takes the two glasses in any order of the candidates");
      Check(cm::ChoosePair({a, hub}, up).count == 2, "with the right glass missing from the frame the left one still pairs with what is there (the loose rules, as before)");
    }
    Check(cm::ChoosePair({a}, up).count == 0, "one patch is never a pair");
    Check(cm::ChoosePair({}, up).count == 0, "no patch is no pair");

    // the refusals, one at a time
    {
      cm::Candidate far_b = b;
      far_b.centre[2] = -3.1f;  // 4 m apart
      Check(!cm::IsMirrorPair(a, far_b, up), "four metres apart is not across one car");
      cm::Candidate near_b = b;
      near_b.centre[2] = 0.6f;  // 0.3 m apart
      Check(!cm::IsMirrorPair(a, near_b, up), "30 cm apart is the same side");
      cm::Candidate high_b = b;
      high_b.centre[1] += 0.4f;
      Check(!cm::IsMirrorPair(a, high_b, up), "40 cm higher is not the same height");
      cm::Candidate small_b = b;
      small_b.area = 0.008f;
      Check(!cm::IsMirrorPair(a, small_b, up), "half the area is not the same size");
      cm::Candidate outward_a = a, outward_b = b;
      outward_a.normal[2] = 0.41f;   // tilted away from the other one
      outward_b.normal[2] = -0.36f;
      Check(!cm::IsMirrorPair(outward_a, outward_b, up), "glasses tilted away from each other are not mirrors (headlamps tilt outwards)");
      cm::Candidate opposite_b = b;
      for (int c = 0; c < 3; ++c) {
        opposite_b.normal[c] = -opposite_b.normal[c];
      }
      Check(!cm::IsMirrorPair(a, opposite_b, up), "facing opposite ways is not a pair");
      cm::Candidate up_a = a;
      up_a.normal[0] = 0.0f;
      up_a.normal[1] = 1.0f;
      up_a.normal[2] = 0.0f;
      Check(!cm::IsMirrorPair(up_a, b, up), "a patch that faces up (a roof panel, a vent) is not a mirror glass");
      cm::Candidate across_a = a, across_b = b;
      across_a.normal[0] = 0.1f;
      across_a.normal[2] = -0.99f;
      across_b.normal[0] = 0.1f;
      across_b.normal[2] = 0.99f;
      Check(!cm::IsMirrorPair(across_a, across_b, up), "patches that face across the car (door panels) are not mirrors");
    }

    // any pose of the car: rotated about the vertical, pitched, moved, mirrored matrix on one glass (the right glass made from the left one's mesh)
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> angle(-3.14159f, 3.14159f), pitch(-0.3f, 0.3f), at(-50.0f, 50.0f);
    int ok = 0, total = 0;
    for (int trial = 0; trial < 200; ++trial) {
      const Pose pose = MakePose(angle(rng), pitch(rng), at(rng), at(rng), at(rng));
      // a car at the origin facing -x (as the R34): the glasses in car space
      const float lc[3] = {-0.34f, 0.97f, 0.90f}, ln[3] = {0.91f, 0.01f, -0.41f};
      const float rc[3] = {-0.38f, 0.97f, -0.88f}, rn[3] = {0.93f, 0.0f, 0.36f};
      const float plate_car_c[3] = {2.33f, 0.50f, -0.06f}, plate_car_n[3] = {1.0f, -0.06f, -0.02f};
      const float hub_car_c[3] = {-0.34f, 0.93f, -0.35f}, hub_car_n[3] = {0.99f, 0.12f, 0.0f};
      std::vector<cm::Candidate> placed = {PlaceStats(0, pose, plate_car_c, plate_car_n, 0.044f), PlaceStats(1, pose, lc, ln, 0.0165f), PlaceStats(2, pose, rc, rn, 0.0165f),
                                           PlaceStats(3, pose, hub_car_c, hub_car_n, 0.0138f)};
      // the world's up through the same pose: the pose carries (0,1,0) to its second column
      const float view_up[3] = {pose.m[0][1], pose.m[1][1], pose.m[2][1]};
      const cm::Pick p = cm::ChoosePair(placed, view_up);
      ++total;
      ok += (p.count == 2 && p.item[0] + p.item[1] == 3 && p.item[0] != p.item[1] && p.item[0] <= 2 && p.item[1] <= 2 && (p.item[0] * p.item[1] == 2)) ? 1 : 0;
    }
    Check(ok == total, "the pair is found in 200 random poses of the car (a pose is a rotation and a translation of the view: the world's up is the pose's own)");

    // a mirrored instance matrix (the right glass is the left glass's mesh through x -> -x): the normal is reflected as a vector, so the pair still holds
    {
      const Pose mirrored = MakePose(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, true);
      cm::PatchStats s;
      s.valid = true;
      s.area = 0.0165f;
      s.flat = 0.999f;
      s.extent = 0.17f;
      const float c[3] = {0.89f, 0.77f, -0.37f};
      float n[3] = {-0.41f, 0.01f, 0.91f};  // the left glass in a car that faces -z: x across the car, z along it
      Normalize(n);
      for (int k = 0; k < 3; ++k) {
        s.centroid[k] = c[k];
        s.normal[k] = n[k];
      }
      const Pose identity;
      const cm::Candidate left = cm::Place(0, identity.m, s);
      const cm::Candidate right = cm::Place(1, mirrored.m, s);
      const float u[3] = {0, 1, 0};
      Check(Near(right.normal[0], -left.normal[0]) && Near(right.normal[2], left.normal[2]) && Near(right.centre[0], -left.centre[0]), "a mirrored matrix reflects the normal as a vector (and the centre)");
      Check(cm::IsMirrorPair(left, right, u), "a glass and its mirrored copy are a pair");
    }
  }
  {
    // the see-through glass: the runtime's identities of xCarGlass::PS_GlassFront, PS_InteriorGlass, PS_InteriorGrime, and nothing else
    Check(cm::IsSeeThroughShader(0x4594B239C3B63D42ull) && cm::IsSeeThroughShader(0xFD705246ABE541BCull) && cm::IsSeeThroughShader(0x4ECA6B7EB23F204Cull), "the windows' shaders are see-through");
    Check(!cm::IsSeeThroughShader(0x67F040F3F685FD68ull) && !cm::IsSeeThroughShader(0x8F31933E41A20C72ull) && !cm::IsSeeThroughShader(0) && !cm::IsSeeThroughShader(0x2D8CA0430FF88ED7ull),
          "xBlackOpaqueGlass, the grille's shader, no shader and the chrome are not");
  }

  // =========================================================================
  // the setting and the class's cell
  // =========================================================================
  {
    Check(cm::SettingOf("auto") == cm::Setting::kAuto && cm::SettingOf("off") == cm::Setting::kOff && cm::SettingOf("on") == cm::Setting::kOn && cm::SettingOf("") == cm::Setting::kAuto &&
              cm::SettingOf("high") == cm::Setting::kAuto,
          "the setting's words; anything else is auto");
    Check(Near(0, 0) && std::string(cm::SettingName(cm::Setting::kOn)) == "on" && std::string(cm::SettingName(cm::Setting::kOff)) == "off" && std::string(cm::SettingName(cm::Setting::kAuto)) == "auto", "the names");
    for (const GpuClass cls : {GpuClass::kLow, GpuClass::kMid, GpuClass::kHigh}) {
      Check(!cm::Wanted(cm::Setting::kOff, true, cls), "off is never wanted");
      Check(!cm::Wanted(cm::Setting::kOn, false, cls) && !cm::Wanted(cm::Setting::kAuto, false, cls), "a device that cannot trace wants nothing, whatever the setting says");
      Check(cm::Wanted(cm::Setting::kOn, true, cls) == (cls != GpuClass::kLow), "on is wanted on the Mid and High classes; the Low class (no ray tracing, shared memory) never runs it, as the pause row is grey there");
    }
    Check(cm::Wanted(cm::Setting::kAuto, true, GpuClass::kHigh) && !cm::Wanted(cm::Setting::kAuto, true, GpuClass::kMid) && !cm::Wanted(cm::Setting::kAuto, true, GpuClass::kLow),
          "auto is on on the strong class only");
    const cm::Limits high = cm::LimitsFor(GpuClass::kHigh, 0), mid = cm::LimitsFor(GpuClass::kMid, 0), low = cm::LimitsFor(GpuClass::kLow, 0);
    Check(high.max_pixels == 160000 && mid.max_pixels == 64000 && low == mid, "the cap by class: High 160 k, Mid 64 k, Low (a test's ask) the Mid's");
    Check(Near(high.reach, 18.0) && Near(mid.reach, 12.0) && Near(low.reach, 12.0), "the reach is the class's reach to another car (car_rt_hit_rules.h)");
    Check(high.min_pixels == cm::kMinPixels, "the floor");
    Check(cm::LimitsFor(GpuClass::kHigh, 50).max_pixels == 50000 && cm::LimitsFor(GpuClass::kMid, 400).max_pixels == cm::kMaxPixelsOverride && cm::LimitsFor(GpuClass::kHigh, -3).max_pixels == 160000,
          "the override is in thousands, capped, and ignored below 1");
    Check(Near(cm::kFadeFrom, mcla::gpu::car_rt_hit::LevelFor(GpuClass::kHigh).others_fade_from), "the fade's start is the High class's fade for another car");
  }

  // =========================================================================
  // the lens
  // =========================================================================
  {
    // a camera looking along +z with a field of view, then rotated and moved: view = world -> view (row vectors); view_projection = view x projection
    const double tx = 0.9, ty = 0.5;  // tan of the half fields
    tp::Mat4 view = tp::Identity();
    const double yaw = 0.7;
    view.m[0][0] = std::cos(yaw);
    view.m[0][2] = std::sin(yaw);
    view.m[2][0] = -std::sin(yaw);
    view.m[2][2] = std::cos(yaw);
    view.m[3][0] = 12.0;
    view.m[3][1] = -3.0;
    view.m[3][2] = 40.0;
    tp::Mat4 projection;
    projection.m[0][0] = 1.0 / tx;
    projection.m[1][1] = 1.0 / ty;
    projection.m[2][2] = 1.0002;
    projection.m[2][3] = 1.0;
    projection.m[3][2] = -0.1;
    const tp::Mat4 view_projection = tp::Mul(view, projection);
    double rx = 0, ry = 0;
    Check(cm::LensOf(view, view_projection, &rx, &ry) && Near(rx, tx, 1e-6) && Near(ry, ty, 1e-6), "the lens is the projection's own, whatever the camera's pose");
    // the game's own projection has a negative x scale or the z negated: the diagonal's sign is not the lens
    tp::Mat4 flipped = projection;
    flipped.m[0][0] = -1.0 / tx;
    Check(cm::LensOf(view, tp::Mul(view, flipped), &rx, &ry) && Near(rx, tx, 1e-6), "a negative scale is the same lens");
    tp::Mat4 flat;  // all zero: not a perspective
    Check(!cm::LensOf(view, flat, &rx, &ry), "a zero matrix is no lens");
    tp::Mat4 singular = tp::Identity();
    singular.m[0][0] = 0.0;
    Check(!cm::LensOf(singular, view_projection, &rx, &ry), "a view that cannot be inverted is no lens");
    tp::Mat4 huge = projection;
    huge.m[0][0] = 1e-3;
    Check(!cm::LensOf(view, tp::Mul(view, huge), &rx, &ry), "a field of view of 89.9 degrees is refused (tan 1000)");
  }

  // =========================================================================
  // the mirrors' rectangles
  // =========================================================================
  {
    const double tx = 1.0, ty = 0.5625;  // 90 degrees across, 16:9
    const cm::Limits limits = cm::LimitsFor(GpuClass::kHigh, 0);
    auto box_at = [](float cx, float cy, float z, float half) {
      ci::ViewBox b;
      b.min[0] = cx - half;
      b.max[0] = cx + half;
      b.min[1] = cy - half;
      b.max[1] = cy + half;
      b.min[2] = z - half * 0.5f;
      b.max[2] = z + half * 0.5f;
      b.valid = true;
      return b;
    };
    // a 20 cm box 1 m in front of the camera on the axis: its rectangle is 0.2 m of a 2 m wide view over 1920 px: 192 px across, 192 * (1080/ (2*0.5625)) ... height 0.2/(1.125) * 1080
    const cm::RectResult r = cm::RectFor(box_at(0.0f, 0.0f, 1.0f, 0.1f), tx, ty, 1920.0, 1080.0, limits);
    Check(r.why == cm::Why::kUsed && !r.rect.Empty(), "a mirror in front of the camera has a rectangle");
    Check(std::abs(r.rect.Width() - (192 + 2 * cm::kRectMargin)) <= 14 && std::abs(r.rect.Height() - (192 + 2 * cm::kRectMargin)) <= 14, "the rectangle is the box's size on the picture, a margin larger (the box's front and back faces differ in size)");
    Check(std::abs((r.rect.x0 + r.rect.x1) / 2 - 960) <= 6 && std::abs((r.rect.y0 + r.rect.y1) / 2 - 540) <= 6, "... centred on the picture's middle");
    const cm::RectResult right = cm::RectFor(box_at(0.8f, 0.2f, 1.0f, 0.05f), tx, ty, 1920.0, 1080.0, limits);
    Check(right.why == cm::Why::kUsed && right.rect.x0 > 960 && right.rect.y1 < 540, "a box right of the axis and above it is right of and above the picture's middle (y up in view space, down on the picture)");
    Check(cm::RectFor(box_at(0.0f, 0.0f, -1.0f, 0.1f), tx, ty, 1920.0, 1080.0, limits).why == cm::Why::kOffScreen, "a box behind the camera is off the picture");
    Check(cm::RectFor(box_at(5.0f, 0.0f, 1.0f, 0.1f), tx, ty, 1920.0, 1080.0, limits).why == cm::Why::kOffScreen, "a box far to the side is off the picture");
    Check(cm::RectFor(ci::ViewBox{}, tx, ty, 1920.0, 1080.0, limits).why == cm::Why::kOffScreen, "a box that was never grown is off the picture");
    Check(cm::RectFor(box_at(0.0f, 0.0f, 1.0f, 0.1f), 0.0, ty, 1920.0, 1080.0, limits).why == cm::Why::kOffScreen && cm::RectFor(box_at(0.0f, 0.0f, 1.0f, 0.1f), tx, ty, 0.0, 1080.0, limits).why == cm::Why::kOffScreen,
          "no lens or no picture: nothing");
    Check(cm::RectFor(box_at(0.0f, 0.0f, 0.0f, 1.0f), tx, ty, 1920.0, 1080.0, limits).why == cm::Why::kOverCap, "a box that crosses the camera's plane has the whole picture and is over the cap");
    Check(cm::RectFor(box_at(0.0f, 0.0f, 1000.0f, 0.1f), tx, ty, 1920.0, 1080.0, limits).why == cm::Why::kTooSmall, "a box a kilometre away is under a pixel: too small to be worth the pass");
    Check(cm::RectFor(box_at(0.0f, 0.0f, 12.0f, 0.1f), tx, ty, 1920.0, 1080.0, limits).why == cm::Why::kUsed, "a 20 cm box 12 m away (a mirror seen from a chase camera, about 17 pixels) is still drawn");
    // the cap by area and by edge: a rectangle that does not fit its slot of the image or the cap is traced at 1 / divisor of its size (MIRROR-RT-GATE: the driver's mirror of a cockpit at 4K), never left
    // to the game while a divisor up to kMaxDivisor fits
    ci::ViewBox wide = box_at(0.0f, 0.0f, 1.0f, 0.1f);
    wide.min[0] = -0.7f;
    wide.max[0] = 0.7f;  // 1.4 m of a 2 m view: 1344 px across
    const cm::RectResult wide_r = cm::RectFor(wide, tx, ty, 1920.0, 1080.0, limits);
    Check(wide_r.why == cm::Why::kUsed && wide_r.rect.Width() > int32_t(cm::kImageEdge) * 2 && wide_r.divisor == 3, "a rectangle between two and three times the image's edge across is traced at a third of its size");
    ci::ViewBox huge = wide;
    huge.min[0] = -2.0f;
    huge.max[0] = 2.0f;  // wider than the picture: clipped to its 1920 px, more than 3 x 512 across
    const cm::RectResult huge_r = cm::RectFor(huge, tx, ty, 1920.0, 1080.0, limits);
    Check(huge_r.why == cm::Why::kUsed && huge_r.divisor == 4 && cm::TexelsAcross(uint32_t(huge_r.rect.Width()), huge_r.divisor) <= cm::kImageEdge, "... a rectangle the whole picture's width across at a quarter");
    Check(cm::RectFor(huge, tx, ty, 3840.0, 2160.0, limits).why == cm::Why::kOverCap && cm::RectFor(huge, tx, ty, 3840.0, 2160.0, limits).divisor == 1, "... and over four times the image's edge across it is left to the game (its divisor stays 1, never 0)");
    const cm::Limits tight = cm::LimitsFor(GpuClass::kHigh, 10);  // 10 k pixels
    const cm::RectResult tight_r = cm::RectFor(box_at(0.0f, 0.0f, 1.0f, 0.1f), tx, ty, 1920.0, 1080.0, tight);
    Check(tight_r.why == cm::Why::kUsed && tight_r.divisor == 3 && uint64_t(cm::TexelsAcross(uint32_t(tight_r.rect.Width()), 3)) * cm::TexelsAcross(uint32_t(tight_r.rect.Height()), 3) <= 10000,
          "the debug cap of 10 k pixels traces a 200 x 200 rectangle at a third of its size");
    Check(cm::RectFor(box_at(0.0f, 0.0f, 1.0f, 0.1f), tx, ty, 1920.0, 1080.0, cm::LimitsFor(GpuClass::kHigh, 1)).why == cm::Why::kOverCap, "the debug cap of 1 k pixels is under what a quarter of a 200 x 200 rectangle holds: over the cap");
    const cm::RectResult near_r = cm::RectFor(box_at(0.0f, 0.0f, 1.0f, 0.05f), tx, ty, 3840.0, 2160.0, limits);
    Check(near_r.why == cm::Why::kUsed && near_r.divisor == 1, "at 4K a 10 cm box 1 m away (about 200 x 200 pixels) is inside the High class's cell, pixel for pixel");
    const cm::RectResult big_r = cm::RectFor(box_at(0.0f, 0.0f, 1.0f, 0.1f), tx, ty, 3840.0, 2160.0, limits);
    Check(big_r.why == cm::Why::kUsed && big_r.divisor == 2, "at 4K a 20 cm box 1 m away is 400 x 400 pixels with its margin: over 160 k pixels, traced at half the size (it was left to the game)");
    // the whole picture is the answer for a box that crosses the camera's plane: no divisor makes a mirror of it (at 720p a quarter of it would fit the cell)
    Check(cm::RectFor(box_at(0.0f, 0.0f, 0.0f, 1.0f), tx, ty, 1280.0, 720.0, limits).why == cm::Why::kOverCap && cm::RectFor(box_at(0.0f, 0.0f, 0.0f, 1.0f), tx, ty, 1280.0, 720.0, cm::LimitsFor(GpuClass::kMid, 0)).why == cm::Why::kOverCap,
          "the whole picture (a box across the camera's plane) is over the cap at 720p too, whatever the class");
    // the driver's mirror of the R34's cockpit at 4K, from the dump's glass (0.157 x 0.115 m, 0.061 m deep, turned 21 degrees) about 0.5 m ahead and 0.53 m to the side, a lens of 90 x 60 degrees:
    // the view space's axis aligned box of the turned glass is 0.17 x 0.115 x 0.12 m, about 330 x 580 pixels at 4K (MIRROR-RT-GATE: the log's `over the cap` in every cockpit frame)
    ci::ViewBox driver;
    driver.min[0] = 0.45f;
    driver.max[0] = 0.62f;
    driver.min[1] = -0.2215f;
    driver.max[1] = -0.1065f;
    driver.min[2] = 0.44f;
    driver.max[2] = 0.56f;
    driver.valid = true;
    const cm::RectResult driver_r = cm::RectFor(driver, 1.0, 0.5774, 3840.0, 2160.0, limits);
    Check(driver_r.why == cm::Why::kUsed && driver_r.divisor == 2 && driver_r.rect.Height() > int32_t(cm::kImageEdge), "the cockpit's driver mirror at 4K is taller than its slot of the image and is traced at half the size");
    Check(cm::RectFor(driver, 1.0, 0.5774, 1920.0, 1080.0, limits).why == cm::Why::kUsed && cm::RectFor(driver, 1.0, 0.5774, 1920.0, 1080.0, limits).divisor == 1, "... and at 1080p pixel for pixel");
  }
  {
    // the numbers of the cap: 4K cockpit mirror 130 x 90 is well inside the High class cell; the image holds two rectangles of the edge
    const cm::Limits limits = cm::LimitsFor(GpuClass::kHigh, 0);
    Check(130u * 90u < limits.max_pixels && cm::kImageEdge * cm::kImageEdge >= limits.max_pixels, "a cockpit mirror at 4K fits the cap and the cap fits the image's rectangle");
    Check(cm::kImageWidth == 2 * cm::kImageEdge && cm::kImageHeight == cm::kImageEdge && cm::ImageX(0) == 0 && cm::ImageX(1) == cm::kImageEdge, "the image holds two rectangles side by side");
    Check(cm::HeldBytes() == uint64_t(1024) * 512 * 8, "the image's size: 4 MiB of RGBA16F");
    // FitDivisor: the smallest divisor at which a rectangle fits its slot (kImageEdge each way) and the cap; 0 when none up to kMaxDivisor does
    const uint32_t cap = limits.max_pixels;
    Check(cm::FitDivisor(300, 584, cap) == 2 && cm::FitDivisor(512, 512, 262144) == 1 && cm::FitDivisor(513, 100, cap) == 2 && cm::FitDivisor(400, 400, cap) == 1 && cm::FitDivisor(401, 400, cap) == 2,
          "FitDivisor: a rectangle one pixel over the edge or the cap goes to half, one that fits goes pixel for pixel");
    Check(cm::FitDivisor(2048, 2048, 262144) == 4 && cm::FitDivisor(2048, 2048, cap) == 0 && cm::FitDivisor(2049, 100, cap) == 0 && cm::FitDivisor(1536, 100, cap) == 3, "FitDivisor: four at most, the cap counts texels, 2049 across is too much");
    Check(cm::FitDivisor(0, 10, cap) == 0 && cm::FitDivisor(10, 0, cap) == 0 && cm::FitDivisor(1, 1, 0) == 0, "FitDivisor: an empty rectangle or no cap fits nothing");
    Check(cm::TexelsAcross(10, 1) == 10 && cm::TexelsAcross(10, 3) == 4 && cm::TexelsAcross(9, 3) == 3 && cm::TexelsAcross(1, 4) == 1 && cm::TexelsAcross(10, 0) == 10, "TexelsAcross rounds up (the last texel is a part of the rectangle)");
    {
      std::mt19937 fit_rng(11);
      std::uniform_int_distribution<uint32_t> side(1, 2300), caps(1000, 262144);
      int wrong = 0;
      for (int trial = 0; trial < 4000; ++trial) {
        const uint32_t w = side(fit_rng), h = side(fit_rng), c = caps(fit_rng);
        const uint32_t d = cm::FitDivisor(w, h, c);
        auto fits = [&](uint32_t k) { return cm::TexelsAcross(w, k) <= cm::kImageEdge && cm::TexelsAcross(h, k) <= cm::kImageEdge && uint64_t(cm::TexelsAcross(w, k)) * cm::TexelsAcross(h, k) <= c; };
        bool any = false;
        for (uint32_t k = 1; k <= cm::kMaxDivisor; ++k) {
          any = any || fits(k);
        }
        const bool minimal = d == 0 ? !any : (d <= cm::kMaxDivisor && fits(d) && (d == 1 || !fits(d - 1)));
        wrong += minimal ? 0 : 1;
      }
      Check(wrong == 0, "FitDivisor over 4000 random rectangles and caps: the answer fits, no smaller divisor does, 0 only when none fits");
    }
  }

  // =========================================================================
  // the pixel's arithmetic
  // =========================================================================
  {
    const float d[3] = {0.0f, 0.0f, 1.0f}, n[3] = {0.0f, 0.0f, -1.0f};
    float r[3];
    cm::Reflect(d, n, r);
    Check(Near(r[2], -1.0) && Near(r[0], 0.0) && Near(r[1], 0.0), "a ray straight onto a glass comes straight back");
    float d2[3] = {0.6f, 0.0f, 0.8f}, n2[3] = {0.0f, 0.0f, -1.0f};
    cm::Reflect(d2, n2, r);
    Check(Near(r[0], 0.6) && Near(r[2], -0.8), "a slanted ray keeps its x and turns its z");
    // an inward tilted glass turns the view towards the car's rear: a camera looking along the car's side
    float v[3] = {0.0f, 0.0f, 1.0f}, tilt[3] = {0.0f, 0.0f, 0.0f};
    tilt[0] = -std::sin(0.4f);
    tilt[2] = -std::cos(0.4f);
    cm::Reflect(v, tilt, r);
    Check(Near(r[0], -std::sin(0.8f), 1e-5) && Near(std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]), 1.0, 1e-5), "a glass turned by an angle turns the ray by twice it, and the ray stays a unit vector");
    Check(Near(cm::CarFade(1.0f, 18.0f), 1.0) && Near(cm::CarFade(18.0f * 0.75f, 18.0f), 1.0) && Near(cm::CarFade(18.0f, 18.0f), 0.0) && cm::CarFade(16.0f, 18.0f) > 0.0f && cm::CarFade(16.0f, 18.0f) < 1.0f,
          "a car weighs 1 up to three quarters of the reach and fades to 0 at it");
    Check(cm::CarFade(2.0f, 0.0f) == 0.0f, "no reach, no car");
    const float env[3] = {0.2f, 0.3f, 0.9f}, car[3] = {1.0f, 0.5f, 0.0f};
    float rgb[3], alpha = 0.0f;
    cm::Compose(env, car, false, 1.0f, 1.0f, 0.92f, rgb, &alpha);
    Check(Near(rgb[0], 0.2) && Near(rgb[2], 0.9) && Near(alpha, 0.92), "no car hit: the world's colour, the coverage times the strength");
    cm::Compose(env, car, true, 1.0f, 0.5f, 0.92f, rgb, &alpha);
    Check(Near(rgb[0], 1.0) && Near(rgb[2], 0.0) && Near(alpha, 0.46), "a full car hit: the car's colour; half the coverage, half the alpha");
    cm::Compose(env, car, true, 0.5f, 1.0f, 1.0f, rgb, &alpha);
    Check(Near(rgb[0], 0.6) && Near(rgb[1], 0.4) && Near(rgb[2], 0.45), "a half weight is the mean of the world and the car");
    cm::Compose(env, car, true, 7.0f, 2.0f, 3.0f, rgb, &alpha);
    Check(Near(rgb[0], 1.0) && Near(alpha, 1.0), "weight, coverage and strength are clamped to 0..1");
    Check(!cm::WorldKnown(0) && cm::WorldKnown(mcla::gpu::rt_hit::kFrameEnv) && cm::WorldKnown(mcla::gpu::rt_hit::kFrameShade) && cm::WorldKnown(mcla::gpu::rt_hit::kFrameEnv | mcla::gpu::rt_hit::kFrameShade) &&
              !cm::WorldKnown(mcla::gpu::rt_hit::kFrameSun | mcla::gpu::rt_hit::kFrameShadow | mcla::gpu::rt_hit::kFrameCascade),
          "the world is known with the map's slot or the shading's rows, and not with only the sun's or the shadow's flags (a frame with nothing is a hole, not a black mirror)");
    Check(cm::Coverage(0, 4) == 0.0f && cm::Coverage(2, 4) == 0.5f && cm::Coverage(4, 4) == 1.0f && cm::Coverage(9, 4) == 1.0f && cm::Coverage(1, 0) == 0.0f, "the coverage is the share of the taps");
    Check(Near(cm::Blend(1.0f, 0.92f, 0.0f), 0.92) && Near(cm::Blend(0.0f, 0.92f, 1.0f), 0.08) && Near(cm::Blend(5.0f, 0.0f, 3.0f), 3.0) && Near(cm::Blend(5.0f, 1.0f, 3.0f), 5.0), "the apply's blend: SRC_ALPHA, INV_SRC_ALPHA");
  }

  // =========================================================================
  // the shaders, the host and the files hold the headers' numbers
  // =========================================================================
  {
    const std::string cs = ReadFile(gpu + "/hlsl/car_mirror_rt_cs.hlsl");
    const std::string ps = ReadFile(gpu + "/hlsl/car_mirror_apply_ps.hlsl");
    const std::string common = ReadFile(gpu + "/hlsl/car_rt_common.hlsli");
    const std::string scene_rules = ReadFile(gpu + "/car_rt_scene_rules.h");
    const std::string host = ReadFile(gpu + "/car_mirror_rt.cpp");
    const std::string header = ReadFile(gpu + "/car_mirror_rt.h");
    const std::string precache = ReadFile(gpu + "/host_precache.h");
    const std::string cmake = ReadFile(gpu + "/CMakeLists.txt");
    const std::string shaders = ReadFile(gpu + "/host_shaders.h");
    const std::string scene = ReadFile(gpu + "/scene.cpp");
    const std::string renderer = ReadFile(gpu + "/renderer.cpp");
    const std::string instances = ReadFile(gpu + "/car_rt_instances.cpp");
    const std::string rscene = ReadFile(gpu + "/car_rt_scene.cpp");
    const std::string manifest = ReadFile(src + "/settings/generated/manifest.json");
    const std::string settings = ReadFile(src + "/settings/generated/settings_gpu.h");
    if (cs.empty() || ps.empty() || common.empty() || host.empty() || cmake.empty() || manifest.empty()) {
      std::printf("note: the source files were not found under %s: the text checks are skipped\n", src.c_str());
    } else {
      auto num = [](float v) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%g", double(v));
        return std::string(buf);
      };
      // the shader's numbers
      Check(Has(cs, "static const float kMirrorEnvLod = " + num(cm::kEnvLod) + ";"), "the shader's environment level is the header's");
      Check(Has(cs, "static const float kMirrorLift = " + std::string("0.003") + ";") && cm::kLift == 0.003f, "the shader's lift is the header's");
      Check(Has(cs, "static const float kMirrorLiftPerMetre = 0.001;") && cm::kLiftPerMetre == 0.001f, "the shader's lift a metre is the header's");
      Check(Has(cs, "static const float kMirrorMinNv = 0.03;") && cm::kMinNv == 0.03f, "the shader's grazing limit is the header's");
      Check(Has(cs, "static const float kMirrorPrimaryReach = 40.0;") && cm::kPrimaryReach == 40.0f, "the shader's primary reach is the header's");
      Check(Has(cs, "static const uint kMirrorSeeThroughSteps = " + std::to_string(cm::kSeeThroughSteps) + ";"), "the shader's see-through steps are the header's");
      Check(Has(cs, "static const uint kMirrorTaps = " + std::to_string(cm::kPrimaryTaps) + ";"), "the shader's taps are the header's");
      Check(Has(cs, "float2(-0.125, -0.375), float2(0.375, -0.125), float2(0.125, 0.375), float2(-0.375, 0.125)"), "the four taps are a rotated grid");
      Check(Has(cs, "[numthreads(" + std::to_string(cm::kGroup) + ", " + std::to_string(cm::kGroup) + ", 1)]"), "the thread group is the header's");
      Check(Has(cs, "CAR_RT_FLAG_MIRROR") && Has(cs, "CAR_RT_FLAG_SEE_THROUGH") && Has(cs, "CAR_RT_MASK_OTHERS"), "the shader reads the table's flags and the cars' mask bit");
      Check(Has(cs, "smoothstep(g_push.tune.z * reach, reach, r.t)"), "the shader's car fade is car_rt_hit_rules.h HitFade's smoothstep");
      Check(Has(cs, "coverage * saturate(g_push.tune.x)"), "the alpha is the coverage times the strength (Compose)");
      Check(Has(cs, "lerp(world, shaded, fade)") && Has(cs, ": 1.0;") && Has(cs, "1.0 - smoothstep"), "the shader mixes the world and the car by the fade, 1 for the player's own parts");
      Check(Has(cs, "known = (f.flags & (RT_HIT_FRAME_ENV | RT_HIT_FRAME_SHADE)) != 0u;") && Has(cs, "if (!shown || any(isnan(colour)) || any(isinf(colour)))") && Has(cs, "(other && worldKnown)"),
            "the shader leaves the game's glass where the frame knows nothing of the world (no map, no shading rows) and no part of the player's own car is hit");
      Check(Has(cs, "CarHitShade(") && Has(cs, "CarRtHitNormal(") && Has(cs, "RtHitEnvAt(") && Has(cs, "MirrorWorld"), "the shader uses the hit shading, the smooth normal, the environment map and the world's hook");
      Check(Has(cs, "ConstantBuffer<CarMirrorPush> g_push : register(b0, space3);") && Has(cs, "#define CAR_RT_SPACE 2") && Has(cs, "RWTexture2D<float4> g_output : register(u0, space1);"), "the shader's sets: the push in space 3, the car's set 2, the image at u0 of space 1");
      // the apply's
      Check(Has(ps, "ConstantBuffer<MirrorApplyPush> g_push : register(b0, space4);") && Has(ps, "uint4 image;") && Has(ps, "uint4 rect;"), "the apply's push block");
      Check(Has(ps, "Load(int3(p - lo + int2(g_push.image.yz), 0))"), "the apply reads one texel for the pixel, one for one (divisor 1)");
      // MIRROR-RT-GATE: the rectangle traced at 1 / divisor of its size (FitDivisor): the trace's texel is a square of divisor x divisor pixels, the apply reads four texels weighted by distance and alpha
      Check(Has(cs, "float MirrorDivisor()") && Has(cs, "return max(float(g_push.city.w), 1.0);") && Has(cs, "return 2.0 * g_push.lens.y / max(g_push.lens.w, 1.0) * MirrorDivisor();") &&
                Has(cs, "const float pixelAngle = MirrorTexelAngle();"),
            "the trace reads the divisor from the push's city.w (1 where it is 0) and a texel's angle is the pixel's times it");
      Check(Has(cs, "const float2 centre = float2(g_push.rect.xy) + (float2(texel) + 0.5) * divisor;") && Has(cs, "const float2 uv = (centre + kMirrorTap[k] * divisor) / render;") && Has(cs, "g_output[id.xy + g_push.image.xy] = MirrorPixel(id.xy);") &&
                Has(cs, "if (id.x >= g_push.rect.z || id.y >= g_push.rect.w)"),
            "a thread is a texel: the centre of its square of pixels, the taps spread over it, the output at the texel");
      Check(Has(ps, "const int divisor = int(max(g_push.image.w, 1u));") && Has(ps, "const int2 texels = (hi - lo + (divisor - 1)) / divisor;") && Has(ps, "const float2 at = (float2(p - lo) + 0.5) / float(divisor) - 0.5;") &&
                Has(ps, "const float w = w2.x * w2.y * texel.a;") && Has(ps, "return float4(sum.rgb / max(sum.a, 1e-6), sum.a);") && Has(ps, "clamp(t0 + step, int2(0, 0), texels - 1)"),
            "the apply's bilinear read: texel centres at half integers, clamped to the rectangle's texels, weighted by the distance and the alpha, the colour divided by the weight");
      Check(Has(header, "uint32_t image[4];  // x the image's bindless slot, y, z the rectangle's first texel in the image, w the divisor") && Has(header, "w the divisor (picture pixels to a texel each way)"), "the host's blocks say what the shaders read in their w");
      Check(Has(host, "push.city[3] = divisor;") && Has(host, "apply_push.image[3] = rects[k].divisor;") && Has(host, "cm::TexelsAcross(uint32_t(r.Width()), divisor)") && Has(host, "push.rect[2] = uint32_t(texels.Width());") &&
                Has(host, "list->dispatch(car_rt_opp_refl::GroupsX(texels, cm::kGroup), car_rt_opp_refl::GroupsY(texels, cm::kGroup), 1);") && Has(host, "pixels += texels.Area();"),
            "the host gives both shaders the divisor and dispatches over the texels");
      // the table's flags
      Check(Has(common, "#define CAR_RT_FLAG_MIRROR 0x200") && Has(common, "#define CAR_RT_FLAG_SEE_THROUGH 0x400") && Has(common, "#define CAR_RT_FLAG_AFTER_WORLD 0x100"), "the shader's flag bits");
      Check(rr::kFlagMirror == cm::kFlagMirror && rr::kFlagSeeThrough == cm::kFlagSeeThrough && rr::kFlagAfterWorld == 0x100 && (cm::kFlagMirror & 0xFFu) == 0 && (cm::kFlagSeeThrough & 0xFFu) == 0 && cm::kFlagMirror != cm::kFlagSeeThrough,
            "the flags are the scene's, above the mask's eight bits and apart");
      Check(Has(scene_rules, "kFlagMirror = 0x200") && Has(scene_rules, "kFlagSeeThrough = 0x400"), "car_rt_scene_rules.h holds the flags");
      Check(cm::kPrimaryMask == (rr::kMaskBody | rr::kMaskGlass | rr::kMaskTyres | rr::kMaskDriver), "the primary ray's parts are the player's own");
      Check(cm::kReflectionMask == (cm::kPrimaryMask | rr::kMaskOthers), "the mirror ray's parts add another car");
      // the classes
      Check(cm::kClassNone == 0 && cm::kClassPaint == uint8_t(mcla::gpu::car_rt_material::Class::kPaint) && cm::kClassGlass == uint8_t(mcla::gpu::car_rt_material::Class::kGlass) &&
                cm::kClassTyre == uint8_t(mcla::gpu::car_rt_material::Class::kTyre) && cm::kClassChrome == uint8_t(mcla::gpu::car_rt_material::Class::kChrome) &&
                cm::kClassLight == uint8_t(mcla::gpu::car_rt_material::Class::kLight) && cm::kClassInterior == uint8_t(mcla::gpu::car_rt_material::Class::kInterior) &&
                cm::kClassOther == uint8_t(mcla::gpu::car_rt_material::Class::kOther),
            "the material classes are car_rt_material_rules.h's");
      for (const uint64_t id : cm::kSeeThroughShaders) {
        char hex[32];
        std::snprintf(hex, sizeof(hex), "0x%016llX", static_cast<unsigned long long>(id));
        Check(Has(ReadFile(gpu + "/car_rt_material_rules.h"), hex), std::string("a see-through shader ") + hex + " is in the material table");
      }
      // the host
      Check(Has(host, "static_assert(sizeof(Push) == car_mirror_city::kPushBytes);") || Has(header, "static_assert(sizeof(Push) == car_mirror_city::kPushBytes);"), "the trace's push block is the city's 80 bytes (MIRROR-CITY: the old 64 and the city's 16)");
      Check(Has(header, "static_assert(sizeof(ApplyPush) == 32);"), "the apply's push block is 32 bytes");
      Check(Has(host, "HostPrecache::Key(HostPrecache::kCarMirrorRt, 0)") && Has(host, "HostPrecache::Key(HostPrecache::kCarMirrorRt, 1 + index)") && Has(precache, "kCarMirrorRt = 40,"), "the precache keys");
      Check(Has(host, "blend.srcBlend = plume::RenderBlend::SRC_ALPHA;") && Has(host, "blend.dstBlend = plume::RenderBlend::INV_SRC_ALPHA;") && Has(host, "blend.renderTargetWriteMask = kWriteRgb;"), "the apply's blend is SRC_ALPHA / INV_SRC_ALPHA on the colour channels");
      Check(Has(host, "push.image[2] = cm::kPrimaryMask;") && Has(host, "push.image[3] = cm::kReflectionMask;") && Has(host, "push.tune[2] = cm::kFadeFrom;") && Has(host, "push.tune[1] = limits.reach;"), "the host pushes the header's masks, fade and reach");
      Check(Has(host, "cm::RectFor(view.mirror_box[k]") && Has(host, "view.mirror.count != cm::kMaxMirrors"), "the host takes the rectangles from the view's boxes and wants a pair");
      Check(Has(host, "temporal::IsPlaySamples(samples)"), "the host leaves the pause and garage views to the game");
      Check(Has(host, "scene_->Request();"), "the host asks for the car's structures in every frame the pass is wanted");
      Check(Has(host, "cm::Wanted(setting, true, gpu_class::CurrentRtClass())") && Has(host, "cm::LimitsFor(gpu_class::CurrentRtClass(), int(") && Has(host, "mcla_gpu_car_mirror_rt"), "the host reads the setting through the header's rule");
      Check(Has(host, "CarMirrorRtShader()") && Has(host, "CarMirrorApplyShader()") && Has(shaders, "MCLA_GPU_HOST_SHADER_BLOB(car_mirror_rt_cs)") && Has(shaders, "MCLA_GPU_HOST_SHADER_BLOB(car_mirror_apply_ps)"),
            "the shaders' accessors");
      Check(Has(shaders, "car_mirror_rt_cs.hlsl.dxil.h") && Has(shaders, "car_mirror_rt_cs.hlsl.spirv.h") && Has(shaders, "car_mirror_apply_ps.hlsl.dxil.h") && Has(shaders, "car_mirror_apply_ps.hlsl.spirv.h"),
            "host_shaders.h includes both backends' blobs");
      Check(Has(cmake, "larecomp_gpu_host_shader_rt(car_mirror_rt_cs cs_6_5)") && Has(cmake, "larecomp_gpu_host_shader(car_mirror_apply_ps ps_6_0)") && Has(cmake, "car_mirror_rt.cpp") &&
                Has(cmake, "larecomp_car_mirror_rt_rules_test"),
            "the CMake file builds the shaders, the host and this test");
      Check(Has(scene, "car_mirror_->BeginFrame();") && Has(scene, "car_mirror_->OnResolve(summary, list,") && Has(scene, "car_rt_->TriggeredThisResolve()") && Has(scene, "car_mirror_->OnPresent();"),
            "the scene calls the pass at the frame's start, at the trigger after the structures are built, and at the present");
      Check(Has(renderer, "car_mirror_.Initialize(") && Has(renderer, "scene_.SetCarMirrorRt(&car_mirror_);") && Has(renderer, "car_mirror_.Shutdown();"), "the renderer makes, hands over and shuts down the pass");
      Check(Has(instances, "car_mirror::ChoosePair(") && Has(instances, "rr::kFlagMirror") && Has(instances, "rr::kFlagSeeThrough") && Has(instances, "car_mirror::IsPatchCandidate("), "the instance list finds the pair and flags the rows");
      Check(Has(rscene, "car_mirror::MeasurePatch(") && Has(rscene, "ref->patch = it->second.patch;") && Has(rscene, "p.mesh.patch = staged.patch;") && Has(rscene, "view_.mirror = cars.mirror;") && Has(rscene, "car_mirror::LensOf("),
            "the scene measures a mesh's patch once, carries it to the list and hands the pair and the lens to the view");
      // the settings
      for (const char* name : {"mcla_gpu_car_mirror_rt", "mcla_gpu_car_mirror_rt_pixels", "mcla_gpu_car_mirror_rt_strength"}) {
        Check(Has(manifest, std::string("\"") + name + "\": {") && Has(settings, std::string("REX_SETTING(") ) && Has(settings, std::string(", ") + name + ", ") && Has(settings, std::string("inline auto& ") + name + " = g_gpu." + name + ";"),
              std::string("the setting ") + name + " is in the manifest and the settings header");
      }
      Check(Has(settings, "mcla_gpu_car_mirror_rt_strength, 0.92,") && Near(cm::kStrength, 0.92), "the strength's default is the header's");
      Check(Has(settings, "mcla_gpu_car_mirror_rt, \"auto\",") && Has(settings, "rex::Allowed(\"auto\", \"off\", \"on\")"), "the mode's default is auto and its words are auto, off, on");
    }
  }

  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
