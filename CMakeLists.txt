
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
    add_executable(larecomp ${LARECOMP_SOURCES})
    if(WIN32)
        # Compile the .rc so the app icon is embedded in the exe (not just copied
        # into assets/). The lowest-ID ICON in larecomp.rc is used by Explorer.
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

        target_link_libraries(larecomp
            PRIVATE
            dxgi
            # real-world weather fetch (mc_engine/real_weather.cpp)
            winhttp
        )
    endif()

    add_custom_command(TARGET larecomp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/src/assets"
        "$<TARGET_FILE_DIR:larecomp>/assets"
    )
endif()
