#pragma once
// ===========================================================================
// MCLA GPU renderer -- what the process holds, by kind (diagnostic)
// ===========================================================================
// The periodic "[gpu] N presents" line says what the renderer holds in video
// memory; nothing says what the rest of the process's memory is. Every
// mcla_gpu_process_memory_log frames (0: never, the default) this logs two
// lines about the process: the commit charge (Windows' PrivateUsage, what the
// harness's game_memory.max_sampled_private_mb samples), the working set, the
// committed address space by kind (private, mapped -- the guest arena is a
// pagefile-backed section, so it is mapped -- and image), the private regions
// by size (a heap that grew organically spreads over the sizes; a block
// allocator shows as one size), the process heaps and the biggest regions with
// their base addresses (the guest arena's are marked). That separates the
// guest's own memory (fixed) from what the host adds: heaps, driver
// allocations, the renderer's buffers.
//
// Cost: one VirtualQuery per region of the address space, tens of thousands
// when the guest's pages are write-watched, a few milliseconds to tens of
// milliseconds on the render thread each time, and a lock on each heap while
// it is summed. Off by default for that reason; hot reload.
//
// Threading: render thread only (VideoMemoryBudget::BeginFrame).
// ===========================================================================

#include <cstdint>

namespace mcla::gpu {

// At the start of a frame: logs the two lines when mcla_gpu_process_memory_log
// frames have passed since the last (the first call after it is turned on logs
// at once). Windows only; elsewhere it does nothing.
void ProcessMemoryTick(uint64_t frame);

}  // namespace mcla::gpu
