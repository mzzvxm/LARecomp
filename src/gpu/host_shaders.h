#pragma once
// ===========================================================================
// MCLA GPU renderer -- host shader blobs
// ===========================================================================
// The renderer's own shaders (src/gpu/hlsl), compiled at build time by DXC
// (cmake/FetchDxcHost.cmake, with thirdparty/dxc-bin as a fallback) once per
// backend the build has: DXIL for D3D12 and SPIR-V for Vulkan (see
// larecomp_gpu_host_shader in src/gpu/CMakeLists.txt). Each generated
// header defines one byte array, g_<stem>_<dxil|spirv>; they have internal
// linkage, so this header is included by the one translation unit that creates
// the shaders of a subsystem. The blob handed out is the active backend's
// (backend.h), so these are called after Device::Initialize.
// ===========================================================================

#include <cstddef>
#include <cstdint>

#include "backend.h"

#if defined(LARECOMP_GPU_HAS_D3D12)
#include "ao_apply_gi_ms_ps.hlsl.dxil.h"
#include "ao_apply_gi_ps.hlsl.dxil.h"
#include "ao_apply_ms_ps.hlsl.dxil.h"
#include "ao_apply_ps.hlsl.dxil.h"
#include "ao_atrous_ps.hlsl.dxil.h"
#include "ao_color_mip_ps.hlsl.dxil.h"
#include "ao_color_ms_ps.hlsl.dxil.h"
#include "ao_color_ps.hlsl.dxil.h"
#include "ao_color_view_ps.hlsl.dxil.h"
#include "ao_contact_ps.hlsl.dxil.h"
#include "ao_denoise_gi_ps.hlsl.dxil.h"
#include "ao_denoise_ps.hlsl.dxil.h"
#include "ao_depth_hiz_ms_ps.hlsl.dxil.h"
#include "ao_depth_hiz_ps.hlsl.dxil.h"
#include "ao_depth_ms_ps.hlsl.dxil.h"
#include "ao_depth_ps.hlsl.dxil.h"
#include "ao_main_2x2_ps.hlsl.dxil.h"
#include "ao_main_3x3_ps.hlsl.dxil.h"
#include "ao_main_gi_2x2_far1_exact_ps.hlsl.dxil.h"
#include "ao_main_gi_2x2_far1_ps.hlsl.dxil.h"
#include "ao_main_gi_2x4_far1_ps.hlsl.dxil.h"
#include "ao_main_gi_2x4_far1_exact_ps.hlsl.dxil.h"
#include "ao_main_gi_3x3_far2_exact_ps.hlsl.dxil.h"
#include "ao_main_gi_3x3_far2_ps.hlsl.dxil.h"
#include "ao_main_gi_ps.hlsl.dxil.h"
#include "ao_main_ps.hlsl.dxil.h"
#include "ao_mip_hiz_ps.hlsl.dxil.h"
#include "ao_mip_ps.hlsl.dxil.h"
#include "ao_normal_ms_ps.hlsl.dxil.h"
#include "ao_normal_ps.hlsl.dxil.h"
#include "ao_temporal_gi_ms_ps.hlsl.dxil.h"
#include "ao_temporal_gi_ps.hlsl.dxil.h"
#include "ao_temporal_ms_ps.hlsl.dxil.h"
#include "ao_temporal_ps.hlsl.dxil.h"
#include "car_history_ms_ps.hlsl.dxil.h"
#include "car_history_ms_shared_ps.hlsl.dxil.h"
#include "car_history_ps.hlsl.dxil.h"
#include "car_rt_ao_cs.hlsl.dxil.h"
#include "city_rt_shadow_cs.hlsl.dxil.h"
#include "rtao_trace_cs.hlsl.dxil.h"
#include "rtao_trace_ms_cs.hlsl.dxil.h"
#include "rt_lights_trace_cs.hlsl.dxil.h"
#include "city_hit_probe_cs.hlsl.dxil.h"
#include "car_rt_check_cs.hlsl.dxil.h"
#include "car_rt_refl_cs.hlsl.dxil.h"
#include "car_rt_refl_denoise_cs.hlsl.dxil.h"
#include "car_rt_refl_glass_cs.hlsl.dxil.h"
#include "car_glass_apply_ps.hlsl.dxil.h"
#include "car_rt_refl_hit_cs.hlsl.dxil.h"
#include "car_rt_refl_opp_cs.hlsl.dxil.h"
#include "car_rt_view_ps.hlsl.dxil.h"
#include "rt_bench_cs.hlsl.dxil.h"
#include "fg_compose_ps.hlsl.dxil.h"
#include "dlssg_ui_hdr_ps.hlsl.dxil.h"
#include "dlssg_ui_ps.hlsl.dxil.h"
#include "fg_copy_ps.hlsl.dxil.h"
#include "fg_prepare_ps.hlsl.dxil.h"
#include "fsr_easu_ps.hlsl.dxil.h"
#include "fxaa_ps.hlsl.dxil.h"
#include "fsr_rcas_ps.hlsl.dxil.h"
#include "fsr_rcas_hdr_ps.hlsl.dxil.h"
#include "fullscreen_vs.hlsl.dxil.h"
#include "gbuffer_view_ms_ps.hlsl.dxil.h"
#include "gbuffer_view_ps.hlsl.dxil.h"
#include "reflhit_native_ps.hlsl.dxil.h"
#include "reflhit_native_ms_ps.hlsl.dxil.h"
#include "gi_apply_lookup_ms_ps.hlsl.dxil.h"
#include "gi_apply_lookup_ps.hlsl.dxil.h"
#include "gi_apply_ms_ps.hlsl.dxil.h"
#include "gi_apply_ps.hlsl.dxil.h"
#include "vpl_hit_0_cs.hlsl.dxil.h"
#include "vpl_hit_1_cs.hlsl.dxil.h"
#include "vpl_hit_rt_0_cs.hlsl.dxil.h"
#include "vpl_hit_rt_1_cs.hlsl.dxil.h"
#include "light_irradiance_0_ps.hlsl.dxil.h"
#include "light_irradiance_1_ps.hlsl.dxil.h"
#include "light_composite_0_ps.hlsl.dxil.h"
#include "light_composite_1_ps.hlsl.dxil.h"
#include "vpl_irradiance_0_ps.hlsl.dxil.h"
#include "vpl_irradiance_1_ps.hlsl.dxil.h"
#include "vpl_composite_0_ps.hlsl.dxil.h"
#include "vpl_composite_1_ps.hlsl.dxil.h"
#include "gi_apply_up_ms_ps.hlsl.dxil.h"
#include "gi_apply_up_ps.hlsl.dxil.h"
#include "gi_field_reset_cs.hlsl.dxil.h"
#include "gi_field_view_ps.hlsl.dxil.h"
#include "gi_field_weights_cs.hlsl.dxil.h"
#include "gi_pixel_trace_cs.hlsl.dxil.h"
#include "gi_pixel_trace_ms_cs.hlsl.dxil.h"
#include "gi_update_blend_cs.hlsl.dxil.h"
#include "gi_update_trace_cs.hlsl.dxil.h"
#include "hdr_compose_ps.hlsl.dxil.h"
#include "layers_resolve_ps.hlsl.dxil.h"
#include "layers_restore_ps.hlsl.dxil.h"
#include "local_light_ms_ps.hlsl.dxil.h"
#include "local_light_ps.hlsl.dxil.h"
#include "overlay_hdr_ps.hlsl.dxil.h"
#include "overlay_ps.hlsl.dxil.h"
#include "overlay_vs.hlsl.dxil.h"
#include "post_luma_ps.hlsl.dxil.h"
#include "present_hdr_ps.hlsl.dxil.h"
#include "present_ps.hlsl.dxil.h"
#include "present_stretch_hdr_ps.hlsl.dxil.h"
#include "present_stretch_ps.hlsl.dxil.h"
#include "reduce_ps.hlsl.dxil.h"
#include "resolve_color_ms_ps.hlsl.dxil.h"
#include "resolve_color_ps.hlsl.dxil.h"
#include "resolve_depth_fused_frac_ps.hlsl.dxil.h"
#include "resolve_depth_fused_ps.hlsl.dxil.h"
#include "resolve_depth_ms_fused_frac_ps.hlsl.dxil.h"
#include "resolve_depth_ms_fused_ps.hlsl.dxil.h"
#include "resolve_depth_ms_ps.hlsl.dxil.h"
#include "resolve_depth_ps.hlsl.dxil.h"
#include "resolve_memory_color_cs.hlsl.dxil.h"
#include "resolve_memory_depth_cs.hlsl.dxil.h"
#include "resolve_memory_depth_ms_cs.hlsl.dxil.h"
#include "resolve_memory_ps.hlsl.dxil.h"
#include "resolve_memory_vertex_cs.hlsl.dxil.h"
#include "screen_colour_mip_ps.hlsl.dxil.h"
#include "screen_colour_ms_ps.hlsl.dxil.h"
#include "screen_colour_ps.hlsl.dxil.h"
#include "screen_hiz_ps.hlsl.dxil.h"
#include "screen_view_ps.hlsl.dxil.h"
#include "ssr_apply_ms_ps.hlsl.dxil.h"
#include "ssr_apply_ps.hlsl.dxil.h"
#include "ssr_apply_raw_ps.hlsl.dxil.h"
#include "ssr_hit_ps.hlsl.dxil.h"
#include "ssr_denoise_spatial_cs.hlsl.dxil.h"
#include "rt_denoise_plain_spatial_cs.hlsl.dxil.h"
#include "rt_denoise_plain_temporal_cs.hlsl.dxil.h"
#include "rt_denoise_diffuse_spatial_cs.hlsl.dxil.h"
#include "rt_denoise_diffuse_temporal_cs.hlsl.dxil.h"
#include "rt_denoise_vis_spatial_cs.hlsl.dxil.h"
#include "rt_denoise_vis_temporal_cs.hlsl.dxil.h"
#include "ssr_temporal_ps.hlsl.dxil.h"
#include "ssr_classify_cs.hlsl.dxil.h"
#include "ssr_classify_reproject_cs.hlsl.dxil.h"
#include "ssr_rt_select_cs.hlsl.dxil.h"
#include "ssr_rt_trace_cs.hlsl.dxil.h"
#include "ssr_rt_trace_ms_cs.hlsl.dxil.h"
#include "ssr_trace_ms_ps.hlsl.dxil.h"
#include "ssr_trace_ps.hlsl.dxil.h"
#include "ssr_trace_rt_ms_ps.hlsl.dxil.h"
#include "ssr_trace_rt_ps.hlsl.dxil.h"
#include "ssr_trace_reflhit_ps.hlsl.dxil.h"
#include "capture_road_reflection_ps.hlsl.dxil.h"
#include "ssr_trace_reflhit_ms_ps.hlsl.dxil.h"
#include "ssr_trace_rt_reflhit_ps.hlsl.dxil.h"
#include "ssr_trace_rt_reflhit_ms_ps.hlsl.dxil.h"
#include "ssr_rt_trace_reflhit_cs.hlsl.dxil.h"
#include "ssr_rt_trace_reflhit_ms_cs.hlsl.dxil.h"
#include "rr_guides_cs.hlsl.dxil.h"
#include "rr_guides_ms_cs.hlsl.dxil.h"
#include "rr_guides_view_cs.hlsl.dxil.h"
#include "ray_regen_ssr_cs.hlsl.dxil.h"
#include "ray_regen_bounce_cs.hlsl.dxil.h"
#include "rtao_denoise_spatial_cs.hlsl.dxil.h"
#include "rt_denoise_v2_ssr_spatial_cs.hlsl.dxil.h"
#include "rt_denoise_v2_ssr_temporal_cs.hlsl.dxil.h"
#include "rt_denoise_v2_ssr_temporal_ms_cs.hlsl.dxil.h"
#include "rt_denoise_v2_diffuse_spatial_cs.hlsl.dxil.h"
#include "rt_denoise_v2_diffuse_temporal_cs.hlsl.dxil.h"
#include "rt_denoise_v2_vis_spatial_cs.hlsl.dxil.h"
#include "rt_denoise_v2_vis_temporal_cs.hlsl.dxil.h"
#include "rt_denoise_v2_ao_spatial_cs.hlsl.dxil.h"
#include "rt_denoise_v2_ao_temporal_cs.hlsl.dxil.h"
#include "rt_denoise_v2_shadow_spatial_cs.hlsl.dxil.h"
#include "rt_denoise_v2_shadow_temporal_cs.hlsl.dxil.h"
#include "rtao_denoise_temporal_cs.hlsl.dxil.h"
#include "rtao_compose_cs.hlsl.dxil.h"
#include "rtao_compose_gi_cs.hlsl.dxil.h"
#include "ray_regen_ao_cs.hlsl.dxil.h"
#include "shafts_add_ps.hlsl.dxil.h"
#include "shafts_blur_ps.hlsl.dxil.h"
#include "shafts_temporal_ps.hlsl.dxil.h"
#include "shafts_sky_ms_ps.hlsl.dxil.h"
#include "shafts_sky_ps.hlsl.dxil.h"
#include "volumetrics_apply_ms_ps.hlsl.dxil.h"
#include "volumetrics_apply_ps.hlsl.dxil.h"
#include "atmosphere_apply_ps.hlsl.dxil.h"
#include "atmosphere_apply_ms_ps.hlsl.dxil.h"
#include "shafts_blur_legacy_ps.hlsl.dxil.h"
#include "shafts_blur_accumulate_ps.hlsl.dxil.h"
#include "volumetrics_inject_cs.hlsl.dxil.h"
#include "volumetrics_integrate_cs.hlsl.dxil.h"
#include "smaa_blend_ps.hlsl.dxil.h"
#include "smaa_edges_ps.hlsl.dxil.h"
#include "smaa_weights_ps.hlsl.dxil.h"
#include "temporal_copy_ps.hlsl.dxil.h"
#include "temporal_mv_ps.hlsl.dxil.h"
#include "temporal_vehicle_table_ps.hlsl.dxil.h"
#include "upscaler_masks_ms_ps.hlsl.dxil.h"
#include "upscaler_masks_ps.hlsl.dxil.h"
#endif
#if defined(LARECOMP_GPU_HAS_VULKAN)
#include "ao_apply_gi_ms_ps.hlsl.spirv.h"
#include "ao_apply_gi_ps.hlsl.spirv.h"
#include "ao_apply_ms_ps.hlsl.spirv.h"
#include "ao_apply_ps.hlsl.spirv.h"
#include "ao_atrous_ps.hlsl.spirv.h"
#include "ao_color_mip_ps.hlsl.spirv.h"
#include "ao_color_ms_ps.hlsl.spirv.h"
#include "ao_color_ps.hlsl.spirv.h"
#include "ao_color_view_ps.hlsl.spirv.h"
#include "ao_contact_ps.hlsl.spirv.h"
#include "ao_denoise_gi_ps.hlsl.spirv.h"
#include "ao_denoise_ps.hlsl.spirv.h"
#include "ao_depth_hiz_ms_ps.hlsl.spirv.h"
#include "ao_depth_hiz_ps.hlsl.spirv.h"
#include "ao_depth_ms_ps.hlsl.spirv.h"
#include "ao_depth_ps.hlsl.spirv.h"
#include "ao_main_2x2_ps.hlsl.spirv.h"
#include "ao_main_3x3_ps.hlsl.spirv.h"
#include "ao_main_gi_2x2_far1_exact_ps.hlsl.spirv.h"
#include "ao_main_gi_2x2_far1_ps.hlsl.spirv.h"
#include "ao_main_gi_2x4_far1_ps.hlsl.spirv.h"
#include "ao_main_gi_2x4_far1_exact_ps.hlsl.spirv.h"
#include "ao_main_gi_3x3_far2_exact_ps.hlsl.spirv.h"
#include "ao_main_gi_3x3_far2_ps.hlsl.spirv.h"
#include "ao_main_gi_ps.hlsl.spirv.h"
#include "ao_main_ps.hlsl.spirv.h"
#include "ao_mip_hiz_ps.hlsl.spirv.h"
#include "ao_mip_ps.hlsl.spirv.h"
#include "ao_normal_ms_ps.hlsl.spirv.h"
#include "ao_normal_ps.hlsl.spirv.h"
#include "ao_temporal_gi_ms_ps.hlsl.spirv.h"
#include "ao_temporal_gi_ps.hlsl.spirv.h"
#include "ao_temporal_ms_ps.hlsl.spirv.h"
#include "ao_temporal_ps.hlsl.spirv.h"
#include "car_history_ms_ps.hlsl.spirv.h"
#include "car_history_ms_shared_ps.hlsl.spirv.h"
#include "car_history_ps.hlsl.spirv.h"
#include "car_rt_ao_cs.hlsl.spirv.h"
#include "city_rt_shadow_cs.hlsl.spirv.h"
#include "rtao_trace_cs.hlsl.spirv.h"
#include "rtao_trace_ms_cs.hlsl.spirv.h"
#include "rt_lights_trace_cs.hlsl.spirv.h"
#include "city_hit_probe_cs.hlsl.spirv.h"
#include "car_rt_check_cs.hlsl.spirv.h"
#include "car_rt_refl_cs.hlsl.spirv.h"
#include "car_rt_refl_denoise_cs.hlsl.spirv.h"
#include "car_rt_refl_glass_cs.hlsl.spirv.h"
#include "car_glass_apply_ps.hlsl.spirv.h"
#include "car_rt_refl_hit_cs.hlsl.spirv.h"
#include "car_rt_refl_opp_cs.hlsl.spirv.h"
#include "car_rt_view_ps.hlsl.spirv.h"
#include "rt_bench_cs.hlsl.spirv.h"
#include "fg_compose_ps.hlsl.spirv.h"
#include "dlssg_ui_hdr_ps.hlsl.spirv.h"
#include "dlssg_ui_ps.hlsl.spirv.h"
#include "fg_copy_ps.hlsl.spirv.h"
#include "fg_prepare_ps.hlsl.spirv.h"
#include "fsr_easu_ps.hlsl.spirv.h"
#include "fxaa_ps.hlsl.spirv.h"
#include "fsr_rcas_ps.hlsl.spirv.h"
#include "fsr_rcas_hdr_ps.hlsl.spirv.h"
#include "fullscreen_vs.hlsl.spirv.h"
#include "gbuffer_view_ms_ps.hlsl.spirv.h"
#include "gbuffer_view_ps.hlsl.spirv.h"
#include "reflhit_native_ps.hlsl.spirv.h"
#include "reflhit_native_ms_ps.hlsl.spirv.h"
#include "gi_apply_lookup_ms_ps.hlsl.spirv.h"
#include "gi_apply_lookup_ps.hlsl.spirv.h"
#include "gi_apply_ms_ps.hlsl.spirv.h"
#include "gi_apply_ps.hlsl.spirv.h"
#include "vpl_hit_0_cs.hlsl.spirv.h"
#include "vpl_hit_1_cs.hlsl.spirv.h"
#include "vpl_hit_rt_0_cs.hlsl.spirv.h"
#include "vpl_hit_rt_1_cs.hlsl.spirv.h"
#include "light_irradiance_0_ps.hlsl.spirv.h"
#include "light_irradiance_1_ps.hlsl.spirv.h"
#include "light_composite_0_ps.hlsl.spirv.h"
#include "light_composite_1_ps.hlsl.spirv.h"
#include "vpl_irradiance_0_ps.hlsl.spirv.h"
#include "vpl_irradiance_1_ps.hlsl.spirv.h"
#include "vpl_composite_0_ps.hlsl.spirv.h"
#include "vpl_composite_1_ps.hlsl.spirv.h"
#include "gi_apply_up_ms_ps.hlsl.spirv.h"
#include "gi_apply_up_ps.hlsl.spirv.h"
#include "gi_field_reset_cs.hlsl.spirv.h"
#include "gi_field_view_ps.hlsl.spirv.h"
#include "gi_field_weights_cs.hlsl.spirv.h"
#include "gi_pixel_trace_cs.hlsl.spirv.h"
#include "gi_pixel_trace_ms_cs.hlsl.spirv.h"
#include "gi_update_blend_cs.hlsl.spirv.h"
#include "gi_update_trace_cs.hlsl.spirv.h"
#include "hdr_compose_ps.hlsl.spirv.h"
#include "layers_resolve_ps.hlsl.spirv.h"
#include "layers_restore_ps.hlsl.spirv.h"
#include "local_light_ms_ps.hlsl.spirv.h"
#include "local_light_ps.hlsl.spirv.h"
#include "overlay_hdr_ps.hlsl.spirv.h"
#include "overlay_ps.hlsl.spirv.h"
#include "overlay_vs.hlsl.spirv.h"
#include "post_luma_ps.hlsl.spirv.h"
#include "present_hdr_ps.hlsl.spirv.h"
#include "present_ps.hlsl.spirv.h"
#include "present_stretch_hdr_ps.hlsl.spirv.h"
#include "present_stretch_ps.hlsl.spirv.h"
#include "reduce_ps.hlsl.spirv.h"
#include "resolve_color_ms_ps.hlsl.spirv.h"
#include "resolve_color_ps.hlsl.spirv.h"
#include "resolve_depth_fused_frac_ps.hlsl.spirv.h"
#include "resolve_depth_fused_ps.hlsl.spirv.h"
#include "resolve_depth_ms_fused_frac_ps.hlsl.spirv.h"
#include "resolve_depth_ms_fused_ps.hlsl.spirv.h"
#include "resolve_depth_ms_ps.hlsl.spirv.h"
#include "resolve_depth_ps.hlsl.spirv.h"
#include "resolve_memory_color_cs.hlsl.spirv.h"
#include "resolve_memory_depth_cs.hlsl.spirv.h"
#include "resolve_memory_depth_ms_cs.hlsl.spirv.h"
#include "resolve_memory_ps.hlsl.spirv.h"
#include "resolve_memory_vertex_cs.hlsl.spirv.h"
#include "screen_colour_mip_ps.hlsl.spirv.h"
#include "screen_colour_ms_ps.hlsl.spirv.h"
#include "screen_colour_ps.hlsl.spirv.h"
#include "screen_hiz_ps.hlsl.spirv.h"
#include "screen_view_ps.hlsl.spirv.h"
#include "ssr_apply_ms_ps.hlsl.spirv.h"
#include "ssr_apply_ps.hlsl.spirv.h"
#include "ssr_apply_raw_ps.hlsl.spirv.h"
#include "ssr_hit_ps.hlsl.spirv.h"
#include "ssr_denoise_spatial_cs.hlsl.spirv.h"
#include "rt_denoise_plain_spatial_cs.hlsl.spirv.h"
#include "rt_denoise_plain_temporal_cs.hlsl.spirv.h"
#include "rt_denoise_diffuse_spatial_cs.hlsl.spirv.h"
#include "rt_denoise_diffuse_temporal_cs.hlsl.spirv.h"
#include "rt_denoise_vis_spatial_cs.hlsl.spirv.h"
#include "rt_denoise_vis_temporal_cs.hlsl.spirv.h"
#include "ssr_temporal_ps.hlsl.spirv.h"
#include "ssr_classify_cs.hlsl.spirv.h"
#include "ssr_classify_reproject_cs.hlsl.spirv.h"
#include "ssr_rt_select_cs.hlsl.spirv.h"
#include "ssr_rt_trace_cs.hlsl.spirv.h"
#include "ssr_rt_trace_ms_cs.hlsl.spirv.h"
#include "ssr_trace_ms_ps.hlsl.spirv.h"
#include "ssr_trace_ps.hlsl.spirv.h"
#include "ssr_trace_rt_ms_ps.hlsl.spirv.h"
#include "ssr_trace_rt_ps.hlsl.spirv.h"
#include "ssr_trace_reflhit_ps.hlsl.spirv.h"
#include "capture_road_reflection_ps.hlsl.spirv.h"
#include "ssr_trace_reflhit_ms_ps.hlsl.spirv.h"
#include "ssr_trace_rt_reflhit_ps.hlsl.spirv.h"
#include "ssr_trace_rt_reflhit_ms_ps.hlsl.spirv.h"
#include "ssr_rt_trace_reflhit_cs.hlsl.spirv.h"
#include "ssr_rt_trace_reflhit_ms_cs.hlsl.spirv.h"
#include "rr_guides_cs.hlsl.spirv.h"
#include "rr_guides_ms_cs.hlsl.spirv.h"
#include "rr_guides_view_cs.hlsl.spirv.h"
#include "ray_regen_ssr_cs.hlsl.spirv.h"
#include "ray_regen_bounce_cs.hlsl.spirv.h"
#include "rtao_denoise_spatial_cs.hlsl.spirv.h"
#include "rt_denoise_v2_ssr_spatial_cs.hlsl.spirv.h"
#include "rt_denoise_v2_ssr_temporal_cs.hlsl.spirv.h"
#include "rt_denoise_v2_ssr_temporal_ms_cs.hlsl.spirv.h"
#include "rt_denoise_v2_diffuse_spatial_cs.hlsl.spirv.h"
#include "rt_denoise_v2_diffuse_temporal_cs.hlsl.spirv.h"
#include "rt_denoise_v2_vis_spatial_cs.hlsl.spirv.h"
#include "rt_denoise_v2_vis_temporal_cs.hlsl.spirv.h"
#include "rt_denoise_v2_ao_spatial_cs.hlsl.spirv.h"
#include "rt_denoise_v2_ao_temporal_cs.hlsl.spirv.h"
#include "rt_denoise_v2_shadow_spatial_cs.hlsl.spirv.h"
#include "rt_denoise_v2_shadow_temporal_cs.hlsl.spirv.h"
#include "rtao_denoise_temporal_cs.hlsl.spirv.h"
#include "rtao_compose_cs.hlsl.spirv.h"
#include "rtao_compose_gi_cs.hlsl.spirv.h"
#include "ray_regen_ao_cs.hlsl.spirv.h"
#include "shafts_add_ps.hlsl.spirv.h"
#include "shafts_blur_ps.hlsl.spirv.h"
#include "shafts_temporal_ps.hlsl.spirv.h"
#include "shafts_sky_ms_ps.hlsl.spirv.h"
#include "shafts_sky_ps.hlsl.spirv.h"
#include "volumetrics_apply_ms_ps.hlsl.spirv.h"
#include "volumetrics_apply_ps.hlsl.spirv.h"
#include "atmosphere_apply_ps.hlsl.spirv.h"
#include "atmosphere_apply_ms_ps.hlsl.spirv.h"
#include "shafts_blur_legacy_ps.hlsl.spirv.h"
#include "shafts_blur_accumulate_ps.hlsl.spirv.h"
#include "volumetrics_inject_cs.hlsl.spirv.h"
#include "volumetrics_integrate_cs.hlsl.spirv.h"
#include "smaa_blend_ps.hlsl.spirv.h"
#include "smaa_edges_ps.hlsl.spirv.h"
#include "smaa_weights_ps.hlsl.spirv.h"
#include "temporal_copy_ps.hlsl.spirv.h"
#include "temporal_mv_ps.hlsl.spirv.h"
#include "temporal_vehicle_table_ps.hlsl.spirv.h"
#include "upscaler_masks_ms_ps.hlsl.spirv.h"
#include "upscaler_masks_ps.hlsl.spirv.h"
#endif

