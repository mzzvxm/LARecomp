#pragma once
// ===========================================================================
// MCLA GPU renderer -- host shader blobs
// ===========================================================================
// The renderer's own shaders (src/gpu/hlsl), compiled at build time by the dxc
// of thirdparty/dxc-bin once per backend the build has: DXIL for D3D12, SPIR-V
// for Vulkan (src/gpu/CMakeLists.txt larecomp_gpu_host_shader). Each generated
// header defines one byte array, g_<stem>_<dxil|spirv>; they have internal
// linkage, so this header is included by the one translation unit that creates
// the shaders of a subsystem. The blob handed out is the active backend's
// (backend.h), so these are called after Device::Initialize.
// ===========================================================================

#include <cstddef>
#include <cstdint>

#include "backend.h"

#if defined(LARECOMP_GPU_HAS_D3D12)
#include "fsr_easu_ps.hlsl.dxil.h"
#include "fxaa_ps.hlsl.dxil.h"
#include "fsr_rcas_ps.hlsl.dxil.h"
#include "fullscreen_vs.hlsl.dxil.h"
#include "overlay_ps.hlsl.dxil.h"
#include "overlay_vs.hlsl.dxil.h"
#include "post_luma_ps.hlsl.dxil.h"
#include "present_ps.hlsl.dxil.h"
#include "present_stretch_ps.hlsl.dxil.h"
#include "reduce_ps.hlsl.dxil.h"
#include "resolve_color_ms_ps.hlsl.dxil.h"
#include "resolve_color_ps.hlsl.dxil.h"
#include "resolve_depth_ms_ps.hlsl.dxil.h"
#include "resolve_depth_ps.hlsl.dxil.h"
#include "resolve_memory_color_cs.hlsl.dxil.h"
#include "resolve_memory_depth_cs.hlsl.dxil.h"
#include "resolve_memory_depth_ms_cs.hlsl.dxil.h"
#include "resolve_memory_ps.hlsl.dxil.h"
#include "resolve_memory_vertex_cs.hlsl.dxil.h"
#include "smaa_blend_ps.hlsl.dxil.h"
#include "smaa_edges_ps.hlsl.dxil.h"
#include "smaa_weights_ps.hlsl.dxil.h"
#endif
#if defined(LARECOMP_GPU_HAS_VULKAN)
#include "fsr_easu_ps.hlsl.spirv.h"
#include "fxaa_ps.hlsl.spirv.h"
#include "fsr_rcas_ps.hlsl.spirv.h"
#include "fullscreen_vs.hlsl.spirv.h"
#include "overlay_ps.hlsl.spirv.h"
#include "overlay_vs.hlsl.spirv.h"
#include "post_luma_ps.hlsl.spirv.h"
#include "present_ps.hlsl.spirv.h"
#include "present_stretch_ps.hlsl.spirv.h"
#include "reduce_ps.hlsl.spirv.h"
#include "resolve_color_ms_ps.hlsl.spirv.h"
#include "resolve_color_ps.hlsl.spirv.h"
#include "resolve_depth_ms_ps.hlsl.spirv.h"
#include "resolve_depth_ps.hlsl.spirv.h"
#include "resolve_memory_color_cs.hlsl.spirv.h"
#include "resolve_memory_depth_cs.hlsl.spirv.h"
#include "resolve_memory_depth_ms_cs.hlsl.spirv.h"
#include "resolve_memory_ps.hlsl.spirv.h"
#include "resolve_memory_vertex_cs.hlsl.spirv.h"
#include "smaa_blend_ps.hlsl.spirv.h"
#include "smaa_edges_ps.hlsl.spirv.h"
#include "smaa_weights_ps.hlsl.spirv.h"
#endif

