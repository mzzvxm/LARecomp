#pragma once
// ===========================================================================
// MCLA GPU renderer -- the player car's draws of a frame: the rules that need no renderer
// ===========================================================================
// car_frame.h collects the player's car every frame, on the render thread of a CPU-bound game. This header holds what needs no
// renderer, as pure functions on plain numbers, so that car_frame_rules_test.cpp builds on its own (car_dump_rules.h has the
// recipe of a draw, the bank's matrices, the skin blend and the triangle set that this one builds on):
//
//   - the mesh key: one number for "the same triangles in model space" (MeshKeyOf), made from where the position stream, the
//     index range and the declaration are, the draw's primitive and counts, and a fingerprint of the mesh's own guest bytes (the
//     indices and the vertices they name: SummarizeIndices, OwnVertices, ContentCache);
//   - which draw is the player's: the stencil ID a car's draws write is handed out per frame (a car has no fixed ID), so the
//     player's ID is the one whose gWorld is nearest to the player's vehicle in the game's memory, and the one it had in the
//     frame before stands in for the draws that come first (IdentityTracker);
//   - the matrix of a draw from its bank with only the registers its recipe reads (DecodeModel);
//   - the camera of the frame out of a car draw's vertex constants, in the occlusion's view space (CameraFromBank);
//   - the sun's direction, a normal as the declarations give it (NormalFormatOf, DecodeNormal), the blend of a skinned vertex's
//     normal (SkinBlendDirection);
//   - a model-space box of a mesh and its place in the world (BoxOfMesh, AddTransformedBox), and when a cached box is stale.
//
// Matrices are row vectors (v' = v M) as in temporal_math.h. This header includes nothing of the renderer.
// ===========================================================================

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <unordered_map>

#include "car_dump_rules.h"
#include "temporal_math.h"