namespace mcla::gpu {

struct HostShaderBlob {
  const void* data;
  uint64_t size;
  const char* name;
};

#define MCLA_GPU_HOST_SHADER_DXIL(stem) \
  HostShaderBlob{g_##stem##_dxil, sizeof(g_##stem##_dxil), #stem}
#define MCLA_GPU_HOST_SHADER_SPIRV(stem) \
  HostShaderBlob{g_##stem##_spirv, sizeof(g_##stem##_spirv), #stem}
// The active backend's blob of `stem`.
#if defined(LARECOMP_GPU_HAS_D3D12) && defined(LARECOMP_GPU_HAS_VULKAN)
#define MCLA_GPU_HOST_SHADER_BLOB(stem)                                \
  (ActiveBackend() == Backend::kD3D12 ? MCLA_GPU_HOST_SHADER_DXIL(stem) \
                                      : MCLA_GPU_HOST_SHADER_SPIRV(stem))
#elif defined(LARECOMP_GPU_HAS_D3D12)
#define MCLA_GPU_HOST_SHADER_BLOB(stem) MCLA_GPU_HOST_SHADER_DXIL(stem)
#else
#define MCLA_GPU_HOST_SHADER_BLOB(stem) MCLA_GPU_HOST_SHADER_SPIRV(stem)
#endif

inline HostShaderBlob OverlayVertexShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(overlay_vs);
}

inline HostShaderBlob OverlayPixelShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(overlay_ps);
}

// HDR10 output (hdr.h): the overlay, the present, its stretch and FSR1's RCAS as they are with
// hlsl/hdr_output.hlsli's encoding as their last step, for an HDR10 back buffer.
inline HostShaderBlob OverlayHdrPixelShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(overlay_hdr_ps);
}

