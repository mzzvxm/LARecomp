#ifndef REXGLUE_HAS_XEO3_TARGET
// MCLA Native Graphics Runtime — vertex fetch instruction decoding.
// See vertex_fetch_decode.h. The control-flow walk is the same one
// shader_identity.cpp uses to find these instructions; the two must agree on
// what counts as a vertex fetch.

#include "vertex_fetch_decode.h"

#include <dxgiformat.h>

namespace mcla::native_gfx {

namespace {

inline uint32_t LoadBe32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]);
}

inline bool IsExecOpcode(uint32_t op) {
  return (op >= 1 && op <= 7) || op == 13 || op == 14;
}

}  // namespace

std::vector<VertexFetchInstr> DecodeVertexFetches(const uint8_t* ucode, size_t size) {
  std::vector<VertexFetchInstr> out;
  const size_t n = size / 4;
  if (!ucode || n < 3) {
    return out;
  }

  struct ExecRef {
    uint32_t address, count, sequence;
  };
  std::vector<ExecRef> execs;
  size_t limit = n;
  for (size_t i = 0; i + 2 < limit; i += 3) {
    const uint32_t d0 = LoadBe32(ucode + 4 * i);
    const uint32_t d1 = LoadBe32(ucode + 4 * (i + 1));
    const uint32_t d2 = LoadBe32(ucode + 4 * (i + 2));
    const uint32_t w0[2] = {d0, (d1 >> 16) | (d2 << 16)};
    const uint32_t w1[2] = {d1 & 0xFFFFu, d2 >> 16};
    for (int k = 0; k < 2; ++k) {
      if (!IsExecOpcode((w1[k] >> 12) & 0xF)) {
        continue;
      }
      const uint32_t addr = w0[k] & 0xFFFu;
      if (addr == 0) {
        continue;
      }
      if (size_t(addr) * 3 < limit) {
        limit = size_t(addr) * 3;
      }
      execs.push_back(ExecRef{addr, (w0[k] >> 12) & 0x7u, (w0[k] >> 16) & 0xFFFu});
    }
  }

  for (const ExecRef& e : execs) {
    const uint32_t cap = e.count < 6 ? e.count : 6;
    for (uint32_t k = 0; k < cap; ++k) {
      if (((e.sequence >> (2 * k)) & 1u) == 0) {
        continue;  // ALU
      }
      const uint32_t instr_addr = e.address + k;
      const size_t base = size_t(instr_addr) * 3;
      if (base + 2 >= n) {
        continue;
      }
      const uint32_t w0 = LoadBe32(ucode + 4 * base);
      if ((w0 & 0x1Fu) != 0) {
        continue;  // texture fetch
      }
      const uint32_t w1 = LoadBe32(ucode + 4 * (base + 1));
      const uint32_t w2 = LoadBe32(ucode + 4 * (base + 2));

      VertexFetchInstr f;
      f.address = instr_addr;
      // The fetch slot is a single 7-bit field at bit 20, NOT const_index*3 +
      // const_index_sel: the patcher (sub_82423A38) writes `95 - stream`
      // masked with 0x7F00000 into word0. The two readings agree only for
      // slot 95 (0b1011111 -> 31*3+2 = 95), which is stream 0 and therefore
      // almost every draw — that coincidence hid the bug.
      f.fetch_slot = (w0 >> 20) & 0x7Fu;
      f.dst_swizzle = w1 & 0xFFFu;
      f.is_float = ((w1 >> 13) & 0x1u) != 0;  // num_format_all
      f.signed_format = ((w1 >> 12) & 0x1u) != 0;  // fomat_comp_all
      f.format = (w1 >> 16) & 0x3Fu;
      f.stride_bytes = (w2 & 0xFFu) * 4;   // stride is in dwords
      f.offset_bytes = ((w2 >> 8) & 0x7FFFFFu) * 4;  // offset is in dwords
      out.push_back(f);
    }
  }
  return out;
}

uint32_t VertexFormatSizeBytes(uint32_t format) {
  switch (format) {
    case 6:   // k_8_8_8_8
    case 7:   // k_2_10_10_10
    case 16:  // k_10_11_11
    case 17:  // k_11_11_10
    case 25:  // k_16_16
    case 31:  // k_16_16_FLOAT
    case 33:  // k_32
    case 36:  // k_32_FLOAT
      return 4;
    case 26:  // k_16_16_16_16
    case 32:  // k_16_16_16_16_FLOAT
    case 34:  // k_32_32
    case 37:  // k_32_32_FLOAT
      return 8;
    case 57:  // k_32_32_32_FLOAT
      return 12;
    case 35:  // k_32_32_32_32
    case 38:  // k_32_32_32_32_FLOAT
      return 16;
    default:
      return 0;
  }
}

uint32_t VertexFormatToDxgi(uint32_t format, bool signed_format, bool is_float) {
  switch (format) {
    case 6:  // k_8_8_8_8
      return signed_format ? DXGI_FORMAT_R8G8B8A8_SNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
    case 7:  // k_2_10_10_10 -- no SNORM equivalent in DXGI, so it reaches the
             // shader as a raw dword and tfetchR11G11B10() unpacks the three
             // signed 10-bit components.
             //
             // R10G10B10A2_UINT was wrong here even though the bit widths line
             // up: it makes the input assembler SPLIT the dword into four
             // components, so the shader's value.x holds only the low 10 bits
             // and the unpack has nothing left to shift. With the spec constant
             // off the shader then ran asfloat() over that small integer, which
             // is a denormal indistinguishable from zero -- measured as a zero
             // normal on 2148 of 2158 normal/tangent attributes in one frame,
             // i.e. the whole game shaded without normals.
      return DXGI_FORMAT_R32_UINT;
    case 16:  // k_10_11_11
    case 17:  // k_11_11_10
      // No direct DXGI equivalent; delivered as a raw dword and unpacked in
      // the shader.
      return DXGI_FORMAT_R32_UINT;
    case 25:  // k_16_16
      return signed_format ? DXGI_FORMAT_R16G16_SNORM : DXGI_FORMAT_R16G16_UNORM;
    case 26:  // k_16_16_16_16
      return signed_format ? DXGI_FORMAT_R16G16B16A16_SNORM : DXGI_FORMAT_R16G16B16A16_UNORM;
    case 31:  // k_16_16_FLOAT
      return DXGI_FORMAT_R16G16_FLOAT;
    case 32:  // k_16_16_16_16_FLOAT
      return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case 33:  // k_32
      return signed_format ? DXGI_FORMAT_R32_SINT : DXGI_FORMAT_R32_UINT;
    case 34:  // k_32_32
      return signed_format ? DXGI_FORMAT_R32G32_SINT : DXGI_FORMAT_R32G32_UINT;
    case 35:  // k_32_32_32_32
      return signed_format ? DXGI_FORMAT_R32G32B32A32_SINT : DXGI_FORMAT_R32G32B32A32_UINT;
    case 36:  // k_32_FLOAT
      return DXGI_FORMAT_R32_FLOAT;
    case 37:  // k_32_32_FLOAT
      return DXGI_FORMAT_R32G32_FLOAT;
    case 38:  // k_32_32_32_32_FLOAT
      return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case 57:  // k_32_32_32_FLOAT
      return DXGI_FORMAT_R32G32B32_FLOAT;
    default:
      (void)is_float;
      return DXGI_FORMAT_UNKNOWN;
  }
}

}  // namespace mcla::native_gfx

#endif // REXGLUE_HAS_XEO3_TARGET
