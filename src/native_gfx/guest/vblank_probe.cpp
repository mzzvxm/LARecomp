#ifndef REXGLUE_HAS_XEO3_TARGET
// MCLA Native Graphics Runtime — guest vblank/flip probe.
// See vblank_probe.h for the decompilation this is built on.

#include "vblank_probe.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>

#include "guest_resources.h"

REXCVAR_DEFINE_BOOL(
    mcla_native_gfx_vblank_probe, false, "MCLA/NativeGfx",
    "Read-only probe of the guest's vblank and flip machinery, for the port to a runtime with no "
    "command processor. Reports the vblank counter, the flip counter and the pending-flip queue "
    "indices in the D3DDevice, so the offsets the replacement will drive are proven while the "
    "emulated path still works. Writes nothing.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace mcla::native_gfx {

namespace {

// D3DDevice offsets, as dword indices in the decompilation of
// D3DDevice_HandleVblank (sub_82419718). Kept as byte offsets here.
constexpr uint32_t kDevNotifyCallback = 4136 * 4;  // 0x4064: user callback
constexpr uint32_t kDevVblankCount = 4137 * 4;     // 0x4064+4
constexpr uint32_t kDevVblankTimebase = 4138 * 4;
constexpr uint32_t kDevFlipCount = 4142 * 4;
constexpr uint32_t kDevFlipRead = 4175 * 4;   // pending flip queue, read index
constexpr uint32_t kDevFlipWrite = 4176 * 4;  // pending flip queue, write index
// The largest offset touched, for the readability check.
constexpr uint32_t kDevProbeSpan = kDevFlipWrite + 4;

struct State {
  std::atomic<uint32_t> device_va{0};
  std::atomic<uint32_t> callback_va{0};

  // First and latest samples, so a rate can be derived without timestamps.
  std::atomic<uint64_t> samples{0};
  std::atomic<uint32_t> first_vblank{0};
  std::atomic<uint32_t> last_vblank{0};
  std::atomic<uint32_t> first_flip{0};
  std::atomic<uint32_t> last_flip{0};
  std::atomic<uint32_t> max_queue_depth{0};
  std::atomic<uint32_t> notify_callback{0};
  std::atomic<uint64_t> stalled_samples{0};  // vblank counter did not move
  std::atomic<uint64_t> unreadable{0};
};

State& state() {
  static State s;
  return s;
}

inline uint32_t LoadBe32At(const uint8_t* base, uint32_t ea) {
  uint32_t v;
  std::memcpy(&v, rex::memory::GuestPtr(const_cast<uint8_t*>(base), ea), 4);
  return __builtin_bswap32(v);
}

}  // namespace

void NoteGraphicsEnginesInitialized(const uint8_t* base, uint32_t device_va) {
  State& s = state();
  if (!device_va || s.device_va.load(std::memory_order_relaxed)) {
    return;
  }
  s.device_va.store(device_va, std::memory_order_relaxed);
  // The callback is a constant in the caller: VdSetGraphicsInterruptCallback
  // is handed sub_82411478 directly. Recorded for the port, which has to
  // invoke it from its own vblank thread once the SDK's graphics system is
  // gone and the Vd export stops storing it.
  s.callback_va.store(0x82411478u, std::memory_order_relaxed);
  REXLOG_INFO(
      "[native_gfx] graphics engines initialized: device={:#010x}, interrupt callback "
      "{:#010x} (D3DDevice_GraphicsInterruptCallback)",
      device_va, 0x82411478u);
  (void)base;
}

void ProbeVblankState(const uint8_t* base) {
  if (!REXCVAR_GET(mcla_native_gfx_vblank_probe)) {
    return;
  }
  State& s = state();
  const uint32_t dev = s.device_va.load(std::memory_order_relaxed);
  if (!base || !dev || !IsGuestRangeReadable(dev, kDevProbeSpan)) {
    if (dev) {
      s.unreadable.fetch_add(1, std::memory_order_relaxed);
    }
    return;
  }

  const uint32_t vblank = LoadBe32At(base, dev + kDevVblankCount);
  const uint32_t flips = LoadBe32At(base, dev + kDevFlipCount);
  const uint32_t read_index = LoadBe32At(base, dev + kDevFlipRead);
  const uint32_t write_index = LoadBe32At(base, dev + kDevFlipWrite);
  const uint32_t notify = LoadBe32At(base, dev + kDevNotifyCallback);

  const uint64_t n = s.samples.fetch_add(1, std::memory_order_relaxed);
  if (n == 0) {
    s.first_vblank.store(vblank, std::memory_order_relaxed);
    s.first_flip.store(flips, std::memory_order_relaxed);
  } else if (vblank == s.last_vblank.load(std::memory_order_relaxed)) {
    // The counter standing still between two frame boundaries would mean the
    // vblank source is not running -- which is exactly what the port has to
    // take over, so it is worth counting rather than assuming.
    s.stalled_samples.fetch_add(1, std::memory_order_relaxed);
  }
  s.last_vblank.store(vblank, std::memory_order_relaxed);
  s.last_flip.store(flips, std::memory_order_relaxed);
  s.notify_callback.store(notify, std::memory_order_relaxed);

  // The queue is a ring of 16 entries: HandleVblank indexes it with
  // ((8 * read_index) & 0x78), so depth is the raw difference.
  const uint32_t depth = write_index - read_index;
  uint32_t prev = s.max_queue_depth.load(std::memory_order_relaxed);
  while (depth > prev &&
         !s.max_queue_depth.compare_exchange_weak(prev, depth, std::memory_order_relaxed)) {
  }
}

uint32_t GraphicsDeviceVa() { return state().device_va.load(std::memory_order_relaxed); }

std::string VblankProbeSummary() {
  State& s = state();
  const uint32_t first_v = s.first_vblank.load(std::memory_order_relaxed);
  const uint32_t last_v = s.last_vblank.load(std::memory_order_relaxed);
  const uint32_t first_f = s.first_flip.load(std::memory_order_relaxed);
  const uint32_t last_f = s.last_flip.load(std::memory_order_relaxed);
  char buf[288];
  std::snprintf(buf, sizeof(buf),
                "device=%08X callback=%08X | samples=%llu vblank %u->%u (+%u) flips %u->%u (+%u) "
                "| queue_depth_max=%u notify_cb=%08X stalled=%llu unreadable=%llu",
                s.device_va.load(std::memory_order_relaxed),
                s.callback_va.load(std::memory_order_relaxed),
                (unsigned long long)s.samples.load(std::memory_order_relaxed), first_v, last_v,
                last_v - first_v, first_f, last_f, last_f - first_f,
                s.max_queue_depth.load(std::memory_order_relaxed),
                s.notify_callback.load(std::memory_order_relaxed),
                (unsigned long long)s.stalled_samples.load(std::memory_order_relaxed),
                (unsigned long long)s.unreadable.load(std::memory_order_relaxed));
  return std::string(buf);
}

}  // namespace mcla::native_gfx

#endif  // REXGLUE_HAS_XEO3_TARGET
