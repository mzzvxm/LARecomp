#pragma once
// ===========================================================================
// MCLA GPU renderer -- the side mirrors' glass found without ray tracing: the rules that need no renderer (MIRROR-VIEW MV3)
// ===========================================================================
// perf/MIRROR-VIEW.md 2.6 and 4 (MV3). The shared rear view's side mirror overlay (MV4) and the "is a mirror visible" need rule (mirror_glass_need.h) both need to know which two draws of the
// player's car are the mirror glass. The shape rules in car_mirror_patch_rules.h find small flat patches across the car at one height facing back and inward. The finder takes
// the triangles from the ray traced scene's staged meshes, which a card without ray tracing does not have. CarFrame has them on every GPU (car_frame.h: the player's draws of a frame, each with its
// matrix, its host geometry and a mesh key, and a MeshReader that reads one draw's triangles from guest memory). This header is the part of the listener that needs no renderer:
//
//   THE GATE      WhySkipped: which draws are worth reading at all (a small draw of the car's body: 2 to 160 triangles, not a tyre, a lamp, a paint panel or the interior: the class of the draw's pixel
//                 shader by the table the ray traced scene uses, car_rt_material_rules.h FindShader, so both finders take the same draws).
//   THE CACHE     GlassCache: a mesh is read and measured ONCE, by its mesh_key (a fingerprint of the mesh's own bytes: stable across frames and sessions). What is kept is the mesh's PatchStats and its
//                 box, whether or not it is a patch, so a steady frame costs one hash probe a draw and no guest memory. An unreadable mesh is tried again after a while; keys no draw names for a long
//                 while go (a car given up), and the cache has a cap.
//   THE MEASURE   MeasureMesh: the ray traced scene's own clean-up (car_rt_scene_rules.h CleanMesh) and car_mirror::MeasurePatch: the numbers used to select the mirror pair.
//   THE PAIR      Finder::Choose: the candidates of the frame (a draw whose mesh is a patch and whose class may be a glass) placed in the frame's view space (car_rt_scene_rules.h PackInstanceTransform
//                 with Frame::view's z forward), car_mirror::ChoosePair for the pair, then what the overlay and the guest need: each glass's box, its world centre and normal, the eye,
//                 the lens, the box's rectangle on a reference picture, and whether it is wide enough to be worth a view (the need rule: kMinNeedWidth pixels at 1080 lines).
//
// The finder is stateless about the pair: it is chosen again in every frame from the cached numbers (a few dozen candidates at most, every pair is looked at), so a camera that hides the
// mirrors for a frame, a car that is replaced or a draw that is missing needs no bookkeeping. The only memory is the cache and (mirror_glass_need.h) the hold of the flag the
// guest reads.
//
// WHAT IT DOES NOT KNOW (nothing here ran in the game):
// CHECK: with no ray traced scene a draw's Material is not made, so the class comes from the pixel shader's identity alone (the same table, and kNone for a shader the table does not know, which
// MIRROR-RT also accepts: two lamp lenses of an unknown shader would look like a pair, the test pins it).
// CHECK: another car's mirror glass is such a patch (car_mirror_patch_rules.h's own CHECK).
// CHECK: the pause view's and the garage's frames list the car once per tile (car_frame.cpp): the duplicates are the same glass at the same place and are dropped here, but the finder does not tell
// the views apart (the readers do: play view only).
// CHECK: Frame::view has z forward (a run says so: "[car rt] the frame's camera matrix has z forward"); Camera::flip_z is fixed false, not decided per frame as the ray traced scene does.
//
// This header includes nothing of the renderer: mirror_glass_rules_test.cpp builds on its own.
// ===========================================================================

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "car_mirror_patch_rules.h"
#include "car_rt_instances_rules.h"
#include "car_rt_material_rules.h"
#include "car_rt_opp_refl_rules.h"
#include "car_rt_scene_rules.h"
#include "mirror_glass_need.h"
#include "mirror_view_rules.h"
#include "temporal_math.h"