namespace mcla::gpu::car_frame_rules {

// ---------------------------------------------------------------------------
// the mesh key
// ---------------------------------------------------------------------------

inline uint64_t Mix(uint64_t h, uint64_t v) {
  h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
  h *= 0xFF51AFD7ED558CCDull;
  h ^= h >> 32;
  return h;
}

// One vertex stream the triangles are read from, and the element in it.
struct StreamKey {
  uint32_t address = 0;  // physical, from the stream's vertex fetch constant (0: the element is in the position's stream)
  uint32_t size = 0;     // bytes of the fetch window
  uint32_t stride = 0;
  uint32_t offset = 0;   // of the element in a vertex
  uint32_t format = 0;   // the host's format code of the element (plume::RenderFormat)
  uint64_t content = 0;  // a fingerprint of the guest bytes of the vertices THIS draw uses (VertexContent); 0: not known
};

// What makes two draws give the same triangles in model space.
struct MeshKeyParts {
  StreamKey position;
  StreamKey normal;       // the NORMAL element's stream when it is another one; otherwise its offset and format only
  uint32_t indexed = 0;   // 1: indexed, 0: auto (inline draws have no key)
  uint32_t primitive = 0; // xenos::PrimitiveType
  uint32_t count = 0;
  uint32_t start = 0;
  int32_t base_vertex = 0;
  uint32_t index_address = 0;  // physical, with the draw's start offset in it
  uint32_t index_bytes = 0;
  uint32_t index32 = 0;
  uint32_t endian = 0;
  uint64_t index_content = 0;  // a fingerprint of the draw's own index bytes (SummarizeIndices); 0: not known
  uint32_t reset = 0;          // strips: the guest's primitive reset is on, and its index
  uint32_t reset_index = 0;
};

inline uint64_t MixStream(uint64_t h, const StreamKey& s) {
  h = Mix(h, (uint64_t(s.address) << 32) | s.size);
  h = Mix(h, (uint64_t(s.stride) << 32) | s.offset);
  h = Mix(h, s.format);
  return Mix(h, s.content);
}

// Never 0 (0 means "no key").
inline uint64_t MeshKeyOf(const MeshKeyParts& p) {
  uint64_t h = 0xCA4F12A3E5ull;
  h = MixStream(h, p.position);
  h = MixStream(h, p.normal);
  h = Mix(h, (uint64_t(p.indexed) << 32) | p.primitive);
  h = Mix(h, (uint64_t(p.count) << 32) | p.start);
  h = Mix(h, uint64_t(uint32_t(p.base_vertex)));
  h = Mix(h, (uint64_t(p.index_address) << 32) | p.index_bytes);
  h = Mix(h, (uint64_t(p.index32) << 32) | p.endian);
  h = Mix(h, p.index_content);
  h = Mix(h, (uint64_t(p.reset) << 32) | p.reset_index);
  return h != 0 ? h : 1;
}

// A cached box (or any per-key record) that was last seen more than `keep` frames ago is dropped.
inline bool Stale(uint64_t last_seen, uint64_t now, uint64_t keep) { return now > last_seen && now - last_seen > keep; }

// ---------------------------------------------------------------------------
// what a mesh's own bytes say
// ---------------------------------------------------------------------------
// The geometry cache's stamp of a guest range (GeometryCache::RangeStamp) says only that the range MAY have changed: it moves on any
// write the watch saw in a page the range touches (the range's neighbours in the page, rewritten), when the region goes from watched to
// hashed and back, and when the region is made again; a hashed region's stamp also carries the hash of the WHOLE fetch window. A key
// made from the stamp therefore changed about a dozen times a frame for a parked car (measured 2026-11-18: 56731 meshes built over
// 4700 frames for ~195 draws). The key is made from what the mesh IS instead: a fingerprint of the bytes of its own vertices and its
// own indices, made again only when the stamp has moved (ContentCache).
//
// The indices: SummarizeIndices reads the draw's index bytes once and gives the smallest and the largest index (which vertices of the
// stream are the mesh's own: OwnVertices) and a fingerprint. For a triangle list the fingerprint does not depend on the order of the
// triangles (a game that sorts a translucent part's triangles every frame leaves the same triangles), for a strip it does.

// A cheap hash of one triangle's three vertex numbers, in order (a rotation of the corners is another hash: the winding is part of it).
inline uint64_t TriangleHash(uint32_t a, uint32_t b, uint32_t c) {
  uint64_t h = ((uint64_t(a) << 32) | b) * 0x9E3779B97F4A7C15ull;
  h ^= (uint64_t(c) + 0x632BE59BD9B4E019ull) * 0xC2B2AE3D27D4EB4Full;
  h ^= h >> 29;
  h *= 0xBF58476D1CE4E5B9ull;
  h ^= h >> 32;
  return h;
}

struct IndexSummary {
  bool any = false;              // at least one index that is not a primitive reset
  uint32_t lo = 0;               // the smallest and the largest index of those (the guest's own numbers, without the base vertex)
  uint32_t hi = 0;
  uint64_t fingerprint = 0;      // never 0
};

// `count` guest indices at `raw` (the guest's bytes; 16 or 32 bit, swapped by `endian` as the renderer does). `order_free`: a triangle
// list, whose fingerprint is the sum of its triangles' hashes (the same triangles in any order give the same fingerprint, and a
// triangle that appears twice counts twice); otherwise the fingerprint follows the indices in order. `skip_reset`: an index equal to
// `reset_index` is a strip's cut, not a vertex (it stays in the order-bound fingerprint, and out of lo and hi).
inline IndexSummary SummarizeIndices(const uint8_t* raw, uint32_t count, bool index32, uint32_t endian, bool order_free, bool skip_reset,
                                     uint32_t reset_index) {
  // A block is a multiple of 3 (whole triangles) and of 2 (the 8in32 and 16in32 swaps work on dwords, 16-bit indices come in pairs).
  constexpr uint32_t kBlock = 96;
  const uint32_t size = index32 ? 4u : 2u;
  uint8_t swapped[kBlock * 4];
  IndexSummary out;
  uint64_t sum = 0;
  uint64_t chain = 0x1DC0FFEE5ull;
  uint32_t triangles = 0;
  uint32_t lo = 0xFFFFFFFFu, hi = 0;
  for (uint32_t done = 0; done < count; done += kBlock) {
    const uint32_t n = std::min(kBlock, count - done);
    std::memset(swapped, 0, size_t(n) * size);
    car_dump::SwapEndian(swapped, raw + size_t(done) * size, n * size, endian);
    uint32_t v[kBlock];
    for (uint32_t i = 0; i < n; ++i) {
      if (index32) {
        uint32_t x;
        std::memcpy(&x, swapped + size_t(i) * 4, 4);
        v[i] = x;
      } else {
        uint16_t x;
        std::memcpy(&x, swapped + size_t(i) * 2, 2);
        v[i] = x;
      }
      if (!(skip_reset && v[i] == reset_index)) {
        lo = std::min(lo, v[i]);
        hi = std::max(hi, v[i]);
        out.any = true;
      }
    }
    if (order_free) {
      for (uint32_t i = 0; i + 3 <= n; i += 3) {
        sum += TriangleHash(v[i], v[i + 1], v[i + 2]);
        ++triangles;
      }
    } else {
      for (uint32_t i = 0; i < n; ++i) {
        chain = Mix(chain, v[i]);
      }
    }
  }
  if (out.any) {
    out.lo = lo;
    out.hi = hi;
  }
  uint64_t h = order_free ? Mix(sum, (uint64_t(triangles) << 1) | 1u) : Mix(chain, uint64_t(count) << 1);
  h = Mix(h, (uint64_t(index32 ? 1u : 0u) << 1) | (order_free ? 1u : 0u));
  out.fingerprint = h != 0 ? h : 1;
  return out;
}

// The vertices of a stream that a draw uses: `count` vertices from vertex `first` of the stream's window. `whole`: not known (hash the
// window).
struct VertexSpan {
  bool whole = true;
  uint32_t first = 0;
  uint32_t count = 0;  // 0 with whole == false: the draw names nothing the window holds
};

// An indexed draw uses the vertices lo + base_vertex .. hi + base_vertex (`have_indices`: SummarizeIndices found some); an auto draw uses
// `start` .. start + draw_count - 1. Both are clipped to the window's `window_vertices` (the renderer's draw reads past it as zeros).
inline VertexSpan OwnVertices(bool indexed, uint32_t start, uint32_t draw_count, int32_t base_vertex, bool have_indices, uint32_t lo,
                              uint32_t hi, uint32_t window_vertices) {
  VertexSpan out;
  int64_t first = 0, last = -1;
  if (indexed) {
    if (!have_indices) {
      return out;  // an index buffer with nothing but resets, or one that is not read: the whole window
    }
    first = int64_t(lo) + int64_t(base_vertex);
    last = int64_t(hi) + int64_t(base_vertex);
  } else {
    if (draw_count == 0) {
      return out;
    }
    first = int64_t(start);
    last = int64_t(start) + int64_t(draw_count) - 1;
  }
  out.whole = false;
  first = std::max<int64_t>(first, 0);
  last = std::min<int64_t>(last, int64_t(window_vertices) - 1);
  if (first <= last) {
    out.first = uint32_t(first);
    out.count = uint32_t(last - first + 1);
  }
  return out;
}

// The fingerprint of a stream's own vertices from the hash of their bytes. Never 0 (0 means "not known").
inline uint64_t VertexContent(uint64_t bytes_hash, const VertexSpan& span) {
  uint64_t h = Mix(bytes_hash, (uint64_t(span.first) << 32) | span.count);
  h = Mix(h, span.whole ? 1u : 0u);
  return h != 0 ? h : 1;
}

// Where a fingerprint is kept: the range it was made from and how it was read.
inline uint64_t IndexSlot(uint32_t address, uint32_t bytes, bool index32, uint32_t endian, bool order_free, bool skip_reset,
                          uint32_t reset_index) {
  uint64_t h = Mix(0x1D5EC0DE1ull, (uint64_t(address) << 32) | bytes);
  h = Mix(h, (uint64_t(endian) << 8) | (index32 ? 4u : 0u) | (order_free ? 2u : 0u) | (skip_reset ? 1u : 0u));
  return Mix(h, skip_reset ? reset_index : 0u);
}

inline uint64_t VertexSlot(uint32_t address, uint32_t size, uint32_t stride, const VertexSpan& span) {
  uint64_t h = Mix(0x7E47E8ull, (uint64_t(address) << 32) | size);
  h = Mix(h, (uint64_t(stride) << 32) | span.first);
  return Mix(h, (uint64_t(span.count) << 1) | (span.whole ? 1u : 0u));
}

// The fingerprints made so far, by slot (IndexSlot, VertexSlot), each with the stamp of the range it was made at: Get returns it while
// the stamp is the same and makes it again (compute) when the stamp has moved or the slot is new. The stamp is only a "may have changed"
// signal, so a made-again fingerprint tells which it was: kSame (the stamp moved, the bytes did not: the mesh keeps its key), kChanged.
struct ContentEntry {
  uint64_t stamp = 0;
  uint64_t content = 0;
  bool has_range = false;  // index entries: lo and hi hold the smallest and largest index
  uint32_t lo = 0;
  uint32_t hi = 0;
  uint64_t seen = 0;       // the frame it was last asked for
};

class ContentCache {
 public:
  enum class Outcome : uint8_t {
    kHit,      // the stamp is the one the fingerprint was made at: nothing read
    kNew,      // a slot not seen before (or since it was dropped): made
    kSame,     // the stamp moved, the bytes did not: made again, the same fingerprint
    kChanged,  // the stamp moved and the bytes did too
  };

