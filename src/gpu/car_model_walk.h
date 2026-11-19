#pragma once
// ===========================================================================
// MCLA GPU renderer -- the car model walk: every car in the game from its own model data, checked against the frame's draws (debug, log only)
// ===========================================================================
// PHASE 1 of the ray traced scene's route to cars the camera has never drawn (perf/RT-MODEL-SURVEY.md): a WALKER (car_model_walk_rules.h: vehicle -> entity ->
// model info -> model set -> part table -> drawable -> LOD -> model -> geometry -> vertex / index buffers, bone index and palette, paint) and the PROOF that it
// reads what a draw reads: for the cars the camera does see, each walked geometry is matched to the DrawRecords the frame captured (the GPU header words of its
// buffers are the draw's vertex fetch constant and the device's index buffer), and for the cars it does not, the walk says how many geometries it found and
// whether their buffers are resident. Nothing is built from the walked data (phase 2), nothing is written in the guest, and the existing draw and kit paths
// are untouched: this class only reads, and only while mcla_gpu_car_model_walk is above 0.
//
// HOW A FRAME RUNS (all on the render thread; CarFrame calls it):
//   StartFrame(frame)       decides whether this frame is a walk frame (every N-th guest frame, N = mcla_gpu_car_model_walk), and clears the frame's notes
//   OnVehicleDraw(...)      for every main-view draw that writes a vehicle stencil ID, inside the draw's call (the guest is held back): the draw's buffers are noted
//                           (DrawKey); the FIRST call of a walk frame also walks the cars: the player's vehicle (the global), the vehicles of the renderer's slot-7
//                           client (the game's list of mcCarModel items), and the vehicles the guest-thread tap saw Vehicle::Draw called with (car_model_walk_tap.h)
//   FinishFrame()           at the guest present: MatchFrame (car_model_walk_rules.h) and the log lines
//
// THE HOOK: Vehicle::Draw (0x82360518) at its second instruction, 0x8236051C (the existing midasm hook Patch_RearViewCarPhase, r3 = the vehicle, r4 = the
// phase), calls car_model_walk::tap::NoteVehicle. Vehicle::Draw runs for EVERY vehicle in every phase, also for a car whose draws the game culls (phase 0x200
// is the visibility and LOD update), so a culled car is seen. The entry itself carries Hook_SquashSelectVehicle (one hook per address), so no new hook and no
// codegen change is needed.
//
// THE LOG ([car model walk], one header line and one line per car, every N frames): for a drawn car matched / walked-only / drawn-only counts of its in-use
// geometries; for a culled one the geometries found and whether their buffers look resident (the part entry's load state is 3, the data is readable, the
// indices are in range, the positions are finite).
// ===========================================================================

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "car_model_rt_rules.h"
#include "car_model_walk_rules.h"
#include "car_model_walk_tap.h"
#include "car_rt_instances_rules.h"
#include "temporal_math.h"