namespace mcla::gpu::mirror_glass {

namespace cm = car_mirror;

inline bool LensOf(const temporal::Mat4& view, const temporal::Mat4& view_projection, double* tan_x, double* tan_y) {
  temporal::Mat4 inverse;
  if (!tan_x || !tan_y || !temporal::Inverse(view, &inverse) || !temporal::Finite(view_projection)) {
    return false;
  }
  const temporal::Mat4 p = temporal::Mul(inverse, view_projection);
  const double px = std::fabs(p.m[0][0]);
  const double py = std::fabs(p.m[1][1]);
  if (!std::isfinite(px) || !std::isfinite(py) || !(px > 1e-6) || !(py > 1e-6)) {
    return false;
  }
  const double tx = 1.0 / px;
  const double ty = 1.0 / py;
  if (!(tx >= 0.05 && tx <= 20.0 && ty >= 0.05 && ty <= 20.0)) {
    return false;
  }
  *tan_x = tx;
  *tan_y = ty;
  return true;
}

namespace ci = car_rt_instances;
namespace rr = car_rt_rules;

// ---------------------------------------------------------------------------
// the numbers
// ---------------------------------------------------------------------------

// A draw with more host indices than this has more triangles than car_mirror::kMaxPatchTriangles: never a glass (and never read).
inline constexpr uint32_t kMaxIndices = 3 * cm::kMaxPatchTriangles;
inline constexpr uint32_t kMinIndices = 6;  // MeasurePatch wants at least two triangles
// Meshes read in one frame (a draw's call holds the guest back: a read of a small mesh is microseconds, but the first frame of a car has about a hundred of them). The rest wait for the next frame.
inline constexpr uint32_t kReadsPerFrame = 32;
// A mesh no draw has named for this many frames leaves the cache (a car given up: the garage, a new race), and the most entries it holds (a car is about a hundred small draws).
inline constexpr uint64_t kKeepFrames = 1800;
inline constexpr size_t kMaxEntries = 2048;
// An unreadable mesh is tried again after this many frames (its range may have been streamed in).
inline constexpr uint64_t kRetryFailedFrames = 600;
inline constexpr uint64_t kSweepEveryFrames = 300;
// Two candidates of one mesh this near each other in view space (metres) are the same draw listed twice (the pause view's tiles).
inline constexpr float kDuplicateMetres = 0.001f;
// The picture the need rule is judged on: 1080 lines high, as wide as the lens makes it.
// CHECK: 40 px (mirror_glass_need.h kMinNeedWidth) on this picture is the design's first guess of what a mirror must cover to be worth a view; the owner's eye on the chase cameras decides.
inline constexpr double kReferenceHeight = 1080.0;

// ---------------------------------------------------------------------------
// the gate
// ---------------------------------------------------------------------------

// The class of a draw's pixel shader by the ray traced scene's table (car_rt_material_rules.h; car_mirror_patch_rules.h kClass*, the same numbers): kClassNone for a shader the table does not know.
inline uint8_t ClassOfShader(uint64_t ps_identity) {
  const car_rt_material::ShaderRule* rule = car_rt_material::FindShader(ps_identity);
  return rule ? uint8_t(rule->cls) : cm::kClassNone;
}

// Whether a draw of this material class may be a mirror glass at all: car_mirror::IsPatchCandidate's own class rule, asked with a patch that passes its numbers, so that there is one rule.
inline bool ClassMayBeGlass(uint8_t material_class) {
  cm::PatchStats s;
  s.valid = true;
  s.triangles = 2;
  s.area = cm::kMinPatchArea;
  s.extent = 0.0f;
  s.flat = 1.0f;
  return cm::IsPatchCandidate(s, material_class);
}

enum class Skip : uint8_t {
  kNone,        // worth looking at
  kNotBody,     // the driver (skinned: its vertices are blended afresh every frame)
  kNoKey,       // no mesh key (inline vertices, a stream that cannot be resolved)
  kFewIndices,
  kManyIndices, // a panel, a window, a wheel: more than a glass can have
  kClass,       // a tyre, a lamp, a paint panel, the interior
  kCount
};

inline const char* SkipName(Skip s) {
  switch (s) {
    case Skip::kNone: return "looked at";
    case Skip::kNotBody: return "not a rigid part";
    case Skip::kNoKey: return "no mesh key";
    case Skip::kFewIndices: return "under two triangles";
    case Skip::kManyIndices: return "too many triangles";
    case Skip::kClass: return "a class that is never a glass";
    case Skip::kCount: break;
  }
  return "?";
}

// `host_indices` is the draw's host index count (DrawGeometry::count: a list's indices, a strip already converted to a list).
inline Skip WhySkipped(bool rigid, uint64_t mesh_key, uint32_t host_indices, uint8_t material_class) {
  if (!rigid) {
    return Skip::kNotBody;
  }
  if (mesh_key == 0) {
    return Skip::kNoKey;
  }
  if (host_indices < kMinIndices) {
    return Skip::kFewIndices;
  }
  if (host_indices > kMaxIndices) {
    return Skip::kManyIndices;
  }
  if (!ClassMayBeGlass(material_class)) {
    return Skip::kClass;
  }
  return Skip::kNone;
}

// ---------------------------------------------------------------------------
// the measure
// ---------------------------------------------------------------------------

struct Measured {
  bool ok = false;           // the mesh had triangles after the clean-up (an unreadable or empty one did not)
  cm::PatchStats stats;      // valid false: a mesh that is no patch (too big, too wide, no area): remembered all the same
  bool has_box = false;
  float box_min[3] = {};
  float box_max[3] = {};
  uint32_t triangles = 0;
};

// What the RT finder does with a mesh it reads (car_rt_scene.cpp FinishStaged): the clean-up, the box, the patch numbers. The vectors are cleaned in place.
inline Measured MeasureMesh(std::vector<float>* positions, std::vector<uint32_t>* indices, std::vector<float>* normals) {
  Measured out;
  rr::CleanMesh(positions, indices, normals);
  out.triangles = uint32_t(indices->size() / 3);
  if (out.triangles == 0 || positions->empty()) {
    return out;
  }
  out.ok = true;
  out.has_box = ci::BoundsOf(*positions, out.box_min, out.box_max);
  out.stats = cm::MeasurePatch(*positions, *indices, *normals);
  return out;
}

// ---------------------------------------------------------------------------
// the cache, by mesh_key
// ---------------------------------------------------------------------------

class GlassCache {
 public:
  enum class State : uint8_t { kMeasured, kFailed };
  struct Entry {
    State state = State::kMeasured;
    cm::PatchStats stats;
    bool has_box = false;
    float box_min[3] = {};
    float box_max[3] = {};
    uint64_t seen = 0;  // the frame a draw last named the key
    uint64_t made = 0;  // the frame the entry was made
  };
  // What a draw of a key has to do: nothing (the key is known), read the mesh, or wait (an unreadable mesh is tried again after kRetryFailedFrames).
  enum class Need : uint8_t { kHave, kRead, kWait };

