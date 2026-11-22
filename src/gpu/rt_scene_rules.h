#pragma once
// ===========================================================================
// MCLA GPU renderer -- the ray tracing scene service: who wants what, and what the top level holds (CITY-RT M2 package H3)
// ===========================================================================
// Until now the top level acceleration structure was the player's car's: it was built only in a frame where the car scene had been asked and the car was found, and the
// city's structures (city_rt_scene.h) rode along in it ("the city is in the top level only in a frame the car scene builds one", CITY-RT-M1-RUNPLAN section 4). Every consumer
// that comes after M1 breaks that: the city's reflections (M2), the screen space reflections' ray traced fallback, the global illumination's probes (M3) look at the WORLD and
// have no business with whether the car is in the picture (a Director camera far from the car, the pause menu's world, a photo with the car hidden).
//
// So there is ONE place that decides, every frame, what the top level holds, and consumers ask it: the scene service (rt_scene_service.h, the device side). This header is its
// rules, as pure functions on plain numbers so that rt_scene_rules_test.cpp builds on its own:
//
//   Needs        what one consumer asks for: the player's car (its draws and the other cars: car_frame.h collects them only while somebody asks), the city's structures at a level,
//                the city's hit data (the pool, city_hit_pool_rules.h) at a level, a debug radius
//   WantBoard    the lifecycle of the asking. A consumer Wants in every frame it needs the scene (any time in the frame, before or after the trigger); a want counts for the frame
//                it was made in AND the next (the top level is built at the history trigger, in the middle of the frame, so a consumer that runs after it asks for the next one);
//                the merge of all the wants is what the scene builds; a scene nobody has asked for during kIdleReleaseFrames frames is given back (once)
//   TlasPlan     what the top level is, this frame: built or not, with the car's instances or without, with the city's, and in WHICH VIEW (the car frame's exact camera when the car
//                was found; else the camera of the last frames, predicted)
//   CameraPredictor  the camera of THIS frame when the car is not there to carry it. The car frame reads the frame's camera out of the player's draws (car_frame.h NoteCamera):
//                without the car there is none, and the game's own camera (the anchor, the motion blur draw) comes after the trigger. The camera of the last two frames, carried one
//                step on at constant twist (the pose times the last step: a car's camera moves along its heading and turns at a steady rate, which is an arc), is the answer: the
//                error is the change of the step in one frame (millimetres in a steady drive) and a frame that is a CUT (a teleport, a shot change: the step is beyond what a frame
//                moves) holds the last pose and says so.
//
// This header includes nothing of the renderer: city_rt_rules.h (the level) and temporal_math.h (the matrices).
// ===========================================================================

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "city_rt_rules.h"
#include "city_texture_scope.h"
#include "temporal_math.h"

namespace mcla::gpu::rt_scene {

using Level = city_rt::Level;

constexpr Level MaxLevel(Level a, Level b) { return uint8_t(a) >= uint8_t(b) ? a : b; }

// ---------------------------------------------------------------------------
// the consumers and what they ask for
// ---------------------------------------------------------------------------

enum class Consumer : uint8_t {
  kCars,             // the car effects (car_rt_ao, car_rt_refl, the opponents' pass: the car scene's own Request())
  kCitySunShadow,    // the city's ray traced sun shadows (city_rt_shadow.h)
  kCityReflections,  // M2: the car paint, glass and chrome reflecting the city
  kSsrFallback,      // M2: the screen space reflections' ray traced fallback
  kCityGi,           // M3: the global illumination's probes and rays
  kCheck,            // the debug checks (mcla_gpu_car_rt_check, mcla_gpu_city_rt_check, mcla_gpu_city_hit_data)
  kRtLights,         // the ray traced visibility of the top K lights (rt_lights.h, mcla_gpu_rt_lights)
  kCount,
};
inline constexpr size_t kConsumerCount = size_t(Consumer::kCount);

constexpr std::string_view ConsumerName(Consumer who) {
  switch (who) {
    case Consumer::kCars: return "car effects";
    case Consumer::kCitySunShadow: return "city sun shadows";
    case Consumer::kCityReflections: return "city reflections";
    case Consumer::kSsrFallback: return "ssr fallback";
    case Consumer::kCityGi: return "city gi";
    case Consumer::kCheck: return "debug checks";
    case Consumer::kRtLights: return "rt lights";
    default: return "?";
  }
}

struct Needs {
  bool cars = false;          // the player's car's draws, the other cars (CarFrame collects them only while asked)
  Level city = Level::kOff;   // the city's structures at this level
  Level hit = Level::kOff;    // the city's hit data in the pool at this level (it needs the structures: Normalized)
  TextureScope textures = TextureScope::kNone;  // which city textures the hit data's readers need resident (city_texture_scope.h; it needs the hit data: Normalized); the widest of the wants
  float city_radius = 0.0f;   // the debug radius of the hi detail ring (0: the class's); the largest of the wants

