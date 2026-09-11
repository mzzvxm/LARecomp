#pragma once
// ===========================================================================
// MCLA Native Graphics Runtime — the guest-visible GPU, with no emulator
// ===========================================================================
// What the SDK's GraphicsSystem provides to the guest that has nothing to do
// with drawing, and therefore has to be provided by something once
// config.graphics is null and the command processor is gone.
//
// There are exactly two such things, both found by decompiling MCLA:
//
//   1. The GPU register window at 0x7FC80000 (64 KB), mapped by
//      GraphicsSystem::Setup through Memory::AddVirtualMappedRange. The
//      guest's vblank handler (D3DDevice_GraphicsInterruptCallback,
//      sub_82411478) reads 0x7FC86544 -- register 0x1951, "interrupt status"
//      -- and does nothing at all unless bit 0 is set. Unmapped, that read is
//      a fault.
//
//   2. The vblank itself. GraphicsSystem runs a worker thread that calls
//      MarkVblank at the video mode's refresh rate (or 1000 Hz with vsync
//      off), which dispatches the guest's interrupt callback. Measured on
//      MCLA: 910 vblanks/s against 27 frames/s, and the game's frame pacing
//      is built on that counter.
//
// And one thing the emulator throws away that this wants: the flip. The
// guest's vblank handler retires pending flips by writing the front buffer
// address to 0x7FC86110 -- register 0x1844,
// AVIVO_D1GRPH_PRIMARY_SURFACE_ADDRESS -- and GraphicsSystem::WriteRegister
// has an empty `case` for it. That write is the present signal, address
// included: no swap packet, no command processor, no parsing.
//
// Nothing here draws. This is the substrate the native renderer presents on
// top of.
// ===========================================================================

#include <cstdint>
#include <string>

namespace mcla::native_gfx::nocp {

// Claims the GPU register window and starts the vblank source. Refuses (and
// says so) when something already owns the window -- which is the case
// whenever the SDK's graphics system is alive, and is the check that keeps
// this from being switched on halfway.
bool Install();
void Shutdown();

bool Installed();

// The most recent front buffer address the guest flipped to, and how many
// flips have happened. Returns 0 when the guest has not flipped yet.
uint32_t LatestFrontBuffer();
uint64_t FlipCount();

std::string Summary();

}  // namespace mcla::native_gfx::nocp
