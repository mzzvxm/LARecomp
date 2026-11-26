# The Plume renderer, src/gpu.
#
# larecomp_gpu is an OBJECT library so the renderer compiles with its own
# include order and definitions: plume's headers (its Vulkan-Headers and VMA)
# ahead of rex::sdk's, which arrive as system includes, and
# LARECOMP_GPU_HAS_<backend> visible to these files only. larecomp links the
# objects. Expects the caller to have added thirdparty/plume, included
# FetchDxcHost.cmake, and set LARECOMP_GPU_BACKENDS (D3D12 and/or VULKAN; the
# one that runs is picked at startup).

add_library(larecomp_gpu OBJECT
    ab.cpp
    ao.cpp
    backend.cpp
    background_build.cpp
    buffer_heap.cpp
    capture.cpp
    car_dump.cpp
    car_frame.cpp
    car_mesh_read.cpp
    car_model_walk.cpp
    car_rt_instances.cpp
    car_rt_scene.cpp
    car_rt_ao.cpp
    city_hit_probe.cpp
    city_hit_scene.cpp
    city_rt_scene.cpp
    city_rt_shadow.cpp
    rt_lights.cpp
    rt_scene_service.cpp
    rt_world_frame.cpp
    city_texture_set.cpp
    car_selfrefl.cpp
    car_shadow_map.cpp
    cabin_mask.cpp
    mirror_view.cpp
    mirror_overlay.cpp
    local_shadow.cpp
    car_rt_refl.cpp
    car_rt_glass_refl.cpp
    car_rt_opp_refl.cpp
    mirror_glass.cpp
    city_refl.cpp
    city_refl_setting.cpp
    capture_readback.cpp
    choice_box.cpp
    constants.cpp
    cut_request.cpp
    cycles.cpp
    denoiser_setting.cpp
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
    gbuffer.cpp
    gi_apply.cpp
    gi_pixel.cpp
    gi_field.cpp
    gi_integration.cpp
    gi_update.cpp
    gi_setting.cpp
    mirror_setting.cpp
    gbuffer_material.cpp
    gbuffer_materials.cpp
    geometry.cpp
    gpu_class.cpp
    gfx_preset_apply.cpp
    host_passes.cpp
    host_precache.cpp
    guest_capture.cpp
    guest_memory.cpp
    hdr.cpp
    hdr_scene.cpp
    hdr_tonemap.cpp
    headlight_rig.cpp
    input_layout.cpp
    latency.cpp
    lamp_shadows.cpp
    latency_streamline.cpp
    layout.cpp
    light_manager.cpp
    local_lights.cpp
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
    rt_bench.cpp
    rt_tier.cpp
    rr_guides.cpp
    rt_denoise.cpp
    ray_regen_guides.cpp
    ray_regen_plan.cpp
    samplers.cpp
    scene.cpp
    screen_extract.cpp
    screen_lights.cpp
    screen_shared.cpp
    ssr.cpp
    ssr_rt.cpp
    shaders.cpp
    shadow_atlas.cpp
    shadow_filter.cpp
    shafts.cpp
    side_view_env.cpp
    side_view_reflection.cpp
    side_view_shadow.cpp
    side_view_ui_map.cpp
    side_views.cpp
    split.cpp
    stream.cpp
    swap_chain.cpp
    sync_builds.cpp
    temporal.cpp
    temporal_backend.cpp
    temporal_camera.cpp
    temporal_cut.cpp
    temporal_mv.cpp
    texture_decode.cpp
    texture_decode_pool.cpp
    texture_mods.cpp
    texture_names.cpp
    textures.cpp
    thread_load.cpp
    timing.cpp
    topology.cpp
    uma_budget.cpp
    ui_fade.cpp
    ui_sharp.cpp
    upload.cpp
    upscaler_masks.cpp
    validation.cpp
    vehicle_motion.cpp
    viewport.cpp
    volumetrics.cpp
    volumetrics_inputs.cpp
    vram_budget.cpp
    vulkan_pipeline_cache.cpp
    watch.cpp
)
# The occlusion's reach round vehicles and its slice sum, with the C++ twin of its main pass (ao_halo.h) likewise:
#   cmake --build <dir> --target larecomp_ao_halo_test, then run src/gpu/larecomp_ao_halo_test <src/gpu dir> (exit 0 = passed).
add_executable(larecomp_ao_halo_test EXCLUDE_FROM_ALL ao_halo_test.cpp)
# The occlusion on a car's paint (ao_paint.h: the stored visibility raised to a power below 1 on a vehicle's pixel; given the src/gpu folder it
# also checks that the shader's and ao.cpp's constants and flags are the header's) likewise:
#   cmake --build <dir> --target larecomp_ao_paint_test, then run src/gpu/larecomp_ao_paint_test <src/gpu dir> [--print] (exit 0 = passed).
add_executable(larecomp_ao_paint_test EXCLUDE_FROM_ALL ao_paint_test.cpp)
# The occlusion's blur radius that follows the picture's size, its wide blur, the noise sequence and the history weight (ao_stable.h; given
# the src/gpu folder it also checks that the shaders' and ao.cpp's constants are the header's) likewise:
#   cmake --build <dir> --target larecomp_ao_stable_test, then run src/gpu/larecomp_ao_stable_test <src/gpu dir> (exit 0 = passed).
add_executable(larecomp_ao_stable_test EXCLUDE_FROM_ALL ao_stable_test.cpp)
# The bounce light's clip box and a-trous passes (ssgi_stable.h; given the src/gpu folder it also checks that the shaders' and ao.cpp's
# constants are the header's) likewise:
#   cmake --build <dir> --target larecomp_ssgi_stable_test, then run src/gpu/larecomp_ssgi_stable_test <src/gpu dir> (exit 0 = passed).
add_executable(larecomp_ssgi_stable_test EXCLUDE_FROM_ALL ssgi_stable_test.cpp)
# The cheaper forms of the occlusion's and the bounce light's shader maths (ao_fast.h: the polynomial acos and its error in sectors, the
# blur's one exponential, the variants' counts; on the twins of the passes the cheaper arithmetic against before; given the src/gpu folder
# it also checks that the shaders', CMake's and ao.cpp's constants are the header's) likewise:
#   cmake --build <dir> --target larecomp_ao_fast_test, then run src/gpu/larecomp_ao_fast_test <src/gpu dir> [--print].
add_executable(larecomp_ao_fast_test EXCLUDE_FROM_ALL ao_fast_test.cpp)
# How steady the occlusion and the bounce light are, the first version against ao_stable.h and ssgi_stable.h, on the C++ twin of their passes
# (ao_stable_twin.h: a still camera over a road, a wall and a car at 3840x2160 and 1280x720; about half a minute) likewise:
#   cmake --build <dir> --target larecomp_ao_stable_noise_test, then run src/gpu/larecomp_ao_stable_noise_test [--print].
add_executable(larecomp_ao_stable_noise_test EXCLUDE_FROM_ALL ao_stable_noise_test.cpp)
# The blue noise tile the screen-space effects sample with (blue_noise.h; hlsl/blue_noise.hlsli is its table: the check compares them, and `--emit` writes the
# file): a permutation, uniform, deterministic, blue (no low frequencies, no peak) against interleaved gradient noise, which is a lattice; no renderer needed:
#   cmake --build <dir> --target larecomp_blue_noise_test, then run src/gpu/larecomp_blue_noise_test [--print | --emit > src/gpu/hlsl/blue_noise.hlsli].
add_executable(larecomp_blue_noise_test EXCLUDE_FROM_ALL blue_noise_test.cpp)
# What the blue noise does to the occlusion, the bounce light and the contact shadows (ao_halo.h's twin of the main pass over a wall, through the real blur and history:
# the error's lattice against interleaved gradient noise; given the src/gpu folder it also reads the shaders', ao.cpp's, the setting's and the debug row's use of it):
#   cmake --build <dir> --target larecomp_ao_blue_noise_test, then run src/gpu/larecomp_ao_blue_noise_test <src/gpu dir> [--print] (about ten seconds).
add_executable(larecomp_ao_blue_noise_test EXCLUDE_FROM_ALL ao_blue_noise_test.cpp)
# The shared per-frame blue noise (hlsl/blue_noise_frame.hlsli: the tile stepped along the R2 sequence, which the stochastic passes other than the occlusion and the reflections of the screen use;
# NOISE-AUDIT) and the debug setting mcla_gpu_noise_blue that switches them to the first versions' gradient noise: uniform, changes every frame, nothing slides, no lattice after the average:
#   cmake --build <dir> --target larecomp_noise_frame_test, then run src/gpu/larecomp_noise_frame_test [<src/gpu dir>] [--print] (a second or two).
add_executable(larecomp_noise_frame_test EXCLUDE_FROM_ALL noise_frame_test.cpp)
# The ray traced city sun shadows disc noise (hlsl/city_rt_shadow_noise.hlsli: the blue noise tile in place of gradient noise; NOISE-AUDIT): numbers, error after the average, still pattern, sources:
#   cmake --build <dir> --target larecomp_city_rt_shadow_noise_test, then run src/gpu/larecomp_city_rt_shadow_noise_test [<src/gpu dir>] [--print] (a few seconds).
add_executable(larecomp_city_rt_shadow_noise_test EXCLUDE_FROM_ALL city_rt_shadow_noise_test.cpp)
# The noise of a city hit sun ray and lamp choice in the car reflections (hlsl/city_refl_noise.hlsli: the blue noise tile stepped along R2, no gradient noise and no direction salt; NOISE-AUDIT):
#   cmake --build <dir> --target larecomp_city_refl_noise_test, then run src/gpu/larecomp_city_refl_noise_test [<src/gpu dir>] [--print] (a few seconds).
add_executable(larecomp_city_refl_noise_test EXCLUDE_FROM_ALL city_refl_noise_test.cpp)
# The ray traced car occlusion's and sun ray's noise (hlsl/car_rt_ao_noise.hlsli: the blue noise tile in place of gradient noise; NOISE-AUDIT): numbers, history, sources:
#   cmake --build <dir> --target larecomp_car_rt_ao_noise_test, then run src/gpu/larecomp_car_rt_ao_noise_test [<src/gpu dir>] [--print] (a few seconds).
add_executable(larecomp_car_rt_ao_noise_test EXCLUDE_FROM_ALL car_rt_ao_noise_test.cpp)
# The ray traced car reflection's cone turn (hlsl/car_rt_refl_cs.hlsl ConeRotation and its twin: the blue noise tile in place of gradient noise; NOISE-AUDIT): turns, error after the average, flag, sources:
#   cmake --build <dir> --target larecomp_car_refl_noise_test, then run src/gpu/larecomp_car_refl_noise_test [<src/gpu dir>] [--print] (a few seconds).
add_executable(larecomp_car_refl_noise_test EXCLUDE_FROM_ALL car_refl_noise_test.cpp)
# The sun shafts' ray-start noise (hlsl/shafts_noise.hlsli: the blue noise tile in place of gradient noise; NOISE-AUDIT): numbers, error after the average, still pattern, sources:
#   cmake --build <dir> --target larecomp_shafts_noise_test, then run src/gpu/larecomp_shafts_noise_test [<src/gpu dir>] [--print] (a few seconds).
add_executable(larecomp_shafts_noise_test EXCLUDE_FROM_ALL shafts_noise_test.cpp)
# The fog volume per-voxel sample offset (hlsl/volumetrics_noise.hlsli: the blue noise tile, no sliding gradient noise; NOISE-AUDIT): offsets, history error, sources:
#   cmake --build <dir> --target larecomp_volumetrics_noise_test, then run src/gpu/larecomp_volumetrics_noise_test [<src/gpu dir>] [--print] (a few seconds).
add_executable(larecomp_volumetrics_noise_test EXCLUDE_FROM_ALL volumetrics_noise_test.cpp)
# The sun shadow filter disk turn (shaderpack/variants/shadow_filter.cpp ShadowFilterNoise and its blue noise table, shadow_filter.h mode bit 5; NOISE-AUDIT): table, turn, penumbra error, host:
#   cmake --build <dir> --target larecomp_shadow_filter_noise_test, then run src/gpu/larecomp_shadow_filter_noise_test [<src/gpu dir>] [--print] (a few seconds).
add_executable(larecomp_shadow_filter_noise_test EXCLUDE_FROM_ALL shadow_filter_noise_test.cpp)
# What the sun shadow filter's disk does while no temporal upscaler runs (NOISE-RESOLVE, noise_resolve.h): the model of the variant's disk (a straight edge, the weights, the golden angle taps) against the options
# (fixed turn, a per pixel turn, more taps, an average over frames) with the numbers the decision rests on:
#   cmake --build <dir> --target larecomp_shadow_filter_resolve_test, then run src/gpu/larecomp_shadow_filter_resolve_test [<src/gpu dir>] [--print] (a few seconds).
add_executable(larecomp_shadow_filter_resolve_test EXCLUDE_FROM_ALL shadow_filter_resolve_test.cpp)
# NOISE-RESOLVE (noise_resolve.h, NOISE-AUDIT.md CHECK 1 and 2): the effects that held one pattern without a temporal upscaler (the contact march, the traced sun rays, the shafts, the car reflection cone) change their noise
# every frame and resolve it with a history; the twins of the passes measure the per-frame variation, the error after the history under a still and a moving camera, the flicker and the ghost; the upscaler's jitter in
# a pinned still; the memory; the sources:
#   cmake --build <dir> --target larecomp_noise_resolve_test, then run src/gpu/larecomp_noise_resolve_test [<src/gpu dir>] [--print] (about ten seconds).
add_executable(larecomp_noise_resolve_test EXCLUDE_FROM_ALL noise_resolve_test.cpp)
# The size of the occlusion's and the bounce light's images (ao_size.h: half the picture's size above 1080 lines of render height, the
# contact shadows keeping their own size, the memory each frame holds and the cost model; given the src/gpu folder it also checks that the
# shaders' and ao.cpp's flags and cvars are the header's) likewise:
#   cmake --build <dir> --target larecomp_ao_size_test, then run src/gpu/larecomp_ao_size_test <src/gpu dir> [--print].
add_executable(larecomp_ao_size_test EXCLUDE_FROM_ALL ao_size_test.cpp)
# The occlusion's use of the G-buffer's normal plane (ao_gbuffer.h; GB-E: the normal image the passes read, the rules, the camera's rows, the facing of the bounce light's samples; it also
# compiles hlsl/ao_gbuffer_normal.hlsli as C++ and compares it with the header, and reads the sources it is given) has a check that needs no renderer:
#   cmake --build <dir> --target larecomp_ao_gbuffer_test, then run src/gpu/larecomp_ao_gbuffer_test <src/gpu dir> [--print] (exit 0 = passed).
add_executable(larecomp_ao_gbuffer_test EXCLUDE_FROM_ALL ao_gbuffer_test.cpp)
# The reader of a setting's definition that the source checks of the three occlusion tests above use (setting_source.h: the REX_SETTING row of
# src/settings/generated, and the REXCVAR_DEFINE call of a .cpp before the conversion) has a check that needs no renderer:
#   cmake --build <dir> --target larecomp_setting_source_test, then run src/gpu/larecomp_setting_source_test (exit 0 = passed).
add_executable(larecomp_setting_source_test EXCLUDE_FROM_ALL setting_source_test.cpp)
# Which draws wait in the background for a pipeline whatever the game's rule says (variant_wait.h: a draw whose key has the headlight's word or the G-buffer's
# planes has the game's own shader as its fallback, so the lit world draws that write the stencil no longer make those pipelines on the render thread):
#   cmake --build <dir> --target larecomp_variant_wait_test, then run src/gpu/larecomp_variant_wait_test (exit 0 = passed).
add_executable(larecomp_variant_wait_test EXCLUDE_FROM_ALL variant_wait_test.cpp)
target_include_directories(larecomp_variant_wait_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
# The guest frame's once-only state (guest_frame.h; LIGHT-STACK-REVIEW F2: Scene::BeginFrame runs for every list, the reflections' trigger mark and the shared screen products' published frame must
# survive a second list of one guest frame), the two owners' frames as twins and their sources:
#   cmake --build <dir> --target larecomp_guest_frame_test, then run src/gpu/larecomp_guest_frame_test [<src/gpu dir>] (exit 0 = passed).
add_executable(larecomp_guest_frame_test EXCLUDE_FROM_ALL guest_frame_test.cpp)
target_include_directories(larecomp_guest_frame_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
# The render thread's own pipeline builds and waits as log lines (sync_build_log.h: the rate limit, the counts, the wording that perf/scripts/stall_check.py reads) have a check
# that needs no renderer:
#   cmake --build <dir> --target larecomp_sync_build_log_test, then run src/gpu/larecomp_sync_build_log_test (exit 0 = passed).
add_executable(larecomp_sync_build_log_test EXCLUDE_FROM_ALL sync_build_log_test.cpp)
# The render thread's wait for a guest pipeline that a worker is making (pipeline_wait.h: after 30 ms the worker gets normal priority, after 400 ms the render thread makes the
# pipeline too; VK-FIRST-FRAME, the 5.4 s wait of the Vulkan race smoke) has a check that needs no renderer:
#   cmake --build <dir> --target larecomp_pipeline_wait_test, then run src/gpu/larecomp_pipeline_wait_test (exit 0 = passed).
add_executable(larecomp_pipeline_wait_test EXCLUDE_FROM_ALL pipeline_wait_test.cpp)
# The stencil reference of a guest draw (stencil_ref.h: not in the pipeline key, cleared from an older cache's records, put on the command list only when it changes; STENCIL-DYN)
# has a check that needs no renderer:
#   cmake --build <dir> --target larecomp_stencil_ref_test, then run src/gpu/larecomp_stencil_ref_test <src/gpu dir> (exit 0 = passed).
add_executable(larecomp_stencil_ref_test EXCLUDE_FROM_ALL stencil_ref_test.cpp)

# Vulkan pipeline cache file identity and snapshot rules, standalone like pipeline_wait_test.
add_executable(larecomp_vulkan_pipeline_cache_test EXCLUDE_FROM_ALL vulkan_pipeline_cache_test.cpp)
# The capture sync's counting and decision (sync_builds.h) have a check that needs no renderer:
#   cmake --build <dir> --target larecomp_sync_builds_test, then run src/gpu/larecomp_sync_builds_test (exit 0 = passed).
add_executable(larecomp_sync_builds_test EXCLUDE_FROM_ALL sync_builds_test.cpp)
# Every descriptor set the renderer makes agrees with its builder (descriptor_count_test.cpp; tools/check_descriptor_counts.py is its Python twin): D3D12 refuses a set
# whose CreateDescriptorSet counts are not the builder's ranges (Vulkan does not count: the city shadow pass never ran on D3D12 for that), and the binding code's indices,
# range types and structured views are read against the ranges. Reads the .cpp / .h files of the folder it is given, no renderer needed:
#   cmake --build <dir> --target larecomp_descriptor_count_test, then run src/gpu/larecomp_descriptor_count_test <src dir> [--print] [--strict] (exit 0 = passed).
add_executable(larecomp_descriptor_count_test EXCLUDE_FROM_ALL descriptor_count_test.cpp)
# The GPU class rule and the `auto` table (gpu_class.h) have a check that needs no renderer:
#   cmake --build <dir> --target larecomp_gpu_class_test, then run src/gpu/larecomp_gpu_class_test (exit 0 = passed).
add_executable(larecomp_gpu_class_test EXCLUDE_FROM_ALL gpu_class_test.cpp)
# The graphics presets' table and rules (gfx_preset.h: the cells of the six presets, VANILLA's values, the old `auto` tables that now read it, the recommendation, the first start and
# the CUSTOM flip) have a check that needs no renderer; tools/check_gfx_presets.py is the half that reads the pause menu's pages:
#   cmake --build <dir> --target larecomp_gfx_preset_test, then run src/gpu/larecomp_gfx_preset_test (exit 0 = passed).
add_executable(larecomp_gfx_preset_test EXCLUDE_FROM_ALL gfx_preset_test.cpp)
# The ray tracing tier beside it (rt_tier.h: the tables' every row, the detection's order, the benchmark's decision with synthetic rates, the rungs, the
# `auto` table's cells, the launcher's words) and the benchmark's rules (rt_bench_rules.h: the synthetic scene, the plan, the rate, the cache record), likewise:
#   cmake --build <dir> --target larecomp_rt_tier_test, then run src/gpu/larecomp_rt_tier_test (exit 0 = passed).
#   cmake --build <dir> --target larecomp_rt_bench_rules_test, then run src/gpu/larecomp_rt_bench_rules_test (exit 0 = passed).
add_executable(larecomp_rt_tier_test EXCLUDE_FROM_ALL rt_tier_test.cpp)
add_executable(larecomp_rt_bench_rules_test EXCLUDE_FROM_ALL rt_bench_rules_test.cpp)
# The sun shadow filter's rules (shadow_filter.h: the choice, the taps, the constants) likewise:
#   cmake --build <dir> --target larecomp_shadow_filter_rules_test, then run src/gpu/larecomp_shadow_filter_rules_test.
add_executable(larecomp_shadow_filter_rules_test EXCLUDE_FROM_ALL shadow_filter_test.cpp)
# Where the sun shafts are added and when they fall back (shafts_anchor.h) likewise:
#   cmake --build <dir> --target larecomp_shafts_anchor_test, then run src/gpu/larecomp_shafts_anchor_test.
add_executable(larecomp_shafts_anchor_test EXCLUDE_FROM_ALL shafts_anchor_test.cpp)
# Where the sun is in the picture (shafts_sun.h: the sky's sun constant against the light, the placement and the fades) likewise:
#   cmake --build <dir> --target larecomp_shafts_sun_test, then run src/gpu/larecomp_shafts_sun_test.
add_executable(larecomp_shafts_sun_test EXCLUDE_FROM_ALL shafts_sun_test.cpp)
# The sun shafts' strength levels (shafts_level.h: the setting's words, the multipliers, auto per class, the old values, the ray steps) likewise:
#   cmake --build <dir> --target larecomp_shafts_level_test, then run src/gpu/larecomp_shafts_level_test.
add_executable(larecomp_shafts_level_test EXCLUDE_FROM_ALL shafts_level_test.cpp)
# The volumetric light's setting words and what `auto` comes to per class (volumetrics_level.h) likewise:
#   cmake --build <dir> --target larecomp_volumetrics_level_test, then run src/gpu/larecomp_volumetrics_level_test (exit 0 = passed).
add_executable(larecomp_volumetrics_level_test EXCLUDE_FROM_ALL volumetrics_level_test.cpp)
# The volumetric light's rules (volumetrics_rules.h, volumetrics_types.h: the slice maths and the 2D atlas, the phase function, the lamp law, the
# cascade pick, the headlight shadow lookup, the reprojection, the constants' layout and builder; given the src/gpu folder it also checks that the
# shaders' constants and byte offsets are the header's) likewise:
#   cmake --build <dir> --target larecomp_volumetrics_rules_test, then run src/gpu/larecomp_volumetrics_rules_test <src/gpu dir> (exit 0 = passed).
add_executable(larecomp_volumetrics_rules_test EXCLUDE_FROM_ALL volumetrics_rules_test.cpp)
# The volumetric light's light inputs (volumetrics_lamps.h: the game's lamp law and the calibration against the lit road, the pick with its hysteresis and fades;
# vol_lamp_list.h: the guest list's record and the table; volumetrics_headlights.h: the headlights' units and the shadow atlas's state, with the real headers
# of feat/headlights when they are in the tree) likewise:
#   cmake --build <dir> --target larecomp_volumetrics_inputs_test, then run src/gpu/larecomp_volumetrics_inputs_test <src/gpu dir> (exit 0 = passed).
add_executable(larecomp_volumetrics_inputs_test EXCLUDE_FROM_ALL volumetrics_inputs_test.cpp)
target_include_directories(larecomp_volumetrics_inputs_test PRIVATE ${CMAKE_SOURCE_DIR}/src)  # settings/immortal.h (vol_lamp_list.h, cut_request.cpp)
# The volumetric light's stability rules (volumetrics_stable.h: a lamp's term averaged over a slice of the grid, the composite's samples, the history's weight as a time
# constant) and a simulation of the passes for a camera driving at a street lamp, which puts a number on the flicker of the first version and of the fixes; given the
# src/gpu folder it also checks that the shaders' flags are the header's, likewise:
#   cmake --build <dir> --target larecomp_volumetrics_stable_test, then run src/gpu/larecomp_volumetrics_stable_test <src/gpu dir> (exit 0 = passed).
add_executable(larecomp_volumetrics_stable_test EXCLUDE_FROM_ALL volumetrics_stable_test.cpp)
target_include_directories(larecomp_volumetrics_stable_test PRIVATE ${CMAKE_SOURCE_DIR}/src)  # settings/immortal.h (vol_lamp_list.h, cut_request.cpp)
# The sun shafts' image size rule (shafts_image.h: half the picture, capped at a 960x540 box; the sky pass's taps; given the hlsl folder it
# also checks that the shaders say what the header says) likewise:
#   cmake --build <dir> --target larecomp_shafts_image_test, then run src/gpu/larecomp_shafts_image_test [<src/gpu/hlsl dir>].
add_executable(larecomp_shafts_image_test EXCLUDE_FROM_ALL shafts_image_test.cpp)
# The bounce light's cap, falloff and gain maths and its strength levels (ssgi_light.h, ssgi_level.h; given the hlsl folder it also
# checks that the shaders' constants are the header's) likewise:
#   cmake --build <dir> --target larecomp_ssgi_test, then run src/gpu/larecomp_ssgi_test [<src/gpu/hlsl dir>].
add_executable(larecomp_ssgi_test EXCLUDE_FROM_ALL ssgi_test.cpp)
# The bounce light's lights' share (ssgi_lights.h, GI-BOUNCE-SS: the last frame's light reprojected into the colour chain: the C++ twin of hlsl/ao_color_ps.hlsl LightsHistory and SsgiCarryLights, its rejection rules, the energy
# cap, the history's validity, mutation-checked; given the src/gpu folder it also checks the shaders', the push block's, the frame order's and the scene's text) likewise:
#   cmake --build <dir> --target larecomp_ssgi_lights_test, then run src/gpu/larecomp_ssgi_lights_test <src/gpu dir> (exit 0 = passed).
add_executable(larecomp_ssgi_lights_test EXCLUDE_FROM_ALL ssgi_lights_test.cpp)
# Which host passes a start makes pipelines for (host_plan.h: the class against the settings, the plan, the words of the log line) likewise:
#   cmake --build <dir> --target larecomp_host_plan_test, then run src/gpu/larecomp_host_plan_test.
add_executable(larecomp_host_plan_test EXCLUDE_FROM_ALL host_plan_test.cpp)
# The resolve copy's conversion table and the list of its pipelines made at start (resolve_variants.h: every destination the pass can
# convert into is listed, the copy into k_1_5_5_5 included; given the src/gpu folder it also checks that the mode numbers are the
# shader's and that resolve.cpp keeps no table of its own) likewise. It reads Plume's types header only (the format names), and
# includes format.cpp itself:
#   cmake --build <dir> --target larecomp_resolve_variants_test, then run src/gpu/larecomp_resolve_variants_test <src/gpu dir>.
add_executable(larecomp_resolve_variants_test EXCLUDE_FROM_ALL resolve_variants_test.cpp)
target_link_libraries(larecomp_resolve_variants_test PRIVATE plume)
# HostImage's release order (host_passes.h: framebuffer, view, texture, by the destructor and by move assignment; the defaulted assignment freed the texture before the view: ASan
# heap-use-after-free in GiField::Shutdown) has a check with stand-ins for Plume's texture, view and framebuffer, no device. It reads Plume's headers only:
#   cmake --build <dir> --target larecomp_host_image_test, then run src/gpu/larecomp_host_image_test (exit 0 = passed).
add_executable(larecomp_host_image_test EXCLUDE_FROM_ALL host_image_test.cpp)
target_link_libraries(larecomp_host_image_test PRIVATE plume)
# The guards for the structs the renderer hashes or compares as bytes (key_guard.h: no padding, every member set, the float zero canonical) and the list of the
# places in src/gpu that read a struct as bytes (a new one fails until it is audited): the headlight replay's key had tail padding and made 98 759 to 543 441
# pipelines in a minute on some runs, 230 on others. Given the src/gpu folder it also reads the sources:
#   cmake --build <dir> --target larecomp_key_guard_test, then run src/gpu/larecomp_key_guard_test <src/gpu dir> (exit 0 = passed).
add_executable(larecomp_key_guard_test EXCLUDE_FROM_ALL key_guard_test.cpp)
# The contact shadows' trace rules and the C++ twin of their march (contact_trace.h: the depth tolerance in pixel footprints, the
# ray's fade and steps, the filter weights, the cost model) likewise:
#   cmake --build <dir> --target larecomp_contact_trace_test, then run src/gpu/larecomp_contact_trace_test [<src/gpu/hlsl dir>] (exit 0 = passed; given the shaders' folder it also compares their constants and texts with the header's, else it says it did not).
add_executable(larecomp_contact_trace_test EXCLUDE_FROM_ALL contact_trace_test.cpp)
add_executable(larecomp_cabin_shadow_test EXCLUDE_FROM_ALL cabin_shadow_test.cpp)
add_executable(larecomp_interior_camera_test EXCLUDE_FROM_ALL interior_camera_test.cpp)
# Which traffic cars cast into the sun's shadow cascades (traffic_shadow.h: the setting's words and its auto per class, the limits, the
# candidates, the cutoff that picks the nearest cars, the census over a frame's phases, the cutoff the renderer is told) likewise:
#   cmake --build <dir> --target larecomp_traffic_shadow_test, then run src/gpu/larecomp_traffic_shadow_test.
add_executable(larecomp_traffic_shadow_test EXCLUDE_FROM_ALL traffic_shadow_test.cpp)
# What a press of the compare key does and says (effect_compare.h, effect_compare_rules.h: the effects, the flip, the label) likewise:
#   cmake --build <dir> --target larecomp_effect_compare_test, then run src/gpu/larecomp_effect_compare_test.
add_executable(larecomp_effect_compare_test EXCLUDE_FROM_ALL effect_compare_test.cpp)
# Which effects run in the pause menu's world, the garage and the map, and the line that names a world (menu_world.h) likewise:
#   cmake --build <dir> --target larecomp_menu_world_test, then run src/gpu/larecomp_menu_world_test.
add_executable(larecomp_menu_world_test EXCLUDE_FROM_ALL menu_world_test.cpp)
# The temporal upscalers' two menu-world rules (temporal_menu.h: the pause view's vehicles take the camera's motion, a jitter held while
# the history restarts in every frame) and where temporal.cpp applies them:
#   cmake --build <dir> --target larecomp_temporal_menu_test, then run src/gpu/larecomp_temporal_menu_test <src/gpu dir>.
add_executable(larecomp_temporal_menu_test EXCLUDE_FROM_ALL temporal_menu_test.cpp)
# The car self-reflection's rules (car_selfrefl_rules.h: the modes, the matrix, the constants, the depth linearisation) likewise:
#   cmake --build <dir> --target larecomp_car_selfrefl_rules_test, then run src/gpu/larecomp_car_selfrefl_rules_test.
add_executable(larecomp_car_selfrefl_rules_test EXCLUDE_FROM_ALL car_selfrefl_rules_test.cpp)
# The ray traced car self-reflection's rules (car_rt_refl_rules.h: the image's codes, the places in it, the tolerances, the fallback colour of a hit
# the camera cannot see, the paint's edge-aware read) likewise:
#   cmake --build <dir> --target larecomp_car_rt_refl_rules_test, then run src/gpu/larecomp_car_rt_refl_rules_test (exit 0 = passed).
add_executable(larecomp_car_rt_refl_rules_test EXCLUDE_FROM_ALL car_rt_refl_rules_test.cpp)
# The opponents' ray traced reflection's rules (car_rt_opp_refl_rules.h: the paint mask, the budget, the cars' rectangles in the image and the plan, the aux image's instance codes, the player's record,
# the decision for a hit on the player's car; and that hlsl/car_rt_refl_opp.hlsli, car_rt_refl_cs.hlsl and the host hold the header's numbers) likewise; run it with the src folder or from there:
#   cmake --build <dir> --target larecomp_car_rt_opp_refl_rules_test, then run src/gpu/larecomp_car_rt_opp_refl_rules_test [<src dir>] (exit 0 = passed).
add_executable(larecomp_car_rt_opp_refl_rules_test EXCLUDE_FROM_ALL car_rt_opp_refl_rules_test.cpp)
# The traffic and parked cars' ray traced reflection (car_rt_traffic_rules.h, RT-TRAFFIC-REFL: the traffic paint's identity and mask, which boxes are a traffic car's, the class levels, the plan with the
# traffic's share of the budget against the opponents' own plan on random scenes, the cost estimate; and that the variant, the host and the settings hold the header's numbers) likewise:
#   cmake --build <dir> --target larecomp_car_rt_traffic_rules_test, then run src/gpu/larecomp_car_rt_traffic_rules_test [<src dir>] (exit 0 = passed).
add_executable(larecomp_car_rt_traffic_rules_test EXCLUDE_FROM_ALL car_rt_traffic_rules_test.cpp)
# The parked cars' own ray traced paint (car_rt_parked_paint_rules.h, RT-PARKED-PAINT: the history's negated distance, the rule that marks a texel, the box's depth range, the marks' plan over the
# traffic plan's admission, the still-car constants and their flag, the paint bit and the record radius of a parked car in the list; and that the history pass, the ray traced passes, the host, the variant
# and the settings hold the header's numbers) likewise:
#   cmake --build <dir> --target larecomp_car_rt_parked_paint_rules_test, then run src/gpu/larecomp_car_rt_parked_paint_rules_test [<src dir>] (exit 0 = passed).
add_executable(larecomp_car_rt_parked_paint_rules_test EXCLUDE_FROM_ALL car_rt_parked_paint_rules_test.cpp)
# The ray traced car scene's materials (car_rt_material_rules.h: the table of the cars' shaders, a draw's material from its pixel constants, a walked part's class, the packed
# texture coordinates, the records the shaders read, a mesh's texel density, the environment map's mapping, the reference of the shading; and that
# hlsl/car_rt_hit_shade.hlsli, hlsl/car_rt_refl_cs.hlsl, the host and the settings hold the header's numbers) likewise:
#   cmake --build <dir> --target larecomp_car_rt_material_rules_test, then run src/gpu/larecomp_car_rt_material_rules_test [<src dir>] (exit 0 = passed).
add_executable(larecomp_car_rt_material_rules_test EXCLUDE_FROM_ALL car_rt_material_rules_test.cpp)
# RT-HIT-SHADING part two (car_rt_hit_rules.h: the level table by class and its overrides, the reach to another car and the weight of a hit by it, the scene's radius against the reach, the clear
# coat's share by the paint's fresnel, the two lobes added (exact for the paint's blend), the aux image's word and the denoise's scale, the push block's packed word; and that
# hlsl/car_rt_hit_shade.hlsli, the trace, the opponents' variant, the denoise, the host and the settings hold the header's numbers) likewise:
#   cmake --build <dir> --target larecomp_car_rt_hit_rules_test, then run src/gpu/larecomp_car_rt_hit_rules_test [<src dir>] (exit 0 = passed).
add_executable(larecomp_car_rt_hit_rules_test EXCLUDE_FROM_ALL car_rt_hit_rules_test.cpp)
# The car chrome's rules (car_chrome_rules.h: the numbers of the two short probes of the chrome variant and a reference march of them over
# boxes: a tailpipe under a lip of the bumper, in the open, a plane of every slope) likewise:
#   cmake --build <dir> --target larecomp_car_chrome_rules_test, then run src/gpu/larecomp_car_chrome_rules_test (exit 0 = passed).
add_executable(larecomp_car_chrome_rules_test EXCLUDE_FROM_ALL car_chrome_rules_test.cpp)
# The other cars' ray traced chrome (car_chrome_rt_rules.h: the masks, the sky probe's rays, the texel's code in the ray traced image, the chrome variant's read of it,
# the probes over boxes: a tailpipe under a lip, in the open sun, a plane of every slope; and that hlsl/car_rt_refl_cs.hlsl, the host and the chrome variant
# hold the header's numbers) likewise; run it with the src folder (the one that holds gpu and shaderpack) or from there:
#   cmake --build <dir> --target larecomp_car_chrome_rt_rules_test, then run src/gpu/larecomp_car_chrome_rt_rules_test [<src dir>] (exit 0 = passed).
add_executable(larecomp_car_chrome_rt_rules_test EXCLUDE_FROM_ALL car_chrome_rt_rules_test.cpp)
# The car dump's rules (car_dump_rules.h: the matrices out of a vertex shader bank, the positions and indices as the guest holds them, the
# triangle list, the CPU rasteriser and the statistics against the car history image) likewise:
#   cmake --build <dir> --target larecomp_car_dump_rules_test, then run src/gpu/larecomp_car_dump_rules_test (exit 0 = passed).
add_executable(larecomp_car_dump_rules_test EXCLUDE_FROM_ALL car_dump_rules_test.cpp)
# The headlight rig's rules (headlight_rig_rules.h: the CPU twin of the lit shaders' headlight term, the night factor, where a car's headlights are, the
# tracker's picks and fades, the shadow views' table, the constants block) likewise:
#   cmake --build <dir> --target larecomp_headlight_rig_test, then run src/gpu/larecomp_headlight_rig_test (exit 0 = passed).
add_executable(larecomp_headlight_rig_test EXCLUDE_FROM_ALL headlight_rig_test.cpp)
# The G-buffer's frozen interface (gbuffer_types.h, gbuffer_rules.h, light_set.h; it compiles hlsl/gbuffer_common.hlsli as C++ and compares it with the twin)
# and the host's pure rules (which draws get the sidecar, the frame window, the trigger) likewise:
#   cmake --build <dir> --target larecomp_gbuffer_rules_test, then run src/gpu/larecomp_gbuffer_rules_test [<hlsl/gbuffer_common.hlsli>] (exit 0 = passed).
add_executable(larecomp_gbuffer_rules_test EXCLUDE_FROM_ALL gbuffer_rules_test.cpp)
# The probe field's frame interface (gi_frame.h, CITY-RT M3 package G0: the budgets and the (ray tracing tier, level) cells, the cascades' toroidal addressing, the octahedral
# probe maps, the frame rows, the lookup with its weights, the ambient and the ground baseline; it compiles hlsl/gi_common.hlsli as C++ and compares it with the twin, and reads
# gbuffer_rules.h's octahedral map to check that they are the same) likewise:
#   cmake --build <build dir> --target larecomp_gi_frame_test, then run src/gpu/larecomp_gi_frame_test [-v] [<hlsl/gi_common.hlsli>] (exit 0 = passed).
add_executable(larecomp_gi_frame_test EXCLUDE_FROM_ALL gi_frame_test.cpp)
# The probe field owner's rules (gi_field_rules.h, CITY-RT M3 package G1: the memory of each cell and the order of loss, the windows' toroidal moves and their dirty sets, cuts, the fade,
# the state word and the weight rule, the reset list; it compiles hlsl/gi_field_common.hlsli as C++ and compares it with the twin, checks the traversal that finds the nearest probe sphere
# against a brute force, and reads the three gi_field_*.hlsl files for their registers) likewise:
#   cmake --build <build dir> --target larecomp_gi_field_rules_test, then run src/gpu/larecomp_gi_field_rules_test [-v] [<hlsl dir>] (exit 0 = passed).
add_executable(larecomp_gi_field_rules_test EXCLUDE_FROM_ALL gi_field_rules_test.cpp)
# The probe update's rules (gi_update_rules.h, CITY-RT M3 package G2: the ray set and its rotation, the ray record, the inside / back face classification, the blend with adaptive hysteresis, the scheduler
# (dirty and stale probes, the round robin, the safe range, the warm-up and the governor's scales), the governor, the budgets of the cells, the census lines; it compiles hlsl/gi_update_common.hlsli as C++
# and compares it with the twin and with gi_frame.h's IntegrateTexel, and reads the two shaders' registers, the CMake lines, host_shaders.h and the wiring) likewise:
#   cmake --build <dir> --target larecomp_gi_update_rules_test, then run src/gpu/larecomp_gi_update_rules_test [-v] [<src/gpu dir>] (exit 0 = passed).
add_executable(larecomp_gi_update_rules_test EXCLUDE_FROM_ALL gi_update_rules_test.cpp)
# The probe field's apply (gi_apply_rules.h, CITY-RT M3 package G3: the identity of an empty or open field, the ceiling and the cap, the parity with the screen-space bounce light's rules, the vehicle share,
# the SSGI switch, the upsample's weights, the buffer's rows, the leak test; it compiles hlsl/gi_apply_common.hlsli together with gi_common.hlsli as C++ and compares them with the twin, and reads the
# shader's registers, the CMake lines, host_shaders.h and the settings) likewise:
#   cmake --build <dir> --target larecomp_gi_apply_test, then run src/gpu/larecomp_gi_apply_test [-v] [<src/gpu dir>] (exit 0 = passed).
add_executable(larecomp_gi_apply_test EXCLUDE_FROM_ALL gi_apply_test.cpp)
# The player's cabin and the probe field's bounce light (gi_cabin_rules.h, INTERIOR-CAM-FIX: the interior camera's weight, the shares and the rows, the cabin's surfaces by the G-buffer's class and flag, the
# identity of every frame without a rule; it compiles hlsl/gi_cabin_common.hlsli as C++ and compares it with the twin, and reads the apply's shader, host, settings, debug row and the CMake lines) likewise:
#   cmake --build <dir> --target larecomp_gi_cabin_test, then run src/gpu/larecomp_gi_cabin_test [-v] [<src/gpu dir>] (exit 0 = passed).
add_executable(larecomp_gi_cabin_test EXCLUDE_FROM_ALL gi_cabin_test.cpp)
# The per-pixel detail pass (gi_pixel_rules.h, CITY-RT M3 package G5: the cell and the identity of the other cells, the governor, the plan, the ray's direction and noise, the estimator's pieces, the mix with the
# field, the surface lights picked from the light manager's frame, the denoiser's push block and the camera's motion; it compiles hlsl/gi_pixel_common.hlsli as C++ and compares it with the twin, and reads the
# shader, the host and the CMake lines) likewise:
#   cmake --build <dir> --target larecomp_gi_pixel_test, then run src/gpu/larecomp_gi_pixel_test [-v] [<src/gpu dir>] (exit 0 = passed).
add_executable(larecomp_gi_pixel_test EXCLUDE_FROM_ALL gi_pixel_test.cpp)
# The ray traced bounce light's setting and its pause row (gi_setting.h, CITY-RT M3 package G4: what a word comes to on each class and ray tracing tier, the gate that greys the row and what it says, AUTO's
# label, the launcher's line, the BOUNCE LIGHT rows' "(REPLACED)" and the strength row's rule, the debug feedback; it also reads gi_integration.h / .cpp, scene.cpp, renderer.cpp, pause_menu.cpp, the settings and
# the CMake lines for the order and the wiring) likewise:
#   cmake --build <dir> --target larecomp_gi_setting_test, then run src/gpu/larecomp_gi_setting_test [-v] [<src/gpu dir>] (exit 0 = passed; the folder defaults to the test's own, src/gpu).
add_executable(larecomp_gi_setting_test EXCLUDE_FROM_ALL gi_setting_test.cpp)
# RTAO-R3 begin
target_sources(larecomp_gpu PRIVATE rtao_setting.cpp)
# Check the Auto table, gate, labels and reader wiring, then run larecomp_rtao_setting_test [<src dir>].
add_executable(larecomp_rtao_setting_test EXCLUDE_FROM_ALL rtao_setting_test.cpp)
# RTAO-R3 end
# The one light description (light_frame.h: Light, LightFrame, the 64 byte GpuLight and its bits; light_id.h: the ids, a street lamp's being the old LampKey / KeyOf; light_laws.h: the
# headlight's, the game's lamp's and the volume's laws held to the rig's LampScale and the game's numbers; light_convert.h: the conversions from the headlight rig, the game's lamp
# list, the shadow replay and into the volume's lamp; and it compiles hlsl/light_laws.hlsli as C++ and compares it with the twin) likewise:
#   cmake --build <dir> --target larecomp_light_frame_test, then run src/gpu/larecomp_light_frame_test [<hlsl/light_laws.hlsli>] (exit 0 = passed).
add_executable(larecomp_light_frame_test EXCLUDE_FROM_ALL light_frame_test.cpp)
# The shared screen products' frozen interface (screen_frame.h, screen_rules.h: the products, the consumers' Want registry, the Hi-Z chain's sizes, the plan, the frame and its
# generation; it compiles hlsl/screen_hiz.hlsli as C++ and compares it with the twin, and reads the sources it is given) likewise:
#   cmake --build <dir> --target larecomp_screen_shared_test, then run src/gpu/larecomp_screen_shared_test <src/gpu dir> [--print] (exit 0 = passed).
add_executable(larecomp_screen_shared_test EXCLUDE_FROM_ALL screen_shared_test.cpp)
# The shared screen products' extraction (screen_extract.h, SS-2: the push blocks of the depth chain, the normal image and the colour chain built from the AO's frame block, held to a verbatim copy of the
# statements ao.cpp's old path records them with and to ao.cpp's own text, over a grid of frames; the pass list and the cost model, printed for 1080p and 4K) likewise:
#   cmake --build <dir> --target larecomp_screen_extract_test, then run src/gpu/larecomp_screen_extract_test <src/gpu dir> [--print] (exit 0 = passed).
add_executable(larecomp_screen_extract_test EXCLUDE_FROM_ALL screen_extract_test.cpp)
# The fused Hi-Z (screen_extract.h, SS-3: the depth chain's passes write the Hi-Z's first levels beside their images; hlsl/screen_hiz.hlsli and hlsl/ao_mip.hlsli compiled as C++, a whole frame's depth chain and
# Hi-Z made the separate way and the fused way on random depth buffers and compared bit for bit; the rules, the push blocks, the pass list and the bandwidth model printed for 1080p and 4K, the sources) likewise:
#   cmake --build <dir> --target larecomp_screen_hiz_fuse_test, then run src/gpu/larecomp_screen_hiz_fuse_test <src/gpu dir> [--print] (exit 0 = passed).
add_executable(larecomp_screen_hiz_fuse_test EXCLUDE_FROM_ALL screen_hiz_fuse_test.cpp)
# The screen-space reflections' rules (ssr_rules.h: levels, sizes, the decision, the push block, the wetness) and the pure HLSL (hlsl/ssr_common.hlsli and ssr_trace.hlsli compiled as C++: the class
# gate, the BRDF, the GGX sampling, the virtual image's reprojection, the Hi-Z march against analytic scenes and a brute force march) likewise; it reads the sources it is given:
#   cmake --build <dir> --target larecomp_ssr_test, then run src/gpu/larecomp_ssr_test <src/gpu dir> [--print] (exit 0 = passed).
add_executable(larecomp_ssr_test EXCLUDE_FROM_ALL ssr_test.cpp)
# The ray traced fallback of those reflections (ssr_rt_rules.h: the class groups, the confidence target, the candidates and the cap, the levels' decision and cells, the governor, the census; it compiles
# hlsl/ssr_rt_common.hlsli as C++ and compares it with the twins, and reads the fallback's sources) likewise:
#   cmake --build <dir> --target larecomp_ssr_rt_rules_test, then run src/gpu/larecomp_ssr_rt_rules_test <src/gpu dir> [--print] (exit 0 = passed).
add_executable(larecomp_ssr_rt_rules_test EXCLUDE_FROM_ALL ssr_rt_rules_test.cpp)
# RtDenoise (rt_denoise_rules.h: the radius law, the weights, the temporal rules; ssr_denoise_rules.h: the reflections' use, the decision, the setting, the census; it compiles
# hlsl/rt_denoise_common.hlsli as C++ and compares it with the twins, runs a CPU model of the two stages on synthetic frames, and reads the sources it is given) likewise:
#   cmake --build <dir> --target larecomp_rt_denoise_test, then run src/gpu/larecomp_rt_denoise_test <src/gpu dir> [--print] (exit 0 = passed).
add_executable(larecomp_rt_denoise_test EXCLUDE_FROM_ALL rt_denoise_test.cpp)
# The NaN guard (hlsl/nan_guard.hlsli: a NaN decided by its bits, the same on D3D12 and on Vulkan, whose max / min / clamp / saturate disagree about a NaN; it compiles the .hlsli as C++ and compares it with the
# standard library on every kind of NaN, the infinities and random bit patterns) and the places that use it (the reflections', the occlusion's, the volume's and the lights' shaders: the text of each place) likewise:
#   cmake --build <dir> --target larecomp_nan_guard_test, then run src/gpu/larecomp_nan_guard_test [<src/gpu dir>] (exit 0 = passed).
add_executable(larecomp_nan_guard_test EXCLUDE_FROM_ALL nan_guard_test.cpp)
# The frame's order (frame_order.h, LIGHT-UNIFY U9: the one table of every pass that makes or reads the shared screen and light data, what each makes, reads, must follow and Wants, and the recorder the
# Scene notes its trigger passes in) held to itself (every read has an earlier maker, every step moved before what it needs is found), to the consumers' own wants (gbuffer_rules.h, screen_rules.h), to a model of
# frames with passes missing and every kind of mistake, and, given the src/gpu folder (it defaults to the one the test is in), to scene.cpp, ao.cpp and the consumers' sources (the calls stand in the table's
# order, each has its Note line, the PassTiming labels and the Want calls exist) likewise:
#   cmake --build <dir> --target larecomp_frame_order_test, then run src/gpu/larecomp_frame_order_test [<src/gpu dir>] [--print] (exit 0 = passed).
add_executable(larecomp_frame_order_test EXCLUDE_FROM_ALL frame_order_test.cpp)
add_executable(larecomp_present_rules_test EXCLUDE_FROM_ALL present_rules_test.cpp)
add_executable(larecomp_swap_chain_rules_test EXCLUDE_FROM_ALL swap_chain_rules_test.cpp)
target_compile_features(larecomp_swap_chain_rules_test PRIVATE cxx_std_20)

# RTAO-R4 begin
# Compile the test, then run src/gpu/larecomp_rtao_integration_test [<src/gpu dir>] against the merged renderer sources.
add_executable(larecomp_rtao_integration_test EXCLUDE_FROM_ALL rtao_integration_test.cpp)
target_compile_features(larecomp_rtao_integration_test PRIVATE cxx_std_23)
# RTAO-R4 end
# The material channel (gbuffer_material.h: the class table of the 192 shader groups, the gloss table, the specular exponent's mapping to a smoothness, the material word, the table file the
# shader pack build writes and the draw's word) likewise; with the folder shaderpack/baseline (hlsl-rpf and hlsl-xex with their shader_report.tsv) it also checks the tables against the
# translated HLSL of the retail game:
#   cmake --build <dir> --target larecomp_gbuffer_material_test, then run src/gpu/larecomp_gbuffer_material_test [<shaderpack/baseline>] (exit 0 = passed).
add_executable(larecomp_gbuffer_material_test EXCLUDE_FROM_ALL gbuffer_material_test.cpp)
# The passive light manager's rules (light_manager_rules.h: the slot table, the collector that makes one LightFrame from the headlight rig, the replay's maps and the volume's street lamps, the
# GPU buffer, and the self-checks against the headlight block, the volume's inputs and the frame's own invariants, held to the real structs and the real lamp picker, and each field's
# deliberate difference found) likewise:
#   cmake --build <dir> --target larecomp_light_manager_test, then run src/gpu/larecomp_light_manager_test (exit 0 = passed).
add_executable(larecomp_light_manager_test EXCLUDE_FROM_ALL light_manager_test.cpp)
# The deferred local light pass's rules (local_lights_rules.h: which lights the pass takes from the frame and from the lamp A/B, the camera's rows and a pixel's world position, the game's night
# light intensity and height factor, the votes that pick them from the draws' constants, the scissor of a light, the buffer and the push block the shader reads; it compiles
# hlsl/local_light.hlsli as C++ and compares it with the twin, and reads the sources it is given: the shaders' CMake lines, the settings, the A/B's hook in mod_lights.cpp) likewise:
#   cmake --build <dir> --target larecomp_local_lights_test, then run src/gpu/larecomp_local_lights_test <src dir> (exit 0 = passed).
add_executable(larecomp_local_lights_test EXCLUDE_FROM_ALL local_lights_test.cpp)
# What feeds that pass besides the frame's lights (local_lights_sources.h, GB-H3: the police light bars, the other cars' head, rear, turn and reverse lamps, the mods' lamps the game's grid is not
# lighting, the budgets a class and a tier), held to the night probe's recorded points (the player, a racer, three traffic cars), to the lamp picker's score and to the one laws' pixel term;
# it also reads the sources it is given (the settings, the manager's cars, the mod loader's publish, the pass's wiring):
#   cmake --build <dir> --target larecomp_local_lights_sources_test, then run src/gpu/larecomp_local_lights_sources_test <src dir> (exit 0 = passed).
add_executable(larecomp_local_lights_sources_test EXCLUDE_FROM_ALL local_lights_sources_test.cpp)
# A car's neon underglow as lights of that pass (neon_light.h, GB-NEON: the decode of the game's car effect object laid out in a fake guest memory, the colour and the pulse as the game makes them and the
# console's 30 Hz hold at every frame rate, the tube and the two readings of its four points, the lights, their budget (whole cars, the tier, the pass's 32 and the priority), the pixel term held to
# the one laws, the table the hook fills, the ids, the one budget table); it includes hooks/neon_pulse.h for the pulse, and reads the sources it is given (the settings, the class table, the row, the hook, the wiring):
#   cmake --build <dir> --target larecomp_neon_light_test, then run src/gpu/larecomp_neon_light_test <src dir> (exit 0 = passed).
add_executable(larecomp_neon_light_test EXCLUDE_FROM_ALL neon_light_test.cpp)
target_include_directories(larecomp_neon_light_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
# The emitters of the ray traced bounce light (rt_emitters_rules.h, GI-BOUNCE-RT, perf/GI-BOUNCE.md: the cars' lamps and the other lights of the light pass's classes that light what a bounce ray hits: the groups and
# their switches, the light pass's own gathers and budgets, the night factor and the gains, the lights too far or with no colour taken out before the budgets, static first with the motion of each light by its id,
# the GpuLight rows; it compiles hlsl/rt_emitters.hlsli as C++ over the world rows' emitter records and holds RtEmitterRadiance to light_laws.h's LightTerm and RtEmitterTo to the geometry, and reads the sources
# it is given): no citypack sources
#   cmake --build <dir> --target larecomp_rt_emitters_rules_test, then run src/gpu/larecomp_rt_emitters_rules_test <src dir> (exit 0 = passed).
add_executable(larecomp_rt_emitters_rules_test EXCLUDE_FROM_ALL rt_emitters_rules_test.cpp)
target_include_directories(larecomp_rt_emitters_rules_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
# The rule that joins the screen-space and the ray traced halves of the lights that bounce (gi_bounce_carrier.h, GI-BOUNCE-JOIN: who carries a light's bounce at RT off / HIGH / ULTRA, with every group switch, so each light
# bounces exactly once; the partition of the light pass's draws; the lights-only chain's plan; given the src/gpu folder it also checks the text of the places that wire it, mutation-checked):
#   cmake --build <dir> --target larecomp_gi_bounce_carrier_test, then run src/gpu/larecomp_gi_bounce_carrier_test <src/gpu dir> (exit 0 = passed).
add_executable(larecomp_gi_bounce_carrier_test EXCLUDE_FROM_ALL gi_bounce_carrier_test.cpp)
target_include_directories(larecomp_gi_bounce_carrier_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
# The forward headlight stage against the deferred pass (headlight_pass_rules.h, GB-H2: the switch mcla_gpu_headlight_pass and what each route does, the retail census of the stage, the
# picture-identity rule and the retirement gate on made-up pictures, and the PIXEL IDENTITY: the pass's light, the one law and the one shadow lookup, equals the forward stage's CPU twins on random
# lights, surfaces and maps at all three levels of LIGHT SHADOWS; it also reads the sources it is given: the route's wiring in the rig, the pass and the pipelines, the setting, the debug row):
#   cmake --build <dir> --target larecomp_headlight_pass_test, then run src/gpu/larecomp_headlight_pass_test <src dir> (exit 0 = passed).
add_executable(larecomp_headlight_pass_test EXCLUDE_FROM_ALL headlight_pass_test.cpp)
# The consumers' shapes as projections of the light manager's frame (light_project.h, LIGHT-UNIFY U4: the headlight variant's constant block, the volume's light inputs, the lights the
# shadow replay picks from) held to the producers that make them today, byte for byte, over a recorded drive (the rig's real tracker, the replay's real rules at all three levels, the volume's
# real lamp picker; and the game's own recording when one is given), with each part of each shape deliberately changed and found:
#   cmake --build <dir> --target larecomp_light_project_test, then run src/gpu/larecomp_light_project_test [<path>.rig.txt] (exit 0 = passed).
add_executable(larecomp_light_project_test EXCLUDE_FROM_ALL light_project_test.cpp)
# The one shadow atlas's rules (shadow_atlas_rules.h: pools of tiles by provider laid out in one texture by the level, one pool = local_shadow::MakeAtlas bit for bit; the memory table; the fit of a level to
# the video memory; the one barrier for the pixel and the compute stage; the clear plan; slots that stay with a light; the published form and its read by light id), the provider interface
# (shadow_provider.h) and the ONE shadow lookup (light_shadow_lookup.h and its HLSL twin hlsl/light_shadow_lookup.hlsli, compiled as C++ by the test) held to the replay's own rules, the variant's
# CPU twin (headlight_rules::SoftShadow), the volume's (volumetrics_rules.h ShadowShare) and light_laws.h CombineShadow; light_project_test.cpp holds the published form to a recorded drive:
#   cmake --build <dir> --target larecomp_shadow_atlas_test, then run src/gpu/larecomp_shadow_atlas_test [<hlsl/light_shadow_lookup.hlsli>] (exit 0 = passed).
add_executable(larecomp_shadow_atlas_test EXCLUDE_FROM_ALL shadow_atlas_test.cpp)
# The street lamps' shadows, second try (lamp_shadows_rules.h, lamp_shadow_frame.h, lamp_shadow_lookup.h, hlsl/lamp_shadow_lookup.hlsli, LAMP-SHADOWS-4: the lamps' sources and ids, the game's
# half float, the level budgets and pools, the pick and its fades in scene seconds, the movers and the static layers' rules, the passes' plan, the published table, the pass's light and its pixel path
# in both modes, the buffer, the shader's text; the HLSL twin is compiled as C++ and compared bit for bit):
#   cmake --build <dir> --target larecomp_lamp_shadows_test, then run src/gpu/larecomp_lamp_shadows_test [<src/gpu folder>] (exit 0 = passed).
add_executable(larecomp_lamp_shadows_test EXCLUDE_FROM_ALL lamp_shadows_test.cpp)
# The one picker of the lights and the budget table (light_pick.h: the importance, the keep bonus, the fading and the held pick, the coast, all in seconds of the scene clock; light_budget.h: the
# headlight, shadow view and volume numbers of each level; light_pick_record.h: the text format of a recording of the pickers' inputs) held to the two pickers it replaced: the old suites' checks
# ported, a recorded drive through the rig's Tracker and the shadow replay's ViewPicker in both modes (the same picks), the same scenes at 30, 60 and 144 FPS, a switch of the mode in a run;
# given recordings the game made (mcla_gpu_light_pick_record) it replays those too:
#   cmake --build <dir> --target larecomp_light_pick_test, then run src/gpu/larecomp_light_pick_test [<path>.rig.txt] [<path>.views.txt] (exit 0 = passed).
add_executable(larecomp_light_pick_test EXCLUDE_FROM_ALL light_pick_test.cpp)
# The vehicles' light points, all 19 kinds (light_points.h: the hook's decoder and the table keyed by the light object; car_lights.h: the cars, their frames, sides, directions, bits and levels,
# the omni lights the lamps make, the census line; mod_light_source.h: a mod's lamps as candidates) held to the night probe's recorded points (the player, two racers, three traffic cars) likewise:
#   cmake --build <dir> --target larecomp_car_lights_test, then run src/gpu/larecomp_car_lights_test (exit 0 = passed).
add_executable(larecomp_car_lights_test EXCLUDE_FROM_ALL car_lights_test.cpp)
# The per-texture material table (gbuffer_materials.h: the text format and its parser, the keys, what a row does to a word, the files' hot reload, the census lines, the shader names file) likewise;
# with tools/gbuffer_materials/sample_table.txt (the file the Python tools' tests also read) it checks that the sample parses:
#   cmake --build <dir> --target larecomp_gbuffer_materials_test, then run src/gpu/larecomp_gbuffer_materials_test [<tools/gbuffer_materials/sample_table.txt>] (exit 0 = passed).
add_executable(larecomp_gbuffer_materials_test EXCLUDE_FROM_ALL gbuffer_materials_test.cpp)
# The other cars' lamps in a ray traced reflection (car_rt_lights_rules.h, LIGHT-UNIFY LU-8: the glow law of each lamp family, the lamp set, the lens's glow from the lamps that stand in its mesh's
# box, the record's driven bit, AppendOthers' use of the lamps, the light points' consumers; and that hlsl/car_rt_hit_shade.hlsli, the host and the settings hold the header's numbers) likewise; run it
# with the src folder or from there:
#   cmake --build <dir> --target larecomp_car_rt_lights_rules_test, then run src/gpu/larecomp_car_rt_lights_rules_test [<src dir>] (exit 0 = passed).
add_executable(larecomp_car_rt_lights_rules_test EXCLUDE_FROM_ALL car_rt_lights_rules_test.cpp)
# The car frame's rules (car_frame_rules.h: the mesh key, which draw is the player's, the matrix and the camera out of a draw's bank, the
# normals, the boxes) likewise:
#   cmake --build <dir> --target larecomp_car_frame_rules_test, then run src/gpu/larecomp_car_frame_rules_test (exit 0 = passed).
add_executable(larecomp_car_frame_rules_test EXCLUDE_FROM_ALL car_frame_rules_test.cpp)
# The car-only sun shadow map's rules (car_shadow_map_rules.h: the setting, the car's frame, the fit and its held centre, the matrices, the pipeline
# variants) likewise:
#   cmake --build <dir> --target larecomp_car_shadow_map_rules_test, then run src/gpu/larecomp_car_shadow_map_rules_test (exit 0 = passed).
add_executable(larecomp_car_shadow_map_rules_test EXCLUDE_FROM_ALL car_shadow_map_rules_test.cpp)
# The headlight shadow maps' rules (local_shadow_rules.h: the setting, the atlas, the lights' groups and views, the matrices and the replay's c8-c11,
# the camera out of a draw's bank, which draws are casters, the face culling and the pipeline key) likewise:
#   cmake --build <dir> --target larecomp_local_shadow_rules_test, then run src/gpu/larecomp_local_shadow_rules_test (exit 0 = passed).
add_executable(larecomp_local_shadow_rules_test EXCLUDE_FROM_ALL local_shadow_rules_test.cpp)
# The headlight shadows' lookup (light_shadow_filter.h, the CPU twin of the lit shaders' HeadlightShadow: four gathers at the receiver's plane, contact hardening, the
# fade at the tile's border) on ray cast maps of a road, a post, a kerb and a wall, against the first version's box, and the levels' sizes (HL-SHADOW-QUALITY):
#   cmake --build <dir> --target larecomp_light_shadow_filter_test, then run src/gpu/larecomp_light_shadow_filter_test (exit 0 = passed).
add_executable(larecomp_light_shadow_filter_test EXCLUDE_FROM_ALL light_shadow_filter_test.cpp)
# The player's car as ray tracing structures (car_rt_scene_rules.h: the instance transform and the camera's z, the masks, the mesh clean-up, the
# range allocator, the eviction policy, the check pass's statistics; given the src/gpu folder it also checks that hlsl/car_rt_check_cs.hlsl and
# hlsl/car_rt_common.hlsli use the header's numbers) likewise:
#   cmake --build <dir> --target larecomp_car_rt_scene_rules_test, then run src/gpu/larecomp_car_rt_scene_rules_test <src/gpu dir> (exit 0 = passed).
add_executable(larecomp_car_rt_scene_rules_test EXCLUDE_FROM_ALL car_rt_scene_rules_test.cpp)
# The city as ray tracing structures (city_rt_rules.h, CITY-RT M1: the setting and the class budgets, the instance mask against the car scene's, the streaming plan
# with its hysteresis and memory cap on a synthetic city, the frame's build schedule) likewise:
#   cmake --build <dir> --target larecomp_city_rt_rules_test, then run src/gpu/larecomp_city_rt_rules_test (exit 0 = passed).
add_executable(larecomp_city_rt_rules_test EXCLUDE_FROM_ALL city_rt_rules_test.cpp)
# The ray tracing scene service's rules (rt_scene_rules.h, CITY-RT M2 H3: the Want lifecycle, the merge of the consumers, the top level's plan with and without the player's car, the
# camera predictor on made-up drives) likewise:
#   cmake --build <dir> --target larecomp_rt_scene_rules_test, then run src/gpu/larecomp_rt_scene_rules_test (exit 0 = passed).
add_executable(larecomp_rt_scene_rules_test EXCLUDE_FROM_ALL rt_scene_rules_test.cpp)
# The city hit data's pool (city_hit_pool_rules.h, CITY-RT M2 H3: the budgets, the pool's cap in the streaming plan, the allocator, the sector table, the book with its deferred frees,
# 30,000 random operations, the probe's checksum, and the chain from a block through the table to the decode); given the src/gpu folder it also checks hlsl/city_hit_probe_cs.hlsl:
#   cmake --build <dir> --target larecomp_city_hit_pool_rules_test, then run src/gpu/larecomp_city_hit_pool_rules_test [<src/gpu dir>] (exit 0 = passed).
add_executable(larecomp_city_hit_pool_rules_test EXCLUDE_FROM_ALL city_hit_pool_rules_test.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/../citypack/city_hit_format.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../citypack/pack_format.cpp")
target_include_directories(larecomp_city_hit_pool_rules_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_compile_features(larecomp_city_hit_pool_rules_test PRIVATE cxx_std_23)
# The city texture set's rules (city_texture_rules.h, CITY-RT M2 H2 finished for M1c: the bindless slot maths, the footprints of every level against Plume's D3D12 arithmetic, the memory a
# texture takes, the budgets by class, level and scope, the pressure rule, a sector's two texture lists, and the loader's conversation with a fake device over a retail-sized table with
# the residency's invariants held at every frame) likewise:
#   cmake --build <dir> --target larecomp_city_texture_rules_test, then run src/gpu/larecomp_city_texture_rules_test (exit 0 = passed).
add_executable(larecomp_city_texture_rules_test EXCLUDE_FROM_ALL city_texture_rules_test.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/../citypack/city_hit_format.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../citypack/pack_format.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../citypack/texture_residency.cpp")
target_include_directories(larecomp_city_texture_rules_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_compile_features(larecomp_city_texture_rules_test PRIVATE cxx_std_23)
# The exact alpha of the city's cut-outs (hlsl/city_hit_alpha.hlsli compiled as C++ against its twin city_hit_alpha_rules.h on a synthetic city of fences, every way a candidate's alpha is not
# known, the shadow shader's wiring read from its text); given the src/gpu folder it reads the shader sources:
#   cmake --build <dir> --target larecomp_city_hit_alpha_rules_test, then run src/gpu/larecomp_city_hit_alpha_rules_test [<src/gpu dir>] (exit 0 = passed).
add_executable(larecomp_city_hit_alpha_rules_test EXCLUDE_FROM_ALL city_hit_alpha_rules_test.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/../citypack/city_hit_format.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../citypack/pack_format.cpp")
target_include_directories(larecomp_city_hit_alpha_rules_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_compile_features(larecomp_city_hit_alpha_rules_test PRIVATE cxx_std_23)
# What every hit shading shares (CITY-RT M2 H4: rt_hit_frame.h and hlsl/rt_hit_frame.hlsli + hlsl/rt_hit_sample.hlsli compiled as C++ through hlsl_cpp_shim.h and checked against it: the frame's rows,
# the cascades' lookup, the environment map's mapping, a texture read by slot with the game's gamma; the exposure calibration of rt_hit_calibration.h, the car's numbers unchanged and the city's
# per class gains); given the src folder it also reads the shader files' text:
#   cmake --build <dir> --target larecomp_rt_hit_frame_test, then run src/gpu/larecomp_rt_hit_frame_test <src dir> (exit 0 = passed).
add_executable(larecomp_rt_hit_frame_test EXCLUDE_FROM_ALL rt_hit_frame_test.cpp)
target_include_directories(larecomp_rt_hit_frame_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_compile_features(larecomp_rt_hit_frame_test PRIVATE cxx_std_23)
# The city hit library (hlsl/city_hit_shade.hlsli compiled as C++ against its twin city_hit_shade_rules.h on a synthetic city of a wall, a road, glass, a lamp lens, metal, a wet road, water, a neon
# sign and a lit window, thousands of random rays, frames and parameters: CityHitShade, CityHitResolve, the rays they cast, the calibration's counters; the twin's laws: the texture's level, the lamps'
# laws and the estimator of the lamp choice, the sun ray, the fresnel, the screen's weight, the mix, the lamp's GPU record; and (CITY-RT R7, city_hit_lamp_term_lib.inc and
# city_hit_lamp_term_checks.inc) the library once more with the world frame's lamp grid and glow schedule: the lamp term at hits against the twin, its caps, the glow kinds, the emissive calibration);
# given the src folder it also reads the library's text:
#   cmake --build <dir> --target larecomp_city_hit_shade_rules_test, then run src/gpu/larecomp_city_hit_shade_rules_test <src dir> (exit 0 = passed).
add_executable(larecomp_city_hit_shade_rules_test EXCLUDE_FROM_ALL city_hit_shade_rules_test.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/../citypack/city_hit_format.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../citypack/lamp_merge.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../citypack/lamp_table.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../citypack/pack_format.cpp")
target_include_directories(larecomp_city_hit_shade_rules_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_compile_features(larecomp_city_hit_shade_rules_test PRIVATE cxx_std_23)
# The city in the car paint's reflections (CITY-RT M2 R1, R2: city_refl_rules.h: the mask, the levels and cells by class and tier, the opponents' budget, the lamp schedule, the constants rows the shader reads,
# the counters and the census lines; given the src folder it also checks that hlsl/city_refl.hlsli, car_rt_refl_cs.hlsl, the host and the settings hold the header's numbers):
#   cmake --build <dir> --target larecomp_city_refl_rules_test, then run src/gpu/larecomp_city_refl_rules_test <src dir> (exit 0 = passed).
add_executable(larecomp_city_refl_rules_test EXCLUDE_FROM_ALL city_refl_rules_test.cpp)
target_include_directories(larecomp_city_refl_rules_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_compile_features(larecomp_city_refl_rules_test PRIVATE cxx_std_23)
# The player's car windows reflecting the city (CITY-RT M2 R3b: car_glass_rules.h: the Fresnel, the primary ray's reach, the colour a pane adds, the dispatch's rectangle, the decision, the apply pass's
# arithmetic; given the src folder it checks that the shaders, the host, the CMake files and the settings hold the header's numbers and are wired as the design says) likewise:
#   cmake --build <dir> --target larecomp_car_glass_rules_test, then run src/gpu/larecomp_car_glass_rules_test <src dir> (exit 0 = passed).
add_executable(larecomp_car_glass_rules_test EXCLUDE_FROM_ALL car_glass_rules_test.cpp)
target_include_directories(larecomp_car_glass_rules_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_compile_features(larecomp_car_glass_rules_test PRIVATE cxx_std_23)

# The city reflections' setting and its pause row (city_refl_setting.h, CITY-RT M2 package R8: what a word comes to on each class and ray tracing tier, the gate that greys the row and what it says, how the
# state of the city's files becomes a word, the interplay with the ray traced CAR SELF-REFLECTIONS row, AUTO's label, the page width; it also reads the scenes, the host, pause_menu.cpp, the settings and the CMake
# lines for the wiring) likewise:
#   cmake --build <dir> --target larecomp_city_refl_setting_test, then run src/gpu/larecomp_city_refl_setting_test [-v] [<src/gpu dir>] (exit 0 = passed; the folder defaults to the test's own, src/gpu).
add_executable(larecomp_city_refl_setting_test EXCLUDE_FROM_ALL city_refl_setting_test.cpp city_refl_setting.cpp)
target_include_directories(larecomp_city_refl_setting_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_compile_features(larecomp_city_refl_setting_test PRIVATE cxx_std_23)
# The ray tracing world frame (CITY-RT package FRAME: rt_world_frame_rules.h, hlsl/rt_world_frame.hlsli compiled as C++ through hlsl_cpp_shim.h and checked against it: the two buffers' layout and
# packing, the lamp schedule (the on hours and the night curve), the merge of the static lamp table with the game's live list, the budget, the 64 m grid, the lookup against brute force, the
# lamp light through the grid against the library's twin; given the src folder it also reads the shader files' text, the descriptor counts of the car scene's set and the wiring):
#   cmake --build <dir> --target larecomp_rt_world_frame_test, then run src/gpu/larecomp_rt_world_frame_test <src dir> (exit 0 = passed).
add_executable(larecomp_rt_world_frame_test EXCLUDE_FROM_ALL rt_world_frame_test.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/../citypack/lamp_merge.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../citypack/lamp_table.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../citypack/city_hit_format.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../citypack/pack_format.cpp")
target_include_directories(larecomp_rt_world_frame_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_compile_features(larecomp_rt_world_frame_test PRIVATE cxx_std_23)
# The city's ray traced sun shadows' maths (city_rt_shadow_rules.h: the sun, the disc samples, the lift, the alpha rule, the noise hash; a brute force tracer over made-up
# triangles checks the penumbra of a roof's edge against the exact disc integral, contact hardening, no acne on a plane at every sun height; given the src/gpu folder it also
# checks that hlsl/city_rt_shadow_cs.hlsl holds the header's numbers) likewise:
#   cmake --build <dir> --target larecomp_city_rt_shadow_rules_test, then run src/gpu/larecomp_city_rt_shadow_rules_test <src/gpu dir> (exit 0 = passed).
add_executable(larecomp_city_rt_shadow_rules_test EXCLUDE_FROM_ALL city_rt_shadow_rules_test.cpp)
# What the city's ray tracing needs to be right on Vulkan (CITY-VK), read from the sources: every shader that traces is built through larecomp_gpu_host_shader_rt (SPIR-V 1.5 for
# Vulkan 1.2), the format of every typed storage image in the shader (Vulkan takes it from there) is the one the C++ makes the image in, the alignments of the structures' inputs.
# Takes the src/gpu folder (default: the folder of the test's own source):
#   cmake --build <dir> --target larecomp_city_rt_vulkan_test, then run src/gpu/larecomp_city_rt_vulkan_test [<src/gpu dir>] (exit 0 = passed).
add_executable(larecomp_city_rt_vulkan_test EXCLUDE_FROM_ALL city_rt_vulkan_test.cpp)
target_include_directories(larecomp_city_rt_vulkan_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_compile_features(larecomp_city_rt_vulkan_test PRIVATE cxx_std_23)
# The ray traced visibility of the top K lights (RT-LIGHTS-D1: rt_lights_rules.h, hlsl/rt_lights_common.hlsli and hlsl/rt_mask_read.hlsli compiled as C++ through hlsl_cpp_shim.h: the top K, the channels, the
# rows, the composition with the identity, the ray's geometry, the noise rule, the mask read against brute force; given the src folder it also reads the shader, the host class, the settings and the wiring) likewise:
#   cmake --build <dir> --target larecomp_rt_lights_test, then run src/gpu/larecomp_rt_lights_test <src dir> (exit 0 = passed).
add_executable(larecomp_rt_lights_test EXCLUDE_FROM_ALL rt_lights_test.cpp)
target_include_directories(larecomp_rt_lights_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_compile_features(larecomp_rt_lights_test PRIVATE cxx_std_23)
# The other cars in the car ray tracing scene (car_rt_instances_rules.h: the limits by class, which cars are kept, a car's body centre, the budgets of a
# car that is new, the instance list) likewise:
#   cmake --build <dir> --target larecomp_car_rt_instances_rules_test, then run src/gpu/larecomp_car_rt_instances_rules_test (exit 0 = passed).
add_executable(larecomp_car_rt_instances_rules_test EXCLUDE_FROM_ALL car_rt_instances_rules_test.cpp)
# The cars the camera does not see (car_rt_memory_rules.h: the game's vehicle poses, the match of the drawn cars to the game's vehicles, a car's draws kept
# relative to its vehicle and placed again at its matrix, which kits are placed and when one goes, and the instance list with the placed cars) likewise:
#   cmake --build <dir> --target larecomp_car_rt_memory_rules_test, then run src/gpu/larecomp_car_rt_memory_rules_test (exit 0 = passed).
add_executable(larecomp_car_rt_memory_rules_test EXCLUDE_FROM_ALL car_rt_memory_rules_test.cpp)
# The walk through a car's own model data (car_model_walk_rules.h: the vehicle -> entity -> model info -> model set -> part table -> drawable -> LOD -> model ->
# geometry -> buffers chain on made-up bytes, the declaration, the 16-bit and 32-bit index formats, the bone index and palette, what is in use, and the comparison
# with a frame's draws) likewise:
#   cmake --build <dir> --target larecomp_car_model_walk_rules_test, then run src/gpu/larecomp_car_model_walk_rules_test (exit 0 = passed).
add_executable(larecomp_car_model_walk_rules_test EXCLUDE_FROM_ALL car_model_walk_rules_test.cpp)
# The ray traced scene's cars built from the cars' own model data (car_model_rt_rules.h, RT-MODEL-CARS phase 2: the game's lod rule against the first check run's
# samples, the draw loop's part rules, the five placement hypotheses and the calibration against the draws, the view-space conversion and the error of a walked
# instance against a draw's, the mesh read from the car's buffers and its key, the paint, which cars are staged and placed, the budgets and the cleanup) likewise:
#   cmake --build <dir> --target larecomp_car_model_rt_rules_test, then run src/gpu/larecomp_car_model_rt_rules_test (exit 0 = passed).
add_executable(larecomp_car_model_rt_rules_test EXCLUDE_FROM_ALL car_model_rt_rules_test.cpp)
# The wheels of a walked car (car_model_wheel_rules.h, car_model_parts_rt_rules.h, RT-MODEL-PARTS: the wheel manager's tyre and rim by the game's accessors, the wheel draw's flags,
# the geometries added to a vehicle's walk, the four hypotheses for where a wheel stands and the calibration against the draws, one placement per wheel, the kinds of what a car
# brings) likewise:
#   cmake --build <dir> --target larecomp_car_model_wheel_rules_test, then run src/gpu/larecomp_car_model_wheel_rules_test (exit 0 = passed).
add_executable(larecomp_car_model_wheel_rules_test EXCLUDE_FROM_ALL car_model_wheel_rules_test.cpp)
# The traffic and parked cars as walk sources (car_model_traffic_rules.h, car_model_parts_rt_rules.h, RT-MODEL-PARTS: the renderer's ambient client's items, an item's car and model,
# the walk of the model's drawable, the pose block's root and bones, the hypotheses for a bone's placement and the calibration, the traffic's own count and the racers' and the
# traffic's cars chosen apart) likewise:
#   cmake --build <dir> --target larecomp_car_model_traffic_rules_test, then run src/gpu/larecomp_car_model_traffic_rules_test (exit 0 = passed).
add_executable(larecomp_car_model_traffic_rules_test EXCLUDE_FROM_ALL car_model_traffic_rules_test.cpp)
# The ray traced occlusion and contact shadows around the player's car (car_rt_ao_rules.h: the setting, the rays and their lengths, the cosine pattern, the
# car's box in view space and the pixel rectangle of it, the slab test, the flags; given the src/gpu folder it also checks that hlsl/car_rt_ao_cs.hlsl,
# ao_main_ps.hlsl and ao_contact_ps.hlsl use the header's numbers and flags) likewise:
#   cmake --build <dir> --target larecomp_car_rt_ao_rules_test, then run src/gpu/larecomp_car_rt_ao_rules_test <src/gpu dir> [--print] (exit 0 = passed).
add_executable(larecomp_car_rt_ao_rules_test EXCLUDE_FROM_ALL car_rt_ao_rules_test.cpp)
# The plain side mirrors' setting and its pause row (mirror_setting.h, MIRROR-VIEW.md MV6: what each word of mcla_gpu_side_mirrors comes to on every class, handheld flag and ray tracing tier next to every
# word of the traced mirrors' setting and every value of the two debug switches under it, that the traced mirrors win exactly where the pass's own Wanted says, the class table's cells, the levels' size and rate,
# the row's words and page width; it also reads pause_menu.cpp, the settings, gpu_class.h, the readers and the CMake lines for the wiring) likewise:
#   cmake --build <dir> --target larecomp_mirror_setting_test, then run src/gpu/larecomp_mirror_setting_test [-v] [--table] [<src/gpu dir>] (exit 0 = passed; the folder defaults to the test's own, src/gpu).
add_executable(larecomp_mirror_setting_test EXCLUDE_FROM_ALL mirror_setting_test.cpp)
target_include_directories(larecomp_mirror_setting_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_compile_features(larecomp_mirror_setting_test PRIVATE cxx_std_23)
# The camera cut rule (temporal_camera.h, temporal_cut.cpp) is pure and has a check of its own, on camera
# sequences recorded from the game's logs and made up:
#   cmake --build <dir> --target larecomp_camera_cut_test, then run src/gpu/larecomp_camera_cut_test
#   (exit 0 = passed; with a camera trace csv as the argument it also reports the cuts in that trace).
add_executable(larecomp_camera_cut_test EXCLUDE_FROM_ALL camera_cut_test.cpp temporal_cut.cpp)
# The upscalers' pure arithmetic (upscaler_math.h: the jitter sequence, its length at a ratio, the scene's mip bias) has a check that needs
# no renderer: cmake --build <dir> --target larecomp_upscaler_math_test, then run src/gpu/larecomp_upscaler_math_test (exit 0 = passed).
add_executable(larecomp_upscaler_math_test EXCLUDE_FROM_ALL upscaler_math_test.cpp)
# The render scale's choice (render_scale.h: the asked scale, the screen's, an FSR1 preset, the upscaler's output, the scene's scale under an upscaler),
# which the renderer asks again at every frame boundary and moves the images of the screen to, has a check that needs no renderer:
#   cmake --build <dir> --target larecomp_render_scale_test, then run src/gpu/larecomp_render_scale_test (exit 0 = passed).
add_executable(larecomp_render_scale_test EXCLUDE_FROM_ALL render_scale_test.cpp)
# The host MSAA level's rules (msaa_plan.h: what mcla_gpu_msaa says, the device's limit, and when MsaaLevel makes a live change: the settle, the pipelines'
# wait, the retries after a failure, the upscaler's own path) are checked on a made-up clock, with no renderer:
#   cmake --build <dir> --target larecomp_msaa_plan_test, then run src/gpu/larecomp_msaa_plan_test (exit 0 = passed).
add_executable(larecomp_msaa_plan_test EXCLUDE_FROM_ALL msaa_plan_test.cpp)
# The anisotropic filtering rules (aniso_rules.h: what mcla_gpu_anisotropy says, which of the game's samplers take it, the level a sampler is made with;
# the setting is live) have a check that needs no renderer:
#   cmake --build <dir> --target larecomp_aniso_rules_test, then run src/gpu/larecomp_aniso_rules_test (exit 0 = passed).
add_executable(larecomp_aniso_rules_test EXCLUDE_FROM_ALL aniso_rules_test.cpp)
# The upscalers' words, mode tables and ratios (upscale_modes.h: mcla_gpu_upscaler and the three mode settings, which the temporal system reads at every
# frame boundary, and the scene's size each mode gives at an output) have a check that needs no renderer:
#   cmake --build <dir> --target larecomp_upscale_modes_test, then run src/gpu/larecomp_upscale_modes_test (exit 0 = passed).
add_executable(larecomp_upscale_modes_test EXCLUDE_FROM_ALL upscale_modes_test.cpp)
# The rules for when the upscalers' libraries are loaded (upscaler_load_policy.h) have a check that needs no renderer:
#   cmake --build <dir> --target larecomp_upscaler_load_policy_test, then run src/gpu/larecomp_upscaler_load_policy_test (exit 0 = passed).
add_executable(larecomp_upscaler_load_policy_test EXCLUDE_FROM_ALL upscaler_load_policy_test.cpp)
find_package(Threads REQUIRED)
target_link_libraries(larecomp_upscaler_load_policy_test PRIVATE Threads::Threads)
# The side views' scale rules (side_views.h: the shadow, car reflection and road reflection detail rows, which are live: a setting's word to a level on each
# GPU class, a level to a scale, the step down when the video memory is short, the class that follows the render scale, the declared sizes) have a check that
# needs no renderer: cmake --build <dir> --target larecomp_side_view_scales_test, then run src/gpu/larecomp_side_view_scales_test (exit 0 = passed).
add_executable(larecomp_side_view_scales_test EXCLUDE_FROM_ALL side_view_scales_test.cpp)
# DLSS Ray Reconstruction's pure rules (dlss_rr_rules.h: the tag plan, the options, the choice of denoiser, the frame gate) have a check that needs no renderer
# and no Streamline: cmake --build <dir> --target larecomp_dlss_rr_rules_test, then run src/gpu/larecomp_dlss_rr_rules_test (exit 0 = passed).
add_executable(larecomp_dlss_rr_rules_test EXCLUDE_FROM_ALL dlss_rr_rules_test.cpp)
# DLSS Ray Reconstruction's guide images (rr_guides_rules.h: the images, the views, who runs the pass, the push blocks, and the C++ twin of hlsl/rr_guides_common.hlsli, which the test also
# compiles as C++): cmake --build <dir> --target larecomp_rr_guides_test, then run src/gpu/larecomp_rr_guides_test [<src/gpu dir>] (exit 0 = passed).
add_executable(larecomp_rr_guides_test EXCLUDE_FROM_ALL rr_guides_test.cpp)
# The DENOISER setting (denoiser_setting.h, RR-5: the words, the gate that greys the row and what it says, AUTO's label, the consumers' rule, the launcher's text, and the text of the wiring: the row in
# upscaling_menu.cpp, the plan in temporal.cpp, the plugin request in temporal_dlss.cpp, the three settings files, the CMake lines): cmake --build <dir> --target larecomp_denoiser_setting_test, then run
# src/gpu/larecomp_denoiser_setting_test [-v] [<src/gpu dir>] (exit 0 = passed).
add_executable(larecomp_denoiser_setting_test EXCLUDE_FROM_ALL denoiser_setting_test.cpp)

# AMD FSR Ray Regeneration's pure rules (ray_regen_rules.h): the AMD ids, the signals and consumers, the plan beside DLSS RR's, the input map, the dispatch plan, the conventions, the memory estimate and
# the row's words. No renderer, no Windows, no AMD runtime: cmake --build <dir> --target larecomp_ray_regen_rules_test, then run src/gpu/larecomp_ray_regen_rules_test (exit 0 = passed).
add_executable(larecomp_ray_regen_rules_test EXCLUDE_FROM_ALL ray_regen_rules_test.cpp)
# The guide conversion's pure rules and the C++ twin of hlsl/ray_regen_common.hlsli (ray_regen_guides_rules.h): the images, the frame decision, the camera of the dispatch, the push block, and the texels' arithmetic
# against the shader's own text: cmake --build <dir> --target larecomp_ray_regen_guides_test, then run src/gpu/larecomp_ray_regen_guides_test [<src/gpu dir>] (exit 0 = passed).
add_executable(larecomp_ray_regen_guides_test EXCLUDE_FROM_ALL ray_regen_guides_test.cpp)
# The rule that gives another pass's use of a side view's EDRAM identity (the night shadow's, which binds the car
# reflections' depth through a surface of another size) a target of its own (side_views.h):
#   cmake --build <dir> --target larecomp_side_view_use_test, then run src/gpu/larecomp_side_view_use_test <src/gpu dir>.
add_executable(larecomp_side_view_use_test EXCLUDE_FROM_ALL side_view_use_test.cpp)
# The camera cut requests and the motion blur constants of a cut (cut_request.h) have one too:
#   cmake --build <dir> --target larecomp_cut_request_test, then run src/gpu/larecomp_cut_request_test (exit 0 = passed).
add_executable(larecomp_cut_request_test EXCLUDE_FROM_ALL cut_request_test.cpp cut_request.cpp)
target_include_directories(larecomp_cut_request_test PRIVATE ${CMAKE_SOURCE_DIR}/src)  # settings/immortal.h (vol_lamp_list.h, cut_request.cpp)
# The rear view mirror's draw class (mirror_draws.h: the rectangle the guest publishes and the viewport rule) likewise:
#   cmake --build <dir> --target larecomp_mirror_draws_test, then run src/gpu/larecomp_mirror_draws_test (exit 0 = passed).
add_executable(larecomp_mirror_draws_test EXCLUDE_FROM_ALL mirror_draws_test.cpp)
# The shared rear view's rules (mirror_view_rules.h: the reflection and lookup of a side mirror pixel, the parallax error, the view's field and size, the viewport redirect, the HUD's crop, and MV1's markers, phase, route of a draw, life and maps into the view; perf/MIRROR-VIEW.md)
# have a check that needs no renderer; `--table` prints the design's tables:
#   cmake --build <dir> --target larecomp_mirror_view_rules_test, then run src/gpu/larecomp_mirror_view_rules_test (exit 0 = passed).
add_executable(larecomp_mirror_view_rules_test EXCLUDE_FROM_ALL mirror_view_rules_test.cpp)
# The side mirrors' glass found without ray tracing (mirror_glass_rules.h: the gate of the draws worth reading, the cache by mesh_key, the pair among synthetic cars, the need rule; mirror_glass_need.h: the flag
# the guest reads and its hold; and that the listener, the setting, the scene's calls and these build lines are there; perf/MIRROR-VIEW.md MV3) likewise:
#   cmake --build <dir> --target larecomp_mirror_glass_rules_test, then run src/gpu/larecomp_mirror_glass_rules_test [<src dir>] (exit 0 = passed); `--table` prints the pair's verdict for a few cameras.
add_executable(larecomp_mirror_glass_rules_test EXCLUDE_FROM_ALL mirror_glass_rules_test.cpp)
add_executable(larecomp_cabin_mirror_rules_test EXCLUDE_FROM_ALL cabin_mirror_rules_test.cpp)
# The JOIN of the shared rear view's three halves (mirror_view_join_test.cpp: the one switch over the guest's rules and the host's, the request published once a frame with the finder's need and its hold,
# the racers, and that the files hold one link header, one publisher and one switch; perf/MIRROR-VIEW.md section 12) likewise:
#   cmake --build <dir> --target larecomp_mirror_view_join_test, then run src/gpu/larecomp_mirror_view_join_test [<src dir>] (exit 0 = passed).
add_executable(larecomp_mirror_view_join_test EXCLUDE_FROM_ALL mirror_view_join_test.cpp)
target_include_directories(larecomp_mirror_view_join_test PRIVATE ${CMAKE_SOURCE_DIR}/src)  # mc_engine/hooks/rear_view_shared.h
# The side mirrors' glass overlay (mirror_overlay_rules.h: the hand-off with the ray traced side mirrors and the rest of the frame's decision, the camera and the pose of the view, the quaternion, the depth test, the pipeline
# variants, the 128 byte push block; hlsl/mirror_overlay.hlsli compiled as C++ through hlsl_cpp_shim.h and held against the design's UvForMirrorPixel; and that the shaders, the setting, the scene's calls, the precache and these
# build lines are there; perf/MIRROR-VIEW.md MV4) likewise:
#   cmake --build <dir> --target larecomp_mirror_overlay_rules_test, then run src/gpu/larecomp_mirror_overlay_rules_test [<src dir>] (exit 0 = passed).
add_executable(larecomp_mirror_overlay_rules_test EXCLUDE_FROM_ALL mirror_overlay_rules_test.cpp)
# The HUD fade's alpha clamp and ramp (ui_fade.h) have a check that needs no renderer:
#   cmake --build <dir> --target larecomp_ui_fade_test, then run src/gpu/larecomp_ui_fade_test (exit 0 = passed).
add_executable(larecomp_ui_fade_test EXCLUDE_FROM_ALL ui_fade_test.cpp)
# The UI sharpening's rules (ui_sharp.h: the strength, the constants, which draws take the variant, the census) likewise:
#   cmake --build <dir> --target larecomp_ui_sharp_rules_test, then run src/gpu/larecomp_ui_sharp_rules_test (exit 0 = passed).
add_executable(larecomp_ui_sharp_rules_test EXCLUDE_FROM_ALL ui_sharp_test.cpp)
# The before/after split's columns and spans (split.h) have a check that needs no renderer:
#   cmake --build <dir> --target larecomp_split_test, then run src/gpu/larecomp_split_test (exit 0 = passed).
add_executable(larecomp_split_test EXCLUDE_FROM_ALL split_test.cpp)
# The upscalers' masks (upscaler_mask_plan.h: which views get second draws and with how many samples, the mask pass as a function) have a
# check that needs no renderer: cmake --build <dir> --target larecomp_upscaler_masks_test, then run src/gpu/larecomp_upscaler_masks_test.
add_executable(larecomp_upscaler_masks_test EXCLUDE_FROM_ALL upscaler_masks_test.cpp)
# The video memory budget of a GPU that shares memory with the CPU (uma_budget.h) has one too:
#   cmake --build <dir> --target larecomp_uma_budget_test, then run src/gpu/larecomp_uma_budget_test (exit 0 = passed).
add_executable(larecomp_uma_budget_test EXCLUDE_FROM_ALL uma_budget_test.cpp)
# The room rules (vram_room.h: the line a context asks before it is made, the upscaler's need, the wait after a refusal, the upscaler
# that fits) have one too:
#   cmake --build <dir> --target larecomp_vram_room_test, then run src/gpu/larecomp_vram_room_test (exit 0 = passed).
add_executable(larecomp_vram_room_test EXCLUDE_FROM_ALL vram_room_test.cpp)
# The level seeds' rules (level_seeds.h: which saved records are made ahead for a new MSAA level, the memory bound, the queue rules, the
# run's counts) have one too: no renderer, no device.
#   cmake --build <dir> --target larecomp_level_seeds_test, then run src/gpu/larecomp_level_seeds_test (exit 0 = passed).
add_executable(larecomp_level_seeds_test EXCLUDE_FROM_ALL level_seeds_test.cpp)
target_link_libraries(larecomp_level_seeds_test PRIVATE Threads::Threads)
# The precache queue's order and the windows the render thread's claims put in front of the workers (precache_queue.h) too:
#   cmake --build <dir> --target larecomp_precache_queue_test, then run src/gpu/larecomp_precache_queue_test (exit 0 = passed).
add_executable(larecomp_precache_queue_test EXCLUDE_FROM_ALL precache_queue_test.cpp)
# The Windows builds the game's Direct3D 12 runtime needs (windows_floor.h, which only explains a refused start) likewise:
#   cmake --build <dir> --target larecomp_windows_floor_test, then run src/gpu/larecomp_windows_floor_test (exit 0 = passed).
add_executable(larecomp_windows_floor_test EXCLUDE_FROM_ALL windows_floor_test.cpp)
# The sampler address modes a device can make (sampler_modes.h: mirror once needs an extension on Vulkan) have a check of their own too:
#   cmake --build <dir> --target larecomp_sampler_modes_test, then run src/gpu/larecomp_sampler_modes_test (exit 0 = passed).
add_executable(larecomp_sampler_modes_test EXCLUDE_FROM_ALL sampler_modes_test.cpp)
# So does the text of the device-loss report (device_fault_format.h):
#   cmake --build <dir> --target larecomp_device_fault_test, then run src/gpu/larecomp_device_fault_test.
add_executable(larecomp_device_fault_test EXCLUDE_FROM_ALL device_fault_format_test.cpp)
# The background build's hand-over rules (background_build.h) have one too: no renderer, no GPU.
#   cmake --build <dir> --target larecomp_background_build_test, then run src/gpu/larecomp_background_build_test.
add_executable(larecomp_background_build_test EXCLUDE_FROM_ALL background_build_test.cpp background_build.cpp
    ${CMAKE_SOURCE_DIR}/src/os/thread_priority.cpp)
# background_build.cpp lowers its worker through the priority intents (os/thread_priority.h).
target_include_directories(larecomp_background_build_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
# std::thread: glibc before 2.34 needs libpthread named at the link.
find_package(Threads REQUIRED)
target_link_libraries(larecomp_background_build_test PRIVATE Threads::Threads)
# The render thread's small CPU savings (render_cpu.h: the mask and the sampled per-draw timers; native_gfx/guest/physical_alias.h: the order of the
# physical range check) have one too: no renderer, no GPU.
#   cmake --build <dir> --target larecomp_render_cpu_test, then run src/gpu/larecomp_render_cpu_test [--bench] (exit 0 = passed).
add_executable(larecomp_render_cpu_test EXCLUDE_FROM_ALL render_cpu_test.cpp)
target_include_directories(larecomp_render_cpu_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_executable(larecomp_render_wait_rules_test EXCLUDE_FROM_ALL render_wait_rules_test.cpp)
target_include_directories(larecomp_render_wait_rules_test PRIVATE ${CMAKE_SOURCE_DIR}/src)

# Render-thread target, layout and pipeline memo checks. No device or shader pack.
add_executable(larecomp_render_draw_memo_test EXCLUDE_FROM_ALL render_draw_memo_test.cpp)
target_include_directories(larecomp_render_draw_memo_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
foreach(_backend IN LISTS LARECOMP_GPU_BACKENDS)
    target_compile_definitions(larecomp_render_draw_memo_test PRIVATE LARECOMP_GPU_HAS_${_backend}=1)
endforeach()
target_link_libraries(larecomp_render_draw_memo_test PRIVATE plume rex::sdk)
# The HDR frame's compose in the present (hlsl/hdr_compose.hlsli, hdr_scene.h) has one too: the shared sum
# compiled as C++, the float16 image and the display's table written out again, the older path against the new:
#   cmake --build <dir> --target larecomp_hdr_compose_test, then run src/gpu/larecomp_hdr_compose_test (exit 0 = passed).
add_executable(larecomp_hdr_compose_test EXCLUDE_FROM_ALL hdr_compose_test.cpp)

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
    # AMD FSR Ray Regeneration (ray_regen.h, perf/RAY-REGEN.md): the denoiser header; Direct3D 12 only (AMD ships no Vulkan provider). Nothing in the renderer calls it yet (package RG-1); the file is
    # built so that it cannot rot, and never opens AMD's DLLs on a PC that is not a Radeon RX 9000 on Direct3D 12.
    if(LARECOMP_FFX_HAS_DENOISER AND "D3D12" IN_LIST LARECOMP_GPU_BACKENDS)
        target_sources(larecomp_gpu PRIVATE ray_regen.cpp)
        target_include_directories(larecomp_gpu PRIVATE "${LARECOMP_FFX_KIT}/denoisers/include")
        target_compile_definitions(larecomp_gpu PRIVATE LARECOMP_HAS_RAY_REGEN=1)
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
    # DLSS Ray Reconstruction (perf/DLSS-RR.md): the plugin's header and its two files (the top-level CMake checked them). Without them temporal_dlss.cpp
    # builds as before and the debug setting mcla_gpu_dlss_rr says the build has no RR.
    if(LARECOMP_STREAMLINE_HAS_RR)
        target_compile_definitions(larecomp_gpu PRIVATE LARECOMP_HAS_DLSS_RR=1)
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
# xxhash.h for xxhash_inline.h (the renderer's XXH3 hashes): rexglue 0.11's rex/hash.h no longer includes it, and the
# SDK's headers do not carry it. Header only (XXH_INLINE_ALL): an include folder, nothing to link. The folder is
# the one src/shaderpack uses: LARECOMP_XXHASH_INCLUDE_DIR, else LAR-XenosRecomp's thirdparty/xxHash (both define
# their cache variables with the same text; whichever runs first wins, and the value is the same).
set(LARECOMP_XXHASH_INCLUDE_DIR "" CACHE PATH "Folder with xxhash.h (empty: LAR-XenosRecomp's)")
get_filename_component(_larecomp_gpu_repo_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(LARECOMP_XENOSRECOMP_DIR "${_larecomp_gpu_repo_root}/thirdparty/LAR-XenosRecomp"
    CACHE PATH "LAR-XenosRecomp checkout (the shader translator, library target XenosRecompLib)")
set(_larecomp_gpu_xxhash_include "${LARECOMP_XXHASH_INCLUDE_DIR}")
if(NOT _larecomp_gpu_xxhash_include)
    foreach(_candidate "${LARECOMP_XENOSRECOMP_DIR}/thirdparty/xxHash")
        if(EXISTS "${_candidate}/xxhash.h")
            set(_larecomp_gpu_xxhash_include "${_candidate}")
            break()
        endif()
    endforeach()
endif()
if(NOT _larecomp_gpu_xxhash_include)
    message(FATAL_ERROR
        "larecomp_gpu: xxhash.h not found (looked in ${LARECOMP_XENOSRECOMP_DIR}/thirdparty/xxHash: "
        "git submodule update --init --recursive). Set LARECOMP_XXHASH_INCLUDE_DIR.")
endif()
target_include_directories(larecomp_gpu SYSTEM PRIVATE "${_larecomp_gpu_xxhash_include}")
# plume first: see the note at the top.
target_link_libraries(larecomp_gpu PRIVATE plume rex::sdk)
foreach(_backend IN LISTS LARECOMP_GPU_BACKENDS)
    target_compile_definitions(larecomp_gpu PRIVATE LARECOMP_GPU_HAS_${_backend}=1)
endforeach()
# The same flags the SDK applies to its own object library (larecomp_recomp).
rexglue_apply_target_settings(larecomp_gpu)

# ---------------------------------------------------------------------------
# Host shaders: hlsl/<stem>.hlsl -> shaders/<stem>.hlsl.<dxil|spirv>.h with
# g_<stem>_<dxil|spirv>, once per backend the build has. Rule as in reeot
# cmake/host_shaders.cmake (BSD-3-Clause): FetchDxcHost.cmake uses dxc-bin's
# command format, a plain path on Windows and "VAR=value;<dxc>" elsewhere.
# The VAR=value parts go through `cmake -E env` for every generator.
# ---------------------------------------------------------------------------
if(NOT LARECOMP_DXC_HOST_TOOL)
    message(FATAL_ERROR "larecomp_gpu: no host shader compiler (check FetchDxcHost.cmake and thirdparty/dxc-bin)")
endif()
set(LARECOMP_GPU_DXC "")
foreach(_part IN LISTS LARECOMP_DXC_HOST_TOOL)
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
#                          [DEFINES NAME=VALUE...] [SPIRV_ARGS <dxc argument>...]):
# the same source compiled more than once (variants) under different stems.
# HLSL 2021 unless HLSL_VERSION says otherwise: AMD's ffx_a.h uses the vector
# ternaries 2021 rejects, so the FSR1 passes build as 2018. SPIRV_ARGS: more
# dxc arguments for the SPIR-V blob only (larecomp_gpu_host_shader_rt below).
function(larecomp_gpu_host_shader STEM PROFILE)
    cmake_parse_arguments(ARG "" "SOURCE;HLSL_VERSION" "DEFINES;SPIRV_ARGS" ${ARGN})
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
            set(format_args -spirv -fvk-use-dx-layout ${ARG_SPIRV_ARGS})
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

# larecomp_gpu_host_shader_rt(<stem> <profile> [SOURCE <hlsl stem>] [HLSL_VERSION <year>] [DEFINES NAME=VALUE...]):
# a host shader that traces rays inline (RayQuery over a top level structure, the car ray tracing's: car_rt_scene.h). DXIL at the profile given
# (cs_6_5: Shader Model 6.5 is where inline ray queries start; the renderer only runs them where Plume reports DXR 1.1 and that shader model,
# CarRtSupported) and SPIR-V for Vulkan 1.2 with -fspv-target-env=vulkan1.2 (SPIR-V 1.5, which the ray query extension needs; DXC adds the
# RayQueryKHR capability and SPV_KHR_ray_query itself). Run only where Plume reports capabilities.rayQuery (VK_KHR_ray_query): a device without
# the feature refuses the module. The headers are <stem>.hlsl.<dxil|spirv>.h with the arrays g_<stem>_<dxil|spirv>, as the others.
function(larecomp_gpu_host_shader_rt STEM PROFILE)
    larecomp_gpu_host_shader(${STEM} ${PROFILE} ${ARGN} SPIRV_ARGS -fspv-target-env=vulkan1.2)
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
# The same masks from a coverage image with the samples of the game's depth (mcla_gpu_msaa_with_upscaler): the mean
# of the samples' masks.
larecomp_gpu_host_shader(upscaler_masks_ms_ps ps_6_0 SOURCE upscaler_masks_ps DEFINES MASKS_MS=1)
# The G-buffer's debug view (gbuffer.h, mcla_gpu_gbuffer_view): a plane of the sidecar images drawn over the main view's colour; once for planes with one
# sample, once for the host's MSAA level.
larecomp_gpu_host_shader(gbuffer_view_ps ps_6_0 DEFINES GB_VIEW_MS=0)
larecomp_gpu_host_shader(gbuffer_view_ms_ps ps_6_0 SOURCE gbuffer_view_ps DEFINES GB_VIEW_MS=1)
# The deferred local light pass (local_lights.h, GB-H1): one scissored additive draw per light into the main colour, the surface from the G-buffer and the main depth; once for planes with one
# sample, once for the host's MSAA level.
larecomp_gpu_host_shader(local_light_ps ps_6_0 DEFINES LL_PASS_MS=0)
larecomp_gpu_host_shader(local_light_ms_ps ps_6_0 SOURCE local_light_ps DEFINES LL_PASS_MS=1)
# The shared screen products (screen_shared.h): one level of the Hi-Z chain (the min of the linear depth), and the debug view of the products.
larecomp_gpu_host_shader(screen_hiz_ps ps_6_0)
# ... and the colour captures: colour_post's level 0 from the main colour (plain or multisampled) and a mip level of it.
larecomp_gpu_host_shader(screen_colour_ps ps_6_0 DEFINES SCREEN_COLOUR_PASS=0 SCREEN_COLOUR_MS=0)
larecomp_gpu_host_shader(screen_colour_ms_ps ps_6_0 SOURCE screen_colour_ps DEFINES SCREEN_COLOUR_PASS=0 SCREEN_COLOUR_MS=1)
larecomp_gpu_host_shader(screen_colour_mip_ps ps_6_0 SOURCE screen_colour_ps DEFINES SCREEN_COLOUR_PASS=1)
larecomp_gpu_host_shader(screen_view_ps ps_6_0)
# The screen-space reflections (ssr.h): the trace (over plain and multisampled planes), the accumulation, the resolve into the main colour (plain and multisampled).
larecomp_gpu_host_shader(ssr_trace_ps ps_6_0 DEFINES SSR_MS=0)
larecomp_gpu_host_shader(ssr_trace_ms_ps ps_6_0 SOURCE ssr_trace_ps DEFINES SSR_MS=1)
# ... the same two with the confidence as a second target (SSR_RT=1), for a frame the ray traced fallback follows (ssr_rt.h, CITY-RT R4), and the fallback's compute passes: the candidates' tile list (no rays,
# Shader Model 6.0) and the rays into the city's top level (inline ray queries: Shader Model 6.5, SPIR-V 1.5 on Vulkan), the latter over plain and multisampled planes.
larecomp_gpu_host_shader(ssr_trace_rt_ps ps_6_0 SOURCE ssr_trace_ps DEFINES SSR_MS=0 SSR_RT=1)
larecomp_gpu_host_shader(ssr_trace_rt_ms_ps ps_6_0 SOURCE ssr_trace_ps DEFINES SSR_MS=1 SSR_RT=1)
larecomp_gpu_host_shader(ssr_rt_select_cs cs_6_0)
larecomp_gpu_host_shader_rt(ssr_rt_trace_cs cs_6_5 DEFINES SSR_MS=0)
larecomp_gpu_host_shader_rt(ssr_rt_trace_ms_cs cs_6_5 SOURCE ssr_rt_trace_cs DEFINES SSR_MS=1)
larecomp_gpu_host_shader(ssr_temporal_ps ps_6_0 DEFINES SSR_MS=0)
larecomp_gpu_host_shader(ssr_apply_ps ps_6_0 DEFINES SSR_MS=0)
larecomp_gpu_host_shader(ssr_apply_ms_ps ps_6_0 SOURCE ssr_apply_ps DEFINES SSR_MS=1)
# RR-4a: the same resolve for a frame DLSS Ray Reconstruction takes (it reads the trace itself: the accumulation does not run), and the pass that writes the reflections' hit distance guide (an R16_FLOAT image
# of the picture's size). Plain planes only: a raw frame has one host sample.
larecomp_gpu_host_shader(ssr_apply_raw_ps ps_6_0 SOURCE ssr_apply_ps DEFINES SSR_MS=0 SSR_RAW=1)
larecomp_gpu_host_shader(ssr_hit_ps ps_6_0 DEFINES SSR_MS=0)
# RtDenoise (rt_denoise.h, CITY-RT R6): the bilateral a-trous over the reflections' accumulated image (the screen-space reflections' use; plain planes only), and the two plain-guide compiles the other
# consumers start from (the probe field's detail pass, CITY-RT M3 G5): the spatial pass and the temporal pass over guides that are plain images. Shader Model 6.0 compute for both backends.
larecomp_gpu_host_shader(ssr_denoise_spatial_cs cs_6_0 DEFINES SSR_MS=0)
larecomp_gpu_host_shader(rt_denoise_plain_spatial_cs cs_6_0)
larecomp_gpu_host_shader(rt_denoise_plain_temporal_cs cs_6_0)
# The same two stages with the DIFFUSE profile (the probe field's per-pixel detail pass's denoiser, gi_pixel.h): a fixed radius of six pixels that shortens as the history grows, no hit distance, looser weights.
larecomp_gpu_host_shader(rt_denoise_diffuse_spatial_cs cs_6_0 SOURCE rt_denoise_plain_spatial_cs DEFINES RT_DN_PROFILE=RT_DN_PROFILE_DIFFUSE)
larecomp_gpu_host_shader(rt_denoise_diffuse_temporal_cs cs_6_0 SOURCE rt_denoise_plain_temporal_cs DEFINES RT_DN_PROFILE=RT_DN_PROFILE_DIFFUSE)
# The same two stages over 8 bit images and lighter guides (the ray traced light visibility, rt_lights.h, RT-LIGHTS-D1 part 2): RGBA8 signal and results (the history's length in the alpha as n / 32, the
# temporal pass keeps the 8 bit average from stalling: hlsl/rt_denoise_unorm8.hlsli), R16F depth, the normal and the share in one RGBA8 snorm image (RT_DN_VIS in hlsl/rt_denoise_plain_consumer.hlsli).
larecomp_gpu_host_shader(rt_denoise_vis_spatial_cs cs_6_0 SOURCE rt_denoise_plain_spatial_cs DEFINES RT_DN_PROFILE=RT_DN_PROFILE_DIFFUSE RT_DN_VIS=1 RT_DN_OUT_UNORM8=1)
larecomp_gpu_host_shader(rt_denoise_vis_temporal_cs cs_6_0 SOURCE rt_denoise_plain_temporal_cs DEFINES RT_DN_PROFILE=RT_DN_PROFILE_DIFFUSE RT_DN_VIS=1 RT_DN_OUT_UNORM8=1)
# DLSS Ray Reconstruction's guide pass (rr_guides.h, RR-2): the five guide images from the G-buffer's planes in one compute pass at the anchor (over planes with one sample and over the host's MSAA
# level), and the debug view that draws one of them over the upscaler's output. Plain Shader Model 6.0 compute for both backends.
larecomp_gpu_host_shader(rr_guides_cs cs_6_0 DEFINES RR_MS=0)
larecomp_gpu_host_shader(rr_guides_ms_cs cs_6_0 SOURCE rr_guides_cs DEFINES RR_MS=1)
larecomp_gpu_host_shader(rr_guides_view_cs cs_6_0)
# AMD FSR Ray Regeneration's guide and signal pass for the screen-space reflections (ray_regen_guides.h, RG-2 / RG-3): the trace's radiance as a signal and the five guides in AMD's formats at the trace's size, one compute
# pass. Plain Shader Model 6.0 (the 6.6 AMD asks for is its own runtime's); run on Direct3D 12 only, built for both backends like every host shader.
larecomp_gpu_host_shader(ray_regen_ssr_cs cs_6_0)
larecomp_gpu_host_shader(ray_regen_bounce_cs cs_6_0)
# RTAO-R2 begin
target_sources(larecomp_gpu PRIVATE
    rtao_denoise.cpp
    rtao_compose.cpp
    ray_regen_ao.cpp)
add_executable(larecomp_rtao_denoise_test EXCLUDE_FROM_ALL rtao_denoise_test.cpp)
larecomp_gpu_host_shader(rtao_denoise_temporal_cs cs_6_0)
larecomp_gpu_host_shader(rtao_denoise_spatial_cs cs_6_0)
larecomp_gpu_host_shader(rtao_compose_cs cs_6_0)
larecomp_gpu_host_shader(rtao_compose_gi_cs cs_6_0 SOURCE rtao_compose_cs DEFINES RTAO_GI=1)
larecomp_gpu_host_shader(ray_regen_ao_cs cs_6_0)
# RTAO-R2 end
# The screen-space ambient occlusion's passes (ao.h): the linear depth and its coarser levels, the
# GTAO visibility, the denoise, the accumulation over frames and the apply into the scene colour; the
# depth, the accumulation (its vehicle stencil test) and the apply read the EDRAM depth target's
# planes, multisampled or not.
larecomp_gpu_host_shader(ao_depth_ps ps_6_0 DEFINES AO_DEPTH_MS=0)
larecomp_gpu_host_shader(ao_depth_ms_ps ps_6_0 SOURCE ao_depth_ps DEFINES AO_DEPTH_MS=1)
larecomp_gpu_host_shader(ao_mip_ps ps_6_0)
# The same two passes writing the Hi-Z's level of the same size into a second target (SS-3, screen_shared.h mcla_gpu_screen_hiz_fused): one draw makes the depth chain's image and the Hi-Z's
# beside it (the depth pass: Hi-Z level 0; the coarser level 1 and 2: Hi-Z levels 1 and 2). The first target is the old pass's, the same code.
larecomp_gpu_host_shader(ao_depth_hiz_ps ps_6_0 SOURCE ao_depth_ps DEFINES AO_DEPTH_MS=0 AO_HIZ0=1)
larecomp_gpu_host_shader(ao_depth_hiz_ms_ps ps_6_0 SOURCE ao_depth_ps DEFINES AO_DEPTH_MS=1 AO_HIZ0=1)
larecomp_gpu_host_shader(ao_mip_hiz_ps ps_6_0 SOURCE ao_mip_ps DEFINES AO_HIZ=1)
# The normal image the passes read (ao.h, GB-E): the unit view-space normal of every pixel, from the G-buffer's normal plane where it has one and from the depth where it has not;
# once for a plane with one sample, once for the host's MSAA level.
larecomp_gpu_host_shader(ao_normal_ps ps_6_0 DEFINES AO_NORMAL_MS=0)
larecomp_gpu_host_shader(ao_normal_ms_ps ps_6_0 SOURCE ao_normal_ps DEFINES AO_NORMAL_MS=1)
larecomp_gpu_host_shader(ao_main_ps ps_6_0)
# The same visibility pass with the slices and steps compiled in (ao_fast.h: 3 slices of 3 steps at high, 2 of 2 at low): the loops over
# the steps unroll and what is a function of the counts folds. ao_main_ps is the compilation that reads the counts from the push block,
# kept for a count that no variant has.
larecomp_gpu_host_shader(ao_main_3x3_ps ps_6_0 SOURCE ao_main_ps DEFINES AO_SLICES=3 AO_STEPS=3)
larecomp_gpu_host_shader(ao_main_2x2_ps ps_6_0 SOURCE ao_main_ps DEFINES AO_SLICES=2 AO_STEPS=2)
larecomp_gpu_host_shader(ao_denoise_ps ps_6_0)
larecomp_gpu_host_shader(ao_temporal_ps ps_6_0 DEFINES AO_DEPTH_MS=0)
larecomp_gpu_host_shader(ao_temporal_ms_ps ps_6_0 SOURCE ao_temporal_ps DEFINES AO_DEPTH_MS=1)
larecomp_gpu_host_shader(ao_apply_ps ps_6_0 DEFINES AO_DEPTH_MS=0)
larecomp_gpu_host_shader(ao_apply_ms_ps ps_6_0 SOURCE ao_apply_ps DEFINES AO_DEPTH_MS=1)
# The contact shadows' ray march over the occlusion's linear depth (ao.h).
larecomp_gpu_host_shader(ao_contact_ps ps_6_0)
# The screen-space global illumination's passes on the same images (ao.h, G7a): the visibility bitmask in
# place of the GTAO horizons (a second compilation of the occlusion's main pass), the scene colour's chain
# (level 0 from a plain or a multisampled colour plane, a coarser level) and the debug picture of it.
larecomp_gpu_host_shader(ao_main_gi_ps ps_6_0 SOURCE ao_main_ps DEFINES AO_GI=1)
# The bitmask pass with the counts compiled in, the far ring's steps a side too (3x3 with 2, 2x2 with 1): with the polynomial acos of
# the sectors (AO_FAST_ACOS, mcla_gpu_ao_fast_math true) and, for mcla_gpu_ao_fast_math false, with the hardware's. ao_main_gi_ps stays
# for a ring that has another number of steps (the debug override) and is the exact one.
larecomp_gpu_host_shader(ao_main_gi_3x3_far2_ps ps_6_0 SOURCE ao_main_ps
                         DEFINES AO_GI=1 AO_SLICES=3 AO_STEPS=3 AO_FAR_STEPS=2 AO_FAST_ACOS=1)
larecomp_gpu_host_shader(ao_main_gi_2x2_far1_ps ps_6_0 SOURCE ao_main_ps
                         DEFINES AO_GI=1 AO_SLICES=2 AO_STEPS=2 AO_FAR_STEPS=1 AO_FAST_ACOS=1)
larecomp_gpu_host_shader(ao_main_gi_3x3_far2_exact_ps ps_6_0 SOURCE ao_main_ps
                         DEFINES AO_GI=1 AO_SLICES=3 AO_STEPS=3 AO_FAR_STEPS=2)
larecomp_gpu_host_shader(ao_main_gi_2x2_far1_exact_ps ps_6_0 SOURCE ao_main_ps
                         DEFINES AO_GI=1 AO_SLICES=2 AO_STEPS=2 AO_FAR_STEPS=1)
larecomp_gpu_host_shader(ao_color_ps ps_6_0 DEFINES AO_COLOR_PASS=0 AO_COLOR_MS=0)
larecomp_gpu_host_shader(ao_color_ms_ps ps_6_0 SOURCE ao_color_ps DEFINES AO_COLOR_PASS=0 AO_COLOR_MS=1)
larecomp_gpu_host_shader(ao_color_mip_ps ps_6_0 SOURCE ao_color_ps DEFINES AO_COLOR_PASS=1)
larecomp_gpu_host_shader(ao_color_view_ps ps_6_0 SOURCE ao_color_ps DEFINES AO_COLOR_PASS=2)
# The bounce light's history and denoise (ao.h, G7b): the occlusion's denoise and accumulation over four
# channels (visibility and bounce; the accumulation from a plain or a multisampled depth target's stencil)
# and the a-trous filter of the bounce.
larecomp_gpu_host_shader(ao_denoise_gi_ps ps_6_0 SOURCE ao_denoise_ps DEFINES AO_GI=1)
larecomp_gpu_host_shader(ao_temporal_gi_ps ps_6_0 SOURCE ao_temporal_ps DEFINES AO_GI=1 AO_DEPTH_MS=0)
larecomp_gpu_host_shader(ao_temporal_gi_ms_ps ps_6_0 SOURCE ao_temporal_ps DEFINES AO_GI=1 AO_DEPTH_MS=1)
larecomp_gpu_host_shader(ao_atrous_ps ps_6_0)
# The bounce light's apply (ao.h, G7c): the apply pass compiled with the bounce, one factor a colour channel
# (a plain or a multisampled colour target's own pipeline, as the occlusion's apply).
larecomp_gpu_host_shader(ao_apply_gi_ps ps_6_0 SOURCE ao_apply_ps DEFINES AO_DEPTH_MS=0 AO_GI=1)
larecomp_gpu_host_shader(ao_apply_gi_ms_ps ps_6_0 SOURCE ao_apply_ps DEFINES AO_DEPTH_MS=1 AO_GI=1)
# The sun shafts' passes (shafts.h): the sky's brightness from the main view's depth and colour planes
# (plain or multisampled), the radial blur toward the sun, the light added to the scene colour.
larecomp_gpu_host_shader(shafts_sky_ps ps_6_0 DEFINES SHAFTS_MS=0)
larecomp_gpu_host_shader(shafts_sky_ms_ps ps_6_0 SOURCE shafts_sky_ps DEFINES SHAFTS_MS=1)
larecomp_gpu_host_shader(shafts_blur_ps ps_6_0)
larecomp_gpu_host_shader(shafts_add_ps ps_6_0)
# The blurred image averaged over frames while no temporal upscaler runs (NOISE-RESOLVE, noise_resolve.h):
larecomp_gpu_host_shader(shafts_temporal_ps ps_6_0)
# The volumetric light's passes (volumetrics.h): the inject and the integrate (compute, Shader Model 6.0, a typed RGBA16F storage image store on both
# APIs) and the composite (plain or multisampled depth plane).
larecomp_gpu_host_shader(volumetrics_inject_cs cs_6_0)
larecomp_gpu_host_shader(volumetrics_integrate_cs cs_6_0)
larecomp_gpu_host_shader(volumetrics_apply_ps ps_6_0 DEFINES APPLY_MS=0)
larecomp_gpu_host_shader(volumetrics_apply_ms_ps ps_6_0 SOURCE volumetrics_apply_ps DEFINES APPLY_MS=1)
# The car self-reflection history (car_selfrefl.h): the main view's colour and the car's depth into one image, read by the
# next frame's car paint variant.
larecomp_gpu_host_shader(car_history_ps ps_6_0 DEFINES CAR_HISTORY_MS=0)
larecomp_gpu_host_shader(car_history_ms_ps ps_6_0 SOURCE car_history_ps DEFINES CAR_HISTORY_MS=1)
# ... and the same with the colour read from the shared screen products' colour_post (screen_shared.h): the depth and stencil planes multisampled, the colour one single sample image.
larecomp_gpu_host_shader(car_history_ms_shared_ps ps_6_0 SOURCE car_history_ps DEFINES CAR_HISTORY_MS=2)
# The player car's sun shadow map (car_shadow_map.h): the depth-only vertex shader of the car's own draws, the 4x1 matrix texture's pass,
# and the debug view's square.
larecomp_gpu_host_shader(car_shadow_map_vs vs_6_0)
larecomp_gpu_host_shader(cabin_mask_ps ps_6_0)
larecomp_gpu_host_shader(car_shadow_matrix_ps ps_6_0)
larecomp_gpu_host_shader(car_shadow_view_ps ps_6_0)
# The shared rear view (mirror_view.h, perf/MIRROR-VIEW.md): the average of the multisampled view's samples into the picture the readers sample, and the HUD stamp's quad into the main colour.
larecomp_gpu_host_shader(mirror_view_resolve_ps ps_6_0 SOURCE mirror_view_ps DEFINES MIRROR_RESOLVE=1)
larecomp_gpu_host_shader(mirror_view_stamp_ps ps_6_0 SOURCE mirror_view_ps DEFINES MIRROR_RESOLVE=0)
# The side mirrors' glass overlay (mirror_overlay.h, package MV4): the glass patch's triangles drawn again with a lookup into that picture; one vertex stage, one pixel stage, the arithmetic in hlsl/mirror_overlay.hlsli.
larecomp_gpu_host_shader(mirror_overlay_vs vs_6_0)
larecomp_gpu_host_shader(mirror_overlay_ps ps_6_0)
# The headlight shadow atlas's debug view (local_shadow.h): the atlas in a rectangle of the picture. The replay itself uses the game's own shaders.
larecomp_gpu_host_shader(local_shadow_view_ps ps_6_0)
# The player's car as ray tracing structures (car_rt_scene.h): the check pass (inline ray queries, Shader Model 6.5; SPIR-V 1.5 on Vulkan) and
# the pass that shows its picture over the scene.
larecomp_gpu_host_shader_rt(car_rt_check_cs cs_6_5)
larecomp_gpu_host_shader(car_rt_view_ps ps_6_0)
# The ray traced half (car_rt_refl.h): one compute pass over the history image with inline ray queries against the car's structures
# (car_rt_scene.h). Shader Model 6.5, and SPIR-V 1.5 on Vulkan (larecomp_gpu_host_shader_rt).
larecomp_gpu_host_shader_rt(car_rt_refl_cs cs_6_5)
# Its opponents' variant (car_rt_opp_refl.h): the same file with CAR_RT_REFL_OPPONENTS, whose entry point (hlsl/car_rt_refl_opp.hlsli) traces the pixels of the paint of the cars beside the player.
larecomp_gpu_host_shader_rt(car_rt_refl_opp_cs cs_6_5 SOURCE car_rt_refl_cs DEFINES CAR_RT_REFL_OPPONENTS=1)
# Its glass variant (car_rt_glass_refl.h, CITY-RT M2 R3b): the same file with CAR_RT_REFL_GLASS, whose entry point (hlsl/car_rt_refl_glass.hlsli) finds the player's windows with a primary ray and traces their
# mirror reflection; and the additive pass that puts the result into the main colour (a host pass pixel shader, one per sample count of the target, the same blob).
larecomp_gpu_host_shader_rt(car_rt_refl_glass_cs cs_6_5 SOURCE car_rt_refl_cs DEFINES CAR_RT_REFL_GLASS=1)
larecomp_gpu_host_shader(car_glass_apply_ps ps_6_0)
# Its denoise (car_rt_refl.h DENOISE): two a-trous passes over the image the trace made, plain Shader Model 6.0 compute (no rays), built for
# both backends like any host shader (the pass runs only where the trace does).
larecomp_gpu_host_shader(car_rt_refl_denoise_cs cs_6_0)
# RR-4b: the hit distance pass of the raw mode (a frame DLSS Ray Reconstruction takes: the denoise above does not run): one compute pass writing an R16_FLOAT image of the picture's size from the aux
# image, on the denoise's layout.
larecomp_gpu_host_shader(car_rt_refl_hit_cs cs_6_0)
# The ray traced occlusion and sun term around the player's car (car_rt_ao.h): one compute pass inside the occlusion's own passes with inline ray queries
# against the car's structures (car_rt_scene.h). Shader Model 6.5, and SPIR-V 1.5 on Vulkan, through the same function as the others (larecomp_gpu_host_shader_rt).
larecomp_gpu_host_shader_rt(car_rt_ao_cs cs_6_5)
# The city's ray traced sun shadows (city_rt_shadow.h, CITY-RT M1): the same shape, one compute pass with inline ray queries against the same top level, which holds the
# city's sectors beside the cars (city_rt_scene.h). Shader Model 6.5, and SPIR-V 1.5 on Vulkan.
larecomp_gpu_host_shader_rt(city_rt_shadow_cs cs_6_5)
# The ray traced visibility of the top K lights (rt_lights.h, RT-LIGHTS-D1): one compute pass over the occlusion's linear depth, a ray a pixel to each light that reaches it, into the visibility images the
# light pass reads. Shader Model 6.5 (wave intrinsics for the counters), SPIR-V 1.5 on Vulkan like the city shadows'.
larecomp_gpu_host_shader_rt(rt_lights_trace_cs cs_6_5)

# RTAO-R1 begin
target_sources(larecomp_gpu PRIVATE rtao.cpp)
# The world ray traced ambient occlusion's rules (rtao_rules.h: the plan, the fallback ladder, the calibration, the memory and ray models) held to the trace shader's text and the frame term's statements, and its noise
# (hlsl/rtao_noise.hlsli compiled as C++: the sub-pixel pick covers the corners, every lane is a low discrepancy set over 16 frames):
#   cmake --build <dir> --target larecomp_rtao_rules_test, then run src/gpu/larecomp_rtao_rules_test [<src/gpu dir>] (exit 0 = passed); larecomp_rtao_noise_test takes no argument.
add_executable(larecomp_rtao_rules_test EXCLUDE_FROM_ALL rtao_rules_test.cpp)
add_executable(larecomp_rtao_noise_test EXCLUDE_FROM_ALL rtao_noise_test.cpp)
larecomp_gpu_host_shader_rt(rtao_trace_cs cs_6_5 DEFINES RTAO_MS=0)
larecomp_gpu_host_shader_rt(rtao_trace_ms_cs cs_6_5 SOURCE rtao_trace_cs DEFINES RTAO_MS=1)
# RTAO-R1 end
# The city hit data's probe (city_hit_probe.h, CITY-RT M2 H3): the pool and the tables read through the ray tracing set's t6..t9.
larecomp_gpu_host_shader_rt(city_hit_probe_cs cs_6_5)
# The probe field owner's passes (gi_field.h, CITY-RT M3 package G1): the reset of the probes a window move exposed and the weights pass (compute, Shader Model 6.0, typed storage image
# stores of RGBA16F, RG16F and R8_UNORM with the format given to SPIR-V), and the debug view's pixel shader (the probes as spheres over the picture).
larecomp_gpu_host_shader(gi_field_reset_cs cs_6_0)
larecomp_gpu_host_shader(gi_field_weights_cs cs_6_0)
larecomp_gpu_host_shader(gi_field_view_ps ps_6_0)
# The probe update's passes (gi_update.h, CITY-RT M3 package G2): the trace (inline ray queries against the top level, the city hit library in GI mode: Shader Model 6.5, SPIR-V 1.5 on Vulkan) and the blend (typed
# RGBA16F and RG16F storage image loads and stores with the format given to SPIR-V).
larecomp_gpu_host_shader_rt(gi_update_trace_cs cs_6_5)
larecomp_gpu_host_shader(gi_update_blend_cs cs_6_0)
# The probe field's per-pixel detail pass (gi_pixel.h, CITY-RT M3 package G5): one cosine ray a pixel into the city and the cars, shaded by the city hit library in GI mode; once over plain planes, once over the
# host's MSAA level. Shader Model 6.5 with inline ray queries, as the probe update's trace.
larecomp_gpu_host_shader_rt(gi_pixel_trace_cs cs_6_5 DEFINES GIP_MS=0)
larecomp_gpu_host_shader_rt(gi_pixel_trace_ms_cs cs_6_5 SOURCE gi_pixel_trace_cs DEFINES GIP_MS=1)
# The probe field's apply (gi_apply.h, CITY-RT M3 package G3): the lookup at the occlusion's traced size and the additive apply at the picture's size, each for plain and multisampled planes (the apply also
# without and with the upsample from the lookup's image); one source, hlsl/gi_apply_ps.hlsl.
larecomp_gpu_host_shader(gi_apply_lookup_ps ps_6_0 SOURCE gi_apply_ps DEFINES GIA_PASS=0 GIA_MS=0 GIA_UP=0)
larecomp_gpu_host_shader(gi_apply_lookup_ms_ps ps_6_0 SOURCE gi_apply_ps DEFINES GIA_PASS=0 GIA_MS=1 GIA_UP=0)
larecomp_gpu_host_shader(gi_apply_ps ps_6_0 DEFINES GIA_PASS=1 GIA_MS=0 GIA_UP=0)
larecomp_gpu_host_shader(gi_apply_ms_ps ps_6_0 SOURCE gi_apply_ps DEFINES GIA_PASS=1 GIA_MS=1 GIA_UP=0)
larecomp_gpu_host_shader(gi_apply_up_ps ps_6_0 SOURCE gi_apply_ps DEFINES GIA_PASS=1 GIA_MS=0 GIA_UP=1)
larecomp_gpu_host_shader(gi_apply_up_ms_ps ps_6_0 SOURCE gi_apply_ps DEFINES GIA_PASS=1 GIA_MS=1 GIA_UP=1)
# main colour (an ordinary pixel shader).
# The ray tracing benchmark's trace (rt_bench.h, rt_bench_rules.h): random rays through a synthetic city, run once on the first start with a card to move it between
# the weak and the strong ray tracing tier (rt_tier.h). Shader Model 6.5 and SPIR-V 1.5 like the car passes.
larecomp_gpu_host_shader_rt(rt_bench_cs cs_6_5)
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
