// MCLA GPU renderer -- the player's car as ray tracing structures, the shader side (car_rt_scene.h).
//
// The ray traced self-reflection (car_rt_refl_cs.hlsl) and the check pass (car_rt_check_cs.hlsl) include it.
// Shader Model 6.5 (inline ray queries). The includer defines CAR_RT_SPACE (the register space its pipeline layout gives the
// set) before including this file.
//
// The top level holds one instance per draw of the player's car (and of the other cars beside it: CAR_RT_MASK_OTHERS) in the frame's VIEW space (x right, y up, z forward: the linear depth t
// of ao_common.hlsli AoViewPos, so a ray from the origin through a pixel with the direction (ndc.x * tan_x, ndc.y * tan_y, 1) hits at
// RayT = the view-space z of the hit = the depth image's value). An instance's index (InstanceIndex() and InstanceID() are the same
// number) is the row of gCarRtInstances that says which mesh it is and how the draw was classed.

#ifndef CAR_RT_COMMON_HLSLI
#define CAR_RT_COMMON_HLSLI

#ifndef CAR_RT_SPACE
#error "define CAR_RT_SPACE (the register space of the car RT descriptor set) before including car_rt_common.hlsli"
#endif

// Instance flags (CarRtInstance.z) and instance masks (the top level's InstanceMask, for a ray's mask). The flags word holds the mask in
// its low 8 bits and the CAR_RT_FLAG_* above them (car_rt_scene_rules.h has the same numbers; car_rt_scene_rules_test.cpp checks them).
#define CAR_RT_MASK_BODY 0x01     // the opaque rigid parts
#define CAR_RT_MASK_GLASS 0x02    // drawn after the world: glass, lights and other translucent parts
#define CAR_RT_MASK_TYRES 0x04
#define CAR_RT_MASK_DRIVER 0x08   // skinned (may be absent in a first version)
#define CAR_RT_MASK_OTHERS 0x10   // another car (an opponent, traffic, a parked car): the reflection's rays hit it, the primary ray never does
#define CAR_RT_MASK_ALL 0xFF
#define CAR_RT_FLAG_AFTER_WORLD 0x100  // the draw came after the frame's first world draw

// The set, at registers t0..t3 of space CAR_RT_SPACE.
#define CAR_RT_JOIN_(a, b) a##b
#define CAR_RT_JOIN(a, b) CAR_RT_JOIN_(a, b)
#define CAR_RT_SPACE_NAME CAR_RT_JOIN(space, CAR_RT_SPACE)

// The frame's top level, in view space.
RaytracingAccelerationStructure gCarRtTlas : register(t0, CAR_RT_SPACE_NAME);
// Per instance (InstanceIndex()): x = its mesh's first index in gCarRtIndices (uint32 units), y = its mesh's first vertex in gCarRtNormals
// (vertices of 12 bytes), z = the flags word, w = the draw's place in car_frame::Frame::draws; for an instance of another car (mask
// CAR_RT_MASK_OTHERS) w is instead the ROW of the car's record in this same table (after the instances' rows): see CarRtCar.
StructuredBuffer<uint4> gCarRtInstances : register(t1, CAR_RT_SPACE_NAME);
// Every mesh's triangle-list indices (uint32, relative to the mesh's first vertex).
ByteAddressBuffer gCarRtIndices : register(t2, CAR_RT_SPACE_NAME);
// Every mesh's vertex normals (float3, 12 bytes a vertex, model space, unit length).
ByteAddressBuffer gCarRtNormals : register(t3, CAR_RT_SPACE_NAME);

struct CarRtInstance {
  uint first_index;
  uint first_vertex;
  uint flags;
  uint draw;
};

// The record of another car (car_rt_instances_rules.h CarRow), in the row an instance of that car names in its w: the car's body centre in the
// frame's view space and its radius in metres (0: no centre is known: the car has no colour).
struct CarRtCar {
  float3 centre;
  float radius;
};

CarRtCar CarRtLoadCar(CarRtInstance i) {
  const uint4 row = gCarRtInstances[i.draw];
  CarRtCar c;
  c.centre = asfloat(row.xyz);
  c.radius = asfloat(row.w);
  return c;
}

