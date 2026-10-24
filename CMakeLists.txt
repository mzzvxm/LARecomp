
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
# Monocypher (nick keys and tickets, thirdparty/monocypher) is C99 that also
# compiles as C++; the project only enables CXX.
set_source_files_properties(thirdparty/monocypher/monocypher.c PROPERTIES LANGUAGE CXX)

set(LARECOMP_SOURCES
    src/main.cpp
    src/spdlog_console.cpp
    src/crash_handler.cpp

    src/mc_engine/threading.cpp

    # Steering wheel support.
    src/mc_engine/wheel/wheel_fake.cpp
    src/mc_engine/wheel/wheel_input.cpp
    src/mc_engine/wheel/wheel_ffb.cpp
    src/mc_engine/wheel/wheel_dinput.cpp

    # Guest hooks, one file per category. hooks.cpp holds InitHooks and the
    # per-frame Patch_DeltaTimePre; hooks_internal.h is what the files share.
    src/mc_engine/hooks/hooks.cpp
    src/mc_engine/hooks/ui.cpp
    src/mc_engine/hooks/ui_theme.cpp
    src/mc_engine/hooks/world_tuning.cpp
    src/mc_engine/hooks/render_phases.cpp
    src/mc_engine/hooks/frame_timing.cpp
    src/mc_engine/hooks/timing_log.cpp
    src/mc_engine/hooks/graphics.cpp
    src/mc_engine/hooks/dev_options.cpp
    src/mc_engine/hooks/camera.cpp
    src/mc_engine/hooks/vehicle.cpp
    src/mc_engine/hooks/rubberband.cpp
    src/mc_engine/hooks/vinyl.cpp
    src/mc_engine/hooks/vinyl_shapes.cpp
    src/mc_engine/hooks/photo_mode.cpp
    src/mc_engine/hooks/readback_window.cpp
    src/mc_engine/hooks/mods.cpp
    src/mc_engine/hooks/discord.cpp
    src/mc_engine/hooks/diagnostics.cpp
    src/mc_engine/hooks/warper.cpp
    src/mc_engine/hooks/photo_album.cpp
    src/mc_engine/hooks/rear_view.cpp

    src/mc_engine/guest_profiler.cpp
    src/mc_engine/carbon_parts.cpp
    src/mc_engine/graphics_button.cpp
    src/mc_engine/pause_menu.cpp
    src/mc_engine/string_table.cpp
    src/mc_engine/string_dumper.cpp
    src/mc_engine/menu_camera.cpp
    src/mc_engine/map_mouse.cpp
    src/mc_engine/camera_look.cpp
    src/mc_engine/keyboard_native.cpp
    src/mc_engine/time_of_day.cpp
    src/mc_engine/map_warp.cpp
    src/mc_engine/real_weather.cpp
    src/mc_engine/squash_stretch.cpp
    src/mc_engine/tire_offset.cpp
    src/mc_engine/hud_units.cpp
    src/mc_engine/cutscene_gallery.cpp
    src/mc_engine/texture_dump.cpp
    src/mc_engine/boot_progress.cpp

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

    # Runtime features backed by per-mod sidecar data.
    src/mc_engine/modloader/features/mod_data.cpp
    src/mc_engine/modloader/features/mod_glows.cpp
    src/mc_engine/modloader/features/mod_breakables.cpp
    src/mc_engine/modloader/features/mod_lights.cpp

    # Online (split out of hooks.cpp): tournament / System Link / Xbox LIVE
    src/mc_engine/online/online_common.cpp
    src/mc_engine/online/tournament.cpp
    src/mc_engine/online/system_link.cpp
    src/mc_engine/online/relay_info.cpp
    src/mc_engine/online/online_toast.cpp
    src/mc_engine/online/xbox_live.cpp
    src/mc_engine/online/race_props.cpp
    # Server browser (F6): relay list, rooms, joining a picked room.
    src/mc_engine/online/server_browser/browser_client.cpp
    src/mc_engine/online/server_browser/browser_join.cpp
    src/mc_engine/online/server_browser/browser_ui.cpp
    # Online identity (F7): the key that owns the gamertag, its XUID, the
    # nick proof to relays that check, backup and recovery code.
    src/mc_engine/online/identity/identity_store.cpp
    src/mc_engine/online/identity/identity_client.cpp
    src/mc_engine/online/identity/identity_ui.cpp
    thirdparty/monocypher/monocypher.c
    # Mod browser (F5) and the mod check/download the server browser uses too.
    src/mc_engine/modloader/modsbrowser/mod_download.cpp
    src/mc_engine/modloader/modsbrowser/mod_browser_ui.cpp

    # Rate My Ride: the rlUpload "RateMyRide" table served by our own backend
    # (local store, or larecomp_rmr_server shared with the other players).
    src/mc_engine/online/rate_my_ride/rate_my_ride.cpp
    src/mc_engine/online/rate_my_ride/rmr_client.cpp
    src/mc_engine/online/rate_my_ride/rmr_protocol.cpp
    src/mc_engine/online/rate_my_ride/rmr_store.cpp
    src/mc_engine/online/rate_my_ride/rmr_net.cpp

    # Leaderboards: the LIVE stats views served by the same backend.
    src/mc_engine/online/leaderboards/leaderboards.cpp
    src/mc_engine/online/leaderboards/lb_client.cpp
    src/mc_engine/online/leaderboards/lb_protocol.cpp
    src/mc_engine/online/leaderboards/lb_store.cpp

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
    src/native_gfx/guest/occlusion.cpp
    src/native_gfx/guest/texture_registry.cpp
    src/native_gfx/guest/texture_ownership.cpp
    src/native_gfx/guest/resource_lock.cpp
    src/native_gfx/guest/vblank_probe.cpp
    src/native_gfx/nocp/guest_gpu.cpp
    src/native_gfx/nocp/nocp_app.cpp
    src/native_gfx/guest/host_heap.cpp
    src/native_gfx/guest/host_resource_heap.cpp
    src/native_gfx/guest/guest_constants.cpp
    src/native_gfx/guest/render_state.cpp
    src/native_gfx/guest/sampler_state.cpp
    src/native_gfx/guest/guest_resources.cpp
    src/native_gfx/guest/texture_format.cpp
    src/native_gfx/guest/vertex_fetch_decode.cpp
    src/native_gfx/d3d12/constant_upload.cpp
    src/native_gfx/d3d12/context.cpp
    src/native_gfx/d3d12/device_manager.cpp
    src/native_gfx/d3d12/swapchain.cpp
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
    src/native_gfx/d3d12/fxaa_pass.cpp
    src/native_gfx/d3d12/external_blit.cpp
    src/native_gfx/d3d12/gamma_pass.cpp
    src/native_gfx/d3d12/tonemap_pass.cpp
    src/native_gfx/d3d12/d3d12_smoke_triangle.cpp
    src/native_gfx/d3d12/gpu_profiler.cpp
    src/native_gfx/d3d12/barrier_batch.cpp
    src/native_gfx/d3d12/untile_pass.cpp

    src/achievement_metadata.cpp

    src/discord_rpc/rpc_client.cpp
    src/discord_rpc/discord_rpc.cpp
    src/discord_rpc/discord_hooks.cpp

    src/isoinstaller/larecomp_iso_installer.cpp
    src/isoinstaller/install_wizard_dialog.cpp
    src/saveporter/larecomp_save_porter.cpp
    src/profile/larecomp_profile_store.cpp
    src/profile/larecomp_profile_picker.cpp
    src/bootprogress/larecomp_boot_progress.cpp
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

        # Keyboard key caps (src/keycaps/*.png) embedded in the exe, one RCDATA
        # per file named KEYCAP_<FILE NAME> (a.png -> KEYCAP_A, a_w.png ->
        # KEYCAP_A_W); keyboard_native.cpp reads them from there. The .rc is
        # regenerated whenever a PNG is added or removed.
        file(GLOB _larecomp_keycaps CONFIGURE_DEPENDS
             "${CMAKE_CURRENT_SOURCE_DIR}/src/keycaps/*.png")
        set(_larecomp_keycaps_rc "// Generated by CMakeLists.txt from src/keycaps/. Do not edit.\n")
        foreach(_cap ${_larecomp_keycaps})
            get_filename_component(_cap_name "${_cap}" NAME_WE)
            string(TOUPPER "${_cap_name}" _cap_name)
            string(APPEND _larecomp_keycaps_rc "KEYCAP_${_cap_name} RCDATA \"${_cap}\"\n")
        endforeach()
        file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/keycaps.rc"
             CONTENT "${_larecomp_keycaps_rc}" @ONLY)
        # The resource compiler does not report RCDATA inputs as dependencies,
        # so an edited PNG would not relink the exe without this.
        set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/keycaps.rc"
                                    PROPERTIES OBJECT_DEPENDS "${_larecomp_keycaps}")

        add_executable(larecomp WIN32 ${LARECOMP_SOURCES} src/larecomp.rc
                       "${CMAKE_CURRENT_BINARY_DIR}/keycaps.rc")
    else()
        add_executable(larecomp ${LARECOMP_SOURCES})
    endif()

    # GPU_PLUGINS stages rexgpu-xenos next to the executable: the Xenos GPU
    # emulation is a runtime-loaded plugin now, so it is never linked and
    # TARGET_RUNTIME_DLLS does not see it.
    rexglue_setup_target(larecomp GPU_PLUGINS xenos)

    # The Plume renderer, src/gpu.
    # Every backend the platform has is built in -- D3D12 and Vulkan on Windows,
    # Vulkan elsewhere -- and the SDK's graphics_backend cvar picks one at
    # startup; the host shaders are compiled once per
    # backend format. Plume itself is the submodule thirdparty/plume: the
    # project's fork (mzzvxm/plume, branch larecomp) of the zolaware revision
    # reblue ships (SDL3 Vulkan path, fill mode, R11G11B10/D24S8), plus the
    # teardown leak fixes; the host shaders are compiled by
    # the dxc of the submodule thirdparty/dxc-bin (renderbag, the revision reblue
    # and reeot use). Off outside Windows until that path has been compiled.
    if(WIN32)
        set(_larecomp_gpu_default ON)
    else()
        set(_larecomp_gpu_default OFF)
    endif()
    option(LARECOMP_GPU "Build the Plume renderer (src/gpu)" ${_larecomp_gpu_default})
    if(LARECOMP_GPU)
        # Metal is not wired.
        if(WIN32)
            set(LARECOMP_GPU_BACKENDS D3D12 VULKAN)
        else()
            set(LARECOMP_GPU_BACKENDS VULKAN)
        endif()
        foreach(_larecomp_gpu_dep plume/plume_render_interface.h dxc-bin/CMakeLists.txt)
            if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/${_larecomp_gpu_dep}")
                message(FATAL_ERROR "thirdparty/${_larecomp_gpu_dep} is missing: "
                                    "git submodule update --init --recursive")
            endif()
        endforeach()
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            # Plume builds the Vulkan surface through the SDK's SDL3 window.
            set(PLUME_SDL_VULKAN_ENABLED ON CACHE BOOL "" FORCE)
        endif()
        add_subdirectory(thirdparty/plume EXCLUDE_FROM_ALL)
        # Defines DIRECTX_DXC_TOOL. Skipped if something else already brought
        # the same dxc-bin in (an in-tree XenosRecomp carries it too).
        if(NOT TARGET Microsoft::DirectXShaderCompiler)
            add_subdirectory(thirdparty/dxc-bin EXCLUDE_FROM_ALL)
        endif()
        add_subdirectory(src/gpu)
        # The renderer's objects, and plume's archive without plume's include
        # directories in larecomp's own translation units (CMP0131 is NEW with
        # cmake_minimum_required 3.25). The rest of larecomp sees only gpu.h.
        target_link_libraries(larecomp PRIVATE larecomp_gpu $<LINK_ONLY:plume>)
        target_compile_definitions(larecomp PRIVATE LARECOMP_HAS_GPU=1)
        message(STATUS "larecomp: Plume renderer (src/gpu) built for ${LARECOMP_GPU_BACKENDS}")
    endif()

    # rexglue 0.7.4
    if(WIN32)
        # The native_gfx diagnostics use std::fopen on purpose (append-only text
        # logs, no error path worth the fopen_s dance). MSVC's CRT marks it
        # deprecated; the _s variants are not portable, so opt out project-wide.
        target_compile_definitions(larecomp PRIVATE _CRT_SECURE_NO_WARNINGS)

        target_link_libraries(larecomp
            PRIVATE
            # native_gfx guest-visible object allocator (guest/host_heap.cpp):
            # coalescing heap over a block of guest physical memory.
            rex::o1heap
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
            # Rate My Ride client sockets (mc_engine/online/rate_my_ride)
            ws2_32
        )
    endif()

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/assets")
        add_custom_command(TARGET larecomp POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/src/assets"
            "$<TARGET_FILE_DIR:larecomp>/assets"
        )
    endif()

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/gamecontrollerdb.txt")
        add_custom_command(TARGET larecomp POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CMAKE_CURRENT_SOURCE_DIR}/assets/gamecontrollerdb.txt"
                    "$<TARGET_FILE_DIR:larecomp>/gamecontrollerdb.txt"
            COMMENT "Copying gamecontrollerdb.txt next to larecomp.exe")
    endif()

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/D3D12")
        add_custom_command(TARGET larecomp POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${CMAKE_CURRENT_SOURCE_DIR}/assets/D3D12"
                    "$<TARGET_FILE_DIR:larecomp>/D3D12"
            COMMENT "Copying Agility SDK runtime binaries next to larecomp.exe")
    endif()
endif()