  constexpr bool any() const { return cars || city != Level::kOff || hit != Level::kOff; }
  constexpr bool operator==(const Needs& o) const { return cars == o.cars && city == o.city && hit == o.hit && textures == o.textures && city_radius == o.city_radius; }
};

// Hit data is read by rays that hit the city's structures: a consumer of it has the structures at (at least) the same level.
constexpr Needs Normalized(Needs n) {
  n.city = MaxLevel(n.city, n.hit);
  if (n.hit == Level::kOff) n.textures = TextureScope::kNone;  // textures are the hit data's: without it there is nothing to read them
  return n;
}

constexpr Needs Merge(const Needs& a, const Needs& b) {
  Needs m;
  m.cars = a.cars || b.cars;
  m.city = MaxLevel(a.city, b.city);
  m.hit = MaxLevel(a.hit, b.hit);
  m.textures = MaxScope(a.textures, b.textures);
  m.city_radius = std::max(a.city_radius, b.city_radius);
  return Normalized(m);
}

// ---------------------------------------------------------------------------
// the lifecycle of the asking
// ---------------------------------------------------------------------------

class WantBoard {
 public:
  // 600 frames (10 s at 60) without any want and the scene is given back, as CarRtScene::kIdleReleaseFrames and CityRtScene::kIdleReleaseFrames.
  static constexpr uint32_t kIdleReleaseFrames = 600;

  // `who` wants `needs` in this frame. The latest call of a frame replaces the consumer's earlier one (a consumer that changes its mind in a frame is heard once); an empty
  // Needs is "no longer".
  void Want(Consumer who, const Needs& needs) {
    const size_t i = size_t(who);
    if (i < kConsumerCount) now_[i] = Normalized(needs);
  }
  // The merge of this frame's wants and the previous frame's (a want counts for the frame and the next).
  Needs Now() const {
    Needs merged;
    for (size_t i = 0; i < kConsumerCount; ++i) merged = Merge(merged, Merge(now_[i], prev_[i]));
    return merged;
  }
  // Only the wants made in THIS frame (what the frame boundary hands on).
  Needs ThisFrame() const {
    Needs merged;
    for (size_t i = 0; i < kConsumerCount; ++i) merged = Merge(merged, now_[i]);
    return merged;
  }
  bool Wants(Consumer who) const {
    const size_t i = size_t(who);
    return i < kConsumerCount && (now_[i].any() || prev_[i].any());
  }
  // The frame boundary (the guest present): this frame's wants become the previous frame's.
  void EndFrame() {
    bool any = false;
    for (size_t i = 0; i < kConsumerCount; ++i) {
      prev_[i] = now_[i];
      if (now_[i].any()) {
        ++frames_wanted_[i];
        any = true;
      }
      now_[i] = Needs{};
    }
    ++frame_;
    if (any) {
      idle_frames_ = 0;
    } else if (idle_frames_ < kIdleReleaseFrames + 1) {
      ++idle_frames_;
    }
  }
  uint64_t frame() const { return frame_; }
  // Frames in a row (the ones EndFrame has closed) in which nobody asked, counted up to kIdleReleaseFrames + 1.
  uint32_t idle_frames() const { return idle_frames_; }
  // True in exactly one frame: the one after the idle count reached kIdleReleaseFrames. The scene gives everything back then.
  bool ReleaseDue() const { return idle_frames_ == kIdleReleaseFrames; }
  uint64_t frames_wanted(Consumer who) const { return size_t(who) < kConsumerCount ? frames_wanted_[size_t(who)] : 0; }

