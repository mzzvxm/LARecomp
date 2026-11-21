
# larecomp - ReXGlue Recompiled Project
#
# This file is yours to edit. 'rexglue migrate' will NOT overwrite it.
# SDK boilerplate (rexglue_project, rexglue_setup_target, the host settings) comes from the rexglue package.

cmake_minimum_required(VERSION 3.25)
project(larecomp LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# rexglue 0.11: the SDK is a package (find_package) or, for development, a source folder built with the project.
#   -DREXSDK_DIR=<LAR-RXSDK checkout>   add_subdirectory (builds the SDK here)
#   -DCMAKE_PREFIX_PATH=<install>       an installed SDK (the launcher's prebuilt kit: tools/prebuilt-local)
# REXGLUE_REQUIRED_GRAPHICS_BACKENDS is gone (D3D12 on Windows and Vulkan are always built); the Xenia GPU drivers are
# modules the package stages next to the exe (rexgpu-xenos-d3d12, -vulkan, -null).
# A project that still has generated/rexglue.cmake keeps working with a deprecation warning: delete the file.
if(REXSDK_DIR)
    add_subdirectory("${REXSDK_DIR}" rexglue-sdk)
else()
    find_package(rexglue 0.11.0 REQUIRED CONFIG)
endif()
# The codegen output folder stays `generated` (larecomp_manifest.toml out_directory_path), not generated/default.
rexglue_project(larecomp OUTPUT_DIR generated)

# CPU level of the generated code and of larecomp's own sources (applied at the end of this file).
#   baseline   what the SDK builds for: x86-64 with SSE4.1, no -march flag from here. The default.
#   x86-64-v3  adds -march=x86-64-v3: AVX2, FMA, BMI1/BMI2, MOVBE, LZCNT, F16C (Intel Haswell and
#              later, AMD Excavator and Zen, the Steam Deck). The guest code gains from MOVBE (every
#              guest load and store byte-swaps), BMI2 and LZCNT (rlwinm, cntlzw) and inline FMA
#              (std::fma is a call into the C runtime at baseline). The executable then refuses to
#              start on a CPU without these (src/cpu_level.cpp) and a build for another PC has to
#              be made with the launcher's Repair.
# The level is chosen when the build folder is configured; keep -march out of CMAKE_CXX_FLAGS.
set(LARECOMP_CPU_LEVEL "baseline" CACHE STRING "CPU level to compile larecomp for: baseline or x86-64-v3")
set_property(CACHE LARECOMP_CPU_LEVEL PROPERTY STRINGS baseline x86-64-v3)
if(NOT LARECOMP_CPU_LEVEL STREQUAL "baseline" AND NOT LARECOMP_CPU_LEVEL STREQUAL "x86-64-v3")
    message(FATAL_ERROR "LARECOMP_CPU_LEVEL is '${LARECOMP_CPU_LEVEL}': it has to be baseline or x86-64-v3")
endif()
if(LARECOMP_CPU_LEVEL STREQUAL "x86-64-v3")
    if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
        message(FATAL_ERROR "LARECOMP_CPU_LEVEL=x86-64-v3 is for x86-64 targets, not ${CMAKE_SYSTEM_PROCESSOR}")
    endif()
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        message(FATAL_ERROR "LARECOMP_CPU_LEVEL=x86-64-v3 needs Clang (or GCC), not ${CMAKE_CXX_COMPILER_ID}")
    endif()
endif()

# Developer tools: code for the people who work on the game, never in a release. OFF by default: with it off there is no key and no
# code of them in the build (the compile definition LARECOMP_DEV_TOOLS is not set; the compare keys' two settings stay as debug-only
# rows that nothing reads). Configure with
#   -DLARECOMP_DEV_TOOLS=ON
# for a test or play build. It is set for every target of this project (the renderer reads one of the tools' state too).
#   the compare keys (src/mc_engine/display/effect_compare.cpp): F1 turns the selected graphics effect off and on to see what it does,
#   F12 selects the next effect (bounce light, contact shadows, sun shafts, ambient occlusion, shadow filter, self-reflections).
#   the SDK's console (rexglue 0.11 opens its debug console on the grave accent key, ui.bind.console, in every program): without the
#   option LarecompApp::StartOverlays locks that key to "no key", so no player can open it.
option(LARECOMP_DEV_TOOLS "Developer tools: the compare keys (F1 flips an effect, F12 picks the effect) and the SDK's console key (grave accent). Not for a release build" OFF)
if(LARECOMP_DEV_TOOLS)
    add_compile_definitions(LARECOMP_DEV_TOOLS=1)
endif()

if(MINGW)
    # llvm-mingw (Clang with lld, libc++ and mingw-w64 on the UCRT): the build for a PC
    # without Visual Studio. The SDK has to be built with the same toolchain (libc++ and the
    # Microsoft STL do not mix). __try/__except (mc_engine/camera/camera_look.cpp) needs
    # -fms-extensions, and mingw-w64 assumes Windows 7 (_WIN32_WINNT 0x601) where the
    # Windows SDK assumes the current Windows. Both as the SDK sets them for itself.
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fms-extensions>)
    add_compile_definitions(_WIN32_WINNT=0x0A00)
    # No -fasync-exceptions for the generated code: for a MinGW target the Clang driver drops
    # it ("argument unused", on every file), and passed to the compiler itself it has crashed
    # Clang in SimplifyCFG. A fault inside a guest __try scope is therefore not caught with
    # this toolchain; the generated code of this game has no such scope. The sources carry
    # the list as it was when the file was included, so it is set again.
    # CHECK (0.11): rexglue_setup_target applies ${REXGLUE_RECOMP_OPTIONS} to the larecomp_recompiled object library itself
    # (target_compile_options), so removing the option from the variable here, before it, is all that is needed. Does the
    # 0.11 option list still carry -fasync-exceptions for MinGW (rexglue_recomp_options: "always on Windows")?
    list(REMOVE_ITEM REXGLUE_RECOMP_OPTIONS -fasync-exceptions)
endif()

# Sources
# Monocypher (nick keys and tickets, thirdparty/monocypher) is C99 that also
# compiles as C++; the project only enables CXX.
set_source_files_properties(thirdparty/monocypher/monocypher.c PROPERTIES LANGUAGE CXX)

