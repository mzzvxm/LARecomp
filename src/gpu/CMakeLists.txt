# The Plume renderer, src/gpu.
#
# larecomp_gpu is an OBJECT library so the renderer compiles with its own
# include order and definitions: plume's headers (its Vulkan-Headers and VMA)
# ahead of rex::runtime's, which arrive as system includes, and
# LARECOMP_GPU_HAS_<backend> visible to these files only. larecomp links the
# objects. Expects the caller to have added thirdparty/plume and
# thirdparty/dxc-bin and to set LARECOMP_GPU_BACKENDS (D3D12 and/or VULKAN; the
# one that runs is picked at startup).

add_library(larecomp_gpu OBJECT
    ab.cpp
    ao.cpp
    backend.cpp
    buffer_heap.cpp
    capture.cpp
    constants.cpp
    cycles.cpp
    descriptors.cpp
    device.cpp
    draw_census.cpp
    draw_state.cpp
    dlss_frame_generation.cpp
    edram.cpp
    flicker_probe.cpp
    format.cpp
    frame_generation.cpp
    frame_layers.cpp
    frame_trace.cpp
    frame.cpp
    geometry.cpp
    host_passes.cpp
    host_precache.cpp
    guest_capture.cpp
    guest_memory.cpp
    hdr.cpp
    hdr_scene.cpp
    hdr_tonemap.cpp
    input_layout.cpp
    latency.cpp
    latency_streamline.cpp
    layout.cpp
    memory_census.cpp
    msaa_level.cpp
    native_window.cpp
    occlusion.cpp
    overlay.cpp
    pass_timing.cpp
    perf_markers.cpp
    pipeline_prebuild.cpp
    pipelines.cpp
    post.cpp
    present.cpp
    present_schedule.cpp
    process_memory.cpp
    region_index.cpp
    render_scale.cpp
    render_thread.cpp
    renderer.cpp
    resolve.cpp
    resolve_memory.cpp
    samplers.cpp
    scene.cpp
    shaders.cpp
    stream.cpp
    swap_chain.cpp
    temporal.cpp
    temporal_backend.cpp
    temporal_camera.cpp
    temporal_mv.cpp
    texture_decode.cpp
    texture_decode_pool.cpp
    texture_mods.cpp
    texture_names.cpp
    textures.cpp
    thread_load.cpp
    timing.cpp
    topology.cpp
    upload.cpp
    upscaler_masks.cpp
    validation.cpp
    vehicle_motion.cpp
    viewport.cpp
    vram_budget.cpp
    watch.cpp
)
# The only translation units that see a backend's own objects (backend.h).
if("D3D12" IN_LIST LARECOMP_GPU_BACKENDS)
    target_sources(larecomp_gpu PRIVATE backend_d3d12.cpp)
endif()
if("VULKAN" IN_LIST LARECOMP_GPU_BACKENDS)
    target_sources(larecomp_gpu PRIVATE backend_vulkan.cpp)
endif()

# The latency reducers' vendor halves (latency.h), beside the backend files they sit with.
if("D3D12" IN_LIST LARECOMP_GPU_BACKENDS)
    target_sources(larecomp_gpu PRIVATE latency_d3d12.cpp)
endif()
if("VULKAN" IN_LIST LARECOMP_GPU_BACKENDS)
    target_sources(larecomp_gpu PRIVATE latency_vulkan.cpp)
endif()

