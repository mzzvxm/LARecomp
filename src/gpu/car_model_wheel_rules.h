#pragma once
// ===========================================================================
// MCLA GPU renderer -- a car's wheels from the game's own model data: the walk of the wheel manager's tyre and rim, and where each wheel stands (RT-MODEL-PARTS)
// ===========================================================================
// RT-MODEL-CARS (phases 1 to 3) built the body and the model set's parts of a car the camera does not draw. The four wheels are not in the model set: the wheel draw
// (Vehicle's sub_82359F58, read from the recompiled code for this file, asm in h11-work/rt-model-parts/asm/) takes them from a manager the game keeps for every car
// (the global [0x8288E010]) by two ids the vehicle holds, and draws each wheel with matrices the vehicle holds. This header is that chain as pure functions over a memory
// reader (the `Mem` of car_model_walk_rules.h); car_model_wheel_rules_test.cpp feeds it made-up bytes.
//
// THE WHEEL MANAGER W = [0x8288E010]   (asm sub_82374720 / 82374788 / 823747F0 / 82374858)
//   W+0   i32   the number of tyres;       W+4   i32 the number of rims
//   tyre id t (the vehicle's i32 at V+5964), valid when 0 <= t < W+0 and
//     [W+40][t]  (i32 array) > 0                the entry is in use (a count)
//     [W+36][t]  (i32 array) == 0               it has no error
//     the tyre's entry  = [W+28] + 16 t : +0 the DRAWABLE, +8 flags ((flags & 0x30000000) == 0x30000000: placed)
//     the tyre's PACK   = [W+32] + 16 t : +0 the texture / material pack the draw takes the shader group from (+12); the draw needs both
//   rim id r (the vehicle's i32 at V+5968), valid when 0 <= r < W+4 and
//     [W+52][r] > 0, [W+56][r] == 0
//     the rim's entry   = [W+44] + 16 r ;      the rim's PACK = [W+48] + 16 r
//   The drawable is the same object as a part's: D+8 -> the LOD object L -> models -> geometries (the draw takes L = [D+8], no other lod: a wheel resource has body_lod_0
//   only, modloader.cpp "resources/rims/<name>/body_lod_0.xrsc"). A wheel resource is shared by every car that wears it, so the geometries' buffers (and the BLAS the scene
//   builds from them) are shared too.
//
// THE FOUR WHEELS' MATRICES (asm 0x8235A120 ff.; V = the vehicle; each a Matrix44, row vectors, row 3 = the translation, in WORLD space: the draw compares row 3 with the
// eye to pick the side)
//   V+1344 + 64 w   the TYRE's matrix: the draw copies it into the tyre drawable's own matrix store (sub_82323250: [[D+40]+20], count * 64 bytes, count = [[D+40]+16]), builds the
//                   palette from it (sub_823236C8: world = matrix[bone], the same as for the model set's parts) and draws. The tyre is rubber: no brake or caliper bone.
//   V+1600 + 64 w   the RIM's matrix (bone 0 of the rim drawable). The draw passes the rim an ARRAY of matrices that it builds on its stack from three of the vehicle's:
//                     A1 = [ V+1600+64w , V+1856+128w ]     (stack +160..+287)   the usual case
//                     A2 = [ V+1600+64w , V+1920+128w ]     (stack +288..+415)   the other side's brake, when [[V+20]+76 ...] says so
//                   so bone 0 of the rim is the wheel itself and bone 1 (the brake disc, xWheelBrakeRotor) is the second matrix.
//   V+440 + 36 w    a float per wheel (|x| goes to sub_82377D98, the blur's strength): the wheel's spin rate; the spin is IN the matrices above (the draw has no other source),
//                   so a placement by them has the wheel's steer and its turn and its suspension of the last update, as the draw has.
//   V+4192 + 4w / V+4208 + 4w   a pointer per wheel (the update copy / the render copy: the byte at [[r13]+72] picks); a null pointer: that wheel is not drawn
//   the draw's own gates: returns at once when the object at V+1060 has a non-zero byte at +268, +267 or +266 (the wheels are hidden), or when the eye is farther than
//   a constant from the car; the tyre is drawn when V+589 is set, the rim when V+590 is set and V+6477 is clear.
//
// WHAT IS NOT VERIFIED (nothing here has run): the four placement hypotheses below are what the asm says; the walker CHECKS them against the draws of the cars the camera
// sees (the player's own wheels at least: car_model_parts_rt_rules.h WheelCalibration) and places no wheel of a car the camera does not draw until one fits within 0.35 m.
// CHECK: whether V+1344 is the tyre's and V+1600 the rim's in every car (the survey's earlier reading of the wheel draw said V+1600 for all); the meaning of V+4192 / V+4208;
// that the rim drawable has two bones.
// ===========================================================================

