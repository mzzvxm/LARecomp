# The Plume renderer, src/gpu.
#
# larecomp_gpu is an OBJECT library so the renderer compiles with its own
# include order and definitions: plume's headers (its Vulkan-Headers and VMA)
# ahead of rex::runtime's, which arrive as system includes, and
# LARECOMP_GPU_<backend> visible to these files only. larecomp links the
# objects. Expects the caller to have added thirdparty/plume and
# thirdparty/dxc-bin and to set LARECOMP_GPU_BACKEND (D3D12 or VULKAN).

add_library(larecomp_gpu OBJECT
    backend.cpp
    capture.cpp
    constants.cpp
    descriptors.cpp
    device.cpp
    draw_state.cpp
    format.cpp
    frame_trace.cpp
    frame.cpp
    guest_capture.cpp
    guest_memory.cpp
    input_layout.cpp
    layout.cpp
    memory_census.cpp
    native_window.cpp
    overlay.cpp
    pipelines.cpp
    region_index.cpp
    render_thread.cpp
    renderer.cpp
    samplers.cpp
    scene.cpp
    shaders.cpp
    stream.cpp
    swap_chain.cpp
    timing.cpp
    topology.cpp
    upload.cpp
    validation.cpp
    viewport.cpp
)

set(LARECOMP_GPU_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
# src/ too: the guest decoders the renderer reuses from the legacy runtime are
# included as "native_gfx/...", which keeps every such dependency visible by
# its path. Their definitions come from larecomp itself at link.
target_include_directories(larecomp_gpu PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/.."
    "${LARECOMP_GPU_SHADER_DIR}"
)
# plume first: see the note at the top.
target_link_libraries(larecomp_gpu PRIVATE plume rex::runtime rex::renderdoc)
target_compile_definitions(larecomp_gpu PRIVATE LARECOMP_GPU_${LARECOMP_GPU_BACKEND})
# The same flags the SDK applies to its own object library (larecomp_recomp).
rexglue_apply_target_settings(larecomp_gpu)

# ---------------------------------------------------------------------------
# Host shaders: hlsl/<stem>.hlsl -> shaders/<stem>.hlsl.<dxil|spirv>.h with
# g_<stem>_<dxil|spirv>, in this build's backend format only. Rule as in reeot
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

function(larecomp_gpu_host_shader STEM PROFILE)
    if(LARECOMP_GPU_BACKEND STREQUAL "D3D12")
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
                -T ${PROFILE} -HV 2021 -all-resources-bound -Wno-ignored-attributes
                ${format_args}
                -I "${LARECOMP_GPU_HLSL_DIR}"
                -Fh "${out}" -Vn g_${STEM}_${ext}
                "${LARECOMP_GPU_HLSL_DIR}/${STEM}.hlsl"
        DEPENDS "${LARECOMP_GPU_HLSL_DIR}/${STEM}.hlsl" ${LARECOMP_GPU_HLSL_INCLUDES}
        COMMENT "Compiling host shader ${STEM}.hlsl (${PROFILE}, ${ext})"
        VERBATIM)
    target_sources(larecomp_gpu PRIVATE "${out}")
endfunction()

larecomp_gpu_host_shader(overlay_vs vs_6_0)
larecomp_gpu_host_shader(overlay_ps ps_6_0)
