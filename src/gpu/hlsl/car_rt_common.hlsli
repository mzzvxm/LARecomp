// MCLA GPU renderer -- the player's car as ray tracing structures, the shader side (car_rt_scene.h).
//
// CONTRACT (2026-11-18): feat/car-rt-scene implements this file; feat/car-rt-refl includes it. The names below are fixed.
// Shader Model 6.5 (inline ray queries). The includer defines CAR_RT_SPACE (the register space its pipeline layout gives the
// set) before including this file.

#ifndef CAR_RT_COMMON_HLSLI
#define CAR_RT_COMMON_HLSLI

#ifndef CAR_RT_SPACE
#error "define CAR_RT_SPACE (the register space of the car RT descriptor set) before including car_rt_common.hlsli"
#endif

// Instance flags (CarRtInstance.z) and instance masks (the top level's InstanceMask, for a ray's mask).
#define CAR_RT_MASK_BODY 0x01     // the opaque rigid parts
#define CAR_RT_MASK_GLASS 0x02    // drawn after the world: glass, lights and other translucent parts
#define CAR_RT_MASK_TYRES 0x04
#define CAR_RT_MASK_DRIVER 0x08   // skinned (may be absent in a first version)

// Declarations (the implementer writes them with these names, at registers t0..t3 of space CAR_RT_SPACE):
//   RaytracingAccelerationStructure gCarRtTlas;
//   StructuredBuffer<uint4> gCarRtInstances;
//   ByteAddressBuffer gCarRtIndices;
//   ByteAddressBuffer gCarRtNormals;
//
// Helpers (the implementer writes them):
//   // The unit normal of a committed triangle hit in VIEW space, interpolated from the mesh's vertex normals, turned to face
//   // the side the ray came from.
//   float3 CarRtHitNormal(uint instance_index, uint primitive_index, float2 barycentrics, float3x4 object_to_world,
//                         float3 ray_direction);

#endif  // CAR_RT_COMMON_HLSLI
