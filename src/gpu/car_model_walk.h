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
#include <string>
#include <vector>

#include "car_model_walk_rules.h"
#include "car_model_walk_tap.h"

namespace mcla::gpu {

class DrawView;
struct XenosState;

namespace car_model_walk {

// mcla_gpu_car_model_walk: 0 = off, N = walk and log every N-th guest frame.
uint32_t Interval();

class Walker {
 public:
  // True while the debug setting is on; also publishes it to the guest-thread tap (call it once a frame: CarFrame::BeginFrame). The frames then collect.
  bool Wanted();
  // A collecting frame starts at guest frame `guest_frame` (CarFrame::StartFrame).
  void StartFrame(uint64_t guest_frame, bool collecting);
  // This is a walk frame: OnVehicleDraw wants the draws.
  bool active() const { return active_; }
  // A main-view draw that writes the vehicle stencil ID `id`, inside the draw's call.
  void OnVehicleDraw(const DrawView& view, const XenosState& state, uint32_t id);
  // The guest present: compare and log.
  void FinishFrame();
  // The renderer shuts down.
  void Shutdown();

 private:
  struct Found {
    uint32_t vehicle = 0;
    uint32_t sources = 0;  // kFromPlayer | kFromList | kFromTap (car_model_walk.cpp)
    uint32_t phases = 0;   // the phases the tap saw it drawn in since the last walk
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
  void NoteDraw(const DrawView& view, const XenosState& state, uint32_t id);
  void LogSamples(const CarWalk& car, const CarMatch& match, uint32_t car_index, const FrameMatch& frame);

  bool active_ = false;
  bool walked_ = false;
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
