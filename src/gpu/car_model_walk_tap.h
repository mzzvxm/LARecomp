#pragma once
// ===========================================================================
// MCLA GPU renderer -- the guest-thread side of the car model walk: which vehicles the game draws
// ===========================================================================
// car_model_walk.h walks the model data of the cars in the game, on the render thread, inside a draw's call (the guest is held back there). The vehicles to
// walk come from three places: the player's global, the renderer's vehicle client (read on the render thread), and THIS TAP, which the game's own thread fills:
// Vehicle::Draw (mcCarModel's draw, sub_82360518, r3 = the vehicle, r4 = the phase) is called for every vehicle in every phase, the ones the camera does not
// see included (the visibility word +116 and the LOD +6004 are written at phase 0x200 for each), so a hook there sees every vehicle the game has, whether it
// is culled or not. The entry itself (0x82360518) already carries Hook_SquashSelectVehicle and the recompiler takes one hook per address, so the call is made
// from Patch_RearViewCarPhase (0x8236051C, the next instruction, r3 and r4 are the same there; mc_engine/hooks/rear_view.cpp), which runs for every call of
// Vehicle::Draw before it tests anything: no change to the codegen config, no new hook.
//
// The tap costs one relaxed atomic load while mcla_gpu_car_model_walk is 0. It reads and writes no guest memory: it only remembers the vehicle's address and
// the bit of the phase it was drawn in. Header only (inline variables), so the hook's file needs no link-time dependency.
// ===========================================================================

#include <array>
#include <atomic>
#include <cstdint>

namespace mcla::gpu::car_model_walk::tap {

inline constexpr uint32_t kSlots = 64;  // a power of two

struct Slot {
  std::atomic<uint32_t> vehicle{0};
  std::atomic<uint32_t> phases{0};  // the phases it was drawn in since the render side last took them (a bit each; bit 31: a phase that is no single bit)
};

inline std::atomic<bool> g_enabled{false};
inline std::atomic<uint64_t> g_calls{0};
inline std::atomic<uint32_t> g_dropped{0};  // a vehicle that found no free slot
inline std::array<Slot, kSlots> g_slots{};

inline uint32_t PhaseBit(uint32_t phase) { return phase != 0 && (phase & (phase - 1)) == 0 ? phase : 0x80000000u; }

// The game's thread: `vehicle` was drawn in `phase`.
inline void NoteVehicle(uint32_t vehicle, uint32_t phase) {
  if (!g_enabled.load(std::memory_order_relaxed) || vehicle < 0x10000u) {
    return;
  }
  g_calls.fetch_add(1, std::memory_order_relaxed);
  const uint32_t bit = PhaseBit(phase);
  const uint32_t home = (vehicle >> 4) & (kSlots - 1);
  for (uint32_t probe = 0; probe < kSlots; ++probe) {
    Slot& slot = g_slots[(home + probe) & (kSlots - 1)];
    uint32_t current = slot.vehicle.load(std::memory_order_relaxed);
    if (current == 0 && slot.vehicle.compare_exchange_strong(current, vehicle, std::memory_order_relaxed)) {
      current = vehicle;
    }
    if (current == vehicle) {
      slot.phases.fetch_or(bit, std::memory_order_relaxed);
      return;
    }
  }
  g_dropped.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace mcla::gpu::car_model_walk::tap
