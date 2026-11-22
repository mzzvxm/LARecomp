// MCLA GPU renderer -- reading a light's ray traced visibility in the deferred light pass (rt_lights_rules.h GpuRtMaskTable; hlsl/local_light_ps.hlsl; RT-LIGHTS-D1).
//
// A light's visibility is one channel (rgb of an RGBA8 image: channel = image x 3 + component) of the filtered images the ray pass wrote at the MASK's size: the occlusion's TRACED size over the
// table's `step` (1, or 2 where the traced size is taller than 720 lines: a mask texel is the traced pixel at the corner of a 2 x 2 block), and the traced size is the picture's size or less (half of
// it above 1080 lines of render height, or at the AO's lower quality). This reads it at a pixel of the PICTURE:
//   the same size       the texel itself (a whole mask the size of the picture);
//   any other           a DEPTH-AWARE BILINEAR: the four mask texels round the pixel's place weighed by the bilinear weights and by how near each texel's depth is to the pixel's own (the occlusion's
//                       linear depth image at the traced pixel the texel stands for, the vehicle bit in its lowest mantissa bit cleared: a texel on the other side of a silhouette is another surface and
//                       takes no part), and the nearest texel where no tap counts. The weight of a tap is 1 / (1 + (dz / (sigma t))^2), sigma = the table's depth_sigma (0.05: five per cent of the pixel's distance).
//                       The texel i stands for the traced pixel i x step, whose centre is at i x step + 0.5 traced pixels: the pixel's place is ((p + 0.5) x traced / picture - 0.5) / step texels.
// A value that is not a number, or outside [0, 1], is cleaned (rt_lights_rules.h CleanVisibility): NaN and infinity are 1 (lit: they take nothing away), the rest is clamped.
//
// THE SEAMS. The file DECLARES these two and the including shader DEFINES them, anywhere after the include (the pass reads the images through the heap's bindless set 0; the test through arrays):
//     float4 RtmLoadMask(uint slot, int2 p);    one texel of a mask image at the traced size (the index is in range)
//     float  RtmLoadDepth(uint slot, int2 p);   the raw value of the occlusion's linear depth image at the traced size (the vehicle bit still in it)
// src/gpu/rt_lights_test.cpp compiles THIS FILE as C++ through hlsl_shim.h, so the text keeps to the subset both languages read (scalars, float2 / float4, no swizzles of more than one component,
// no out parameters, min / max / floor / lerp / saturate / abs only). Every name starts with Rtm or RTM_.

#ifndef MCLA_RT_MASK_READ_HLSLI
#define MCLA_RT_MASK_READ_HLSLI

#define RTM_NO_SLOT 0xFFFFFFFFu
#define RTM_VALID 1u        // rt_lights_rules.h kMaskFlagValid
#define RTM_VIEW 4u         // kMaskFlagView: the light pass draws the visibility as a picture
#define RTM_RAW_VIEW 2u     // kMaskFlagRawView

// The table as the pass reads it from its buffer (rt_lights_rules.h GpuRtMaskTable, 64 bytes).
struct RtmTable {
  uint images;
  uint flags;
  uint width;
  uint height;
  uint slot0;
  uint slot1;
  uint slot2;
  uint lin_slot;
  uint raw0;
  uint raw1;
  uint raw2;
  uint step;
  float depth_sigma;
  uint traced_w;
  uint traced_h;
};

// The seams (the including shader defines them).
float4 RtmLoadMask(uint slot, int2 p);                 // one texel of a mask image at the mask's size (the index is in range)
float RtmLoadDepth(uint slot, int2 p);                 // the raw value of the occlusion's linear depth image at the TRACED size

bool RtmValid(RtmTable t) {
  return (t.flags & RTM_VALID) != 0u && t.images != 0u && t.width != 0u && t.height != 0u;
}

// The bindless slot of a channel's image (the filtered one, or the raw one for the debug view), RTM_NO_SLOT for a channel with no image.
uint RtmSlotOf(RtmTable t, uint channel, bool raw) {
  uint group = channel / 3u;
  if (group >= t.images) {
    return RTM_NO_SLOT;
  }
  if (raw) {
    return group == 0u ? t.raw0 : (group == 1u ? t.raw1 : t.raw2);
  }
  return group == 0u ? t.slot0 : (group == 1u ? t.slot1 : t.slot2);
}

float RtmComponent(float4 v, uint component) {
  return component == 0u ? v.x : (component == 1u ? v.y : v.z);
}

