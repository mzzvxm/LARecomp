#pragma once
// ===========================================================================
// MCLA GPU renderer -- the per-frame instance list of the car ray tracing scene
// ===========================================================================
// The top level structure of car_rt_scene.h holds one instance per draw of the player's car and, since the other cars are in the scene, one per
// draw of each other car the selection kept (opponents, traffic, parked cars: car_rt_instances_rules.h has the rules, car_frame.h collects the
// draws). This is the list: every frame, at the history trigger, CarRtInstances turns the car frame's draws into the instances (a mesh by key, a
// transform in the frame's view space, a mask) and the rows of the table the shaders read, for the scene to write into the top level.
//
//   - The scene knows which meshes have a bottom level structure (`find`); a draw whose mesh has none yet is left out for the frame (the player's
//     are read at once, another car's within a budget a frame: a car that is new appears in parts over a few frames).
//   - Every instance of a model names the SAME bottom level structure: the mesh key is the model's triangles in model space, so a second car of
//     a model the scene has built costs an instance per draw and nothing else. The log counts distinct meshes against instances.
//   - The player's instances come first (rows 0 .. n-1, `draw` = the draw's place in car_frame::Frame::draws), then the other cars' (mask
//     CAR_RT_MASK_OTHERS, `draw` = the row of the car's record), then one record per other car (the body centre in view space and a radius).
//   - The limits of the other cars come from the graphics class (gpu_class.h; Mid 2 cars, High 4; none on Low) with two debug cvars to A/B.
//
// Threading: render thread only.
// ===========================================================================

#include <cstdint>
#include <functional>

#include "car_frame.h"
#include "car_rt_instances_rules.h"

namespace mcla::gpu {

class CarRtInstances {
 public:
  using FindMesh = std::function<bool(uint64_t, car_rt_instances::MeshRef*)>;

  // The limits of the other cars in force now: the graphics class's cell with the two debug overrides (mcla_gpu_car_rt_cars, mcla_gpu_car_rt_cars_radius).
  // Reads the cvars: once a frame, by the scene's Request.
  car_rt_instances::Limits CurrentLimits() const;
  // Whether the cars the camera does not see are placed from memory (mcla_gpu_car_rt_cars_memory, debug, live).
  bool RememberedOn() const;

  // This frame's list. `flip_z` and `glass_rule` as the scene decided them for the player's car (the other cars' instances all take the mask
  // CAR_RT_MASK_OTHERS, and CAR_RT_MASK_OTHER_CHROME as well for a draw of the chrome's shader; flagged when drawn after the world). `find(key, &ref)` says whether a mesh has a structure and where its indices and
  // vertices are in the shared buffers; the scene marks the mesh seen in it.
  // `extra_cars` / `extra_draws`: the cars placed from memory (car_rt_memory_rules.h: cars the camera does not see this frame, their kits placed at their
  // vehicles' matrices), appended to the frame's own kept cars and ranked with them by the distance to the player, within the class's count of cars.
  const car_rt_instances::List& Build(const car_frame::Frame& frame, const car_rt_instances::Limits& limits, bool flip_z, bool glass_rule,
                                      const FindMesh& find, const std::vector<car_rt_instances::OtherCar>* extra_cars = nullptr,
                                      const std::vector<car_rt_instances::OtherDraw>* extra_draws = nullptr);
  const car_rt_instances::List& list() const { return list_; }

  // What the scene did for the other cars' meshes, kept for the log (the scene adds to it as it works; the log window resets it).
  struct Work {
    uint32_t meshes_read = 0;          // other cars' meshes read from guest memory
    uint64_t triangles_read = 0;
    uint32_t over_stage_budget = 0;    // draws whose mesh was not read because the frame's budget for other cars' reads was spent
    uint32_t over_memory = 0;          // ... because the other cars' share of the video memory was used
  };
  Work& work() { return window_work_; }

  // The scene calls it once a frame after Build (with the number of bottom level structures it has built so far): sums the frame into a window and
  // logs it every few hundred frames, and when the first other car appears.
  void NoteFrame(uint64_t frame, uint64_t blas_built_total, const car_rt_instances::Limits& limits);

 private:
  struct Window {
    uint64_t first_frame = 0;
    uint32_t frames = 0;
    uint32_t frames_with_cars = 0;
    uint64_t player_sum = 0, other_sum = 0, chrome_sum = 0, cars_sum = 0, missing_sum = 0, bad_sum = 0, cut_sum = 0;
    uint64_t remembered_cars_sum = 0, remembered_instances_sum = 0;
    uint32_t frames_with_remembered = 0;
    uint32_t player_max = 0, other_max = 0, chrome_max = 0, cars_max = 0, distinct_max = 0, shared_max = 0, remembered_max = 0;
    uint64_t built_at_start = 0;
  };

  car_rt_instances::List list_;
  std::vector<car_rt_instances::OtherCar> merged_cars_;    // the frame's cars and the remembered ones, for Build
  std::vector<car_rt_instances::OtherDraw> merged_draws_;
  Window window_;
  Work window_work_;
  bool started_ = false;
  bool first_cars_logged_ = false;
  bool first_remembered_logged_ = false;
};

}  // namespace mcla::gpu
