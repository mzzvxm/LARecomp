#ifndef REXGLUE_HAS_XEO3_TARGET
// MCLA Native Graphics Runtime — per-draw geometry snapshot.
// See geometry.h for where each field comes from.

#include "geometry.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <set>

#include <d3d12.h>
#include <dxgiformat.h>

#include <rex/logging.h>
#include <rex/system/xmemory.h>

#include "d3d12/context.h"
#include "d3d12/resource_cache.h"
#include "d3d12/shader_db.h"
#include "guest/guest_resources.h"
#include "shader_identity.h"

namespace mcla::native_gfx {

namespace {

// xenos::Endian -> the swap width the buffer upload has to apply.
// kNone(0) and k8in16(1)/k8in32(2) are the only values the game produces;
// k16in32(3) would need a distinct pass and is reported rather than guessed.
BufferSwap SwapForEndian(uint32_t endian) {
  switch (endian) {
    case 1: return BufferSwap::k8in16;
    case 2: return BufferSwap::k8in32;
    default: return BufferSwap::kNone;
  }
}

// Byte-swap width a vertex attribute needs, derived from its FORMAT rather
// than the fetch constant's endian field. The Xenos applies the swap per
// vfetch instruction (the patcher sub_82423A38 sets it from the format via
// word_82062064), so the swap width is a property of the attribute's component
// size, not of the whole buffer: 16-bit components swap in 16-bit units, and
// 32-bit / packed-dword components swap in 32-bit units. Reading a single
// endian from the fetch constant swaps an entire stream one way, which shatters
// geometry whose position is a 16-bit format (FLOAT16_4, common for RAGE
// compressed positions) that the game happened to tag as 8in32. Returns the
// xenos::Endian value SwapForEndian expects (1 = 8in16, 2 = 8in32).
uint32_t EndianForDxgiFormat(uint32_t dxgi) {
  switch (DXGI_FORMAT(dxgi)) {
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
      return 1;  // 8in16
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R32G32B32_FLOAT:
    case DXGI_FORMAT_R8G8B8A8_UINT:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UINT:
      return 2;  // 8in32
    default:
      return 2;  // 32-bit swap is the safe default for the packed dword formats
  }
}

// The vertex formats whose components are 16 bits wide. Only these lose their
// element order when the stream around them is swapped at 32-bit width: the
// swap reverses the four bytes of the dword, which puts the second half-word
// first. Values stay intact, the pair just arrives as (y, x).
bool Is16BitComponentFormat(uint32_t dxgi) {
  switch (DXGI_FORMAT(dxgi)) {
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
      return true;
    default:
      return false;
  }
}

inline uint32_t R32(const uint8_t* base, uint32_t ea) {
  if (ea < 0x1000u) {
    return 0;
  }
  uint32_t v;
  std::memcpy(&v, rex::memory::GuestPtr(const_cast<uint8_t*>(base), ea), 4);
  return __builtin_bswap32(v);
}

inline uint8_t R8(const uint8_t* base, uint32_t ea) {
  if (ea < 0x1000u) {
    return 0;
  }
  return *reinterpret_cast<const uint8_t*>(
      rex::memory::GuestPtr(const_cast<uint8_t*>(base), ea));
}

// DeclUsage -> the semantic names XenosRecomp emits (USAGE_TYPES order).
const char* UsageToSemantic(uint32_t usage) {
  switch (usage) {
    case 0: return "POSITION";
    case 1: return "BLENDWEIGHT";
    case 2: return "BLENDINDICES";
    case 3: return "NORMAL";
    case 4: return "PSIZE";
    case 5: return "TEXCOORD";
    case 6: return "TANGENT";
    case 7: return "BINORMAL";
    case 8: return "TESSFACTOR";
    case 9: return "POSITIONT";
    case 10: return "COLOR";
    case 11: return "FOG";
    case 12: return "DEPTH";
    case 13: return "SAMPLE";
    default: return nullptr;
  }
}

// Diagnostic for the wedges over the map: a draw whose indices reach past the
// vertices its fetch constant covers. Reads the guest memory the fetch points
// at, at draw time, and reports what is actually there at the first vertex the
// index buffer asks for and the fetch does not cover. Two outcomes, two
// different bugs:
//   uninitialised fill (0xCDCDCDCD) -> the vertex buffer really is that small
//     and the INDEX buffer is stale, left over from a bigger mesh;
//   plausible coordinates -> the vertices exist and the fetch constant (or the
//     address it carries) is the stale side.
// Only runs on the draws that already look wrong, so it costs nothing on the
// normal path, and reports each distinct stream address once, up to a cap.
void ProbeIndexRangeAgainstStream(const GeometrySnapshot& s, uint32_t element_count,
                                  uint32_t start_element) {
  static uint32_t reports = 0;
  static std::set<uint32_t> seen;
  constexpr uint32_t kMaxReports = 64;
  if (!s.indexed || s.index_32bit || reports >= kMaxReports || s.streams.empty()) {
    return;
  }
  const VertexStream& stream = s.streams[0];
  if (!stream.stride || stream.zero_fill) {
    return;
  }
  const uint32_t have = stream.guest_size / stream.stride;
  if (uint64_t(element_count) * stream.stride <= stream.guest_size) {
    return;  // cannot reach past the view no matter what the indices say
  }
  const uint8_t* ib = TranslatePhysicalGuest(s.index_guest_base);
  const uint8_t* vb = TranslatePhysicalGuest(stream.guest_base);
  if (!ib || !vb) {
    return;
  }
  // The draw starts at start_element, and 16-bit indices come byte-swapped
  // under k8in16 (endian 1) but dword-swapped under k8in32 (endian 2), where
  // the two halves of every pair also trade places. Reading them the wrong way
  // is what produced the first round of nonsense maxima (0xC000, 0xFFF3).
  const uint32_t endian = s.index_endian;
  uint32_t imax = 0;
  uint32_t restarts = 0;
  for (uint32_t i = 0; i < element_count; ++i) {
    const uint32_t slot = start_element + i;
    size_t byte = size_t(slot) * 2;
    if (endian == 2) {
      byte = (size_t(slot ^ 1)) * 2;  // 8in32 also swaps the pair
    }
    uint16_t idx;
    std::memcpy(&idx, ib + byte, 2);
    if (endian == 1 || endian == 2) {
      idx = uint16_t((idx >> 8) | (idx << 8));
    }
    if (idx == 0xFFFFu) {
      ++restarts;  // primitive restart, not a vertex
      continue;
    }
    if (idx > imax) {
      imax = idx;
    }
  }
  if (imax < have) {
    return;
  }
  if (!seen.insert(stream.guest_base).second) {
    return;
  }
  ++reports;
  // First vertex the fetch does not cover, read straight from guest memory.
  const uint32_t probe_vertex = have;
  uint32_t words[3] = {0, 0, 0};
  float pos[3] = {0.0f, 0.0f, 0.0f};
  bool readable = IsPhysicalRangeReadable(stream.guest_base + probe_vertex * stream.stride, 12);
  if (readable) {
    std::memcpy(words, vb + size_t(probe_vertex) * stream.stride, 12);
    for (uint32_t i = 0; i < 3; ++i) {
      words[i] = __builtin_bswap32(words[i]);
      std::memcpy(&pos[i], &words[i], 4);
    }
  }
  REXLOG_WARN(
      "[native_gfx] index range past fetch: base={:#010x} size={} stride={} have={} imax={} "
      "elements={} start={} ib={:#010x} ib_endian={} restarts={} | vertex[{}] readable={} "
      "raw={:#010x},{:#010x},{:#010x} pos=({}, {}, {})",
      stream.guest_base, stream.guest_size, stream.stride, have, imax, element_count, start_element,
      s.index_guest_base, endian, restarts, probe_vertex, readable, words[0], words[1], words[2],
      pos[0], pos[1], pos[2]);
}

}  // namespace

uint32_t PrimitiveTypeToTopology(uint32_t primitive_type) {
  switch (primitive_type) {
    case 1: return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
    case 2: return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
    case 3: return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
    case 4: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    case 6: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
    // kTriangleFan (5), kRectangleList (8) and kQuadList (13) have no D3D12
    // equivalent and need index expansion; the draw pipeline handles them.
    default: return D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
  }
}

// TEMP INSTRUMENTATION: cumulative microseconds per phase of
// BuildGeometrySnapshot, in the order ucode, decl, match, index, resolve.
// Early returns simply stop marking, so a bailed-out draw charges only the
// phases it actually reached.
static double g_geometry_phase_us[5] = {};

const double* GeometryPhaseMicroseconds() { return g_geometry_phase_us; }

GeometrySnapshot BuildGeometrySnapshot(const uint8_t* base, uint32_t dev, uint32_t primitive_type,
                                       uint32_t element_count, uint32_t start_element,
                                       int32_t base_vertex, bool indexed, ShaderDatabase& shaders,
                                       BufferCache* buffers, D3D12Context* context,
                                       ID3D12GraphicsCommandList* cl,
                                       const InlineGeometry* inline_geometry) {
  auto phase_prev = std::chrono::steady_clock::now();
  const auto phase_mark = [&phase_prev](int slot) {
    const auto now = std::chrono::steady_clock::now();
    g_geometry_phase_us[slot] += std::chrono::duration<double, std::micro>(now - phase_prev).count();
    phase_prev = now;
  };

  GeometrySnapshot s;
  s.primitive_type = primitive_type;
  s.element_count = element_count;
  s.start_element = start_element;
  s.base_vertex = base_vertex;
  s.indexed = indexed;

  // --- bound vertex shader: microcode is the authoritative vertex layout.
  // The variant matters: vfetch patching is per variant, so reading the
  // wrong one yields a fully unpatched shader.
  const uint32_t vs_obj = R32(base, dev + kDevVertexShaderOffset);
  const uint32_t ps_obj = R32(base, dev + kDevPixelShaderOffset);
  const uint32_t variant = SelectVertexShaderVariant(base, vs_obj, ps_obj);
  const ShaderUcodeRef vs = ReadVertexShaderUcode(base, vs_obj, variant);
  if (!vs.valid() || vs.size_bytes > kMaxUcodeBytes ||
      !IsGuestRangeReadable(vs.guest_address, vs.size_bytes)) {
    s.failure = "no readable vertex shader microcode";
    return s;
  }
  const uint8_t* ucode = reinterpret_cast<const uint8_t*>(
      rex::memory::GuestPtr(const_cast<uint8_t*>(base), vs.guest_address));
  const uint64_t vs_key = ShaderIdentity(ucode, vs.size_bytes);

  uint32_t velem_count = 0;
  const ShaderDatabase::VertexElementRef* velems =
      shaders.GetVertexElements(vs_key, &velem_count);
  if (!velems || velem_count == 0) {
    s.failure = "vertex shader has no vertex element table in the pack";
    return s;
  }

  phase_mark(0);

  // --- the bound vertex declaration is the authoritative layout source.
  // Reading it back out of the patched microcode is NOT reliable: when the
  // canonical microcode is still referenced by in-flight GPU work the D3D
  // runtime patches a copy in the command buffer instead (sub_82424320) and
  // leaves the canonical copy untouched. This mirrors what the patcher
  // itself (sub_82423A38) does: for each element the shader declares, find
  // the declaration element with the same usage/usageIndex, and take stream,
  // offset and decltype from it plus the stride from the device table.
  const uint32_t decl_obj = R32(base, dev + kDevVertexDeclarationOffset);
  if (!decl_obj) {
    s.failure = "no vertex declaration bound";
    return s;
  }
  const uint32_t decl_count = R32(base, decl_obj + kDeclOffsetElementCount);
  if (decl_count == 0 || decl_count > 64) {
    s.failure = "vertex declaration has an implausible element count";
    return s;
  }
  const uint32_t decl_records_ea = decl_obj + kDeclOffsetElements;
  if (!IsGuestRangeReadable(decl_records_ea, uint64_t(decl_count) * 12)) {
    s.failure = "vertex declaration elements not readable";
    return s;
  }
  const uint8_t* decl_records = reinterpret_cast<const uint8_t*>(
      rex::memory::GuestPtr(const_cast<uint8_t*>(base), decl_records_ea));

  // TEMP DIAG (remove after): the WHOLE bound declaration next to the usages
  // the shader declares. The composite's fullscreen quad reaches the GPU with
  // TEXCOORD0 as a single R32_FLOAT (measured: u = 0,1,0,1 across the corners,
  // no v), which is why every pixel samples the same row. The match loop below
  // takes the FIRST record with the same usage+usage_index, so this shows
  // whether the guest really declares a scalar or whether a second record
  // (e.g. TEXCOORD1 at offset 32 of a 36-byte stride) carries the missing
  // component.
  if (inline_geometry) {
    static std::set<uint64_t> seen_decl;
    uint64_t sig = uint64_t(decl_count) << 32 ^ velem_count;
    for (uint32_t d = 0; d < decl_count; ++d) {
      const DeclarationElement e = ReadDeclarationElement(decl_records, d);
      sig = sig * 1099511628211ull ^ (uint64_t(e.type) << 16) ^ (uint64_t(e.usage) << 8) ^
            uint64_t(e.offset);
    }
    if (seen_decl.insert(sig).second) {
      if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
        // Read PAST decl_count on purpose. The 4 records reported for the
        // composite cover offsets 0/12/24/28 of a 36-byte stride, leaving
        // exactly 4 bytes at 32 unaccounted -- one float, which is the size of
        // the TEXCOORD1 the shader asks for and we report as unsupplied. If
        // valid-looking records follow the count, the declaration is being read
        // truncated and the "missing" attribute was there all along.
        std::fprintf(f, "DECL count=%u shader_velems=%u (dumping %u for overread check)\n",
                     decl_count, velem_count, decl_count + 3u);
        for (uint32_t d = 0; d < decl_count + 3u; ++d) {
          const uint32_t rec_ea = decl_records_ea + 12u * d;
          if (!IsGuestRangeReadable(rec_ea, 12)) {
            std::fprintf(f, "  rec%u UNREADABLE\n", d);
            break;
          }
          const DeclarationElement e = ReadDeclarationElement(decl_records, d);
          const uint8_t* raw = decl_records + 12u * d;
          std::fprintf(f,
                       "  rec%u%s stream=%u offset=%u type=0x%08X usage=%u idx=%u -> dxgi=%u | raw="
                       "%02X%02X %02X%02X %02X%02X%02X%02X %02X %02X %02X %02X\n",
                       d, d >= decl_count ? " [PAST COUNT]" : "", e.stream, e.offset, e.type,
                       e.usage, e.usage_index, DeclTypeToDxgi(e.type), raw[0], raw[1], raw[2],
                       raw[3], raw[4], raw[5], raw[6], raw[7], raw[8], raw[9], raw[10], raw[11]);
        }
        for (uint32_t i = 0; i < velem_count; ++i) {
          std::fprintf(f, "  shader wants usage=%u idx=%u addr=%u\n", velems[i].usage,
                       velems[i].usage_index, velems[i].address);
        }
        std::fflush(f);
        std::fclose(f);
      }
    }
  }