// A visibility as the pass uses it: a number in [0, 1]; NaN and infinities are 1.
float RtmClean(float v) {
  if (!(v > -1.0e30f && v < 1.0e30f)) {
    return 1.0f;
  }
  return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

// The atlas's visibility times the ray's (each 1 where the light has none): the twin of rt_lights_rules.h Combine.
float RtmCombine(float atlas, float rt) {
  return RtmClean(atlas) * RtmClean(rt);
}

// The depth a traced texel holds, without the vehicle bit (asuint(raw) & ~1u).
float RtmDepthOf(uint lin_slot, int2 q) {
  return asfloat(asuint(RtmLoadDepth(lin_slot, q)) & ~1u);
}

// The weight of a traced texel whose depth is `z` for a pixel at the view distance t: 1 at equal depths, falling as 1 / (1 + (dz / (sigma t))^2); 0.001 for a texel with no depth (the sky: it is not
// a surface to take a visibility from, but where it is all there is the nearest texel is used).
float RtmDepthWeight(float z, float t, float sigma) {
  if (!(z > 0.0f)) {
    return 0.001f;
  }
  float d = (z - t) / max(sigma * t, 1.0e-4f);
  return 1.0f / (1.0f + d * d);
}

// The traced size of a table (a table of part 1 had none: the mask was the traced size), and its step.
uint RtmStep(RtmTable tb) {
  return tb.step < 2u ? 1u : 2u;
}
float RtmTracedW(RtmTable tb) {
  return tb.traced_w != 0u ? float(tb.traced_w) : float(tb.width * RtmStep(tb));
}
float RtmTracedH(RtmTable tb) {
  return tb.traced_h != 0u ? float(tb.traced_h) : float(tb.height * RtmStep(tb));
}

// The visibility of `channel` at the picture's pixel p (the picture is renderW x renderH) whose view distance is t, 1 where the table, the channel or the image is not there. `raw` reads the unfiltered image.
float RtmVisibility(RtmTable tb, uint channel, int2 p, float renderW, float renderH, float t, bool raw) {
  if (!RtmValid(tb) || channel >= tb.images * 3u) {
    return 1.0f;
  }
  uint slot = RtmSlotOf(tb, channel, raw);
  if (slot == RTM_NO_SLOT) {
    return 1.0f;
  }
  uint component = channel % 3u;
  uint step = RtmStep(tb);
  float w = float(tb.width);
  float h = float(tb.height);
  if (step == 1u && w == renderW && h == renderH) {
    return RtmClean(RtmComponent(RtmLoadMask(slot, p), component));
  }
  // the pixel's place in mask texels: the texel i stands for the traced pixel i x step, whose centre is at i x step + 0.5 traced pixels
  float tw = RtmTracedW(tb);
  float th = RtmTracedH(tb);
  float fstep = float(step);
  float ux = ((float(p.x) + 0.5f) * tw / renderW - 0.5f) / fstep;
  float uy = ((float(p.y) + 0.5f) * th / renderH - 0.5f) / fstep;
  float fx0 = floor(ux);
  float fy0 = floor(uy);
  float fx = ux - fx0;
  float fy = uy - fy0;
  int x0 = int(fx0);
  int y0 = int(fy0);
  int xmax = int(tb.width) - 1;
  int ymax = int(tb.height) - 1;
  int txmax = int(tw) - 1;
  int tymax = int(th) - 1;
  bool guided = tb.lin_slot != RTM_NO_SLOT && t > 0.0f;
  float sum = 0.0f;
  float weights = 0.0f;
  float nearest = 1.0f;
  float nearest_weight = -1.0f;
  for (int k = 0; k < 4; ++k) {
    int dx = k & 1;
    int dy = k >> 1;
    int2 q = int2(min(max(x0 + dx, 0), xmax), min(max(y0 + dy, 0), ymax));
    float bx = dx == 0 ? 1.0f - fx : fx;
    float by = dy == 0 ? 1.0f - fy : fy;
    float bilinear = bx * by;
    float v = RtmClean(RtmComponent(RtmLoadMask(slot, q), component));
    // the guide's depth is the traced pixel the texel stands for (the corner of its block, kept on the image)
    int2 qt = int2(min(q.x * int(step), txmax), min(q.y * int(step), tymax));
    float depth_weight = guided ? RtmDepthWeight(RtmDepthOf(tb.lin_slot, qt), t, tb.depth_sigma) : 1.0f;
    float wk = bilinear * depth_weight;
    sum += v * wk;
    weights += wk;
    if (wk > nearest_weight) {
      nearest_weight = wk;
      nearest = v;
    }
  }
  if (weights > 1.0e-4f) {
    return sum / weights;
  }
  return nearest;
}

#endif  // MCLA_RT_MASK_READ_HLSLI
