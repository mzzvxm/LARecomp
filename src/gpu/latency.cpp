// MCLA GPU renderer -- the GPU vendor's latency reducer: the policy, the frame
// bookkeeping and the calls the game and the renderer make. See latency.h for
// what it is, where each marker goes and why the game's limiter keeps the cap; the
// driver halves are latency_d3d12.cpp and latency_vulkan.cpp.
//
// Design followed (not copied): the MCLA fork's rexglue-low-latency.patch
// (include/rex/ui/low_latency.h, src/ui/low_latency.cpp), written by the same
// author; its Xenia presenters become Plume's device and swap chain here, and
// frame generation, which has no counterpart in this renderer, is left out.

#include "latency.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "backend.h"
#include "device.h"
#include "gpu.h"
#include "latency_backend.h"
#include "swap_chain.h"

REXCVAR_DEFINE_STRING(
    mcla_gpu_low_latency, "auto", "MCLA/GPU",
    "The GPU vendor's latency reducer: NVIDIA Reflex on an NVIDIA GPU, AMD Anti-Lag 2 on an AMD "
    "one. It holds the game back at the start of a frame just long enough that the frame meets a "
    "free GPU: less input lag when the GPU is the limit, at the price of some frame rate when "
    "it is. auto (Reflex on; Anti-Lag 2 off until it has been tried on a Radeon), off, on, or "
    "boost (Reflex's On + Boost: the GPU clocks stay high; Anti-Lag 2 has no boost and takes it "
    "as on). Where the GPU, the driver or the graphics API has no reducer the pause menu has no "
    "row for it and it stays off whatever this says (the log says why at start). While frame "
    "generation runs (mcla_gpu_frame_generation) it is on whatever this says. Live.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace mcla::gpu::latency {

namespace {

using Clock = std::chrono::steady_clock;

uint64_t NowUs() {
  return uint64_t(
      std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch())
          .count());
}

// --- the frame bookkeeping -------------------------------------------------------------
// Frames the pairing remembers (a power of two does not matter here: the index is id % kRing).
constexpr uint64_t kRing = 16;
// Started frames still waiting for their swap beyond which the oldest are written off. Below
// kRing, so that a waiting frame's slot has not been reused.
constexpr uint64_t kMaxWaiting = 8;
// A started frame with a newer one behind it that has waited this long for its swap was lost
// (an update the game merged into the next one, or a swap that never came).
constexpr uint64_t kLostAfterUs = 250'000;

// --- the policy ------------------------------------------------------------------------
// The first passes after the driver takes the mode make no wait: it has seen no markers of
// the frames before them (none go out while the reducer is off).
constexpr uint32_t kWarmupPasses = 2;
// A driver mode that is refused is asked again a second later, twice more, before it counts as
// not working.
constexpr uint32_t kMaxRefusals = 3;
constexpr uint64_t kRetryUs = 1'000'000;
// A wait before the frame over this is long. Three long waits among the last eight give the
// reducer up for a while: the driver is told to leave the mode. It is tried
// again after 5 s, then 10, 20, 40 and at most 60 s while it keeps failing; 30 s without a long
// wait starts the pause at 5 s again.
constexpr uint64_t kLongWaitUs = 250'000;
constexpr uint32_t kLongWaitsToGiveUp = 3;
constexpr uint64_t kFirstBackoffUs = 5'000'000;
constexpr uint64_t kMaxBackoffUs = 60'000'000;
constexpr uint64_t kHealthyUs = 30'000'000;

struct Want {
  bool on = false;
  bool boost = false;
};

// What mcla_gpu_low_latency asks for, for this reducer.
// DLSS frame generation runs and the reducer is Streamline's (UseStreamline).
std::atomic<bool> g_streamline_on{false};

Want ReadWant(const Reducer& reducer) {
  const std::string mode = REXCVAR_GET(mcla_gpu_low_latency);
  if (FrameGenerationRunning() || g_streamline_on.load(std::memory_order_relaxed)) {
    // Forced on while frame generation runs; Reflex's boost where it is asked for.
    return {true, mode == "boost" && reducer.SupportsBoost()};
  }
  if (mode == "off") {
    return {};
  }
  if (mode == "on") {
    return {true, false};
  }
  if (mode == "boost") {
    return {true, reducer.SupportsBoost()};
  }
  // auto, and anything that is not a mode.
  return {reducer.OnByDefault(), false};
}

// The update thread's own state.
struct UpdateState {
  uint64_t started = 0;  // frames numbered so far (published below)
  // The mode the driver has.
  bool applied_on = false;
  bool applied_boost = false;
  uint32_t warmup_passes = 0;
  uint32_t refusals = 0;
  uint64_t retry_at_us = 0;
  bool failed = false;  // refused kMaxRefusals times: off until the next start
  // The watchdog.
  uint8_t long_history = 0;  // bit 0 is the latest wait
  uint64_t paused_until_us = 0;
  uint64_t backoff_us = kFirstBackoffUs;
  uint64_t on_since_us = 0;
  bool sleep_failure_logged = false;
};

struct Module {
  // The reducer: set by Initialize, taken by Shutdown.
  std::mutex mutex;
  std::shared_ptr<Reducer> reducer;

  // The pause menu, from any thread.
  std::atomic<Kind> kind{Kind::kNone};
  std::atomic<bool> auto_on{false};
  std::atomic<Health> health{Health::kOk};

  // True while there is a reducer: frames are numbered and paired whether it is on or not, so
  // turning it on in the middle of a session finds the swaps and the passes in step.
  std::atomic<bool> counting{false};
  // Frames started by the update thread; the slot of frame n is n % kRing, written before the
  // count is.
  std::atomic<uint64_t> published{0};
  std::atomic<uint64_t> ring_start_us[kRing] = {};
  std::atomic<bool> ring_marked[kRing] = {};  // the reducer was on when it started
  // How many of the started frames the guest swap thread / the host render thread have paired
  // with a swap.
  std::atomic<uint64_t> guest_cursor{0};
  std::atomic<uint64_t> host_cursor{0};

  // The device's own reducer, kept while Streamline's is the one in use (UseStreamline); `epoch`
  // counts the changes of `reducer`, which the update thread sees to forget what it applied to the
  // old one. `mode_applied` and `sleeps` mirror the update thread's state for the other threads.
  std::shared_ptr<Reducer> base;
  std::atomic<uint32_t> epoch{0};
  std::atomic<bool> mode_applied{false};
  std::atomic<uint32_t> sleeps{0};

  UpdateState update;  // the update thread
  uint32_t update_epoch = 0;
  // The host render thread: the marked frame it is recording (0: none), and whether the present
  // in progress is an idle one.
  uint64_t render_id = 0;
  bool idle_present = false;

  std::shared_ptr<Reducer> Get() {
    std::lock_guard lock(mutex);
    return reducer;
  }
};

Module& M() {
  static Module module;
  return module;
}

// The next started frame the consumer behind `cursor` has not paired yet, or 0 when every started
// frame is paired (a swap the update thread did not start a frame for). `marked`: the reducer was
// on when that frame started.
uint64_t Pair(Module& m, std::atomic<uint64_t>& cursor, bool* marked) {
  const uint64_t published = m.published.load(std::memory_order_acquire);
  uint64_t done = cursor.load(std::memory_order_relaxed);
  if (done >= published) {
    return 0;
  }
  if (published - done > kMaxWaiting) {
    done = published - 1;  // the swaps fell far behind: all but the newest frame were lost
  } else if (done + 1 < published) {
    const uint64_t now = NowUs();
    while (done + 1 < published) {
      const uint64_t start = m.ring_start_us[(done + 1) % kRing].load(std::memory_order_relaxed);
      if (now <= start || now - start <= kLostAfterUs) {
        break;
      }
      ++done;
    }
  }
  const uint64_t id = done + 1;
  cursor.store(id, std::memory_order_relaxed);
  *marked = m.ring_marked[id % kRing].load(std::memory_order_relaxed);
  return id;
}

// Tells the driver the mode `on` / `boost`, and keeps what it answered.
void ApplyMode(UpdateState& u, Reducer& reducer, bool on, bool boost, uint64_t now) {
  if (reducer.SetMode(on, boost)) {
    const bool was_on = u.applied_on;
    u.applied_on = on;
    u.applied_boost = on && boost;
    M().mode_applied.store(on, std::memory_order_relaxed);
    u.refusals = 0;
    if (on && !was_on) {
      u.warmup_passes = kWarmupPasses;
      u.on_since_us = now;
      u.long_history = 0;
      REXLOG_INFO("[gpu] low latency: {} on{}", reducer.Name(), boost ? " with boost" : "");
    } else if (!on && was_on) {
      REXLOG_INFO("[gpu] low latency: {} off", reducer.Name());
    }
    return;
  }
  if (!on) {
    // The driver would not leave the mode; nothing more can be done about it.
    u.applied_on = false;
    u.applied_boost = false;
    return;
  }
  if (++u.refusals >= kMaxRefusals) {
    u.failed = true;
    REXLOG_WARN("[gpu] low latency: the driver refused {}'s mode {} times; it stays off until the "
                "game is started again",
                reducer.Name(), kMaxRefusals);
    reducer.SetMode(false, false);
    u.applied_on = false;
    u.applied_boost = false;
    return;
  }
  u.retry_at_us = now + kRetryUs;
}

// One wait of the driver's, timed by the caller. Gives the reducer up for a while when too many
// are long.
void WatchWait(UpdateState& u, Reducer& reducer, uint64_t waited_us, uint64_t now) {
  const uint64_t limit = kLongWaitUs;
  const bool is_long = waited_us > limit;
  u.long_history = uint8_t((u.long_history << 1) | (is_long ? 1u : 0u));
  if (std::popcount(u.long_history) >= int(kLongWaitsToGiveUp)) {
    u.paused_until_us = now + u.backoff_us;
    REXLOG_WARN("[gpu] low latency: {} kept the game waiting too long ({} of the last 8 frames "
                "waited over {} ms, the last {} ms); off for {} s",
                reducer.Name(), std::popcount(u.long_history), limit / 1000, waited_us / 1000,
                u.backoff_us / 1'000'000);
    u.backoff_us = std::min(u.backoff_us * 2, kMaxBackoffUs);
    u.long_history = 0;
    reducer.SetMode(false, false);
    u.applied_on = false;
    u.applied_boost = false;
  } else if (!is_long && u.on_since_us && now - u.on_since_us > kHealthyUs) {
    u.backoff_us = kFirstBackoffUs;
  }
}

}  // namespace

