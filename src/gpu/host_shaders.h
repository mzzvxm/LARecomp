#pragma once
// ===========================================================================
// MCLA GPU renderer -- host shader blobs
// ===========================================================================
// The renderer's own shaders (src/gpu/hlsl), compiled at build time by the dxc
// of thirdparty/dxc-bin into the format of this build's backend: DXIL for
// D3D12, SPIR-V for Vulkan (src/gpu/CMakeLists.txt larecomp_gpu_host_shader).
// Each generated header defines one byte array, g_<stem>_<dxil|spirv>; they
// have internal linkage, so this header is included by the one translation unit
// that creates the shaders of a subsystem.
// ===========================================================================

#include <cstddef>
#include <cstdint>

#include "backend.h"

#if defined(LARECOMP_GPU_D3D12)
#include "overlay_ps.hlsl.dxil.h"
#include "overlay_vs.hlsl.dxil.h"
#define MCLA_GPU_HOST_SHADER(stem) g_##stem##_dxil
#else
#include "overlay_ps.hlsl.spirv.h"
#include "overlay_vs.hlsl.spirv.h"
#define MCLA_GPU_HOST_SHADER(stem) g_##stem##_spirv
#endif

namespace mcla::gpu {

struct HostShaderBlob {
  const void* data;
  uint64_t size;
  const char* name;
};

#define MCLA_GPU_HOST_SHADER_BLOB(stem) \
  HostShaderBlob{MCLA_GPU_HOST_SHADER(stem), sizeof(MCLA_GPU_HOST_SHADER(stem)), #stem}

inline HostShaderBlob OverlayVertexShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(overlay_vs);
}

inline HostShaderBlob OverlayPixelShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(overlay_ps);
}

}  // namespace mcla::gpu