set(LARECOMP_SOURCES
    src/main.cpp
    src/larecomp_app.cpp
    src/spdlog_console.cpp
    # LARecomp's side of the rexglue settings: the owners, the aliases of the old names, the by-name API.
    src/settings/larecomp_settings.cpp
    src/crash_handler.cpp
    # The named object a running game holds, so the launcher can tell the game runs without listing processes.
    src/game_marker.cpp

    src/mc_engine/core/threading.cpp
    src/mc_engine/core/fma_fast.cpp

    # Thread priorities for every OS (os/thread_priority.h). The shader pack library and its tool
    # expect this file from whoever links them.
    src/os/thread_priority.cpp
    # The launcher's AppImage variables are removed from the game's environment at start (os/appimage_env.h).
    src/os/appimage_env.cpp
    # The guest's Sleep on Linux: a yield and a sleep that signals do not cut short (os/thread_yield.h).
    src/os/thread_yield.cpp

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
    src/mc_engine/hooks/frame_pacing.cpp
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
    src/mc_engine/hooks/lowcpu_probe.cpp
    src/mc_engine/hooks/job_workers.cpp
    src/mc_engine/hooks/script_aes.cpp
    src/mc_engine/hooks/diagnostics.cpp
    src/mc_engine/hooks/vibration.cpp
    src/mc_engine/hooks/stick_deadzone.cpp
    src/mc_engine/hooks/warper.cpp
    src/mc_engine/hooks/photo_album.cpp
    src/mc_engine/hooks/photo_export.cpp
    src/mc_engine/hooks/rear_view.cpp
    src/mc_engine/hooks/traffic_shadow.cpp
    src/mc_engine/hooks/traffic_probe.cpp
    src/mc_engine/hooks/headlight_points.cpp
    src/mc_engine/hooks/road_reflection_cars_test.cpp
    src/mc_engine/hooks/script_input.cpp
    src/mc_engine/hooks/checkpoint_sweep.cpp
    src/mc_engine/hooks/neon_pulse.cpp
    src/mc_engine/hooks/inflate_fast.cpp
    src/mc_engine/hooks/vol_lamps.cpp
    src/mc_engine/hooks/lzx_native.cpp

    src/mc_engine/core/guest_profiler.cpp
    src/mc_engine/gameplay/carbon_parts.cpp
    src/mc_engine/menus/pause_menu.cpp
    src/mc_engine/core/config_save.cpp
    src/mc_engine/menus/debug_menu.cpp
    src/mc_engine/menus/debug_menu_pages.cpp
    src/mc_engine/menus/ui_palette.cpp
    src/mc_engine/menus/swf_dom.cpp
    src/mc_engine/menus/swf_import.cpp
    src/mc_engine/menus/ui_forms.cpp
    src/mc_engine/menus/string_table.cpp
    src/mc_engine/core/string_dumper.cpp
    src/mc_engine/camera/menu_camera.cpp
    src/mc_engine/input/map_mouse.cpp
    src/mc_engine/input/menu_mouse.cpp
    src/mc_engine/input/cursor_hide.cpp
    src/mc_engine/input/input_state.cpp
    src/mc_engine/input/mouse_look.cpp
    src/mc_engine/camera/camera_look.cpp
    src/mc_engine/camera/camera_trace.cpp
    src/mc_engine/menus/display_menu.cpp
    src/mc_engine/display/display_mode.cpp
    src/mc_engine/display/window_icon.cpp
    src/mc_engine/display/ultrawide.cpp
    src/mc_engine/menus/frame_generation_menu.cpp
    src/mc_engine/menus/upscaling_menu.cpp
    src/mc_engine/keyboard_native.cpp
    src/mc_engine/gameplay/time_of_day.cpp

    # Trailer toolkit capture mode: the fixed step, the frame gate and the scene clock (maths in capture_core), and the take folder (names, memory plan,
    # host ring, take.json in take_folder). Inert unless a scene's capture key or one of the debug cvars mcla_capture_fps / mcla_fixed_step_fps asks.
    src/mc_engine/cinematic/capture_core.cpp
    src/mc_engine/cinematic/take_folder.cpp
    src/mc_engine/cinematic/capture_mode.cpp

    # Trailer toolkit M2c: the take's sound. On rexglue 0.11 a take holds the tap in front of the output device (music/audio_tap.h, made by audio_output.h),
    # pulls the SDK's mix itself and gates the game's audio callback (hooks/audio_mixer.cpp); the sample books, the session and the WAV file are in capture_audio_core.
    src/mc_engine/cinematic/capture_audio_core.cpp
    src/mc_engine/cinematic/capture_audio.cpp

    # Trailer toolkit: the scene file (parser, timeline, clock), its runner and the script native call shim.
    # Inert unless the debug cvar mcla_scene names a scene file.
    src/mc_engine/cinematic/scene.cpp
    src/mc_engine/cinematic/spots.cpp
    src/mc_engine/cinematic/state_log_core.cpp
    src/mc_engine/cinematic/state_log.cpp
    src/mc_engine/cinematic/scene_runtime.cpp
    src/mc_engine/cinematic/camera.cpp
    src/mc_engine/cinematic/camera_runtime.cpp
    src/mc_engine/cinematic/freecam.cpp
    src/mc_engine/cinematic/freecam_runtime.cpp
    src/mc_engine/cinematic/native_call.cpp
    # ... and the scene's cars: spawn / delete / the player's garage car through the natives, the registry of poses for the camera.
    src/mc_engine/cinematic/car_registry.cpp
    src/mc_engine/cinematic/path_follow.cpp
    src/mc_engine/cinematic/car_spawn.cpp
    src/mc_engine/cinematic/ambient_car.cpp
    src/mc_engine/cinematic/cars.cpp
    src/mc_engine/cinematic/scene_seed.cpp
    # The Director's world knowledge (package C): the atlas of lamps, roads and lanes, garages and cars decoded from guest memory (atlas_core, pure), its
    # JSON answers (atlas_answers: empty until the command layer's director.h is in the tree) and the `atlas.dump` / `query.*` commands (handlers_world).
    src/mc_engine/cinematic/director/atlas_core.cpp
    src/mc_engine/cinematic/director/atlas_answers.cpp
    src/mc_engine/cinematic/director/handlers_world.cpp
    # The scene's `split` track on the renderer's before/after split (trailer toolkit M5b).
    src/mc_engine/cinematic/split_runtime.cpp

    # The Director (feature-parity/DIRECTOR-DESIGN.md; documentation/DIRECTOR-PROTOCOL.md): one command layer under the panel, the scene file and an agent's requests.
    # Inert unless the debug setting mcla_director is on. The pure files (json, command, line_server, scene_edit, director_math, png_writer) have larecomp_director_test
    # and larecomp_scene_edit_test; director_runtime and the handlers_* files are the game side. Package C's world queries (handlers_world.cpp) are added below.
    src/mc_engine/cinematic/director/json.cpp
    src/mc_engine/cinematic/director/command.cpp
    src/mc_engine/cinematic/director/line_server.cpp
    src/mc_engine/cinematic/director/scene_edit.cpp
    src/mc_engine/cinematic/director/director_math.cpp
    src/mc_engine/cinematic/director/png_writer.cpp
    src/mc_engine/cinematic/director/director_runtime.cpp
    src/mc_engine/cinematic/director/handlers_transport.cpp
    src/mc_engine/cinematic/director/handlers_scene.cpp
    src/mc_engine/cinematic/director/handlers_camera.cpp
    src/mc_engine/cinematic/director/handlers_cars.cpp
    src/mc_engine/cinematic/director/handlers_capture.cpp
    # the Director panel's own needs: path.set (a route written again) and the camera's path in the snapshot
    src/mc_engine/cinematic/director/handlers_panel.cpp

    src/mc_engine/gameplay/map_warp.cpp
    src/mc_engine/gameplay/real_weather.cpp
    src/mc_engine/gameplay/squash_stretch.cpp
    src/mc_engine/gameplay/tire_offset.cpp
    src/mc_engine/gameplay/hud_units.cpp
    src/mc_engine/input/adaptive_triggers.cpp
    src/mc_engine/input/device_watch.cpp
    src/mc_engine/input/impulse_triggers.cpp
    src/mc_engine/input/impulse_wgi.cpp
    src/mc_engine/gameplay/wheel_slip.cpp
    src/mc_engine/gameplay/cutscene_gallery.cpp
    src/mc_engine/core/texture_dump.cpp
    src/mc_engine/core/boot_progress.cpp

    # Trailer toolkit: the game's own UI on and off (the HUD alpha, its scene track). Inert at alpha 1.
    src/mc_engine/cinematic/hud.cpp
    src/mc_engine/cinematic/hud_track.cpp
    src/mc_engine/cinematic/hud_runtime.cpp

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
    src/mc_engine/modloader/features/mod_bodykits.cpp

    # Online (split out of hooks.cpp): tournament / System Link / Xbox LIVE
    src/mc_engine/online/online_common.cpp
    src/mc_engine/online/tournament.cpp
    src/mc_engine/online/tournament_schedule.cpp
    src/mc_engine/online/tournament_awards.cpp
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
    # The no-command-processor renderer as an in-process Xenos driver (gpu.driver = larecomp).
    src/native_gfx/nocp/nocp_driver.cpp
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
    src/mc_engine/core/restart.cpp
    src/mc_engine/ui/gamepad.cpp
    src/mc_engine/ui/settings.cpp
    src/mc_engine/ui/achievements.cpp
    src/mc_engine/ui/kerning.cpp
    # The Director panel (feature-parity/DIRECTOR-DESIGN.md section 3): the ImGui dialog and its pure parts (the timeline, the world gizmos, the panel's logic; larecomp_director_panel_test).
    src/mc_engine/ui/director_panel.cpp
    src/mc_engine/ui/director_timeline.cpp
    src/mc_engine/ui/director_gizmos.cpp
    src/mc_engine/ui/director_panel_core.cpp
)
# The developer tools' sources are in the build only with -DLARECOMP_DEV_TOOLS=ON (see the option above).
if(LARECOMP_DEV_TOOLS)
    list(APPEND LARECOMP_SOURCES src/mc_engine/display/effect_compare.cpp)
