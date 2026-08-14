#pragma once
// ===========================================================================
// MCLA Native Graphics Runtime — memory census
// ===========================================================================
// Answers one question the process-level number cannot: WHICH pool is growing.
//
// Task Manager reports a single figure that mixes three different things:
//   - host heap (std containers, staging copies)
//   - D3D12 resources in system memory (upload/readback heaps)
//   - VRAM that overflowed and was paged back to system memory by WDDM
// A default-heap resource leak on a card with less VRAM than the working set
// therefore shows up as multi-GB *system* RAM, which is why an apparent "RAM
// leak" is usually a resource leak. The census separates them: process working
// set and commit on one side, DXGI local (VRAM) and non-local (system-backed)
// budget/usage on the other, then the live count and byte cost of every cache
// the native runtime owns.
//
// Written to native_gfx_mem.txt next to the exe, one line per report, every
// mcla_native_gfx_census frame boundaries (0 disables).
// ===========================================================================

#include <cstdint>

namespace mcla::native_gfx {

class BufferCache;
class D3D12Context;
class PipelineCache;
class RenderTargetPool;
class TextureCache;

// Call once per frame boundary; throttles itself to the cvar interval.
void MemoryCensusTick(D3D12Context& context, const BufferCache& buffers,
                      const TextureCache& textures, const PipelineCache& pipelines,
                      const RenderTargetPool& render_targets);

}  // namespace mcla::native_gfx
