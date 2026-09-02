#pragma once

// Sampling profiler for the recompiled guest code.
//
// Every measurement so far has told us what the dense-area slowdown is NOT:
// not GPU-bound (fence wait at zero), not streaming (texture cache misses at
// zero), not shader compilation, not lock contention, and only ~70 draw calls
// per frame. That leaves the recompiled guest code, and nothing we had could
// say which part of it.
//
// This answers that. The recompiled functions are ordinary native code with
// real PDB symbols named rex_sub_82XXXXXX, so a host sampling profiler
// attributes time straight back to guest addresses that can be opened in the
// IDB. A background thread suspends the guest's main thread on a timer, reads
// its instruction pointer, and resumes. Addresses are stored raw and resolved
// only when a report is written, so the sampling loop stays cheap.
//
// Off unless MCLA_PROFILE=1. When off, Tick() is one already-resolved bool
// test and no thread is ever created.

#include <cstdint>

namespace mc::profiler {

// True when MCLA_PROFILE=1. Resolved once.
bool Enabled();

// Call from the per-frame guest hook. The first call adopts the calling thread
// as the sample target and starts the sampler; later calls drive periodic
// reports and record the frame time so slow frames can be reported separately.
void Tick(double frame_ms);

// Write a report immediately. Called on shutdown; safe to call when disabled.
void Report(const char* reason);

// Stop sampling and write a final report.
void Shutdown();

}  // namespace mc::profiler