  // `compute(ContentEntry*)` fills content, has_range, lo and hi; Get sets stamp and seen. Returns a copy.
  template <typename Compute>
  ContentEntry Get(uint64_t slot, uint64_t stamp, uint64_t frame, Compute&& compute, Outcome* outcome) {
    const auto found = entries_.find(slot);
    if (found != entries_.end() && found->second.stamp == stamp) {
      found->second.seen = frame;
      if (outcome) {
        *outcome = Outcome::kHit;
      }
      return found->second;
    }
    ContentEntry made;
    compute(&made);
    made.stamp = stamp;
    made.seen = frame;
    if (outcome) {
      *outcome = found == entries_.end() ? Outcome::kNew : (found->second.content == made.content ? Outcome::kSame : Outcome::kChanged);
    }
    if (found != entries_.end()) {
      found->second = made;
    } else {
      entries_.emplace(slot, made);
    }
    return made;
  }

  // Drops the entries last asked for more than `keep` frames before `now`.
  void Sweep(uint64_t now, uint64_t keep) {
    for (auto it = entries_.begin(); it != entries_.end();) {
      if (Stale(it->second.seen, now, keep)) {
        it = entries_.erase(it);
      } else {
        ++it;
      }
    }
  }
  void Clear() { entries_.clear(); }
  size_t size() const { return entries_.size(); }