inline HostShaderBlob PresentHdrPixelShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(present_hdr_ps);
}

inline HostShaderBlob PresentStretchHdrPixelShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(present_stretch_hdr_ps);
}

inline HostShaderBlob FsrRcasHdrPixelShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(fsr_rcas_hdr_ps);
}

// The UI layer split (frame_layers.h, hdr_scene.h): the underlay drawn back under the UI into the
// guest's target, and the HDR present's compose of the underlay and the UI layer.
inline HostShaderBlob LayersRestorePixelShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(layers_restore_ps);
}

// The same layers' copy out of a multisampled target: the mean of its samples.
inline HostShaderBlob LayersResolvePixelShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(layers_resolve_ps);
}

inline HostShaderBlob HdrComposePixelShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(hdr_compose_ps);
}

inline HostShaderBlob FullscreenVertexShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(fullscreen_vs);
}

// The G-buffer's debug view (gbuffer.h): the planes with one sample, or the host's MSAA level.
inline HostShaderBlob GBufferViewPixelShader(bool multisampled) {
  return multisampled ? MCLA_GPU_HOST_SHADER_BLOB(gbuffer_view_ms_ps) : MCLA_GPU_HOST_SHADER_BLOB(gbuffer_view_ps);
}

inline HostShaderBlob NativeReflectionHitShader(bool multisampled) {
  return multisampled ? MCLA_GPU_HOST_SHADER_BLOB(reflhit_native_ms_ps) : MCLA_GPU_HOST_SHADER_BLOB(reflhit_native_ps);
}

