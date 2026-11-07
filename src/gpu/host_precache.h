#pragma once
// ===========================================================================
// MCLA GPU renderer -- the renderer's own pipelines, made ahead of first use
// ===========================================================================
// The pipelines of the guest's draws are made ahead of play (pipelines.h). The
// renderer's own -- the resolve conversions (resolve.h) and the resolve
// memory's decode (resolve_memory.h) -- used to be made on the render thread
// the first time a frame asked for one: a driver compile between two guest
// frames (a guest pipeline cost 20 to 150 ms there on an RTX 5080 with a cold
// driver cache; these were not timed one by one).
//
// What these pipelines can be is a closed set. A resolve's is picked by
// (depth or colour source, multisampled source or not, destination format), a
// reduction's and a decode's by the destination format alone, and the formats
// are the few format.h gives the resolve and decode paths. So their owners list
// what they can name at start -- each entry a key of the owner's, a name for the
// log and a function that makes the pipeline -- and the Renderer starts ONE
// builder thread when every owner has listed. It makes the entries in the order
// listed (the likeliest first), below normal priority on Windows, and ends with
// the list. A pipeline an owner needs that nobody listed is made by that owner
// as before, on the render thread, and counted.
//
// Asking (Acquire: an owner's first use of a key, which is its own map's miss):
//  - made: taken, no wait;
//  - being made: waited for, and the builder is raised to normal priority at
//    the first wait (a render thread must not wait for a thread that a busy
//    machine starves);
//  - not begun: the render thread makes it itself with the entry's function,
//    and the builder skips it;
//  - failed (Device logged why): null, and the entry is not tried again;
//  - not listed, or already given out: made by the function the owner passes.
// Nothing is made twice: an entry's state moves queued -> building -> ready or
// failed -> taken, or queued -> claimed, and only the move out of queued is
// contested (compare-exchange).
//
// What was made ahead and what the render thread still made at first use is
// logged once, when the list is stopped (Stop): the counts, then the names of
// the pipelines used, never asked for, and not listed. The line the builder
// writes when it is done gives the time it took.
//
// Only closed sets are listed. A variant whose format a device may refuse to
// render to (B5G5R5A1, an optional render target in Vulkan) stays with its
// owner: a pipeline made for a format nobody used would log a failure.
//
// Threading: Add, Start and Stop on the lifecycle thread (Stop also from the
// owners' Shutdown, after the render thread has stopped); Acquire on the render
// thread. A listed function runs on the builder thread, or on the render
// thread for a claimed entry, never both; what it reads must exist before Add
// and until Stop. Creating pipelines from a second thread is what the guest
// pipelines' precache workers do already (device.h): in the pinned Plume the
// D3D12 graphics pipeline takes no shared state and the Vulkan one keeps its
// scratch vectors thread_local and passes no pipeline cache.
// ===========================================================================

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <plume_render_interface.h>

namespace mcla::gpu {

class HostPrecache {
 public:
  // Makes one pipeline; null on failure (Device logs it).
  using Make = std::function<std::unique_ptr<plume::RenderPipeline>()>;

  // Whose entry a key is: the owner in the high word, its variant in the low.
  enum Owner : uint32_t {
    kResolve = 1,        // ResolveCache::Pipeline: depth << 9 | msaa << 8 | format
    kResolveReduce = 2,  // ResolveCache::ReducePipeline: format
    kResolveDecode = 3,  // ResolveMemory::DecodePipeline: format
    kResolveFused = 4,   // ResolveMemory::FusedPipeline: fractional << 9 | msaa << 8 | format
    kAmbientOcclusion = 5,  // AmbientOcclusion: its pass and variant (ao.h Variant)
  };
  static constexpr uint64_t Key(Owner owner, uint32_t variant) {
    return (uint64_t(owner) << 32) | variant;
  }

  HostPrecache() = default;
  ~HostPrecache();
  HostPrecache(const HostPrecache&) = delete;
  HostPrecache& operator=(const HostPrecache&) = delete;

  // Lists a pipeline to make, after the entries listed so far. `key` is unique
  // (a repeat is ignored). Before Start.
  void Add(uint64_t key, std::string name, Make make);
  // Starts the builder on what is listed (nothing is started for an empty
  // list). Once.
  void Start();
  // Ends the builder (a build under way finishes), logs the summary, and drops
  // the list and the pipelines nobody took: the GPU must be idle. Safe to call
  // again, and with nothing listed.
  void Stop();

  // The pipeline for `key` (see the note at the top), null when it failed.
  // `name` and `make_unlisted` serve a key nobody listed (or one already given
  // out): they are the owner's own creation. Render thread.
  std::unique_ptr<plume::RenderPipeline> Acquire(uint64_t key, const std::string& name,
                                                 const Make& make_unlisted);

 private:
  enum State : uint8_t { kQueued, kBuilding, kReady, kFailed, kClaimed, kTaken };
  struct Entry {
    uint64_t key = 0;
    std::string name;
    Make make;
    std::atomic<uint8_t> state{kQueued};
    std::unique_ptr<plume::RenderPipeline> pipeline;  // mutex_: set with kReady
    uint64_t build_ns = 0;  // the time the make function took (mutex_ for the builder's)
    bool used = false;      // render thread
  };

  void Run();
  void Summarize();

  std::vector<std::unique_ptr<Entry>> entries_;
  std::unordered_map<uint64_t, Entry*> by_key_;  // built before Start, then only read
  std::thread builder_;
  std::atomic<bool> stop_{false};
  std::mutex mutex_;
  std::condition_variable done_;  // an entry left kBuilding
  std::chrono::steady_clock::time_point start_;

  // The builder's.
  std::atomic<uint32_t> built_{0};
  std::atomic<uint32_t> failed_{0};
  std::atomic<uint32_t> skipped_{0};  // claimed by the render thread first

  // The render thread's.
  struct Stats {
    uint32_t taken = 0;    // made ahead, asked for
    uint32_t waited = 0;   // ... of those, still being made
    uint64_t wait_ns = 0;
    uint32_t claimed = 0;  // listed, not begun: made on the render thread
    uint64_t claimed_ns = 0;
    uint32_t unlisted = 0;  // not listed: made on the render thread
    uint64_t unlisted_ns = 0;
    uint32_t failed = 0;   // of the two above, made here and failed
  };
  Stats stats_;
  bool raised_ = false;
  std::vector<std::string> unlisted_names_;
};

}  // namespace mcla::gpu
