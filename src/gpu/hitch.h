#pragma once

#include <array>
#include <atomic>
#include <cstdint>

#include "cycles.h"
#include "hitch_rules.h"

namespace mcla::gpu::hitch {

enum class Event : uint8_t {
  kPipeline, kPipelineWait, kCity, kCityBuild, kCityEvict, kTexture, kIo, kCacheSnapshot, kResolve,
  kRetire, kPace, kDeferredPresent, kCount
};

struct Cost {
  uint64_t count = 0;
  uint64_t bytes = 0;
  double ms = 0.0;
  double max_ms = 0.0;
  uint64_t vs = 0;
  uint64_t ps = 0;
};
using Costs = std::array<Cost, size_t(Event::kCount)>;

// Render-thread event counters, consumed at the guest present record.
void Note(Event event, double ms, uint64_t bytes = 0, uint64_t count = 1);
void NotePipeline(Event event, double ms, uint64_t vs, uint64_t ps);
Costs Take();

class Scope {
 public:
  explicit Scope(Event event) : start_(cycles::Now()), event_(event) {}
  ~Scope() { Note(event_, cycles::ToNs(cycles::Now() - start_) / 1e6); }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;

 private:
  uint64_t start_;
  Event event_;
};

enum class Work : uint8_t { kPipelineIo, kShaderPack, kCityRead, kCount };

struct WorkerState {
  uint64_t done_us = 0;    // total worker time through its most recent completion
  bool active = false;
};
WorkerState ReadWorker(Work work);

// Brackets work that can run outside the render thread. Its completions and
// overlap are context for a hitch.
class WorkerScope {
 public:
  explicit WorkerScope(Work work);
  ~WorkerScope();
  WorkerScope(const WorkerScope&) = delete;
  WorkerScope& operator=(const WorkerScope&) = delete;

 private:
  uint64_t start_us_;
  Work work_;
};

struct Guest {
  uint64_t fence_wait_ns = 0;
  uint64_t stream_wait_cycles = 0;
  double step_ms = 0.0;
  double capped_ms = 0.0;
};

// The game loop and the swap hook share this sample when they run on the same thread.
void NoteGameStep(double measured_ms, double capped_ms);
void NoteGuestFence(uint64_t ns);
void NoteGuestStreamWait(uint64_t cycles);
Guest TakeGameStep();

struct Frame {
  uint64_t id = 0;
  double guest_ms = 0.0;
  double guest_step_ms = 0.0;
  double capped_ms = 0.0;
  double guest_present_wait_ms = 0.0;
  double guest_fence_wait_ms = 0.0;
  double guest_stream_wait_ms = 0.0;
  double render_ms = 0.0;
  double render_work_ms = 0.0;
  double idle_ms = 0.0;
  double gpu_wait_ms = 0.0;
  double present_ms = 0.0;
  double guest_median_ms = 0.0;
  double render_median_ms = 0.0;
  WorkerState pipeline_io;
  WorkerState shader_pack;
  WorkerState city_read;
  Costs events;
};

// city_gpu_ns is zero if the slot's build timestamps could not be read.
void Log(const Frame& frame, uint64_t city_gpu_ns, uint64_t suppressed);

}  // namespace mcla::gpu::hitch
