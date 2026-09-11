#pragma once
// ===========================================================================
// MCLA Native Graphics Runtime — the guest's GPU fence pair
// ===========================================================================
// This is the mechanism that has to change hands before the native runtime can
// stop feeding the emulated command processor. Skipping the guest draw builder
// without it deadlocks the game, which is exactly what happened the first time
// (4 frames presented, then a permanent stall).
//
// Reverse-engineered from D3DResource_Lock (sub_82421CA0) and the wait it
// calls, sub_82411E98:
//
//   sub_82411E98(device, fence, ...)
//     issued  = *(u32*)(device + 10908);          // CPU hands these out
//     retired = **(u32**)(device + 10896);        // the GPU writes this back
//     if (issued - fence < (u32)(issued - retired)) {   // not retired yet
//        if (fence == issued) {
//           if (*(u32*)(device + 13232)) return;  // recording, cannot kick
//           sub_82412710(device);                 // kick so it CAN retire
//        }
//        ... spin until retired ...
//     }
//
// Every resource lock reads the resource's own fence (D3DResource::Fence at
// +8, or ReadFence at +12 depending on the lock flags) and waits on it here.
// So: no submission -> retired never advances -> the spin never exits. The
// kick is not a fix on its own, because a kick with an empty command buffer
// submits nothing for the GPU to retire.
//
// The comparison is deliberately wraparound-safe: both differences are taken
// modulo 2^32, so a fence counter that wraps still orders correctly. Any
// reimplementation must keep that form rather than comparing directly.
//
// What owning this means, once the native runtime submits the frame itself:
// advance `issued` when it submits, and write `retired` when its own D3D12
// fence signals. Two dwords in guest memory, driven from the host GPU
// timeline.
// ===========================================================================

#include <cstdint>

namespace mcla::native_gfx {

// Offsets into the guest D3DDevice object.
inline constexpr uint32_t kDevFenceIssuedOffset = 10908;    // u32
inline constexpr uint32_t kDevFenceRetiredPtrOffset = 10896; // u32* (guest VA)
inline constexpr uint32_t kDevCommandBufferRecordingOffset = 13232;

struct GuestFenceState {
  uint32_t issued = 0;   // newest fence the guest has handed out
  uint32_t retired = 0;  // newest fence the GPU has completed
  bool valid = false;    // false when the device or the pointer is unreadable
};

// Samples both halves of the pair. Safe to call from a guest thread.
GuestFenceState ReadFenceState(const uint8_t* base, uint32_t dev);

// The guest's own retirement test, replicated bit for bit including the
// wraparound-safe form. `fence == 0` counts as retired (the guest treats a
// zero fence as "never used").
inline bool IsFenceRetired(const GuestFenceState& s, uint32_t fence) {
  if (!fence || !s.valid) {
    return true;
  }
  return !(uint32_t(s.issued - fence) < uint32_t(s.issued - s.retired));
}

// True while the guest is recording a command buffer, in which case the wait
// path returns without kicking. Nothing may force a submission here.
bool IsRecordingCommandBuffer(const uint8_t* base, uint32_t dev);

// Publishes a new retired value, which is what releases anything spinning in
// sub_82411E98. Only legal once the native runtime is the sole submitter:
// while the emulated command processor is still consuming the ring it owns
// this write, and two writers race.
//
// Never moves the value backwards -- retirement is monotonic under the same
// wraparound rule the guest uses, and a backwards step would strand a waiter
// forever.
void PublishRetiredFence(uint8_t* base, uint32_t dev, uint32_t retired);

// Read-only validation of the offsets above against the running game, so
// nothing gets built on them before they are proven. Self-throttled to one
// line every kFenceProbeInterval draws and stops after a handful; call it
// from a draw hook.
//
// What a correct reading looks like: `issued` climbing steadily, `retired`
// trailing it by a small amount and never passing it, and `lag` staying
// bounded. A wild or static `retired`, or an unreadable pointer, means the
// offsets are wrong and the whole fence handover has to be re-derived.
void ProbeFenceState(const uint8_t* base, uint32_t dev);

}  // namespace mcla::native_gfx