// The probe field owner's passes (gi_field.h, CITY-RT M3 package G1): the reset of the probes a window move exposed and the weights pass (compute, Shader Model 6.0, typed RGBA16F /
// RG16F / R8 storage image stores on both APIs), and the debug view's pixel shader.
inline HostShaderBlob GiFieldResetShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(gi_field_reset_cs);
}

inline HostShaderBlob GiFieldWeightsShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(gi_field_weights_cs);
}

inline HostShaderBlob GiFieldViewShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(gi_field_view_ps);
}

// The probe update's passes (gi_update.h, CITY-RT M3 package G2): the trace (inline ray queries against the top level, Shader Model 6.5, the city hit library in GI mode) and the blend (Shader Model 6.0,
// typed RGBA16F / RG16F storage image loads and stores).
inline HostShaderBlob GiUpdateTraceShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(gi_update_trace_cs);
}

inline HostShaderBlob GiUpdateBlendShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(gi_update_blend_cs);
}

// The probe field's per-pixel detail pass (gi_pixel.h, CITY-RT M3 package G5): the trace (inline ray queries, Shader Model 6.5, the city hit library in GI mode) over plain planes, or over the host's MSAA
// level (hlsl/gi_pixel_trace_cs.hlsl, GIP_MS).
inline HostShaderBlob GiPixelTraceShader(bool multisampled) {
  return multisampled ? MCLA_GPU_HOST_SHADER_BLOB(gi_pixel_trace_ms_cs) : MCLA_GPU_HOST_SHADER_BLOB(gi_pixel_trace_cs);
}

