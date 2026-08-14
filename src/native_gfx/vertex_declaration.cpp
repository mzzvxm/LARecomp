#ifndef REXGLUE_HAS_XEO3_TARGET
// MCLA Native Graphics Runtime — vertex declaration decoding.
// See vertex_declaration.h for the reverse-engineering provenance.

#include "vertex_declaration.h"

#include <dxgiformat.h>

namespace mcla::native_gfx {

namespace {

inline uint16_t LoadBe16(const uint8_t* p) { return uint16_t(p[0]) << 8 | p[1]; }
inline uint32_t LoadBe32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]);
}

// Packed Xenon decltype -> DXGI. Exhaustive over dword_827D42B0; anything
// else is reported, not guessed.
DXGI_FORMAT TranslateType(uint32_t packed) {
  switch (packed) {
    case 0x002C235F: return DXGI_FORMAT_R16G16_FLOAT;
    case 0x001A2360: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case 0x002C23A5: return DXGI_FORMAT_R32_FLOAT;
    case 0x001A23A6: return DXGI_FORMAT_R32G32_FLOAT;
    case 0x002A23B9: return DXGI_FORMAT_R32G32B32_FLOAT;
    case 0x001A2286: return DXGI_FORMAT_R8G8B8A8_UINT;
    case 0x00182886: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case 0x001A2187: return DXGI_FORMAT_R10G10B10A2_UINT;
    default: return DXGI_FORMAT_UNKNOWN;
  }
}

// D3DDECLUSAGE -> semantic name as emitted by XenosRecomp.
const char* TranslateUsage(uint8_t usage) {
  switch (usage) {
    case 0: return "POSITION";
    case 1: return "BLENDWEIGHT";
    case 2: return "BLENDINDICES";
    case 3: return "NORMAL";
    case 4: return "PSIZE";
    case 5: return "TEXCOORD";
    case 6: return "TANGENT";
    case 7: return "BINORMAL";
    case 10: return "COLOR";
    default: return nullptr;
  }
}

}  // namespace

uint32_t DeclTypeToDxgi(uint32_t packed_type) { return uint32_t(TranslateType(packed_type)); }

DeclarationElement ReadDeclarationElement(const uint8_t* records, uint32_t index) {
  DeclarationElement e;
  if (!records) {
    return e;
  }
  const uint8_t* r = records + 12 * index;
  e.stream = LoadBe16(r + 0);
  e.offset = LoadBe16(r + 2);
  e.type = LoadBe32(r + 4);
  e.usage = r[9];
  // usage_index is the byte at +10, NOT a big-endian 16-bit field spanning
  // +10/+11. Reading it as LoadBe16(r+10) was tried and is wrong: it made
  // rej_unsupplied jump from 40k to 4.07M, i.e. almost nothing matched any
  // more. Whatever r[11] carries (it is 0 or 1 across records), it is not part
  // of this field.
  e.usage_index = r[10];
  return e;
}

VertexDeclarationDesc DecodeVertexDeclaration(const uint8_t* records_be, size_t count) {
  VertexDeclarationDesc desc;
  if (records_be == nullptr) {
    desc.unknown_field = true;
    return desc;
  }
  desc.elements.reserve(count);
  uint32_t max_stream = 0;
  for (size_t i = 0; i < count; ++i) {
    const uint8_t* r = records_be + 12 * i;
    const uint16_t stream = LoadBe16(r + 0);
    if (stream == 0xFF) {
      break;  // D3DDECL_END
    }
    const uint16_t offset = LoadBe16(r + 2);
    const uint32_t type = LoadBe32(r + 4);
    const uint8_t usage = r[9];
    const uint8_t usage_index = r[10];

    InputElement e;
    e.semantic_name = TranslateUsage(usage);
    e.semantic_index = usage_index;
    e.dxgi_format = uint32_t(TranslateType(type));
    e.input_slot = stream;
    e.aligned_byte_offset = offset;

    if (e.semantic_name == nullptr || e.dxgi_format == DXGI_FORMAT_UNKNOWN) {
      desc.unknown_field = true;
      if (desc.first_unknown_type == 0) {
        desc.first_unknown_type = e.dxgi_format == DXGI_FORMAT_UNKNOWN ? type : usage;
      }
    }
    if (stream > max_stream) {
      max_stream = stream;
    }
    desc.elements.push_back(e);
  }
  desc.stream_count = desc.elements.empty() ? 0 : max_stream + 1;
  return desc;
}

}  // namespace mcla::native_gfx

#endif // REXGLUE_HAS_XEO3_TARGET