 private:
  std::array<Needs, kConsumerCount> now_{};
  std::array<Needs, kConsumerCount> prev_{};
  std::array<uint64_t, kConsumerCount> frames_wanted_{};
  uint64_t frame_ = 0;
  uint32_t idle_frames_ = 0;
};

// ---------------------------------------------------------------------------
// what the top level is
// ---------------------------------------------------------------------------

enum class ViewSource : uint8_t {
  kNone,      // no top level this frame
  kCarFrame,  // the exact camera the player's draws carry (car_frame.h Frame::view): the car was found
  kCamera,    // the predicted camera (CameraPredictor): the car is not in the frame
};

struct TlasPlan {
  bool build = false;
  bool cars = false;  // the player's and the other cars' instances (the car path of CarRtScene::OnResolve)
  bool city = false;  // the city's instances
  ViewSource view = ViewSource::kNone;
  const char* why = "";
};

// `car_frame_valid`: CarFrame found the player's car with draws this frame (so Frame::view is the frame's exact camera); `camera_ok`: the predictor has a camera for this frame.
constexpr TlasPlan PlanTlas(const Needs& needs, bool car_frame_valid, bool camera_ok) {
  TlasPlan plan;
  if (!needs.any()) {
    plan.why = "nothing wants the scene";
    return plan;
  }
  if (car_frame_valid) {
    // The car's frame is there: its camera is exact whoever asks. The car's instances go in when somebody asks for the cars (else the draws were collected for nobody).
    plan.cars = needs.cars;
    plan.city = needs.city != Level::kOff;
    plan.build = plan.cars || plan.city;
    plan.view = plan.build ? ViewSource::kCarFrame : ViewSource::kNone;
    plan.why = plan.build ? "the car was found: its camera" : "only the hit data is wanted: no structures to put in a top level";
    return plan;
  }
  if (needs.city == Level::kOff) {
    plan.why = "no car in the frame and nothing wants the city";
    return plan;
  }
  if (!camera_ok) {
    plan.why = "no car in the frame and no camera to place the city by";
    return plan;
  }
  plan.build = true;
  plan.city = true;
  plan.view = ViewSource::kCamera;
  plan.why = "no car in the frame: the city alone, in the predicted camera";
  return plan;
}

// ---------------------------------------------------------------------------
// the camera of a frame the car does not carry
// ---------------------------------------------------------------------------

// The game's own conventions (temporal_camera.h): matrices of row vectors. `view` is world to view and looks down -z; `view_inverse` is camera to world with the rows right, up,
// BACKWARD and position, so the camera looks along minus row 2.
struct CameraPrediction {
  temporal::Mat4 view;      // world to view, for CityRtScene::Prepare
  bool flip_z = true;       // the instance transform negates z (the view looks down -z: the occlusion's view space has z forward); false where the view already has z forward
  bool extrapolated = false;  // false: the last pose held (one pose known, an older pose, or a step that is a cut)
  bool cut = false;         // the last step was a cut: the pose is held and the history of the camera is not trusted yet
};

class CameraPredictor {
 public:
  // A step longer than this (metres) or turning more than this (radians) in ONE frame is a cut, not a drive: 30 m is 1,800 m/s at 60 FPS, 0.6 rad is 34 degrees (temporal_camera.h's
  // cut rule floors are 80 m and 15 to 30 degrees at the camera's own scale; this is the stricter one, because a wrong extrapolation moves the whole city while a held pose
  // is only a frame late).
  // CHECK: the two limits against the real camera traces (cam-*/camera.csv of the drive, cut lines of the cut rule): a drive's largest step is 2.9 m, a teleport 1,600 m.
  static constexpr double kCutMetres = 30.0;
  static constexpr double kCutRadians = 0.6;
  // A pose older than this many frames is not used at all (a pause, a loading screen: the camera is not known).
  static constexpr uint64_t kMaxAgeFrames = 3;
  // How far ahead of the camera the point is that tells the view's z direction (metres).
  static constexpr double kProbeMetres = 10.0;

  void Reset() { *this = CameraPredictor{}; }
  bool known() const { return count_ != 0; }
  uint64_t last_frame() const { return pose_[0].frame; }

  // The camera of frame `frame` as the anchor read it (the pose of THAT frame). `cut`: the scene asked for a cut (scene.cpp NoteAnchorCut) or the caller's own rule says so.
  void Note(uint64_t frame, const temporal::Mat4& view_inverse, bool cut) {
    if (!temporal::Finite(view_inverse)) {
      Reset();
      return;
    }
    if (count_ != 0 && frame <= pose_[0].frame) {  // time went backwards (a new session): start again
      Reset();
    }
    pose_[1] = pose_[0];
    pose_[0].frame = frame;
    pose_[0].inverse = view_inverse;
    pose_[0].cut = cut;
    count_ = std::min<uint32_t>(count_ + 1, 2);
  }

