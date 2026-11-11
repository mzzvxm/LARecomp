
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

if(MINGW)
    # llvm-mingw (Clang with lld, libc++ and mingw-w64 on the UCRT): the build for a PC
    # without Visual Studio. The SDK has to be built with the same toolchain (libc++ and the
    # Microsoft STL do not mix). __try/__except (mc_engine/camera_look.cpp) needs
    # -fms-extensions, and mingw-w64 assumes Windows 7 (_WIN32_WINNT 0x601) where the
    # Windows SDK assumes the current Windows. Both as the SDK sets them for itself.
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fms-extensions>)
    add_compile_definitions(_WIN32_WINNT=0x0A00)
    # No -fasync-exceptions for the generated code: for a MinGW target the Clang driver drops
    # it ("argument unused", on every file), and passed to the compiler itself it has crashed
    # Clang in SimplifyCFG. A fault inside a guest __try scope is therefore not caught with
    # this toolchain; the generated code of this game has no such scope. The sources carry
    # the list as it was when the file was included, so it is set again.
    list(REMOVE_ITEM REXGLUE_RECOMP_OPTIONS -fasync-exceptions)
    if(REXGLUE_ENTRYPOINT_GENERATED_SOURCES)
        set_source_files_properties(${REXGLUE_ENTRYPOINT_GENERATED_SOURCES}
            PROPERTIES COMPILE_OPTIONS "${REXGLUE_RECOMP_OPTIONS}")
    endif()
endif()

# Sources
# Monocypher (nick keys and tickets, thirdparty/monocypher) is C99 that also
# compiles as C++; the project only enables CXX.
set_source_files_properties(thirdparty/monocypher/monocypher.c PROPERTIES LANGUAGE CXX)

