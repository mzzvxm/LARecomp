#pragma once
// ===========================================================================
// MCLA GPU renderer -- a cheap clock for per-draw statistics (M6a)
// ===========================================================================
// std::chrono::steady_clock reads
// QueryPerformanceCounter, ~25 ns a call; a few per draw at ~8000 draws a frame
// cost most of a millisecond for statistics alone. The TSC reads in a few ns.
// Per-draw time is summed in TSC cycles and turned into nanoseconds with the
// cycles/steady-clock rate measured over the last statistics window
// (Measure, called by the Scene at each window's end and once at start).
//
// Threading: render thread only.
// ===========================================================================

#include <cstdint>

#if defined(_WIN32)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif

namespace mcla::gpu::cycles {

inline uint64_t Now() {
  return __rdtsc();
}

// Nanoseconds per cycle, as last measured.
double NsPerCycle();

// The rate from the TSC and steady-clock movement since the previous call.
void Measure();

inline double ToNs(uint64_t cycle_count) {
  return double(cycle_count) * NsPerCycle();
}

}  // namespace mcla::gpu::cycles
