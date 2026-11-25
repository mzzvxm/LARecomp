#pragma once
// ===========================================================================
// MCLA Native Graphics Runtime — what is left of the app bring-up with no emulated GPU
// ===========================================================================
// On rexglue 0.10 this file made LarecompApp run without rexgpu-xenos: an empty gpu_plugin, the SDK's D3D12 provider and
// presenter for the window, a close listener for the pipeline cache. On 0.11 the mode is a driver (nocp_driver.h:
// gpu.driver = larecomp) and the Plume renderer owns the window's swap chain, so what remains here is what the guest hooks
// and the debug overlay still read: the last hook that ran, the frame and swap counters, and the guest frame timing
// measured at the swap (the SDK's own numbers come from the command processor's swap packet, which does not exist here).
// ===========================================================================

#include <cstdint>

#include <rex/gpu/graphics_system.h>

namespace mcla::native_gfx::nocp {

// True when the SDK selected the no-command-processor driver (gpu.driver = larecomp). Valid from Init on; was the cvar
// mcla_native_gfx_nocp, a restart-only debug switch, before.
bool WantNoCommandProcessor();

// Records that the guest entered a hooked D3D function, by name. One relaxed
// store; the pointer is a string literal with static lifetime.
//
// This is the only way to tell WHERE a guest that has gone quiet is stuck: the
// fence pair, the write pointer and the kick limit all say what it is NOT
// waiting on, and none of them say what it is. The last function entered, plus
// a total that stops moving, says it directly.
void NoteHook(const char* name);

// The two calls that say whether the guest is completing frames at all: the
// end of a rendered frame and the swap. A last-hook name cannot answer that --
// draws drown it out -- and "is it swapping" decides whether the present path
// is missing or simply never reached. NoteSwapCall also wakes the driver's WaitForFrame.
void NoteFrameEnd();
void NoteSwapCall();

// The guest frame timing measured at the swap, for the F3 overlay. frame_count stays 0 until the guest has swapped
// twice, which the overlay reads as "no data". (IGraphicsSystem::GetFrameStats counts the same swaps through the
// driver's WaitForFrame; this keeps the overlay's numbers as they were until the two have been compared. CHECK.)
rex::gpu::FrameStats GuestFrameStats();

// For the periodic report in guest_gpu.cpp.
const char* LastHookName();
uint64_t HookCallCount();
uint64_t FrameEndCount();
uint64_t SwapCount();

}  // namespace mcla::native_gfx::nocp
