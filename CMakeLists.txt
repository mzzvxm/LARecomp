
# larecomp - ReXGlue Recompiled Project
#
# This file is yours to edit. 'rexglue migrate' will NOT overwrite it.
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(larecomp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(WIN32)
    set(REXGLUE_REQUIRED_GRAPHICS_BACKENDS "d3d12;vulkan" CACHE STRING "Required ReXGlue graphics backends")
else()
    set(REXGLUE_REQUIRED_GRAPHICS_BACKENDS "vulkan" CACHE STRING "Required ReXGlue graphics backends")
endif()

include(generated/rexglue.cmake)

# Sources
set(LARECOMP_SOURCES
    src/main.cpp
    src/spdlog_console.cpp
    src/crash_handler.cpp

    src/mc_engine/threading.cpp
    src/mc_engine/hooks.cpp
    src/mc_engine/carbon_parts.cpp
    src/mc_engine/graphics_button.cpp
    src/mc_engine/pause_menu.cpp
    src/mc_engine/string_table.cpp
    src/mc_engine/menu_camera.cpp
    src/mc_engine/time_of_day.cpp
    src/mc_engine/real_weather.cpp
    src/mc_engine/squash_stretch.cpp
    src/mc_engine/tire_offset.cpp
    src/mc_engine/hud_units.cpp

    # Custom MP3 radio: <exe>/music/*.mp3 injected into mcMusicManager, decoded
    # and played on the host (Media Foundation + XAudio2).
    src/mc_engine/mp3custom/mp3custom.cpp
    src/mc_engine/mp3custom/mp3_player.cpp

    # Native custom music: <exe>/music/*.mp3 -> real radio entries, real cues and
    # real streaming wave banks, packed into the mod archive.
    src/mc_engine/music/audio_dat.cpp
    src/mc_engine/music/wave_bank.cpp
    src/mc_engine/music/custom_music.cpp

    # Modloader: models/<mod>/<asset>.obj -> xarchive_mods.rpf
    src/mc_engine/modloader/aes256.cpp
    src/mc_engine/modloader/rpf3.cpp
    src/mc_engine/modloader/xcompress.cpp
    src/mc_engine/modloader/lzx_encode.cpp
    src/mc_engine/modloader/objmesh.cpp
    src/mc_engine/modloader/gltf.cpp
    src/mc_engine/modloader/rsc5.cpp
    src/mc_engine/modloader/texture.cpp
    src/mc_engine/modloader/modloader.cpp

    # Online (split out of hooks.cpp): tournament / System Link / Xbox LIVE
    src/mc_engine/online/online_common.cpp
    src/mc_engine/online/tournament.cpp
    src/mc_engine/online/system_link.cpp
    src/mc_engine/online/xbox_live.cpp

    # MILESTONE 0 - temporary passive instrumentation, cvar-gated (default off).
    # Remove this line and the src/native_probe/ directory once the Native
    # Graphics Runtime research is closed.
    src/native_probe/mcla_gfx_probe.cpp

    # MCLA Native Graphics Runtime (permanent). Incremental replacement of the
    # Xenia-provided graphics backend; everything is inert until the
    # mcla_native_gfx cvar exists and is enabled.
    src/native_gfx/shader_identity.cpp
    src/native_gfx/vertex_declaration.cpp
    src/native_gfx/draw_slicing.cpp
    src/native_gfx/native_gfx.cpp
    src/native_gfx/hooks.cpp
    src/native_gfx/telemetry.cpp
    src/native_gfx/geometry.cpp
    src/native_gfx/guest/guest_fence.cpp
    src/native_gfx/guest/vblank_probe.cpp
    src/native_gfx/guest/guest_constants.cpp
    src/native_gfx/guest/render_state.cpp
    src/native_gfx/guest/sampler_state.cpp
    src/native_gfx/guest/guest_resources.cpp
    src/native_gfx/guest/texture_format.cpp
    src/native_gfx/guest/vertex_fetch_decode.cpp
    src/native_gfx/d3d12/constant_upload.cpp
    src/native_gfx/d3d12/context.cpp
    src/native_gfx/d3d12/first_draw.cpp
    src/native_gfx/d3d12/frame_capture.cpp
    src/native_gfx/d3d12/render_target_pool.cpp
    src/native_gfx/d3d12/renderdoc_hook.cpp
    src/native_gfx/d3d12/image_dump.cpp
    src/native_gfx/d3d12/memory_census.cpp
    src/native_gfx/d3d12/pipeline_cache.cpp
    src/native_gfx/d3d12/presenter_output.cpp
    src/native_gfx/d3d12/resource_cache.cpp
    src/native_gfx/d3d12/shader_db.cpp
    src/native_gfx/d3d12/texture_binding.cpp
    src/native_gfx/d3d12/texture_cache.cpp
    src/native_gfx/d3d12/topology_expand.cpp
    src/native_gfx/d3d12/blit_pass.cpp
    src/native_gfx/d3d12/tonemap_pass.cpp
    src/native_gfx/d3d12/d3d12_smoke_triangle.cpp

    src/discord_rpc/discord_rpc.cpp
    src/discord_rpc/discord_hooks.cpp

    src/isoinstaller/larecomp_iso_installer.cpp
    src/saveporter/larecomp_save_porter.cpp
)
if(REXGLUE_ENABLE_XEO3_TARGET)
    # The XEO3 DLL is a separate build, not a variant of this one: it needs the
    # XEO3 Clang fork (this project's compiler cannot produce the calling
    # convention), and it must not link rexruntime -- inside XEO3 the real
    # kernel and GPU are the emulator's, not ours. So none of LARECOMP_SOURCES
    # belongs in it.
    #
    # Sources come from a dedicated codegen run:
    #   rexglue codegen --xeo3 larecomp_xeo3_manifest.toml   -> generated_xeo3/
    # which also writes generated_xeo3/CMakeLists.txt. This drives that project.
    message(STATUS "Building XEO3 DLL target for Microsoft Emulator")
    rexglue_add_xeo3_dll_target(larecomp_xeo3
        GENERATED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/generated_xeo3"
        OUTPUT_NAME   "xeo3_545407F8"
    )
else()
    if(WIN32)
        # Compile the .rc so the app icon is embedded in the exe (not just copied
        # into assets/). The lowest-ID ICON in larecomp.rc is used by Explorer.
        #
        # Microsoft's rc.exe cannot parse the `# <line> "file"` markers that
        # clang -E emits, so CMake's cmake_llvm_rc wrapper fails a clean build
        # with "RC2019: expected preprocessor command, found '1'". llvm-rc
        # understands them, so prefer it when it sits beside the clang in use.
        # Must be set before enable_language(RC), and it is cached - delete the
        # build directory if switching toolchains.
        if(NOT CMAKE_RC_COMPILER)
            get_filename_component(_larecomp_llvm_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
            find_program(LLVM_RC_EXECUTABLE NAMES llvm-rc HINTS "${_larecomp_llvm_bin}")
            if(LLVM_RC_EXECUTABLE)
                set(CMAKE_RC_COMPILER "${LLVM_RC_EXECUTABLE}" CACHE FILEPATH "" FORCE)
            endif()
        endif()
        enable_language(RC)
        add_executable(larecomp WIN32 ${LARECOMP_SOURCES} src/larecomp.rc)
    else()
        add_executable(larecomp ${LARECOMP_SOURCES})
    endif()

    # GPU_PLUGINS stages rexgpu-xenos next to the executable: the Xenos GPU
    # emulation is a runtime-loaded plugin now, so it is never linked and
    # TARGET_RUNTIME_DLLS does not see it.
    rexglue_setup_target(larecomp GPU_PLUGINS xenos)

    # rexglue 0.7.4
    if(WIN32)
        # The native_gfx diagnostics use std::fopen on purpose (append-only text
        # logs, no error path worth the fopen_s dance). MSVC's CRT marks it
        # deprecated; the _s variants are not portable, so opt out project-wide.
        target_compile_definitions(larecomp PRIVATE _CRT_SECURE_NO_WARNINGS)

        target_link_libraries(larecomp
            PRIVATE
            dxgi
            # native_gfx D3D12 backend (D3D12SerializeRootSignature etc.)
            d3d12
            # native_gfx memory census (GetProcessMemoryInfo)
            psapi
            # real-world weather fetch (mc_engine/real_weather.cpp)
            winhttp
            # custom MP3 radio (mc_engine/mp3custom): Media Foundation decode.
            # XAudio2 itself is loaded by name at runtime, so it needs no lib.
            mfplat
            mfreadwrite
            mfuuid
            ole32
        )
    endif()

    add_custom_command(TARGET larecomp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/src/assets"
        "$<TARGET_FILE_DIR:larecomp>/assets"
    )

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/gamecontrollerdb.txt")
        add_custom_command(TARGET larecomp POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CMAKE_CURRENT_SOURCE_DIR}/assets/gamecontrollerdb.txt"
                    "$<TARGET_FILE_DIR:larecomp>/gamecontrollerdb.txt"
            COMMENT "Copying gamecontrollerdb.txt next to larecomp.exe")
    endif()
endif()
