#pragma once
// ===========================================================================
// MCLA Native Graphics Runtime — ALU constant banks
// ===========================================================================
// The guest keeps two 256-register float4 banks in the D3DDevice and flushes
// the dirty parts into the command buffer at draw time (sub_824238E0, called
// from every draw builder):
//
//   sub_824238E0(dev, mask, pm4_base, bank_ptr)
//     VS: mask = qword at dev+0,  pm4_base = 0x4000,  bank = dev+1920
//     PS: mask = qword at dev+8,  pm4_base = 0x4400,  bank = dev+6016
//
// Dirty mask semantics, read off the flush loop:
//   * the mask is 64 bits;
//   * `_R31 += cntlzd(mask) << 6` and the inner copy moves 4x16 bytes per
//     step, so ONE BIT COVERS 64 BYTES = 4 float4 registers;
//   * 64 bits x 4 registers = 256 registers = the whole bank;
//   * bits are scanned from the most significant end (cntlzd), i.e. bit 63
//     is register 0..3;
//   * runs of set bits are emitted as a single PM4 type-0 write.
//
// CRITICAL: the draw builder ZEROES both masks right after flushing them
// (`*(_QWORD *)a1 = 0`). A hook that runs after the original draw function
// therefore always reads 0 — the masks must be sampled BEFORE the call.
//
// HLSL side (verified across all 2333 translated shaders, no exceptions):
//   cbuffer VertexShaderConstants : register(b0, space4)
//   cbuffer PixelShaderConstants  : register(b1, space4)
//   cbuffer SharedConstants       : register(b2, space4)
// with fields declared at `packoffset(cN)` where N is the guest ALU register
// index directly (gWorld at c0, gWorldViewProj at c8, gViewInverse at c12;
// the highest offset seen is c230). The constant buffer is therefore the raw
// guest bank, register for register — it must not be repacked.
//
// Bank values are big-endian floats in guest memory and need dword swapping
// on the way to D3D12.
// ===========================================================================

#include <cstdint>

namespace mcla::native_gfx {

inline constexpr uint32_t kAluRegisterCount = 256;
inline constexpr uint32_t kAluRegisterBytes = 16;
inline constexpr uint32_t kAluBankBytes = kAluRegisterCount * kAluRegisterBytes;  // 4096
inline constexpr uint32_t kAluRegistersPerDirtyBit = 4;

inline constexpr uint32_t kDevVsConstantBankOffset = 1920;
inline constexpr uint32_t kDevPsConstantBankOffset = 6016;
inline constexpr uint32_t kDevVsConstantDirtyOffset = 0;
inline constexpr uint32_t kDevPsConstantDirtyOffset = 8;

// The shared constant buffer the translated shaders expect at b2: 32 float4
// of descriptor-index tables (c0..c31) followed by scalars at c32/c33.
inline constexpr uint32_t kSharedConstantsBytes = 34 * 16;
inline constexpr uint32_t kSharedBooleansByteOffset = 512;       // c32.x
inline constexpr uint32_t kSharedSwappedTexcoordsByteOffset = 516;  // c32.y
inline constexpr uint32_t kSharedHalfPixelOffsetByteOffset = 520;   // c32.z/.w
inline constexpr uint32_t kSharedAlphaThresholdByteOffset = 528;    // c33.x

// The dirty masks as they were BEFORE the guest draw consumed them.
struct ConstantDirtyMasks {
  uint64_t vs = 0;
  uint64_t ps = 0;
};

// Samples both masks. Must be called before the original draw function runs.
ConstantDirtyMasks ReadConstantDirtyMasks(const uint8_t* base, uint32_t dev);

// Register range a dirty bit covers. Bit numbering follows the flush loop:
// the scan starts from the most significant bit, so bit (63 - n) maps to
// registers [4n, 4n+4).
inline uint32_t DirtyBitFirstRegister(uint32_t bit_from_msb) {
  return bit_from_msb * kAluRegistersPerDirtyBit;
}

// Copies a bank out of guest memory into `dst` (kAluBankBytes), converting
// big-endian dwords to host order. Returns false if the bank is unreadable.
bool ReadConstantBank(const uint8_t* base, uint32_t bank_ea, void* dst);

}  // namespace mcla::native_gfx
