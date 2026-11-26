// MCLA GPU renderer -- the side mirrors' glass overlay: the arithmetic of both stages (mirror_overlay.h, mirror_overlay_rules.h, perf/MIRROR-VIEW.md package MV4).
//
// Written in the subset of HLSL that C++ reads too, so that mirror_overlay_rules_test.cpp can compile THIS TEXT as C++ (hlsl_cpp_shim.h) and hold it against the double precision reference
// (mirror_view_rules.h UvForMirrorPixel): what the test runs is what the GPU runs. The stages (mirror_overlay_vs.hlsl, mirror_overlay_ps.hlsl) only declare the push block, the input and the
// output and call these.
//
// The glass's triangles are drawn again from the host's own copy of their vertices. Every number the two stages need arrives in ONE 128 byte push block (the most every Vulkan driver takes), eight
// float4s, built by mirror_overlay_rules.h MakePush:
//   col_x col_y col_z  the model space -> the main camera's view space, eye-relative (x right, y up, z forward = the distance along the view): pv.k = dot(float4(position, 1), col_k)
//   proj_a             x' = pv.x * a.x + pv.z * a.y ; y' = pv.y * a.z + pv.z * a.w   (the game's clip space with the draw's viewport transform and the frame's jitter folded in)
//   proj_b             z' = pv.z * b.x + b.y ; w' = pv.z ; b.z, b.w = tan of half the rig's horizontal and vertical field
//   normal             xyz the glass's unit normal in the view space, w the assumed distance t0 (metres) beyond the glass
//   delta              xyz the main camera's eye minus the rig's, in the main camera's axes (0 for a picture drawn this frame), w the bits of the picture's bindless slot
//   rot                the quaternion (x, y, z, w) turning a direction of the main camera's axes into the rig's
// The rig is the main camera turned half a circle about its up axis, so for a picture drawn this frame rot = (0, 1, 0, 0), which maps (x, y, z) to (-x, y, -z).

#ifndef MCLA_GPU_MIRROR_OVERLAY_HLSLI
#define MCLA_GPU_MIRROR_OVERLAY_HLSLI

// The two numbers of the overlay that are not in the block (mirror_overlay_rules.h holds the same, the test checks the text).
static const float kMirrorOverlayEdgeFade = 0.04f;  // the last share of the picture, on each side, that fades to the game's own glass
static const float kMirrorOverlayBias = 0.005f;     // metres towards the eye along the view ray: the overlay lands on the game's glass, not behind it

struct MirrorOverlayPush {
  float4 col_x;
  float4 col_y;
  float4 col_z;
  float4 proj_a;
  float4 proj_b;
  float4 normal;
  float4 delta;
  float4 rot;
};

// Per-draw constants. Vulkan passes the address of the upload as an 8-byte
// push constant. D3D12 binds the same bytes as a root CBV.
struct MirrorOverlayDraw {
  MirrorOverlayPush mirror;
  float4 mask_u;
  float4 mask_v;
  float4 mask_meta;
};

// The vertex in the main camera's view space, eye-relative.
float3 MirrorOverlayViewPos(MirrorOverlayPush p, float3 position) {
  return float3(position.x * p.col_x.x + position.y * p.col_x.y + position.z * p.col_x.z + p.col_x.w,
                position.x * p.col_y.x + position.y * p.col_y.y + position.z * p.col_y.z + p.col_y.w,
                position.x * p.col_z.x + position.y * p.col_z.y + position.z * p.col_z.z + p.col_z.w);
}

// The position the rasteriser gets: the point pulled kMirrorOverlayBias towards the eye along its own ray (the same pixel, a nearer depth), then the game's clip space with the draw's viewport
// transform. w' is the distance along the view.
float4 MirrorOverlayClip(MirrorOverlayPush p, float3 pv) {
  float k = 1.0f;
  if (pv.z > kMirrorOverlayBias) {
    k = (pv.z - kMirrorOverlayBias) / pv.z;
  }
  const float x = pv.x * k;
  const float y = pv.y * k;
  const float z = pv.z * k;
  return float4(x * p.proj_a.x + z * p.proj_a.y, y * p.proj_a.z + z * p.proj_a.w, z * p.proj_b.x + p.proj_b.y, z);
}

// A vector turned by the unit quaternion q: v + 2 cross(q.xyz, cross(q.xyz, v) + q.w v).
float3 MirrorOverlayRotate(float4 q, float3 v) {
  const float3 qv = float3(q.x, q.y, q.z);
  const float3 t = cross(qv, v) + v * q.w;
  return v + cross(qv, t) * 2.0f;
}

// The picture's coordinates for the point `pv` of the glass (u 0 at the left edge to 1 at the right, v 0 at the top), and in z whether the lookup points into the picture's half space (1) or behind the
// rig (0). The eye's ray to the glass is reflected about the glass's normal; the rig looks where that ray goes at the assumed distance beyond the glass, from where it stood.
float3 MirrorOverlayUv(MirrorOverlayPush p, float3 pv) {
  const float3 n = float3(p.normal.x, p.normal.y, p.normal.z);
  const float3 d = normalize(pv);
  const float3 r = reflect(d, n);
  const float3 dir = pv + float3(p.delta.x, p.delta.y, p.delta.z) + r * p.normal.w;
  const float3 q = MirrorOverlayRotate(p.rot, dir);
  if (q.z <= 0.0001f) {
    return float3(0.0f, 0.0f, 0.0f);
  }
  const float inv = 1.0f / q.z;
  return float3(0.5f + 0.5f * q.x * inv / p.proj_b.z, 0.5f - 0.5f * q.y * inv / p.proj_b.w, 1.0f);
}

// 0 on and outside the picture's edge, rising to 1 at kMirrorOverlayEdgeFade of the picture inside it: the glass goes back to the game's own where the picture runs out.
float MirrorOverlayEdgeFade(float2 uv) {
  const float e = min(min(uv.x, 1.0f - uv.x), min(uv.y, 1.0f - uv.y));
  return saturate(e / kMirrorOverlayEdgeFade);
}

#endif  // MCLA_GPU_MIRROR_OVERLAY_HLSLI
