#pragma once
// ===========================================================================
// MCLA -- the turn of a traffic car's sun shadow (the TRAFFIC SHADOWS setting): the rules, pure
// ===========================================================================
// Owner, 2026-11-19: "as the traffic car turns, the shadow follows the car positionally, but doesn't update in any other
// way".
//
// WHY [guest, read from the recompiled code and the shader sources 2026-11-19; the run is CHECK]. A traffic car (an item of
// mcAmbientCarRenderClass) is drawn by sub_827914B8 (the model's vtbl+28) from the item's per-buffer block, `block =
// [[item+24] + (buffer + 2) * 4]`:
//   block + 16   a vec4, the car's ROOT translation;
//   block + 32   the bone matrices, 48 bytes each, three rows of four floats (a row is an output component, the fourth
//                float its translation); bone 0 is the body. The palette is world space minus the root translation: the
//                car's heading is IN THE BONES.
// sub_827914B8 sets the viewport's World matrix first (0x827917C0: `bl sub_8217D918`, r3 = r1 + 144, which is
// sub_8217C088 = copy four rows into the current viewport's world matrix and rebuild WorldViewProj): rows 0 to 2 are the
// unit axes, row 3 is block + 16. So World is a pure translation. The colour pass (VSAmbientBumpSpec and its sisters) skins
// every vertex by gCarMtxBuffer(bone) and THEN multiplies by gWorldViewProj, so the car turns. The shadow technique of every
// ambient shader group (VS_ShadowDepthLocal: xAmbientBumpSpec, BlackMatte, CarGlass, CarLight, CarPaint, Tire) is rigid:
//     oPos = float4(iPosition0.xyz, 1) * gWorldViewProj
// with no palette. The player's and the racers' cars have VS_CarShadowDepthLocal, which does skin. So a raised traffic car
// (hooks/traffic_shadow.cpp: the cascade phases drawn through the item's body branch) is drawn into the sun cascades as its
// bind-pose mesh at its root position, never turned: it follows the car's position and nothing else. The game never made a
// traffic car cast, so nobody saw that the shader is rigid; its soft decal (sub_82795960) is built from bone 0's axes and
// does turn.
//
// THE FIX: in a sun cascade phase, the World matrix of the item's body draw is made from bone 0 instead of the translation
// alone: the rows are the bone's columns (the bone maps a local point p to M * p + t, a row vector maps it to p * W), the
// translation row is the root plus the bone's own translation. The wheels, which are separate meshes with their own bones,
// then sit in their bind pose against the body, which a shadow does not show (a wheel's spin and a steering angle do not
// change its outline from the sun).
//
// Everything that decides something is here, pure; traffic_shadow_test.cpp checks it.
// ===========================================================================

#include <atomic>
#include <cmath>
#include <cstdint>

namespace mcla::gpu::traffic_shadow {

inline constexpr uint32_t kRendererPhaseOffset = 356u;  // [guest] [renderer] + 356: the render phase being drawn
inline constexpr uint32_t kBlockRoot = 16u;             // [guest] block + 16: the car's root translation (vec4)
inline constexpr uint32_t kBlockBones = 32u;            // [guest] block + 32: bone 0's 3x4 matrix (48 bytes a bone)
inline constexpr uint32_t kWorldRows = 4u;              // [guest] the World matrix sub_8217D918 takes: four rows of four floats

// The bone's axis and size limits: a matrix that is not finite, or whose axes are nothing or enormous, is not used (the
// game's own World stays).
inline constexpr float kMinAxisLength = 1.0e-3f;
inline constexpr float kMaxAxisLength = 1.0e3f;
inline constexpr float kMaxCoordinate = 1.0e6f;

// The shadow's World for a car whose body bone is `bone` (three rows of four, a row an output component, the fourth column
// the translation) from the game's own World `game_world` (four rows of four, the unit axes and the root translation in
// row 3). On success `out` is the matrix the shadow draw should use and true is returned; otherwise `out` is a copy of
// `game_world` and false is returned.
inline bool BodyBoneWorld(const float game_world[4][4], const float bone[3][4], float out[4][4]) {
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      out[r][c] = game_world[r][c];
    }
  }
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 4; ++c) {
      if (!std::isfinite(bone[r][c]) || std::fabs(bone[r][c]) > kMaxCoordinate) {
        return false;
      }
    }
  }
  for (int axis = 0; axis < 3; ++axis) {
    const float length = std::sqrt(bone[0][axis] * bone[0][axis] + bone[1][axis] * bone[1][axis] +
                                   bone[2][axis] * bone[2][axis]);
    if (!(length >= kMinAxisLength) || length > kMaxAxisLength) {
      return false;
    }
  }
  for (int axis = 0; axis < 3; ++axis) {
    out[axis][0] = bone[0][axis];
    out[axis][1] = bone[1][axis];
    out[axis][2] = bone[2][axis];
  }
  for (int i = 0; i < 3; ++i) {
    out[3][i] = game_world[3][i] + bone[i][3];
  }
  return true;
}