  Need Probe(uint64_t key, uint64_t frame) {
    const auto it = map_.find(key);
    if (it == map_.end()) {
      return Need::kRead;
    }
    it->second.seen = frame;
    if (it->second.state == State::kFailed) {
      return frame >= it->second.made + kRetryFailedFrames ? Need::kRead : Need::kWait;
    }
    return Need::kHave;
  }

  void NoteMeasured(uint64_t key, const Measured& m, uint64_t frame) {
    Entry& e = Slot(key, frame);
    e.state = State::kMeasured;
    e.stats = m.stats;
    e.has_box = m.has_box;
    for (int c = 0; c < 3; ++c) {
      e.box_min[c] = m.box_min[c];
      e.box_max[c] = m.box_max[c];
    }
    e.seen = frame;
    e.made = frame;
  }

  void NoteFailed(uint64_t key, uint64_t frame) {
    Entry& e = Slot(key, frame);
    e = Entry{};
    e.state = State::kFailed;
    e.seen = frame;
    e.made = frame;
  }

  const Entry* Find(uint64_t key) const {
    const auto it = map_.find(key);
    return it == map_.end() ? nullptr : &it->second;
  }

  // Drops the keys no draw named for kKeepFrames; returns how many.
  size_t Sweep(uint64_t frame) {
    size_t erased = 0;
    for (auto it = map_.begin(); it != map_.end();) {
      if (frame > it->second.seen + kKeepFrames) {
        it = map_.erase(it);
        ++erased;
      } else {
        ++it;
      }
    }
    return erased;
  }

