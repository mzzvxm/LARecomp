#pragma once
// ===========================================================================
// MCLA GPU renderer -- AMD FSR Ray Regeneration: the plan of a frame, and what the DENOISER row reads (perf/RAY-REGEN.md, packages RG-3 and RG-5)
// ===========================================================================
// ray_regen_rules.h has every rule; ray_regen.h the runtime. This is the small piece of state between them that needs no AMD header, so that it is built on every PC (the row and the plan read it
// everywhere; on a PC that is not a Radeon RX 9000 on Direct3D 12 it says "not this graphics card" and nothing of AMD's is ever opened):
//
//   the device   NoteDevice: the graphics card's vendor and name and the Windows build, once at the start (TemporalSystem::Initialize), so that the pause menu lists the AMD FSR RAY REGENERATION row only on a
//                Radeon RX 9000 series card on Windows 11 (RowListed; every other PC has no such row), before the runtime has been asked.
//   the facts    MenuFacts: what the runtime said when it has been asked, else what the device alone says (an AMD card on Direct3D 12: "starting", so the row stays usable and the choice of its value
//                is what asks the runtime).
//   the plan     Planner::Update, once a frame at the frame boundary: the plan (ray_regen_rules.h SelectRayRegen), the live probe when the row has left OURS, the video memory room, the context (made while
//                the plan is on and for rr::kContextHoldFrames after, freed after: only while the option is selected), the choice beside DLSS Ray Reconstruction's plan, and the log lines. Modes()
//                answers the reflections' trigger: this frame is Ray Regeneration's for the effect, or it keeps its own filter.
//
// Threading: Planner is for the render thread. The boards (NoteDevice, NoteSsrTraced) are atomics read from any thread (the pause menu's).
// ===========================================================================

#include <cstdint>
#include <string_view>

#include "ray_regen_guides_rules.h"

namespace mcla::gpu {
class Device;
class FrameRing;
}  // namespace mcla::gpu

namespace mcla::gpu::rayregen {

// Whether this build has the AMD denoiser (LARECOMP_HAS_RAY_REGEN: the header and a Direct3D 12 backend).
constexpr bool BuildHas() {
#if defined(LARECOMP_HAS_RAY_REGEN)
  return true;
#else
  return false;
#endif
}

// The mcla_gpu_ray_regen debug switch as the rules take it (a word that is none of the four is the row's decision).
rr::DebugSwitch ReadDebugSwitch();

// The graphics card, once at the start. `d3d12`: the renderer runs on Direct3D 12. `adapter_name` and `windows_build` say whether the pause menu lists the AMD FSR RAY REGENERATION row at all (ray_regen_rules.h
// RowListed: a Radeon RX 9000 series card on Windows 11; the Windows build is 0 where it cannot be read).
void NoteDevice(uint32_t vendor_id, bool d3d12, std::string_view adapter_name, uint32_t windows_build);
uint32_t DeviceVendor();   // 0 until noted
// Whether the pause menu lists the AMD FSR RAY REGENERATION row on this PC (false until the device is noted).
bool RowListed();
// What the plan and the row read: the runtime's facts once it has been asked, else what the device alone says.
Facts MenuFacts();
// A context is asked for and not ready yet (the row says " (APPLYING)").
bool ContextMaking();

// The reflections' traced size of the last frame they ran (the size the context is made for).
void NoteSsrTraced(uint32_t width, uint32_t height);

class Planner {
 public:
  struct Inputs {
    Device* device = nullptr;
    FrameRing* frames = nullptr;
    rr::Denoiser setting = rr::Denoiser::kAuto;   // the DENOISER row
    uint32_t consumers_on = 0;                    // rr::Consumer bits (denoiser::ConsumersOn)
    uint64_t frame = 0;
    rr::Plan dlss_plan;                           // DLSS Ray Reconstruction's plan of this frame (the choice is made beside it)
  };
  // Once a frame, at the frame boundary, before the frame's effects ask.
  void Update(const Inputs& in);

  const Plan& plan() const { return plan_; }
  DenoiserKind chosen() const { return chosen_; }
  // The frame's signal modes for the effects (the twin of rr::RawModes): `play_view` is the frame's main view. The effects' own debug switches are the A/B (1 takes the frame, 0 keeps the effect's filter).
  rr::RawModes Modes(bool play_view, bool switch_ssr, bool switch_car_refl);
  // The signals of the context asked for, and the traced size it was asked for.
  uint32_t context_signals() const { return signals_; }
  // The frame's counters, for the summary (the effect adds what it recorded).
  struct Counters {
    uint64_t frames_planned_on = 0;
    uint64_t modes_asked = 0;
  };
  const Counters& counters() const { return counters_; }

 private:
  bool RoomOk(Device* device, uint32_t signals, uint32_t w, uint32_t h);

  Plan plan_;
  DenoiserKind chosen_ = DenoiserKind::kOurs;
  rr::ContextKeeper keeper_;
  uint32_t signals_ = 0;
  uint32_t asked_size_[2] = {};
  bool asked_ = false;                 // a context was asked for and not yet released
  rr::DebugSwitch debug_ = rr::DebugSwitch::kRow;
  rr::Denoiser setting_ = rr::Denoiser::kAuto;
  bool plan_logged_ = false;
  Plan plan_last_;
  DenoiserKind chosen_last_ = DenoiserKind::kOurs;
  bool modes_logged_ = false;
  rr::RawModes modes_last_;
  // The video memory room (asked once a second where the plan would be on, then less often while there is none).
  bool room_ok_ = true;
  bool room_refused_logged_ = false;
  uint64_t room_wait_ms_ = 0;
  uint64_t room_next_ms_ = 0;
  Counters counters_;
};

}  // namespace mcla::gpu::rayregen
