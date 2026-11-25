#pragma once
// ===========================================================================
// MCLA GPU renderer -- the car dump prototype: the rules that need no renderer
// ===========================================================================
// perf/CAR-RT-SPIKE.md section 7. The prototype (car_dump.h) takes the triangles of the cars the game draws in one frame from guest
// memory and checks them against the car self-reflection history image. This header holds what needs no renderer, as pure
// functions on plain numbers, so that car_dump_rules_test.cpp builds on its own:
//
//   - the matrix of a draw out of its vertex shader bank (raw big-endian registers): the world matrix gWorld c0-c3, the world
//     to clip matrix gWorldViewProj c8-c11, and the bone the car vertex shaders pick out of the palette gCarMtxBuffer
//     (c64-c255, three registers a bone, the bone chosen per draw by gBoneIndex.x = c204.x):
//         local = [position 1] * palette(bone)     world = [local 1] * gWorld     clip = [local 1] * gWorldViewProj
//     (for the shaders that read the palette: the tyres and the wheel proxies have none, and the character shaders blend bones)
//   - the position of a vertex out of the bytes the guest holds (the formats the game's declarations give a position) and
//     the index bytes under their endian swap
//   - the index list of a draw as a clean list of triangles (no degenerate ones, none that name a vertex the stream does not hold)
//   - the recipe of a draw (palette then gWorld, gWorld alone, or per-vertex blended bones) from what its vertex shader READS: a walk
//     over the shader's Xenos microcode (ScanConstantReads) that says which float constants it reads at a fixed index and whether it
//     reads the bone palette through a0; the blend of a skinned vertex's up to four bones (SkinBlend)
//   - what a draw is (body, skinned part, wheel proxy) and which stencil ID is the player's car (ChoosePlayerId)
//   - a tiny CPU rasteriser with a nearest-distance buffer, and the statistics that compare it with an image of distances
//
// Matrices are row vectors (v' = v M) as in temporal_math.h: a product A * B applies A first. The bank's registers are the
// rows of gWorld and gWorldViewProj; the bone's three registers are the ROWS of a 3x4 matrix applied to a column vector (the
// shader dots each with the position), which is transposed into the row-vector form here.
//
// This header includes nothing of the renderer.
// ===========================================================================

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include "temporal_math.h"