set(LARECOMP_SOURCES
    src/main.cpp
    src/spdlog_console.cpp
    src/crash_handler.cpp

    src/mc_engine/threading.cpp
    src/mc_engine/fma_fast.cpp

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
    src/mc_engine/hooks/ui_smooth.cpp
    src/mc_engine/hooks/world_tuning.cpp
    src/mc_engine/hooks/render_phases.cpp
    src/mc_engine/hooks/frame_timing.cpp
    src/mc_engine/hooks/timer_precision.cpp
    src/mc_engine/hooks/timing_log.cpp
    src/mc_engine/hooks/graphics.cpp
    src/mc_engine/hooks/dev_options.cpp
    src/mc_engine/hooks/display_gamma.cpp
    src/mc_engine/hooks/camera.cpp
    src/mc_engine/hooks/vehicle.cpp
    src/mc_engine/hooks/vehicle_records.cpp
    src/mc_engine/hooks/showroom.cpp
    src/mc_engine/hooks/rubberband.cpp
    src/mc_engine/hooks/vinyl.cpp
    src/mc_engine/hooks/vinyl_shapes.cpp
    src/mc_engine/hooks/pump_wake.cpp
    src/mc_engine/hooks/photo_mode.cpp
    src/mc_engine/hooks/readback_window.cpp
    src/mc_engine/hooks/mods.cpp
    src/mc_engine/hooks/discord.cpp
    src/mc_engine/hooks/audio_mixer.cpp
    src/mc_engine/hooks/job_workers.cpp
    src/mc_engine/hooks/diagnostics.cpp
    src/mc_engine/hooks/vibration.cpp
    src/mc_engine/hooks/stick_deadzone.cpp
    src/mc_engine/hooks/warper.cpp
    src/mc_engine/hooks/photo_album.cpp
    src/mc_engine/hooks/rear_view.cpp
    src/mc_engine/hooks/script_input.cpp

    src/mc_engine/guest_profiler.cpp
    src/mc_engine/carbon_parts.cpp
    src/mc_engine/graphics_button.cpp
    src/mc_engine/pause_menu.cpp
    src/mc_engine/config_save.cpp
    src/mc_engine/ui_palette.cpp
    src/mc_engine/swf_dom.cpp
    src/mc_engine/swf_import.cpp
    src/mc_engine/ui_forms.cpp
    src/mc_engine/string_table.cpp
    src/mc_engine/string_dumper.cpp
    src/mc_engine/menu_camera.cpp
    src/mc_engine/map_mouse.cpp
    src/mc_engine/cursor_hide.cpp
    src/mc_engine/camera_look.cpp
    src/mc_engine/display_menu.cpp
    src/mc_engine/display_mode.cpp
    src/mc_engine/ultrawide.cpp
    src/mc_engine/frame_generation_menu.cpp
    src/mc_engine/upscaling_menu.cpp
    src/mc_engine/keyboard_native.cpp
    src/mc_engine/time_of_day.cpp
    src/mc_engine/map_warp.cpp
    src/mc_engine/real_weather.cpp
    src/mc_engine/squash_stretch.cpp
    src/mc_engine/tire_offset.cpp
    src/mc_engine/hud_units.cpp
    src/mc_engine/adaptive_triggers.cpp
    src/mc_engine/impulse_triggers.cpp
    src/mc_engine/impulse_wgi.cpp
    src/mc_engine/wheel_slip.cpp
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
    src/mc_engine/online/session_limits.cpp
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
    src/bootprogress/larecomp_shader_build.cpp
    src/bootprogress/larecomp_pipeline_build.cpp
    src/mc_engine/ui/fonts/larecomp_embedded_fonts.cpp
    src/mc_engine/ui/imgui_theme.cpp
    src/mc_engine/ui/debug_overlay.cpp
    src/mc_engine/ui/controller_toast.cpp
    src/mc_engine/ui/pad_vendor.cpp
    src/mc_engine/restart.cpp
    src/mc_engine/ui/gamepad.cpp
    src/mc_engine/ui/settings.cpp
    src/mc_engine/ui/achievements.cpp
    src/mc_engine/ui/kerning.cpp
)
if(NOT WIN32)
    # The D3D12 native graphics runtime (native_gfx/d3d12 and the files that
    # drive it) is Windows-only; Linux renders through Plume. The guest-side
    # files Plume shares (native_gfx/guest, nocp, vertex_declaration, ...) stay,
    # with their DXGI enums coming from src/compat/linux.
    set(_larecomp_win_only
        src/native_gfx/native_gfx.cpp
        src/native_gfx/geometry.cpp
        src/native_gfx/telemetry.cpp
        src/native_gfx/guest/sampler_state.cpp)
    foreach(_f IN LISTS LARECOMP_SOURCES)
        if(_f MATCHES "^src/native_gfx/d3d12/")
            list(APPEND _larecomp_win_only "${_f}")
        endif()
    endforeach()
    list(REMOVE_ITEM LARECOMP_SOURCES ${_larecomp_win_only})
    list(APPEND LARECOMP_SOURCES src/compat/linux/linux_stubs.cpp)
    # The generated code and the SDK headers read and write guest memory through
    # typed pointers; clang -O2/-O3 on Linux reorders those accesses (measured: an
    # .fxc parsed to different bytes than on Windows). Aliasing off keeps them in order.
    string(APPEND CMAKE_CXX_FLAGS " -fno-strict-aliasing")
    include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src/compat/linux)
