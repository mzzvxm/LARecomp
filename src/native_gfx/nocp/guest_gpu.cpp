#ifndef REXGLUE_HAS_XEO3_TARGET
// MCLA Native Graphics Runtime — the guest-visible GPU, with no emulator.
// See guest_gpu.h for what this replaces and why.

#include "guest_gpu.h"
#include "nocp_app.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <vector>

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>
#include <rex/thread.h>

#include <cstring>

#include "../guest/guest_fence.h"
#include "../guest/guest_resources.h"
#include "../guest/vblank_probe.h"

// Defined in nocp_app.cpp, at global scope like every cvar here.
REXCVAR_DECLARE(bool, mcla_native_gfx_nocp_vsync);

namespace mcla::native_gfx::nocp {

namespace {

// The window GraphicsSystem::Setup claims, byte for byte.
constexpr uint32_t kGpuWindowBase = 0x7FC80000u;
constexpr uint32_t kGpuWindowMask = 0xFFFF0000u;
constexpr uint32_t kGpuWindowSize = 0x0000FFFFu;

// Registers, as (address & 0xFFFF) / 4. Named after what the SDK calls them.
constexpr uint32_t kRegEdramTiming = 0x0F00;
constexpr uint32_t kRegBcControl = 0x0F01;
constexpr uint32_t kRegPrimarySurfaceAddress = 0x1844;  // the flip
constexpr uint32_t kRegCpRbWptr = 0x01C5;               // ring write pointer
constexpr uint32_t kRegVCounter = 0x194C;
constexpr uint32_t kRegInterruptStatus = 0x1951;
constexpr uint32_t kRegViewportSize = 0x1961;

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
  std::atomic<uint32_t> write_ptr{0};
  std::atomic<uint32_t> kick_limit{0};
  std::atomic<bool> installed{false};
  std::atomic<bool> worker_running{false};
  rex::system::object_ref<rex::system::XHostThread> worker;
};

State& state() {
  static State s;
  return s;
}

uint32_t ReadRegister(void* /*ppc_context*/, void* /*context*/, uint32_t addr) {
  State& s = state();
  const uint32_t r = (addr & 0xFFFF) / 4;
  switch (r) {
    case kRegEdramTiming:
      return 0x08100748;
    case kRegBcControl:
      return 0x0000200E;
    case kRegVCounter: {
      // The scanline the display is on. The guest uses it to spin for a
      // raster position; returning the bottom line means "never mid-frame".
      //
      // Sweeping this over the frame period was tried (a wait-for-beam spin
      // cannot escape a constant) and changed nothing about the missing pause
      // menu, so it is back to the value that was measured working.
      rex::system::X_VIDEO_MODE video_mode;
      rex::kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
      return std::min(uint32_t(video_mode.display_height), uint32_t(0x0FFF));
    }
    case kRegInterruptStatus:
      // Bit 0 = vblank. The guest's handler early-outs without it, so this is
      // the single value that makes the whole vblank path run.
      return 1;
    case kRegViewportSize: {
      rex::system::X_VIDEO_MODE video_mode;
      rex::kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
      const uint32_t w = std::min(uint32_t(video_mode.display_width), uint32_t(0x0FFF));
      const uint32_t h = std::min(uint32_t(video_mode.display_height), uint32_t(0x0FFF));
      return (w << 16) | h;
    }
    default:
      break;
  }
  return r < s.registers.size() ? s.registers[r] : 0;
}

void WriteRegister(void* /*ppc_context*/, void* /*context*/, uint32_t addr, uint32_t value) {
  State& s = state();
  const uint32_t r = (addr & 0xFFFF) / 4;
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
// and two writers would race. Being the only writer is what makes it correct.
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
  PublishRetiredFence(base, device_va, fence.issued);
  s.fence_publishes.fetch_add(1, std::memory_order_relaxed);
}

// Retires the ring itself.
//
// The 96-byte block behind dev+10896 holds TWO writeback words that hardware
// fills in, and D3DDevice_SetRingBufferParameters initializes both:
//
//   *(block + 0) = dev[10908] - 2;                 // the retired fence
//   *(block + 4) = dev[14920] & 3 | dev[48];       // ring consumption
//
// The second one is what the ring allocator waits on. sub_82411180, reached
// from D3DDevice_RingBufferAlloc, spins while the wrap counter in the low two
// bits disagrees or while the position it wants is past what has been
// consumed. Measured: the title ran 22500 hooked D3D calls and then stopped
// inside D3DDevice_BeginVertices, which ends in exactly that allocation --
// with the fence pair caught up and the command buffer nowhere near full,
// because it was never the fence or the space.
//
// Publishing "consumed everything" is true here for the same reason retiring
// every fence is: the ring goes nowhere. dev+14908 is the end of the command
// buffer and sub_82411640 clamps every request to it, so it is the ceiling no
// request can exceed. The wrap counter is copied from the guest's own
// dev+14920 so the two always agree.
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
  if (block < 0x1000u || !IsGuestRangeReadable(block, 8)) {
    return;
  }
  const uint32_t wrap = load_be(device_va + 14920) & 3u;
  const uint32_t buffer_end = load_be(device_va + 14908);
  if (!buffer_end) {
    return;
  }
  const uint32_t consumed = (buffer_end & ~3u) | wrap;
  if (load_be(block + 4) == consumed) {
    return;
  }
  const uint32_t be = __builtin_bswap32(consumed);
  std::memcpy(rex::memory::GuestPtr(base, block + 4), &be, 4);
  s.last_consumed.store(consumed, std::memory_order_relaxed);
  s.ring_publishes.fetch_add(1, std::memory_order_relaxed);
}