namespace mcla::gpu {

class DrawView;
struct XenosState;

namespace car_model_walk {

// mcla_gpu_car_model_walk: 0 = off, N = walk and log every N-th guest frame.
uint32_t Interval();

// ---------------------------------------------------------------------------
// PRODUCTION (RT-MODEL-CARS phase 2): the cars of the ray traced scene from the cars' own model data
// ---------------------------------------------------------------------------
// The scene asks for it with its other cars (CarFrame::RequestModelCars): in every frame it collects, the FIRST vehicle draw of the frame walks the cars near the
// player (the player's vehicle and the renderer's list of vehicles, within the class's radius): the structure of a car's walk is cached per vehicle and made
// again every car_model_rt::kRewalkFrames frames or when the model set, the lods, the part variants or the draw flags change; every frame the part matrices of
// the geometries the scene takes are read and placed (car_model_rt_rules.h: the five hypotheses, the one the calibration chose against the frame's own draws).
// The same call hands the scene the cars to stage (WalkedCar::staging) and a MeshSource, which reads a geometry's triangles out of guest memory while the
// guest is held back. At the history trigger the scene calls Resolve: which walked cars the frame drew (the draw path keeps those), the calibration and the proof.
struct Production {
  bool on = false;
  car_rt_instances::Limits limits;
  car_model_rt::Mode mode = car_model_rt::Mode::kOff;
};

// One car of a production walk.
struct WalkedCar {
  uint32_t vehicle = 0;
  bool is_player = false;
  bool visible = false;       // the game's +116 at the walk: the main view shows it
  bool drawn = false;         // Resolve: a draw of the frame is one of its geometries
  bool staging = false;       // the scene is to stage this car's meshes: near, not visible, within the class's count (and a spare)
  double position[3] = {};    // the entity's world matrix row 3
  double distance = 0.0;      // from the player's vehicle, metres
  int32_t min_lod = 1;        // the lod floor it was walked with: 0 for a car the main view shows (to be checked against its draws), 1 for the others
  car_model_rt::PaintColour paint;
  std::shared_ptr<const CarWalk> walk;           // the cached structure (shared with the walker's cache)
  std::vector<car_model_rt::Part> parts;         // the geometries the scene takes, largest first, with their placements of this frame
  std::vector<car_model_rt::PoseSet> poses;      // for the calibration (only in the frames that calibrate): one per (part entry, bone)
  std::vector<int32_t> pose_of_geometry;         // by geometry index into `walk->geometries`: the place in `poses`, -1 none
};

// What a listener may call inside CarFrame's Listener::OnWalkedCars (the guest is held back there): the triangles of a geometry of a walked car.
class MeshSource {
 public:
  virtual ~MeshSource() = default;
  virtual car_model_rt::MeshProblem Read(const CarWalk& walk, uint32_t geometry, std::vector<float>* positions, std::vector<uint32_t>* indices,
                                         car_model_rt::MeshRead* info) const = 0;
};

// What the proof and the calibration found in one Resolve (for the scene's log).
struct ProofReport {
  bool ran = false;               // the pairs were compared in this call
  uint32_t pairs = 0;             // matched (geometry, draw) pairs compared in the view space with the chosen pose
  double mean = 0.0, worst = 0.0; // metres, in the view space of the top level
  double basis_worst = 0.0;
  uint32_t cars = 0;              // walked cars with pairs
  bool player_included = false;
  uint32_t walked_cars = 0;       // cars in this frame's walk
  uint32_t drawn_cars = 0;        // ... that the frame draws
};

class Walker {
 public:
  Walker();
  ~Walker();
  // True while the debug setting is on; also publishes it to the guest-thread tap (call it once a frame: CarFrame::BeginFrame). The frames then collect.
  bool Wanted();
  // A collecting frame starts at guest frame `guest_frame` (CarFrame::StartFrame).
  void StartFrame(uint64_t guest_frame, bool collecting);
  // This is a walk frame (the debug log) or a producing one: OnVehicleDraw wants the draws.
  bool active() const { return active_ || production_frame_; }
  // A main-view draw that writes the vehicle stencil ID `id`, inside the draw's call.
  // `world`: the draw's own model-to-world where the caller made it (a rigid draw), for the placement check; null otherwise.
  void OnVehicleDraw(const DrawView& view, const XenosState& state, uint32_t id, const temporal::Mat4* world = nullptr);
  // The guest present: compare and log.
  void FinishFrame();
  // The renderer shuts down.
  void Shutdown();