# The temporal upscaler's FidelityFX backend (temporal_ffx.h): only when the top-level CMake found
# the SDK (LARECOMP_FFX_SDK_DIR); LARECOMP_FFX_KIT is its Kits/FidelityFX folder. Without it
# temporal_backend.cpp makes no backend and none of this is compiled. The D3D12 and Vulkan headers
# of AMD's API cannot share a translation unit, so each API has its own small file.
if(LARECOMP_FFX_KIT)
    target_sources(larecomp_gpu PRIVATE temporal_ffx.cpp)
    if("D3D12" IN_LIST LARECOMP_GPU_BACKENDS)
        target_sources(larecomp_gpu PRIVATE temporal_ffx_dx12.cpp)
    endif()
    if("VULKAN" IN_LIST LARECOMP_GPU_BACKENDS)
        target_sources(larecomp_gpu PRIVATE temporal_ffx_vk.cpp)
    endif()
    target_include_directories(larecomp_gpu PRIVATE
        "${LARECOMP_FFX_KIT}/api/include"
        "${LARECOMP_FFX_KIT}/upscalers/include")
    target_compile_definitions(larecomp_gpu PRIVATE LARECOMP_HAS_FFX=1)
    # Frame generation (frame_generation.h): the same runtime's frame generation effect. Its
    # headers include each other and the api folder by relative path, so only its folder is added.
    if(LARECOMP_FFX_HAS_FRAMEGENERATION)
        target_sources(larecomp_gpu PRIVATE frame_generation_ffx.cpp)
        target_include_directories(larecomp_gpu PRIVATE "${LARECOMP_FFX_KIT}/framegeneration/include")
        target_compile_definitions(larecomp_gpu PRIVATE LARECOMP_HAS_FFX_FG=1)
    endif()
endif()

# The temporal upscaler's DLSS backend (temporal_dlss.h): only when the top-level CMake found
# Streamline's headers (LARECOMP_STREAMLINE_DIR, fetched by cmake/FetchStreamline.cmake);
# LARECOMP_STREAMLINE_KIT is that folder. Without it temporal_backend.cpp makes no DLSS backend and
# none of this is compiled. Streamline's headers are only read (types and constants): nothing is
# linked, the DLL is opened at run time.
if(LARECOMP_STREAMLINE_KIT)
    target_sources(larecomp_gpu PRIVATE temporal_dlss.cpp)
    target_include_directories(larecomp_gpu SYSTEM PRIVATE "${LARECOMP_STREAMLINE_KIT}/include")
    target_compile_definitions(larecomp_gpu PRIVATE LARECOMP_HAS_DLSS=1)
    # DLSS Frame Generation and Reflex through Streamline (the Streamline-typed half is in
    # temporal_dlss.cpp; dlss_frame_generation.cpp below is Streamline-free and always built).
    if(LARECOMP_STREAMLINE_HAS_FRAMEGEN)
        target_compile_definitions(larecomp_gpu PRIVATE LARECOMP_HAS_DLSS_G=1)
    endif()
endif()
# Plume's creation hooks for Streamline on Vulkan and the usage flags it keeps of an image
# (Plume's larecomp branch: PR #10 and PR #8, both in the pin): a Plume without them leaves DLSS out
# of the Vulkan backend (D3D12 needs neither).
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../thirdparty/plume/plume_vulkan.h")
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../../thirdparty/plume/plume_vulkan.h" _larecomp_plume_vulkan_header)
    if(_larecomp_plume_vulkan_header MATCHES "struct VulkanInterfaceHooks" AND
       _larecomp_plume_vulkan_header MATCHES "imageUsage")
        target_compile_definitions(larecomp_gpu PRIVATE LARECOMP_PLUME_STREAMLINE=1)
    endif()
    unset(_larecomp_plume_vulkan_header)
endif()
# Plume's hook that wraps the D3D12 swap chain at a present (upgradeSwapChainAtPresent, Plume's
# larecomp branch, PR #10): Streamline's wrapper can only go in once Streamline has started, long after
# the swap chain exists. A Plume without it still runs DLSS on D3D12, but Streamline does not see
# the presents (temporal_dlss.h).
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../thirdparty/plume/plume_d3d12.h")
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../../thirdparty/plume/plume_d3d12.h" _larecomp_plume_d3d12_header)
    if(_larecomp_plume_d3d12_header MATCHES "upgradeSwapChainAtPresent")
        target_compile_definitions(larecomp_gpu PRIVATE LARECOMP_PLUME_D3D12_PRESENT_HOOK=1)
    endif()
    unset(_larecomp_plume_d3d12_header)