#include <array>
#include <cstdint>

#include "car_model_walk_rules.h"
#include "temporal_math.h"

namespace mcla::gpu::car_model_walk {

inline constexpr uint32_t kWheelManagerGlobal = 0x8288E010u;
inline constexpr uint32_t kManagerTyreCount = 0;
inline constexpr uint32_t kManagerRimCount = 4;
inline constexpr uint32_t kManagerTyreEntries = 28;   // pointer to 16-byte entries
inline constexpr uint32_t kManagerTyrePacks = 32;
inline constexpr uint32_t kManagerTyreErrors = 36;    // i32 per tyre id
inline constexpr uint32_t kManagerTyreUses = 40;
inline constexpr uint32_t kManagerRimEntries = 44;
inline constexpr uint32_t kManagerRimPacks = 48;
inline constexpr uint32_t kManagerRimUses = 52;
inline constexpr uint32_t kManagerRimErrors = 56;
inline constexpr uint32_t kManagerEntryBytes = 16;

inline constexpr uint32_t kVehicleTyreId = 5964;
inline constexpr uint32_t kVehicleRimId = 5968;
inline constexpr uint32_t kVehicleTyreOn = 589;       // byte
inline constexpr uint32_t kVehicleRimOn = 590;        // byte; the rim also needs V+6477 (kVehicleLoop44A) clear
inline constexpr uint32_t kVehicleWheelHide = 1060;   // -> an object: bytes +266, +267, +268
inline constexpr uint32_t kVehicleTyreMatrices = 1344;   // + 64 w
inline constexpr uint32_t kVehicleBrakeMatrices = 1856;  // + 128 w: the rim's second bone (A1); + 64 more: A2's
inline constexpr uint32_t kVehicleWheelPointers = 4192;  // + 16 * copy + 4 w, copy 0 = the update copy, 1 = the render copy
inline constexpr uint32_t kVehicleWheelSpin = 440;       // + 36 w, a float

// ---------------------------------------------------------------------------
// the walk
// ---------------------------------------------------------------------------

namespace detail {

// One resource of the manager (a tyre or a rim): the entry's drawable and the pack, by the game's accessors (the four functions above). `uses` / `errors` are the
// manager's arrays, `entries` / `packs` its pointers to 16-byte entries.
template <class Mem>
bool WheelResource(const Mem& mem, uint32_t manager, int32_t id, uint32_t count_offset, uint32_t uses_offset, uint32_t errors_offset, uint32_t entries_offset, uint32_t packs_offset,
                   uint32_t* entry_out, uint32_t* drawable_out, uint32_t* pack_out) {
  *entry_out = *drawable_out = *pack_out = 0;
  uint32_t count = 0;
  if (id < 0 || !mem.Word(manager + count_offset, &count) || id >= int32_t(count)) {
    return false;
  }
  uint32_t uses_array = 0, errors_array = 0, entries = 0, packs = 0;
  if (!mem.Word(manager + uses_offset, &uses_array) || !mem.Word(manager + errors_offset, &errors_array) || !mem.Word(manager + entries_offset, &entries) ||
      !mem.Word(manager + packs_offset, &packs) || !Plausible(uses_array) || !Plausible(errors_array) || !Plausible(entries) || !Plausible(packs)) {
    return false;
  }
  uint32_t uses = 0, errors = 0;
  if (!mem.Word(uses_array + 4u * uint32_t(id), &uses) || int32_t(uses) <= 0 || !mem.Word(errors_array + 4u * uint32_t(id), &errors) || errors != 0) {
    return false;
  }
  const uint32_t entry = entries + kManagerEntryBytes * uint32_t(id);
  const uint32_t pack_entry = packs + kManagerEntryBytes * uint32_t(id);
  uint32_t flags = 0, pack_flags = 0, drawable = 0, pack = 0;
  if (!mem.Word(entry + kEntryFlags, &flags) || (flags & kPlacedMask) != kPlacedMask || !mem.Word(entry + kEntryDrawable, &drawable) || !Plausible(drawable)) {
    return false;
  }
  // The draw needs the pack as well (a null pack: no wheel); the pack's own placed bits are the accessor's test too.
  if (!mem.Word(pack_entry + kEntryFlags, &pack_flags) || (pack_flags & kPlacedMask) != kPlacedMask || !mem.Word(pack_entry + kEntryDrawable, &pack) || !Plausible(pack)) {
    return false;
  }
  *entry_out = entry;
  *drawable_out = drawable;
  *pack_out = pack;
  return true;
}

}  // namespace detail

// Reads the vehicle's wheel state and appends the tyre's and the rim's geometries to `out` (the parts kTyrePart and kRimPart; in use when the wheel draw's own flag for
// them is set and the wheels are not hidden). Writes nothing in the guest. Called by WalkVehicle.
template <class Mem>
void WalkWheels(const Mem& mem, uint32_t vehicle, const Options& options, CarWalk* out) {
  WheelWalk& w = out->wheels;
  w = WheelWalk{};
  uint32_t manager = 0, value = 0;
  if (!mem.Word(kWheelManagerGlobal, &manager) || !Plausible(manager)) {
    return;
  }
  w.manager = manager;
  if (!mem.Word(vehicle + kVehicleTyreId, &value)) {
    return;
  }
  w.tyre_id = int32_t(value);
  if (!mem.Word(vehicle + kVehicleRimId, &value)) {
    return;
  }
  w.rim_id = int32_t(value);
  w.read = true;

  // the wheel draw's flags
  uint32_t b = 0;
  w.tyre_on = mem.Byte(vehicle + kVehicleTyreOn, &b) && b != 0;
  const bool rim_flag = mem.Byte(vehicle + kVehicleRimOn, &b) && b != 0;
  const bool rim_off = mem.Byte(vehicle + kVehicleLoop44A, &b) && b != 0;
  w.rim_on = rim_flag && !rim_off;
  {
    uint32_t hide = 0;
    if (mem.Word(vehicle + kVehicleWheelHide, &hide) && Plausible(hide)) {
      uint32_t b266 = 0, b267 = 0, b268 = 0;
      mem.Byte(hide + 266, &b266);
      mem.Byte(hide + 267, &b267);
      mem.Byte(hide + 268, &b268);
      w.hidden = b266 != 0 || b267 != 0 || b268 != 0;
    }
  }
  for (uint32_t i = 0; i < kWheelCount; ++i) {
    uint32_t update = 0, render = 0;
    mem.Word(vehicle + kVehicleWheelPointers + 4u * i, &update);
    mem.Word(vehicle + kVehicleWheelPointers + 16u + 4u * i, &render);
    w.present[i] = update != 0 || render != 0;
  }

  uint32_t tyre_pack = 0, rim_pack = 0;
  w.tyre_ok = detail::WheelResource(mem, manager, w.tyre_id, kManagerTyreCount, kManagerTyreUses, kManagerTyreErrors, kManagerTyreEntries, kManagerTyrePacks, &w.tyre_entry,
                                    &w.tyre_drawable, &tyre_pack);
  w.rim_ok = detail::WheelResource(mem, manager, w.rim_id, kManagerRimCount, kManagerRimUses, kManagerRimErrors, kManagerRimEntries, kManagerRimPacks, &w.rim_entry,
                                   &w.rim_drawable, &rim_pack);

  struct Resource {
    bool ok;
    uint32_t part, entry, drawable;
    int32_t id;
    bool on;
  };
  const Resource resources[2] = {{w.tyre_ok, kTyrePart, w.tyre_entry, w.tyre_drawable, w.tyre_id, w.tyre_on},
                                 {w.rim_ok, kRimPart, w.rim_entry, w.rim_drawable, w.rim_id, w.rim_on}};
  for (const Resource& r : resources) {
    if (!r.ok) {
      continue;
    }
    PartEntry entry;
    entry.part = uint16_t(r.part);
    entry.variant = 0;
    entry.lod = 0;
    entry.entry = r.entry;
    entry.drawable = r.drawable;
    entry.state = kStateLoaded;
    entry.count = r.id;
    entry.loaded = true;
    entry.placed = true;
    entry.valid = true;
    // In use by the draw's own flags. The wheels' pointers (V+4192 / V+4208) say WHICH wheels are drawn (the placement skips a wheel whose pointer is null once any is set), not
    // whether the resource is: their meaning is a CHECK, so a car whose pointers all read zero still has its wheels.
    entry.in_use = r.on && !w.hidden;
    uint32_t flags = 0;
    mem.Word(r.entry + kEntryFlags, &flags);
    entry.flags = flags;
    uint32_t lod_object = 0;
    if (!mem.Word(r.drawable + kDrawableLod, &lod_object) || !Plausible(lod_object)) {
      out->parts.push_back(entry);
      continue;
    }
    entry.lod_object = lod_object;
    if (const uint8_t* l = mem.Window(lod_object, 20)) {
      entry.models = Be16(l + kLodModelCount);
      entry.bucket_mask = Be32(l + kLodBucketMask);
    }
    const uint32_t entry_index = uint32_t(out->parts.size());
    entry.first_geometry = uint32_t(out->geometries.size());
    entry.geometries = detail::WalkLod(mem, lod_object, entry, entry_index, out);
    // A wheel drawable with no bone table has one bone: the wheel's own (the draw hands it one matrix).
    for (uint32_t i = 0; i < entry.geometries && entry.first_geometry + i < out->geometries.size(); ++i) {
      Geometry& g = out->geometries[entry.first_geometry + i];
      if (g.bone == kNoBone) {
        g.bone = 0;
      }
    }
    (r.part == kTyrePart ? w.tyre_geometries : w.rim_geometries) = entry.geometries;
    (r.part == kTyrePart ? w.tyre_entry_index : w.rim_entry_index) = entry_index;
    ++out->entries_present;
    ++out->entries_loaded;
    ++out->entries_valid;
    out->parts.push_back(entry);
    if (out->truncated) {
      break;
    }
  }
  (void)options;
}

// ---------------------------------------------------------------------------
// where the wheels stand
// ---------------------------------------------------------------------------

// The matrices one vehicle holds for its wheels, as the guest has them this frame (the walker reads them in the draw's call).
struct WheelMatrices {
  bool tyre_ok[kWheelCount] = {}, rim_ok[kWheelCount] = {}, aux1_ok[kWheelCount] = {}, aux2_ok[kWheelCount] = {};
  temporal::Mat4 tyre[kWheelCount], rim[kWheelCount], aux1[kWheelCount], aux2[kWheelCount];
  bool any() const {
    for (uint32_t i = 0; i < kWheelCount; ++i) {
      if (tyre_ok[i] || rim_ok[i]) {
        return true;
      }
    }
    return false;
  }
};

template <class Mem>
WheelMatrices ReadWheelMatrices(const Mem& mem, uint32_t vehicle) {
  WheelMatrices m;
  for (uint32_t w = 0; w < kWheelCount; ++w) {
    m.tyre_ok[w] = ReadMatrix(mem, vehicle + kVehicleTyreMatrices + 64u * w, &m.tyre[w]);
    m.rim_ok[w] = ReadMatrix(mem, vehicle + kVehicleWheelMatrices + 64u * w, &m.rim[w]);
    m.aux1_ok[w] = ReadMatrix(mem, vehicle + kVehicleBrakeMatrices + 128u * w, &m.aux1[w]);
    m.aux2_ok[w] = ReadMatrix(mem, vehicle + kVehicleBrakeMatrices + 128u * w + 64u, &m.aux2[w]);
  }
  return m;
}

// The hypotheses for the model-to-world of a wheel geometry whose model's bone is `bone`, wheel w.
enum class WheelPose : uint8_t {
  kTyreMatrix,  // the array starting at V+1344+64w: bone b is the b-th matrix on (the tyre has one bone: the wheel's)
  kRimMatrix,   // V+1600+64w for every bone (the rim and its brake move as one)
  kRimAux1,     // the draw's own array A1 = [V+1600+64w, V+1856+128w] (stack layout: bone 0 the wheel, 1 the brake; 2 and 3 as the stack goes on)
  kRimAux2,     // A2 = [V+1600+64w, V+1920+128w]
};
inline constexpr uint32_t kWheelPoseCount = 4;

inline const char* WheelPoseName(WheelPose pose) {
  switch (pose) {
    case WheelPose::kTyreMatrix: return "tyre matrix V+1344+64w";
    case WheelPose::kRimMatrix: return "wheel matrix V+1600+64w";
    case WheelPose::kRimAux1: return "wheel + brake array [V+1600+64w, V+1856+128w]";
    case WheelPose::kRimAux2: return "wheel + brake array [V+1600+64w, V+1920+128w]";
  }
  return "?";
}

// What the asm says each resource is placed by.
inline constexpr WheelPose DefaultWheelPose(bool rim) { return rim ? WheelPose::kRimAux1 : WheelPose::kTyreMatrix; }

// The placement of wheel `w`'s geometry (its model's bone `bone`) under a hypothesis; false where a matrix it needs was not readable or the bone is not one of the
// array's.
inline bool PlaceWheel(const WheelMatrices& m, uint32_t w, WheelPose pose, uint32_t bone, temporal::Mat4* out) {
  if (w >= kWheelCount) {
    return false;
  }
  const auto take = [&](bool ok, const temporal::Mat4& matrix) {
    if (!ok) {
      return false;
    }
    *out = matrix;
    return true;
  };
  switch (pose) {
    case WheelPose::kTyreMatrix:
      return w + bone < kWheelCount && take(m.tyre_ok[w + bone], m.tyre[w + bone]);
    case WheelPose::kRimMatrix:
      return take(m.rim_ok[w], m.rim[w]);
    case WheelPose::kRimAux1:
      switch (bone) {
        case 0: case 2: return take(m.rim_ok[w], m.rim[w]);
        case 1: return take(m.aux1_ok[w], m.aux1[w]);
        case 3: return take(m.aux2_ok[w], m.aux2[w]);
        default: return false;
      }
    case WheelPose::kRimAux2:
      switch (bone) {
        case 0: case 2: return take(m.rim_ok[w], m.rim[w]);
        case 1: return take(m.aux2_ok[w], m.aux2[w]);
        default: return false;
      }
  }
  return false;
}

// Whether a geometry belongs to a wheel resource, and which.
inline bool IsWheelPart(uint32_t part) { return part == kTyrePart || part == kRimPart; }
inline bool IsRimPart(uint32_t part) { return part == kRimPart; }

}  // namespace mcla::gpu::car_model_walk