  size_t size() const { return map_.size(); }
  void Clear() { map_.clear(); }

 private:
  // The slot of a key; a full cache gives up the stalest entry first.
  Entry& Slot(uint64_t key, uint64_t frame) {
    const auto it = map_.find(key);
    if (it != map_.end()) {
      return it->second;
    }
    if (map_.size() >= kMaxEntries) {
      auto stalest = map_.begin();
      for (auto i = map_.begin(); i != map_.end(); ++i) {
        if (i->second.seen < stalest->second.seen) {
          stalest = i;
        }
      }
      map_.erase(stalest);
    }
    Entry& e = map_[key];
    e.seen = frame;
    return e;
  }

  std::unordered_map<uint64_t, Entry> map_;
};

// ---------------------------------------------------------------------------
// the frame's candidates and the pair
// ---------------------------------------------------------------------------

// A draw of the frame whose mesh is a small flat patch of a class that may be a glass.
struct Candidate {
  uint32_t draw = 0;  // its place in the frame's list (car_frame::Frame::draws)
  uint64_t mesh_key = 0;
  temporal::Mat4 model_to_world;  // row vectors, as the game's
  cm::PatchStats stats;
  bool has_box = false;
  float box_min[3] = {};
  float box_max[3] = {};
};

// What the frame's camera says (car_frame.h Frame::view and view_projection). Frame::view is the occlusion's view space BY CONSTRUCTION (car_frame_rules.h CameraFromBank: the game's view with z
// negated: x right, y up, z forward), and a run says so ("[car rt] the frame's camera matrix has z forward, as the occlusion's view space has it: used as it is"), so `flip_z` is false. It is
// not decided from the frame the way the ray traced scene does (car_rt_scene.cpp: which side of the camera's plane the car's origin is on): a camera ahead of the car's origin (a bumper or a hood
// camera) has the origin behind its plane and the rule would flip a view that is right. A caller that has the game's own matrix (z backward) says flip_z.
struct Camera {
  temporal::Mat4 view;
  temporal::Mat4 view_projection;
  bool flip_z = false;
};

// One mirror glass of the pair, as the overlay and the need rule want it.
struct Glass {
  uint32_t draw = 0;       // its place in the frame's list of the player's draws (car_frame::Frame::draws): the draw's host geometry and matrix
  uint64_t mesh_key = 0;
  float area = 0.0f;       // m2
  float centre_view[3] = {};  // the frame's view space (x right, y up, z forward: the AO's)
  float normal_view[3] = {};  // unit, the way the glass faces (towards the driver)
  double centre_world[3] = {};  // the game's world: the draw's own model-to-world applied to the patch's centroid
  float normal_world[3] = {};   // unit, the patch's mean normal turned as a vector by the draw's matrix
  ci::ViewBox box;         // the mesh's box through the draw's matrix, in the frame's view space
  car_rt_opp_refl::TexelRect rect;  // the box on the reference picture (kReferenceHeight lines): empty when off the picture
  int32_t px_wide = 0;     // rect's width, reference pixels
  bool needed = false;     // on the picture and at least kMinNeedWidth wide
};

enum class Why : uint8_t {
  kNeeded,           // a pair was found and a glass is worth a view
  kNoCar,            // the frame has no player's car (a camera that does not draw it, a menu)
  kFewCandidates,    // under two patches among the draws: no mirrors, one mirror, or the meshes are not read yet
  kBadCamera,        // the frame's matrices are not usable (z rule undecided, no up)
  kNoPair,           // patches, but no two of them are a pair of mirrors (door handles, badges)
  kNoLens,           // a pair, but the lens is not known: nothing can be said of the picture
  kOffPicture,       // a pair, both boxes behind the camera or off the picture
  kTooSmall,         // a pair on the picture, both narrower than kMinNeedWidth (a chase camera)
  kCount
};

inline const char* WhyName(Why w) {
  switch (w) {
    case Why::kNeeded: return "needed";
    case Why::kNoCar: return "no player's car in the frame";
    case Why::kFewCandidates: return "under two glass patches";
    case Why::kBadCamera: return "camera matrices not usable";
    case Why::kNoPair: return "no pair of mirrors";
    case Why::kNoLens: return "no lens";
    case Why::kOffPicture: return "off the picture";
    case Why::kTooSmall: return "too small";
    case Why::kCount: break;
  }
  return "?";
}

struct Result {
  uint64_t guest_frame = 0;
  Why why = Why::kNoCar;
  bool found = false;       // a pair was chosen: glass[0] and glass[1]
  Glass glass[2];           // ordered by the picture: [0] the one on the left (smaller view x), [1] on the right
  bool flip_z = false;      // the z rule the placements used (Camera::flip_z)
  double eye_world[3] = {}; // the camera's place in the world (the view's inverse)
  bool eye_known = false;
  double tan_x = 0.0;       // tan of the half fields of view (LensOf)
  double tan_y = 0.0;
  bool lens_known = false;
  float up_view[3] = {0.0f, 1.0f, 0.0f};  // the world's up in the frame's view space
  float score = 0.0f;       // car_mirror::Pick::score
  uint32_t candidates = 0;  // patches among the frame's draws (after the duplicates were dropped)
  NeedState need;           // what the guest is told for this frame (before the hold)
};

// Enclose the directions the overlay samples, using the bounds of each
// needed glass in current view space. The rear rig is the main rig spun 180 degrees.
// CHECK: verify the fitted field covers the live glass in both first-person rigs.
inline void FitRearField(Result& result) {
  namespace mv = mirror_view;
  mv::Rig rear;
  rear.forward = {0, 0, -1};
  rear.up = {0, 1, 0};
  rear.right = {-1, 0, 0};
  mv::V3 rays[16];
  int count = 0;
  for (unsigned i = 0; i < 2; ++i) {
    if (!(result.need.mask & (1u << i))) continue;
    const auto& g = result.glass[i];
    const auto& box = g.box;
    if (!box.valid) continue;
    const mv::V3 normal = mv::Normalize({g.normal_view[0], g.normal_view[1], g.normal_view[2]});
    for (unsigned corner = 0; corner < 8; ++corner) {
      const mv::V3 p{corner & 1 ? box.max[0] : box.min[0], corner & 2 ? box.max[1] : box.min[1], corner & 4 ? box.max[2] : box.min[2]};
      rays[count++] = p + mv::Reflect(mv::Normalize(p), normal) * mv::kAssumedDistance;
    }
  }
  if (count == 0) return;
  const mv::Extent extent = mv::ExtentOf(rear, rays, count);
  constexpr double kDegreesAcross = 360.0 / 3.14159265358979323846;
  result.need.hfov_deg = uint16_t(extent.ok ? std::ceil(extent.half_h * kDegreesAcross) : mv::kMaxHorizontalFov);
  result.need.vfov_deg = uint16_t(extent.ok ? std::ceil(extent.half_v * kDegreesAcross) : mv::kMaxVerticalFov);
}

// The box of a mesh through a placement (the eight corners, or the origin where the box is not known).
inline void GrowViewBox(ci::ViewBox* box, const float transform[3][4], bool has_box, const float lo[3], const float hi[3]) {
  ci::MeshRef ref;
  ref.has_bounds = has_box;
  for (int c = 0; c < 3; ++c) {
    ref.bounds_min[c] = lo[c];
    ref.bounds_max[c] = hi[c];
  }
  ci::GrowBox(box, transform, ref);
}

// The rectangle a box covers on the reference picture (kReferenceHeight lines, the width the lens makes it), no margin.
inline car_rt_opp_refl::TexelRect ReferenceRect(const ci::ViewBox& box, double tan_x, double tan_y) {
  const double h = kReferenceHeight;
  const double w = std::max(1.0, std::floor(h * tan_x / tan_y + 0.5));
  return car_rt_opp_refl::BoxRect(box, tan_x, tan_y, w, h, 1.0, int32_t(w), int32_t(h), 0);
}

// The finder: the cache, the budget of a frame's reads, the frame's candidates.
class Finder {
 public:
  // What a draw's call came to.
  enum class Outcome : uint8_t {
    kSkipped,     // the gate left it out (Reply::skip says why)
    kHave,        // the mesh was known
    kMeasured,    // read and measured now
    kUnreadable,  // the read failed; tried again later
    kDeferred,    // the frame's reads were spent; the next frame
    kWaiting      // unreadable a short while ago
  };
  struct Reply {
    Outcome outcome = Outcome::kSkipped;
    Skip skip = Skip::kNone;
  };

