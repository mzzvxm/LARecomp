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
// Only closed sets are listed. The resolve copies' list is the conversion table's own
// (resolve_variants.h), so every destination the pass can convert into is listed, B5G5R5A1
// (k_1_5_5_5) included: its multisampled copy was once left out as a format a device may refuse
// to render to, and two runs of the 2026-11-17 starter verify round made it on the render thread.
// A device that does refuse it logs one failed entry from the builder; no copy ever asks for it
// there (the product texture is not made). The decode (resolve_memory.h) still keeps B5G5R5A1
// for itself: no log of a measured drive has a decode into it.
//
// The graphics settings' passes (ambient occlusion, contact shadows, sun shafts, bounce light) are listed
// only when the settings in force at the start use them (host_plan.h says which), and each owner says
// what it listed and why with Note: Start writes one log line with the count and the words of every
// note. A pass whose setting is switched on later lists its pipelines with AddLive, which makes them
// on a worker thread of their own while the game runs; the owner draws without the pass until Ready
// says every pipeline is made (a frame never waits for a compile), as perf/boot-lazy-pipelines does
// for the anti-aliasing and FSR1 pipelines with its BackgroundBuild. Acquire still serves what is
// made, takes nothing from a pipeline that is being made, and a pipeline the render thread has to
// make itself (listed but not begun, or not listed) is logged as it happens.
//
// Threading: Add, Note, Start and Stop on the lifecycle thread (Stop also from the
// owners' Shutdown, after the render thread has stopped); Acquire, AddLive and Ready on the render
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
    kSunShafts = 6,         // SunShafts: its pass and variant (shafts.h Variant)
    kVolumetrics = 7,       // Volumetrics: its compute passes and the composite per sample count (volumetrics.h Variant)
    kCarSelfRefl = 12,      // CarSelfRefl: its history pass (car_selfrefl.h Pass); the numbers between are left for other owners
    kCarShadowMap = 13,     // CarShadowMap: its matrix pass and the depth-only pipelines of the car's vertex formats (car_shadow_map.h VariantKey)
    kCarRtRefl = 21,        // CarRtRefl: its ray traced compute pass (car_rt_refl.h)
    kLocalShadow = 22,      // LocalShadow: the depth-only pipelines of the headlight shadow replay, one per (vertex shader, layout, faces...) (local_shadow.h)
    kScreenShared = 23,     // ScreenShared: its Hi-Z pass (screen_shared.h)
    kScreenReflections = 24,  // ScreenReflections: its trace, accumulation and resolve passes (ssr.h)
  };
  static constexpr uint64_t Key(Owner owner, uint32_t variant) {
    return (uint64_t(owner) << 32) | variant;
  }

  HostPrecache() = default;
  ~HostPrecache();
  HostPrecache(const HostPrecache&) = delete;
  HostPrecache& operator=(const HostPrecache&) = delete;

  // Lists a pipeline to make, after the entries listed so far. `key` is unique
  // (a repeat is ignored). Before Start. True when it was listed.
  bool Add(uint64_t key, std::string name, Make make);
  // Says, for the start's log line, what a setting-driven owner listed: `text` names the pass, its
  // level and why (host_plan.h Describe), `listed` is how many pipelines it added (0: none, made in the
  // background if the setting is switched on). Before Start.
  void Note(std::string text, uint32_t listed);
  // Starts the builder on what is listed (nothing is started for an empty
  // list) and writes the line that says what was listed and why. Once.
  void Start();
  // Ends the builder (a build under way finishes), logs the summary, and drops
  // the list and the pipelines nobody took: the GPU must be idle. Safe to call
  // again, and with nothing listed.
  void Stop();

  // One pipeline of a late list.
  struct Item {
    uint64_t key = 0;
    std::string name;
    Make make;
  };
  // A pass whose setting was off at the start and is on now: lists its pipelines (the keys not listed
  // before; a repeat is ignored) and starts a worker thread of their own that makes them in the order
  // given, below normal priority on Windows. Returns how many were new (0: nothing started). The render
  // thread, after Start or without one; the owner has made what the functions read before the call.
  uint32_t AddLive(std::string pass, std::vector<Item> items);
  // True when no worker is making or about to make `key` any more: it is made (Acquire takes it with no
  // wait), failed (Acquire says null and Device logged why), or already taken; also true for a key nobody
  // listed. False while it waits in the list or is being made. Render thread.
  bool Ready(uint64_t key) const;

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

  enum class Built : uint8_t { kMade, kFailed, kClaimed };
  // Makes one entry unless the render thread claimed it first. Builder threads.
  Built BuildEntry(Entry& entry);
  void Run();
  // The worker of one AddLive.
  void RunLive(const std::string& pass, const std::vector<Entry*>& list,
               std::chrono::steady_clock::time_point began);
  // Logs a pipeline the render thread had to make (the first kRenderLogMax).
  void NoteRenderThread(const std::string& name, const char* why, uint64_t ns);
  void Summarize();

  struct Noted {
    std::string text;
    uint32_t listed = 0;
  };
  static constexpr uint32_t kRenderLogMax = 32;

  std::vector<std::unique_ptr<Entry>> entries_;
  std::vector<Noted> notes_;
  // The late lists' entries and workers (AddLive): render thread; joined in Stop. entries_ is read by
  // the builder while they are added, so they have a vector of their own.
  std::vector<std::unique_ptr<Entry>> live_entries_;
  std::vector<std::thread> live_builders_;
  // Built before Start by Add, then touched only by the render thread (Acquire, AddLive, Ready).
  std::unordered_map<uint64_t, Entry*> by_key_;
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
  uint32_t render_logged_ = 0;
  std::vector<std::string> unlisted_names_;
};

}  // namespace mcla::gpu