endif()
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
        # Not with MinGW: there CMake drives windres (llvm-windres in llvm-mingw, which
        # preprocesses with Clang and calls llvm-rc itself), and llvm-rc alone does not
        # take windres' command line (-O coff).
        if(NOT CMAKE_RC_COMPILER AND NOT MINGW)
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

    if(NOT WIN32 AND NOT APPLE)
        # The ISO installer and the save importer open GTK file dialogs.
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)
        target_link_libraries(larecomp PRIVATE PkgConfig::GTK3)
    endif()

    # The Plume renderer, src/gpu.
    # Every backend the platform has is built in -- D3D12 and Vulkan on Windows,
    # Vulkan elsewhere -- and the SDK's graphics_backend cvar picks one at
    # startup; the host shaders are compiled once per
    # backend format. Plume itself is the submodule thirdparty/plume: the
    # project's fork (mzzvxm/plume, branch larecomp) of the zolaware revision
    # reblue ships (SDL3 Vulkan path, fill mode, R11G11B10/D24S8), plus the
    # teardown leak fixes; the host shaders are compiled by
    # the dxc of the submodule thirdparty/dxc-bin (renderbag, the revision reblue
    # and reeot use). On Linux it renders through Vulkan.
    option(LARECOMP_GPU "Build the Plume renderer (src/gpu)" ON)
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
        # AMD's FidelityFX SDK (2.4.0 or later, the folder that holds Kits/): the headers the temporal
        # upscaler's backend (src/gpu/temporal_ffx.h) is compiled with, and the signed DLLs and the
        # licence copied next to the exe below. By default the pinned 2.4.0 files are fetched into the
        # build folder and checked (cmake/FetchFidelityFX.cmake); LARECOMP_FFX_SDK_DIR names a local
        # SDK instead, LARECOMP_FFX_FETCH=OFF with it unset builds without FSR. Read-only: nothing
        # from the SDK is built or run here.
        set(LARECOMP_FFX_SDK_DIR "" CACHE PATH
            "AMD FidelityFX SDK (the folder holding Kits/FidelityFX); empty builds without FSR")
        option(LARECOMP_FFX_FETCH "Fetch the pinned FidelityFX SDK 2.4.0 files when LARECOMP_FFX_SDK_DIR is unset" ON)
        # The fetch runs at every configure (it only hashes what is there), so a new pin or a new file
        # reaches a build folder that was configured before. A fetched folder that an earlier configure
        # left in the cache counts as unset.
        string(FIND "${LARECOMP_FFX_SDK_DIR}" "${CMAKE_BINARY_DIR}/_deps/" _larecomp_ffx_fetched_before)
        if(_larecomp_ffx_fetched_before EQUAL 0)
            set(LARECOMP_FFX_SDK_DIR "" CACHE PATH
                "AMD FidelityFX SDK (the folder holding Kits/FidelityFX); empty builds without FSR" FORCE)
        endif()
        if(NOT LARECOMP_FFX_SDK_DIR AND LARECOMP_FFX_FETCH AND WIN32)
            include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/FetchFidelityFX.cmake")
        endif()
        set(LARECOMP_FFX_KIT "")
        if(LARECOMP_FFX_SDK_DIR AND WIN32)
            foreach(_larecomp_ffx_candidate "${LARECOMP_FFX_SDK_DIR}/Kits/FidelityFX"
                                            "${LARECOMP_FFX_SDK_DIR}")
                if(EXISTS "${_larecomp_ffx_candidate}/api/include/ffx_api.h" AND
                   EXISTS "${_larecomp_ffx_candidate}/upscalers/include/ffx_upscale.h")
                    set(LARECOMP_FFX_KIT "${_larecomp_ffx_candidate}")
                    break()
                endif()
            endforeach()
            if(NOT LARECOMP_FFX_KIT)
                message(WARNING "LARECOMP_FFX_SDK_DIR=${LARECOMP_FFX_SDK_DIR} holds no "
                                "Kits/FidelityFX/api/include/ffx_api.h: building without FSR")
            else()
                message(STATUS "larecomp: FidelityFX upscalers from ${LARECOMP_FFX_KIT}")
                # Frame generation (src/gpu/frame_generation_ffx.h) needs the same SDK's frame generation
                # header: a local checkout without it builds without frame generation.
                if(EXISTS "${LARECOMP_FFX_KIT}/framegeneration/include/ffx_framegeneration.h")
                    set(LARECOMP_FFX_HAS_FRAMEGENERATION ON)
                    message(STATUS "larecomp: FidelityFX frame generation from ${LARECOMP_FFX_KIT}")
                else()
                    set(LARECOMP_FFX_HAS_FRAMEGENERATION OFF)
                    message(STATUS "larecomp: no FidelityFX frame generation header in ${LARECOMP_FFX_KIT}: "
                                    "building without frame generation")
                endif()
            endif()
        endif()
        # NVIDIA's Streamline SDK (2.14.16, the folder that holds include/ and bin/x64/): the headers
        # the DLSS backend (src/gpu/temporal_dlss.h) is compiled with, and the production DLLs and
        # licence files copied next to the exe below. Nothing of it is committed: by default the
        # pinned release is fetched into the build folder and every file checked
        # (cmake/FetchStreamline.cmake); LARECOMP_STREAMLINE_DIR names an unpacked SDK instead,
        # LARECOMP_STREAMLINE_ZIP a local copy of the release zip, LARECOMP_DLSS_FETCH=OFF with
        # neither builds without DLSS. Nothing is linked: the DLL is opened at run time.
        set(LARECOMP_STREAMLINE_DIR "" CACHE PATH
            "NVIDIA Streamline SDK (the folder holding include/ and bin/x64/); empty builds without DLSS")
        set(LARECOMP_STREAMLINE_ZIP "" CACHE FILEPATH
            "A local copy of streamline-sdk-v2.14.16.zip, used instead of the download")
        option(LARECOMP_DLSS_FETCH "Fetch the pinned Streamline SDK 2.14.16 files when LARECOMP_STREAMLINE_DIR is unset" ON)
        # As FidelityFX's above: checked at every configure; a fetched folder left in the cache counts as unset.
        string(FIND "${LARECOMP_STREAMLINE_DIR}" "${CMAKE_BINARY_DIR}/_deps/" _larecomp_sl_fetched_before)
        if(_larecomp_sl_fetched_before EQUAL 0)
            set(LARECOMP_STREAMLINE_DIR "" CACHE PATH
                "NVIDIA Streamline SDK (the folder holding include/ and bin/x64/); empty builds without DLSS" FORCE)
        endif()
        if(NOT LARECOMP_STREAMLINE_DIR AND LARECOMP_DLSS_FETCH AND WIN32)
            include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/FetchStreamline.cmake")
        endif()
        set(LARECOMP_STREAMLINE_KIT "")
        if(LARECOMP_STREAMLINE_DIR AND WIN32)
            if(EXISTS "${LARECOMP_STREAMLINE_DIR}/include/sl.h" AND
               EXISTS "${LARECOMP_STREAMLINE_DIR}/include/sl_dlss.h" AND
               EXISTS "${LARECOMP_STREAMLINE_DIR}/bin/x64/sl.interposer.dll")
                set(LARECOMP_STREAMLINE_KIT "${LARECOMP_STREAMLINE_DIR}")
                message(STATUS "larecomp: DLSS (Streamline) from ${LARECOMP_STREAMLINE_KIT}")
            else()
                message(WARNING "LARECOMP_STREAMLINE_DIR=${LARECOMP_STREAMLINE_DIR} holds no include/sl.h, "
                                "include/sl_dlss.h and bin/x64/sl.interposer.dll: building without DLSS")
            endif()
        endif()
        # DLSS Frame Generation (src/gpu/dlss_frame_generation.h) needs the headers of the frame generation,
        # Reflex and PC latency plugins and their DLLs: an unpacked SDK folder without them builds DLSS Super
        # Resolution only (the fetched files above always carry them).
        set(LARECOMP_STREAMLINE_HAS_FRAMEGEN OFF)
        if(LARECOMP_STREAMLINE_KIT)
            set(_larecomp_sl_fg_ok ON)
            foreach(_larecomp_sl_fg_file include/sl_dlss_g.h include/sl_reflex.h include/sl_pcl.h
                    bin/x64/sl.dlss_g.dll bin/x64/nvngx_dlssg.dll bin/x64/sl.reflex.dll bin/x64/sl.pcl.dll)
                if(NOT EXISTS "${LARECOMP_STREAMLINE_KIT}/${_larecomp_sl_fg_file}")
                    set(_larecomp_sl_fg_ok OFF)
                endif()
            endforeach()
            if(_larecomp_sl_fg_ok)
                set(LARECOMP_STREAMLINE_HAS_FRAMEGEN ON)
                message(STATUS "larecomp: DLSS frame generation (Streamline sl.dlss_g, sl.reflex, sl.pcl)")
            else()
                message(STATUS "larecomp: the Streamline folder lacks the frame generation, Reflex or PC latency "
                                "files: building DLSS without frame generation")
            endif()
            unset(_larecomp_sl_fg_ok)
        endif()
        add_subdirectory(src/gpu)
        # The renderer's objects, and plume's archive without plume's include
        # directories in larecomp's own translation units (CMP0131 is NEW with
        # cmake_minimum_required 3.25). The rest of larecomp sees only gpu.h.
        target_link_libraries(larecomp PRIVATE larecomp_gpu $<LINK_ONLY:plume>)
        target_compile_definitions(larecomp PRIVATE LARECOMP_HAS_GPU=1)
        message(STATUS "larecomp: Plume renderer (src/gpu) built for ${LARECOMP_GPU_BACKENDS}")
    endif()

    # The shader pack builder (src/shaderpack): builds the packs the renderer reads from the
    # player's own game files. The shader translator is LAR-XenosRecomp, thirdparty/LAR-XenosRecomp
    # or the checkout LARECOMP_XENOSRECOMP_DIR names. tools/shaderpack_tool is its command line,
    # built on request:  cmake --build <dir> --target larecomp_shaderpack_tool
    add_subdirectory(src/shaderpack)
    add_subdirectory(tools/shaderpack_tool EXCLUDE_FROM_ALL)
    target_link_libraries(larecomp PRIVATE larecomp_shaderpack)

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
            # audio mixer thread priority (mc_engine/hooks/audio_mixer.cpp): MMCSS
            avrt
            # custom MP3 radio (mc_engine/mp3custom): Media Foundation decode.
            # XAudio2 itself is loaded by name at runtime, so it needs no lib.
            mfplat
            mfreadwrite
            mfuuid
            ole32
            # Rate My Ride client sockets (mc_engine/online/rate_my_ride)
            ws2_32
        )
        if(MINGW)
            # What the sources ask for with #pragma comment(lib, ...) (crash_handler.cpp,
            # guest_profiler.cpp, adaptive_triggers.cpp, identity_store.cpp, identity_ui.cpp),
            # which Clang for MinGW does not turn into a link request, and what MSVC's
            # default libraries cover (the ones llvm-mingw's driver does not: winmm for
            # timeBeginPeriod, dxguid for the DirectInput GUID_* of the wheel code, ntdll).
            # advapi32, shell32, user32 and kernel32 are the driver's own defaults.
            target_link_libraries(larecomp
                PRIVATE
                dbghelp
                setupapi
                hid
                shlwapi
                crypt32
                comdlg32
                winmm
                dxguid
                ntdll
                uuid
            )
            # The line tables of the generated code (-gline-tables-only, from
            # generated/rexglue.cmake) are 37 MB of the executable: split into
            # larecomp.exe.debug beside it, as the MCLA fork does for mcla.exe.
            # Nothing reads them at run time; llvm-symbolizer does, with the exe's address.
            get_filename_component(_larecomp_compiler_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
            find_program(LARECOMP_OBJCOPY NAMES llvm-objcopy objcopy HINTS "${_larecomp_compiler_dir}")
            if(LARECOMP_OBJCOPY)
                add_custom_command(TARGET larecomp POST_BUILD
                    COMMAND "${LARECOMP_OBJCOPY}" --only-keep-debug "$<TARGET_FILE:larecomp>" "$<TARGET_FILE:larecomp>.debug"
                    COMMAND "${LARECOMP_OBJCOPY}" --strip-all "$<TARGET_FILE:larecomp>"
                    COMMAND "${LARECOMP_OBJCOPY}" "--add-gnu-debuglink=$<TARGET_FILE:larecomp>.debug" "$<TARGET_FILE:larecomp>"
                    COMMENT "Splitting the debug information off larecomp.exe"
                    VERBATIM)
            else()
                message(WARNING "llvm-objcopy was not found: larecomp.exe keeps its debug information")
            endif()
        endif()
    endif()

    # The shared Rate My Ride server players reach on TCP 31002. Standalone: the
    # store and protocol sources only, no SDK.
    add_executable(larecomp_rmr_server
        src/mc_engine/online/rate_my_ride/rmr_server_main.cpp
        src/mc_engine/online/rate_my_ride/rmr_protocol.cpp
        src/mc_engine/online/rate_my_ride/rmr_store.cpp
        src/mc_engine/online/rate_my_ride/rmr_net.cpp
        src/mc_engine/online/leaderboards/lb_protocol.cpp
        src/mc_engine/online/leaderboards/lb_store.cpp
    )
    if(WIN32)
        target_compile_definitions(larecomp_rmr_server PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_link_libraries(larecomp_rmr_server PRIVATE ws2_32)
    endif()

    # The online relay (UDP 31000) anyone can host, and the master server
    # (UDP+TCP 31003) that lists those relays in the in-game server browser.
    # Standalone, no SDK.
    add_executable(larecomp_relay_server
        src/mc_engine/online/relay_server/relay_server.cpp
        thirdparty/monocypher/monocypher.c
    )
    add_executable(larecomp_master_server
        src/mc_engine/online/master_server/master_server_main.cpp
        src/mc_engine/online/master_server/identity_registry.cpp
        src/mc_engine/online/rate_my_ride/rmr_net.cpp
        src/mc_engine/online/rate_my_ride/rmr_protocol.cpp
        thirdparty/monocypher/monocypher.c
    )
    if(WIN32)
        target_link_libraries(larecomp_relay_server PRIVATE ws2_32)
        target_compile_definitions(larecomp_master_server PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_link_libraries(larecomp_master_server PRIVATE ws2_32)
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

    # The starter pipeline list, beside the shader pack (src/gpu/pipelines.h).
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/mcla_pipelines.bin")
        add_custom_command(TARGET larecomp POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:larecomp>/assets"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CMAKE_CURRENT_SOURCE_DIR}/assets/mcla_pipelines.bin"
                    "$<TARGET_FILE_DIR:larecomp>/assets/mcla_pipelines.bin"
            COMMENT "Copying the starter pipeline list next to the shader pack")
    endif()

    # The FidelityFX runtime (when LARECOMP_FFX_SDK_DIR is set): the loader and the upscaler DLL of each
    # API the build has, AMD-signed and unchanged, and the licence that must travel with them (its
    # redistribution terms: binary form only, the notice reproduced). The renderer opens them by full
    # path from this folder and nowhere else.
    if(LARECOMP_FFX_KIT)
        set(_larecomp_ffx_dlls "")
        if("D3D12" IN_LIST LARECOMP_GPU_BACKENDS)
            list(APPEND _larecomp_ffx_dlls amd_fidelityfx_loader_dx12.dll amd_fidelityfx_upscaler_dx12.dll)
        endif()
        if("VULKAN" IN_LIST LARECOMP_GPU_BACKENDS)
            list(APPEND _larecomp_ffx_dlls amd_fidelityfx_loader_vk.dll amd_fidelityfx_upscaler_vk.dll)
        endif()
        # The frame generation effect DLLs (the loader opens them by name from this folder).
        if(LARECOMP_FFX_HAS_FRAMEGENERATION)
            if("D3D12" IN_LIST LARECOMP_GPU_BACKENDS)
                list(APPEND _larecomp_ffx_dlls amd_fidelityfx_framegeneration_dx12.dll)
            endif()
            if("VULKAN" IN_LIST LARECOMP_GPU_BACKENDS)
                list(APPEND _larecomp_ffx_dlls amd_fidelityfx_framegeneration_vk.dll)
            endif()
        endif()
        foreach(_larecomp_ffx_dll IN LISTS _larecomp_ffx_dlls)
            if(NOT EXISTS "${LARECOMP_FFX_KIT}/signedbin/${_larecomp_ffx_dll}")
                message(WARNING "FidelityFX: ${LARECOMP_FFX_KIT}/signedbin/${_larecomp_ffx_dll} is missing")
                continue()
            endif()
            add_custom_command(TARGET larecomp POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${LARECOMP_FFX_KIT}/signedbin/${_larecomp_ffx_dll}"
                        "$<TARGET_FILE_DIR:larecomp>/${_larecomp_ffx_dll}"
                COMMENT "Copying ${_larecomp_ffx_dll} next to larecomp.exe")
        endforeach()
        if(EXISTS "${LARECOMP_FFX_KIT}/docs/license.md")
            add_custom_command(TARGET larecomp POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${LARECOMP_FFX_KIT}/docs/license.md"
                        "$<TARGET_FILE_DIR:larecomp>/amd_fidelityfx_license.md"
                COMMENT "Copying the FidelityFX licence next to larecomp.exe")
        endif()
    endif()

    # NVIDIA Streamline and DLSS (when LARECOMP_STREAMLINE_DIR is set): the production DLLs of Super
    # Resolution, NVIDIA-signed and unchanged, and the licence texts that go with them. The renderer
    # opens them by full path from this folder after checking their signature, and nowhere else.
    # NvLowLatencyVk.dll is the Vulkan Reflex runtime that sl.common.dll loads by itself on Vulkan.
    if(LARECOMP_STREAMLINE_KIT)
        set(_larecomp_sl_dlls sl.interposer.dll sl.common.dll sl.dlss.dll nvngx_dlss.dll NvLowLatencyVk.dll)
        if(LARECOMP_STREAMLINE_HAS_FRAMEGEN)
            # Frame generation and the two plugins it requires (Reflex and PC latency through Streamline).
            list(APPEND _larecomp_sl_dlls sl.dlss_g.dll nvngx_dlssg.dll sl.reflex.dll sl.pcl.dll)
        endif()
        foreach(_larecomp_sl_dll IN LISTS _larecomp_sl_dlls)
            if(NOT EXISTS "${LARECOMP_STREAMLINE_KIT}/bin/x64/${_larecomp_sl_dll}")
                message(WARNING "Streamline: ${LARECOMP_STREAMLINE_KIT}/bin/x64/${_larecomp_sl_dll} is missing")
                continue()
            endif()
            add_custom_command(TARGET larecomp POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${LARECOMP_STREAMLINE_KIT}/bin/x64/${_larecomp_sl_dll}"
                        "$<TARGET_FILE_DIR:larecomp>/${_larecomp_sl_dll}"
                COMMENT "Copying ${_larecomp_sl_dll} next to larecomp.exe")
        endforeach()
        foreach(_larecomp_sl_notice
                "bin/x64/nvngx_dlss.license.txt|nvngx_dlss_license.txt"
                "bin/x64/reflex.license.txt|reflex_license.txt"
                "license.txt|streamline_license.txt"
                "3rd-party-licenses.md|streamline_third_party_licenses.md")
            string(REPLACE "|" ";" _larecomp_sl_pair "${_larecomp_sl_notice}")
            list(GET _larecomp_sl_pair 0 _larecomp_sl_from)
            list(GET _larecomp_sl_pair 1 _larecomp_sl_to)
            if(EXISTS "${LARECOMP_STREAMLINE_KIT}/${_larecomp_sl_from}")
                add_custom_command(TARGET larecomp POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                            "${LARECOMP_STREAMLINE_KIT}/${_larecomp_sl_from}"
                            "$<TARGET_FILE_DIR:larecomp>/${_larecomp_sl_to}"
                    COMMENT "Copying ${_larecomp_sl_to} next to larecomp.exe")
            endif()
        endforeach()
    endif()

    if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/D3D12")
        add_custom_command(TARGET larecomp POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${CMAKE_CURRENT_SOURCE_DIR}/assets/D3D12"
                    "$<TARGET_FILE_DIR:larecomp>/D3D12"
            COMMENT "Copying Agility SDK runtime binaries next to larecomp.exe")
    endif()

    if(NOT WIN32 AND NOT APPLE)
        # Linux. What a Windows executable carries inside it or in assets/D3D12 sits next to the
        # executable instead.
        #
        # The shader compiler the shader pack builder loads, from <exe folder>/D3D12/ as on Windows
        # (bootprogress/larecomp_shader_build.cpp): libdxcompiler.so, fetched and checked by
        # cmake/FetchDxcLinux.cmake, with the licences that travel with it (the Windows DLLs of
        # assets/D3D12 are not copied here).
        include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/FetchDxcLinux.cmake")
        if(LARECOMP_DXC_LINUX_LIB)
            add_custom_command(TARGET larecomp POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:larecomp>/D3D12"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${LARECOMP_DXC_LINUX_LIB}"
                        "$<TARGET_FILE_DIR:larecomp>/D3D12/libdxcompiler.so"
                COMMENT "Copying libdxcompiler.so next to larecomp")
            if(LARECOMP_DXC_LINUX_LICENCE)
                add_custom_command(TARGET larecomp POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                            "${LARECOMP_DXC_LINUX_LICENCE}"
                            "$<TARGET_FILE_DIR:larecomp>/D3D12/LICENSE-LLVM.txt"
                    COMMENT "Copying the DXC licence next to libdxcompiler.so")
            endif()
            if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/D3D12/LICENCE-MIT.txt")
                add_custom_command(TARGET larecomp POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                            "${CMAKE_CURRENT_SOURCE_DIR}/assets/D3D12/LICENCE-MIT.txt"
                            "$<TARGET_FILE_DIR:larecomp>/D3D12/LICENCE-MIT.txt"
                    COMMENT "Copying the DXC MIT licence next to libdxcompiler.so")
            endif()
        endif()

        # Key caps (src/keycaps: RCDATA in the Windows executable) and the achievements' labels
        # (src/achievements.toml: ACHIEVEMENTS_TOML in larecomp.rc), read from files here
        # (mc_engine/keyboard_native.cpp ReadCapBytes, larecomp_app.h OnPostLoadXexImage).
        add_custom_command(TARGET larecomp POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:larecomp>/assets"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CMAKE_CURRENT_SOURCE_DIR}/src/achievements.toml"
                    "$<TARGET_FILE_DIR:larecomp>/assets/achievements.toml"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${CMAKE_CURRENT_SOURCE_DIR}/src/keycaps"
                    "$<TARGET_FILE_DIR:larecomp>/keycaps"
            COMMENT "Copying the key caps and the achievements file next to larecomp")
    endif()
endif()