// The probe field's apply (gi_apply.h, CITY-RT M3 package G3): 0 the lookup at the traced size, 1 the same over multisampled planes, 2 the apply, 3 the apply over multisampled planes, 4 the apply that takes
// the net irradiance from the lookup's image, 5 the same over multisampled planes. All pixel shaders of one source (hlsl/gi_apply_ps.hlsl).
inline HostShaderBlob VirtualBounceShader(uint32_t stage, bool ms) {
  switch (stage) {
    case 0: return ms ? MCLA_GPU_HOST_SHADER_BLOB(vpl_hit_1_cs) : MCLA_GPU_HOST_SHADER_BLOB(vpl_hit_0_cs);
    case 1: return ms ? MCLA_GPU_HOST_SHADER_BLOB(vpl_hit_rt_1_cs) : MCLA_GPU_HOST_SHADER_BLOB(vpl_hit_rt_0_cs);
    case 4: return ms ? MCLA_GPU_HOST_SHADER_BLOB(light_irradiance_1_ps) : MCLA_GPU_HOST_SHADER_BLOB(light_irradiance_0_ps);
    case 5: return ms ? MCLA_GPU_HOST_SHADER_BLOB(light_composite_1_ps) : MCLA_GPU_HOST_SHADER_BLOB(light_composite_0_ps);
    case 2: return ms ? MCLA_GPU_HOST_SHADER_BLOB(vpl_irradiance_1_ps) : MCLA_GPU_HOST_SHADER_BLOB(vpl_irradiance_0_ps);
    default: return ms ? MCLA_GPU_HOST_SHADER_BLOB(vpl_composite_1_ps) : MCLA_GPU_HOST_SHADER_BLOB(vpl_composite_0_ps);
  }
}

inline HostShaderBlob GiApplyShader(uint32_t which) {
  switch (which) {
    case 0: return MCLA_GPU_HOST_SHADER_BLOB(gi_apply_lookup_ps);
    case 1: return MCLA_GPU_HOST_SHADER_BLOB(gi_apply_lookup_ms_ps);
    case 2: return MCLA_GPU_HOST_SHADER_BLOB(gi_apply_ps);
    case 3: return MCLA_GPU_HOST_SHADER_BLOB(gi_apply_ms_ps);
    case 4: return MCLA_GPU_HOST_SHADER_BLOB(gi_apply_up_ps);
    default: return MCLA_GPU_HOST_SHADER_BLOB(gi_apply_up_ms_ps);
  }
}

// The deferred local light pass (local_lights.h, hlsl/local_light_ps.hlsl): planes and depth with one sample, or the host's MSAA level.
inline HostShaderBlob LocalLightPixelShader(bool multisampled) {
  return multisampled ? MCLA_GPU_HOST_SHADER_BLOB(local_light_ms_ps) : MCLA_GPU_HOST_SHADER_BLOB(local_light_ps);
}

// The shared screen products (screen_shared.h): one level of the Hi-Z chain, and the debug view of the products.
inline HostShaderBlob ScreenHizPixelShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(screen_hiz_ps);
}

// The shared screen products' FUSED depth chain (screen_shared.h, screen_extract.h Kind::kDepthHiz...): the AO's depth pass and its coarser level, each writing the Hi-Z's level of the same size beside its own
// image (a second render target). 0 the depth pass from a plain depth plane (ao_depth_ps.hlsl with AO_HIZ0), 1 from a multisampled one, 2 the coarser level (ao_mip_ps.hlsl with AO_HIZ).
inline HostShaderBlob ScreenFusedDepthPixelShader(uint32_t pass) {
  switch (pass) {
    case 0: return MCLA_GPU_HOST_SHADER_BLOB(ao_depth_hiz_ps);
    case 1: return MCLA_GPU_HOST_SHADER_BLOB(ao_depth_hiz_ms_ps);
    default: return MCLA_GPU_HOST_SHADER_BLOB(ao_mip_hiz_ps);
  }
}

inline HostShaderBlob ScreenViewPixelShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(screen_view_ps);
}

// The shared screen products' colour capture (screen_shared.h CapturePost, hlsl/screen_colour_ps.hlsl): 0 level 0 from a plain colour target, 1 from a multisampled one, 2 a mip level.
inline HostShaderBlob ScreenColourPixelShader(uint32_t pass) {
  switch (pass) {
    case 0: return MCLA_GPU_HOST_SHADER_BLOB(screen_colour_ps);
    case 1: return MCLA_GPU_HOST_SHADER_BLOB(screen_colour_ms_ps);
    default: return MCLA_GPU_HOST_SHADER_BLOB(screen_colour_mip_ps);
  }
}

// The screen-space reflections (ssr.h, hlsl/ssr_*_ps.hlsl): 0 the trace, 1 the trace over multisampled planes, 2 the accumulation, 3 the resolve into the main colour, 4 the same over a multisampled colour
// target and planes; 5 and 6 (CITY-RT R4) the trace and the multisampled trace with the confidence as a second target, for a frame the ray traced fallback follows (ssr_rt.h); 7 (RR-4a) the resolve that
// reads the trace itself (the frames DLSS Ray Reconstruction takes: no accumulation), 8 (RR-4a) the pass that writes their hit distance guide.
inline HostShaderBlob ScreenReflectionsPixelShader(uint32_t pass) {
  switch (pass) {
    case 0: return MCLA_GPU_HOST_SHADER_BLOB(ssr_trace_ps);
    case 1: return MCLA_GPU_HOST_SHADER_BLOB(ssr_trace_ms_ps);
    case 2: return MCLA_GPU_HOST_SHADER_BLOB(ssr_temporal_ps);
    case 3: return MCLA_GPU_HOST_SHADER_BLOB(ssr_apply_ps);
    case 4: return MCLA_GPU_HOST_SHADER_BLOB(ssr_apply_ms_ps);
    case 5: return MCLA_GPU_HOST_SHADER_BLOB(ssr_trace_rt_ps);
    case 6: return MCLA_GPU_HOST_SHADER_BLOB(ssr_trace_rt_ms_ps);
    case 7: return MCLA_GPU_HOST_SHADER_BLOB(ssr_apply_raw_ps);
    default: return MCLA_GPU_HOST_SHADER_BLOB(ssr_hit_ps);
  }
}

// The ray traced fallback's compute passes (ssr_rt.h, CITY-RT R4): 0 the candidates' tile list, 1 the rays into the city's top level, 2 the same over multisampled planes (inline ray queries: Shader Model 6.5,
// SPIR-V 1.5 on Vulkan).
inline HostShaderBlob SsrClassifyShader(bool reproject) {
  return reproject ? MCLA_GPU_HOST_SHADER_BLOB(ssr_classify_reproject_cs) : MCLA_GPU_HOST_SHADER_BLOB(ssr_classify_cs);
}

inline HostShaderBlob ScreenReflectionsRtShader(uint32_t pass) {
  switch (pass) {
    case 0: return MCLA_GPU_HOST_SHADER_BLOB(ssr_rt_select_cs);
    case 1: return MCLA_GPU_HOST_SHADER_BLOB(ssr_rt_trace_cs);
    default: return MCLA_GPU_HOST_SHADER_BLOB(ssr_rt_trace_ms_cs);
  }
}