  // The camera for frame `frame_now` (usually the frame after the last pose: the anchor comes after the trigger). False: nothing usable.
  bool Predict(uint64_t frame_now, CameraPrediction* out) const {
    if (count_ == 0 || !out) return false;
    const Pose& last = pose_[0];
    if (frame_now < last.frame || frame_now - last.frame > kMaxAgeFrames) return false;
    temporal::Mat4 pose = last.inverse;
    CameraPrediction p;
    if (frame_now == last.frame + 1 && count_ == 2 && last.frame == pose_[1].frame + 1) {
      const StepSize step = StepOf(pose_[1].inverse, last.inverse);
      p.cut = last.cut || step.metres > kCutMetres || step.radians > kCutRadians;
      if (!p.cut) {
        // P1 * P0^-1 * P1: the last step (in the camera's own frame) once more.
        temporal::Mat4 p0_inverse;
        if (temporal::Inverse(pose_[1].inverse, &p0_inverse)) {
          const temporal::Mat4 next = temporal::Mul(temporal::Mul(last.inverse, p0_inverse), last.inverse);
          if (temporal::Finite(next) && RigidEnough(next)) {
            pose = next;
            p.extrapolated = true;
          }
        }
      }
    } else if (last.cut) {
      p.cut = true;
    }
    temporal::Mat4 view;
    if (!temporal::Inverse(pose, &view)) return false;
    p.view = view;
    p.flip_z = LooksDownMinusZ(view, pose);
    *out = p;
    return true;
  }

  // The distance the camera moved and the angle it turned between two poses (the step's size, for the cut rule and the tests).
  struct StepSize {
    double metres = 0.0;
    double radians = 0.0;
  };
  static StepSize StepOf(const temporal::Mat4& from, const temporal::Mat4& to) {
    StepSize s;
    double d2 = 0.0;
    for (int k = 0; k < 3; ++k) {
      const double d = to.m[3][k] - from.m[3][k];
      d2 += d * d;
    }
    s.metres = std::sqrt(d2);
    // The angle of the relative rotation R = Rfrom^T Rto: cos = (trace - 1) / 2; the rows are orthonormal so the trace of Rfrom^T Rto is the sum of the row dot products.
    double trace = 0.0;
    for (int r = 0; r < 3; ++r) {
      for (int k = 0; k < 3; ++k) trace += from.m[r][k] * to.m[r][k];
    }
    s.radians = std::acos(std::clamp((trace - 1.0) * 0.5, -1.0, 1.0));
    return s;
  }

  // True when the view looks down -z (the game's own view matrix: the instance transform flips z), false when it already has z forward. Judged by a point kProbeMetres ahead of
  // the camera along minus row 2 of the pose: its z in the view is negative in the game's convention. The same test as car_rt_scene_rules.h ViewZSign, with the camera for the car.
  static bool LooksDownMinusZ(const temporal::Mat4& view, const temporal::Mat4& view_inverse) {
    double ahead[3];
    for (int k = 0; k < 3; ++k) ahead[k] = view_inverse.m[3][k] - view_inverse.m[2][k] * kProbeMetres;
    const double z = ahead[0] * view.m[0][2] + ahead[1] * view.m[1][2] + ahead[2] * view.m[2][2] + view.m[3][2];
    return z < 0.0;
  }

 private:
  struct Pose {
    uint64_t frame = 0;
    temporal::Mat4 inverse;
    bool cut = false;
  };
  // The product of two rigid poses stays rigid; this only guards against a matrix the arithmetic has spoilt (the rows' lengths within 1e-3 of one).
  static bool RigidEnough(const temporal::Mat4& m) {
    for (int r = 0; r < 3; ++r) {
      const double len2 = m.m[r][0] * m.m[r][0] + m.m[r][1] * m.m[r][1] + m.m[r][2] * m.m[r][2];
      if (!(std::fabs(len2 - 1.0) < 2e-3)) return false;
    }
    return true;
  }
  std::array<Pose, 2> pose_{};
  uint32_t count_ = 0;
};

}  // namespace mcla::gpu::rt_scene