 private:
  std::unordered_map<uint64_t, ContentEntry> entries_;
};

// ---------------------------------------------------------------------------
// which draw is the player's
// ---------------------------------------------------------------------------
// The draws of a car write a stencil ID (1-23) that is handed out per frame in drawing order. The player's car is the ID whose
// gWorld (the vehicle's root, row 3 of the draw's c0-c3) is the nearest to the player's vehicle in the game's own memory,
// within car_dump::kPlayerMatchMetres (the render thread reads a state a frame or a few after the draws); with the nearest ID
// kept as the frame goes. A draw of the right car whose own gWorld is not at the root (a tyre, the driver) is told by the
// ID alone, and before the frame's first draw near the vehicle the ID the player had in the frame before stands in.
// CHECK: the render thread reads the vehicle some frames after the draws were made, so while driving fast the distance of the player's own
// root draw is not 0 (the dump measured 0.00 m parked); 5 m holds at about 250 km/h and 3 frames of lead. The car frame's log line
// (mcla_gpu_car_frame_log) says the distance each frame: drive at speed and read it, and with a racer alongside check the ID stays right.
class IdentityTracker {
 public:
  static constexpr uint32_t kSlots = car_dump::kIdSlots;
  enum class Standing : uint8_t {
    kOther,      // not the player's (as far as the frame knows)
    kTentative,  // the ID of the frame before, nothing near the vehicle seen yet in this frame
    kConfirmed,  // the ID of a draw whose gWorld is near the vehicle
  };
  struct Verdict {
    Standing standing = Standing::kOther;
    bool confirmed_now = false;  // this draw gave the frame its first confirmed ID
    bool switched = false;       // ... or moved the confirmed ID from another one to this draw's (a nearer match)
    bool improved = false;       // this draw is the nearest its ID has had to the vehicle in this frame
    double distance = std::numeric_limits<double>::infinity();
  };

  IdentityTracker() { best_.fill(std::numeric_limits<double>::infinity()); }

  // `previous_id`: the player's ID of the frame before (0: none known).
  void BeginFrame(uint32_t previous_id) {
    previous_ = previous_id < kSlots ? previous_id : 0;
    confirmed_ = 0;
    best_.fill(std::numeric_limits<double>::infinity());
  }

