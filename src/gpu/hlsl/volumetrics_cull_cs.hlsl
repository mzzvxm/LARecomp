// One group per 8x8x8 froxel tile. The mask retains the original lamp order.
#include "volumetrics_common.hlsli"
#include "volumetrics_amortize.hlsli"

groupshared float4 tile_slopes;  // left, right, bottom, top
groupshared float2 tile_depth;
groupshared float3 tile_center;  // in view space
groupshared float tile_radius;
groupshared float history_pad;
groupshared uint cull_safe;
groupshared uint lamp_mask[2];
groupshared uint headlight_flags;
groupshared uint changed_lamp;

float3 ViewPosition(float3 p, float3 camera, float3 right, float3 up, float3 forward) {
  const float3 d = p - camera;
  return float3(dot(d, right), dot(d, up), dot(d, forward));
}

bool TileIntersectsLight(uint offset, float padding, float3 camera, float3 right, float3 up, float3 forward) {
  // The slab measures distances in view space. If the camera constants
  // contain scale or shear, keep every lamp rather than shrinking a bound.
  if (cull_safe == 0u) return true;
  const float4 a = VolF4(offset);
  const float3 pos = ViewPosition(a.xyz, camera, right, up, forward);
  // A small outward bias covers floating-point error at shared tile edges.
  const float epsilon = 1e-3 + 6e-5 * max(length(pos), tile_depth.y);
  const float range = a.w + padding + epsilon;
  if (LampSphereOutsideSlab(pos, range, tile_slopes, tile_depth)) return false;

  const float4 b = VolF4(offset + 16u);
  const float3 axis = float3(dot(b.xyz, right), dot(b.xyz, up), dot(b.xyz, forward));
  return !LampSphereOutsideCone(tile_center - pos, axis, b.w, tile_radius + padding + epsilon);
}

uint HeadlightTileFlags(uint offset, bool shadows) {
  const uint4 lamp_views = VolU4(offset + 48u);
  const bool shadowed = shadows && (int(lamp_views.x) >= 0 || int(lamp_views.z) >= 0);
  return kTileHeadlight | (shadowed ? kTileHeadlightShadow : 0u);
}

[numthreads(64, 1, 1)]
void main(uint3 tile : SV_GroupID, uint lane : SV_GroupIndex) {
  const uint3 grid = VolU4(kO_grid).xyz;
  const float4 camera = VolF4(kO_cam_pos);
  const float4 right = VolF4(kO_cam_pos + 16u);
  const float3 up = VolF4(kO_cam_pos + 32u).xyz;
  const float3 forward = VolF4(kO_cam_pos + 48u).xyz;
  const bool history = (g_push.slots.z & 1u) != 0u;
  if (lane == 0u) {
    const uint3 lo = tile * kLampTileSide;
    const uint3 hi = min(lo + kLampTileSide, grid);
    const float3 a = float3(lo) / float3(grid);
    const float3 b = float3(hi) / float3(grid);
    const float4 depth = VolF4(kO_depth);
    tile_slopes = float4((2.0 * a.x - 1.0) * camera.w, (2.0 * b.x - 1.0) * camera.w,
                         (1.0 - 2.0 * b.y) * right.w, (1.0 - 2.0 * a.y) * right.w);
    tile_depth = depth.x * exp(depth.z * float2(a.z, b.z));
    const float4 boundary_near = tile_slopes * tile_depth.x;
    const float4 boundary_far = tile_slopes * tile_depth.y;
    const float3 min_corner = float3(min(boundary_near.xz, boundary_far.xz), tile_depth.x);
    const float3 max_corner = float3(max(boundary_near.yw, boundary_far.yw), tile_depth.y);
    tile_center = (min_corner + max_corner) * 0.5;
    tile_radius = length(max_corner - min_corner) * 0.5;
    history_pad = 0.0;
    bool valid_basis = VolCullingBasisValid(right.xyz, up, forward);
    if (history) {
      const float4 prev_pos = VolF4(kO_prev_pos);
      const float4 prev_right = VolF4(kO_prev_pos + 16u);
      const float3 prev_up = VolF4(kO_prev_pos + 32u).xyz;
      const float4 prev = VolF4(kO_history);
      const float3 prev_forward = VolF4(kO_prev_pos + 48u).xyz;
      valid_basis = valid_basis && VolCullingBasisValid(prev_right.xyz, prev_up, prev_forward);
      const float3 center_world = camera.xyz + right.xyz * tile_center.x + up * tile_center.y + forward * tile_center.z;
      const float max_z = clamp(dot(center_world - prev_pos.xyz, prev_forward) + tile_radius, 0.0, prev.x);
      const float2 tan_half = float2(prev_pos.w, prev_right.w);
      // Pad previous light bounds for the trilinear footprint and the
      // previous samples' sub-voxel jitter, including on a fast camera pan.
      const float sample_z = min(max_z * exp(2.0 * prev.y / float(grid.z)), prev.x);
      history_pad = (sample_z - max_z) * length(float3(tan_half, 1.0)) +
                    sample_z * length(4.0 * tan_half / float2(grid.xy));
    }
    cull_safe = valid_basis ? 1u : 0u;
    lamp_mask[0] = 0u;
    lamp_mask[1] = 0u;
    headlight_flags = 0u;
    changed_lamp = 0u;
  }
  GroupMemoryBarrierWithGroupSync();

  const uint4 counts = VolU4(kO_counts);
  const uint4 changes = VolU4(kO_lamp_changes);
  if (lane < counts.x && TileIntersectsLight(kO_lamps + lane * 64u, 0.0, camera.xyz, right.xyz, up, forward)) {
    const uint bit = 1u << (lane & 31u);
    const uint word = lane >> 5u;
    InterlockedOr(lamp_mask[word], bit);
    if ((changes[word] & bit) != 0u) InterlockedOr(changed_lamp, 1u);
  }
  if (lane < changes.z && TileIntersectsLight(kO_retired_lamps + lane * 64u, history_pad, camera.xyz, right.xyz, up, forward)) {
    InterlockedOr(changed_lamp, 1u);
  }
  const uint head = kO_headlights + lane * 64u;
  const uint previous_head = kO_prev_headlights + lane * 64u;
  if (lane < counts.y && TileIntersectsLight(head, 0.0, camera.xyz, right.xyz, up, forward)) {
    InterlockedOr(headlight_flags, HeadlightTileFlags(head, (g_push.slots.z & 2u) != 0u));
  }
  if (lane < VolU4(kO_prev_counts).x && TileIntersectsLight(previous_head, history_pad, camera.xyz, right.xyz, up, forward)) {
    InterlockedOr(headlight_flags, HeadlightTileFlags(previous_head, true));
  }
  GroupMemoryBarrierWithGroupSync();
  if (lane == 0u) {
    g_lamp_tiles.Store4(LampTileOffset(tile, grid), uint4(lamp_mask[0], lamp_mask[1], headlight_flags, changed_lamp));
  }
}
