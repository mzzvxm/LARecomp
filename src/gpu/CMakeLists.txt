# The Plume renderer, src/gpu.
#
# larecomp_gpu is an OBJECT library so the renderer compiles with its own
# include order and definitions: plume's headers (its Vulkan-Headers and VMA)
# ahead of rex::sdk's, which arrive as system includes, and
# LARECOMP_GPU_HAS_<backend> visible to these files only. larecomp links the
# objects. Expects the caller to have added thirdparty/plume and
# thirdparty/dxc-bin and to set LARECOMP_GPU_BACKENDS (D3D12 and/or VULKAN; the
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
    car_selfrefl.cpp
    car_shadow_map.cpp
    local_shadow.cpp
    car_rt_refl.cpp
    car_rt_opp_refl.cpp
    capture_readback.cpp
    choice_box.cpp
    constants.cpp
    cut_request.cpp
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
    gbuffer.cpp
    gbuffer_material.cpp
    gbuffer_materials.cpp
    geometry.cpp
    gpu_class.cpp
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
    samplers.cpp
    scene.cpp
    screen_shared.cpp
    ssr.cpp
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
# The volumetric light's stability rules (volumetrics_stable.h: a lamp's term averaged over a slice of the grid, the composite's samples, the history's weight as a time
# constant) and a simulation of the passes for a camera driving at a street lamp, which puts a number on the flicker of the first version and of the fixes; given the
# src/gpu folder it also checks that the shaders' flags are the header's, likewise:
#   cmake --build <dir> --target larecomp_volumetrics_stable_test, then run src/gpu/larecomp_volumetrics_stable_test <src/gpu dir> (exit 0 = passed).
add_executable(larecomp_volumetrics_stable_test EXCLUDE_FROM_ALL volumetrics_stable_test.cpp)
# The sun shafts' image size rule (shafts_image.h: half the picture, capped at a 960x540 box; the sky pass's taps; given the hlsl folder it
# also checks that the shaders say what the header says) likewise:
#   cmake --build <dir> --target larecomp_shafts_image_test, then run src/gpu/larecomp_shafts_image_test [<src/gpu/hlsl dir>].
add_executable(larecomp_shafts_image_test EXCLUDE_FROM_ALL shafts_image_test.cpp)
# The bounce light's cap, falloff and gain maths and its strength levels (ssgi_light.h, ssgi_level.h; given the hlsl folder it also
# checks that the shaders' constants are the header's) likewise:
#   cmake --build <dir> --target larecomp_ssgi_test, then run src/gpu/larecomp_ssgi_test [<src/gpu/hlsl dir>].
add_executable(larecomp_ssgi_test EXCLUDE_FROM_ALL ssgi_test.cpp)
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
# The guards for the structs the renderer hashes or compares as bytes (key_guard.h: no padding, every member set, the float zero canonical) and the list of the
# places in src/gpu that read a struct as bytes (a new one fails until it is audited): the headlight replay's key had tail padding and made 98 759 to 543 441
# pipelines in a minute on some runs, 230 on others. Given the src/gpu folder it also reads the sources:
#   cmake --build <dir> --target larecomp_key_guard_test, then run src/gpu/larecomp_key_guard_test <src/gpu dir> (exit 0 = passed).
add_executable(larecomp_key_guard_test EXCLUDE_FROM_ALL key_guard_test.cpp)
# The contact shadows' trace rules and the C++ twin of their march (contact_trace.h: the depth tolerance in pixel footprints, the
# ray's fade and steps, the filter weights, the cost model) likewise:
#   cmake --build <dir> --target larecomp_contact_trace_test, then run src/gpu/larecomp_contact_trace_test.
add_executable(larecomp_contact_trace_test EXCLUDE_FROM_ALL contact_trace_test.cpp)
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
# The one light description (light_frame.h: Light, LightFrame, the 64 byte GpuLight and its bits; light_id.h: the ids, a street lamp's being the old LampKey / KeyOf; light_laws.h: the
# headlight's, the game's lamp's and the volume's laws held to the rig's LampScale and the game's numbers; light_convert.h: the conversions from the headlight rig, the game's lamp
# list, the shadow replay and into the volume's lamp; and it compiles hlsl/light_laws.hlsli as C++ and compares it with the twin) likewise:
#   cmake --build <dir> --target larecomp_light_frame_test, then run src/gpu/larecomp_light_frame_test [<hlsl/light_laws.hlsli>] (exit 0 = passed).
add_executable(larecomp_light_frame_test EXCLUDE_FROM_ALL light_frame_test.cpp)
# The shared screen products' frozen interface (screen_frame.h, screen_rules.h: the products, the consumers' Want registry, the Hi-Z chain's sizes, the plan, the frame and its
# generation; it compiles hlsl/screen_hiz.hlsli as C++ and compares it with the twin, and reads the sources it is given) likewise:
#   cmake --build <dir> --target larecomp_screen_shared_test, then run src/gpu/larecomp_screen_shared_test <src/gpu dir> [--print] (exit 0 = passed).
add_executable(larecomp_screen_shared_test EXCLUDE_FROM_ALL screen_shared_test.cpp)
# The screen-space reflections' rules (ssr_rules.h: levels, sizes, the decision, the push block, the wetness) and the pure HLSL (hlsl/ssr_common.hlsli and ssr_trace.hlsli compiled as C++: the class
# gate, the BRDF, the GGX sampling, the virtual image's reprojection, the Hi-Z march against analytic scenes and a brute force march) likewise; it reads the sources it is given:
#   cmake --build <dir> --target larecomp_ssr_test, then run src/gpu/larecomp_ssr_test <src/gpu dir> [--print] (exit 0 = passed).
add_executable(larecomp_ssr_test EXCLUDE_FROM_ALL ssr_test.cpp)
# The frame's order (frame_order.h, LIGHT-UNIFY U9: the one table of every pass that makes or reads the shared screen and light data, what each makes, reads, must follow and Wants, and the recorder the
# Scene notes its trigger passes in) held to itself (every read has an earlier maker, every step moved before what it needs is found), to the consumers' own wants (gbuffer_rules.h, screen_rules.h), to a model of
# frames with passes missing and every kind of mistake, and, given the src/gpu folder (it defaults to the one the test is in), to scene.cpp, ao.cpp and the consumers' sources (the calls stand in the table's
# order, each has its Note line, the PassTiming labels and the Want calls exist) likewise:
#   cmake --build <dir> --target larecomp_frame_order_test, then run src/gpu/larecomp_frame_order_test [<src/gpu dir>] [--print] (exit 0 = passed).
add_executable(larecomp_frame_order_test EXCLUDE_FROM_ALL frame_order_test.cpp)
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
# The camera cut rule (temporal_camera.h, temporal_cut.cpp) is pure and has a check of its own, on camera
# sequences recorded from the game's logs and made up:
#   cmake --build <dir> --target larecomp_camera_cut_test, then run src/gpu/larecomp_camera_cut_test
#   (exit 0 = passed; with a camera trace csv as the argument it also reports the cuts in that trace).
add_executable(larecomp_camera_cut_test EXCLUDE_FROM_ALL camera_cut_test.cpp temporal_cut.cpp)
# The upscalers' pure arithmetic (upscaler_math.h: the jitter sequence, its length at a ratio, the scene's mip bias) has a check that needs
# no renderer: cmake --build <dir> --target larecomp_upscaler_math_test, then run src/gpu/larecomp_upscaler_math_test (exit 0 = passed).
add_executable(larecomp_upscaler_math_test EXCLUDE_FROM_ALL upscaler_math_test.cpp)
# The rules for when the upscalers' libraries are loaded (upscaler_load_policy.h) have a check that needs no renderer:
#   cmake --build <dir> --target larecomp_upscaler_load_policy_test, then run src/gpu/larecomp_upscaler_load_policy_test (exit 0 = passed).
add_executable(larecomp_upscaler_load_policy_test EXCLUDE_FROM_ALL upscaler_load_policy_test.cpp)
find_package(Threads REQUIRED)
target_link_libraries(larecomp_upscaler_load_policy_test PRIVATE Threads::Threads)
# The rule that gives another pass's use of a side view's EDRAM identity (the night shadow's, which binds the car
# reflections' depth through a surface of another size) a target of its own (side_views.h):
#   cmake --build <dir> --target larecomp_side_view_use_test, then run src/gpu/larecomp_side_view_use_test <src/gpu dir>.
add_executable(larecomp_side_view_use_test EXCLUDE_FROM_ALL side_view_use_test.cpp)
# The camera cut requests and the motion blur constants of a cut (cut_request.h) have one too:
#   cmake --build <dir> --target larecomp_cut_request_test, then run src/gpu/larecomp_cut_request_test (exit 0 = passed).
add_executable(larecomp_cut_request_test EXCLUDE_FROM_ALL cut_request_test.cpp cut_request.cpp)
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
larecomp_gpu_host_shader(ssr_temporal_ps ps_6_0 DEFINES SSR_MS=0)
larecomp_gpu_host_shader(ssr_apply_ps ps_6_0 DEFINES SSR_MS=0)
larecomp_gpu_host_shader(ssr_apply_ms_ps ps_6_0 SOURCE ssr_apply_ps DEFINES SSR_MS=1)
# The screen-space ambient occlusion's passes (ao.h): the linear depth and its coarser levels, the
# GTAO visibility, the denoise, the accumulation over frames and the apply into the scene colour; the
# depth, the accumulation (its vehicle stencil test) and the apply read the EDRAM depth target's
# planes, multisampled or not.
larecomp_gpu_host_shader(ao_depth_ps ps_6_0 DEFINES AO_DEPTH_MS=0)
larecomp_gpu_host_shader(ao_depth_ms_ps ps_6_0 SOURCE ao_depth_ps DEFINES AO_DEPTH_MS=1)
larecomp_gpu_host_shader(ao_mip_ps ps_6_0)
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
larecomp_gpu_host_shader(car_shadow_matrix_ps ps_6_0)
larecomp_gpu_host_shader(car_shadow_view_ps ps_6_0)
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
# Its denoise (car_rt_refl.h DENOISE): two a-trous passes over the image the trace made, plain Shader Model 6.0 compute (no rays), built for
# both backends like any host shader (the pass runs only where the trace does).
larecomp_gpu_host_shader(car_rt_refl_denoise_cs cs_6_0)
# The ray traced occlusion and sun term around the player's car (car_rt_ao.h): one compute pass inside the occlusion's own passes with inline ray queries
# against the car's structures (car_rt_scene.h). Shader Model 6.5, and SPIR-V 1.5 on Vulkan, through the same function as the others (larecomp_gpu_host_shader_rt).
larecomp_gpu_host_shader_rt(car_rt_ao_cs cs_6_5)
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