  // A draw of stencil ID `id` whose gWorld translation is `translation` (null: not a finite number); `player` is the player's
  // vehicle position in the game's memory (null: not known: nothing is ever confirmed).
  Verdict OnDraw(uint32_t id, const double* translation, const double* player) {
    Verdict v;
    if (id == 0 || id >= kSlots) {
      return v;
    }
    if (translation && player) {
      const double dx = translation[0] - player[0], dy = translation[1] - player[1], dz = translation[2] - player[2];
      const double d = std::sqrt(dx * dx + dy * dy + dz * dz);
      v.distance = d;
      if (std::isfinite(d) && d <= car_dump::kPlayerMatchMetres && d < best_[id]) {
        best_[id] = d;
        v.improved = true;
        if (confirmed_ == 0) {
          confirmed_ = id;
          v.confirmed_now = true;
        } else if (confirmed_ != id && d < best_[confirmed_]) {
          confirmed_ = id;
          v.switched = true;
        }
      }
    }
    const uint32_t assumed = assumed_id();
    if (id == assumed) {
      v.standing = confirmed_ != 0 ? Standing::kConfirmed : Standing::kTentative;
    }
    return v;
  }

  uint32_t confirmed() const { return confirmed_; }
  uint32_t previous() const { return previous_; }
  // The ID draws are taken for: the confirmed one, else the previous frame's.
  uint32_t assumed_id() const { return confirmed_ != 0 ? confirmed_ : previous_; }
  double best(uint32_t id) const { return id < kSlots ? best_[id] : std::numeric_limits<double>::infinity(); }

