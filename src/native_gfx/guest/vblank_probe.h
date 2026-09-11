#pragma once
// ===========================================================================
// MCLA Native Graphics Runtime — guest vblank/flip machinery, read-only probe
// ===========================================================================
// Step zero of the no-command-processor port: prove every offset the
// replacement will drive, while the emulated path is still running and the
// game still works. Nothing here writes to the guest.
//
// Reverse-engineering this is built on (all decompiled from MCLA's own IDB):
//
//   D3DDevice_InitializeEngines (sub_82426468) registers the guest's handler:
//       VdInitializeEngines(0x1B540000, sub_82425D78, 0, ...)
//       VdSetGraphicsInterruptCallback(sub_82411478, device)
//   so the callback is a constant and the device arrives in r3.
//
//   D3DDevice_GraphicsInterruptCallback (sub_82411478) branches on the source:
//       source == 1  -> CPU interrupt: calls *(*(dev+10900)+16), clears a bit
//       source == 0  -> vblank: if (MMIO[0x7FC86544] & 1) HandleVblank(dev)
//
//   D3DDevice_HandleVblank (sub_82419718), under the device spin lock:
//       ++dev[4137]                     // vblank counter
//       dev[4138] = mftb                // timebase of this vblank
//       while (dev[4175] != dev[4176])  // pending flip queue, read/write index
//           if (queued_vblank > dev[4137]) break;
//           ++dev[4142];                // flip counter
//           MMIO[0x7FC86110] = front_buffer_address;   // THE FLIP
//       callback dev[4136]({vblank_count, flip_count, 0})
//
// Two facts that decide the whole port:
//   * the vblank handler READS GPU MMIO at 0x7FC86544. That range is mapped by
//     GraphicsSystem::Setup (AddVirtualMappedRange, 0x7FC80000 + 64K). With
//     config.graphics = nullptr nothing maps it, so the runtime has to.
//   * the flip is a WRITE of the front buffer address to MMIO 0x7FC86110. That
//     is the present signal, handed over for free -- no swap packet needed.
// ===========================================================================

#include <cstdint>
#include <string>

namespace mcla::native_gfx {

// Called from the D3DDevice_InitializeEngines hook, AFTER the original: the
// device is r3 and the callback registration has happened.
void NoteGraphicsEnginesInitialized(const uint8_t* base, uint32_t device_va);

// Sampled at the frame boundary. Reads the counters and the flip queue and
// reports whether they move the way the decompilation says they should.
void ProbeVblankState(const uint8_t* base);

std::string VblankProbeSummary();

// The D3DDevice the guest registered its interrupt callback with, or 0.
uint32_t GraphicsDeviceVa();

}  // namespace mcla::native_gfx
