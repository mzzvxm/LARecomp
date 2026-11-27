// MCLA GPU renderer -- the G-buffer's debug view (gbuffer.h, mcla_gpu_gbuffer_view).
//
// Drawn into the main view's colour target at the mid-scene depth copy, in a pipeline that REPLACES the colour (no blend): a plane of the G-buffer shown as
// a picture. The planes are the sidecar images of gbuffer_types.h (RT1 the octahedral world normal, RT2 sqrt(albedo) and the lit weight, RT3 the smoothness and the
// class), each in a bindless slot; a plane the frame does not have is a slot that is not read (info.y has a bit for each plane that exists) and shows as no data.
//
//   view 1  the normal as a colour (0.5 + 0.5 n); black where RT1 holds no data
//   view 2  the albedo (RT2.rgb squared), black where there is none (RT2 all zero: no data; an emissive pixel has its albedo and a lit weight of 0)
//   view 3  the smoothness (RT3.r) as a grey
//   view 4  the class as a colour (a hue per class; a pixel that has a normal and no RT3 is dark green: written, class unknown); black: no data
//   view 5  coverage: white where RT1 holds a normal, black where it does not (the "missing pixels" picture of the survey's run plan R2)
//   view 6  the lit weight (RT2.a) as a grey
//   view 8  the metals: white where the screen consumers see a metal (the class chrome or metal, or the metal flag, RT3.g's bit 4: perf/PBR.md 1.4), black elsewhere (PBR-2)
//   view 9  the roughness (1 - RT3.r, the perceptual roughness) as a grey: white chalk, black mirror (PBR-2)
//   view 7  the missing pixels: GREEN where RT1 holds a normal, RED where the main view's depth holds geometry (stored depth above 0: the guest's depth is 1 near, 0 far) and RT1
//           holds no data (a shader with no variant yet, a mirror's window, a masked draw), black where there is neither (the sky). The survey's R2 picture: which families still have
//           no data.
//
// The two compilations: GB_VIEW_MS 0 reads 1 sample planes (Texture2D, space 0), 1 reads planes of the host's MSAA level (Texture2DMS, space 1) at the host sample
// that is the guest's sample 0. The frame's later passes (particles, the glows, the tone map) are drawn over this as usual: what shows is the game's picture of it
// after the tone map.

#include "host_pass_common.hlsli"
#include "gbuffer_common.hlsli"
#include "visual_class.hlsli"

#ifndef GB_VIEW_MS
#define GB_VIEW_MS 0
#endif

struct GBufferViewPush {
  uint4 slots;  // x RT1's bindless slot, y RT2's, z RT3's, w the view (gbuffer_types.h View)
  uint4 info;   // x the host sample to read (the guest's sample 0), y the planes that exist (PlaneBit: 1 normal, 2 albedo, 4 material), z the main depth plane's slot (view 7)
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<GBufferViewPush> g_push;
#else
ConstantBuffer<GBufferViewPush> g_push : register(b0, space4);
#endif

float4 LoadPlane(uint slot, int2 p) {
#if GB_VIEW_MS
  return g_textures_ms[slot].Load(p, g_push.info.x);
#else
  return g_textures[slot].Load(int3(p, 0));
#endif
}

// The main view's stored depth at `p` (the same samples and size as the planes; the guest's depth is 1 near, 0 far).
float LoadDepth(int2 p) {
#if GB_VIEW_MS
  return g_textures_ms[g_push.info.z].Load(p, g_push.info.x).r;
#else
  return g_textures[g_push.info.z].Load(int3(p, 0)).r;
#endif
}

// A hue per class (the golden ratio's steps keep neighbouring classes apart); class 0 is black.
float3 ClassColour(uint cls) {
  if (cls == 0u) {
    return float3(0.0f, 0.0f, 0.0f);
  }
  const float h = frac(float(cls) * 0.61803398875f) * 6.0f;
  const float3 rgb = saturate(float3(abs(h - 3.0f) - 1.0f, 2.0f - abs(h - 2.0f), 2.0f - abs(h - 4.0f)));
  return lerp(float3(1.0f, 1.0f, 1.0f), rgb, 0.8f);
}

float4 main(float4 position : SV_Position) : SV_Target {
  const int2 p = int2(position.xy);
  const uint view = g_push.slots.w;
  if (view == 0x100u) {  // capture class IDs into a single-sample R8_UNORM target
    const uint material = uint(LoadPlane(g_push.slots.z, p).y * 255.0f + 0.5f);
    const bool sky = g_push.info.z != 0xFFFFFFFFu &&
                     g_textures[g_push.info.z].Load(int3(p, 0)).x == 0.0f;
    const int gap = int(g_push.info.y);
    const bool ui = g_push.info.w != 0xFFFFFFFFu &&
                    g_textures[g_push.info.w].Load(int3(max(p, int2(gap, gap)), 0)).a > 0.0f;
    return float4(float(VisualClass(material, sky, ui)) / 255.0f, 0.0f, 0.0f, 1.0f);
  }
  const bool have_normal = (g_push.info.y & 1u) != 0u;
  const bool have_albedo = (g_push.info.y & 2u) != 0u;
  const bool have_material = (g_push.info.y & 4u) != 0u;
  const float2 rt1 = have_normal ? LoadPlane(g_push.slots.x, p).xy : float2(0.0f, 0.0f);
  const bool valid = GbRt1Valid(rt1);
  float3 colour = float3(0.0f, 0.0f, 0.0f);
  if (view == 1u) {
    if (valid) {
      colour = GbRt1Decode(rt1) * 0.5f + 0.5f;
    }
  } else if (view == 5u) {
    colour = valid ? float3(1.0f, 1.0f, 1.0f) : float3(0.0f, 0.0f, 0.0f);
  } else if (view == 7u) {
    if (valid) {
      colour = float3(0.0f, 0.8f, 0.2f);
    } else if (LoadDepth(p) > 0.0f) {
      colour = float3(1.0f, 0.0f, 0.0f);
    }
  } else if (view == 2u) {
    if (have_albedo) {
      const float4 rt2 = LoadPlane(g_push.slots.y, p);
      // RT2 = (0, 0, 0, 0) is "no albedo"; an emissive pixel has an albedo and a lit weight of 0 (GB-W1): it shows here too
      if (any(rt2 > 0.0f)) {
        colour = GbAlbedoDecode(rt2.rgb);
      }
    }
  } else if (view == 6u) {
    if (have_albedo) {
      colour = LoadPlane(g_push.slots.y, p).aaa;
    }
  } else if (view == 3u || view == 4u || view == 8u || view == 9u) {
    if (have_material) {
      const float3 rt3 = LoadPlane(g_push.slots.z, p).xyz;
      if (view == 3u) {
        colour = GbRt3Smoothness(rt3.xy).xxx;
      } else if (view == 8u) {
        const uint cls = GbRt3Class(rt3.xy);
        const bool flag = GbRt3MetalFlag(rt3);
        colour = (flag || cls == uint(GB_CLASS_CHROME) || cls == uint(GB_CLASS_METAL)) ? float3(1.0f, 1.0f, 1.0f) : float3(0.0f, 0.0f, 0.0f);
      } else if (view == 9u) {
        colour = (1.0f - GbRt3Smoothness(rt3.xy)).xxx;
      } else {
        colour = ClassColour(GbRt3Class(rt3.xy));
      }
    } else if (view == 4u && valid) {
      colour = float3(0.0f, 0.3f, 0.0f);
    }
  }
  return float4(colour, 1.0f);
}