  const uint32_t shadow = dev + kDevFetchShadowOffset;
  phase_mark(1);

  for (uint32_t i = 0; i < velem_count; ++i) {
    const ShaderDatabase::VertexElementRef& ve = velems[i];
    const char* semantic = UsageToSemantic(ve.usage);
    if (!semantic) {
      s.failure = "unsupported vertex usage";
      s.failure_usage = ve.usage;
      s.failure_address = ve.address;
      return s;
    }

    // Match on usage + usageIndex, exactly like the patcher's inner loop.
    const DeclarationElement* match = nullptr;
    DeclarationElement de;
    for (uint32_t d = 0; d < decl_count; ++d) {
      de = ReadDeclarationElement(decl_records, d);
      if (de.usage == ve.usage && de.usage_index == ve.usage_index) {
        match = &de;
        break;
      }
    }
    if (!match) {
      // The declaration does not provide this attribute. On hardware the vfetch
      // is left unpatched and the Xenos delivers zeros, so bind a zero stream
      // with stride 0 -- every vertex then reads the same zeros, which is the
      // same thing. This used to REJECT the whole draw: measured 9243 of 9963
      // such rejections were display-shaped draws, i.e. the composite pass we
      // present was being gutted and came out a single flat colour.
      //
      // The format is float4 because the data is all zeros: any float format
      // reads 0, and R32G32B32A32 covers every component a shader might declare.
      s.unsupplied.push_back(UnsuppliedAttribute{semantic, ve.usage_index, ve.address});

      uint32_t zero_slot = uint32_t(s.streams.size());
      bool have_zero = false;
      for (uint32_t k = 0; k < s.streams.size(); ++k) {
        if (s.streams[k].zero_fill) {
          zero_slot = k;
          have_zero = true;
          break;
        }
      }
      if (!have_zero) {
        VertexStream zero;
        zero.zero_fill = true;
        zero.stride = 0;  // every vertex reads offset 0
        zero.guest_size = 256;
        s.streams.push_back(zero);
      }
      InputElement ze;
      ze.semantic_name = semantic;
      ze.semantic_index = ve.usage_index;
      ze.dxgi_format = uint32_t(DXGI_FORMAT_R32G32B32A32_FLOAT);
      ze.input_slot = zero_slot;
      ze.aligned_byte_offset = 0;
      s.input_layout.push_back(ze);
      continue;
    }

    uint32_t dxgi = DeclTypeToDxgi(match->type);
    // Inline (BeginVertices/EndVertices) draws: the declaration bound on the
    // device describes the buffer-fed layout, not the one the guest wrote into
    // the command stream, and for the composite's fullscreen quad it comes out
    // one component short. Measured: the composite VS reads TEXCOORD0.xy (DXIL
    // input signature: TEXCOORD0 used=0x3) while the declaration supplies
    // R32_FLOAT, so v defaults to 0 and every pixel samples row 0 -- the whole
    // target ends up a single colour. Widen only when the stride actually has
    // room for the second float, so nothing can read past the vertex.
    if (inline_geometry && ve.usage == 5 /* TEXCOORD */ &&
        dxgi == uint32_t(DXGI_FORMAT_R32_FLOAT) &&
        uint32_t(match->offset) + 8u <= inline_geometry->stride) {
      dxgi = uint32_t(DXGI_FORMAT_R32G32_FLOAT);
    }
    if (dxgi == DXGI_FORMAT_UNKNOWN) {
      s.failure = "unsupported vertex decltype";
      s.failure_usage = ve.usage;
      s.failure_format = match->type;
      s.failure_address = ve.address;
      s.failure_offset = match->offset;
      return s;
    }

    // Stream -> fetch slot, and the stride table, both from the patcher.
    const uint32_t fetch_slot = FetchSlotForStream(match->stream);
    const uint32_t stride =
        uint32_t(R8(base, dev + kDevStreamStrideTableOffset + match->stream)) * 4;

    const uint32_t group = fetch_slot / 3;
    const uint32_t pair = fetch_slot % 3;
    if (group >= kFetchShadowGroups) {
      s.failure = "vertex fetch slot out of range";
      return s;
    }
    // Inline geometry never reaches the fetch shadow: the guest writes that
    // fetch constant into the command stream instead, so reading the shadow
    // here would find a stale binding or none at all.
    VertexFetch vf;
    uint32_t effective_stride = stride;
    if (inline_geometry) {
      vf.type = 3;
      vf.guest_address = inline_geometry->address;
      vf.size_bytes = inline_geometry->size_bytes;
      vf.endian = inline_geometry->endian;
      // The stride table is not updated for inline draws either; the value
      // BeginVertices was called with is the only correct one.
      effective_stride = inline_geometry->stride;
    } else {
      const uint32_t ea = shadow + group * kFetchGroupDwords * 4 + pair * 8;
      vf = DecodeVertexFetch(R32(base, ea), R32(base, ea + 4));
    }
    if (!vf.valid()) {
      s.failure = "vertex fetch constant not bound for stream";
      s.failure_usage = ve.usage;
      s.failure_address = fetch_slot;
      return s;
    }

    // One VertexStream per distinct stream; attributes share it.
    uint32_t slot_index = uint32_t(s.streams.size());
    bool found = false;
    for (uint32_t k = 0; k < s.streams.size(); ++k) {
      if (s.streams[k].fetch_slot == fetch_slot) {
        slot_index = k;
        found = true;
        break;
      }
    }
    if (!found) {
      // TEMP DIAG (remove once the stride is settled): the whole chain that
      // decides the vertex stride, once per distinct shape.
      //
      // Measured in "native bugs.rdc": draws 22001 and 24762 bind stride 28
      // while their vertex buffer holds a 40-byte period -- decoded at 40 the
      // positions form a clean lattice, at 28 they are garbage, and the
      // collapsed vertices are what stretch a triangle across the screen.
      //
      // Every step of the guest chain was checked against the XEX and matches
      // what this code does: SetStreamSource stores stride>>2 at
      // dev+12528+stream (sub_82415DC8), and the vfetch patcher indexes that
      // same table with the matched declaration record's stream field
      // (sub_82423A38: `a4[*v16]`, a4 = dev+12528). So the disagreement is in a
      // value only a live dump can show -- which record got matched, what its
      // stream is, and where the fetch constant actually points.
      if (!inline_geometry) {
        static std::set<uint64_t> seen_stride;
        static uint32_t lines = 0;
        const uint64_t sig = (uint64_t(match->stream) << 56) ^ (uint64_t(stride) << 48) ^
                             (uint64_t(decl_count) << 40) ^ (uint64_t(match->offset) << 24) ^
                             uint64_t(match->type);
        if (lines < 128 && seen_stride.insert(sig).second) {
          ++lines;
          if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
            std::fprintf(f,
                         "STRIDE usage=%u/%u stream=%u table[%u]=%u(->%u) mirror[%u]=%u "
                         "decl_off=%u decl_type=0x%08X decl_count=%u fetch_slot=%u "
                         "addr=0x%08X size=%u\n",
                         ve.usage, ve.usage_index, match->stream, match->stream,
                         uint32_t(R8(base, dev + kDevStreamStrideTableOffset + match->stream)),
                         stride, match->stream,
                         uint32_t(R8(base, dev + kDevDeclStrideMirrorOffset + match->stream)),
                         match->offset, match->type, decl_count, fetch_slot, vf.guest_address,
                         vf.size_bytes);
            // Every record, so a wrong match is visible next to the right one.
            for (uint32_t d = 0; d < decl_count && d < 16; ++d) {
              const DeclarationElement r = ReadDeclarationElement(decl_records, d);
              std::fprintf(f, "  decl%u stream=%u off=%u type=0x%08X usage=%u idx=%u\n", d,
                           r.stream, r.offset, r.type, r.usage, r.usage_index);
            }
            std::fflush(f);
            std::fclose(f);
          }
        }
      }
      VertexStream stream;
      stream.fetch_slot = fetch_slot;
      stream.guest_base = vf.guest_address;
      stream.guest_size = vf.size_bytes;
      stream.stride = effective_stride;
      // Inline (2D/UI) vertices are host-built and skip the swap entirely (see
      // host_gpu_address below); for scene geometry derive the swap width from
      // the attribute FORMAT, not the fetch constant, so a 16-bit position is
      // never swapped as if it were 32-bit (the geometry-shatter bug).
      stream.endian = inline_geometry ? vf.endian : EndianForDxgiFormat(dxgi);
      s.streams.push_back(stream);
    }