// --- yaw, for the debug log ---------------------------------------------------------------------------------------

constexpr float kRadiansToDegrees = 57.29577951308232f;

// A direction's heading in degrees on the ground plane (the world's up is y: sub_822F3918 reads the height at +52): 0 along
// +z, 90 along +x. A direction with no length on the ground has no heading: 0.
inline float YawOfDirection(float x, float z) {
  if (!std::isfinite(x) || !std::isfinite(z) || (x * x + z * z) < 1.0e-12f) {
    return 0.0f;
  }
  return std::atan2(x, z) * kRadiansToDegrees;
}

// The heading of the body bone: where the model's +z axis (the bone's third column) points.
inline float BodyYaw(const float bone[3][4]) { return YawOfDirection(bone[0][2], bone[2][2]); }
// The heading of a World matrix: where the model's +z axis (row 2) points.
inline float WorldYaw(const float world[4][4]) { return YawOfDirection(world[2][0], world[2][2]); }

// An angle in degrees brought into (-180, 180].
inline float WrapDegrees(float degrees) {
  if (!std::isfinite(degrees)) {
    return 0.0f;
  }
  float d = std::fmod(degrees, 360.0f);
  if (d > 180.0f) {
    d -= 360.0f;
  } else if (d <= -180.0f) {
    d += 360.0f;
  }
  return d;
}

// How far the shadow's heading is from the body's, degrees in [0, 180].
inline float YawError(float body_deg, float shadow_deg) { return std::fabs(WrapDegrees(shadow_deg - body_deg)); }

// Turn rate in degrees a second between two readings of one car; 0 when the readings are the same instant or too far apart
// to be a rate of one turn (more than max_gap_s).
inline float TurnRate(float yaw_deg, float previous_deg, double dt_s, double max_gap_s = 0.5) {
  if (!(dt_s > 1.0e-4) || dt_s > max_gap_s) {
    return 0.0f;
  }
  return float(double(WrapDegrees(yaw_deg - previous_deg)) / dt_s);
}

// The tolerance of the judged line (the scene chain): a shadow that is turned with its car is within this of the body's
// heading in every logged frame.
inline constexpr float kYawToleranceDegrees = 2.0f;

// Per-car readings for the turn rate: the last heading and when it was read. Small and linear (a handful of cars cast).
class YawHistory {
 public:
  static constexpr uint32_t kCapacity = 32;
  // Records the heading of car `id` read at `now_ns`; returns its turn rate against the previous reading (0 for a car not
  // seen in the last half second, or the first reading).
  float Note(uint32_t id, float yaw_deg, int64_t now_ns) {
    uint32_t slot = kCapacity;
    for (uint32_t i = 0; i < count_; ++i) {
      if (ids_[i] == id) {
        slot = i;
        break;
      }
    }
    float rate = 0.0f;
    if (slot == kCapacity) {
      if (count_ < kCapacity) {
        slot = count_++;
      } else {
        slot = next_++ % kCapacity;  // the table is full: replace in turn
      }
      ids_[slot] = id;
    } else {
      rate = TurnRate(yaw_deg, yaw_[slot], double(now_ns - at_ns_[slot]) * 1.0e-9);
    }
    yaw_[slot] = yaw_deg;
    at_ns_[slot] = now_ns;
    return rate;
  }
  void Reset() { *this = YawHistory(); }

 private:
  uint32_t ids_[kCapacity] = {};
  float yaw_[kCapacity] = {};
  int64_t at_ns_[kCapacity] = {};
  uint32_t count_ = 0;
  uint32_t next_ = 0;
};

// Counters of the world hook, for the debug log line.
struct PoseCounts {
  std::atomic<uint64_t> calls{0};      // the hook's calls in a cascade phase
  std::atomic<uint64_t> turned{0};     // calls that put the body bone into the World
  std::atomic<uint64_t> refused{0};    // memory, pointer or matrix not usable: the game's World stayed
  std::atomic<uint64_t> switched_off{0};  // the debug switch is off (the game's World stayed)
};

}  // namespace mcla::gpu::traffic_shadow