endif()

set(LARECOMP_GPU_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
# src/ too: the guest decoders the renderer reuses from the legacy runtime are
# included as "native_gfx/...", which keeps every such dependency visible by
# its path. Their definitions come from larecomp itself at link.
# thirdparty/ too: post.cpp uploads SMAA's lookup textures from the vendored
# headers, included as "smaa/...".
target_include_directories(larecomp_gpu PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/.."
    "${CMAKE_CURRENT_SOURCE_DIR}/../../thirdparty"
    "${LARECOMP_GPU_SHADER_DIR}"
)
# plume first: see the note at the top.
target_link_libraries(larecomp_gpu PRIVATE plume rex::runtime rex::renderdoc)
foreach(_backend IN LISTS LARECOMP_GPU_BACKENDS)
    target_compile_definitions(larecomp_gpu PRIVATE LARECOMP_GPU_HAS_${_backend}=1)
endforeach()
# The same flags the SDK applies to its own object library (larecomp_recomp).
rexglue_apply_target_settings(larecomp_gpu)

# ---------------------------------------------------------------------------
# Host shaders: hlsl/<stem>.hlsl -> shaders/<stem>.hlsl.<dxil|spirv>.h with
# g_<stem>_<dxil|spirv>, once per backend the build has. Rule as in reeot
# cmake/host_shaders.cmake (BSD-3-Clause): dxc-bin's DIRECTX_DXC_TOOL is a
# plain path on Windows and "VAR=value;<dxc>" elsewhere; the VAR=value parts go
# through `cmake -E env` so the command works under every generator.
# ---------------------------------------------------------------------------
if(NOT DIRECTX_DXC_TOOL)
    message(FATAL_ERROR "larecomp_gpu: DIRECTX_DXC_TOOL is not set (thirdparty/dxc-bin not added?)")
endif()
set(LARECOMP_GPU_DXC "")
foreach(_part IN LISTS DIRECTX_DXC_TOOL)
    if(_part MATCHES "^[A-Za-z_][A-Za-z0-9_]*=")
        list(APPEND LARECOMP_GPU_DXC "${CMAKE_COMMAND}" -E env "${_part}")
    else()
        list(APPEND LARECOMP_GPU_DXC "${_part}")
    endif()
endforeach()

set(LARECOMP_GPU_HLSL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/hlsl")
file(GLOB LARECOMP_GPU_HLSL_INCLUDES CONFIGURE_DEPENDS "${LARECOMP_GPU_HLSL_DIR}/*.hlsli")
# M6c: the vendored post-processing sources the host shaders
# include by their directory -- "ffx-fsr1/ffx_a.h", "smaa/SMAA.hlsl",
# "fxaa/FXAA3_11.h" -- unchanged, each with its licence.
set(LARECOMP_GPU_THIRDPARTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../thirdparty")
file(GLOB LARECOMP_GPU_THIRDPARTY_SHADER_SOURCES CONFIGURE_DEPENDS
     "${LARECOMP_GPU_THIRDPARTY_DIR}/ffx-fsr1/*.h"
     "${LARECOMP_GPU_THIRDPARTY_DIR}/smaa/*.h"
     "${LARECOMP_GPU_THIRDPARTY_DIR}/smaa/*.hlsl"
     "${LARECOMP_GPU_THIRDPARTY_DIR}/fxaa/*.h")

# larecomp_gpu_host_shader(<stem> <profile> [SOURCE <hlsl stem>] [HLSL_VERSION <year>]
#                          [DEFINES NAME=VALUE...]):
# the same source compiled more than once (variants) under different stems.
# HLSL 2021 unless HLSL_VERSION says otherwise: AMD's ffx_a.h uses the vector
# ternaries 2021 rejects, so the FSR1 passes build as 2018.
function(larecomp_gpu_host_shader STEM PROFILE)
    cmake_parse_arguments(ARG "" "SOURCE;HLSL_VERSION" "DEFINES" ${ARGN})
    if(NOT ARG_SOURCE)
        set(ARG_SOURCE "${STEM}")
    endif()
    if(NOT ARG_HLSL_VERSION)
        set(ARG_HLSL_VERSION 2021)
    endif()
    set(define_args "")
    foreach(_define IN LISTS ARG_DEFINES)
        list(APPEND define_args -D "${_define}")
    endforeach()
    foreach(_backend IN LISTS LARECOMP_GPU_BACKENDS)
        if(_backend STREQUAL "D3D12")
            set(ext dxil)
            set(format_args "")
        else()
            set(ext spirv)
            # D3D cbuffer packing on Vulkan too, and D3D clip-space y.
            set(format_args -spirv -fvk-use-dx-layout)
            if(PROFILE MATCHES "^vs")
                list(APPEND format_args -fvk-invert-y)
            endif()
        endif()
        set(out "${LARECOMP_GPU_SHADER_DIR}/${STEM}.hlsl.${ext}.h")
        add_custom_command(
            OUTPUT "${out}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${LARECOMP_GPU_SHADER_DIR}"
            COMMAND ${LARECOMP_GPU_DXC}
                    -T ${PROFILE} -HV ${ARG_HLSL_VERSION} -all-resources-bound
                    -Wno-ignored-attributes
                    ${format_args} ${define_args}
                    -I "${LARECOMP_GPU_HLSL_DIR}" -I "${LARECOMP_GPU_THIRDPARTY_DIR}"
                    -Fh "${out}" -Vn g_${STEM}_${ext}
                    "${LARECOMP_GPU_HLSL_DIR}/${ARG_SOURCE}.hlsl"
            DEPENDS "${LARECOMP_GPU_HLSL_DIR}/${ARG_SOURCE}.hlsl" ${LARECOMP_GPU_HLSL_INCLUDES}
                    ${LARECOMP_GPU_THIRDPARTY_SHADER_SOURCES}
            COMMENT "Compiling host shader ${STEM} from ${ARG_SOURCE}.hlsl (${PROFILE}, ${ext})"
            VERBATIM)
        target_sources(larecomp_gpu PRIVATE "${out}")
    endforeach()
endfunction()

larecomp_gpu_host_shader(overlay_vs vs_6_0)
larecomp_gpu_host_shader(overlay_ps ps_6_0)
# M4.6: full-screen passes -- resolve conversions, present.
larecomp_gpu_host_shader(fullscreen_vs vs_6_0)
larecomp_gpu_host_shader(resolve_color_ps ps_6_0 SOURCE resolve_ps
                         DEFINES RESOLVE_DEPTH=0 RESOLVE_MSAA=0)
larecomp_gpu_host_shader(resolve_color_ms_ps ps_6_0 SOURCE resolve_ps
                         DEFINES RESOLVE_DEPTH=0 RESOLVE_MSAA=1)
larecomp_gpu_host_shader(resolve_depth_ps ps_6_0 SOURCE resolve_ps
                         DEFINES RESOLVE_DEPTH=1 RESOLVE_MSAA=0)
larecomp_gpu_host_shader(resolve_depth_ms_ps ps_6_0 SOURCE resolve_ps
                         DEFINES RESOLVE_DEPTH=1 RESOLVE_MSAA=1)
# A depth resolve at a whole resolution scale that also writes the resolve
# memory's dwords (resolve_memory.h FusedWrite).
larecomp_gpu_host_shader(resolve_depth_fused_ps ps_6_0 SOURCE resolve_ps
                         DEFINES RESOLVE_DEPTH=1 RESOLVE_MSAA=0 RESOLVE_FUSED=1)
larecomp_gpu_host_shader(resolve_depth_ms_fused_ps ps_6_0 SOURCE resolve_ps
                         DEFINES RESOLVE_DEPTH=1 RESOLVE_MSAA=1 RESOLVE_FUSED=1)
# The same at a fractional resolution scale (RESOLVE_FUSED=2: a pixel stores for none, one or
# several guest texels).
larecomp_gpu_host_shader(resolve_depth_fused_frac_ps ps_6_0 SOURCE resolve_ps
                         DEFINES RESOLVE_DEPTH=1 RESOLVE_MSAA=0 RESOLVE_FUSED=2)
larecomp_gpu_host_shader(resolve_depth_ms_fused_frac_ps ps_6_0 SOURCE resolve_ps
                         DEFINES RESOLVE_DEPTH=1 RESOLVE_MSAA=1 RESOLVE_FUSED=2)
larecomp_gpu_host_shader(present_ps ps_6_0)
larecomp_gpu_host_shader(reduce_ps ps_6_0)
# M6c.1: FSR1 on the present -- EASU passes, RCAS, and the
# present's bilinear stretch without the gamma table (it was applied first).
larecomp_gpu_host_shader(fsr_easu_ps ps_6_0 HLSL_VERSION 2018)
larecomp_gpu_host_shader(fsr_rcas_ps ps_6_0 HLSL_VERSION 2018)
larecomp_gpu_host_shader(present_stretch_ps ps_6_0 SOURCE present_ps DEFINES PRESENT_NO_RAMP=1)
# HDR10 output (hdr.h): the passes that write the back buffer end with hlsl/hdr_output.hlsli.
# The intermediates' passes (the gamma table into FSR1's images, EASU, RCAS into an image) stay SDR.
larecomp_gpu_host_shader(present_hdr_ps ps_6_0 SOURCE present_ps DEFINES PRESENT_HDR=1)
larecomp_gpu_host_shader(present_stretch_hdr_ps ps_6_0 SOURCE present_ps
                         DEFINES PRESENT_NO_RAMP=1 PRESENT_HDR=1)
larecomp_gpu_host_shader(fsr_rcas_hdr_ps ps_6_0 SOURCE fsr_rcas_ps HLSL_VERSION 2018
                         DEFINES RCAS_HDR=1)
larecomp_gpu_host_shader(overlay_hdr_ps ps_6_0 SOURCE overlay_ps DEFINES OVERLAY_HDR=1)
# Frame generation (frame_generation.h): the scene and the generated frame prepared for the
# interpolation, the generated frame composed with the UI layer, and the pacer's copy of a finished
# present image into the swap chain's back buffer.
larecomp_gpu_host_shader(fg_prepare_ps ps_6_0)
larecomp_gpu_host_shader(fg_compose_ps ps_6_0)
larecomp_gpu_host_shader(fg_copy_ps ps_6_0)
# DLSS frame generation (dlss_frame_generation.h): the UI layer fitted to the window as the present
# pass fits the picture, with the display's table (and, for an HDR10 back buffer, the HDR output step).
larecomp_gpu_host_shader(dlssg_ui_ps ps_6_0)
larecomp_gpu_host_shader(dlssg_ui_hdr_ps ps_6_0 SOURCE dlssg_ui_ps DEFINES PRESENT_HDR=1)
# The UI layer split (frame_layers.h, hdr_scene.h): the scene put back under the UI in the guest's
# target, and the HDR present's compose of the two layers.
larecomp_gpu_host_shader(layers_restore_ps ps_6_0 SOURCE frame_layers_ps DEFINES LAYERS_COMPOSE=0)
larecomp_gpu_host_shader(hdr_compose_ps ps_6_0 SOURCE frame_layers_ps DEFINES LAYERS_COMPOSE=1)
# The same layers from a multisampled target (the pause menu's and the map's UI target): the samples of
# each pixel averaged into a single-sampled image.
larecomp_gpu_host_shader(layers_resolve_ps ps_6_0 SOURCE frame_layers_ps
                         DEFINES LAYERS_COMPOSE=0 LAYERS_RESOLVE=1)
# M6c.2/M6c.3: the scene's anti-aliasing before the UI -- the copy
# (luma in alpha), FXAA 3.11, SMAA 1x's three passes.
larecomp_gpu_host_shader(post_luma_ps ps_6_0)
larecomp_gpu_host_shader(fxaa_ps ps_6_0)
larecomp_gpu_host_shader(smaa_edges_ps ps_6_0 SOURCE smaa_ps DEFINES SMAA_PASS=1)
larecomp_gpu_host_shader(smaa_weights_ps ps_6_0 SOURCE smaa_ps DEFINES SMAA_PASS=2)
larecomp_gpu_host_shader(smaa_blend_ps ps_6_0 SOURCE smaa_ps DEFINES SMAA_PASS=3)
# F4 step 1b: the temporal system's passes -- the motion vectors and the copy of the depth it keeps
# (temporal_mv.h).
larecomp_gpu_host_shader(temporal_mv_ps ps_6_0)
larecomp_gpu_host_shader(temporal_copy_ps ps_6_0)
# The vehicles' motion (vehicle_motion.h): a row of the table the vector pass reads; and the masks the
# upscalers take as their reactive and transparency inputs (upscaler_masks.h).
larecomp_gpu_host_shader(temporal_vehicle_table_ps ps_6_0)
larecomp_gpu_host_shader(upscaler_masks_ps ps_6_0)
# The screen-space ambient occlusion's passes (ao.h): the linear depth and its coarser levels, the
# GTAO visibility, the denoise, the accumulation over frames and the apply into the scene colour; the
# depth, the accumulation (its vehicle stencil test) and the apply read the EDRAM depth target's
# planes, multisampled or not.
larecomp_gpu_host_shader(ao_depth_ps ps_6_0 DEFINES AO_DEPTH_MS=0)
larecomp_gpu_host_shader(ao_depth_ms_ps ps_6_0 SOURCE ao_depth_ps DEFINES AO_DEPTH_MS=1)
larecomp_gpu_host_shader(ao_mip_ps ps_6_0)
larecomp_gpu_host_shader(ao_main_ps ps_6_0)
larecomp_gpu_host_shader(ao_denoise_ps ps_6_0)
larecomp_gpu_host_shader(ao_temporal_ps ps_6_0 DEFINES AO_DEPTH_MS=0)
larecomp_gpu_host_shader(ao_temporal_ms_ps ps_6_0 SOURCE ao_temporal_ps DEFINES AO_DEPTH_MS=1)
larecomp_gpu_host_shader(ao_apply_ps ps_6_0 DEFINES AO_DEPTH_MS=0)
larecomp_gpu_host_shader(ao_apply_ms_ps ps_6_0 SOURCE ao_apply_ps DEFINES AO_DEPTH_MS=1)
# M5.2b: the resolve memory -- a resolve's bytes (colour from the
# product, depth from the EDRAM) and a texture decoded from them.
larecomp_gpu_host_shader(resolve_memory_color_cs cs_6_0 SOURCE resolve_memory_cs
                         DEFINES RESOLVE_DEPTH=0 RESOLVE_MSAA=0)
larecomp_gpu_host_shader(resolve_memory_depth_cs cs_6_0 SOURCE resolve_memory_cs
                         DEFINES RESOLVE_DEPTH=1 RESOLVE_MSAA=0)
larecomp_gpu_host_shader(resolve_memory_depth_ms_cs cs_6_0 SOURCE resolve_memory_cs
                         DEFINES RESOLVE_DEPTH=1 RESOLVE_MSAA=1)
larecomp_gpu_host_shader(resolve_memory_ps ps_6_0)
# a vertex stream over memory a resolve wrote (the rain's particle state).
larecomp_gpu_host_shader(resolve_memory_vertex_cs cs_6_0)
