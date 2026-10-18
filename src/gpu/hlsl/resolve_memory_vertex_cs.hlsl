// MCLA GPU renderer -- a vertex stream over memory a resolve wrote
// (resolve_memory.h CopyVertices).
//
// The console's resolve-to-vertex-buffer: the guest resolves a render target
// into memory and draws with that memory as vertex streams (MCLA's rain:
// xrain_system's particle state, k_32_32_FLOAT, read by ParticleRenderVS as
// four streams). The bytes are in the resolve memory, not in guest memory, so
// the stream is built here instead of by the CPU copy (geometry.cpp): the
// same bytes, swapped by the stream's plan (SwapVertices: 8in32 for 32-bit
// elements and for bytes outside elements, 8in16 for 16-bit ones), into a
// device buffer the draw binds.
//
// One thread per dword of a destination vertex. A folded stream (the computed
// fetch vertex shaders, geometry.h) reads source vertex (first + v) / 4 for
// destination vertex v; past the fetch's range the dword is zero, as the CPU
// fold writes it.

struct VertexPush {
  uint address;     // the stream's guest physical address (source vertex 0)
  uint stride;      // bytes, a multiple of 4
  uint first;       // source vertex of destination vertex 0 (fold: the draw's start)
  uint vertices;    // destination vertices
  uint sources;     // source vertices the fetch covers
  uint flags;       // bit 0: fold
  uint dst_offset;  // bytes into the destination buffer
  uint reserved;
  uint4 modes;      // 2 bits per dword of a vertex (64 dwords): 0 as is, 1 8in16, 2 8in32
  uint4 reserved2;
};

ByteAddressBuffer g_memory : register(t0, space0);
ByteAddressBuffer g_memory_table : register(t1, space0);
RWByteAddressBuffer g_vertices : register(u0, space1);

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<VertexPush> g_push;
#else
ConstantBuffer<VertexPush> g_push : register(b0, space2);
#endif

#include "resolve_memory.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  const uint stride_dwords = g_push.stride >> 2u;
  const uint i = id.x;
  if (i >= g_push.vertices * stride_dwords) {
    return;
  }
  const uint v = i / stride_dwords;
  const uint d = i - v * stride_dwords;
  const uint source = (g_push.flags & 1u) != 0u ? (g_push.first + v) >> 2u : g_push.first + v;
  uint value = 0u;
  if (source < g_push.sources) {
    const uint raw = ResolveMemoryLoad(g_push.address + source * g_push.stride + (d << 2u));
    const uint mode = (g_push.modes[d >> 4u] >> ((d & 15u) << 1u)) & 3u;
    value = mode == 2u ? XeEndianSwap32(raw, 2u) : (mode == 1u ? XeEndianSwap32(raw, 1u) : raw);
  }
  g_vertices.Store(g_push.dst_offset + (i << 2u), value);
}
