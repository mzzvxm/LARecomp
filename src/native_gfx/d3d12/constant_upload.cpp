#ifndef REXGLUE_HAS_XEO3_TARGET
// MCLA Native Graphics Runtime — constant upload.
// See constant_upload.h.

#include "constant_upload.h"

#include <cstring>

#include "../guest/guest_constants.h"
#include "context.h"

namespace mcla::native_gfx {

bool UploadConstants(D3D12Context& context, const void* vs_bank, const void* ps_bank,
                     const SharedConstantValues& shared, ConstantBindings& out) {
  out = ConstantBindings{};
  if (!vs_bank) {
    return false;
  }

  // Constant buffer views require 256-byte alignment.
  constexpr uint64_t kCbvAlignment = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;

  D3D12Context::UploadAlloc alloc;
  if (!context.AllocateUpload(kAluBankBytes, kCbvAlignment, alloc)) {
    return false;
  }
  std::memcpy(alloc.cpu, vs_bank, kAluBankBytes);
  out.vs = alloc.gpu;

  if (ps_bank) {
    if (!context.AllocateUpload(kAluBankBytes, kCbvAlignment, alloc)) {
      return false;
    }
    std::memcpy(alloc.cpu, ps_bank, kAluBankBytes);
    out.ps = alloc.gpu;
  }

  if (!context.AllocateUpload(kSharedConstantsBytes, kCbvAlignment, alloc)) {
    return false;
  }
  auto* bytes = static_cast<uint8_t*>(alloc.cpu);
  std::memset(bytes, 0, kSharedConstantsBytes);
  std::memcpy(bytes + kSharedBooleansByteOffset, &shared.booleans, 4);
  std::memcpy(bytes + kSharedSwappedTexcoordsByteOffset, &shared.swapped_texcoords, 4);
  std::memcpy(bytes + kSharedHalfPixelOffsetByteOffset, shared.half_pixel_offset, 8);
  std::memcpy(bytes + kSharedAlphaThresholdByteOffset, &shared.alpha_threshold, 4);
  out.shared = alloc.gpu;
  return true;
}

}  // namespace mcla::native_gfx

#endif // REXGLUE_HAS_XEO3_TARGET