// ---------------------------------------------------------------------------------------
// The pause menu
// ---------------------------------------------------------------------------------------

Kind AvailableKind() {
  return M().kind.load(std::memory_order_relaxed);
}

bool AutoIsOn() {
  return M().auto_on.load(std::memory_order_relaxed);
}

Health CurrentHealth() {
  return M().health.load(std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------------------
// The guest's threads
// ---------------------------------------------------------------------------------------

void OnFrameStart() {
  Module& m = M();
  if (!m.counting.load(std::memory_order_acquire)) {
    return;
  }
  const std::shared_ptr<Reducer> reducer = m.Get();
  if (!reducer) {
    return;
  }
  UpdateState& u = m.update;
  if (const uint32_t epoch = m.epoch.load(std::memory_order_acquire); epoch != m.update_epoch) {
    // The reducer changed (UseStreamline): what the old one was told and what it refused do not
    // carry over; the new one takes the mode at once and makes no wait for the first passes.
    m.update_epoch = epoch;
    u.applied_on = false;
    u.applied_boost = false;
    u.warmup_passes = 0;
    u.refusals = 0;
    u.retry_at_us = 0;
    u.failed = false;
    u.long_history = 0;
    u.paused_until_us = 0;
    u.backoff_us = kFirstBackoffUs;
    u.sleep_failure_logged = false;
    m.mode_applied.store(false, std::memory_order_relaxed);
    m.sleeps.store(0, std::memory_order_relaxed);
  }
  const uint64_t id = u.started + 1;
  uint64_t now = NowUs();

  // The mode: what the setting asks for, less what the watchdog and the driver's refusals say.
  const Want want = ReadWant(*reducer);
  const bool paused = now < u.paused_until_us;
  const bool on = want.on && !u.failed && !paused;
  const bool boost = on && want.boost;
  if (on != u.applied_on || (on && boost != u.applied_boost)) {
    if (now >= u.retry_at_us) {
      ApplyMode(u, *reducer, on, boost, now);
    }
  }
  m.health.store(u.failed ? Health::kFailed : (paused ? Health::kPaused : Health::kOk),
                 std::memory_order_relaxed);

  // The driver's wait, before the game reads its input (none in the warm-up passes).
  if (u.applied_on) {
    if (u.warmup_passes > 0) {
      --u.warmup_passes;
    } else {
      const uint64_t before = NowUs();
      const bool slept = reducer->Sleep(id);
      now = NowUs();
      if (slept) {
        m.sleeps.fetch_add(1, std::memory_order_relaxed);
      }
      if (!slept && !u.sleep_failure_logged) {
        u.sleep_failure_logged = true;
        REXLOG_WARN("[gpu] low latency: {}'s wait before the frame did not run (no swap chain "
                    "yet, or the driver refused the call)",
                    reducer->Name());
      }
      WatchWait(u, *reducer, now - before, now);
    }
  }

  // The frame starts now: published for the swaps (guest side and host side) to pair with, then
  // marked. The slot is written before the count that makes it visible.
  now = NowUs();
  const bool marked = u.applied_on;
  m.ring_start_us[id % kRing].store(now, std::memory_order_relaxed);
  m.ring_marked[id % kRing].store(marked, std::memory_order_relaxed);
  u.started = id;
  m.published.store(id, std::memory_order_release);
  if (marked) {
    // The input sample is taken at the frame's start too: the game reads its pad in the update
    // that follows the engine timer, and nothing in a frame comes before the timer.
    reducer->SetMarker(Marker::kSimulationStart, id);
    reducer->SetMarker(Marker::kInputSample, id);
  }
}

void OnGuestSwap() {
  Module& m = M();
  if (!m.counting.load(std::memory_order_acquire)) {
    return;
  }
  bool marked = false;
  const uint64_t id = Pair(m, m.guest_cursor, &marked);
  if (!id || !marked) {
    return;
  }
  if (const std::shared_ptr<Reducer> reducer = m.Get()) {
    reducer->SetMarker(Marker::kSimulationEnd, id);
  }
}

// ---------------------------------------------------------------------------------------
// The renderer
// ---------------------------------------------------------------------------------------

void Initialize(Device& device, SwapChain& swap_chain) {
  Module& m = M();
  Shutdown();
  std::string why;
  std::shared_ptr<Reducer> reducer;
#if defined(LARECOMP_GPU_HAS_D3D12)
  if (ActiveBackend() == ::mcla::gpu::Backend::kD3D12) {
    reducer = CreateD3D12Reducer(device.device(), why);
  }
#endif
#if defined(LARECOMP_GPU_HAS_VULKAN)
  if (ActiveBackend() == ::mcla::gpu::Backend::kVulkan) {
    reducer = CreateVulkanReducer(device.device(), swap_chain.native(), why);
  }
#endif
  (void)swap_chain;
  if (!reducer) {
    REXLOG_INFO("[gpu] low latency: not available on {} ({}): {}; the setting is hidden and the "
                "feature stays off",
                device.backend_info(), device.description().name,
                why.empty() ? std::string("no reducer for this GPU") : why);
    return;
  }
  m.kind.store(reducer->IsReflex() ? Kind::kReflex : Kind::kAntiLag, std::memory_order_relaxed);
  m.auto_on.store(reducer->OnByDefault(), std::memory_order_relaxed);
  m.health.store(Health::kOk, std::memory_order_relaxed);
  m.update = UpdateState{};
  m.update_epoch = m.epoch.load(std::memory_order_relaxed);
  m.published.store(0, std::memory_order_relaxed);
  m.guest_cursor.store(0, std::memory_order_relaxed);
  m.host_cursor.store(0, std::memory_order_relaxed);
  m.render_id = 0;
  m.idle_present = false;
  {
    std::lock_guard lock(m.mutex);
    m.reducer = reducer;
  }
  m.counting.store(true, std::memory_order_release);
  REXLOG_INFO("[gpu] low latency: {} is available on {} ({}); mcla_gpu_low_latency = {} ({})",
              reducer->Name(), device.backend_info(), device.description().name,
              REXCVAR_GET(mcla_gpu_low_latency),
              ReadWant(*reducer).on ? "on" : "off");
}

void Shutdown() {
  Module& m = M();
  m.counting.store(false, std::memory_order_release);
  std::shared_ptr<Reducer> reducer;
  {
    std::lock_guard lock(m.mutex);
    reducer.swap(m.reducer);
  }
  m.kind.store(Kind::kNone, std::memory_order_relaxed);
  if (!reducer) {
    return;
  }
  // The driver is told to leave the mode (the swap chain is still there for Vulkan's to name),
  // then the vendor's objects go. A call another thread has in progress finishes first.
  if (m.update.applied_on) {
    reducer->SetMode(false, false);
  }
  reducer->Shutdown();
  std::shared_ptr<Reducer> base;
  {
    std::lock_guard lock(m.mutex);
    base.swap(m.base);
  }
  if (base) {
    base->Shutdown();  // the device's own, put aside while Streamline's was in use
  }
  g_streamline_on.store(false, std::memory_order_relaxed);
  m.mode_applied.store(false, std::memory_order_relaxed);
  m.update = UpdateState{};
}

void OnRenderFrameOpen() {
  Module& m = M();
  m.render_id = 0;
  if (!m.counting.load(std::memory_order_acquire) || m.idle_present) {
    return;
  }
  bool marked = false;
  const uint64_t id = Pair(m, m.host_cursor, &marked);
  if (!id || !marked) {
    return;
  }
  if (const std::shared_ptr<Reducer> reducer = m.Get()) {
    m.render_id = id;
    reducer->SetMarker(Marker::kRenderSubmitStart, id);
  }
}

void OnRenderSubmitted() {
  Module& m = M();
  if (!m.render_id) {
    return;
  }
  if (const std::shared_ptr<Reducer> reducer = m.Get()) {
    reducer->SetMarker(Marker::kRenderSubmitEnd, m.render_id);
  }
}

void OnPresentBegin() {
  Module& m = M();
  if (!m.render_id) {
    return;
  }
  if (const std::shared_ptr<Reducer> reducer = m.Get()) {
    reducer->SetMarker(Marker::kPresentStart, m.render_id);
  }
}

void OnPresentEnd() {
  Module& m = M();
  const uint64_t id = m.render_id;
  m.render_id = 0;
  if (!id) {
    return;
  }
  if (const std::shared_ptr<Reducer> reducer = m.Get()) {
    reducer->SetMarker(Marker::kPresentEnd, id);
  }
}

bool UseStreamline(bool on) {
  Module& m = M();
  std::shared_ptr<Reducer> old;
  std::shared_ptr<Reducer> next;
  {
    std::lock_guard lock(m.mutex);
    if (on == g_streamline_on.load(std::memory_order_relaxed)) {
      return true;
    }
    old = m.reducer;
  }
  if (on) {
    // (A device with no reducer of its own, `old` null, is fine: Streamline's Reflex is its own.)
    std::string why;
    next = CreateStreamlineReducer(why);
    if (!next) {
      REXLOG_WARN("[gpu] low latency: Reflex through Streamline cannot be used for frame generation: {}",
                  why);
      return false;
    }
  }
  // The old reducer is left (it was told its mode; the driver takes the mode away), the new one is
  // taken at the update thread's next frame start.
  if (old && m.mode_applied.load(std::memory_order_relaxed)) {
    old->SetMode(false, false);
  }
  {
    std::lock_guard lock(m.mutex);
    if (on) {
      m.base = m.reducer;
      m.reducer = next;
    } else {
      m.reducer = m.base;
      m.base.reset();
    }
    g_streamline_on.store(on, std::memory_order_relaxed);
    if (on) {
      // The frames are numbered whether a reducer is on or not (Initialize arms the counting only
      // when the device has one): with Streamline's the counting has to run too.
      m.kind.store(Kind::kReflex, std::memory_order_relaxed);
      m.auto_on.store(true, std::memory_order_relaxed);
      m.counting.store(true, std::memory_order_release);
    } else if (!m.reducer) {
      m.counting.store(false, std::memory_order_release);
      m.kind.store(Kind::kNone, std::memory_order_relaxed);
    } else {
      m.kind.store(m.reducer->IsReflex() ? Kind::kReflex : Kind::kAntiLag, std::memory_order_relaxed);
      m.auto_on.store(m.reducer->OnByDefault(), std::memory_order_relaxed);
    }
    m.epoch.fetch_add(1, std::memory_order_release);
  }
  REXLOG_INFO("[gpu] low latency: the reducer is {} now",
              on ? "NVIDIA Reflex through Streamline (DLSS frame generation requires it)"
                 : "the device's own");
  return true;
}

uint64_t RenderFrameId() {
  return M().render_id;
}

bool StreamlineRunning() {
  Module& m = M();
  return g_streamline_on.load(std::memory_order_relaxed) &&
         m.mode_applied.load(std::memory_order_relaxed) &&
         m.sleeps.load(std::memory_order_relaxed) >= 3;
}

uint64_t DetachRenderFrame() {
  Module& m = M();
  const uint64_t id = m.render_id;
  m.render_id = 0;
  return id;
}

void OnPresentBegin(uint64_t frame_id) {
  if (!frame_id) {
    return;
  }
  if (const std::shared_ptr<Reducer> reducer = M().Get()) {
    reducer->SetMarker(Marker::kPresentStart, frame_id);
  }
}

void OnPresentEnd(uint64_t frame_id) {
  if (!frame_id) {
    return;
  }
  if (const std::shared_ptr<Reducer> reducer = M().Get()) {
    reducer->SetMarker(Marker::kPresentEnd, frame_id);
  }
}

IdlePresentScope::IdlePresentScope(bool idle) : idle_(idle) {
  if (idle_) {
    M().idle_present = true;
  }
}

IdlePresentScope::~IdlePresentScope() {
  if (idle_) {
    M().idle_present = false;
  }
}

SwapChainChange::SwapChainChange() {
  Module& m = M();
  if (!m.counting.load(std::memory_order_acquire)) {
    return;
  }
  if (std::shared_ptr<Reducer> reducer = m.Get()) {
    reducer->BeginSwapChainChange();
    reducer_ = std::move(reducer);
  }
}

void SwapChainChange::Replaced(SwapChain& swap_chain) {
  if (reducer_) {
    static_cast<Reducer*>(reducer_.get())->SwapChainReplaced(swap_chain.native());
  }
}

SwapChainChange::~SwapChainChange() {
  if (reducer_) {
    static_cast<Reducer*>(reducer_.get())->EndSwapChainChange();
  }
}

SwapChainUse::SwapChainUse() {
  Module& m = M();
  if (!m.counting.load(std::memory_order_acquire)) {
    return;
  }
  if (std::shared_ptr<Reducer> reducer = m.Get()) {
    reducer->BeginSwapChainUse();
    reducer_ = std::move(reducer);
  }
}

SwapChainUse::~SwapChainUse() {
  if (reducer_) {
    static_cast<Reducer*>(reducer_.get())->EndSwapChainUse();
  }
}

}  // namespace mcla::gpu::latency
