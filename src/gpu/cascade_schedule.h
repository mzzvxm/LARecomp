#pragma once

#include <atomic>
#include <cstdint>

#include "cascade_schedule_rules.h"

namespace mcla::gpu::cascade_schedule {

// Capture and the phase callbacks run on the same guest render thread. The
// draw needs the index only while measuring. Resolves carry it for atlas
// readiness as well, even when timing is off.
inline thread_local uint8_t capture_cascade = 0;
inline thread_local bool time_draws = false;
inline thread_local bool time_guest = false;
inline thread_local bool track_casters = false;
inline thread_local uint64_t capture_cycles = 0;
inline thread_local unsigned captured_draws = 0;
inline thread_local unsigned captured_clears = 0;
inline thread_local unsigned captured_resolves = 0;

inline void NoteCapture(uint64_t cycles) {
  if (time_guest && capture_cascade) {
    capture_cycles += cycles;
  }
}

inline std::atomic<uint32_t> atlas_ready_scale{0};
inline std::atomic<uint32_t> camera_cut{0};

void NoteCaster(uint32_t id, const float (&position)[3], uint8_t last_cascade);

}  // namespace mcla::gpu::cascade_schedule
