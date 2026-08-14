#pragma once
// ===========================================================================
// MCLA Native Graphics Runtime — resource telemetry
// ===========================================================================
// Passive observation of the resources the game actually binds, collected
// from the draw hooks while the native runtime is active (or while the
// dedicated cvar below is on). This runs BEFORE the real draw pipeline
// exists: its output drives the design of the resource cache
// (formats to support, overlap behavior of buffer ranges, index buffer
// endianness, texture fetch layouts).
//
// Output: mcla_native_gfx_resources_*.csv next to the executable,
// rewritten periodically so a crash cannot lose the whole session.
//
//   _vbuf.csv: guest_addr, size, endian, draws, first_frame
//   _ibuf.csv: guest_addr, bits, endian_raw, draws
//   _tex.csv : six raw big-endian dwords of each unique texture fetch group
//              (decoded offline against xe_gpu_texture_fetch_t)
//   _summary.csv: counters, including overlap statistics for buffer ranges
// ===========================================================================

#include <cstdint>

namespace mcla::native_gfx {

// Records the state visible at one draw. `base` is the guest membase, `dev`
// the guest D3DDevice. Cheap enough for every draw; internally deduplicated.
void TelemetryRecordDraw(const uint8_t* base, uint32_t dev, uint32_t prim_type, uint32_t elements,
                         bool indexed);

// Advances the frame counter and periodically flushes the CSVs.
void TelemetryOnFrameEnd();

// Geometry diagnostic: builds a GeometrySnapshot for a draw straight from
// guest state (no D3D12 resolution) and writes a human-readable dump so the
// guest geometry state can be compared against what the native runtime would
// bind. Gated by mcla_native_gfx_geomdump; writes a bounded number of draws.
// `vs_dirty` / `ps_dirty` are the ALU constant dirty masks sampled BEFORE
// the original draw function ran — it clears them, so they cannot be read
// afterwards.
void TelemetryRecordGeometry(const uint8_t* base, uint32_t dev, uint32_t primitive_type,
                             uint32_t element_count, uint32_t start_element, int32_t base_vertex,
                             bool indexed, uint64_t vs_dirty, uint64_t ps_dirty);

}  // namespace mcla::native_gfx