namespace mcla::gpu {

struct HostShaderBlob {
  const void* data;
  uint64_t size;
  const char* name;
};

#define MCLA_GPU_HOST_SHADER_DXIL(stem) \
  HostShaderBlob{g_##stem##_dxil, sizeof(g_##stem##_dxil), #stem}
#define MCLA_GPU_HOST_SHADER_SPIRV(stem) \
  HostShaderBlob{g_##stem##_spirv, sizeof(g_##stem##_spirv), #stem}
// The active backend's blob of `stem`.
#if defined(LARECOMP_GPU_HAS_D3D12) && defined(LARECOMP_GPU_HAS_VULKAN)
#define MCLA_GPU_HOST_SHADER_BLOB(stem)                                \
  (ActiveBackend() == Backend::kD3D12 ? MCLA_GPU_HOST_SHADER_DXIL(stem) \
                                      : MCLA_GPU_HOST_SHADER_SPIRV(stem))
#elif defined(LARECOMP_GPU_HAS_D3D12)
#define MCLA_GPU_HOST_SHADER_BLOB(stem) MCLA_GPU_HOST_SHADER_DXIL(stem)
#else
#define MCLA_GPU_HOST_SHADER_BLOB(stem) MCLA_GPU_HOST_SHADER_SPIRV(stem)
#endif

inline HostShaderBlob OverlayVertexShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(overlay_vs);
}

inline HostShaderBlob OverlayPixelShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(overlay_ps);
}

inline HostShaderBlob FullscreenVertexShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(fullscreen_vs);
}

inline HostShaderBlob PresentPixelShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(present_ps);
}

// FSR1 on the present (M6c.1): an EASU pass, the RCAS pass, and
// the present's bilinear stretch of an image the gamma table already went into.
inline HostShaderBlob FsrEasuPixelShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(fsr_easu_ps);
}

inline HostShaderBlob FsrRcasPixelShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(fsr_rcas_ps);
}

inline HostShaderBlob PresentStretchPixelShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(present_stretch_ps);
}

// The scene's anti-aliasing before the UI (M6c.2/M6c.3): the copy
// with the luma in alpha, FXAA, and SMAA's passes (1 edges, 2 weights, 3 blend).
inline HostShaderBlob PostLumaPixelShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(post_luma_ps);
}

inline HostShaderBlob FxaaPixelShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(fxaa_ps);
}

inline HostShaderBlob SmaaPixelShader(int pass) {
  if (pass == 1) {
    return MCLA_GPU_HOST_SHADER_BLOB(smaa_edges_ps);
  }
  return pass == 2 ? MCLA_GPU_HOST_SHADER_BLOB(smaa_weights_ps)
                   : MCLA_GPU_HOST_SHADER_BLOB(smaa_blend_ps);
}

// The resolve conversion (resolve_ps.hlsl) by source kind.
// A resolve product reduced to a reader at a smaller resolution scale
// (ResolveCache::ReducedSlot).
inline HostShaderBlob ReducePixelShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(reduce_ps);
}

inline HostShaderBlob ResolvePixelShader(bool depth, bool msaa) {
  if (depth) {
    return msaa ? MCLA_GPU_HOST_SHADER_BLOB(resolve_depth_ms_ps)
                : MCLA_GPU_HOST_SHADER_BLOB(resolve_depth_ps);
  }
  return msaa ? MCLA_GPU_HOST_SHADER_BLOB(resolve_color_ms_ps)
              : MCLA_GPU_HOST_SHADER_BLOB(resolve_color_ps);
}

// A resolve's bytes into the resolve memory (resolve_memory_cs.hlsl): colour
// from the product, depth from the EDRAM target (multisampled or not).
inline HostShaderBlob ResolveMemoryEncodeShader(bool depth, bool msaa) {
  if (depth) {
    return msaa ? MCLA_GPU_HOST_SHADER_BLOB(resolve_memory_depth_ms_cs)
                : MCLA_GPU_HOST_SHADER_BLOB(resolve_memory_depth_cs);
  }
  return MCLA_GPU_HOST_SHADER_BLOB(resolve_memory_color_cs);
}

// A texture level decoded from the resolve memory (resolve_memory_ps.hlsl).
inline HostShaderBlob ResolveMemoryDecodeShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(resolve_memory_ps);
}

// A vertex stream built from the resolve memory (resolve_memory_vertex_cs.hlsl,).
inline HostShaderBlob ResolveMemoryVertexShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(resolve_memory_vertex_cs);
}

}  // namespace mcla::gpu