    // A stream carries ONE swap width, chosen above from whichever attribute
    // reached it first -- but a 28-byte RAGE vertex mixes a 32-bit POSITION
    // with a 16-bit TEXCOORD, and the two want different widths. When the
    // stream ends up swapped 8in32, the 16-bit pair's halves trade places and
    // the shader reads (v, u): every texture comes out transposed, which on
    // screen reads as a 90-degree rotation to the left.
    //
    // XenosRecomp compiles the correction into the vertex shader already --
    // tfetchTexcoord() returns value.yxwz when bit `usageIndex` of
    // g_SwappedTexcoords is set (see its README on 16-bit vertex formats) --
    // and nothing was ever filling that mask, so it stayed 0 = identity.
    //
    // Measured in "brokenbuilds.rdc" draw 16209, the SAVINGS & TRUST sign:
    // input layout is POSITION R32G32B32_FLOAT @0 + TEXCOORD R16G16_FLOAT @20
    // in one 28-byte stream, and the post-VS TEXCOORD is (1,0) (1,1) (0,0)
    // (0,1) for screen corners bottom-left, bottom-right, top-left, top-right
    // -- u running down the screen and v across it, i.e. transposed.
    //
    // Host-built inline vertices are already in host order and never go
    // through the swap, so they are exempt.
    const bool host_built = inline_geometry && inline_geometry->host_gpu_address != 0;
    if (ve.usage == 5 /* TEXCOORD */ && !host_built && ve.usage_index < 32 &&
        Is16BitComponentFormat(dxgi) && s.streams[slot_index].endian == 2 /* k8in32 */) {
      s.swapped_texcoords |= 1u << ve.usage_index;
    }