inline HostShaderBlob CaptureRoadReflectionShader() { return MCLA_GPU_HOST_SHADER_BLOB(capture_road_reflection_ps); }

inline HostShaderBlob ScreenReflectionsHitShader(bool rt, bool ms) {
  if (rt) return ms ? MCLA_GPU_HOST_SHADER_BLOB(ssr_trace_rt_reflhit_ms_ps) : MCLA_GPU_HOST_SHADER_BLOB(ssr_trace_rt_reflhit_ps);
  return ms ? MCLA_GPU_HOST_SHADER_BLOB(ssr_trace_reflhit_ms_ps) : MCLA_GPU_HOST_SHADER_BLOB(ssr_trace_reflhit_ps);
}

inline HostShaderBlob ScreenReflectionsRtHitShader(bool ms) {
  return ms ? MCLA_GPU_HOST_SHADER_BLOB(ssr_rt_trace_reflhit_ms_cs) : MCLA_GPU_HOST_SHADER_BLOB(ssr_rt_trace_reflhit_cs);
}

// RtDenoise (rt_denoise.h, CITY-RT R6): the screen-space reflections' spatial pass (hlsl/ssr_denoise_spatial_cs.hlsl over plain planes), and the plain-guide compiles of the two stages the other
// consumers start from (hlsl/rt_denoise_plain_consumer.hlsli). Plain Shader Model 6.0 compute.
inline HostShaderBlob SsrDenoiseSpatialShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(ssr_denoise_spatial_cs);
}
inline HostShaderBlob RtDenoisePlainShader(bool temporal) {
  return temporal ? MCLA_GPU_HOST_SHADER_BLOB(rt_denoise_plain_temporal_cs) : MCLA_GPU_HOST_SHADER_BLOB(rt_denoise_plain_spatial_cs);
}
// The same two stages compiled with the DIFFUSE profile (RT_DN_PROFILE_DIFFUSE: a fixed radius, no hit distance, looser weights): the probe field's per-pixel detail pass's denoiser (gi_pixel.h).
inline HostShaderBlob RtDenoiseDiffuseShader(bool temporal) {
  return temporal ? MCLA_GPU_HOST_SHADER_BLOB(rt_denoise_diffuse_temporal_cs) : MCLA_GPU_HOST_SHADER_BLOB(rt_denoise_diffuse_spatial_cs);
}
// The same two stages over 8 bit images and the visibility's lighter guides (rt_lights.h, RT-LIGHTS-D1 part 2: RT_DN_VIS, RT_DN_OUT_UNORM8).
inline HostShaderBlob RtDenoiseVisShader(bool temporal) {
  return temporal ? MCLA_GPU_HOST_SHADER_BLOB(rt_denoise_vis_temporal_cs) : MCLA_GPU_HOST_SHADER_BLOB(rt_denoise_vis_spatial_cs);
}

// DLSS Ray Reconstruction's guide pass (rr_guides.h, hlsl/rr_guides_cs.hlsl): over planes with one sample, or the host's MSAA level; and its debug view over the upscaler's output
// (hlsl/rr_guides_view_cs.hlsl). Plain Shader Model 6.0 compute.
inline HostShaderBlob RrGuidesShader(bool multisampled) {
  return multisampled ? MCLA_GPU_HOST_SHADER_BLOB(rr_guides_ms_cs) : MCLA_GPU_HOST_SHADER_BLOB(rr_guides_cs);
}

inline HostShaderBlob RrGuidesViewShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(rr_guides_view_cs);
}

// AMD FSR Ray Regeneration's guide and signal pass of the screen-space reflections (ray_regen_guides.h, hlsl/ray_regen_ssr_cs.hlsl): the trace's radiance and the five guides in AMD's formats, one
// compute pass at the trace's size. Plain Shader Model 6.0 compute; only ever run on Direct3D 12 (AMD ships no Vulkan provider), built for both backends like every host shader.
inline HostShaderBlob RtDenoiseAoShader(bool temporal) {
  return temporal ? MCLA_GPU_HOST_SHADER_BLOB(rtao_denoise_temporal_cs) : MCLA_GPU_HOST_SHADER_BLOB(rtao_denoise_spatial_cs);
}
inline HostShaderBlob RtDenoiseV2SsrShader(bool temporal) {
  return temporal ? MCLA_GPU_HOST_SHADER_BLOB(rt_denoise_v2_ssr_temporal_cs) : MCLA_GPU_HOST_SHADER_BLOB(rt_denoise_v2_ssr_spatial_cs);
}
inline HostShaderBlob RtDenoiseV2SsrMsShader() { return MCLA_GPU_HOST_SHADER_BLOB(rt_denoise_v2_ssr_temporal_ms_cs); }
inline HostShaderBlob RtDenoiseV2DiffuseShader(bool temporal) {
  return temporal ? MCLA_GPU_HOST_SHADER_BLOB(rt_denoise_v2_diffuse_temporal_cs) : MCLA_GPU_HOST_SHADER_BLOB(rt_denoise_v2_diffuse_spatial_cs);
}
inline HostShaderBlob RtDenoiseV2VisShader(bool temporal) {
  return temporal ? MCLA_GPU_HOST_SHADER_BLOB(rt_denoise_v2_vis_temporal_cs) : MCLA_GPU_HOST_SHADER_BLOB(rt_denoise_v2_vis_spatial_cs);
}
inline HostShaderBlob RtDenoiseV2AoShader(bool temporal) {
  return temporal ? MCLA_GPU_HOST_SHADER_BLOB(rt_denoise_v2_ao_temporal_cs) : MCLA_GPU_HOST_SHADER_BLOB(rt_denoise_v2_ao_spatial_cs);
}
inline HostShaderBlob RtAoComposeShader(bool gi) {
  return gi ? MCLA_GPU_HOST_SHADER_BLOB(rtao_compose_gi_cs) : MCLA_GPU_HOST_SHADER_BLOB(rtao_compose_cs);
}
inline HostShaderBlob RtDenoiseV2ShadowShader(bool temporal) {
  return temporal ? MCLA_GPU_HOST_SHADER_BLOB(rt_denoise_v2_shadow_temporal_cs) : MCLA_GPU_HOST_SHADER_BLOB(rt_denoise_v2_shadow_spatial_cs);
}
inline HostShaderBlob RayRegenAoShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(ray_regen_ao_cs);
}

inline HostShaderBlob RayRegenSsrShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(ray_regen_ssr_cs);
}

inline HostShaderBlob RayRegenBounceShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(ray_regen_bounce_cs);
}

inline HostShaderBlob PresentPixelShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(present_ps);
}

// FSR1 on the present (M6c.1): an EASU pass, the RCAS pass, and
// the present's bilinear stretch of an image the gamma table already went into.
inline HostShaderBlob FsrEasuPixelShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(fsr_easu_ps);
}

inline HostShaderBlob FsrRcasPixelShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(fsr_rcas_ps);
}

inline HostShaderBlob PresentStretchPixelShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(present_stretch_ps);
}

// The scene's anti-aliasing before the UI (M6c.2/M6c.3): the copy
// with the luma in alpha, FXAA, and SMAA's passes (1 edges, 2 weights, 3 blend).
inline HostShaderBlob PostLumaPixelShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(post_luma_ps);
}

inline HostShaderBlob FxaaPixelShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(fxaa_ps);
}

inline HostShaderBlob SmaaPixelShader(int pass) {
  if (pass == 1) {
    return MCLA_GPU_HOST_SHADER_BLOB(smaa_edges_ps);
  }
  return pass == 2 ? MCLA_GPU_HOST_SHADER_BLOB(smaa_weights_ps)
                   : MCLA_GPU_HOST_SHADER_BLOB(smaa_blend_ps);
}

// The resolve conversion (resolve_ps.hlsl) by source kind.
// A resolve product reduced to a reader at a smaller resolution scale
// (ResolveCache::ReducedSlot).
inline HostShaderBlob ReducePixelShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(reduce_ps);
}

