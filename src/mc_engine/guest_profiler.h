#pragma once

// Sampling profiler for the recompiled guest code.
//
// Every measurement so far has told us what the dense-area slowdown is NOT:
// not GPU-bound (fence wait at zero), not streaming (texture cache misses at
// zero), not shader compilation, not lock contention, and only ~70 draw calls
// per frame. That leaves the recompiled guest code, and nothing we had could
// say which part of it.
//
// This answers that. A background thread suspends a handful of threads on a
// timer, reads their instruction pointers, and resumes. Addresses are stored
// raw and resolved only when a report is written, so the sampling loop stays
// cheap.
//
// Two things about the design are worth knowing before reading a report:
//
// - It samples up to four threads, not one. The thread that drives frames is
//   pinned (MCLA_PROFILE_THREAD overrides which one); the rest are re-chosen
//   every report from the per-thread CPU table, because the busy thread
//   changes with what the game is doing and usually does not exist yet when
//   the first frame ticks. Sampling only the frame thread produced reports
//   that were 70% "blocked in a wait" and said nothing about who it waited on.
//
// - Guest addresses come from the codegen function table (PPCFuncMappings),
//   not from symbols, so they are correct on a machine that has no PDB next to
//   the exe. That is the normal case for a profile taken by somebody else.
//
// Off unless MCLA_PROFILE=1 or the guest_profile cvar is set. When off, Tick()
// is one already-resolved bool test and no thread is ever created.

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
