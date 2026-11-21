#pragma once
// ===========================================================================
// MCLA GPU renderer -- the ray tracing micro-benchmark, the GPU side
// ===========================================================================
// rt_bench_rules.h says what the benchmark is and decides around it; this is the part that needs a device. RunIfWanted, called once by the renderer's start
// (Renderer::Initialize, after the car ray tracing's support check and before the host precache starts, so the GPU is idle and nothing else submits):
//   1. asks gpu_class::RtBenchWanted (a card that can trace, a real discrete one, a class above Low) and the setting mcla_gpu_rt_bench (auto | off | force);
//   2. reads the cache, rt_bench.txt next to larecomp.toml (rt_bench_rules.h: valid for one card, driver, API and benchmark version);
//   3. when it has to run: builds the synthetic city (a bottom level of about 7,000 triangles, a top level of 16 instances of it) into acceleration structures,
//      makes the compute pipeline of hlsl/rt_bench_cs.hlsl, times one warm-up dispatch of a million rays, plans the timed runs from its time (PlanRuns) and times
//      them, reads the two counters back (rays traced and rays that hit: they prove the dispatches ran and the rays found the city), and takes the rate of the
//      fastest run (Summarize); writes the cache; logs "[gpu] rt bench: ...";
//   4. hands the result to gpu_class::NoteRtBench, which applies the decision (rt_tier.h ApplyBench) and logs the tier.
// It takes about 100 to 250 ms once (a warm-up and up to three runs in the 150 ms budget, and the structures and the pipeline), and nothing at the starts after.
// Every step is checked: a failure logs why, makes no measurement, leaves the table's tier alone and costs the start nothing more; the structures,
// buffers and pipeline are given back before it returns, after the GPU is idle (every submit is waited for).
//
// Render thread / lifecycle thread, before the first frame. CHECK: written without a build or a run (no compiler in the loop but the one that checked the
// shader): the first run on D3D12 and on Vulkan, on the debug layer, is the proof (the log line says the rate; mcla_gpu_rt_bench=force repeats it).
// ===========================================================================

#include "rt_bench_rules.h"

namespace mcla::gpu {

class Device;

namespace rt_bench {

// Decides (the setting, the cache, the device), runs if it has to, tells the tier rule. The result is also returned for the caller's log; Outcome::kNotRun
// when nothing ran and nothing was cached.
Result RunIfWanted(Device& device);

}  // namespace rt_bench

}  // namespace mcla::gpu
