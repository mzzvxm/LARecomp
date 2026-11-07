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
#include "ao_apply_ms_ps.hlsl.dxil.h"
#include "ao_apply_ps.hlsl.dxil.h"
#include "ao_denoise_ps.hlsl.dxil.h"
#include "ao_depth_ms_ps.hlsl.dxil.h"
#include "ao_depth_ps.hlsl.dxil.h"
#include "ao_main_ps.hlsl.dxil.h"
#include "ao_mip_ps.hlsl.dxil.h"
#include "ao_temporal_ms_ps.hlsl.dxil.h"
#include "ao_temporal_ps.hlsl.dxil.h"
#include "fg_compose_ps.hlsl.dxil.h"
#include "dlssg_ui_hdr_ps.hlsl.dxil.h"
#include "dlssg_ui_ps.hlsl.dxil.h"
#include "fg_copy_ps.hlsl.dxil.h"
#include "fg_prepare_ps.hlsl.dxil.h"
#include "fsr_easu_ps.hlsl.dxil.h"
#include "fxaa_ps.hlsl.dxil.h"
#include "fsr_rcas_ps.hlsl.dxil.h"
#include "fsr_rcas_hdr_ps.hlsl.dxil.h"
#include "fullscreen_vs.hlsl.dxil.h"
#include "hdr_compose_ps.hlsl.dxil.h"
#include "layers_resolve_ps.hlsl.dxil.h"
#include "layers_restore_ps.hlsl.dxil.h"
#include "overlay_hdr_ps.hlsl.dxil.h"
#include "overlay_ps.hlsl.dxil.h"
#include "overlay_vs.hlsl.dxil.h"
#include "post_luma_ps.hlsl.dxil.h"
#include "present_hdr_ps.hlsl.dxil.h"
#include "present_ps.hlsl.dxil.h"
#include "present_stretch_hdr_ps.hlsl.dxil.h"
#include "present_stretch_ps.hlsl.dxil.h"
#include "reduce_ps.hlsl.dxil.h"
#include "resolve_color_ms_ps.hlsl.dxil.h"
#include "resolve_color_ps.hlsl.dxil.h"
#include "resolve_depth_fused_frac_ps.hlsl.dxil.h"
#include "resolve_depth_fused_ps.hlsl.dxil.h"
#include "resolve_depth_ms_fused_frac_ps.hlsl.dxil.h"
#include "resolve_depth_ms_fused_ps.hlsl.dxil.h"
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
#include "temporal_copy_ps.hlsl.dxil.h"
#include "temporal_mv_ps.hlsl.dxil.h"
#endif
#if defined(LARECOMP_GPU_HAS_VULKAN)
#include "ao_apply_ms_ps.hlsl.spirv.h"
#include "ao_apply_ps.hlsl.spirv.h"
#include "ao_denoise_ps.hlsl.spirv.h"
#include "ao_depth_ms_ps.hlsl.spirv.h"
#include "ao_depth_ps.hlsl.spirv.h"
#include "ao_main_ps.hlsl.spirv.h"
#include "ao_mip_ps.hlsl.spirv.h"
#include "ao_temporal_ms_ps.hlsl.spirv.h"
#include "ao_temporal_ps.hlsl.spirv.h"
#include "fg_compose_ps.hlsl.spirv.h"
#include "dlssg_ui_hdr_ps.hlsl.spirv.h"
#include "dlssg_ui_ps.hlsl.spirv.h"
#include "fg_copy_ps.hlsl.spirv.h"
#include "fg_prepare_ps.hlsl.spirv.h"
#include "fsr_easu_ps.hlsl.spirv.h"
#include "fxaa_ps.hlsl.spirv.h"
#include "fsr_rcas_ps.hlsl.spirv.h"
#include "fsr_rcas_hdr_ps.hlsl.spirv.h"
#include "fullscreen_vs.hlsl.spirv.h"
#include "hdr_compose_ps.hlsl.spirv.h"
#include "layers_resolve_ps.hlsl.spirv.h"
#include "layers_restore_ps.hlsl.spirv.h"
#include "overlay_hdr_ps.hlsl.spirv.h"
#include "overlay_ps.hlsl.spirv.h"
#include "overlay_vs.hlsl.spirv.h"
#include "post_luma_ps.hlsl.spirv.h"
#include "present_hdr_ps.hlsl.spirv.h"
#include "present_ps.hlsl.spirv.h"
#include "present_stretch_hdr_ps.hlsl.spirv.h"
#include "present_stretch_ps.hlsl.spirv.h"
#include "reduce_ps.hlsl.spirv.h"
#include "resolve_color_ms_ps.hlsl.spirv.h"
#include "resolve_color_ps.hlsl.spirv.h"
#include "resolve_depth_fused_frac_ps.hlsl.spirv.h"
#include "resolve_depth_fused_ps.hlsl.spirv.h"
#include "resolve_depth_ms_fused_frac_ps.hlsl.spirv.h"
#include "resolve_depth_ms_fused_ps.hlsl.spirv.h"
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
#include "temporal_copy_ps.hlsl.spirv.h"
#include "temporal_mv_ps.hlsl.spirv.h"
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