  // A frame begins (every guest frame, before its draws): the budget of reads.
  void BeginFrame(uint64_t frame) {
    frame_ = frame;
    reads_ = 0;
  }

  // One draw of the player's car, inside CarFrame's call for it. `read(positions, indices, normals)` reads the mesh's triangles from guest memory (false: unreadable); it is called at most once
  // and only for a mesh the cache does not know, within the frame's budget.
  template <class ReadFn>
  Reply OnDraw(bool rigid, uint64_t mesh_key, uint32_t host_indices, uint64_t ps_identity, ReadFn&& read) {
    Reply reply;
    reply.skip = WhySkipped(rigid, mesh_key, host_indices, ClassOfShader(ps_identity));
    if (reply.skip != Skip::kNone) {
      return reply;
    }
    switch (cache_.Probe(mesh_key, frame_)) {
      case GlassCache::Need::kHave:
        reply.outcome = Outcome::kHave;
        return reply;
      case GlassCache::Need::kWait:
        reply.outcome = Outcome::kWaiting;
        return reply;
      case GlassCache::Need::kRead:
        break;
    }
    if (reads_ >= kReadsPerFrame) {
      reply.outcome = Outcome::kDeferred;
      return reply;
    }
    ++reads_;
    positions_.clear();
    indices_.clear();
    normals_.clear();
    if (!read(&positions_, &indices_, &normals_)) {
      cache_.NoteFailed(mesh_key, frame_);
      reply.outcome = Outcome::kUnreadable;
      return reply;
    }
    const Measured m = MeasureMesh(&positions_, &indices_, &normals_);
    if (!m.ok) {
      cache_.NoteFailed(mesh_key, frame_);
      reply.outcome = Outcome::kUnreadable;
      return reply;
    }
    cache_.NoteMeasured(mesh_key, m, frame_);
    reply.outcome = Outcome::kMeasured;
    return reply;
  }

