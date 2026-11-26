#pragma once
// ===========================================================================
// MCLA GPU renderer -- the ray tracing scene service (CITY-RT M2 package H3)
// ===========================================================================
// ONE place decides what the ray tracing scene holds each frame, and any consumer asks it: the car effects, the city's sun shadows, M2's reflections (the car paint, glass and chrome, the
// screen space reflections' fallback), M3's probes. The rules are rt_scene_rules.h (the wants, the plan, the camera predictor; rt_scene_rules_test.cpp); this class is their device side:
//
//   Want(consumer, needs)   a consumer needs the scene in this frame: the player's car (CarFrame collects its draws only while asked), the city's structures at a level, the city's hit data
//                           at a level. A want counts for its frame and the next; a scene nobody asks for during 600 frames is given back (the structures and the pool each know when).
//   EndFrame()              at the guest present: the wants of the frame become the previous frame's, and the city scene and the hit data's pool are told the merged level for the frame
//                           that follows (CityRtScene::BeginFrame, CityHitScene::BeginFrame): this is the ONE caller of those, so every consumer shares the structures and the pool.
//   NoteAnchor(banks, cut)  the game's camera of the frame (the motion blur draw, after the trigger): kept for the next frame's predicted camera.
//   PlanTlas / PredictedView  what CarRtScene::OnResolve (the history trigger) does this frame: the top level of the cars and the city in the car frame's exact camera, or, where the player's
//                           car is not in the frame (a Director camera, the pause world, a hidden car), the city alone in the predicted camera. The top level is built by CarRtScene (it
//                           owns the per-slot buffers and the descriptor set); this decides WHEN and IN WHICH VIEW, so the car is not what the city's rays wait for.
//
// The consumers' view of the result is CarRtView (car_rt_scene.h): `valid` is the car's top level (as before: every car effect keeps its gate), `scene_valid` any top level.
// ===========================================================================

#include <cstdint>
#include <string>

#include "constants.h"
#include "rt_scene_rules.h"

namespace mcla::gpu {

class CarRtScene;
class CityHitScene;
class CityRtScene;

class RtSceneService {
 public:
  // Not owned; null where a part does not exist. `cars` is asked (Request) for the consumers that want the player's car.
  void Initialize(CarRtScene* cars, CityRtScene* city, CityHitScene* hit);
  void Shutdown();

  // A consumer wants the scene in this frame (any time in it). Cheap: call it every frame the consumer is on, with the same needs.
  void Want(rt_scene::Consumer who, const rt_scene::Needs& needs);
  // The car effects' own Request() lands here (CarRtScene::Request), so the board knows them too.
  void NoteCarWant() { board_.Want(rt_scene::Consumer::kCars, CarsNeeds()); }
  // A top level was built with the car frame's camera (CarRtScene::OnResolve), for the census.
  void NoteCarTop() { ++counts_.car_tops; }
  void NoteTopLevel(bool reused, uint32_t city_instances);

  // The merged wants of this frame and the last (what the structures and the pool are told, and what the plan below judges).
  rt_scene::Needs Needs() const { return board_.Now(); }
  bool Wants(rt_scene::Consumer consumer) const { return board_.Wants(consumer); }
  bool CityWanted() const { return board_.CityWanted(); }

  // The game's camera (scene.cpp, at the anchor draw): its pose of this frame. `cut`: the scene asked for a camera cut.
  void NoteAnchor(const ConstantBanks::Banks& banks, bool cut);

  // What the top level is in the frame being recorded (OnResolve): `car_frame_valid` as CarFrame says.
  rt_scene::TlasPlan PlanTlas(bool car_frame_valid);
  // The predicted camera of the frame being recorded, for a top level without the car. False: none (and the plan said so).
  bool PredictedView(temporal::Mat4* view, bool* flip_z);

  // The frame boundary (the guest present): after the car scene's own.
  void EndFrame();

  // The log's and the census's text: the wants by consumer, the top levels built without the car, the frames that had no camera.
  std::string CensusText() const;
  uint64_t frame() const { return board_.frame(); }

 private:
  static constexpr rt_scene::Needs CarsNeeds() {
    rt_scene::Needs n;
    n.cars = true;
    return n;
  }
  void LogPlan(const rt_scene::TlasPlan& plan, bool car_frame_valid);

  CarRtScene* cars_ = nullptr;
  CityRtScene* city_ = nullptr;
  CityHitScene* hit_ = nullptr;
  rt_scene::WantBoard board_;
  rt_scene::CameraPredictor camera_;
  uint64_t anchors_ = 0;
  uint64_t cut_anchors_ = 0;

  struct Counts {
    uint64_t car_tops = 0;        // top levels planned with the car frame's camera
    uint64_t city_only_tops = 0;  // ... and without the car, in the predicted camera
    uint64_t held_pose = 0;       // of those, frames the predicted pose was a held one (a cut, or one pose known)
    uint64_t no_camera = 0;       // frames the city was wanted, the car was not found and no camera was known
    uint64_t tlas_builds = 0;
    uint64_t tlas_reuses = 0;
    uint64_t city_ready_frames = 0;
    uint64_t nothing = 0;         // frames nothing needed a top level
  } counts_;
  int logged_why_ = -1;
  uint64_t log_frame_ = 0;
};

}  // namespace mcla::gpu