endif()
# The HUD track's parser and evaluation (src/mc_engine/cinematic/hud_track.h) have a check that needs no game:
#   cmake --build <build dir> --target larecomp_hud_track_test, then run <build dir>/larecomp_hud_track_test (exit 0 = passed).
add_executable(larecomp_hud_track_test EXCLUDE_FROM_ALL
    src/mc_engine/cinematic/hud_track_test.cpp
    src/mc_engine/cinematic/hud_track.cpp)
target_include_directories(larecomp_hud_track_test PRIVATE src src/mc_engine/cinematic)
# The guard of the game's bound-node test (src/mc_engine/hooks/bound_node_guard.h: which nodes it must not read) has a check that needs no game:
#   cmake --build <build dir> --target larecomp_bound_node_guard_test, then run <build dir>/larecomp_bound_node_guard_test (exit 0 = passed).
add_executable(larecomp_bound_node_guard_test EXCLUDE_FROM_ALL
    src/mc_engine/hooks/bound_node_guard_test.cpp)
target_include_directories(larecomp_bound_node_guard_test PRIVATE src/mc_engine/hooks)
# The pure part of the pad reader of the dialogs (src/mc_engine/ui/pad_feed_core.h: merging pads, the dead zone, the back-off of empty
# XInput slots, when SDL's gamepad list is looked at again) has a check that needs no game:
#   cmake --build <build dir> --target larecomp_pad_feed_test, then run <build dir>/larecomp_pad_feed_test (exit 0 = passed).
add_executable(larecomp_pad_feed_test EXCLUDE_FROM_ALL
    src/mc_engine/ui/pad_feed_test.cpp)
target_include_directories(larecomp_pad_feed_test PRIVATE src src/mc_engine/ui)
# Which SDL the exe's own SDL code talks to (src/mc_engine/input/sdl_instance.h: the one-SDL rule, the wheel thread's two gates, the
# SDL_GAMECONTROLLER_IGNORE_DEVICES list and its command-line reader) has a check that needs no game:
#   cmake --build <build dir> --target larecomp_sdl_instance_test, then run <build dir>/larecomp_sdl_instance_test (exit 0 = passed).
add_executable(larecomp_sdl_instance_test EXCLUDE_FROM_ALL
    src/mc_engine/input/sdl_instance_test.cpp)
target_include_directories(larecomp_sdl_instance_test PRIVATE src src/mc_engine)
target_compile_definitions(larecomp_sdl_instance_test PRIVATE LARECOMP_SHARED_SDL=1)  # as the exe: one SDL here (see larecomp_sdl3 below)
# The legacy Direct3D 12 native graphics runtime (native_gfx/d3d12, 27.8k lines, plus native_gfx.cpp, geometry.cpp,
# telemetry.cpp, guest/sampler_state.cpp): "no longer offered" since the renderer choice was Plume or XENIA, and it sits on
# headers that are private in rexglue 0.11 (rex/ui/d3d12/*: d3d12_api.h, d3d12_util.h, d3d12_provider.h, d3d12_presenter.h).
# It stays in the tree, untouched, and OFF: Windows is built like Linux, which has always been built without it (the stubs
# in src/compat/linux/linux_stubs.cpp stand in for the observers hooks.cpp feeds it). Turning it ON needs those four headers
# and what they include vendored from the SDK's src/gpu/xenos/host/d3d12, and the rex::Runtime / KernelState / Memory call sites
# of its 34 files ported (about 9 + 3 + 20 sites); nobody has done that.
option(LARECOMP_NATIVE_GFX_LEGACY "Build the legacy Direct3D 12 native graphics runtime (Windows only, not ported to rexglue 0.11)" OFF)
if(NOT WIN32 OR NOT LARECOMP_NATIVE_GFX_LEGACY)
    # The guest-side files Plume shares (native_gfx/guest, nocp, vertex_declaration, ...) stay,
    # with their DXGI enums coming from the Windows SDK or, off Windows, from src/compat/linux.
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
    if(NOT WIN32)
        # The generated code and the SDK headers read and write guest memory through
        # typed pointers; clang -O2/-O3 on Linux reorders those accesses (measured: an
        # .fxc parsed to different bytes than on Windows). Aliasing off keeps them in order.
        string(APPEND CMAKE_CXX_FLAGS " -fno-strict-aliasing")
        include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src/compat/linux)
    endif()
endif()
# Capture mode's fixed step, tags, frame gate books, take books and scene clock (src/mc_engine/cinematic/capture_core.h) have a check that
# needs no game:
#   cmake --build <build dir> --target larecomp_capture_test, then run <build dir>/larecomp_capture_test (exit 0 = passed).
add_executable(larecomp_capture_test EXCLUDE_FROM_ALL
    src/mc_engine/cinematic/capture_core_test.cpp
    src/mc_engine/cinematic/capture_core.cpp)
target_include_directories(larecomp_capture_test PRIVATE src src/mc_engine/cinematic)
# The take folder's names, memory plan, host ring (real threads), take.json and state log rows (src/mc_engine/cinematic/take_folder.h) have a check that
# needs no game (give it a file name and it also writes a sample take.json for scene_run.py's reader):
#   cmake --build <build dir> --target larecomp_take_test, then run <build dir>/larecomp_take_test [sample.json] (exit 0 = passed).
add_executable(larecomp_take_test EXCLUDE_FROM_ALL
    src/mc_engine/cinematic/take_folder_test.cpp
    src/mc_engine/cinematic/take_folder.cpp
    src/mc_engine/cinematic/capture_core.cpp)
target_include_directories(larecomp_take_test PRIVATE src src/mc_engine/cinematic)
# A take's sound (src/mc_engine/cinematic/capture_audio_core.h): the samples a number of video frames owns, the mixer frames that cover them, the books of what
# was released, arrived, given up and written, and the WAV file. A check that needs no game:
#   cmake --build <build dir> --target larecomp_capture_audio_test, then run <build dir>/larecomp_capture_audio_test (exit 0 = passed).
add_executable(larecomp_capture_audio_test EXCLUDE_FROM_ALL
    src/mc_engine/cinematic/capture_audio_test.cpp
    src/mc_engine/cinematic/capture_audio_core.cpp)
target_include_directories(larecomp_capture_audio_test PRIVATE src src/mc_engine/cinematic)
# The take's sound session (capture_audio_core.h: Session against a MixerPort) is checked against a fake mixer with a thread of its own; the test
# includes capture_audio_core.cpp itself: cmake --build <build dir> --target larecomp_capture_audio_session_test, then run it (exit 0 = passed).
add_executable(larecomp_capture_audio_session_test EXCLUDE_FROM_ALL
    src/mc_engine/cinematic/capture_audio_session_test.cpp)
target_include_directories(larecomp_capture_audio_session_test PRIVATE src src/mc_engine/cinematic)
# The audio tap in front of the output device (src/mc_engine/music/audio_tap.h) has a check that needs no game or SDK:
#   cmake --build <build dir> --target larecomp_audio_tap_test, then run <build dir>/larecomp_audio_tap_test (exit 0 = passed).
add_executable(larecomp_audio_tap_test EXCLUDE_FROM_ALL
    src/mc_engine/music/audio_tap_test.cpp)
