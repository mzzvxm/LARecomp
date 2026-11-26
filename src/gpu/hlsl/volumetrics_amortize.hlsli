#ifndef MCLA_GPU_VOLUMETRICS_AMORTIZE_HLSLI
#define MCLA_GPU_VOLUMETRICS_AMORTIZE_HLSLI

static const uint kLampTileSide = 8u;
static const uint kLampTileBytes = 16u;
static const uint kFusedSlices = 64u;
static const uint kTileHeadlight = 1u;
static const uint kTileHeadlightShadow = 2u;

#ifdef __HLSL_VERSION
uint3 LampTileGrid(uint3 grid) { return (grid + kLampTileSide - 1u) / kLampTileSide; }

uint LampTileOffset(uint3 tile, uint3 grid) {
  const uint3 size = LampTileGrid(grid);
  return (tile.x + size.x * (tile.y + size.y * tile.z)) * kLampTileBytes;
}

RWByteAddressBuffer g_lamp_tiles : register(u2, space3);
#endif

uint CheckerSlice(uint lane, uint2 column, uint frame) {
  return 2u * (lane & 31u) + ((lane >> 5u) ^ ((column.x + column.y + frame) & 1u));
}

float VolHistoryWeight(float weight, bool retained_slice, bool full_rate) {
  return full_rate ? weight : (retained_slice ? 1.0f : weight * weight);
}

bool VolCullingBasisValid(float3 right, float3 up, float3 forward) {
  const float tolerance = 1e-5f;
  return abs(dot(right, right) - 1.0f) <= tolerance && abs(dot(up, up) - 1.0f) <= tolerance &&
         abs(dot(forward, forward) - 1.0f) <= tolerance && abs(dot(right, up)) <= tolerance &&
         abs(dot(up, forward)) <= tolerance && abs(dot(forward, right)) <= tolerance;
}

// The four side planes have unnormalized normals of length sqrt(1 + slope^2).
bool LampSphereOutsideSlab(float3 pos, float range, float4 slope, float2 depth) {
  return pos.z < depth.x - range || pos.z > depth.y + range ||
         slope.x * pos.z - pos.x > range * sqrt(1.0f + slope.x * slope.x) ||
         pos.x - slope.y * pos.z > range * sqrt(1.0f + slope.y * slope.y) ||
         slope.z * pos.z - pos.y > range * sqrt(1.0f + slope.z * slope.z) ||
         pos.y - slope.w * pos.z > range * sqrt(1.0f + slope.w * slope.w);
}

// A plane tangent to a spotlight cone can separate the slab's bounding
// sphere from the whole cone. Wide cones conservatively pass this test.
bool LampSphereOutsideCone(float3 d, float3 axis, float cos_half_angle, float radius) {
  if (cos_half_angle <= 0.0f || cos_half_angle > 1.0f) return false;
  const float axis_distance = dot(d, axis);
  const float radial_distance = length(d - axis_distance * axis);
  const float sin_half_angle = sqrt(1.0f - cos_half_angle * cos_half_angle);
  return radial_distance * cos_half_angle - axis_distance * sin_half_angle > radius;
}

// Each segment carries its scattered light and the fraction of light it
// transmits. Composition is associative and ordered from near to far.
float4 ComposeVolume(float4 near_segment, float4 far_segment) {
  return float4(near_segment.rgb + near_segment.w * far_segment.rgb, near_segment.w * far_segment.w);
}

float4 VolumeSegment(float4 sv, float metres) {
  const float sigma = max(sv.w, 1e-6f);
  const float t = exp(-sigma * metres);
  return float4((sv.rgb - sv.rgb * t) * (1.0f / sigma), t);
}

#endif