namespace mcla::gpu::car_dump {

// ---------------------------------------------------------------------------
// the registers of a draw
// ---------------------------------------------------------------------------

inline constexpr uint32_t kRegisterCount = 256;
inline constexpr uint32_t kWorldRegister = 0;         // gWorld, c0-c3
inline constexpr uint32_t kWvpRegister = 8;           // gWorldViewProj, c8-c11
inline constexpr uint32_t kBoneIndexRegister = 204;   // gBoneIndex, x = the palette's bone
inline constexpr uint32_t kPaletteRegister = 64;      // gCarMtxBuffer, c64-c255
inline constexpr uint32_t kBoneRegisters = 3;         // a bone's rows
inline constexpr uint32_t kPaletteBones = (kRegisterCount - kPaletteRegister) / kBoneRegisters;  // 64
// How far gWorld times the camera's view-projection may be from the draw's gWorldViewProj, relative to the latter's largest entry.
inline constexpr double kWvpTolerance = 1e-3;

// Component `comp` (0 x, 1 y, 2 z, 3 w) of register `reg` of a bank of raw big-endian bytes (ConstantBanks::Banks).
inline float BankFloat(const uint8_t* bank, uint32_t reg, uint32_t comp) {
  const uint8_t* p = bank + size_t(reg) * 16 + size_t(comp) * 4;
  const uint32_t bits = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
  float value;
  std::memcpy(&value, &bits, 4);
  return value;
}

// Four registers as the rows of a matrix.
inline temporal::Mat4 BankMatrix(const uint8_t* bank, uint32_t first) {
  temporal::Mat4 m;
  for (uint32_t r = 0; r < 4; ++r) {
    for (uint32_t c = 0; c < 4; ++c) {
      m.m[r][c] = double(BankFloat(bank, first + r, c));
    }
  }
  return m;
}

// What one draw's bank says of where its model space is.
struct CarMatrices {
  bool finite = false;       // every number read is finite
  bool bone_valid = false;   // c204.x names a bone of the palette (0 to 63)
  uint32_t bone = 0;         // trunc(c204.x) when it does
  float bone_raw = 0.0f;     // c204.x as it is
  temporal::Mat4 world;      // gWorld, c0-c3
  temporal::Mat4 wvp;        // gWorldViewProj, c8-c11
  temporal::Mat4 palette;    // the bone's three rows as a row-vector matrix: local = [p 1] * palette (the identity without a bone)
  temporal::Mat4 model;      // palette * world: model space to world (recipe A); without a bone, gWorld alone
};

// The row-vector matrix of the palette's bone `bone`: local.x = dot(row0.xyz, p) + row0.w, and so on for y and z.
inline temporal::Mat4 BoneMatrix(const uint8_t* bank, uint32_t bone) {
  temporal::Mat4 m = temporal::Identity();
  const uint32_t first = kPaletteRegister + bone * kBoneRegisters;
  for (uint32_t row = 0; row < kBoneRegisters; ++row) {
    for (uint32_t r = 0; r < 3; ++r) {
      m.m[r][row] = double(BankFloat(bank, first + row, r));
    }
    m.m[3][row] = double(BankFloat(bank, first + row, 3));
  }
  return m;
}

inline CarMatrices DecodeCarMatrices(const uint8_t* vs_bank) {
  CarMatrices out;
  out.world = BankMatrix(vs_bank, kWorldRegister);
  out.wvp = BankMatrix(vs_bank, kWvpRegister);
  out.bone_raw = BankFloat(vs_bank, kBoneIndexRegister, 0);
  out.palette = temporal::Identity();
  const float raw = out.bone_raw;
  if (std::isfinite(raw) && raw >= 0.0f && raw < float(kPaletteBones)) {
    out.bone = uint32_t(raw);  // trunc, as the shader's
    out.bone_valid = true;
    out.palette = BoneMatrix(vs_bank, out.bone);
  }
  out.model = temporal::Mul(out.palette, out.world);
  out.finite = temporal::Finite(out.world) && temporal::Finite(out.wvp) && temporal::Finite(out.palette);
  return out;
}

// ---------------------------------------------------------------------------
// the recipe: how a draw's vertices reach the world, by what its vertex shader READS
// ---------------------------------------------------------------------------
// The car vertex shaders are not alike (perf/CAR-RT-SPIKE.md section 9): the rigid parts read the bone palette gCarMtxBuffer through the
// address register a0 (the bone chosen per draw by gBoneIndex.x = c204.x), the tyres and the wheel blur proxies read no palette at all
// (gWorld alone), and the character shaders (the driver) blend up to four palette bones per vertex. Using the palette on a shader that
// does not read it puts its triangles up to hundreds of metres away (c204 and the palette hold what an earlier draw left there).
// Which constant registers a shader reads is not in the shader pack (its entries hold the texture fetch mask and the vertex elements,
// shaderpack/pack_format.h), but the draw record names the microcode (DrawRecord::vs_ucode_address) and the microcode says it: a walk
// over its exec blocks (ScanConstantReads below) gives the registers read at a fixed index and whether a read goes through a0.
enum class Recipe : uint8_t {
  kPalette,  // [p 1] * palette(bone c204.x) * gWorld: the shader reads the palette by a0 and the bone index c204
  kWorld,    // [p 1] * gWorld: the shader reads no palette (tyres, wheel proxies, parts a skinning pass has placed already)
  kSkinned,  // blend of up to four palette bones per vertex (BLENDINDICES / BLENDWEIGHT), then gWorld: the characters (the driver)
};

inline const char* RecipeName(Recipe recipe) {
  switch (recipe) {
    case Recipe::kPalette: return "palette then gWorld";
    case Recipe::kWorld: return "gWorld alone";
    case Recipe::kSkinned: return "blended bones then gWorld";
  }
  return "?";
}

// Where the recipe came from.
enum class RecipeSource : uint8_t {
  kMicrocode,    // the vertex shader's microcode was read and scanned (ScanConstantReads)
  kDeclaration,  // no microcode: the declaration has BLENDINDICES
  kTable,        // no microcode: the shader is in kKnownWorldOnlyVs
  kDefault,      // no microcode and nothing known: the car default
};

inline const char* RecipeSourceName(RecipeSource source) {
  switch (source) {
    case RecipeSource::kMicrocode: return "microcode";
    case RecipeSource::kDeclaration: return "declaration";
    case RecipeSource::kTable: return "table";
    case RecipeSource::kDefault: return "default";
  }
  return "?";
}

// A draw's model-to-world matrix under a recipe: palette then gWorld for kPalette (without a valid bone that is gWorld alone), gWorld for
// the others (kSkinned: its vertices are blended first, SkinBlend below).
inline const temporal::Mat4& ModelMatrix(const CarMatrices& m, Recipe recipe) {
  return recipe == Recipe::kPalette ? m.model : m.world;
}

// [p 1] * m, all four components of the result.
inline void TransformPoint(const temporal::Mat4& m, const double p[3], double out[4]) {
  for (int c = 0; c < 4; ++c) {
    out[c] = p[0] * m.m[0][c] + p[1] * m.m[1][c] + p[2] * m.m[2][c] + m.m[3][c];
  }
}

// The largest entry of (world * view_projection - wvp) over the largest entry of wvp: 0 when the draw's gWorld and gWorldViewProj
// are the same transform seen through this camera (gWorld is the matrix the shader's own world position goes through).
inline double WvpError(const temporal::Mat4& world, const temporal::Mat4& view_projection, const temporal::Mat4& wvp) {
  const double scale = std::max(temporal::MaxAbs(wvp), 1e-30);
  return temporal::MaxAbsDiff(temporal::Mul(world, view_projection), wvp) / scale;
}

// ---------------------------------------------------------------------------
// vertices and indices as the guest holds them
// ---------------------------------------------------------------------------

// The formats the game's declarations give a POSITION (native_gfx/vertex_declaration.h, gpu/format.cpp VertexFormat), by their
// packed Xenon decltype. Floats are big-endian 32-bit words, half floats big-endian 16-bit words in the order x y z w.
enum class PositionFormat : uint8_t { kNone, kFloat2, kFloat3, kFloat4, kHalf2, kHalf4 };

inline PositionFormat PositionFormatOf(uint32_t packed_decltype) {
  switch (packed_decltype) {
    case 0x002A23B9: return PositionFormat::kFloat3;  // k_32_32_32_FLOAT
    case 0x001A23A6: return PositionFormat::kFloat4;  // k_32_32_32_32_FLOAT
    case 0x001A2360: return PositionFormat::kHalf4;   // k_16_16_16_16_FLOAT
    case 0x002C23A5: return PositionFormat::kFloat2;  // k_32_32_FLOAT
    case 0x002C235F: return PositionFormat::kHalf2;   // k_16_16_FLOAT
    default: return PositionFormat::kNone;
  }
}

inline uint32_t PositionBytes(PositionFormat format) {
  switch (format) {
    case PositionFormat::kFloat2: return 8;
    case PositionFormat::kFloat3: return 12;
    case PositionFormat::kFloat4: return 16;
    case PositionFormat::kHalf2: return 4;
    case PositionFormat::kHalf4: return 8;
    default: return 0;
  }
}

inline const char* PositionFormatName(PositionFormat format) {
  switch (format) {
    case PositionFormat::kFloat2: return "float2";
    case PositionFormat::kFloat3: return "float3";
    case PositionFormat::kFloat4: return "float4";
    case PositionFormat::kHalf2: return "half2";
    case PositionFormat::kHalf4: return "half4";
    default: return "none";
  }
}

inline float HalfToFloat(uint16_t h) {
  const uint32_t sign = uint32_t(h & 0x8000u) << 16;
  const uint32_t exponent = (h >> 10) & 0x1Fu;
  const uint32_t mantissa = h & 0x3FFu;
  uint32_t bits;
  if (exponent == 0) {
    if (mantissa == 0) {
      bits = sign;
    } else {
      // A subnormal half: normalise it.
      uint32_t e = 113;
      uint32_t m = mantissa;
      while ((m & 0x400u) == 0) {
        m <<= 1;
        --e;
      }
      bits = sign | (e << 23) | ((m & 0x3FFu) << 13);
    }
  } else if (exponent == 31) {
    bits = sign | 0x7F800000u | (mantissa << 13);
  } else {
    bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
  }
  float value;
  std::memcpy(&value, &bits, 4);
  return value;
}

inline uint32_t LoadBe32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
inline uint16_t LoadBe16(const uint8_t* p) { return uint16_t((uint32_t(p[0]) << 8) | uint32_t(p[1])); }

// The position at `be` (the guest's raw bytes of one vertex's element) as three floats; a two-component format has z = 0.
inline void DecodePosition(const uint8_t* be, PositionFormat format, float out[3]) {
  const auto f32 = [&](uint32_t i) {
    const uint32_t bits = LoadBe32(be + 4 * i);
    float v;
    std::memcpy(&v, &bits, 4);
    return v;
  };
  const auto f16 = [&](uint32_t i) { return HalfToFloat(LoadBe16(be + 2 * i)); };
  out[0] = out[1] = out[2] = 0.0f;
  switch (format) {
    case PositionFormat::kFloat2:
      out[0] = f32(0);
      out[1] = f32(1);
      break;
    case PositionFormat::kFloat3:
    case PositionFormat::kFloat4:
      out[0] = f32(0);
      out[1] = f32(1);
      out[2] = f32(2);
      break;
    case PositionFormat::kHalf2:
      out[0] = f16(0);
      out[1] = f16(1);
      break;
    case PositionFormat::kHalf4:
      out[0] = f16(0);
      out[1] = f16(1);
      out[2] = f16(2);
      break;
    default:
      break;
  }
}

// xenos::Endian: the swap of raw index bytes (geometry.cpp ApplyEndian). `length` is a multiple of the unit.
inline constexpr uint32_t kEndianNone = 0;
inline constexpr uint32_t kEndian8in16 = 1;
inline constexpr uint32_t kEndian8in32 = 2;
inline constexpr uint32_t kEndian16in32 = 3;

inline void SwapEndian(uint8_t* dst, const uint8_t* src, uint32_t length, uint32_t endian) {
  switch (endian) {
    case kEndian8in16:
      for (uint32_t i = 0; i + 2 <= length; i += 2) {
        dst[i] = src[i + 1];
        dst[i + 1] = src[i];
      }
      break;
    case kEndian8in32:
      for (uint32_t i = 0; i + 4 <= length; i += 4) {
        dst[i] = src[i + 3];
        dst[i + 1] = src[i + 2];
        dst[i + 2] = src[i + 1];
        dst[i + 3] = src[i];
      }
      break;
    case kEndian16in32:
      for (uint32_t i = 0; i + 4 <= length; i += 4) {
        dst[i] = src[i + 2];
        dst[i + 1] = src[i + 3];
        dst[i + 2] = src[i];
        dst[i + 3] = src[i + 1];
      }
      break;
    default:
      std::memmove(dst, src, length);
      break;
  }
}

// `count` guest indices (16 or 32 bit) at `raw`, swapped by `endian`, as host numbers.
inline void DecodeIndices(const uint8_t* raw, uint32_t count, bool index32, uint32_t endian, std::vector<uint32_t>* out) {
  const uint32_t size = index32 ? 4u : 2u;
  std::vector<uint8_t> swapped(size_t(count) * size);
  SwapEndian(swapped.data(), raw, count * size, endian);
  out->resize(count);
  for (uint32_t i = 0; i < count; ++i) {
    if (index32) {
      uint32_t v;
      std::memcpy(&v, swapped.data() + size_t(i) * 4, 4);
      (*out)[i] = v;
    } else {
      uint16_t v;
      std::memcpy(&v, swapped.data() + size_t(i) * 2, 2);
      (*out)[i] = v;
    }
  }
}

// ---------------------------------------------------------------------------
// what a vertex shader reads: the walk over its microcode
// ---------------------------------------------------------------------------
// The Xenos microcode (rex/xenos/ucode.h, the layout is copied here so that this header stays free of the SDK): three dwords
// of control flow hold two 48-bit instructions; an exec instruction (opcodes 1-6, 13, 14) names a block of up to six instructions
// (address, count, a sequence of 2 bits an instruction whose low bit says fetch (1) or ALU (0)); an ALU instruction is three dwords:
//   dword 0: bits 26-31 scalar opcode
//   dword 1: bit 29 const_address_register_relative (1: a0, 0: the loop counter aL), bit 30 const_1_rel_abs, bit 31 const_0_rel_abs
//   dword 2: bits 0-7 src3_reg, 8-15 src2_reg, 16-23 src1_reg (a constant's FULL index, or a temporary register), bits 24-28 vector
//            opcode, bit 29 src3_sel, 30 src2_sel, 31 src1_sel (1: a temporary register, 0: a float constant)
// The vector opcode takes 0-3 operands (src1..src3), the scalar opcode 0, 1 (src3, a register or a constant) or 2 (the constant
// src3 and a temporary: mulsc, addsc, subsc); which of the three const addressing bits belongs to an operand follows the SDK's
// AluInstruction::src_const_is_addressed. The walk is the pack builder's (shaderpack/containers.cpp TextureFetchMask): control
// flow pairs up to the first exec block's address.
//
// A VS the same walk was run over on all 1,072 retail vertex shaders gives, for the car and character families: a palette VS (the
// rigid car parts) reads c204 and a0-relative registers 64, 65, 66 (the bone's three rows); a character (skinned) VS reads the same
// a0-relative rows and no c204; the tyres (xWheelRubber VS_ParallaxRubber2) and the wheel proxies (xTestRimBlur VS_drawCheap) read
// neither. (Some "Skinned" car VS, xBumpSpec VS_BumpSpecSkinned for one, read no palette either: gWorld alone.)

// The float constants a vertex shader reads.
struct ConstantReads {
  bool valid = false;             // at least one exec block was found in the microcode
  uint32_t instructions = 0;      // ALU instructions walked
  uint64_t absolute[4] = {};      // bit r of word r / 64: the register r is read at a fixed index
  bool relative_a0 = false;       // some constant is read relative to a0
  bool relative_loop = false;     // ... or to the loop counter
  bool palette_rows = false;      // a0-relative with a base of 64, 65 or 66: the rows of the bone palette
  bool Reads(uint32_t reg) const { return reg < kRegisterCount && ((absolute[reg >> 6] >> (reg & 63u)) & 1u) != 0; }
  bool bone_index() const { return Reads(kBoneIndexRegister); }
};

// The first and last base register of the palette's three rows read through a0 (gCarMtxBuffer(a0 + 0..2) with a0 = 3 bone).
inline constexpr uint32_t kPaletteFirstBase = kPaletteRegister;
inline constexpr uint32_t kPaletteLastBase = kPaletteRegister + kBoneRegisters - 1;

namespace ucode {
// Operands the vector opcodes 0-31 and the scalar opcodes 0-63 take (the SDK's kAluVectorOpcodeInfos / kAluScalarOpcodeInfos).
inline constexpr uint8_t kVectorOperands[32] = {
    2, 2, 2, 2, 2, 2, 2, 2,  // add mul max min seq sgt sge sne
    1, 1, 1,                 // frc trunc floor
    3, 3, 3, 3,              // mad cndeq cndge cndgt
    2, 2, 3, 2, 1,           // dp4 dp3 dp2add cube max4
    2, 2, 2, 2,              // setp_eq/ne/gt/ge_push
    2, 2, 2, 2,              // kill_eq/gt/ge/ne
    2, 2, 0, 0};             // dst maxa (30 and 31: none)
inline constexpr std::array<uint8_t, 64> kScalarOperands = [] {
  std::array<uint8_t, 64> table{};
  for (int i = 0; i <= 40; ++i) {
    table[size_t(i)] = 1;  // adds ... sqrt (the setp, kills and maxas forms included)
  }
  table[33] = 0;  // setp_clr
  for (int i = 42; i <= 47; ++i) {
    table[size_t(i)] = 2;  // mulsc, addsc, subsc: two opcodes each
  }
  table[48] = table[49] = 1;  // sin cos
  return table;           // 41 and retain_prev (50) and the unused 51-63: none
}();
}  // namespace ucode

inline bool IsExecOpcode(uint32_t op) { return (op >= 1 && op <= 6) || op == 13 || op == 14; }

// The constants the vertex shader microcode `ucode` (raw big-endian guest bytes) reads.
inline ConstantReads ScanConstantReads(const uint8_t* ucode, uint32_t size) {
  ConstantReads out;
  const uint32_t n = ucode ? size / 4 : 0;
  if (n < 3) {
    return out;
  }
  const auto dword = [&](uint32_t i) { return LoadBe32(ucode + size_t(i) * 4); };
  struct Block {
    uint32_t address, count, sequence;
  };
  std::vector<Block> blocks;
  uint32_t limit = n;  // the control flow ends where the first block's instructions start
  for (uint32_t i = 0; i + 2 < limit; i += 3) {
    const uint32_t d0 = dword(i), d1 = dword(i + 1), d2 = dword(i + 2);
    const uint32_t w0[2] = {d0, (d1 >> 16) | (d2 << 16)};
    const uint32_t w1[2] = {d1 & 0xFFFFu, d2 >> 16};
    for (int k = 0; k < 2; ++k) {
      if (!IsExecOpcode((w1[k] >> 12) & 0xFu)) {
        continue;
      }
      const uint32_t address = w0[k] & 0xFFFu;
      if (address != 0) {
        limit = std::min(limit, address * 3u);
      }
      blocks.push_back(Block{address, (w0[k] >> 12) & 0x7u, (w0[k] >> 16) & 0xFFFu});
    }
  }
  for (const Block& block : blocks) {
    out.valid = true;
    for (uint32_t k = 0; k < block.count; ++k) {
      if (((block.sequence >> (2 * k)) & 1u) != 0) {
        continue;  // a fetch
      }
      const uint32_t first = (block.address + k) * 3;
      if (first + 2 >= n) {
        continue;
      }
      const uint32_t a0 = dword(first), a1 = dword(first + 1), a2 = dword(first + 2);
      ++out.instructions;
      const uint32_t vector_op = (a2 >> 24) & 0x1Fu;
      const uint32_t scalar_op = (a0 >> 26) & 0x3Fu;
      const uint32_t reg[3] = {(a2 >> 16) & 0xFFu, (a2 >> 8) & 0xFFu, a2 & 0xFFu};
      const bool temp[3] = {((a2 >> 31) & 1u) != 0, ((a2 >> 30) & 1u) != 0, ((a2 >> 29) & 1u) != 0};
      const bool rel0 = ((a1 >> 31) & 1u) != 0;
      const bool rel1 = ((a1 >> 30) & 1u) != 0;
      const bool via_a0 = ((a1 >> 29) & 1u) != 0;
      // Whether operand i (0-2) addresses relatively: const_0_rel_abs unless an earlier operand is a constant too.
      const auto addressed = [&](int i) {
        if (i == 0) {
          return rel0;
        }
        if (i == 1) {
          return temp[0] ? rel0 : rel1;
        }
        return (temp[0] && temp[1]) ? rel0 : rel1;
      };
      const auto note = [&](int i, bool force_constant) {
        if (temp[i] && !force_constant) {
          return;
        }
        if (addressed(i)) {
          if (via_a0) {
            out.relative_a0 = true;
            out.palette_rows |= reg[i] >= kPaletteFirstBase && reg[i] <= kPaletteLastBase;
          } else {
            out.relative_loop = true;
          }
        } else {
          out.absolute[reg[i] >> 6] |= uint64_t(1) << (reg[i] & 63u);
        }
      };
      for (uint32_t i = 0; i < ucode::kVectorOperands[vector_op]; ++i) {
        note(int(i), false);
      }
      const uint32_t scalar_operands = ucode::kScalarOperands[scalar_op];
      if (scalar_operands == 1) {
        note(2, false);
      } else if (scalar_operands == 2) {
        note(2, true);  // mulsc / addsc / subsc: src3 is the constant whatever src3_sel says (it bears a register number then)
      }
    }
  }
  return out;
}

// The vertex shaders (runtime identity, the pack's keys) that read no palette, for a draw whose microcode cannot be read (an
// address that is no committed memory, or microcode that does not hash to the draw's identity). From the scan of the retail
// game's 1,072 vertex shaders (the names are the translator's group and entry point):
//   eddaf3e3986ae099  xWheelRubber VS_ParallaxRubber2   the tyres (draws of the car's own ID)
//   8b2189251306af48  xTestRimBlur VS_drawCheap         the wheel blur proxies (stencil-only)
//   4764a78e1ea7b71b  xBumpSpec VS_BumpSpecSkinned      \ car parts of a "skinned" material that read gWorld alone (the vertices
//   b9b899942e0fd120  xBumpSpecAlpha VS_BumpSpecSkinned |  were placed by a pass before) -- not the xBumpSpecSkinned VS_BumpSpecSkinned,
//   5887ef4cbf2630ce  xUnderbody VSSkinned               /  which blends bones (f3875986f1cffb77)
inline constexpr uint64_t kKnownWorldOnlyVs[] = {
    0xEDDAF3E3986AE099ull, 0x8B2189251306AF48ull, 0x4764A78E1EA7B71Bull, 0xB9B899942E0FD120ull, 0x5887EF4CBF2630CEull,
};

struct RecipeChoice {
  Recipe recipe = Recipe::kPalette;
  RecipeSource source = RecipeSource::kDefault;
};

// The recipe of a draw from what its vertex shader reads. With the microcode: a shader that reads the palette rows by a0 and the bone
// index c204 is a rigid part (the palette's bone c204.x, then gWorld), one that reads the rows and no bone index takes its bones per
// vertex (kSkinned), one that reads no rows reads gWorld alone. Without it: BLENDINDICES in the declaration says skinned, the table
// says gWorld alone, anything else is the car default (the palette).
inline RecipeChoice ChooseRecipe(const ConstantReads& reads, bool blend_indices, uint64_t vs_identity) {
  RecipeChoice choice;
  if (reads.valid) {
    choice.source = RecipeSource::kMicrocode;
    choice.recipe = !reads.palette_rows ? Recipe::kWorld : (reads.bone_index() ? Recipe::kPalette : Recipe::kSkinned);
    return choice;
  }
  if (blend_indices) {
    choice.recipe = Recipe::kSkinned;
    choice.source = RecipeSource::kDeclaration;
    return choice;
  }
  for (uint64_t known : kKnownWorldOnlyVs) {
    if (known == vs_identity) {
      choice.recipe = Recipe::kWorld;
      choice.source = RecipeSource::kTable;
      return choice;
    }
  }
  return choice;
}

// ---------------------------------------------------------------------------
// what a draw is: body, skinned part, wheel proxy
// ---------------------------------------------------------------------------

// The wheel blur proxies: per wheel one stencil-only draw (78 triangles, 80 vertices, a stride of 12) of the player's car with a
// stencil ID of its own (feature-parity/GHOSTING-DESIGN.md 9.2: "stencil-only proxies ... shader pair VS 8B2189251306AF48 / PS
// 7D4887A7CB75AF65"). They are markers for the wheels' motion blur, not geometry, and are not part of the car.
inline constexpr uint64_t kWheelProxyVs = 0x8B2189251306AF48ull;  // xTestRimBlur VS_drawCheap
inline constexpr uint64_t kWheelProxyPs = 0x7D4887A7CB75AF65ull;
// The car paint's pixel shader (xCarPaintCustomizable PS_CarPaint, shaderpack/variants/car_selfrefl.h kCarPaint).
inline constexpr uint64_t kCarPaintPs = 0x439B193A3F675533ull;

enum class ProxyWhy : uint8_t {
  kNo,        // an ordinary draw
  kShader,    // the wheel proxy's shader pair
  kNoColour,  // no colour target takes a write: the draw only marks stencil (or depth), it shows nothing
};

inline ProxyWhy WheelProxyOf(uint64_t vs_identity, uint64_t ps_identity, uint32_t colour_write_mask) {
  if (vs_identity == kWheelProxyVs || ps_identity == kWheelProxyPs) {
    return ProxyWhy::kShader;
  }
  return (colour_write_mask & 0xFu) == 0 ? ProxyWhy::kNoColour : ProxyWhy::kNo;
}

inline const char* ProxyWhyName(ProxyWhy why) {
  switch (why) {
    case ProxyWhy::kNo: return "no";
    case ProxyWhy::kShader: return "its shader pair";
    case ProxyWhy::kNoColour: return "colour mask 0";
  }
  return "?";
}

// A draw's part in a car: what the car is made of (body: rigid parts and the tyres), what hangs on it with bones of its own
// (skinned: the driver), and what is no geometry of it (proxy).
enum class Role : uint8_t { kBody, kSkinned, kProxy };

inline Role RoleOf(ProxyWhy proxy, Recipe recipe) {
  if (proxy != ProxyWhy::kNo) {
    return Role::kProxy;
  }
  return recipe == Recipe::kSkinned ? Role::kSkinned : Role::kBody;
}

inline const char* RoleName(Role role) {
  switch (role) {
    case Role::kBody: return "body";
    case Role::kSkinned: return "skinned";
    case Role::kProxy: return "wheel proxy";
  }
  return "?";
}

// ---------------------------------------------------------------------------
// skinned vertices
// ---------------------------------------------------------------------------
// The character vertex shaders (xCharacter*: the driver, which has the vehicle's stencil ID) take up to four bones a vertex:
// BLENDINDICES (4 bone numbers) and BLENDWEIGHT (4 weights), a 3x4 matrix of the palette for each (the rows c64 + 3 bone ... + 2, as the
// rigid parts' bone), the weights applied to the blended matrix, which takes the position to the "local" position gWorld takes to the world
// (xCharacter_normalmap VSSkinned_Wind: r5 / r10 / r8 are the blended rows, gWorldViewProj multiplies the result). The bone's row
// is picked by a0 = round(index * 255 * 3) in the shader, from constants in registers that differ per shader (c250 there): the
// numbers here take a bone as the byte of the vertex stream (CHECK in the run: the log counts the vertices whose bone is not in the
// palette and the weight sums).
//
// The formats the declarations give (format.cpp VertexFormat): k_8_8_8_8 normalised (decltype 0x00182886, which the host reads as BGRA
// unorm after the 4-byte swap: the guest bytes g0 g1 g2 g3 are x y z w = g1 g2 g3 g0), k_8_8_8_8 raw (0x001A2286, R8G8B8A8_UINT: x y z w = g3
// g2 g1 g0 as integers), four floats and four half floats.
enum class BlendFormat : uint8_t { kNone, kUnorm8, kUint8, kFloat4, kHalf4 };

inline BlendFormat BlendFormatOf(uint32_t packed_decltype) {
  switch (packed_decltype) {
    case 0x00182886: return BlendFormat::kUnorm8;
    case 0x001A2286: return BlendFormat::kUint8;
    case 0x001A23A6: return BlendFormat::kFloat4;
    case 0x001A2360: return BlendFormat::kHalf4;
    default: return BlendFormat::kNone;
  }
}

inline uint32_t BlendBytes(BlendFormat format) {
  switch (format) {
    case BlendFormat::kUnorm8:
    case BlendFormat::kUint8: return 4;
    case BlendFormat::kFloat4: return 16;
    case BlendFormat::kHalf4: return 8;
    default: return 0;
  }
}

// The four components of an element's guest bytes, as the shader gets them (x y z w): 0-1 for the unorm bytes, the byte for the raw ones.
inline void DecodeBlend4(const uint8_t* be, BlendFormat format, float out[4]) {
  out[0] = out[1] = out[2] = out[3] = 0.0f;
  switch (format) {
    case BlendFormat::kUnorm8:
      out[0] = float(be[1]) / 255.0f;
      out[1] = float(be[2]) / 255.0f;
      out[2] = float(be[3]) / 255.0f;
      out[3] = float(be[0]) / 255.0f;
      break;
    case BlendFormat::kUint8:
      out[0] = float(be[3]);
      out[1] = float(be[2]);
      out[2] = float(be[1]);
      out[3] = float(be[0]);
      break;
    case BlendFormat::kFloat4:
      for (uint32_t i = 0; i < 4; ++i) {
        const uint32_t bits = LoadBe32(be + 4 * i);
        std::memcpy(&out[i], &bits, 4);
      }
      break;
    case BlendFormat::kHalf4:
      for (uint32_t i = 0; i < 4; ++i) {
        out[i] = HalfToFloat(LoadBe16(be + 2 * i));
      }
      break;
    default:
      break;
  }
}

// One vertex's influences: the bone of each (out of the palette's range: kNoBone) and its weight.
inline constexpr uint32_t kNoBone = 0xFFFFFFFFu;
struct SkinInfluence {
  uint32_t bone[4] = {kNoBone, kNoBone, kNoBone, kNoBone};
  float weight[4] = {};
};

inline uint32_t BoneOf(float component, BlendFormat format) {
  const float scaled = format == BlendFormat::kUnorm8 ? component * 255.0f : component;
  if (!std::isfinite(scaled) || scaled < -0.5f || scaled > float(kPaletteBones) - 0.5f) {
    return kNoBone;
  }
  return uint32_t(scaled + 0.5f);
}

inline float WeightOf(float component, BlendFormat format) {
  return format == BlendFormat::kUint8 ? component / 255.0f : component;
}

inline SkinInfluence ReadInfluence(const uint8_t* indices, BlendFormat index_format, const uint8_t* weights, BlendFormat weight_format) {
  float i[4], w[4];
  DecodeBlend4(indices, index_format, i);
  DecodeBlend4(weights, weight_format, w);
  SkinInfluence out;
  for (int k = 0; k < 4; ++k) {
    out.bone[k] = BoneOf(i[k], index_format);
    out.weight[k] = std::isfinite(w[k]) ? WeightOf(w[k], weight_format) : 0.0f;
  }
  return out;
}

// The palette of one draw: bone b's three rows (c64 + 3 b ...), 4 floats each.
struct SkinPalette {
  float rows[kPaletteBones][kBoneRegisters][4];
};

inline void LoadPalette(const uint8_t* vs_bank, SkinPalette* out) {
  for (uint32_t b = 0; b < kPaletteBones; ++b) {
    for (uint32_t r = 0; r < kBoneRegisters; ++r) {
      for (uint32_t c = 0; c < 4; ++c) {
        out->rows[b][r][c] = BankFloat(vs_bank, kPaletteRegister + b * kBoneRegisters + r, c);
      }
    }
  }
}

// What the blending of a draw's vertices met.
struct SkinStats {
  uint32_t vertices = 0;
  uint32_t bad_bone = 0;    // vertices with an influence of weight > 0.001 whose bone is not in the palette
  uint32_t bad_weight = 0;  // vertices whose weights do not add up to 1 (within 2%)
  double worst_weight_sum_error = 0.0;
};

// The blended local position of vertex `p` (3 floats) with `skin`: the sum of weight * (bone rows applied to [p 1]).
inline void SkinBlend(const SkinPalette& palette, const SkinInfluence& skin, const float p[3], float out[3], SkinStats* stats) {
  double sum_weight = 0.0;
  double local[3] = {0.0, 0.0, 0.0};
  bool bad_bone = false;
  for (int k = 0; k < 4; ++k) {
    const double w = double(skin.weight[k]);
    sum_weight += w;
    if (skin.bone[k] == kNoBone) {
      bad_bone |= w > 0.001;
      continue;
    }
    if (w == 0.0) {
      continue;
    }
    const float(*rows)[4] = palette.rows[skin.bone[k]];
    for (int r = 0; r < 3; ++r) {
      local[r] += w * (double(rows[r][0]) * double(p[0]) + double(rows[r][1]) * double(p[1]) + double(rows[r][2]) * double(p[2]) +
                       double(rows[r][3]));
    }
  }
  out[0] = float(local[0]);
  out[1] = float(local[1]);
  out[2] = float(local[2]);
  if (stats) {
    ++stats->vertices;
    stats->bad_bone += bad_bone;
    const double error = std::fabs(sum_weight - 1.0);
    stats->bad_weight += error > 0.02;
    stats->worst_weight_sum_error = std::max(stats->worst_weight_sum_error, error);
  }
}

// ---------------------------------------------------------------------------
// the triangles of a draw
// ---------------------------------------------------------------------------

// A draw's triangles as vertex numbers into its stream, once expanded to a list (topology.h ConvertIndices / StripToList /
// GenerateIndices do that on the renderer's side): a triangle with a repeated vertex has no area (a strip's joins) and is
// counted and left out, one that names a vertex the stream does not hold is counted and left out.
struct TriangleSet {
  std::vector<uint32_t> tri;  // 3 vertex numbers a triangle
  uint32_t degenerate = 0;
  uint32_t out_of_range = 0;
  uint32_t first_vertex = 0xFFFFFFFFu;  // the smallest and largest vertex number the kept triangles name
  uint32_t last_vertex = 0;
  uint32_t triangles() const { return uint32_t(tri.size() / 3); }
  uint32_t span() const { return tri.empty() ? 0u : last_vertex - first_vertex + 1; }
};

// `count` list entries (whole triangles only), each plus `vertex_base` the vertex number; `available` vertices exist.
inline void CollectTriangles(const uint32_t* list, uint32_t count, int64_t vertex_base, uint32_t available, TriangleSet* out) {
  out->tri.reserve(out->tri.size() + size_t(count));
  for (uint32_t t = 0; t + 3 <= count; t += 3) {
    int64_t v[3];
    bool inside = true;
    for (int k = 0; k < 3; ++k) {
      v[k] = int64_t(list[t + k]) + vertex_base;
      inside &= v[k] >= 0 && v[k] < int64_t(available);
    }
    if (!inside) {
      ++out->out_of_range;
      continue;
    }
    if (v[0] == v[1] || v[1] == v[2] || v[0] == v[2]) {
      ++out->degenerate;
      continue;
    }
    for (int k = 0; k < 3; ++k) {
      const uint32_t n = uint32_t(v[k]);
      out->tri.push_back(n);
      out->first_vertex = std::min(out->first_vertex, n);
      out->last_vertex = std::max(out->last_vertex, n);
    }
  }
}

// ---------------------------------------------------------------------------
// the rasteriser
// ---------------------------------------------------------------------------

struct Vec4 {
  double x = 0.0, y = 0.0, z = 0.0, w = 0.0;
};

// The image a triangle is rasterised into: `width` x `height` pixels, the pixel (x, y) sampled at (x + sample, y + sample).
struct Image {
  uint32_t width = 0;
  uint32_t height = 0;
  double sample = 0.5;
};

// Where the history image's texel is read at: car_history_ps.hlsl takes the pixel at position.xy * divisor, which is the pixel at
// the lower right of a 2x2 block at divisor 2 (its centre is 3/4 of the way across the texel), and the pixel itself at 1.
inline double HistorySample(uint32_t divisor) {
  const uint32_t d = std::max(divisor, 1u);
  return (double(d / 2) + 0.5) / double(d);
}

inline Vec4 Project(const temporal::Mat4& view_projection_of_model, const double p[3]) {
  double c[4];
  TransformPoint(view_projection_of_model, p, c);
  return Vec4{c[0], c[1], c[2], c[3]};
}

// Clip-space w a vertex must have to be kept: the camera's near plane in w (w is the view distance).
inline constexpr double kNearW = 1e-3;

// The triangle clipped against w >= kNearW: 0 (all behind), 3 or 4 vertices (Sutherland-Hodgman, one plane).
inline int ClipNear(const Vec4 in[3], Vec4 out[4]) {
  int n = 0;
  for (int i = 0; i < 3; ++i) {
    const Vec4& a = in[i];
    const Vec4& b = in[(i + 1) % 3];
    const bool a_in = a.w >= kNearW;
    const bool b_in = b.w >= kNearW;
    if (a_in) {
      out[n++] = a;
    }
    if (a_in != b_in) {
      const double t = (kNearW - a.w) / (b.w - a.w);
      out[n++] = Vec4{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, kNearW};
    }
  }
  return n;
}

// The pixel position of a clip-space vertex (w > 0): ndc x to the left edge 0 and the right edge `width`, ndc y up to row 0 at the top.
inline void ToImage(const Vec4& v, const Image& im, double* x, double* y) {
  *x = (v.x / v.w * 0.5 + 0.5) * double(im.width);
  *y = (0.5 - v.y / v.w * 0.5) * double(im.height);
}

// One screen triangle (clip space, all w > 0): calls sink(x, y, distance) for every pixel whose sample point it covers (edges
// inclusive, so two triangles sharing an edge leave no gap), with the distance w interpolated perspective-correct (1/w is
// linear on the screen). Either winding.
template <typename Sink>
void RasterizeScreen(const Image& im, const Vec4& a, const Vec4& b, const Vec4& c, Sink&& sink) {
  double ax, ay, bx, by, cx, cy;
  ToImage(a, im, &ax, &ay);
  ToImage(b, im, &bx, &by);
  ToImage(c, im, &cx, &cy);
  const double area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
  if (!(std::fabs(area) > 1e-12) || !std::isfinite(area)) {
    return;
  }
  const double min_x = std::min({ax, bx, cx});
  const double max_x = std::max({ax, bx, cx});
  const double min_y = std::min({ay, by, cy});
  const double max_y = std::max({ay, by, cy});
  if (max_x < 0.0 || max_y < 0.0 || min_x > double(im.width) || min_y > double(im.height)) {
    return;
  }
  const int64_t x0 = std::max<int64_t>(0, int64_t(std::ceil(min_x - im.sample)));
  const int64_t x1 = std::min<int64_t>(int64_t(im.width) - 1, int64_t(std::floor(max_x - im.sample)));
  const int64_t y0 = std::max<int64_t>(0, int64_t(std::ceil(min_y - im.sample)));
  const int64_t y1 = std::min<int64_t>(int64_t(im.height) - 1, int64_t(std::floor(max_y - im.sample)));
  const double iwa = 1.0 / a.w, iwb = 1.0 / b.w, iwc = 1.0 / c.w;
  const double inv_area = 1.0 / area;
  constexpr double kEdge = -1e-9;
  for (int64_t y = y0; y <= y1; ++y) {
    const double py = double(y) + im.sample;
    for (int64_t x = x0; x <= x1; ++x) {
      const double px = double(x) + im.sample;
      const double la = ((cx - bx) * (py - by) - (cy - by) * (px - bx)) * inv_area;
      const double lb = ((ax - cx) * (py - cy) - (ay - cy) * (px - cx)) * inv_area;
      const double lc = ((bx - ax) * (py - ay) - (by - ay) * (px - ax)) * inv_area;
      if (la < kEdge || lb < kEdge || lc < kEdge) {
        continue;
      }
      const double inverse = la * iwa + lb * iwb + lc * iwc;
      if (inverse > 0.0) {
        sink(uint32_t(x), uint32_t(y), 1.0 / inverse);
      }
    }
  }
}

// A triangle in clip space: clipped at the near plane, then rasterised (a clipped triangle may leave a quad: two triangles).
template <typename Sink>
void RasterizeClip(const Image& im, const Vec4 clip[3], Sink&& sink) {
  Vec4 poly[4];
  const int n = ClipNear(clip, poly);
  for (int i = 1; i + 1 < n; ++i) {
    RasterizeScreen(im, poly[0], poly[i], poly[i + 1], sink);
  }
}

// What the triangle is relative to the image, for the "off the image" count: no vertex of it is in front of the camera and
// inside the image's rectangle.
inline bool OffImage(const Vec4 clip[3], const Image& im) {
  for (int i = 0; i < 3; ++i) {
    if (!(clip[i].w >= kNearW)) {
      continue;
    }
    double x, y;
    ToImage(clip[i], im, &x, &y);
    if (x >= 0.0 && x < double(im.width) && y >= 0.0 && y < double(im.height)) {
      return false;
    }
  }
  return true;
}
inline bool BehindCamera(const Vec4 clip[3]) { return !(clip[0].w >= kNearW) && !(clip[1].w >= kNearW) && !(clip[2].w >= kNearW); }

// The nearest distance of every pixel a triangle covers (infinity where none does).
struct DepthGrid {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<float> distance;