CarRtInstance CarRtLoadInstance(uint instance_index) {
  const uint4 row = gCarRtInstances[instance_index];
  CarRtInstance i;
  i.first_index = row.x;
  i.first_vertex = row.y;
  i.flags = row.z;
  i.draw = row.w;
  return i;
}

uint CarRtInstanceMask(CarRtInstance i) {
  return i.flags & 0xFFu;
}

// The unit normal of a committed triangle hit in VIEW space, interpolated from the mesh's vertex normals, turned to face
// the side the ray came from.
//
// `barycentrics` are the committed hit's (b1, b2): the point is (1 - b1 - b2) v0 + b1 v1 + b2 v2. The normal is carried by the
// cofactor matrix of the instance's 3x3, which is the inverse transpose up to a scale and the sign of the determinant, so it is right for
// any invertible matrix: the draws' matrices are rigid, but the game scales parts (the body's squash and stretch), where the rotation
// alone would tilt the normal. The sign of the determinant is taken out (a mirrored matrix would turn the normal inside out); the last
// step turns it against the ray anyway, so the winding of the mesh does not matter.
float3 CarRtHitNormal(uint instance_index, uint primitive_index, float2 barycentrics, float3x4 object_to_world, float3 ray_direction) {
  const uint4 row = gCarRtInstances[instance_index];
  const uint tri = (row.x + primitive_index * 3u) * 4u;
  const uint3 vertex = uint3(gCarRtIndices.Load(tri), gCarRtIndices.Load(tri + 4u), gCarRtIndices.Load(tri + 8u)) + row.y;
  const float3 n0 = asfloat(gCarRtNormals.Load3(vertex.x * 12u));
  const float3 n1 = asfloat(gCarRtNormals.Load3(vertex.y * 12u));
  const float3 n2 = asfloat(gCarRtNormals.Load3(vertex.z * 12u));
  const float3 n = n0 * (1.0f - barycentrics.x - barycentrics.y) + n1 * barycentrics.x + n2 * barycentrics.y;
  const float3 a0 = object_to_world[0].xyz;
  const float3 a1 = object_to_world[1].xyz;
  const float3 a2 = object_to_world[2].xyz;
  const float3 c0 = cross(a1, a2);
  const float3 c1 = cross(a2, a0);
  const float3 c2 = cross(a0, a1);
  const float determinant = dot(a0, c0);
  float3 world = float3(dot(c0, n), dot(c1, n), dot(c2, n)) * (determinant < 0.0f ? -1.0f : 1.0f);
  const float length_squared = dot(world, world);
  world = length_squared > 1e-30f ? world * rsqrt(length_squared) : float3(0.0f, 0.0f, -1.0f);
  return dot(world, ray_direction) > 0.0f ? -world : world;
}

// The nearest hit of a ray whose instances' masks meet `mask` (CAR_RT_MASK_*; the ray's direction need not be a unit vector: t is in its
// units). Every part of the car is built opaque, so no candidate is ever asked about.
struct CarRtHit {
  bool hit;
  float t;
  uint instance_index;
  uint primitive_index;
  float2 barycentrics;
  float3x4 object_to_world;
};

CarRtHit CarRtTrace(float3 origin, float3 direction, float t_min, float t_max, uint mask) {
  RayDesc ray;
  ray.Origin = origin;
  ray.Direction = direction;
  ray.TMin = t_min;
  ray.TMax = t_max;
  RayQuery<RAY_FLAG_FORCE_OPAQUE> query;
  query.TraceRayInline(gCarRtTlas, RAY_FLAG_NONE, mask, ray);
  while (query.Proceed()) {
  }
  CarRtHit h = (CarRtHit)0;
  if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
    h.hit = true;
    h.t = query.CommittedRayT();
    h.instance_index = query.CommittedInstanceIndex();
    h.primitive_index = query.CommittedPrimitiveIndex();
    h.barycentrics = query.CommittedTriangleBarycentrics();
    h.object_to_world = query.CommittedObjectToWorld3x4();
  }
  return h;
}

#endif  // CAR_RT_COMMON_HLSLI
