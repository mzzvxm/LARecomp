#include "hitch.h"

#include <chrono>
#include <format>
#include <string>

#include <rex/logging/log_category.h>

namespace mcla::gpu::hitch {
namespace {

thread_local Costs g_costs{};
thread_local Guest g_guest{};

struct Worker {
  std::atomic<uint64_t> done_us{0};
  std::atomic<uint32_t> active{0};
};
constinit Worker g_workers[size_t(Work::kCount)];

uint64_t NowUs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}

const char* const kEventNames[] = {
    "pipeline", "pipeline_wait", "city_cpu", "city_build", "city_evict", "texture_upload",
    "file_io", "cache_snapshot", "resolve", "retire", "pacing", "deferred_present"};
static_assert(std::size(kEventNames) == size_t(Event::kCount));

}  // namespace

void Note(Event event, double ms, uint64_t bytes, uint64_t count) {
  Cost& cost = g_costs[size_t(event)];
  cost.count += count;
  cost.bytes += bytes;
  cost.ms += ms;
  cost.max_ms = std::max(cost.max_ms, ms);
}

void NotePipeline(Event event, double ms, uint64_t vs, uint64_t ps) {
  Cost& cost = g_costs[size_t(event)];
  if (ms >= cost.max_ms) {
    cost.vs = vs;
    cost.ps = ps;
  }
  Note(event, ms);
}

Costs Take() {
  const Costs out = g_costs;
  g_costs = {};
  return out;
}

WorkerState ReadWorker(Work work) {
  const Worker& worker = g_workers[size_t(work)];
  const bool active = worker.active.load(std::memory_order_acquire) != 0;
  return {worker.done_us.load(std::memory_order_relaxed), active};
}

WorkerScope::WorkerScope(Work work) : start_us_(NowUs()), work_(work) {
  g_workers[size_t(work_)].active.fetch_add(1, std::memory_order_release);
}

WorkerScope::~WorkerScope() {
  Worker& worker = g_workers[size_t(work_)];
  worker.done_us.fetch_add(NowUs() - start_us_, std::memory_order_relaxed);
  worker.active.fetch_sub(1, std::memory_order_release);
}

void NoteGameStep(double measured_ms, double capped_ms) {
  g_guest.step_ms = measured_ms;
  g_guest.capped_ms = capped_ms;
}

void NoteGuestFence(uint64_t ns) {
  g_guest.fence_wait_ns += ns;
}

void NoteGuestStreamWait(uint64_t cycles) {
  g_guest.stream_wait_cycles += cycles;
}

Guest TakeGameStep() {
  const Guest out = g_guest;
  g_guest = {};
  return out;
}

void Log(const Frame& f, uint64_t city_gpu_ns, uint64_t suppressed) {
  std::string events;
  for (size_t i = 0; i < f.events.size(); ++i) {
    const Cost& c = f.events[i];
    if (c.count == 0) continue;
    events += std::format(" {}={}x/{:.2f}ms(max={:.2f})/{:.2f}MiB", kEventNames[i], c.count,
                          c.ms, c.max_ms, double(c.bytes) / 1048576.0);
    if (c.vs != 0) events += std::format("[vs={:016X},ps={:016X}]", c.vs, c.ps);
  }
  const auto worker = [&](const char* name, WorkerState state) {
    if (state.active || state.done_us != 0) {
      events += std::format(" {}={}/{:.2f}ms_completed", name, state.active ? "active" : "done",
                            double(state.done_us) / 1000.0);
    }
  };
  worker("pipeline_io_worker", f.pipeline_io);
  worker("shader_pack_load", f.shader_pack);
  worker("city_read_decode", f.city_read);
  if (f.events[size_t(Event::kCityBuild)].count != 0) {
    events += city_gpu_ns ? std::format(" city_gpu={:.2f}ms", double(city_gpu_ns) / 1e6)
                          : " city_gpu=unavailable";
  }
  const double guest_other = std::max(0.0, f.guest_ms - f.guest_present_wait_ms -
                                     f.guest_fence_wait_ms - f.guest_stream_wait_ms);
  REXLOG_INFO("[hitch] frame={} guest_swap={:.2f}ms(median={:.2f}) guest_step={:.2f}ms "
              "step_cap={:.2f}ms guest_prev_present_wait={:.2f}ms guest_fence_wait={:.2f}ms guest_stream_wait={:.2f}ms "
              "guest_other={:.2f}ms "
              "render_interval={:.2f}ms(median={:.2f}) render_work={:.2f}ms render_idle={:.2f}ms "
              "gpu_wait={:.2f}ms present={:.2f}ms suppressed={} events={}",
              f.id, f.guest_ms, f.guest_median_ms, f.guest_step_ms, f.capped_ms,
              f.guest_present_wait_ms, f.guest_fence_wait_ms, f.guest_stream_wait_ms, guest_other, f.render_ms, f.render_median_ms,
              f.render_work_ms, f.idle_ms, f.gpu_wait_ms, f.present_ms, suppressed,
              events.empty() ? "none" : events);
}

}  // namespace mcla::gpu::hitch