  // The frame's draws are complete (or as complete as they are): a new evaluation starts.
  void BeginEvaluation() { candidates_.clear(); }

  // One draw of the frame: a candidate when its mesh is a measured patch and its class may be a glass. Returns whether it was one.
  bool Consider(uint32_t draw_index, bool rigid, uint64_t mesh_key, uint32_t host_indices, uint64_t ps_identity, const temporal::Mat4& model_to_world) {
    const uint8_t cls = ClassOfShader(ps_identity);
    if (WhySkipped(rigid, mesh_key, host_indices, cls) != Skip::kNone) {
      return false;
    }
    const GlassCache::Entry* e = cache_.Find(mesh_key);
    if (!e || e->state != GlassCache::State::kMeasured || !cm::IsPatchCandidate(e->stats, cls)) {
      return false;
    }
    Candidate c;
    c.draw = draw_index;
    c.mesh_key = mesh_key;
    c.model_to_world = model_to_world;
    c.stats = e->stats;
    c.has_box = e->has_box;
    for (int k = 0; k < 3; ++k) {
      c.box_min[k] = e->box_min[k];
      c.box_max[k] = e->box_max[k];
    }
    candidates_.push_back(c);
    return true;
  }

  // The pair, among the candidates Consider collected, for the frame's camera.
  Result Choose(uint64_t guest_frame, const Camera& camera) {
    Result r;
    r.guest_frame = guest_frame;
    if (candidates_.size() < 2) {
      r.why = Why::kFewCandidates;
      r.candidates = uint32_t(candidates_.size());
      return r;
    }
    r.flip_z = camera.flip_z;
    float up[3];
    const double world_up[3] = {0.0, 1.0, 0.0};
    if (!ci::ViewVector(camera.view, camera.flip_z, world_up, up)) {
      r.why = Why::kBadCamera;
      return r;
    }
    for (int c = 0; c < 3; ++c) {
      r.up_view[c] = up[c];
    }
    // place the candidates in the view space; a matrix that is not usable leaves its draw out; the same draw listed twice (the pause view's tiles) once
    placed_.clear();
    transforms_.clear();
    sources_.clear();
    for (const Candidate& c : candidates_) {
      std::array<std::array<float, 4>, 3> t{};
      float m[3][4];
      if (!rr::PackInstanceTransform(c.model_to_world, camera.view, camera.flip_z, m)) {
        continue;
      }
      const cm::Candidate placed = cm::Place(uint32_t(placed_.size()), m, c.stats);
      bool duplicate = false;
      for (size_t i = 0; i < placed_.size() && !duplicate; ++i) {
        if (sources_[i]->mesh_key != c.mesh_key) {
          continue;
        }
        const float dx = placed_[i].centre[0] - placed.centre[0];
        const float dy = placed_[i].centre[1] - placed.centre[1];
        const float dz = placed_[i].centre[2] - placed.centre[2];
        duplicate = std::sqrt(dx * dx + dy * dy + dz * dz) < kDuplicateMetres;
      }
      if (duplicate) {
        continue;
      }
      for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 4; ++j) {
          t[size_t(i)][size_t(j)] = m[i][j];
        }
      }
      placed_.push_back(placed);
      transforms_.push_back(t);
      sources_.push_back(&c);
    }
    r.candidates = uint32_t(placed_.size());
    if (placed_.size() < 2) {
      r.why = Why::kFewCandidates;
      return r;
    }
    const cm::Pick pick = cm::ChoosePair(placed_, up);
    if (pick.count != 2) {
      r.why = Why::kNoPair;
      return r;
    }
    r.found = true;
    r.score = pick.score;
    for (uint32_t k = 0; k < 2; ++k) {
      const uint32_t at = pick.item[k];  // the place in placed_ (Place was given it as the item)
      const Candidate& c = *sources_[at];
      Glass& g = r.glass[k];
      g.draw = c.draw;
      g.mesh_key = c.mesh_key;
      g.area = c.stats.area;
      for (int i = 0; i < 3; ++i) {
        g.centre_view[i] = placed_[at].centre[i];
        g.normal_view[i] = placed_[at].normal[i];
      }
      WorldPose(c, &g);
      float m[3][4];
      for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 4; ++j) {
          m[i][j] = transforms_[at][size_t(i)][size_t(j)];
        }
      }
      GrowViewBox(&g.box, m, c.has_box, c.box_min, c.box_max);
    }
    if (r.glass[1].centre_view[0] < r.glass[0].centre_view[0]) {
      std::swap(r.glass[0], r.glass[1]);  // [0] is the glass on the left of the picture
    }
    r.eye_known = ci::CameraWorld(camera.view, r.eye_world);
    r.lens_known = LensOf(camera.view, camera.view_projection, &r.tan_x, &r.tan_y);
    if (!r.lens_known) {
      r.why = Why::kNoLens;
      r.need.found = true;
      return r;
    }
    bool any_on = false;
    for (Glass& g : r.glass) {
      g.rect = ReferenceRect(g.box, r.tan_x, r.tan_y);
      g.px_wide = g.rect.Width();
      any_on = any_on || !g.rect.Empty();
      g.needed = !g.rect.Empty() && g.px_wide >= kMinNeedWidth;
      if (g.needed) {
        r.need.mask = uint8_t(r.need.mask | (&g == &r.glass[0] ? 1u : 2u));
        r.need.widest = uint16_t(std::min<int32_t>(std::max<int32_t>(r.need.widest, g.px_wide), 0xFFFF));
      }
    }
    r.need.found = true;
    FitRearField(r);
    r.why = r.need.mask != 0 ? Why::kNeeded : (any_on ? Why::kTooSmall : Why::kOffPicture);
    return r;
  }

  const GlassCache& cache() const { return cache_; }
  GlassCache& cache() { return cache_; }
  uint32_t reads_this_frame() const { return reads_; }
  size_t candidates() const { return candidates_.size(); }

  // Every kSweepEveryFrames: the keys no draw has named for a long while.
  size_t SweepIfDue(uint64_t frame) { return (frame % kSweepEveryFrames == 0) ? cache_.Sweep(frame) : 0; }

 private:
  // The glass's centre and normal in the game's world: the patch's centroid through the draw's matrix (row vectors: world = (p, 1) M), its normal as a vector through the matrix's 3x3.
  static void WorldPose(const Candidate& c, Glass* g) {
    const double p[3] = {c.stats.centroid[0], c.stats.centroid[1], c.stats.centroid[2]};
    const double n[3] = {c.stats.normal[0], c.stats.normal[1], c.stats.normal[2]};
    double nw[3];
    for (int k = 0; k < 3; ++k) {
      g->centre_world[k] = p[0] * c.model_to_world.m[0][k] + p[1] * c.model_to_world.m[1][k] + p[2] * c.model_to_world.m[2][k] + c.model_to_world.m[3][k];
      nw[k] = n[0] * c.model_to_world.m[0][k] + n[1] * c.model_to_world.m[1][k] + n[2] * c.model_to_world.m[2][k];
    }
    const double length = std::sqrt(nw[0] * nw[0] + nw[1] * nw[1] + nw[2] * nw[2]);
    for (int k = 0; k < 3; ++k) {
      g->normal_world[k] = length > 1e-12 ? float(nw[k] / length) : 0.0f;
    }
  }

  GlassCache cache_;
  uint64_t frame_ = 0;
  uint32_t reads_ = 0;
  std::vector<Candidate> candidates_;
  std::vector<float> positions_;
  std::vector<uint32_t> indices_;
  std::vector<float> normals_;
  std::vector<cm::Candidate> placed_;
  std::vector<std::array<std::array<float, 4>, 3>> transforms_;
  std::vector<const Candidate*> sources_;
};

}  // namespace mcla::gpu::mirror_glass