// HDR10 output (hdr.h): the overlay, the present, its stretch and FSR1's RCAS as they are with
// hlsl/hdr_output.hlsli's encoding as their last step, for an HDR10 back buffer.
inline HostShaderBlob OverlayHdrPixelShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(overlay_hdr_ps);
}

inline HostShaderBlob PresentHdrPixelShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(present_hdr_ps);
}

inline HostShaderBlob PresentStretchHdrPixelShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(present_stretch_hdr_ps);
}

inline HostShaderBlob FsrRcasHdrPixelShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(fsr_rcas_hdr_ps);
}

// The UI layer split (frame_layers.h, hdr_scene.h): the underlay drawn back under the UI into the
// guest's target, and the HDR present's compose of the underlay and the UI layer.
inline HostShaderBlob LayersRestorePixelShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(layers_restore_ps);
}

// The same layers' copy out of a multisampled target: the mean of its samples.
inline HostShaderBlob LayersResolvePixelShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(layers_resolve_ps);
}

inline HostShaderBlob HdrComposePixelShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(hdr_compose_ps);
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

// The depth conversion that also writes the resolve memory's dwords of the same
// resolve (resolve_ps.hlsl RESOLVE_FUSED), by multisampled source or not, and for a
// whole resolution scale or a fractional one.
inline HostShaderBlob ResolveFusedPixelShader(bool msaa, bool fractional) {
  if (fractional) {
    return msaa ? MCLA_GPU_HOST_SHADER_BLOB(resolve_depth_ms_fused_frac_ps)
                : MCLA_GPU_HOST_SHADER_BLOB(resolve_depth_fused_frac_ps);
  }
  return msaa ? MCLA_GPU_HOST_SHADER_BLOB(resolve_depth_ms_fused_ps)
              : MCLA_GPU_HOST_SHADER_BLOB(resolve_depth_fused_ps);
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

// The temporal system's passes (temporal_mv.h): the motion vectors and the copy of the depth it keeps.
inline HostShaderBlob TemporalMotionPixelShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(temporal_mv_ps);
}

inline HostShaderBlob TemporalCopyPixelShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(temporal_copy_ps);
}

// Frame generation (frame_generation.h): the scene prepared for the interpolation, the generated
// frame composed with the UI layer, and the pacer's copy of a present image into the back buffer.
inline HostShaderBlob FrameGenerationPreparePixelShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(fg_prepare_ps);
}

inline HostShaderBlob FrameGenerationComposePixelShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(fg_compose_ps);
}

inline HostShaderBlob FrameGenerationCopyPixelShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(fg_copy_ps);
}

// DLSS frame generation (dlss_frame_generation.h): the UI layer in the back buffer's terms, for an SDR
// and for an HDR10 back buffer.
inline HostShaderBlob DlssFrameGenerationUiPixelShader(bool hdr) {
  return hdr ? MCLA_GPU_HOST_SHADER_BLOB(dlssg_ui_hdr_ps) : MCLA_GPU_HOST_SHADER_BLOB(dlssg_ui_ps);
}

// The ambient occlusion's passes (ao.h, hlsl/ao_*_ps.hlsl), numbered as AmbientOcclusion::Pass: 0
// the linear depth, 1 the same from a multisampled depth plane, 2 a coarser level, 3 the
// visibility, 4 the denoise, 5 the apply, 6 the apply over a multisampled depth plane, 7 the
// accumulation over frames, 8 the same over a multisampled depth target.
inline HostShaderBlob AmbientOcclusionPixelShader(uint32_t pass) {
  switch (pass) {
    case 0: return MCLA_GPU_HOST_SHADER_BLOB(ao_depth_ps);
    case 1: return MCLA_GPU_HOST_SHADER_BLOB(ao_depth_ms_ps);
    case 2: return MCLA_GPU_HOST_SHADER_BLOB(ao_mip_ps);
    case 3: return MCLA_GPU_HOST_SHADER_BLOB(ao_main_ps);
    case 4: return MCLA_GPU_HOST_SHADER_BLOB(ao_denoise_ps);
    case 5: return MCLA_GPU_HOST_SHADER_BLOB(ao_apply_ps);
    case 6: return MCLA_GPU_HOST_SHADER_BLOB(ao_apply_ms_ps);
    case 7: return MCLA_GPU_HOST_SHADER_BLOB(ao_temporal_ps);
    default: return MCLA_GPU_HOST_SHADER_BLOB(ao_temporal_ms_ps);
  }
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