inline HostShaderBlob ResolvePixelShader(bool depth, bool msaa) {
  if (depth) {
    return msaa ? MCLA_GPU_HOST_SHADER_BLOB(resolve_depth_ms_ps)
                : MCLA_GPU_HOST_SHADER_BLOB(resolve_depth_ps);
  }
  return msaa ? MCLA_GPU_HOST_SHADER_BLOB(resolve_color_ms_ps)
              : MCLA_GPU_HOST_SHADER_BLOB(resolve_color_ps);
}

// The depth conversion that also writes the resolve memory's dwords of the same
// resolve (resolve_ps.hlsl RESOLVE_FUSED), by multisampled source or not, and for a
// whole resolution scale or a fractional one.
inline HostShaderBlob ResolveFusedPixelShader(bool msaa, bool fractional) {
  if (fractional) {
    return msaa ? MCLA_GPU_HOST_SHADER_BLOB(resolve_depth_ms_fused_frac_ps)
                : MCLA_GPU_HOST_SHADER_BLOB(resolve_depth_fused_frac_ps);
  }
  return msaa ? MCLA_GPU_HOST_SHADER_BLOB(resolve_depth_ms_fused_ps)
              : MCLA_GPU_HOST_SHADER_BLOB(resolve_depth_fused_ps);
}

// A resolve's bytes into the resolve memory (resolve_memory_cs.hlsl): colour
// from the product, depth from the EDRAM target (multisampled or not).
inline HostShaderBlob ResolveMemoryEncodeShader(bool depth, bool msaa) {
  if (depth) {
    return msaa ? MCLA_GPU_HOST_SHADER_BLOB(resolve_memory_depth_ms_cs)
                : MCLA_GPU_HOST_SHADER_BLOB(resolve_memory_depth_cs);
  }
  return MCLA_GPU_HOST_SHADER_BLOB(resolve_memory_color_cs);
}

// The temporal system's passes (temporal_mv.h): the motion vectors and the copy of the depth it keeps.
inline HostShaderBlob TemporalMotionPixelShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(temporal_mv_ps);
}

inline HostShaderBlob TemporalCopyPixelShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(temporal_copy_ps);
}

// One row of the vehicle table the motion vector pass reads (temporal_mv.h, vehicle_motion.h), and the
// R8 masks the upscalers take as their reactive and transparency inputs (upscaler_masks.h).
inline HostShaderBlob TemporalVehicleTablePixelShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(temporal_vehicle_table_ps);
}

inline HostShaderBlob UpscalerMasksPixelShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(upscaler_masks_ps);
}

// The same masks from a coverage image with several samples (the host's MSAA level under an upscaler).
inline HostShaderBlob UpscalerMasksMsPixelShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(upscaler_masks_ms_ps);
}

// Frame generation (frame_generation.h): the scene prepared for the interpolation, the generated
// frame composed with the UI layer, and the pacer's copy of a present image into the back buffer.
inline HostShaderBlob FrameGenerationPreparePixelShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(fg_prepare_ps);
}

inline HostShaderBlob FrameGenerationComposePixelShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(fg_compose_ps);
}

inline HostShaderBlob FrameGenerationCopyPixelShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(fg_copy_ps);
}

// DLSS frame generation (dlss_frame_generation.h): the UI layer in the back buffer's terms, for an SDR
// and for an HDR10 back buffer.
inline HostShaderBlob DlssFrameGenerationUiPixelShader(bool hdr) {
  return hdr ? MCLA_GPU_HOST_SHADER_BLOB(dlssg_ui_hdr_ps) : MCLA_GPU_HOST_SHADER_BLOB(dlssg_ui_ps);
}

// The ambient occlusion's passes (ao.h, hlsl/ao_*_ps.hlsl), numbered as AmbientOcclusion::Pass: 0
// the linear depth, 1 the same from a multisampled depth plane, 2 a coarser level, 3 the
// visibility, 4 the denoise, 5 the apply, 6 the apply over a multisampled depth plane, 7 the
// accumulation over frames, 8 the same over a multisampled depth target, 9 the contact shadows' ray
// march (ao_contact_ps.hlsl), 10 the visibility bitmask that gives the occlusion and a bounce
// (ao_main_ps.hlsl compiled with AO_GI), 11 the scene colour's level 0 from a plain colour plane, 12
// the same from a multisampled one, 13 a coarser level of the colour chain, 14 the debug picture of it
// (ao_color_ps.hlsl; G7a), 15 the denoise over four channels, 16 and 17 the accumulation over frames over
// four channels (from a plain and from a multisampled depth target's stencil), 18 the a-trous filter of
// the bounce (ao_denoise_ps.hlsl and ao_temporal_ps.hlsl compiled with AO_GI, ao_atrous_ps.hlsl; G7b), 19 and 20
// the apply pass with the bounce light, for a plain and for a multisampled colour target (ao_apply_ps.hlsl
// compiled with AO_GI; G7c), then the visibility pass with its counts compiled in (ao_fast.h; ao_main_ps.hlsl with
// AO_SLICES, AO_STEPS and for the bitmask AO_FAR_STEPS): 21 the occlusion's at 3 slices of 3 steps, 22 at 2 of 2, 23 the
// bitmask's at 3x3 with 2 ring steps and the polynomial acos, 24 at 2x2 with 1, 25 and 26 the same with the hardware's acos, then the normal image's pass (GB-E,
// ao_normal_ps.hlsl): 27 for a G-buffer plane with one sample, 28 for the host's MSAA level.
inline HostShaderBlob AmbientOcclusionPixelShader(uint32_t pass) {
  switch (pass) {
    case 0: return MCLA_GPU_HOST_SHADER_BLOB(ao_depth_ps);
    case 1: return MCLA_GPU_HOST_SHADER_BLOB(ao_depth_ms_ps);
    case 2: return MCLA_GPU_HOST_SHADER_BLOB(ao_mip_ps);
    case 3: return MCLA_GPU_HOST_SHADER_BLOB(ao_main_ps);
    case 4: return MCLA_GPU_HOST_SHADER_BLOB(ao_denoise_ps);
    case 5: return MCLA_GPU_HOST_SHADER_BLOB(ao_apply_ps);
    case 6: return MCLA_GPU_HOST_SHADER_BLOB(ao_apply_ms_ps);
    case 7: return MCLA_GPU_HOST_SHADER_BLOB(ao_temporal_ps);
    case 8: return MCLA_GPU_HOST_SHADER_BLOB(ao_temporal_ms_ps);
    case 10: return MCLA_GPU_HOST_SHADER_BLOB(ao_main_gi_ps);
    case 11: return MCLA_GPU_HOST_SHADER_BLOB(ao_color_ps);
    case 12: return MCLA_GPU_HOST_SHADER_BLOB(ao_color_ms_ps);
    case 13: return MCLA_GPU_HOST_SHADER_BLOB(ao_color_mip_ps);
    case 14: return MCLA_GPU_HOST_SHADER_BLOB(ao_color_view_ps);
    case 15: return MCLA_GPU_HOST_SHADER_BLOB(ao_denoise_gi_ps);
    case 16: return MCLA_GPU_HOST_SHADER_BLOB(ao_temporal_gi_ps);
    case 17: return MCLA_GPU_HOST_SHADER_BLOB(ao_temporal_gi_ms_ps);
    case 18: return MCLA_GPU_HOST_SHADER_BLOB(ao_atrous_ps);
    case 19: return MCLA_GPU_HOST_SHADER_BLOB(ao_apply_gi_ps);
    case 20: return MCLA_GPU_HOST_SHADER_BLOB(ao_apply_gi_ms_ps);
    case 21: return MCLA_GPU_HOST_SHADER_BLOB(ao_main_3x3_ps);
    case 22: return MCLA_GPU_HOST_SHADER_BLOB(ao_main_2x2_ps);
    case 23: return MCLA_GPU_HOST_SHADER_BLOB(ao_main_gi_3x3_far2_ps);
    case 24: return MCLA_GPU_HOST_SHADER_BLOB(ao_main_gi_2x2_far1_ps);
    case 25: return MCLA_GPU_HOST_SHADER_BLOB(ao_main_gi_3x3_far2_exact_ps);
    case 26: return MCLA_GPU_HOST_SHADER_BLOB(ao_main_gi_2x2_far1_exact_ps);
    case 27: return MCLA_GPU_HOST_SHADER_BLOB(ao_normal_ps);
    case 28: return MCLA_GPU_HOST_SHADER_BLOB(ao_normal_ms_ps);
    case 29: return MCLA_GPU_HOST_SHADER_BLOB(ao_main_gi_2x4_far1_ps);
    case 30: return MCLA_GPU_HOST_SHADER_BLOB(ao_main_gi_2x4_far1_exact_ps);
    default: return MCLA_GPU_HOST_SHADER_BLOB(ao_contact_ps);
  }
}