// Delivers one vblank to the guest, the same way
// GraphicsSystem::DispatchInterruptCallback does: on a thread that can run
// guest code, with the CPU pinned, through the function dispatcher.
void DispatchVblank(uint32_t device_va) {
  auto* kernel = rex::system::KernelState::shared();
  auto* dispatcher = kernel ? kernel->function_dispatcher() : nullptr;
  if (!dispatcher) {
    return;
  }
  auto thread = rex::system::XThread::GetCurrentThread();
  if (!thread) {
    return;
  }
  thread->SetActiveCpu(kInterruptCpu);
  uint64_t args[] = {uint64_t(kInterruptSourceVblank), uint64_t(device_va)};
  dispatcher->ExecuteInterrupt(thread->thread_state(),
                               /*address=*/0x82411478u, args, 2);
}

int VblankWorker() {
  State& s = state();
  rex::system::X_VIDEO_MODE video_mode;
  rex::kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
  const double refresh_hz = std::max(1.0, double(float(video_mode.refresh_rate)));
  const uint64_t tick_frequency = rex::chrono::Clock::guest_tick_frequency();
  const uint64_t vsync_interval = std::max(uint64_t(1), uint64_t(double(tick_frequency) / refresh_hz));
  // Vsync off runs at 1000 Hz, not uncapped: measured on MCLA at 910/s, and
  // the game's pacing is built on the rate.
  const uint64_t free_interval = std::max(uint64_t(1), tick_frequency / 1000);

  REXLOG_INFO(
      "[nocp] vblank source: {} Hz (mcla_native_gfx_nocp_vsync={}). The SDK's `vsync` cvar is "
      "defined in the GPU plugin and is absent in this mode, so a `vsync` line in larecomp.toml "
      "does nothing here.",
      REXCVAR_GET(mcla_native_gfx_nocp_vsync) ? refresh_hz : 1000.0,
      REXCVAR_GET(mcla_native_gfx_nocp_vsync) ? "true" : "false");

  uint32_t report_ticks = 0;
  uint64_t last = rex::chrono::Clock::QueryGuestTickCount();
  while (s.worker_running.load(std::memory_order_relaxed)) {
    const uint64_t now = rex::chrono::Clock::QueryGuestTickCount();
    // Our own cvar, not the SDK's `vsync`: that one is defined in
    // src/graphics/command_processor.cpp, so it lives in the GPU plugin and
    // does not exist in this mode. Querying it by name would have answered
    // false no matter what the config said -- Query<bool> on an unregistered
    // name returns the default -- which is a silent wrong answer, not an error.
    const uint64_t interval =
        REXCVAR_GET(mcla_native_gfx_nocp_vsync) ? vsync_interval : free_interval;
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
    auto* runtime = rex::Runtime::instance();
    auto* membase = runtime ? runtime->virtual_membase() : nullptr;
    // Before the interrupt, so anything the guest releases in the handler sees
    // an already-consistent fence pair.
    if (device && membase) {
      RetireGuestFences(membase, device);
      RetireRingConsumption(membase, device);
    }
    for (uint64_t i = 0; i < pending; ++i) {
      if (device) {
        DispatchVblank(device);
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
    rex::thread::Sleep(std::chrono::milliseconds(1));
  }
  return 0;
}

}  // namespace

bool Install() {
  State& s = state();
  if (s.installed.load(std::memory_order_relaxed)) {
    return true;
  }
  auto* runtime = rex::Runtime::instance();
  auto* memory = runtime ? runtime->memory() : nullptr;
  auto* kernel = rex::system::KernelState::shared();
  if (!memory || !kernel) {
    REXLOG_ERROR("[nocp] no memory or kernel state; the guest GPU cannot be installed");
    return false;
  }
  // Refusing here is the point: if the window is already mapped, the SDK's
  // graphics system is alive, and running both would mean two owners of the
  // guest's vblank and flip. This is a whole-path switch, not a mode.
  if (memory->LookupVirtualMappedRange(kGpuWindowBase)) {
    REXLOG_ERROR(
        "[nocp] the GPU register window at {:#010x} is already mapped -- the SDK graphics system "
        "is running. Set config.graphics = nullptr before installing the native guest GPU.",
        kGpuWindowBase);
    return false;
  }

  s.registers.assign(kRegisterCount, 0);
  if (!memory->AddVirtualMappedRange(kGpuWindowBase, kGpuWindowMask, kGpuWindowSize,
                                     /*context=*/nullptr, &ReadRegister, &WriteRegister)) {
    REXLOG_ERROR("[nocp] failed to map the GPU register window at {:#010x}", kGpuWindowBase);
    return false;
  }

  s.worker_running.store(true, std::memory_order_relaxed);
  s.worker = rex::system::object_ref<rex::system::XHostThread>(
      new rex::system::XHostThread(kernel, 128 * 1024, 0, VblankWorker));
  s.worker->set_name("MCLA Native VSync");
  s.worker->Create();

  s.installed.store(true, std::memory_order_relaxed);
  REXLOG_INFO("[nocp] guest GPU installed: register window {:#010x}, vblank source running",
              kGpuWindowBase);
  return true;
}

void Shutdown() {
  State& s = state();
  if (!s.installed.load(std::memory_order_relaxed)) {
    return;
  }
  s.worker_running.store(false, std::memory_order_relaxed);
  s.worker.reset();
  s.installed.store(false, std::memory_order_relaxed);
}

bool Installed() { return state().installed.load(std::memory_order_relaxed); }

uint32_t LatestFrontBuffer() { return state().front_buffer.load(std::memory_order_acquire); }

uint64_t FlipCount() { return state().flips.load(std::memory_order_relaxed); }

std::string Summary() {
  State& s = state();
  char buf[224];
  std::snprintf(buf, sizeof(buf),
                "installed=%d vblanks=%llu flips=%llu front_buffer=%08X ring_kicks=%llu | "
                "fence issued=%u retired=%u published=%llu | wptr=%08X limit=%08X "
                "idle_spin=%u | ring pub=%llu | hooks=%llu frames=%llu swaps=%llu last=%s",
                s.installed.load(std::memory_order_relaxed) ? 1 : 0,
                (unsigned long long)s.vblanks.load(std::memory_order_relaxed),
                (unsigned long long)s.flips.load(std::memory_order_relaxed),
                s.front_buffer.load(std::memory_order_relaxed),
                (unsigned long long)s.ring_kicks.load(std::memory_order_relaxed),
                s.last_issued.load(std::memory_order_relaxed),
                s.last_retired.load(std::memory_order_relaxed),
                (unsigned long long)s.fence_publishes.load(std::memory_order_relaxed),
                s.write_ptr.load(std::memory_order_relaxed),
                s.kick_limit.load(std::memory_order_relaxed),
                s.idle_spin.load(std::memory_order_relaxed),
                (unsigned long long)s.ring_publishes.load(std::memory_order_relaxed),
                (unsigned long long)HookCallCount(), (unsigned long long)FrameEndCount(),
                (unsigned long long)SwapCount(), LastHookName());
  return std::string(buf);
}

}  // namespace mcla::native_gfx::nocp

#endif  // REXGLUE_HAS_XEO3_TARGET