  void Reset(uint32_t w, uint32_t h) {
    width = w;
    height = h;
    distance.assign(size_t(w) * h, std::numeric_limits<float>::infinity());
  }
  void Splat(uint32_t x, uint32_t y, double w) {
    float& d = distance[size_t(y) * width + x];
    if (float(w) < d) {
      d = float(w);
    }
  }
  bool Covered(size_t i) const { return distance[i] < std::numeric_limits<float>::infinity(); }
};

// ---------------------------------------------------------------------------
// the statistics
// ---------------------------------------------------------------------------

// The q-th quantile (0 to 1) of `v` (reordered): 0 for none.
inline double Quantile(std::vector<float>* v, double q) {
  if (v->empty()) {
    return 0.0;
  }
  const size_t k = std::min(v->size() - 1, size_t(q * double(v->size() - 1) + 0.5));
  std::nth_element(v->begin(), v->begin() + std::ptrdiff_t(k), v->end());
  return double((*v)[k]);
}

// The rasterised triangles against an image of view distances (the car history image's alpha: > 0 on a vehicle's pixel, 0
// elsewhere): how much of the vehicle's pixels the triangles cover and how close their nearest distance is to it.
struct CheckStats {
  uint64_t pixels = 0;          // pixels of the image
  uint64_t vehicle = 0;         // ... whose distance is > 0
  uint64_t covered = 0;         // vehicle pixels with a triangle
  uint64_t raster_only = 0;     // pixels with a triangle and no vehicle (hidden behind the world, or off the car)
  uint64_t within_1pct = 0;     // covered pixels whose distances differ by at most 1%
  uint64_t within_2pct = 0;
  uint64_t triangle_nearer = 0;   // covered pixels where the triangle is nearer than the vehicle surface by more than the tolerance
  uint64_t triangle_farther = 0;  // ... farther
  double coverage = 0.0;        // covered / vehicle
  double mean_rel = 0.0, p50_rel = 0.0, p95_rel = 0.0, max_rel = 0.0;  // |triangle - image| / image over the covered pixels
  double mean_abs = 0.0, p95_abs = 0.0;                                 // the same in the image's units
};

inline CheckStats CompareToDistance(const std::vector<float>& image, const DepthGrid& grid, double tolerance) {
  CheckStats s;
  s.pixels = std::min<uint64_t>(image.size(), grid.distance.size());
  std::vector<float> rel, absolute;
  for (size_t i = 0; i < s.pixels; ++i) {
    const float reference = image[i];
    const bool vehicle = std::isfinite(reference) && reference > 0.0f;
    const bool covered = grid.Covered(i);
    if (vehicle) {
      ++s.vehicle;
    }
    if (covered && !vehicle) {
      ++s.raster_only;
    }
    if (!vehicle || !covered) {
      continue;
    }
    ++s.covered;
    const double d = double(grid.distance[i]);
    const double diff = d - double(reference);
    const double r = std::fabs(diff) / double(reference);
    rel.push_back(float(r));
    absolute.push_back(float(std::fabs(diff)));
    s.mean_rel += r;
    s.mean_abs += std::fabs(diff);
    s.max_rel = std::max(s.max_rel, r);
    s.within_1pct += r <= 0.01;
    s.within_2pct += r <= 0.02;
    s.triangle_nearer += diff < -tolerance * double(reference);
    s.triangle_farther += diff > tolerance * double(reference);
  }
  s.coverage = s.vehicle ? double(s.covered) / double(s.vehicle) : 0.0;
  if (s.covered) {
    s.mean_rel /= double(s.covered);
    s.mean_abs /= double(s.covered);
  }
  s.p50_rel = Quantile(&rel, 0.5);
  s.p95_rel = Quantile(&rel, 0.95);
  s.p95_abs = Quantile(&absolute, 0.95);
  return s;
}

// How a draw's triangles sit against the image, pixel by pixel of THEIR OWN coverage (no nearest-triangle test: a closed mesh
// has surfaces behind the visible one, which the image does not see). A model matrix that is right gives mostly `match` on the
// pixels where the draw is the visible surface and `behind` on the others, and next to no `front`; a wrong one gives all three
// at random.
struct FitCount {
  uint64_t samples = 0;  // triangle pixels on a vehicle's pixel
  uint64_t match = 0;    // distance within the tolerance of the image's
  uint64_t front = 0;    // nearer than the image's by more than the tolerance
  uint64_t behind = 0;   // farther
  uint64_t off = 0;      // triangle pixels on a pixel that is no vehicle's
  void Add(double distance, float reference, double tolerance) {
    if (!(reference > 0.0f)) {
      ++off;
      return;
    }
    ++samples;
    const double diff = distance - double(reference);
    const double limit = tolerance * double(reference);
    if (diff < -limit) {
      ++front;
    } else if (diff > limit) {
      ++behind;
    } else {
      ++match;
    }
  }
  void Merge(const FitCount& o) {
    samples += o.samples;
    match += o.match;
    front += o.front;
    behind += o.behind;
    off += o.off;
  }
  double Share(uint64_t part) const { return samples ? double(part) / double(samples) : 0.0; }
};

// ---------------------------------------------------------------------------
// which stencil ID is the player's car
// ---------------------------------------------------------------------------
// IDs are handed out per frame in drawing order (a car has no stable ID, feature-parity/GHOSTING-DESIGN.md 9.2), and the wheel proxies take
// IDs of their own, so "the ID with the most draws" is only a guess. What the frame knows of each ID (IdSummary) is judged here:
//   1. the ID the settings ask for, when it has any draw that is not a wheel proxy;
//   2. the ID whose gWorld is nearest to the player's vehicle in the game's own memory (the global dword_8288DCF8, the vehicle the local
//      player drives; vehicle + 0x14 the entity, entity + 0x90 its world matrix, perf/CAR-RT-SPIKE.md section 2 and
//      mc_engine/squash_stretch.cpp), when that is within kPlayerMatchMetres (the render thread reads the guest's state a frame or a few
//      after the draws: a car at 25 m/s moves 0.4 m a frame);
//   3. else the ID with a paint draw that is nearest the camera (the player's car is the one the camera follows), or with the most paint
//      draws when the camera is not known;
//   4. else the ID with the most body draws.
// An ID with no body draw (the wheel proxies' IDs, the driver alone) is never chosen by 2-4.
inline constexpr uint32_t kIdSlots = 24;  // the stencil IDs 0-23 (temporal::kVehicleIds)
inline constexpr double kPlayerMatchMetres = 5.0;

struct IdSummary {
  uint32_t body_draws = 0;     // draws that are the car's geometry (not skinned, not a wheel proxy)
  uint32_t skinned_draws = 0;
  uint32_t proxy_draws = 0;
  uint32_t paint_draws = 0;    // of the body draws, those drawn by the car paint's pixel shader
  double camera_distance = std::numeric_limits<double>::infinity();  // the nearest gWorld of its body draws to the camera
  double player_distance = std::numeric_limits<double>::infinity();  // ... to the player's vehicle
  uint32_t Draws() const { return body_draws + skinned_draws + proxy_draws; }
};

enum class PickReason : uint8_t {
  kNone,           // no ID has a body draw
  kRequested,      // mcla_gpu_car_dump_id
  kPlayerVehicle,  // the nearest to the player's vehicle in the game's memory
  kNearestPaint,   // an ID with paint draws, nearest the camera
  kMostPaint,      // ... the camera not known: the most paint draws
  kMostDraws,      // no paint draw anywhere: the most body draws
};

inline const char* PickReasonName(PickReason reason) {
  switch (reason) {
    case PickReason::kNone: return "no ID has a body draw";
    case PickReason::kRequested: return "asked for by mcla_gpu_car_dump_id";
    case PickReason::kPlayerVehicle: return "its gWorld is the nearest to the player's vehicle";
    case PickReason::kNearestPaint: return "it has paint draws and is the nearest to the camera";
    case PickReason::kMostPaint: return "it has the most paint draws (the camera is not known)";
    case PickReason::kMostDraws: return "it has the most body draws (no paint draw in the frame)";
  }
  return "?";
}

struct Pick {
  uint32_t id = 0;  // 0: none
  PickReason reason = PickReason::kNone;
};

inline Pick ChoosePlayerId(const IdSummary (&ids)[kIdSlots], int32_t requested, bool player_known) {
  Pick pick;
  if (requested > 0 && uint32_t(requested) < kIdSlots && ids[requested].body_draws + ids[requested].skinned_draws != 0) {
    pick.id = uint32_t(requested);
    pick.reason = PickReason::kRequested;
    return pick;
  }
  // 2: the player's vehicle.
  if (player_known) {
    double best = kPlayerMatchMetres;
    for (uint32_t id = 1; id < kIdSlots; ++id) {
      if (ids[id].body_draws != 0 && ids[id].player_distance <= best) {
        best = ids[id].player_distance;
        pick.id = id;
      }
    }
    if (pick.id != 0) {
      pick.reason = PickReason::kPlayerVehicle;
      return pick;
    }
  }
  // 3: the nearest ID with a paint draw.
  double nearest = std::numeric_limits<double>::infinity();
  uint32_t most_paint = 0;
  for (uint32_t id = 1; id < kIdSlots; ++id) {
    if (ids[id].body_draws == 0 || ids[id].paint_draws == 0) {
      continue;
    }
    if (ids[id].camera_distance < nearest) {
      nearest = ids[id].camera_distance;
      pick.id = id;
    }
    if (ids[id].paint_draws > ids[most_paint].paint_draws) {
      most_paint = id;
    }
  }
  if (pick.id != 0) {
    pick.reason = PickReason::kNearestPaint;
    return pick;
  }
  if (most_paint != 0) {
    pick.id = most_paint;
    pick.reason = PickReason::kMostPaint;
    return pick;
  }
  // 4: the most body draws.
  for (uint32_t id = 1; id < kIdSlots; ++id) {
    if (ids[id].body_draws > ids[pick.id].body_draws) {
      pick.id = id;
    }
  }
  pick.reason = pick.id != 0 ? PickReason::kMostDraws : PickReason::kNone;
  return pick;
}

}  // namespace mcla::gpu::car_dump