// The volumetric light's passes (volumetrics.h, hlsl/volumetrics_*.hlsl): the inject and the integrate compute passes, and the composite (the
// multisampled one reads the depth plane as a Texture2DMS).
inline HostShaderBlob VolumetricsInjectShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(volumetrics_inject_cs);
}
inline HostShaderBlob VolumetricsIntegrateShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(volumetrics_integrate_cs);
}
inline HostShaderBlob VolumetricsApplyShader(bool multisampled, bool shafts) {
  if (shafts) {
    return multisampled ? MCLA_GPU_HOST_SHADER_BLOB(atmosphere_apply_ms_ps) : MCLA_GPU_HOST_SHADER_BLOB(atmosphere_apply_ps);
  }
  return multisampled ? MCLA_GPU_HOST_SHADER_BLOB(volumetrics_apply_ms_ps) : MCLA_GPU_HOST_SHADER_BLOB(volumetrics_apply_ps);
}

// The sun shafts' passes (shafts.h, hlsl/shafts_*_ps.hlsl), numbered as SunShafts::Pass: 0 the sky's
// brightness from the depth and colour planes, 1 the same from multisampled planes, 2 the radial
// blur toward the sun, 3 the light added to the scene colour, 4 the blurred image averaged over frames (no temporal upscaler: noise_resolve.h).
inline HostShaderBlob SunShaftsPixelShader(uint32_t pass) {
  switch (pass) {
    case 0: return MCLA_GPU_HOST_SHADER_BLOB(shafts_sky_ps);
    case 1: return MCLA_GPU_HOST_SHADER_BLOB(shafts_sky_ms_ps);
    case 2: return MCLA_GPU_HOST_SHADER_BLOB(shafts_blur_ps);
    case 3: return MCLA_GPU_HOST_SHADER_BLOB(shafts_add_ps);
    case 5: return MCLA_GPU_HOST_SHADER_BLOB(shafts_blur_legacy_ps);
    case 6: return MCLA_GPU_HOST_SHADER_BLOB(shafts_blur_accumulate_ps);
    default: return MCLA_GPU_HOST_SHADER_BLOB(shafts_temporal_ps);
  }
}

// The car self-reflection history pass (car_selfrefl.h, hlsl/car_history_ps.hlsl), numbered as CarSelfRefl::Pass: 0 from plain
// planes, 1 from multisampled planes, 2 from multisampled depth and stencil planes with the colour taken from the shared screen products' colour_post.
inline HostShaderBlob CarHistoryPixelShader(uint32_t pass) {
  switch (pass) {
    case 0: return MCLA_GPU_HOST_SHADER_BLOB(car_history_ps);
    case 1: return MCLA_GPU_HOST_SHADER_BLOB(car_history_ms_ps);
    default: return MCLA_GPU_HOST_SHADER_BLOB(car_history_ms_shared_ps);
  }
}

// The player's car as ray tracing structures (car_rt_scene.h): the check pass, a compute shader with inline ray queries (Shader Model 6.5,
// SPIR-V 1.5 for Vulkan 1.2; built with larecomp_gpu_host_shader_rt), the ray traced self-reflection's trace (car_rt_refl.h, the same kind) and
// its denoise (plain compute), and the ray traced occlusion and sun term around the car (car_rt_ao.h, the same kind as the trace). The renderer
// creates the ones with ray queries only where CarRtSupported said yes (VK_KHR_ray_query on Vulkan): a device without the feature refuses the
// SPIR-V module. The other, a pixel shader that puts the check's picture over the scene, is an ordinary host shader.
inline HostShaderBlob CarRtCheckShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(car_rt_check_cs);
}

inline HostShaderBlob CarRtAoShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(car_rt_ao_cs);
}

// The city's ray traced sun shadows (city_rt_shadow.h): inline ray queries against the car scene's top level, which holds the city's sectors beside the cars.
inline HostShaderBlob CityRtShadowShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(city_rt_shadow_cs);
}

inline HostShaderBlob RtAoTraceShader(bool ms) {
  return ms ? MCLA_GPU_HOST_SHADER_BLOB(rtao_trace_ms_cs) : MCLA_GPU_HOST_SHADER_BLOB(rtao_trace_cs);
}

// The ray traced visibility of the top K lights (rt_lights.h, RT-LIGHTS-D1): the rays' trace into the visibility images, over the occlusion's linear depth.
inline HostShaderBlob RtLightsTraceShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(rt_lights_trace_cs);
}

// The city hit data's probe (city_hit_probe.h, CITY-RT M2 H3): reads the pool and the tables through the ray tracing descriptor set's t6..t9.
inline HostShaderBlob CityHitProbeShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(city_hit_probe_cs);
}

// The ray tracing benchmark's trace (rt_bench.h, hlsl/rt_bench_cs.hlsl): inline ray queries over a synthetic city, the same kind as the car passes. Created only
// where CarRtSupported said yes, once, on the first start with a card.
inline HostShaderBlob RtBenchShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(rt_bench_cs);
}

inline HostShaderBlob CarRtReflShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(car_rt_refl_cs);
}

inline HostShaderBlob CarRtReflDenoiseShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(car_rt_refl_denoise_cs);
}

// RR-4b: the raw mode's hit distance pass (hlsl/car_rt_refl_hit_cs.hlsl), on the denoise's layout.
inline HostShaderBlob CarRtReflHitShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(car_rt_refl_hit_cs);
}

// The opponents' variant of the trace (car_rt_opp_refl.h): hlsl/car_rt_refl_cs.hlsl built with CAR_RT_REFL_OPPONENTS (its own entry point, hlsl/car_rt_refl_opp.hlsli), the same kind as the trace.
inline HostShaderBlob CarRtReflOppShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(car_rt_refl_opp_cs);
}

// The glass variant of the trace (car_rt_glass_refl.h, CITY-RT M2 R3b): hlsl/car_rt_refl_cs.hlsl built with CAR_RT_REFL_GLASS (its own entry point, hlsl/car_rt_refl_glass.hlsli), the same kind as the trace; and the
// additive pass that puts its image into the main colour (hlsl/car_glass_apply_ps.hlsl, a host pass pixel shader).
inline HostShaderBlob CarRtReflGlassShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(car_rt_refl_glass_cs);
}

inline HostShaderBlob CarGlassApplyShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(car_glass_apply_ps);
}

inline HostShaderBlob CarRtViewPixelShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(car_rt_view_ps);
}

// A texture level decoded from the resolve memory (resolve_memory_ps.hlsl).
inline HostShaderBlob ResolveMemoryDecodeShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(resolve_memory_ps);
}

// A vertex stream built from the resolve memory (resolve_memory_vertex_cs.hlsl,).
inline HostShaderBlob ResolveMemoryVertexShader() {
  return MCLA_GPU_HOST_SHADER_BLOB(resolve_memory_vertex_cs);
}

}  // namespace mcla::gpu