 private:
  uint32_t previous_ = 0;
  uint32_t confirmed_ = 0;
  std::array<double, kSlots> best_{};
};

// ---------------------------------------------------------------------------
// the matrix of a draw
// ---------------------------------------------------------------------------

struct ModelDecode {
  bool finite = false;      // what was read is finite
  bool bone_valid = false;  // kPalette: c204.x names a bone of the palette
  uint32_t bone = 0;
  temporal::Mat4 world;     // gWorld, c0-c3
  temporal::Mat4 model;     // the recipe's model-to-world (car_dump::ModelMatrix): palette(bone) then gWorld, else gWorld
};

// The same model matrix as car_dump::DecodeCarMatrices + ModelMatrix, from the registers the recipe reads only (gWorld; for the
// palette recipe c204.x and the bone's three rows; not gWorldViewProj).
inline ModelDecode DecodeModel(const uint8_t* vs_bank, car_dump::Recipe recipe) {
  ModelDecode out;
  out.world = car_dump::BankMatrix(vs_bank, car_dump::kWorldRegister);
  out.model = out.world;
  out.finite = temporal::Finite(out.world);
  if (recipe == car_dump::Recipe::kPalette) {
    const float raw = car_dump::BankFloat(vs_bank, car_dump::kBoneIndexRegister, 0);
    if (std::isfinite(raw) && raw >= 0.0f && raw < float(car_dump::kPaletteBones)) {
      out.bone = uint32_t(raw);
      out.bone_valid = true;
      const temporal::Mat4 palette = car_dump::BoneMatrix(vs_bank, out.bone);
      out.finite = out.finite && temporal::Finite(palette);
      out.model = temporal::Mul(palette, out.world);
    }
  }
  return out;
}

// gWorld's translation (row 3 of c0-c3), read without the rest; false when it is not finite.
inline bool WorldTranslation(const uint8_t* vs_bank, double out[3]) {
  for (uint32_t c = 0; c < 3; ++c) {
    out[c] = double(car_dump::BankFloat(vs_bank, car_dump::kWorldRegister + 3, c));
  }
  return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
}

// ---------------------------------------------------------------------------
// the camera of the frame, from a car draw's vertex constants
// ---------------------------------------------------------------------------
// VS c12-c15 hold the view inverse (c12 right, c13 up, c14 BACKWARD, c15 the position: the camera looks along -c14), VS c16
// the direction the sunlight travels (at noon y is -0.867), and gWorldViewProj c8-c11 is gWorld times the camera's view-projection
// (the car dump measured it to 2.6e-5 in every draw). The occlusion's view space (ao_common.hlsli AoViewPos: x right, y up, z =
// the linear depth t) is the game's view with z negated (ao.cpp: `view[2] = -view[2]`): AoView below negates column 2.
// CHECK: that is read off ao.cpp and ao_common.hlsli, not run: in a run, take a pixel of the depth image (t), its AoViewPos and a car vertex
// through `view` and compare, or compare `Frame.view` with ao.cpp's `previous_.view` with z negated. That every car draw's c12-c15 and c16 hold
// this frame's view inverse and sun (they are shared constants a draw whose shader does not read them leaves as they were) is the occlusion's
// own assumption too (ao.cpp OnMainDraw, vs_pose_trusted_); the car frame takes them from the first draw of the player's car with a view
// inverse that is one (LooksLikeViewInverse).

inline constexpr uint32_t kViewInverseRegister = 12;
inline constexpr uint32_t kSunRegister = 16;

struct BankCamera {
  bool valid = false;               // view and view_projection were made
  temporal::Mat4 view;              // world -> the occlusion's view space (z forward)
  temporal::Mat4 view_projection;   // world -> clip: gWorld^-1 * gWorldViewProj
  bool sun_valid = false;
  double to_sun[3] = {};            // unit vector toward the sun in the world
};

// The view inverse of the game (rows 0-2 the axes with w 0, row 3 the position with w 1): the last column must say so.
inline bool LooksLikeViewInverse(const temporal::Mat4& m) {
  if (!temporal::Finite(m)) {
    return false;
  }
  return std::fabs(m.m[0][3]) < 1e-3 && std::fabs(m.m[1][3]) < 1e-3 && std::fabs(m.m[2][3]) < 1e-3 && std::fabs(m.m[3][3] - 1.0) < 1e-3;
}

// The game's world -> view matrix as the occlusion's: the same with z negated.
inline temporal::Mat4 AoView(const temporal::Mat4& game_view) {
  temporal::Mat4 out = game_view;
  for (int r = 0; r < 4; ++r) {
    out.m[r][2] = -out.m[r][2];
  }
  return out;
}

inline BankCamera CameraFromBank(const uint8_t* vs_bank, const temporal::Mat4& world, const temporal::Mat4& wvp) {
  BankCamera out;
  const temporal::Mat4 view_inverse = car_dump::BankMatrix(vs_bank, kViewInverseRegister);
  temporal::Mat4 game_view, world_inverse;
  if (LooksLikeViewInverse(view_inverse) && temporal::Inverse(view_inverse, &game_view) && temporal::Inverse(world, &world_inverse) &&
      temporal::Finite(wvp)) {
    out.view = AoView(game_view);
    out.view_projection = temporal::Mul(world_inverse, wvp);
    out.valid = temporal::Finite(out.view) && temporal::Finite(out.view_projection);
  }
  double dir[3];
  for (uint32_t c = 0; c < 3; ++c) {
    dir[c] = double(car_dump::BankFloat(vs_bank, kSunRegister, c));
  }
  const double length = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
  if (std::isfinite(length) && length > 1e-6) {
    for (int c = 0; c < 3; ++c) {
      out.to_sun[c] = -dir[c] / length;
    }
    out.sun_valid = true;
  }
  return out;
}

// ---------------------------------------------------------------------------
// normals
// ---------------------------------------------------------------------------
// The NORMAL element of a car's declaration: three or four floats, four halves, or the packed dword the translated shaders unpack
// themselves (tfetchR11G11B10 with the spec constant R11G11B10_NORMAL: three signed 10-bit fields at bits 0, 10 and 20, over 511).
// CHECK: the packed decode follows the translated shader's text (native/work/hlsl-head/cars/*.hlsl tfetchR11G11B10), not a car's data: in a run
// compare these normals with the triangles' own (the dot of a decoded normal and the face normal of its triangle is mostly positive).

enum class NormalFormat : uint8_t { kNone, kFloat3, kFloat4, kHalf4, kPacked };

inline NormalFormat NormalFormatOf(uint32_t packed_decltype) {
  switch (packed_decltype) {
    case 0x002A23B9: return NormalFormat::kFloat3;  // k_32_32_32_FLOAT
    case 0x001A23A6: return NormalFormat::kFloat4;  // k_32_32_32_32_FLOAT
    case 0x001A2360: return NormalFormat::kHalf4;   // k_16_16_16_16_FLOAT
    case 0x001A2187: return NormalFormat::kPacked;  // k_2_10_10_10
    default: return NormalFormat::kNone;
  }
}

inline uint32_t NormalBytes(NormalFormat format) {
  switch (format) {
    case NormalFormat::kFloat3: return 12;
    case NormalFormat::kFloat4: return 16;
    case NormalFormat::kHalf4: return 8;
    case NormalFormat::kPacked: return 4;
    default: return 0;
  }
}

inline float Signed10(uint32_t bits) {
  const int32_t v = int32_t(bits & 0x3FFu);
  return float(v - ((v & 0x200) << 1)) / 511.0f;
}

// The normal at `be` (the guest's raw bytes of the element) as x y z; the guest's own numbers, not normalised.
inline void DecodeNormal(const uint8_t* be, NormalFormat format, float out[3]) {
  out[0] = out[1] = out[2] = 0.0f;
  switch (format) {
    case NormalFormat::kFloat3:
    case NormalFormat::kFloat4:
      for (uint32_t i = 0; i < 3; ++i) {
        const uint32_t bits = car_dump::LoadBe32(be + 4 * i);
        std::memcpy(&out[i], &bits, 4);
      }
      break;
    case NormalFormat::kHalf4:
      for (uint32_t i = 0; i < 3; ++i) {
        out[i] = car_dump::HalfToFloat(car_dump::LoadBe16(be + 2 * i));
      }
      break;
    case NormalFormat::kPacked: {
      const uint32_t v = car_dump::LoadBe32(be);
      out[0] = Signed10(v);
      out[1] = Signed10(v >> 10);
      out[2] = Signed10(v >> 20);
      break;
    }
    default:
      break;
  }
}

// The blend of a skinned vertex's direction (a normal): the sum of weight * (the rotation part of the bone's rows applied to it),
// the counterpart of car_dump::SkinBlend for a position (no translation).
inline void SkinBlendDirection(const car_dump::SkinPalette& palette, const car_dump::SkinInfluence& skin, const float n[3], float out[3]) {
  double sum[3] = {0.0, 0.0, 0.0};
  for (int k = 0; k < 4; ++k) {
    const double w = double(skin.weight[k]);
    if (skin.bone[k] == car_dump::kNoBone || w == 0.0) {
      continue;
    }
    const float(*rows)[4] = palette.rows[skin.bone[k]];
    for (int r = 0; r < 3; ++r) {
      sum[r] += w * (double(rows[r][0]) * double(n[0]) + double(rows[r][1]) * double(n[1]) + double(rows[r][2]) * double(n[2]));
    }
  }
  out[0] = float(sum[0]);
  out[1] = float(sum[1]);
  out[2] = float(sum[2]);
}

// ---------------------------------------------------------------------------
// boxes
// ---------------------------------------------------------------------------

struct Box3 {
  double lo[3] = {std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity()};
  double hi[3] = {-std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
                  -std::numeric_limits<double>::infinity()};
  bool Empty() const { return !(lo[0] <= hi[0] && lo[1] <= hi[1] && lo[2] <= hi[2]); }
  void Add(const double p[3]) {
    for (int c = 0; c < 3; ++c) {
      lo[c] = std::min(lo[c], p[c]);
      hi[c] = std::max(hi[c], p[c]);
    }
  }
};

// The box of the vertices a triangle list names (`positions` x y z a vertex, `tri` three vertex numbers a triangle): false when no
// finite vertex is named.
inline bool BoxOfMesh(const float* positions, size_t vertex_count, const uint32_t* tri, size_t index_count, float lo[3], float hi[3]) {
  Box3 box;
  for (size_t i = 0; i < index_count; ++i) {
    const uint32_t v = tri[i];
    if (v >= vertex_count) {
      continue;
    }
    const double p[3] = {double(positions[size_t(v) * 3]), double(positions[size_t(v) * 3 + 1]), double(positions[size_t(v) * 3 + 2])};
    if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) {
      continue;
    }
    box.Add(p);
  }
  if (box.Empty()) {
    return false;
  }
  for (int c = 0; c < 3; ++c) {
    lo[c] = float(box.lo[c]);
    hi[c] = float(box.hi[c]);
  }
  return true;
}

// The model-space box [lo, hi] through `m` (model to world), as the box of its eight corners, added to `box`.
inline void AddTransformedBox(Box3* box, const temporal::Mat4& m, const float lo[3], const float hi[3]) {
  for (int corner = 0; corner < 8; ++corner) {
    const double p[3] = {double((corner & 1) ? hi[0] : lo[0]), double((corner & 2) ? hi[1] : lo[1]), double((corner & 4) ? hi[2] : lo[2])};
    double w[4];
    car_dump::TransformPoint(m, p, w);
    const double world[3] = {w[0], w[1], w[2]};
    if (std::isfinite(world[0]) && std::isfinite(world[1]) && std::isfinite(world[2])) {
      box->Add(world);
    }
  }
}

}  // namespace mcla::gpu::car_frame_rules
