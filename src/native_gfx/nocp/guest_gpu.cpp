#ifndef REXGLUE_HAS_XEO3_TARGET
// MCLA Native Graphics Runtime — the guest-visible GPU, with no emulator.
// See guest_gpu.h for what this replaces and why.

#include "guest_gpu.h"
#include "settings/immortal.h"
#include "nocp_app.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <vector>

#include <rex/chrono.h>
#include "settings/larecomp_settings.h"
#include <rex/guest.h>
#include <rex/title.h>
#include <rex/xenos/register.h>
#include <rex/logging/log_category.h>
#include <rex/kernel/title_runtime.h>
#include <rex/memory/mapped_ptr.h>
#include <stop_token>
#include <rex/platform/thread.h>

#include <cstring>

#include "../diag.h"
#include "../guest/guest_fence.h"
#include "../guest/guest_resources.h"
#include "../guest/vblank_probe.h"

// Defined in nocp_app.cpp, at global scope like every cvar here.

namespace mcla::native_gfx::nocp {

namespace {

// Registers, as (address & 0xFFFF) / 4. Named after what the SDK calls them.
constexpr uint32_t kRegPrimarySurfaceAddress = 0x1844;  // the flip
constexpr uint32_t kRegCpRbWptr = 0x01C5;               // ring write pointer

// Same size as the SDK's RegisterFile, so an unknown register reads back what
// was last written to it instead of faulting.
constexpr uint32_t kRegisterCount = 0x5003;

// The guest interrupt callback's two arguments, from
// GraphicsSystem::DispatchInterruptCallback: source 0 is the vblank, and the
// callback data is the D3DDevice the guest registered with.
constexpr uint32_t kInterruptSourceVblank = 0;
constexpr uint32_t kInterruptCpu = 2;

struct State {
  std::vector<uint32_t> registers;
  std::atomic<uint32_t> front_buffer{0};
  std::atomic<uint64_t> flips{0};
  std::atomic<uint64_t> vblanks{0};
  std::atomic<uint64_t> ring_kicks{0};  // CP_RB_WPTR writes, which should be 0
  std::atomic<uint64_t> fence_publishes{0};
  std::atomic<uint32_t> last_issued{0};
  std::atomic<uint32_t> last_retired{0};
  // Where the guest is stuck, when it is. dev+11008 is the flag
  // D3DDevice_BlockUntilIdle spins on; the write pointer and the kick limit
  // say whether it is still filling a command buffer at all.
  std::atomic<uint32_t> idle_spin{0};
  std::atomic<uint64_t> ring_publishes{0};
  std::atomic<uint32_t> last_consumed{0};
  // The ring allocator's whole wait condition, sampled from the same read that
  // publishes it. sub_82411180 spins while the writeback's low two bits
  // disagree with the guest's own wrap counter or while the position it wants
  // is past what has been consumed, so without these four the report cannot
  // tell "the guest is waiting on the ring" from "the guest is waiting on
  // something else entirely".
  std::atomic<uint32_t> ring_wrap{0};       // dev+14920 & 3
  std::atomic<uint32_t> ring_end{0};        // dev+14908, the command buffer end
  std::atomic<uint32_t> wb_fence{0};        // *(block + 0), the retired fence
  std::atomic<uint32_t> wb_consumed{0};     // *(block + 4), ring consumption
  std::atomic<uint32_t> wb_alloc_consumed{0};  // *(block + 60), allocator read cursor
  std::atomic<uint32_t> write_ptr{0};
  std::atomic<uint32_t> kick_limit{0};
  // Who writes the retired fence (guest_gpu.h). Guarded by fence_mutex, which
  // every write of the value is made under.
  std::mutex fence_mutex;
  bool fence_acquired = false;
  std::atomic<bool> fence_acquired_view{false};  // for reports, not for decisions
  std::atomic<uint64_t> owned_publishes{0};
  std::atomic<bool> installed{false};
  std::atomic<bool> worker_running{false};
  rex::IGuestInterrupts* interrupts = nullptr;  // from the driver's AttachTitle
  rex::VideoMode video_mode;
  rex::ScopeGuard worker;  // StartHostThread's guard: destroyed = stopped and joined
};

// The host base of the title's guest memory: valid from TitleRuntime::Load until Unload (GuestMembase::GetBase, in
// rex/memory/mapped_ptr.h).
uint8_t* GuestMembaseNow() {
  rex::kernel::TitleRuntime* runtime = rex::kernel::GetProcessRuntime();
  return runtime ? runtime->GetMembase().GetBase() : nullptr;
}

State& state() {
  static State& s = larecomp::Leak<State>();
  return s;
}

uint32_t ReadRegisterIndex(uint32_t r) {
  State& s = state();
  // The registers that report the display mode or a fixed value (EDRAM timing, BC control, the scanline, the
  // interrupt status with bit 0 set, the viewport size) are the SDK's own table now; the values are the ones
  // this file used to return.
  if (const auto fixed = rex::xenos::ReadFixedRegister(r, s.video_mode)) {
    return *fixed;
  }
  return r < s.registers.size() ? s.registers[r] : 0;
}

void WriteRegisterIndex(uint32_t r, uint32_t value) {
  State& s = state();
  switch (r) {
    case kRegPrimarySurfaceAddress:
      // THE FLIP. The guest's vblank handler writes the front buffer address
      // here when a queued flip retires. The emulator discards it; here it is
      // the present signal, and it arrives with the address of exactly what
      // the game wants on screen.
      s.front_buffer.store(value, std::memory_order_release);
      s.flips.fetch_add(1, std::memory_order_relaxed);
      break;
    case kRegCpRbWptr:
      // The ring write pointer. With the ring dead there is no consumer and
      // there should be no writer either -- counted so a nonzero value says
      // the ring is still alive somewhere.
      s.ring_kicks.fetch_add(1, std::memory_order_relaxed);
      break;
    default:
      break;
  }
  if (r < s.registers.size()) {
    s.registers[r] = value;
  }
}

// Retires every fence the guest has handed out.
//
// The guest's D3DDevice keeps a pair: `issued` at +10908, which it increments
// itself, and `retired` behind the pointer at +10896, which on real hardware
// the GPU writes back. D3DDevice_BlockOnFence spins until retired catches up,
// and D3DResource_Lock, D3DResource_Destroy, BlockUntilIdle and the swap all
// go through it.
//
// With no command processor there is no GPU to write it, and measured: the
// title kicked the ring four times and then stopped dead, spinning on a value
// nothing would ever move.
//
// Publishing retired = issued is not a lie here. It says "everything handed
// out has completed", and in this mode nothing was ever handed to any GPU at
// all -- the ring goes nowhere. This is also the write that could never be
// done while the emulator was alive, because the command processor owned it
// and two writers would race. Being the only writer is what makes it correct,
// which is why it stops while a renderer holds the write
// (AcquireFenceRetirement): that renderer does execute the guest's work, and
// retiring ahead of it would let the guest reuse memory it has not read yet.
void RetireGuestFences(uint8_t* base, uint32_t device_va) {
  State& s = state();
  const GuestFenceState fence = ReadFenceState(base, device_va);
  if (!fence.valid) {
    return;
  }
  s.last_issued.store(fence.issued, std::memory_order_relaxed);
  s.last_retired.store(fence.retired, std::memory_order_relaxed);
  // Sampled from the same read, so the report says WHERE the guest is rather
  // than leaving it to be inferred from what stopped moving.
  if (IsGuestRangeReadable(device_va, 11012)) {
    const auto load_be = [&](uint32_t off) {
      uint32_t v;
      std::memcpy(&v, rex::memory::GuestPtr(base, device_va + off), 4);
      return __builtin_bswap32(v);
    };
    s.write_ptr.store(load_be(48), std::memory_order_relaxed);
    s.kick_limit.store(load_be(56), std::memory_order_relaxed);
    s.idle_spin.store(load_be(11008), std::memory_order_relaxed);
  }
  if (fence.retired == fence.issued) {
    return;
  }
  std::lock_guard lock(s.fence_mutex);
  if (s.fence_acquired) {
    return;
  }
  PublishRetiredFence(base, device_va, fence.issued);
  s.fence_publishes.fetch_add(1, std::memory_order_relaxed);
}

// Retires the ring itself.
//
// The block behind dev+10896 holds hardware writebacks and the consumer cursors
// polled by the guest ring allocators:
//
//   *(block + 0) = dev[10908] - 2;                 // the retired fence
//   *(block + 4) = dev[14920] & 3 | dev[48];       // ring consumption
//
// Both +4 and +60 are polled by ring-space waits: sub_82411180 reads +4, while
// sub_82411218 reads +60. Advancing only +4 strands the latter even after the
// fence and the other allocator's cursor have caught up.
//
// Publishing "consumed everything" is true here for the same reason retiring
// every fence is: the ring goes nowhere. dev+14908 is the end of the command
// buffer and sub_82411640 clamps every request to it, so it is the ceiling no
// request can exceed. The wrap counter is copied from the guest's own
// dev+14920 so the two always agree.
//
// +60 is a different unit: the primary ring's read pointer, a dword index the
// hardware writes back (the RPtr writeback), which sub_82411218 compares with
// the write index dev+10952 masked by dev+14900 before it appends a kick's
// INDIRECT_BUFFER packets (sub_82411928, which then stores the new index in
// CP_RB_WPTR). "Consumed everything" there is read == write. It used to get
// the command buffer address above: harmless while the write index has not
// wrapped (the address compares above every index, so the wait returns at
// once), an endless spin once it has -- 8192 dwords at 3 per kick, which the
// boot movies reach when they run uncapped (frame 1361, 2728
// kicks).
void RetireRingConsumption(uint8_t* base, uint32_t device_va) {
  State& s = state();
  if (!IsGuestRangeReadable(device_va, 14912)) {
    return;
  }
  const auto load_be = [&](uint32_t ea) {
    uint32_t v;
    std::memcpy(&v, rex::memory::GuestPtr(base, ea), 4);
    return __builtin_bswap32(v);
  };
  const uint32_t block = load_be(device_va + 10896);
  if (block < 0x1000u || !IsGuestRangeReadable(block, 64)) {
    return;
  }
  const uint32_t wrap = load_be(device_va + 14920) & 3u;
  const uint32_t buffer_end = load_be(device_va + 14908);
  // Sampled before every early return: the frames where nothing is published
  // are exactly the frames a stuck guest is spinning through, so those are the
  // ones whose values the report has to carry.
  s.ring_wrap.store(wrap, std::memory_order_relaxed);
  s.ring_end.store(buffer_end, std::memory_order_relaxed);
  s.wb_fence.store(load_be(block + 0), std::memory_order_relaxed);
  s.wb_consumed.store(load_be(block + 4), std::memory_order_relaxed);
  s.wb_alloc_consumed.store(load_be(block + 60), std::memory_order_relaxed);
  if (!buffer_end) {
    return;
  }
  const uint32_t consumed = (buffer_end & ~3u) | wrap;
  const uint32_t ring_read = load_be(device_va + 10952);  // the primary ring's write index
  bool published = false;
  for (const auto [offset, value] : {std::pair{4u, consumed}, std::pair{60u, ring_read}}) {
    if (load_be(block + offset) == value) {
      continue;
    }
    const uint32_t be = __builtin_bswap32(value);
    std::memcpy(rex::memory::GuestPtr(base, block + offset), &be, 4);
    published = true;
  }
  if (published) {
    s.last_consumed.store(consumed, std::memory_order_relaxed);
    s.ring_publishes.fetch_add(1, std::memory_order_relaxed);
  }
}

// Delivers one vblank to the guest: the SDK calls the graphics interrupt callback the title registered
// (VdSetGraphicsInterruptCallback: sub_82411478 with the D3DDevice as its context, the same pair this file used
// to call by hand) on this thread, source 0 = vblank, on guest CPU 2, as the SDK's own vsync thread does.
// The guest does register the pair: D3DDevice_InitializeEngines (sub_82426468) calls VdSetGraphicsInterruptCallback(
// sub_82411478, device) (native_gfx/hooks.cpp), the kernel export stores both unconditionally
// (FunctionDispatcher::SetGraphicsInterrupt, src/kernel/xboxkrnl/xboxkrnl_video.cpp: no graphics system is involved any
// more, which 0.10 needed), and RaiseGraphicsInterrupt calls callback(source, user_data) = sub_82411478(0, device) on this
// thread: the same call the old dispatch made with the constant address. Until the guest has registered, it does nothing.
// KernelThread::SetActiveCPU(2) (RaiseGraphicsInterrupt does it on every call) logs "Too few processor cores" only ONCE
// (REXSYS_WARN_ONCE) on a PC with fewer than 6 logical processors, so the 0.10 log flood is gone; with 6 or more it sets the
// thread affinity mask on every call, up to 1000 syscalls a second with vsync off. CHECK: measure that against the 0.10
// pin-once (the SDK's own vsync thread pays the same).
void DispatchVblank() {
  State& s = state();
  if (!s.interrupts) {
    return;
  }
  if (auto raised = s.interrupts->RaiseGraphicsInterrupt(kInterruptSourceVblank, kInterruptCpu); !raised) {
    REXLOG_ERROR("[nocp] graphics interrupt refused: {}", raised.error().message);
  }
}

void VblankWorker(std::stop_token stop) {
  State& s = state();
  const double refresh_hz = std::max(1.0, double(s.video_mode.refresh_rate));
  const uint64_t tick_frequency = rex::Clock::GetGuestTickFrequency();
  const uint64_t vsync_interval = std::max(uint64_t(1), uint64_t(double(tick_frequency) / refresh_hz));
  // Vsync off runs at 1000 Hz, not uncapped: measured on MCLA at 910/s, and
  // the game's pacing is built on the rate.
  const uint64_t free_interval = std::max(uint64_t(1), tick_frequency / 1000);

  REXLOG_INFO(
      "[nocp] vblank source: {} Hz (mcla_native_gfx_nocp_vsync={}). The SDK's `vsync` cvar is "
      "defined in the GPU plugin and is absent in this mode, so a `vsync` line in larecomp.toml "
      "does nothing here.",
      larecomp::settings::mcla_native_gfx_nocp_vsync.Get() ? refresh_hz : 1000.0,
      larecomp::settings::mcla_native_gfx_nocp_vsync.Get() ? "true" : "false");

  uint32_t report_ticks = 0;
  uint64_t last = rex::Clock::QueryGuestTickCount();
  while (!stop.stop_requested()) {
    const uint64_t now = rex::Clock::QueryGuestTickCount();
    // Our own cvar, not the SDK's `vsync`: that one is defined in
    // src/graphics/command_processor.cpp, so it lives in the GPU plugin and
    // does not exist in this mode. Querying it by name would have answered
    // false no matter what the config said -- Query<bool> on an unregistered
    // name returns the default -- which is a silent wrong answer, not an error.
    const uint64_t interval =
        larecomp::settings::mcla_native_gfx_nocp_vsync.Get() ? vsync_interval : free_interval;
    // The guest clock is not monotonic across two reads: it re-bases its
    // snapshot, and truncation can publish a baseline one tick lower. Every
    // value here is unsigned, so a single backwards step made the difference
    // wrap to ~1.8e19 and the loop dispatched interrupts forever -- measured
    // at 11M/s against a healthy ~1k/s, which collapsed the runtime to about
    // 1 FPS with no recovery. Clamp instead of wrapping.
    if (now < last) {
      last = now;
    }
    uint64_t pending = (now - last) / interval;
    // A stall (loading, alt-tab, a shader compile hitch) leaves a backlog of
    // one vblank per elapsed millisecond. Draining it one interrupt at a time
    // only starves the guest further.
    constexpr uint64_t kMaxCatchUp = 4;
    if (pending > kMaxCatchUp) {
      last += (pending - kMaxCatchUp) * interval;
      pending = kMaxCatchUp;
    }
    const uint32_t device = GraphicsDeviceVa();
    uint8_t* membase = GuestMembaseNow();
    // Before the interrupt, so anything the guest releases in the handler sees
    // an already-consistent fence pair.
    if (device && membase) {
      RetireGuestFences(membase, device);
      RetireRingConsumption(membase, device);
    }
    for (uint64_t i = 0; i < pending; ++i) {
      if (device) {
        DispatchVblank();
        s.vblanks.fetch_add(1, std::memory_order_relaxed);
      }
      last += interval;
    }
    // The only telemetry this mode has. native_gfx's frame-boundary report is
    // gated on Active(), which latches false without a graphics system, so
    // nothing else would print anything at all.
    if (++report_ticks >= 5000) {  // ~5 s of 1 ms sleeps
      report_ticks = 0;
      REXLOG_INFO("[nocp] {}", Summary());
    }
    rex::platform::SleepFor(std::chrono::milliseconds(1));
  }
}

}  // namespace

uint32_t ReadGuestRegister(uint32_t index) { return ReadRegisterIndex(index); }
void WriteGuestRegister(uint32_t index, uint32_t value) { WriteRegisterIndex(index, value); }

bool Install(rex::IGuestInterrupts& interrupts, const rex::VideoMode& video_mode) {
  State& s = state();
  if (s.installed.load(std::memory_order_relaxed)) {
    return true;
  }
  // No mapped range to claim any more: the SDK attached the GPU register window to the driver this is called
  // from (TitleState::AttachSystems), and routes every guest load and store of it to ReadRegisterIndex and
  // WriteRegisterIndex. What 0.10 checked here (that no graphics system already owned the window) is the
  // driver selection now: gpu.driver = larecomp.
  s.registers.assign(kRegisterCount, 0);
  s.interrupts = &interrupts;
  s.video_mode = video_mode;
  s.worker_running.store(true, std::memory_order_relaxed);
  auto started = interrupts.StartHostThread("MCLA Native VSync", [](std::stop_token stop) { VblankWorker(stop); });
  if (!started) {
    s.worker_running.store(false, std::memory_order_relaxed);
    REXLOG_ERROR("[nocp] the vblank thread could not start: {}", started.error().message);
    return false;
  }
  s.worker = std::move(*started);
  s.installed.store(true, std::memory_order_relaxed);
  REXLOG_INFO("[nocp] guest GPU installed: register window routed by the SDK, vblank source running");
  return true;
}

void Shutdown() {
  State& s = state();
  if (!s.installed.load(std::memory_order_relaxed)) {
    return;
  }
  s.worker_running.store(false, std::memory_order_relaxed);
  s.worker = rex::ScopeGuard();  // stops and joins the vblank thread
  s.interrupts = nullptr;
  s.installed.store(false, std::memory_order_relaxed);
}

bool Installed() { return state().installed.load(std::memory_order_relaxed); }

uint32_t LatestFrontBuffer() { return state().front_buffer.load(std::memory_order_acquire); }

uint64_t FlipCount() { return state().flips.load(std::memory_order_relaxed); }

void AcquireFenceRetirement() {
  State& s = state();
  {
    std::lock_guard lock(s.fence_mutex);
    if (s.fence_acquired) {
      return;
    }
    s.fence_acquired = true;
    s.fence_acquired_view.store(true, std::memory_order_relaxed);
  }
  REXLOG_INFO("[nocp] retired fence: written by the renderer that executes the guest's work "
              "from now on");
}

void ReleaseFenceRetirement() {
  State& s = state();
  {
    std::lock_guard lock(s.fence_mutex);
    if (!s.fence_acquired) {
      return;
    }
    s.fence_acquired = false;
    s.fence_acquired_view.store(false, std::memory_order_relaxed);
  }
  REXLOG_INFO("[nocp] retired fence: back to the vblank thread ({} renderer publishes)",
              s.owned_publishes.load(std::memory_order_relaxed));
}

bool FenceRetirementAcquired() {
  return state().fence_acquired_view.load(std::memory_order_relaxed);
}

bool PublishOwnedRetiredFence(uint32_t device, uint32_t retired) {
  State& s = state();
  uint8_t* membase = GuestMembaseNow();
  if (!membase || device == 0) {
    return false;
  }
  std::lock_guard lock(s.fence_mutex);
  if (!s.fence_acquired) {
    return false;
  }
  PublishRetiredFence(membase, device, retired);
  s.owned_publishes.fetch_add(1, std::memory_order_relaxed);
  return true;
}

std::string Summary() {
  State& s = state();
  // 224 bytes truncated the line right where `last=` starts, and that field is
  // the name of the hook a stuck guest is parked in -- the one thing a freeze
  // report exists to say. It was measured cut to "last=D3D", then "last=D",
  // shrinking as the vblank count grew digits.
  char buf[512];
  std::snprintf(buf, sizeof(buf),
                "installed=%d vblanks=%llu flips=%llu front_buffer=%08X ring_kicks=%llu | "
                "fence issued=%u retired=%u published=%llu owner=%s owned=%llu | "
                "wptr=%08X limit=%08X "
                "idle_spin=%u | ring pub=%llu wrap=%u end=%08X wb_fence=%08X "
                "wb_consumed=%08X/%08X | "
                "hooks=%llu frames=%llu swaps=%llu last=%s | rt=%s wk=%s",
                s.installed.load(std::memory_order_relaxed) ? 1 : 0,
                (unsigned long long)s.vblanks.load(std::memory_order_relaxed),
                (unsigned long long)s.flips.load(std::memory_order_relaxed),
                s.front_buffer.load(std::memory_order_relaxed),
                (unsigned long long)s.ring_kicks.load(std::memory_order_relaxed),
                s.last_issued.load(std::memory_order_relaxed),
                s.last_retired.load(std::memory_order_relaxed),
                (unsigned long long)s.fence_publishes.load(std::memory_order_relaxed),
                s.fence_acquired_view.load(std::memory_order_relaxed) ? "renderer" : "vblank",
                (unsigned long long)s.owned_publishes.load(std::memory_order_relaxed),
                s.write_ptr.load(std::memory_order_relaxed),
                s.kick_limit.load(std::memory_order_relaxed),
                s.idle_spin.load(std::memory_order_relaxed),
                (unsigned long long)s.ring_publishes.load(std::memory_order_relaxed),
                s.ring_wrap.load(std::memory_order_relaxed),
                s.ring_end.load(std::memory_order_relaxed),
                s.wb_fence.load(std::memory_order_relaxed),
                s.wb_consumed.load(std::memory_order_relaxed),
                s.wb_alloc_consumed.load(std::memory_order_relaxed),
                (unsigned long long)HookCallCount(), (unsigned long long)FrameEndCount(),
                (unsigned long long)SwapCount(), LastHookName(),
                g_hang_render.load(std::memory_order_relaxed),  // TEMP DIAG
                g_hang_worker.load(std::memory_order_relaxed));
  return std::string(buf);
}

}  // namespace mcla::native_gfx::nocp

#endif  // REXGLUE_HAS_XEO3_TARGET