    // Skinning: peds/vehicles declare BLENDWEIGHT/BLENDINDICES with the D3DCOLOR
    // decltype, which DeclTypeToDxgi maps to B8G8R8A8_UNORM -- and that is
    // exactly right, so nothing is overridden here.
    //
    // This used to force BLENDINDICES to R8G8B8A8_UINT on the theory that bone
    // indices are integers. They are, but the Xenos delivers them NORMALISED
    // (D3DCOLOR), and the microcode scales them back with its own constant:
    // measured in the car shaders, `r0 = iBlendIndices * c254.zzzz` with
    // c254.z = 765.006 = 255 * 3, three constant registers per bone. Feeding the
    // raw byte overshot by 255x -- bone 2 asked for register 1530, the shader's
    // own min(index, 191) clamp caught it, and every vertex not bound to bone 0
    // read the same matrix, which folded the car body into a vertical sliver
    // while the unskinned wheels stayed correct ("where-the-car.rdc", draws
    // 37604/37623/37642/37661).
    //
    // The paired weights keep the declaration's format too. Indices and weights
    // then land in the same component order, which is all the skinning sum needs
    // -- it pairs weight[i] with index[i], so a consistent permutation of both
    // is harmless.
    const uint32_t use_fmt = dxgi;

    InputElement e;
    e.semantic_name = semantic;
    e.semantic_index = ve.usage_index;
    e.dxgi_format = use_fmt;
    e.input_slot = slot_index;
    e.aligned_byte_offset = match->offset;
    s.input_layout.push_back(e);
  }

  phase_mark(2);

  // --- index buffer
  if (indexed) {
    const uint32_t ib_obj = R32(base, dev + kDevIndexBufferOffset);
    if (!ib_obj) {
      s.failure = "indexed draw with no index buffer bound";
      return s;
    }
    const GuestIndexBuffer ib =
        DecodeIndexBuffer(R32(base, ib_obj), R32(base, ib_obj + kIndexBufferObjAddrOffset));
    if (!ib.valid()) {
      s.failure = "index buffer object has no address";
      return s;
    }
    s.index_32bit = ib.indices_32bit;
    // DecodeIndexBuffer keeps the field where the guest word holds it.
    s.index_endian = ib.endian >> 30;
    s.index_guest_base = ib.guest_address;
    const uint32_t index_size = ib.indices_32bit ? 4u : 2u;
    s.index_buffer_bytes = (start_element + element_count) * index_size;
  }

  phase_mark(3);

  // --- resolve to native resources (skipped in guest-only diagnostic mode)
  if (buffers && context && cl) {
    for (VertexStream& stream : s.streams) {
      if (stream.zero_fill) {
        // Shared, permanently zero, bound with stride 0. Not routed through the
        // BufferCache: it has no guest backing to resolve against.
        stream.gpu_address = buffers->ZeroStreamAddress(*context);
        stream.resolved = stream.gpu_address != 0;
        if (!stream.resolved) {
          s.failure = "zero vertex stream unavailable";
          return s;
        }
        continue;
      }
      if (inline_geometry && inline_geometry->host_gpu_address) {
        // Host-built vertices (see InlineGeometry): already in host byte order
        // and already the right count, so the BufferCache must not touch them.
        stream.gpu_address = inline_geometry->host_gpu_address;
        stream.guest_size = inline_geometry->host_size_bytes;
        stream.resolved = true;
        continue;
      }
      // The fetch constant's size is authoritative here, and widening the view
      // to cover the index range is NOT the fix for the wedges over the map.
      //
      // Measured in "sem2d+mapglitch.rdc", draw 62374 (the map's road overlay,
      // stride 32, baseVertex 0): the fetch says 3232 bytes = 101 vertices,
      // the index buffer is a clean quad list reaching vertex 403. Reading the
      // uploaded region past those 101 vertices gives 0xCDCDCDCD, so the
      // vertices the indices ask for were never written -- the vertex buffer
      // is current and the INDEX buffer is stale, left over from a larger
      // mesh. Its region was keyed by the windowed address (0xB0710270) while
      // the write watch reports physical ones, so it was uploaded once and
      // never invalidated again; see BufferCache::Resolve.
      BufferBinding b;
      if (!buffers->Resolve(*context, cl, stream.guest_base, stream.guest_size,
                            SwapForEndian(stream.endian), b)) {
        s.failure = "vertex buffer could not be resolved";
        return s;
      }
      stream.resource_base = b.region_base;
      stream.resource_size = b.region_size;
      stream.view_offset = b.view_offset;
      stream.gpu_address = b.gpu_address;
      stream.resolved = true;
    }
    if (s.indexed) {
      BufferBinding b;
      if (!buffers->Resolve(*context, cl, s.index_guest_base, s.index_buffer_bytes,
                            SwapForEndian(s.index_endian), b)) {
        s.failure = "index buffer could not be resolved";
        return s;
      }
      s.index_gpu_address = b.gpu_address;
    }
    ProbeIndexRangeAgainstStream(s, element_count, start_element);
  }

  phase_mark(4);

  s.complete = true;
  return s;
}

}  // namespace mcla::native_gfx

#endif // REXGLUE_HAS_XEO3_TARGET