target_include_directories(larecomp_audio_tap_test PRIVATE src/mc_engine/music)
find_package(Threads)
if(Threads_FOUND)
    target_link_libraries(larecomp_capture_audio_session_test PRIVATE Threads::Threads)
    target_link_libraries(larecomp_audio_tap_test PRIVATE Threads::Threads)
endif()
# The state log's pure half (src/mc_engine/cinematic/state_log_core.h: the row, the hashes, the file's place, the bounded queue, the writer)
# has a check that needs no game: cmake --build <build dir> --target larecomp_state_log_test, then run <build dir>/larecomp_state_log_test.
add_executable(larecomp_state_log_test EXCLUDE_FROM_ALL
    src/mc_engine/cinematic/state_log_test.cpp
    src/mc_engine/cinematic/state_log_core.cpp)
target_include_directories(larecomp_state_log_test PRIVATE src src/mc_engine/cinematic)
find_package(Threads)
if(Threads_FOUND)
    target_link_libraries(larecomp_state_log_test PRIVATE Threads::Threads)
endif()
# The game-running marker's names (src/game_marker_core.h: the key of a program path, the Windows mutex and the Linux lock file) have a check
# that needs no game. The launcher computes the same names, with the same expected values (Os\GameMarkerName.cs and its tests):
#   cmake --build <build dir> --target larecomp_game_marker_test, then run <build dir>/larecomp_game_marker_test (exit 0 = passed).
add_executable(larecomp_game_marker_test EXCLUDE_FROM_ALL
    src/game_marker_test.cpp)
target_include_directories(larecomp_game_marker_test PRIVATE src)

# The switch that skips the crash box for a test run (src/crash_options.h: LARECOMP_NO_CRASH_DIALOG, the exit status) has a check that needs no game:
#   cmake --build <build dir> --target larecomp_crash_options_test, then run <build dir>/larecomp_crash_options_test (exit 0 = passed).
add_executable(larecomp_crash_options_test EXCLUDE_FROM_ALL
    src/crash_options_test.cpp)
