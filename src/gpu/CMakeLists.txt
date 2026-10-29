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
    backend.cpp
    buffer_heap.cpp
    capture.cpp
    constants.cpp
    cycles.cpp
    descriptors.cpp
    device.cpp
    draw_census.cpp
    draw_state.cpp
    edram.cpp
    flicker_probe.cpp
    format.cpp
    frame_trace.cpp
    frame.cpp
    geometry.cpp
    host_passes.cpp
    guest_capture.cpp
    guest_memory.cpp
    input_layout.cpp
    layout.cpp
    memory_census.cpp
    native_window.cpp
    overlay.cpp
    pass_timing.cpp
    pipelines.cpp
    post.cpp
    present.cpp
    region_index.cpp
    render_thread.cpp
    renderer.cpp
    resolve.cpp
    resolve_memory.cpp
    samplers.cpp
    scene.cpp
    shaders.cpp
    stream.cpp
    swap_chain.cpp
    texture_decode.cpp
    texture_names.cpp
    textures.cpp
    thread_load.cpp
    timing.cpp
    topology.cpp
    upload.cpp
    validation.cpp
    viewport.cpp
    watch.cpp
)
# The only translation units that see a backend's own objects (backend.h).
if("D3D12" IN_LIST LARECOMP_GPU_BACKENDS)
    target_sources(larecomp_gpu PRIVATE backend_d3d12.cpp)
endif()
if("VULKAN" IN_LIST LARECOMP_GPU_BACKENDS)
    target_sources(larecomp_gpu PRIVATE backend_vulkan.cpp)
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
larecomp_gpu_host_shader(present_ps ps_6_0)
larecomp_gpu_host_shader(reduce_ps ps_6_0)
# M6c.1: FSR1 on the present -- EASU passes, RCAS, and the
# present's bilinear stretch without the gamma table (it was applied first).
larecomp_gpu_host_shader(fsr_easu_ps ps_6_0 HLSL_VERSION 2018)
larecomp_gpu_host_shader(fsr_rcas_ps ps_6_0 HLSL_VERSION 2018)
larecomp_gpu_host_shader(present_stretch_ps ps_6_0 SOURCE present_ps DEFINES PRESENT_NO_RAMP=1)
# M6c.2/M6c.3: the scene's anti-aliasing before the UI -- the copy
# (luma in alpha), FXAA 3.11, SMAA 1x's three passes.
larecomp_gpu_host_shader(post_luma_ps ps_6_0)
larecomp_gpu_host_shader(fxaa_ps ps_6_0)
larecomp_gpu_host_shader(smaa_edges_ps ps_6_0 SOURCE smaa_ps DEFINES SMAA_PASS=1)
larecomp_gpu_host_shader(smaa_weights_ps ps_6_0 SOURCE smaa_ps DEFINES SMAA_PASS=2)
larecomp_gpu_host_shader(smaa_blend_ps ps_6_0 SOURCE smaa_ps DEFINES SMAA_PASS=3)
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
