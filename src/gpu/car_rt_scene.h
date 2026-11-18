#pragma once
// ===========================================================================
// MCLA GPU renderer -- the player's car as ray tracing acceleration structures
// ===========================================================================
// For ray traced car-only effects (first: the car's reflections of itself, car_rt_refl). One bottom-level structure per mesh
// of the player's car, built ONCE from guest memory when the mesh is first seen (car_frame.h MeshReader) and kept while its
// key lives; every frame one top-level structure with one instance per draw of the car, placed by the draw's matrix. The
// instances are in the frame's VIEW space (the AO's: x right, y up, z forward = the linear depth t; ao_common.hlsli
// AoViewPos), so a pass at the history trigger traces rays in the same space as the depth image and the history image.
//
// D3D12 with DXR tier 1.1 (inline ray queries, Shader Model 6.5) first. Vulkan: off until Plume enables VK_KHR_ray_query
// (separate Plume branch); keep the decision in ONE function, CarRtSupported.
//
// CONTRACT (2026-11-18): feat/car-rt-scene implements CarRtScene (car_rt_scene.cpp), hlsl/car_rt_common.hlsli and the
// CMake function larecomp_gpu_host_shader_rt; feat/car-rt-refl uses View(), the descriptor set and the .hlsli. The names
// below are fixed; the class may grow.
//
// Threading: render thread only. Initialize and Shutdown on the lifecycle thread.
// ===========================================================================

#include <cstdint>

#include <plume_render_interface.h>

namespace mcla::gpu {

// The descriptor set every ray traced car pass binds (hlsl/car_rt_common.hlsli declares it; the bindings, in this order):
//   0  RaytracingAccelerationStructure  the frame's top level
//   1  StructuredBuffer<uint4>          per instance (InstanceIndex()): x = its mesh's first index in buffer 2 (uint32 units),
//                                       y = its mesh's first vertex in buffer 3, z = flags (car_rt_common.hlsli CAR_RT_*),
//                                       w = the draw's place in car_frame::Frame::draws
//   2  ByteAddressBuffer                every mesh's triangle-list indices (uint32, relative to the mesh's first vertex)
//   3  ByteAddressBuffer                every mesh's vertex normals (float3, 12 bytes a vertex, model space, unit length)
struct CarRtView {
  bool valid = false;                         // this frame's top level was built and recorded on the list before the caller's pass
  plume::RenderDescriptorSet* set = nullptr;  // bound by the caller at its own set index
  uint32_t instance_count = 0;
};

// The decision whether this device can run the ray traced car effects (D3D12: DXR 1.1; Vulkan: false until Plume has
// VK_KHR_ray_query).
bool CarRtSupported(const plume::RenderDeviceCapabilities& caps, bool vulkan);

// The implementer adds the class here: Initialize/Shutdown, the car_frame::Listener for the mesh builds, a per-frame entry the
// Scene calls at the history trigger (it records the top-level build on that list), and:
//   const CarRtView& View() const;
//   const plume::RenderDescriptorSetDesc& SetDesc() const;   // for the caller's pipeline layout
class CarRtScene;

}  // namespace mcla::gpu
