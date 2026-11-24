#pragma once
// ===========================================================================
// LARecomp shader pack builder -- the shader feature flags of a DXIL container
// ===========================================================================
// A DXIL blob is a DXBC container: "DXBC", a 16 byte digest, the version, the size, the part count, the part offsets, and each part is
// a four character code, a size and its bytes. dxc writes the part SFI0 (D3D11_SHADER_FEATURE_INFO) with one u64: the features the shader
// needs of the GPU. The D3D12 runtime reads it when a pipeline is made and refuses a shader that needs what the device does not have; its
// debug layer says which ("Shader uses double precision float ops which are not supported on the current device").
//
// The one used here is Doubles (bit 0). The Intel iGPUs (UHD, Xe) have no double precision on D3D12: a pixel shader with it fails
// CreateGraphicsPipelineState there and nowhere else (an RTX or a Radeon accepts it, which is why it went unseen). A text edit gets one by
// accident: a ternary of bare HLSL literals (`c ? 4.0 : 3.0`) is a double that dxc may keep as `select double`, then `fptrunc`
// (the car self-reflection variant of 2026-11: 27 precached pipelines failed on the laptop's UHD). A literal takes an f suffix (4.0f) to stay a float.
//
// Pure: bytes in, no I/O, nothing but the standard library.
// ===========================================================================

#include <cstdint>
#include <cstring>
#include <span>

namespace larecomp::shaderpack {

// D3D11_SHADER_FEATURE_INFO bits (d3d12shader.h / DxilConstants.h ShaderFeatureInfo_*): the ones this code reads.
inline constexpr uint64_t kDxilFeatureDoubles = 1ull << 0;  // double precision float operations

// The u64 of the container's SFI0 part. False when `dxil` is not a DXBC container, is cut short, or has no such part (*flags is then 0).
inline bool ReadDxilFeatureFlags(std::span<const uint8_t> dxil, uint64_t* flags) {
  *flags = 0;
  constexpr size_t kHeaderBytes = 32;  // "DXBC", digest 16, version 4, size 4, part count 4
  if (dxil.size() < kHeaderBytes || std::memcmp(dxil.data(), "DXBC", 4) != 0) {
    return false;
  }
  const auto u32 = [&](size_t at) {
    uint32_t value = 0;
    std::memcpy(&value, dxil.data() + at, 4);  // the container is little endian, as the host
    return value;
  };
  const size_t parts = u32(28);
  if (parts > (dxil.size() - kHeaderBytes) / 4) {
    return false;
  }
  for (size_t i = 0; i < parts; ++i) {
    const size_t at = u32(kHeaderBytes + 4 * i);
    if (at > dxil.size() || dxil.size() - at < 8) {
      return false;
    }
    const size_t size = u32(at + 4);
    if (std::memcmp(dxil.data() + at, "SFI0", 4) == 0) {
      if (size < 8 || dxil.size() - (at + 8) < 8) {
        return false;
      }
      std::memcpy(flags, dxil.data() + at + 8, 8);
      return true;
    }
  }
  return false;
}

// True when the shader's feature flags have Doubles.
inline bool DxilUsesDoublePrecision(std::span<const uint8_t> dxil) {
  uint64_t flags = 0;
  return ReadDxilFeatureFlags(dxil, &flags) && (flags & kDxilFeatureDoubles) != 0;
}

}  // namespace larecomp::shaderpack
