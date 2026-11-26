// MCLA GPU renderer -- the occlusion's normal image (ao.h, GB-E): the unit view-space normal of every pixel of the traced image, once a frame.
//
// The ambient occlusion, the bounce light and the contact shadows took each pixel's normal from the depth's neighbours in every one of their passes (ao_common.hlsli
// AoNormalFromImage: four loads, four view positions, a cross product, a square root, ten times a frame). The G-buffer (gbuffer_types.h) holds the surface's own WORLD normal in RT1
// wherever the shader of the draw has a G-buffer variant, and 0 ("no data") everywhere else. This pass, run once after the depth chain in the frames that have the plane, makes
// the image the passes load from, per pixel:
//
//   a pixel whose RT1 texel holds a normal that faces the camera (ao_gbuffer_normal.hlsli AoGbViewNormal: turned into view space with the camera's rotation, a normal that faces away
//   by more than a margin is stale or wrong) takes it: the surface's own normal, no reconstruction, no bending across a depth edge;
//   every other pixel takes the depth's normal, AoNormalFromImage, exactly as the passes had it.
//
// R16G16_UNORM, the octahedral code of the unit normal (hlsl/gbuffer_common.hlsli GbOctEncode: 0.003 degrees); the passes decode it with GbOctDecode (ao_common.hlsli AoNormalAt). Every
// pixel has a normal: the sky and what the depth cannot place get (0, 0, -1) and no pass reads it (they stop at the sky first).
//
// AT HALF SIZE (AoLow; the occlusion above 1080 lines, ao_size.h) a pixel stands for a 2x2 block of the depth plane and the depth pass took the depth of ONE of its four samples (or
// the mean of a flat block); it left the number of that sample in the two bits above the vehicle mark of the depth (ao_depth_ps.hlsl, ao_gbuffer_normal.hlsli AoGbSourceOf), and this
// pass reads RT1 at that sample: across a depth edge the block's first texel could be the other surface. At full size the pixel is its own texel. RT1 has the depth plane's size
// (the picture's), the traced image's size is this pass's target (sizes.xy).
//
// The two compilations: AO_NORMAL_MS 0 reads a plane with one sample (Texture2D, space 0), 1 reads the planes of the host's MSAA level (Texture2DMS, space 1) at the host sample that is
// the guest's sample 0, the one the depth is read at (the G-buffer's sample0).
//
// Push block (ao_common.hlsli AoPush), this pass's fields:
//   slots.x    RT1's bindless slot (GBufferFrame::slot[kPlaneNormal])
//   slots.y    the host sample to read (GBufferFrame::sample0, the guest's sample 0)
//   slots.z    the linear depth image the passes read (level 0, with the vehicle mark and the source bits), at this pass's size
//   sizes.xy   this pass's target size; sizes.zw RT1's size
//   lens.xy    tan of the half field of view (AoViewPos)
//   lin.xyz    the camera's right axis in world space, shape.xyz its up, light.xyz its forward (ao_gbuffer.h RowsFromViewInverse: the world direction (dx, dy, dz) is the view
//              direction (dot(d, right), dot(d, up), dot(d, forward)))
//   flags      bit 16 AoLow (the image is half the plane's size and the depth's source bits say which texel)

#include "ao_common.hlsli"
#include "ao_gbuffer_normal.hlsli"
#include "ssgi_quarter.hlsli"

#ifndef AO_NORMAL_MS
#define AO_NORMAL_MS 0
#endif

float2 LoadRt1(int2 p) {
#if AO_NORMAL_MS
  return g_textures_ms[g_push.slots.x].Load(p, g_push.slots.y).xy;
#else
  return g_textures[g_push.slots.x].Load(int3(p, 0)).xy;
#endif
}

float4 main(float4 position : SV_Position) : SV_Target {
  const int2 pixel = int2(position.xy);
  const float2 size = g_push.sizes.xy;
  const int2 isize = int2(size);
  const uint depth_slot = g_push.slots.z;
  const float tc = AoLoadLinear(depth_slot, pixel);
  // The sky and what the linearisation could not place: nothing reads a normal there.
  if (tc >= kAoSky * 0.5f) {
    return float4(GbOctEncode(float3(0.0f, 0.0f, -1.0f)), 0.0f, 0.0f);
  }
  const float3 pc = AoViewPos(float2(pixel) + 0.5f, size, tc);
  // The texel of RT1 that belongs to this pixel: its own, or at half size the block's sample the depth is of.
  int2 source = pixel;
  if ((g_push.slots2.z & kSsgiQuarterFlag) != 0u) {
    source = SsgiQuarterSource(pixel, AoGbSourceOf(tc), int2(g_push.extra.xy) - 1);
  } else if (AoLow()) {
    const uint index = AoGbSourceOf(tc);
    source = pixel * 2 + int2(int(index & 1u), int(index >> 1u));
  }
  source = min(source, int2(g_push.sizes.zw) - 1);
  const float4 picked = AoGbViewNormal(LoadRt1(source), pc, g_push.lin.xyz, g_push.shape.xyz, g_push.light.xyz);
  float3 normal;
  if (picked.w > 0.5f) {
    normal = picked.xyz;
  } else {
    normal = AoNormalFromImage(depth_slot, pixel, isize, tc, pc);
  }
  return float4(GbOctEncode(normal), 0.0f, 0.0f);
}
