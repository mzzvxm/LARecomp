#pragma once
// ===========================================================================
// MCLA Native Graphics Runtime — per-draw geometry snapshot
// ===========================================================================
// Captures everything a draw needs about its geometry, deterministically and
// immutably, from guest state:
//
//   bound VS microcode  -> vfetch instructions (stride/offset/format/slot)
//   shader pack         -> vertex elements (usage/usageIndex by instruction)
//   fetch shadow[slot]  -> guest physical base + size of each stream
//   BufferCache         -> ID3D12Resource + view offset (suballocation)
//   D3DDevice+12436     -> index buffer (16-bit, per telemetry)
//   draw arguments      -> topology + counts, sliced by draw_slicing
//
// The snapshot is built once per draw and consumed by the draw pipeline; it
// never reads guest memory again afterwards.
// ===========================================================================

#include <cstdint>
#include <vector>

#include "guest/vertex_fetch_decode.h"
#include "vertex_declaration.h"

// Declared at global scope on purpose: an elaborated type specifier inside
// the namespace would introduce mcla::native_gfx::ID3D12GraphicsCommandList,
// a distinct type from the real one.
struct ID3D12GraphicsCommandList;

namespace mcla::native_gfx {

class D3D12Context;
class BufferCache;
class ShaderDatabase;

// One bound vertex stream: the physical resource plus this draw's window
// into it.
struct VertexStream {
  uint32_t fetch_slot = 0;
  uint32_t guest_base = 0;   // from the fetch constant
  uint32_t guest_size = 0;
  uint32_t stride = 0;
  uint32_t endian = 0;  // xenos::Endian from the fetch constant
  // Resolved against the BufferCache.
  uint32_t resource_base = 0;  // guest base of the cached region
  uint32_t resource_size = 0;
  uint32_t view_offset = 0;  // guest_base - resource_base
  uint64_t gpu_address = 0;  // D3D12_GPU_VIRTUAL_ADDRESS of guest_base
  bool resolved = false;
  // Backed by the shared all-zero buffer instead of guest memory, for an
  // attribute the declaration does not supply. Bound with stride 0 so every
  // vertex reads the same zeros — which is what the Xenos delivers when the
  // vfetch is left unpatched (see UnsuppliedAttribute below).
  bool zero_fill = false;
};

// An attribute the vertex shader declares but the bound vertex declaration
// does not supply: its vfetch instruction was left unpatched (format, stride
// and offset all zero), so the Xenos fetches nothing and the shader sees
// zeros. Confirmed in a real draw: vfetch_addr=5, usage=POSITION, all fields
// zero, while every other attribute of the same shader was patched.
//
// D3D12 has no equivalent of "declared but unbound": a PSO fails to create
// if a shader input is missing from the input layout. The draw pipeline
// therefore has to supply these from a zero-filled stream (a vertex buffer
// view with stride 0 makes every vertex read the same zeros).
struct UnsuppliedAttribute {
  const char* semantic_name = nullptr;
  uint32_t semantic_index = 0;
  uint32_t vfetch_address = 0;
};

// Vertices written straight into the command buffer by the guest between
// D3DDevice_BeginVertices and D3DDevice_EndVertices, instead of living in a
// vertex buffer bound through a fetch constant.
//
// The distinction matters because the fetch constant for this data is written
// into the COMMAND STREAM, not into the device shadow BuildGeometrySnapshot
// reads — so the snapshot would fail with "vertex fetch constant not bound".
// The shader still supplies the layout; only the stream's address, size,
// stride and endianness come from here.
//
// Values taken verbatim from the packet sub_8241CD88 builds:
//   dword0 = (((v >> 20) + 512) & 0x1000) + (v & 0x1FFFFFFF) | 3   type = 3
//   dword1 = (4 * dwords) & 0x3FFFFFC | 0x10000002                 endian = 2
// so the endianness is k8in32, matching DecodeVertexFetch's `dword1 & 3`.
struct InlineGeometry {
  uint32_t address = 0;  // guest byte address, page fixup already applied
  uint32_t size_bytes = 0;
  uint32_t stride = 0;
  uint32_t endian = 2;  // xenos::Endian k8in32

  // Set when the host has already built the vertices itself and they must NOT
  // be fetched from guest memory — kRectangleList, whose fourth corner does
  // not exist in the guest buffer and is derived on the CPU. The BufferCache
  // is bypassed entirely: it would upload and byte-swap the guest data, which
  // is neither what was synthesised nor the right vertex count.
  uint64_t host_gpu_address = 0;
  uint32_t host_size_bytes = 0;
};

struct GeometrySnapshot {
  // Input assembler.
  std::vector<InputElement> input_layout;
  std::vector<VertexStream> streams;
  // Attributes the declaration did not supply. These used to land here and get
  // the whole draw rejected; they are now bound to a zero stream instead, which
  // is what the hardware does, and the list is kept only for diagnostics.
  // Measured before the fix: 9243 of 9963 such rejections were display-shaped
  // draws, i.e. the composite we present was being gutted.
  std::vector<UnsuppliedAttribute> unsupplied;

  // Bit N is set when TEXCOORD N reaches the shader with its components in
  // swapped order, because its 16-bit pair shares a stream that had to be
  // byte-swapped at 32-bit width. Goes into SharedConstants.g_SwappedTexcoords,
  // which the translated vertex shader consumes as `value.yxwz`.
  uint32_t swapped_texcoords = 0;

  // Index buffer (absent for non-indexed draws).
  bool indexed = false;
  bool index_32bit = false;
  uint32_t index_endian = 0;  // xenos::Endian, from the index buffer object
  uint32_t index_guest_base = 0;
  uint64_t index_gpu_address = 0;
  uint32_t index_buffer_bytes = 0;

  // Draw arguments, as the guest issued them.
  uint32_t primitive_type = 0;  // xenos::PrimitiveType
  uint32_t element_count = 0;   // indices or vertices
  uint32_t start_element = 0;
  int32_t base_vertex = 0;

  // Diagnosis: set when the snapshot cannot be used for rendering.
  // `failure_usage` / `failure_format` carry the offending values so an
  // unsupported attribute can be identified without another capture.
  bool complete = false;
  const char* failure = nullptr;
  uint32_t failure_usage = 0;
  uint32_t failure_format = 0;
  uint32_t failure_address = 0;
  uint32_t failure_stride = 0;
  uint32_t failure_offset = 0;
};

// Builds the snapshot for the draw currently described by the guest device.
// `base` is the guest membase, `dev` the guest D3DDevice address.
// `cl` may be null: then buffers are not resolved (streams stay unresolved)
// and only the guest-side description is captured — used by the diagnostic
// mode before the draw pipeline exists.
// `inline_geometry` non-null replaces the fetch constant for every stream with
// command-buffer-resident data; see InlineGeometry.
GeometrySnapshot BuildGeometrySnapshot(const uint8_t* base, uint32_t dev, uint32_t primitive_type,
                                       uint32_t element_count, uint32_t start_element,
                                       int32_t base_vertex, bool indexed, ShaderDatabase& shaders,
                                       BufferCache* buffers, D3D12Context* context,
                                       ::ID3D12GraphicsCommandList* cl,
                                       const InlineGeometry* inline_geometry = nullptr);

// Native primitive topology for a Xenos primitive type. Returns 0
// (D3D_PRIMITIVE_TOPOLOGY_UNDEFINED) for types with no direct equivalent —
// kQuadList and kRectangleList need index/geometry expansion, which the draw
// pipeline handles separately.
uint32_t PrimitiveTypeToTopology(uint32_t primitive_type);

}  // namespace mcla::native_gfx
