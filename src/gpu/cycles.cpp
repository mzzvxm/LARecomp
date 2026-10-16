// MCLA GPU renderer -- a cheap clock for per-draw statistics. See cycles.h.

#include "cycles.h"

#include <chrono>

namespace mcla::gpu::cycles {

namespace {

uint64_t g_last_cycles = 0;
uint64_t g_last_ns = 0;
double g_ns_per_cycle = 0.0;

uint64_t SteadyNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

}  // namespace

double NsPerCycle() {
  return g_ns_per_cycle;
}

void Measure() {
  const uint64_t c = Now();
  const uint64_t ns = SteadyNs();
  if (g_last_cycles != 0 && c > g_last_cycles && ns > g_last_ns) {
    g_ns_per_cycle = double(ns - g_last_ns) / double(c - g_last_cycles);
  }
  g_last_cycles = c;
  g_last_ns = ns;
}

}  // namespace mcla::gpu::cycles