target_include_directories(larecomp_crash_options_test PRIVATE src)
if(WIN32)
    target_compile_definitions(larecomp_crash_options_test PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# What LARecomp does with each option of the command line (src/settings/cli_rules.h: which options the SDK keeps, which setting
# belongs to a driver that is not loaded this run, what a name nothing knows costs) has a check that needs no game:
#   cmake --build <build dir> --target larecomp_cli_rules_test, then run <build dir>/larecomp_cli_rules_test (exit 0 = passed).
add_executable(larecomp_cli_rules_test EXCLUDE_FROM_ALL
    src/settings/cli_rules_test.cpp)
target_include_directories(larecomp_cli_rules_test PRIVATE src)

# Which folder the shader packs go in (src/bootprogress/pack_folder_rules.h: only a cache folder the player named moves them, not LARecomp's
# own default for cache_root) has a check that needs no game:
#   cmake --build <build dir> --target larecomp_pack_folder_test, then run <build dir>/larecomp_pack_folder_test (exit 0 = passed).
add_executable(larecomp_pack_folder_test EXCLUDE_FROM_ALL
    src/bootprogress/pack_folder_rules_test.cpp)
target_include_directories(larecomp_pack_folder_test PRIVATE src)

# Which renderer each value of the SDK's gpu.driver means (src/gpu/renderer_choice.h: LARecomp's own, or the SDK's Xenia GPU), and the hidden
# way back to the XENIA renderer (src/gpu/renderer_fallback.h: the debug row's picks, the box's words and its larecomp.toml write through the
# profile store, the count of starts that never got going) have a check that needs no game:
#   cmake --build <build dir> --target larecomp_renderer_choice_test, then run <build dir>/larecomp_renderer_choice_test (exit 0 = passed).
add_executable(larecomp_renderer_choice_test EXCLUDE_FROM_ALL
    src/gpu/renderer_choice_test.cpp
    src/profile/larecomp_profile_store.cpp)
target_include_directories(larecomp_renderer_choice_test PRIVATE src/gpu src/profile)

# The two-button box of the compatibility renderer offer (src/gpu/choice_box.h, Windows: its labels fit its buttons, the action is first and the
# default) has a check that needs no game and shows nothing on the screen (the dialog is built hidden and looked at from its own message loop):
#   cmake --build <build dir> --target larecomp_choice_box_test, then run <build dir>/larecomp_choice_box_test [--aware] [--shot <folder>]
# (exit 0 = passed; off Windows it passes at once). The layout numbers it relies on are checked in larecomp_renderer_choice_test.
add_executable(larecomp_choice_box_test EXCLUDE_FROM_ALL
    src/gpu/choice_box_test.cpp
    src/gpu/choice_box.cpp)
target_include_directories(larecomp_choice_box_test PRIVATE src/gpu)
if(WIN32)
    target_link_libraries(larecomp_choice_box_test PRIVATE user32 gdi32)
endif()

# The device watcher's table of present HID interfaces, its retry schedule and the DualSense effect worker's mailbox and write pacing
# (src/mc_engine/input/device_watch_core.h, trigger_mailbox.h) have a check that needs no game (real threads for the mailbox):
#   cmake --build <build dir> --target larecomp_device_watch_test, then run <build dir>/larecomp_device_watch_test (exit 0 = passed).
add_executable(larecomp_device_watch_test EXCLUDE_FROM_ALL
    src/mc_engine/input/device_watch_test.cpp)
target_include_directories(larecomp_device_watch_test PRIVATE src src/mc_engine)
if(Threads_FOUND)
    target_link_libraries(larecomp_device_watch_test PRIVATE Threads::Threads)
endif()

# The active-device state machine (src/mc_engine/input/input_state_core.h: pad / mouse / key filters, the last-input-wins rules, hot-plug, the
# generation) has a check that needs no game:
#   cmake --build <build dir> --target larecomp_input_state_test, then run <build dir>/larecomp_input_state_test (exit 0 = passed).
add_executable(larecomp_input_state_test EXCLUDE_FROM_ALL
    src/mc_engine/input/input_state_test.cpp)
target_include_directories(larecomp_input_state_test PRIVATE src src/mc_engine)
if(Threads_FOUND)
    target_link_libraries(larecomp_input_state_test PRIVATE Threads::Threads)
endif()

# The scene file's parser, timeline, clock and reload rule (src/mc_engine/cinematic/scene.h) have a check that needs no game:
#   cmake --build <build dir> --target larecomp_scene_test, then run <build dir>/larecomp_scene_test (exit 0 = passed).
add_executable(larecomp_scene_test EXCLUDE_FROM_ALL
    src/mc_engine/cinematic/scene_test.cpp
    src/mc_engine/cinematic/scene.cpp)
target_include_directories(larecomp_scene_test PRIVATE src src/mc_engine/cinematic)
# Which scene is asked for (src/mc_engine/cinematic/scene_want.h: the cell behind mcla_scene, header only), on a fake settings registry that
# refuses a watch before RegisterAll like 0.11's, plus a source check that no change callback is added from a static initializer:
#   cmake --build <build dir> --target larecomp_scene_want_test, then run <build dir>/larecomp_scene_want_test (exit 0 = passed).
add_executable(larecomp_scene_want_test EXCLUDE_FROM_ALL
    src/mc_engine/cinematic/scene_want_test.cpp)
target_include_directories(larecomp_scene_want_test PRIVATE src src/mc_engine/cinematic)
if(Threads_FOUND)
    target_link_libraries(larecomp_scene_want_test PRIVATE Threads::Threads)
endif()
# The camera rig's maths, track parser and evaluator (src/mc_engine/cinematic/camera.h) have one too:
#   cmake --build <build dir> --target larecomp_camera_test, then run <build dir>/larecomp_camera_test (exit 0 = passed).
add_executable(larecomp_camera_test EXCLUDE_FROM_ALL
    src/mc_engine/cinematic/camera_test.cpp
    src/mc_engine/cinematic/camera.cpp
    src/mc_engine/cinematic/scene.cpp
    src/mc_engine/cinematic/spots.cpp
    src/mc_engine/cinematic/path_follow.cpp)
target_include_directories(larecomp_camera_test PRIVATE src src/mc_engine/cinematic)
# The scene's named places (src/mc_engine/cinematic/spots.h: the spot words of a statement, the spots file's reader, the lookup of the files a scene uses) and the
# way scene.cpp turns spot words into numbers have a check that needs no game (with scene files as arguments it prints their statements, spots resolved):
#   cmake --build <build dir> --target larecomp_spots_test, then run <build dir>/larecomp_spots_test (exit 0 = passed).
add_executable(larecomp_spots_test EXCLUDE_FROM_ALL
    src/mc_engine/cinematic/spots_test.cpp
    src/mc_engine/cinematic/spots.cpp
    src/mc_engine/cinematic/scene.cpp)
target_include_directories(larecomp_spots_test PRIVATE src src/mc_engine/cinematic)
# The scene's cars (car_verbs.h, car_registry.h, car_spawn.h: the statements (header only), the registry of poses and the script of native calls, pure) have a check that needs no game:
#   cmake --build <build dir> --target larecomp_cars_test, then run <build dir>/larecomp_cars_test (exit 0 = passed).
add_executable(larecomp_cars_test EXCLUDE_FROM_ALL
    src/mc_engine/cinematic/cars_test.cpp
    src/mc_engine/cinematic/car_registry.cpp
    src/mc_engine/cinematic/car_spawn.cpp
    src/mc_engine/cinematic/ambient_car.cpp
    src/mc_engine/cinematic/path_follow.cpp)
target_include_directories(larecomp_cars_test PRIVATE src src/mc_engine/cinematic)
# A scene car's route (path_follow.h: the file, the speed along it, the heading, the cut into the AI's destinations: pure) has a check that needs no game:
#   cmake --build <build dir> --target larecomp_path_test, then run <build dir>/larecomp_path_test (exit 0 = passed; with route files as arguments it summarises each).
add_executable(larecomp_path_test EXCLUDE_FROM_ALL
    src/mc_engine/cinematic/path_follow_test.cpp
    src/mc_engine/cinematic/path_follow.cpp)
target_include_directories(larecomp_path_test PRIVATE src src/mc_engine/cinematic)
# The Director's atlas (director/atlas_core.h: decoding the lamps, the rail network, the car table from a byte buffer, the queries, a route along the roads, the camera
# maths, the JSON files: pure) has a check that needs no game:
#   cmake --build <build dir> --target larecomp_atlas_test, then run <build dir>/larecomp_atlas_test (exit 0 = passed; with --rails <traffic.xrn decompressed> it
#   decodes that and prints what it found).
add_executable(larecomp_atlas_test EXCLUDE_FROM_ALL
    src/mc_engine/cinematic/director/atlas_core_test.cpp
    src/mc_engine/cinematic/director/atlas_core.cpp)
target_include_directories(larecomp_atlas_test PRIVATE src/mc_engine/cinematic/director)
# ... and its JSON answers (atlas_answers.h), which need the command layer's json.h / json.cpp (package A): the target exists once those are in the tree.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/mc_engine/cinematic/director/json.cpp")
    add_executable(larecomp_atlas_answers_test EXCLUDE_FROM_ALL
        src/mc_engine/cinematic/director/atlas_answers_test.cpp
        src/mc_engine/cinematic/director/atlas_answers.cpp
        src/mc_engine/cinematic/director/atlas_core.cpp
        src/mc_engine/cinematic/director/json.cpp)
    target_include_directories(larecomp_atlas_answers_test PRIVATE src/mc_engine/cinematic/director)
endif()
# What a settings save does with a cvar a scene holds (src/mc_engine/core/config_save.h, DecideCvarSave, a plain function) has a check that needs no game:
#   cmake --build <build dir> --target larecomp_config_save_test, then run <build dir>/larecomp_config_save_test (exit 0 = passed).
add_executable(larecomp_config_save_test EXCLUDE_FROM_ALL
    src/mc_engine/core/config_save_test.cpp)
target_include_directories(larecomp_config_save_test PRIVATE src src/mc_engine)
# The always-on keyboard's event latch, the mouse keys and the neutral pad rule (src/mc_engine/input/kbm_input.h, header only) have a check that needs no game:
#   cmake --build <build dir> --target larecomp_kbm_input_test, then run <build dir>/larecomp_kbm_input_test (exit 0 = passed).
add_executable(larecomp_kbm_input_test EXCLUDE_FROM_ALL
    src/mc_engine/input/kbm_input_test.cpp)
target_include_directories(larecomp_kbm_input_test PRIVATE src src/mc_engine)
find_package(Threads)
if(Threads_FOUND)
    target_link_libraries(larecomp_kbm_input_test PRIVATE Threads::Threads)
endif()
# Mouse look and keyboard steering's pure part (src/mc_engine/input/mouse_look_core.h, header only: the raw delta sum, the relative-mode gate, the look angles and their return behind
# the car, the steering ramp, the sensitivity rule) has a check that needs no game:
#   cmake --build <build dir> --target larecomp_mouse_look_test, then run <build dir>/larecomp_mouse_look_test (exit 0 = passed).
add_executable(larecomp_mouse_look_test EXCLUDE_FROM_ALL
    src/mc_engine/input/mouse_look_test.cpp)
target_include_directories(larecomp_mouse_look_test PRIVATE src src/mc_engine)
if(Threads_FOUND)
    target_link_libraries(larecomp_mouse_look_test PRIVATE Threads::Threads)
endif()
# The mouse in the menus' pure part (src/mc_engine/input/menu_mouse_core.h, header only: the window -> stage mapping, the instance matrices, the row boxes and their hit test, the settings rows'
# arrow zones, the click / hover / wheel plans, the press pacing, the highlight walk) has a check that needs no game:
#   cmake --build <build dir> --target larecomp_menu_mouse_test, then run <build dir>/larecomp_menu_mouse_test (exit 0 = passed).
add_executable(larecomp_menu_mouse_test EXCLUDE_FROM_ALL
    src/mc_engine/input/menu_mouse_test.cpp)
target_include_directories(larecomp_menu_mouse_test PRIVATE src src/mc_engine)
# The live button prompts' pure part (src/mc_engine/input/prompt_glyphs.h, header only: the slot tables, the menu / road context, which art each glyph family shows, the PlayStation sources and
# their override, the reading back of the game's DXT5 bitmaps) has a check that needs no game:
#   cmake --build <build dir> --target larecomp_prompt_glyphs_test, then run <build dir>/larecomp_prompt_glyphs_test (exit 0 = passed).
add_executable(larecomp_prompt_glyphs_test EXCLUDE_FROM_ALL
    src/mc_engine/input/prompt_glyphs_test.cpp)
target_include_directories(larecomp_prompt_glyphs_test PRIVATE src src/mc_engine)
# The free camera's flight, the pad, the recorder's chords and the raw track file (src/mc_engine/cinematic/freecam.h):
#   cmake --build <build dir> --target larecomp_freecam_test, then run <build dir>/larecomp_freecam_test (exit 0 = passed).
add_executable(larecomp_freecam_test EXCLUDE_FROM_ALL
    src/mc_engine/cinematic/freecam_test.cpp
    src/mc_engine/cinematic/freecam.cpp
    src/mc_engine/cinematic/camera.cpp
    src/mc_engine/cinematic/scene.cpp
    src/mc_engine/cinematic/path_follow.cpp)
target_include_directories(larecomp_freecam_test PRIVATE src src/mc_engine/cinematic)
# The scene's `split` track (the parser's keys and EvaluateSplit) has a check of its own, also without the game:
#   cmake --build <build dir> --target larecomp_split_track_test, then run <build dir>/larecomp_split_track_test (exit 0 = passed).
add_executable(larecomp_split_track_test EXCLUDE_FROM_ALL
    src/mc_engine/cinematic/split_track_test.cpp
    src/mc_engine/cinematic/scene.cpp)
target_include_directories(larecomp_split_track_test PRIVATE src src/mc_engine/cinematic)
# The Director's pure parts (src/mc_engine/cinematic/director: the JSON of the wire, the command engine and its argument check, the console grammar, the line server over real
# loopback sockets, the camera projection, the route reducer, the path rule, the PNG writer) have a check that needs no game:
#   cmake --build <build dir> --target larecomp_director_test, then run <build dir>/larecomp_director_test (exit 0 = passed).
add_executable(larecomp_director_test EXCLUDE_FROM_ALL
    src/mc_engine/cinematic/director/director_test.cpp
    src/mc_engine/cinematic/director/json.cpp
    src/mc_engine/cinematic/director/command.cpp
    src/mc_engine/cinematic/director/line_server.cpp
    src/mc_engine/cinematic/director/scene_edit.cpp
    src/mc_engine/cinematic/director/director_math.cpp
    src/mc_engine/cinematic/director/png_writer.cpp
    src/mc_engine/cinematic/scene.cpp
    src/mc_engine/cinematic/camera.cpp
    src/mc_engine/cinematic/freecam.cpp
    src/mc_engine/cinematic/path_follow.cpp)
target_include_directories(larecomp_director_test PRIVATE src src/mc_engine/cinematic src/mc_engine/cinematic/director)
if(Threads_FOUND)
    target_link_libraries(larecomp_director_test PRIVATE Threads::Threads)
endif()
if(WIN32)
    target_link_libraries(larecomp_director_test PRIVATE ws2_32)
endif()
# The scene file's line-addressed edits (insert, replace, delete, move, param; comments and layout kept; the stale rule; the diff):
#   cmake --build <build dir> --target larecomp_scene_edit_test, then run <build dir>/larecomp_scene_edit_test (exit 0 = passed).
add_executable(larecomp_scene_edit_test EXCLUDE_FROM_ALL
    src/mc_engine/cinematic/director/scene_edit_test.cpp
    src/mc_engine/cinematic/director/scene_edit.cpp
    src/mc_engine/cinematic/director/json.cpp
    src/mc_engine/cinematic/scene.cpp)
target_include_directories(larecomp_scene_edit_test PRIVATE src src/mc_engine/cinematic src/mc_engine/cinematic/director)

# The Director panel's pure parts (src/mc_engine/ui/director_timeline.h: lanes, markers, the time axis, snapping, drags; director_gizmos.h: the projection into the window
# and the lines of the routes and the camera's path; director_panel_core.h: every action of the panel as a command of the command layer, checked against the real SceneEdit)
# have a check that needs no game:
#   cmake --build <build dir> --target larecomp_director_panel_test, then run <build dir>/larecomp_director_panel_test (exit 0 = passed).
add_executable(larecomp_director_panel_test EXCLUDE_FROM_ALL
    src/mc_engine/ui/director_panel_test.cpp
    src/mc_engine/ui/director_timeline.cpp
    src/mc_engine/ui/director_gizmos.cpp
    src/mc_engine/ui/director_panel_core.cpp
    src/mc_engine/cinematic/director/json.cpp
    src/mc_engine/cinematic/director/command.cpp
    src/mc_engine/cinematic/director/line_server.cpp
    src/mc_engine/cinematic/director/scene_edit.cpp
    src/mc_engine/cinematic/director/director_math.cpp
    src/mc_engine/cinematic/director/png_writer.cpp
    src/mc_engine/cinematic/scene.cpp
    src/mc_engine/cinematic/camera.cpp
    src/mc_engine/cinematic/freecam.cpp
    src/mc_engine/cinematic/path_follow.cpp)
target_include_directories(larecomp_director_panel_test PRIVATE src src/mc_engine/cinematic src/mc_engine/cinematic/director src/mc_engine/ui)
if(Threads_FOUND)
    target_link_libraries(larecomp_director_panel_test PRIVATE Threads::Threads)
endif()
if(WIN32)
    target_link_libraries(larecomp_director_panel_test PRIVATE ws2_32)
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
    # rexglue 0.11 has no XEO3 target (rexglue_add_xeo3_dll_target is gone): the files larecomp_xeo3_manifest.toml and
    # larecomp_config_xeo3.toml stay for whoever picks it up again, with their own [rexcrt] table (a [shim] now).
    message(FATAL_ERROR "REXGLUE_ENABLE_XEO3_TARGET: the XEO3 DLL target is not part of rexglue 0.11")
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
        # The app icon (src/larecomp.ico, `1 ICON` in larecomp.rc): the resource compiler does not report ICON inputs as dependencies either,
        # so an edited icon would not rebuild the .rc without this. The same for MinGW (windres -> llvm-rc reads the same .rc).
        set_source_files_properties(src/larecomp.rc PROPERTIES
                                    OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/larecomp.ico")
    else()
        add_executable(larecomp ${LARECOMP_SOURCES})
    endif()

    # rexglue_setup_target links rex::sdk (rexbase, rexppc, rexxenos, rexkernel, rexxam, rexxbdm, rexapp), builds the
    # generated sources into larecomp_recompiled, and stages next to the exe the 12 provider modules (rexfilesystem, rexhid,
    # rexapu, rexui, rexgpu, rexxam, rexxbdm, and the drivers rexgpu-xenos-{d3d12,vulkan,null}, rexapu-sdl3, rexhid-{sdl3,
    # xinput}, rexui-sdl3) and shared SDL3. The Xenia GPU drivers are what gpu.driver = d3d12/vulkan loads (the XENIA
    # renderer); the default renderer is the driver compiled into the exe (native_gfx/nocp/nocp_driver.cpp).
    # EXCLUDE <name> drops a staged file (rexgpu-xenos-null is only a test driver); NO_COPY stages nothing.
    # The launcher's file lists and tools/prebuilt-local must follow the new set (R7 of the plan).
    rexglue_setup_target(larecomp)

    # Third-party code that rexglue 0.10 installed for every program and 0.11 keeps private. Each is a submodule of
    # LARecomp (.gitmodules), pinned to the commit the 0.11 SDK itself pins (o1heap: the one the 0.10 SDK pinned).

    # o1heap (MIT, https://github.com/pavel-kirienko/o1heap, 3.0.0): the guest-visible object allocator of
    # native_gfx/guest/host_heap.cpp. It was the SDK target rex::o1heap up to 0.10 and is gone in 0.11 (the SDK uses tlsf
    # privately).
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/o1heap/o1heap/o1heap.c")
        message(FATAL_ERROR "thirdparty/o1heap is missing: git submodule update --init --recursive "
                            "(rexglue 0.11 no longer ships rex::o1heap)")
    endif()
    add_library(larecomp_o1heap STATIC thirdparty/o1heap/o1heap/o1heap.c)
    target_include_directories(larecomp_o1heap PUBLIC thirdparty/o1heap/o1heap)
    target_link_libraries(larecomp PRIVATE larecomp_o1heap)

    # toml++ (MIT, https://github.com/marzer/tomlplusplus, v3.4.0): header only, `#include <toml++/toml.hpp>` in
    # achievement_metadata.cpp, mc_engine/string_dumper.cpp and settings/larecomp_settings.cpp. Another copy than the SDK's
    # (which parses its own config files inside its libraries) is no clash: the SDK's headers do not expose a toml++ type.
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/tomlplusplus/include/toml++/toml.hpp")
        message(FATAL_ERROR "thirdparty/tomlplusplus is missing: git submodule update --init --recursive "
                            "(rexglue 0.11 no longer installs toml++)")
    endif()
    add_library(larecomp_tomlplusplus INTERFACE)
    target_include_directories(larecomp_tomlplusplus SYSTEM INTERFACE
        "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/tomlplusplus/include")
    target_link_libraries(larecomp PRIVATE larecomp_tomlplusplus)

    # SDL3: LARecomp calls it itself (the controller list, the pad's rumble and hints). One SDL in the process, the SDK's.
    # The SDK has the target SDL3::SDL3-shared in both layouts, and it is what the SDK's own staging of SDL3 tests
    # (_rexglue_stage_sdl): a REXSDK_DIR build gets it from thirdparty/sdl3 (add_subdirectory), a package gets it from SDL3's own
    # config file, which rexglueConfig.cmake finds with find_dependency(SDL3 CONFIG) (Windows: <prefix>/cmake/SDL3Config.cmake,
    # Linux: <prefix>/lib/cmake/SDL3/SDL3Config.cmake). It carries the headers, the import library (SDL3.lib) or the library and its
    # soname (libSDL3.so.0 on Linux), wherever the SDK put them.
    add_library(larecomp_sdl3 INTERFACE)
    # The exe's SDL IS the SDK's here (the 0.10 line had a second, static copy in the exe on Windows): the exe's SDL code
    # (wheel layer, dialogs' pad reader) then leaves the pad drivers and the events on and talks to the SDK's driver, as it does
    # on Linux. See src/mc_engine/input/sdl_instance.h.
    target_compile_definitions(larecomp_sdl3 INTERFACE LARECOMP_SHARED_SDL=1)
    if(TARGET SDL3::SDL3-shared)
        target_link_libraries(larecomp_sdl3 INTERFACE SDL3::SDL3-shared)
    else()
        # A package that exports no SDL3 target: look in the install tree it came from (rexglue_DIR is <prefix>/lib/cmake/rexglue)
        # and in the prefixes the build was given. Linux: the dev link libSDL3.so, else the library by its soname (libSDL3.so.0): a
        # kit that stages only the soname still links.
        set(_larecomp_sdl3_hints ${CMAKE_PREFIX_PATH})
        if(rexglue_DIR)
            list(APPEND _larecomp_sdl3_hints "${rexglue_DIR}/../../..")
        endif()
        find_path(LARECOMP_SDL3_INCLUDE_DIR SDL3/SDL.h HINTS ${_larecomp_sdl3_hints} PATH_SUFFIXES include NO_DEFAULT_PATH)
        find_library(LARECOMP_SDL3_LIBRARY NAMES SDL3 libSDL3.so.0 HINTS ${_larecomp_sdl3_hints}
                     PATH_SUFFIXES lib lib64 NO_DEFAULT_PATH)
        if(NOT LARECOMP_SDL3_INCLUDE_DIR OR NOT LARECOMP_SDL3_LIBRARY)
            message(FATAL_ERROR "SDL3 not found: there is no target SDL3::SDL3-shared and the SDK install has no include/SDL3/SDL.h "
                                "with lib/SDL3.lib or lib/libSDL3.so (searched: ${_larecomp_sdl3_hints}). A build from the SDK's "
                                "sources (-DREXSDK_DIR=...) or an installed SDK (-DCMAKE_PREFIX_PATH=...) brings it with it.")
        endif()
        target_include_directories(larecomp_sdl3 INTERFACE "${LARECOMP_SDL3_INCLUDE_DIR}")
        target_link_libraries(larecomp_sdl3 INTERFACE "${LARECOMP_SDL3_LIBRARY}")
    endif()
    target_link_libraries(larecomp PRIVATE larecomp_sdl3)

    # libmspack (LGPL-2.1, https://github.com/kyz/libmspack, v1.11): lzxd.c only, the LZX decoder of the modloader's
    # xcompress.cpp (the game's XCompress streams). It was reached as rex/system/lzx.h up to 0.10; in 0.11 the SDK's LZX
    # code (src/filesystem/lzx.cpp) is private to rexfilesystem, so LARecomp builds the decoder itself, as the SDK does
    # (-w: third-party C). The headers (mspack.h, lzx.h) are visible to xcompress.cpp alone: their names are too generic for
    # the include path of every source.
    set(LARECOMP_MSPACK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/libmspack/libmspack/mspack")
    if(NOT EXISTS "${LARECOMP_MSPACK_DIR}/lzxd.c")
        message(FATAL_ERROR "thirdparty/libmspack is missing: git submodule update --init --recursive "
                            "(rexglue 0.11 no longer exposes its LZX decoder)")
    endif()
    # lzxd.c is LGPL-2.1 (only): it is a shared library of its own (larecomp_mspack.dll / .so next to the executable), not
    # linked into larecomp, so a player can replace it with a build of their own as the license asks. Its license goes with
    # every build, next to the library: LICENSES/larecomp_mspack_license.txt (the LGPL-2.1 text) and
    # LICENSES/larecomp_mspack_notice.txt (what it is, its version, where the source is, that it can be replaced) are copied
    # to the library's folder after it is linked. Whatever packages the executable's folder (the launcher's staging, an AppImage)
    # has to carry the library and both files; keep the notice's version line the same as the submodule's commit.
    set(LARECOMP_MSPACK_NOTICES larecomp_mspack_notice.txt larecomp_mspack_license.txt)
    foreach(_larecomp_mspack_notice IN LISTS LARECOMP_MSPACK_NOTICES)
        if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/LICENSES/${_larecomp_mspack_notice}")
            message(FATAL_ERROR "LICENSES/${_larecomp_mspack_notice} is missing: larecomp_mspack is LGPL-2.1 and its notice "
                                "goes with the library in every build")
        endif()
    endforeach()
    add_library(larecomp_mspack SHARED "${LARECOMP_MSPACK_DIR}/lzxd.c")
    set_target_properties(larecomp_mspack PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
    target_include_directories(larecomp_mspack PRIVATE "${LARECOMP_MSPACK_DIR}")
    target_compile_options(larecomp_mspack PRIVATE -w)
    foreach(_larecomp_mspack_notice IN LISTS LARECOMP_MSPACK_NOTICES)
        add_custom_command(TARGET larecomp_mspack POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CMAKE_CURRENT_SOURCE_DIR}/LICENSES/${_larecomp_mspack_notice}"
                    "$<TARGET_FILE_DIR:larecomp_mspack>/${_larecomp_mspack_notice}"
            COMMENT "Copying ${_larecomp_mspack_notice} next to larecomp_mspack")
    endforeach()
    set_source_files_properties(src/mc_engine/modloader/xcompress.cpp PROPERTIES
        INCLUDE_DIRECTORIES "${LARECOMP_MSPACK_DIR}")
    target_link_libraries(larecomp PRIVATE larecomp_mspack)

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
        # larecomp_app.cpp hands the renderer the SDK's window (gpu/native_window.h, which names plume's types): that one
        # file sees plume's headers.
        set_source_files_properties(src/larecomp_app.cpp PROPERTIES
            INCLUDE_DIRECTORIES "$<TARGET_PROPERTY:plume,INTERFACE_INCLUDE_DIRECTORIES>")
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
            # (the native_gfx guest-visible object allocator, guest/host_heap.cpp, needs o1heap: see below)
            dxgi
            # native_gfx D3D12 backend (D3D12SerializeRootSignature etc.)
            d3d12
            # native_gfx memory census (GetProcessMemoryInfo)
            psapi
            # real-world weather fetch (mc_engine/gameplay/real_weather.cpp)
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
            # timeBeginPeriod/timeEndPeriod (the frame timing); rexglue 0.10 linked winmm publicly, 0.11 does not
            winmm
        )
        if(MINGW)
            # What the sources ask for with #pragma comment(lib, ...) (crash_handler.cpp,
            # guest_profiler.cpp, adaptive_triggers.cpp, device_watch.cpp, identity_store.cpp, identity_ui.cpp),
            # which Clang for MinGW does not turn into a link request, and what MSVC's
            # default libraries cover (the ones llvm-mingw's driver does not: winmm for
            # timeBeginPeriod, dxguid for the DirectInput GUID_* of the wheel code, ntdll).
            # advapi32, shell32, user32 and kernel32 are the driver's own defaults.
            target_link_libraries(larecomp
                PRIVATE
                dbghelp
                setupapi
                cfgmgr32
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

    # The frame maths of the ASPECT RATIO setting (src/mc_engine/display/frame_fit.h) has a check that needs no game:
    # cmake --build <dir> --target larecomp_frame_fit_test, then run it (exit 0 = passed).
    add_executable(larecomp_frame_fit_test EXCLUDE_FROM_ALL src/mc_engine/display/frame_fit_test.cpp)
    # The FULLSCREEN RESOLUTION list's rules (mc_engine/display/display_sizes.h) have a check that needs no
    # display, no SDK and no GPU: cmake --build <dir> --target larecomp_display_sizes_test, then run
    # larecomp_display_sizes_test (exit 0 = passed).
    add_executable(larecomp_display_sizes_test EXCLUDE_FROM_ALL src/mc_engine/display/display_sizes_test.cpp)

    # The shared Rate My Ride server players reach on TCP 31002. Standalone: the
    # store and protocol sources only, no SDK.
    add_executable(larecomp_rmr_server
        src/mc_engine/online/rate_my_ride/rmr_server_main.cpp
        src/mc_engine/online/rate_my_ride/rmr_protocol.cpp
        src/mc_engine/online/rate_my_ride/rmr_store.cpp
        src/mc_engine/online/tournament_schedule.cpp
        src/mc_engine/online/tournament_awards.cpp
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

    # The value steps and texts of the pause-menu rows the launcher also has (pause_parity_rows.h) have a
    # check that needs no game: cmake --build <dir> --target larecomp_pause_parity_test, then run it
    # (exit 0 = passed).
    add_executable(larecomp_pause_parity_test EXCLUDE_FROM_ALL src/mc_engine/menus/pause_parity_rows_test.cpp)
    # The CAR SELF-REFLECTIONS row's values and how it reads its two settings back (pause_selfrefl_row.h):
    # cmake --build <dir> --target larecomp_pause_selfrefl_test, then run it (exit 0 = passed).
    add_executable(larecomp_pause_selfrefl_test EXCLUDE_FROM_ALL src/mc_engine/menus/pause_selfrefl_row_test.cpp)
    # The pause menu's "a row that depends on another row" rule (row_dependency.h: FOLIAGE SHADOWS, SHADOW DETAIL, SHADOW FILTER and TRAFFIC SHADOWS grey
    # while SHADOWS is off), the order of LIGHTING AND SHADOWS and WORLD DETAIL as pause_menu.cpp has them, the page width, and how the cursor, the mouse
    # and A treat a grey row (needs no game; it reads pause_menu.cpp and the settings files beside it as text):
    #   cmake --build <dir> --target larecomp_row_dependency_test, then run it (exit 0 = passed).
    add_executable(larecomp_row_dependency_test EXCLUDE_FROM_ALL src/mc_engine/menus/row_dependency_test.cpp)
    target_include_directories(larecomp_row_dependency_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/src/mc_engine")
    # The thread priority intents (src/os/thread_priority.h) have a check that needs no game:
    #   cmake --build <dir> --target larecomp_thread_priority_test, then run it (exit 0 = passed).
    add_executable(larecomp_thread_priority_test EXCLUDE_FROM_ALL
        src/os/thread_priority_test.cpp
        src/os/thread_priority.cpp
    )
    target_include_directories(larecomp_thread_priority_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_features(larecomp_thread_priority_test PRIVATE cxx_std_23)
    # std::thread: glibc before 2.34 needs libpthread named at the link.
    find_package(Threads REQUIRED)
    target_link_libraries(larecomp_thread_priority_test PRIVATE Threads::Threads)
    # The AppImage variable rule (src/os/appimage_env.h) has a check that needs no game:
    #   cmake --build <dir> --target larecomp_appimage_env_test, then run it (exit 0 = passed).
    add_executable(larecomp_appimage_env_test EXCLUDE_FROM_ALL
        src/os/appimage_env_test.cpp
        src/os/appimage_env.cpp
    )
    target_include_directories(larecomp_appimage_env_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_features(larecomp_appimage_env_test PRIVATE cxx_std_23)
    # The yield and the sleep (src/os/thread_yield.h) have a check that needs no game:
    #   cmake --build <dir> --target larecomp_thread_yield_test, then run it (exit 0 = passed).
    add_executable(larecomp_thread_yield_test EXCLUDE_FROM_ALL
        src/os/thread_yield_test.cpp
        src/os/thread_yield.cpp
    )
    target_include_directories(larecomp_thread_yield_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_features(larecomp_thread_yield_test PRIVATE cxx_std_23)
    target_link_libraries(larecomp_thread_yield_test PRIVATE Threads::Threads)

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

    # Floating-point contraction and the CPU level, for the targets that hold the guest code and
    # the host code it calls: larecomp (the executable's own sources and the SDK's entry point
    # sources), larecomp_recompiled (the generated code, an OBJECT library made by
    # rexglue_setup_target) and larecomp_gpu (src/gpu, which the guest thread also runs). Not for
    # the SDK libraries (prebuilt), Plume, the shader pack builder, the standalone servers and the
    # start-up check: those have to run on any x86-64 CPU.
    #  * -ffp-contract=off, at every level. With FMA available, Clang may fuse a separate multiply
    #    and add into one instruction, which rounds once where the console's code rounds twice (an
    #    fmul then an fadd) and so changes results, physics included. The console's own fused
    #    multiply-adds are explicit std::fma calls in the generated code and stay fused. At baseline
    #    there is no FMA to fuse into, so this changes no code there.
    #  * -march=x86-64-v3 for that level. Nothing else: no -ffast-math.
    # Order on the compile line: the SDK's -msse4.1 (rexglue_apply_target_settings) comes first, then
    # these, which target_compile_options appends in the order the targets get them; a source's own
    # COMPILE_OPTIONS (-fasync-exceptions, -gline-tables-only of the generated code) follow. Clang
    # takes the last -march, and -msse4.1 only adds a feature that x86-64-v3 has, so the order does
    # not matter (checked with -dM -E: both orders enable AVX2, FMA, BMI1/2, MOVBE, LZCNT, F16C).
    # Only C++ sources get them (not the .rc). With clang-cl the flags go through /clang:.
    set(_larecomp_cpu_flags -ffp-contract=off)
    if(LARECOMP_CPU_LEVEL STREQUAL "x86-64-v3")
        list(APPEND _larecomp_cpu_flags -march=x86-64-v3)
    endif()
    if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        list(TRANSFORM _larecomp_cpu_flags PREPEND "/clang:")
    endif()
    foreach(_larecomp_cpu_target larecomp larecomp_recompiled larecomp_gpu)
        if(TARGET ${_larecomp_cpu_target})
            target_compile_options(${_larecomp_cpu_target} PRIVATE
                "$<$<COMPILE_LANGUAGE:CXX>:${_larecomp_cpu_flags}>")
        endif()
    endforeach()

    # The CPU level the log reports, and for a build for x86-64-v3 the check that stops the
    # executable on a CPU that lacks it, before any code compiled with -march runs (including the
    # static initializers of those files). Its own OBJECT library so that it gets none of the
    # flags above; its objects are linked into the executable directly (an archive would drop it:
    # nothing references the start-up hook).
    add_library(larecomp_cpuguard OBJECT src/cpu_level.cpp)
    if(LARECOMP_CPU_LEVEL STREQUAL "x86-64-v3")
        target_compile_definitions(larecomp_cpuguard PRIVATE LARECOMP_CPU_LEVEL_V3=1)
    endif()
    target_sources(larecomp PRIVATE $<TARGET_OBJECTS:larecomp_cpuguard>)
    message(STATUS "larecomp: CPU level ${LARECOMP_CPU_LEVEL}")
endif()