  // --- production ---
  // What the scene wants for the NEXT frame (call it in every frame it is wanted, as CarFrame::RequestOtherCars): StartFrame takes it.
  void SetProduction(const Production& wanted) { production_next_ = wanted; }
  // This frame's draws are noted and its first vehicle draw walks the cars for the scene.
  bool producing() const { return production_frame_; }
  // True once after the first vehicle draw of a producing frame walked the cars (inside that draw's call): the scene stages the cars' meshes then.
  bool TakeFresh() {
    const bool fresh = fresh_;
    fresh_ = false;
    return fresh;
  }
  const std::vector<WalkedCar>& walked() const { return walked_cars_; }
  const MeshSource& mesh_source() const;
  // The history trigger: marks the walked cars the frame draws, calibrates the pose against the draws of the cars the camera sees (the player's included) and
  // checks the chosen pose in the view space of the top level (`view`, `flip_z` as the scene's instances take them). The scene reads walked() after it.
  ProofReport Resolve(const temporal::Mat4& view, bool flip_z);
  // The pose the placements are made with, and how it was chosen.
  car_model_rt::Pose pose() const { return calibration_.decided() ? calibration_.chosen() : car_model_rt::kDefaultPose; }
  const car_model_rt::PoseCalibration& calibration() const { return calibration_; }
  // What the last production walk cost on the render thread (microseconds).
  double last_walk_microseconds() const { return walk_microseconds_; }
  // The caps and the mode of this frame (as StartFrame took them).
  const Production& production() const { return production_now_; }

 private:
  struct Found {
    uint32_t vehicle = 0;
    uint32_t sources = 0;  // kFromPlayer | kFromList | kFromTap (car_model_walk.cpp)
    uint32_t phases = 0;   // the phases the tap saw it drawn in since the last walk
    uint32_t vtable = 0;   // [vehicle + 0]: 0x82047B14 for mcCarModel (Vehicle::Draw is its slot 2)
  };
  struct Racer {
    bool known = false;      // the nearest opponent manager racer: its matrix is this far from the vehicle's
    double distance = 0.0;
    uint32_t racer = 0;      // its object
  };
  struct ListScan {
    bool renderer = false, client = false;
    uint32_t types = 0, instances = 0;
    uint32_t client_object = 0, client_mask = 0;
  };

  void RunWalk();
  void RunProduction();
  class Source;
  struct Cached {
    std::shared_ptr<CarWalk> walk;
    car_model_rt::WalkSignature signature;
    std::vector<uint32_t> picks;
    uint64_t last_frame = 0;
  };
  void NoteDraw(const DrawView& view, const XenosState& state, uint32_t id, const temporal::Mat4* world);
  void LogSamples(const CarWalk& car, const CarMatch& match, uint32_t car_index, const FrameMatch& frame);

  bool active_ = false;
  bool walked_ = false;
  // production
  Production production_next_, production_now_;
  bool production_frame_ = false;
  bool production_walked_ = false;
  bool fresh_ = false;
  std::vector<WalkedCar> walked_cars_;
  std::unordered_map<uint32_t, Cached> cache_;
  car_model_rt::PoseCalibration calibration_;
  uint64_t last_calibration_frame_ = 0;
  bool calibrating_ = false;
  std::unique_ptr<Source> source_;
  uint64_t proof_log_frame_ = 0;
  uint64_t proof_pairs_total_ = 0;
  uint32_t proof_logs_ = 0;
  uint32_t pair_samples_logged_ = 0;
  uint64_t frame_ = 0;
  uint32_t walks_logged_ = 0;
  bool options_read_ = false;
  Options options_;
  std::vector<Found> found_;
  std::vector<Racer> racers_;
  std::vector<CarWalk> cars_;
  std::vector<DrawKey> draws_;
  uint32_t draws_proxy_ = 0;
  uint32_t draws_no_key_ = 0;       // a draw with inline vertices or no vertex fetch: not a model's
  uint32_t draws_dropped_ = 0;
  uint32_t player_vehicle_ = 0;
  double player_position_[3] = {};
  bool player_position_known_ = false;
  ListScan list_;
  uint32_t tap_vehicles_ = 0, tap_dropped_ = 0;
  uint64_t tap_calls_total_ = 0;
  std::array<uint8_t, tap::kSlots> idle_{};
  double walk_microseconds_ = 0.0;
  std::string note_;
};

}  // namespace car_model_walk
}  // namespace mcla::gpu
