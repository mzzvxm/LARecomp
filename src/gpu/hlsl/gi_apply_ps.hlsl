// MCLA GPU renderer -- the probe field's apply (gi_apply.h, CITY-RT M3 package G3; the rules and the reasons are gi_apply_rules.h's comment, the maths gi_apply_common.hlsli).
//
// Two passes, one source:
//   GIA_PASS 0  the LOOKUP, at the occlusion's traced size (half the picture's above 1080 lines), into an RGBA16F image: per texel the world position of the picture's pixel it stands for
//               (2x, 2y: the normal and the depth are read there, so they agree) and the field's lookup there: rgb = E_net x the lookup's confidence, a = the linear depth the texel was
//               looked up at (the upsample's depth test). A texel with no normal or no geometry holds 0.
//   GIA_PASS 1  the APPLY, at the picture's size, ADDED to the main colour (blend ONE, ONE; the alpha is not written): albedo (RT2) x the field's net irradiance x the strength x the field's
//               share x the lit weight x the occlusion's term, softly clipped to the room under the bounce light's ceiling. GIA_UP 1 takes the net irradiance from the lookup image (the four nearest
//               texels weighted by depth and normal agreement, the visibility and the vehicle mark by the same weights); GIA_UP 0 looks the field up itself at the pixel (the traced size is the
//               picture's). The pixel's own colour is never read.
//               THE CABIN (gi_cabin_rules.h, INTERIOR-CAM-FIX): on a surface of the player's car's inside (the G-buffer's RT3: a vehicle's part that is not paint, chrome or glass) that is inside the car's box,
//               in a frame whose camera is in a roofed car, the field's share is multiplied by the cabin's (the probes cannot see the car's roof: gi_cabin_common.hlsli). Two rows after the apply's 9 and
//               the material plane's slot in the push block's last word (bits 16-31); a frame with no rule has share 1 in the first row and reads nothing.
// A debug view (push.info.w bits 8-15, gi_apply_rules.h): the same pass in a pipeline whose blend is SRC_ALPHA / INV_SRC_ALPHA, drawing a colour with alpha 1 where there is something to show and
// alpha 0 where there is not (a pixel with no normal, the sky): 1 E_surf, 2 the confidence, 3 the sky visibility, 5 the baseline, 6 the leak test, 9 the bounce added (alone on black, 4x), 10 tuned E_net, 11 the
// occlusion's term. The views 1, 2, 3, 5 and 6 look the field up at the picture's size.
//
// MSAA (GIA_MS 1): the depth plane and the G-buffer's planes are Texture2DMS read at the guest's sample 0 (push.info.z); the pass runs once a pixel and the add goes to every sample of the pixel.
// The occlusion's images (linear depth, visibility) and the lookup image are single sample at the traced size.
//
// Layout (gi_apply.cpp): set 0 (space0) the bindless Texture2D<float4>[], set 1 (space1) the same set as Texture2DMS<float4>[], set 2 (space2) the samplers (0 = linear clamp), set 3 (space3) the
// buffer of rows (t0: the field's 20 rows, the apply's 9, then the cabin's 2), push constants (b0, space4), 32 bytes.

#include "gbuffer_common.hlsli"

#ifndef GIA_PASS
#define GIA_PASS 1
#endif
#ifndef GIA_MS
#define GIA_MS 0
#endif
#ifndef GIA_UP
#define GIA_UP 0
#endif

Texture2D<float4> g_textures[] : register(t0, space0);
Texture2DMS<float4> g_textures_ms[] : register(t0, space1);
SamplerState g_samplers[] : register(s0, space2);
ByteAddressBuffer g_rows : register(t0, space3);

struct GiaPush
{
    uint4 slots;  // x the main depth plane's slot, y RT1's, z RT2's, w the occlusion's linear depth (level 0, traced size)
    uint4 info;   // x the visibility image's slot (GIA_NO_SLOT: none), y the lookup image's slot, z the host sample that is the guest's sample 0, w flags | view << 8
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<GiaPush> g_push;
#else
ConstantBuffer<GiaPush> g_push : register(b0, space4);
#endif

#define GI_FRAME_ROW(i) g_rows.Load4((i) * 16u)
#define GI_SAMPLE_IRRADIANCE(uv) g_textures[GiSlot(GI_PRODUCT_IRRADIANCE)].SampleLevel(g_samplers[0], (uv), 0.0f)
#define GI_SAMPLE_MOMENTS(uv) g_textures[GiSlot(GI_PRODUCT_MOMENTS)].SampleLevel(g_samplers[0], (uv), 0.0f).xy
#define GI_LOAD_PROBE_WEIGHT(px, py) g_textures[GiSlot(GI_PRODUCT_PROBE_WEIGHT)].Load(int3((int)(px), (int)(py), 0)).x
#include "gi_common.hlsli"
#include "gi_apply_common.hlsli"
#include "gi_pixel_common.hlsli"
#include "gi_cabin_common.hlsli"
#include "nan_guard.hlsli"

// G5: the per-pixel detail pass's term at the traced texel c (the lookup's own coordinates), for the pixel at the view distance t with the world normal n: rgb its denoised irradiance, w its weight against the
// field's (GipWeight: the history's length in the image's alpha, 0 where the pass did not trace the pixel). A frame without the image gives w = 0 and nothing else is read. A reduced image (divisor 2) is read by
// the four nearest of its pixels weighted by bilinear x depth x normal agreement (the guides are the pass's own images).
// G6: in a RAW frame (row.raw: Ray Reconstruction takes the frame, no filter ran) the image is the 1 spp signal itself, whose alpha is no length: the weight is GipRawWeight (the whole of it where the pass traced).
float4 GiaPixelTerm(int2 c, float t, float3 n, GiaFrame fr)
{
    if (!GiPixelPresent())
    {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }
    GiPixelRow row = GiLoadPixelRow();
    if (row.width == 0u || row.height == 0u || row.depthSlot == 0xFFFFu)
    {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }
    uint slot = GiSlot(GI_PRODUCT_PIXEL_IRRADIANCE);
    int2 size = int2(int(row.width), int(row.height));
    if (row.divisor <= 1u)
    {
        int2 q = clamp(c, int2(0, 0), size - 1);
        float4 e = g_textures[slot].Load(int3(q, 0));
        float d = g_textures[row.depthSlot].Load(int3(q, 0)).x;
        return float4(e.x, e.y, e.z, row.raw != 0u ? GipRawWeight(d) : GipWeight(e.w, d));
    }
    float2 f0 = (float2(c) + 0.5f) / (float)row.divisor - 0.5f;
    int2 q0 = int2(floor(f0));
    float2 f = f0 - float2(q0);
    float3 nv = float3(dot(n, fr.right), dot(n, fr.up), dot(n, fr.forward));
    float3 eSum = float3(0.0f, 0.0f, 0.0f);
    float lengthSum = 0.0f;
    float wSum = 0.0f;
    float cover = 0.0f;
    for (int j = 0; j < 2; ++j)
    {
        for (int i = 0; i < 2; ++i)
        {
            int2 q = clamp(q0 + int2(i, j), int2(0, 0), size - 1);
            float bilinear = (i == 0 ? 1.0f - f.x : f.x) * (j == 0 ? 1.0f - f.y : f.y);
            float d = g_textures[row.depthSlot].Load(int3(q, 0)).x;
            float4 e = g_textures[slot].Load(int3(q, 0));
            float3 gn = g_textures[row.normalSlot].Load(int3(q, 0)).xyz;
            if (dot(nv, gn) < 0.0f)
            {
                gn = -gn;
            }
            float w = GipTapWeight(bilinear, d, GiaDepthWeight(t, d), GiaNormalWeight(nv, gn));
            eSum = eSum + float3(e.x, e.y, e.z) * w;
            lengthSum = lengthSum + e.w * w;
            wSum = wSum + w;
            cover = cover + (d > 0.0f ? bilinear : 0.0f);
        }
    }
    if (!(wSum > 1.0e-8f))
    {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }
    float3 ePixel = eSum * (1.0f / wSum);
    return float4(ePixel.x, ePixel.y, ePixel.z, (row.raw != 0u ? GipRawWeight(1.0f) : GipWeight(lengthSum / wSum, 1.0f)) * saturate(cover));
}

// The lookup's net irradiance with the per-pixel term mixed into the field's irradiance first (GipMix; a weight of 0 is the field's exactly).
float3 GiaLookupNetMixed(GiSample s, float3 n, GiaFrame fr, float4 pixelTerm)
{
    s.irradiance = GipMix(s.irradiance, float3(pixelTerm.x, pixelTerm.y, pixelTerm.z), pixelTerm.w);
    return GiaLookupNet(s, n, fr);
}

float4 GiaLoadPlane(uint slot, int2 p)
{
#if GIA_MS
    return g_textures_ms[slot].Load(p, g_push.info.z);
#else
    return g_textures[slot].Load(int3(p, 0));
#endif
}

bool GiaQuarterLookup()
{
    return (g_push.info.w & GIA_FLAG_QUARTER_LOOKUP) != 0u;
}

bool GiaFastLookupApply()
{
    return (g_push.info.w & GIA_FLAG_FAST_LOOKUP_APPLY) != 0u;
}

uint GiaView()
{
    return (g_push.info.w >> 8u) & 0xFFu;
}

// The player's cabin (gi_cabin_rules.h, INTERIOR-CAM-FIX): the host's two rows after the apply's 9 (GIA_ROW(GIA_ROWS + i): the box, the cabin's share, the edge), and the material plane's slot in the
// push block's last word above the flags and the view. A frame with no rule has share 1 in row 0 and no slot is read.
float4 GiaCabinRow(uint i)
{
    return GiaRowFloats(GIA_ROWS + i);
}

uint GiaMaterialSlot()
{
    return g_push.info.w >> GIC_MATERIAL_SLOT_SHIFT;
}

bool GiaHasVisibility()
{
    return (g_push.info.w & GIA_FLAG_VISIBILITY) != 0u && g_push.info.x != GIA_NO_SLOT;
}

// The depth pass marks a vehicle's pixel in the lowest bit of the level 0 linear depth (screen_frame.h kDepthMarkMask).
float GiaVehicleMark(float depthRaw)
{
    return (asuint(depthRaw) & 1u) != 0u ? 1.0f : 0.0f;
}

float GiaDepthOf(float depthRaw)
{
    return asfloat(asuint(depthRaw) & 0xFFFFFFF8u);
}

float3 GiaReceiverPosition(int2 pixel, float t, GiaFrame fr)
{
    return GiaWorldPosition(fr, (float)pixel.x + 0.5f, (float)pixel.y + 0.5f, fr.render.x, fr.render.y, t);
}

#if GIA_PASS == 0

float4 main(float4 position : SV_Position) : SV_Target
{
    int2 p = int2(position.xy);
    GiaFrame fr = GiaLoadFrame();
    int2 last = int2(fr.render) - 1;
    int2 r = min(p * (GiaQuarterLookup() ? 4 : 2), last);
    float2 rt1 = GiaLoadPlane(g_push.slots.y, r).xy;
    if (!GbRt1Valid(rt1))
    {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }
#if GIA_MS
    float stored = g_textures_ms[g_push.slots.x].Load(r, g_push.info.z).r;
#else
    float stored = g_textures[g_push.slots.x].Load(int3(r, 0)).r;
#endif
    float t = GiaLinearDepth(stored, fr.lin);
    if (!(t < GIA_SKY))
    {
        return float4(0.0f, 0.0f, 0.0f, t);
    }
    float3 n = GbRt1Decode(rt1);
    float3 pos = GiaReceiverPosition(r, t, fr);
    GiSample s = GiLookup(pos, n, GiaFastLookupApply());
    float3 e = GiaLookupNetMixed(s, n, fr, GiaPixelTerm(p, t, n, fr));
    return float4(e.x, e.y, e.z, t);
}

#else

float3 GiaTone(float3 c)
{
    return c / (float3(1.0f, 1.0f, 1.0f) + c);
}

#if GIA_UP
float3 GiaQuarterIrradiance(int2 pixel, float t, float3 n, GiaFrame fr, bool fast)
{
    int2 last = int2(fr.render) - 1;
    if (all((pixel & 3) == 0) && all(pixel <= last))
    {
        // This receiver is the lookup's own sample, with the same guides.
        return g_textures[g_push.info.y].Load(int3(pixel / 4, 0)).xyz;
    }
    // Quarter texels represent the full-size pixel at c*4, whose centre is at
    // c*4+0.5. This avoids a shift when exactly on a sample and at odd extents.
    float2 q = float2(pixel) * 0.25f;
    int2 q0 = int2(floor(q));
    float2 f = q - float2(q0);
    int2 size = (int2(fr.render) + 3) / 4;
    float depthTolerance = GiaDepthTolerance(t);
    float3 radiance = 0.0f;
    float weight = 0.0f;
    [unroll] for (int j = 0; j < 2; ++j)
    {
        [unroll] for (int i = 0; i < 2; ++i)
        {
            float bilinear = (i == 0 ? 1.0f - f.x : f.x) * (j == 0 ? 1.0f - f.y : f.y);
            if (bilinear <= 0.0f)
            {
                continue;
            }
            int2 c = min(q0 + int2(i, j), size - 1);
            float4 look = g_textures[g_push.info.y].Load(int3(c, 0));
            if (look.w > 0.0f && look.w < GIA_SKY &&
                (!fast || GiaQuarterDepthMayContribute(abs(look.w - t), depthTolerance)))
            {
                float depth = GiaDepthWeight(t, look.w);
                // The normal agreement cannot raise this tap above the cutoff.
                if (!fast || depth > kGiaQuarterMinAgreement)
                {
                    float2 nc1 = GiaLoadPlane(g_push.slots.y, min(c * 4, last)).xy;
                    if (GbRt1Valid(nc1))
                    {
                        float bilateral = depth * GiaNormalWeight(n, GbRt1Decode(nc1));
                        float w = GiaQuarterTap(bilinear, bilateral);
                        radiance += look.xyz * w;
                        weight += w;
                    }
                }
            }
        }
    }
    // Blend into the direct answer at guide boundaries so the fallback does
    // not create a new edge on a surface that crosses the coarse grid.
    if (weight >= kGiaQuarterSupport)
    {
        return radiance / weight;
    }
    float3 exact = GiaLookupNet(GiLookup(GiaReceiverPosition(pixel, t, fr), n), n, fr);
    return lerp(exact, radiance / max(weight, 1.0e-8f), GiaQuarterBlend(weight));
}
#endif

float4 main(float4 position : SV_Position) : SV_Target
{
    int2 pixel = int2(position.xy);
    uint view = GiaView();
    GiaFrame fr = GiaLoadFrame();
    float4 none = float4(0.0f, 0.0f, 0.0f, 0.0f);

    // The surface: no normal, nothing to light (a family the G-buffer does not cover yet, the sky).
    float2 rt1 = GiaLoadPlane(g_push.slots.y, pixel).xy;
    if (!GbRt1Valid(rt1))
    {
        if (view != 0u)
        {
            return none;
        }
        discard;
    }
    float4 rt2 = GiaLoadPlane(g_push.slots.z, pixel);
    if (view == 0u && (!any(rt2 > 0.0f) || !(rt2.w > 0.0f)))
    {
        discard;  // no albedo (a family that writes none yet, a road) or an emissive surface: it takes no bounce
    }
#if GIA_MS
    float stored = g_textures_ms[g_push.slots.x].Load(pixel, g_push.info.z).r;
#else
    float stored = g_textures[g_push.slots.x].Load(int3(pixel, 0)).r;
#endif
    float t = GiaLinearDepth(stored, fr.lin);
    if (!(t < GIA_SKY))
    {
        if (view != 0u)
        {
            return none;
        }
        discard;
    }
    float3 n = GbRt1Decode(rt1);

    // The views that look at the field itself.
    if (view == 1u || view == 2u || view == 3u || view == 5u || view == 6u)
    {
        float3 pos = GiaReceiverPosition(pixel, t, fr);
        GiSample s = GiLookup(pos, n);
        float3 colour = float3(1.0f, 0.0f, 1.0f);
        if (view == 1u)
        {
            colour = GiaTone(s.irradiance);
        }
        else if (view == 2u)
        {
            colour = float3(0.9f * (1.0f - s.confidence) + 0.1f * s.confidence, 0.1f * (1.0f - s.confidence) + 0.9f * s.confidence, 0.1f + 0.1f * s.confidence);
        }
        else if (view == 3u)
        {
            float v = saturate(s.skyVisibility);
            colour = float3(0.02f + 0.98f * v, 0.05f + 0.95f * v, 0.35f + 0.65f * v);
        }
        else if (view == 5u)
        {
            colour = GiaTone(GiGroundBaseline(n));
        }
        else
        {
            float g = GiaGuardShare(pos, n);
            colour = float3(0.9f * g, 0.9f * (1.0f - g), 0.0f);
        }
        return float4(colour, 1.0f);
    }

    // The net irradiance, the vehicle share and the occlusion's visibility at the pixel.
    float3 eNet = float3(0.0f, 0.0f, 0.0f);
    float vehicle = 0.0f;
    float ao = 1.0f;
    bool fast = GiaFastLookupApply();
    bool visibility = GiaHasVisibility() && (!fast || (fr.aoStrength > 0.0f && t < fr.aoFadeEnd));
#if GIA_UP
    {
        bool quarter = GiaQuarterLookup();
        if (quarter)
        {
            eNet = GiaQuarterIrradiance(pixel, t, n, fr, fast);
            if (fast && !any(eNet > 0.0f))
            {
                discard;
            }
        }
        float2 q = (float2(pixel) + 0.5f) * 0.5f - 0.5f;
        int2 q0 = int2(floor(q));
        float2 f = q - float2(q0);
        int2 tsize = int2(fr.traced);
        int2 rlast = int2(fr.render) - 1;
        float3 eSum = float3(0.0f, 0.0f, 0.0f);
        float vSum = 0.0f;
        float aoSum = 0.0f;
        float wSum = 0.0f;
        // CHECK: confirm the gather's order and marked depth bits on both APIs.
        // Gather is only needed on the interior; padded and border pixels use clamped loads.
        bool gather = fast && all(q0 >= 0) && all(q0 + 1 < tsize);
        float4 guideVehicle = 0.0f;
        float4 guideAo = 1.0f;
        bool guidesDone = false;
        if (gather)
        {
            float2 uv = (float2(q0) + 1.0f) / fr.traced;
            uint4 tags = asuint(g_textures[g_push.slots.w].GatherRed(g_samplers[0], uv).wzxy);
            guideVehicle = float4(tags & 1u);
            if (visibility)
            {
                guideAo = g_textures[g_push.info.x].GatherRed(g_samplers[0], uv).wzxy;
            }
            if (quarter && GiaConstantGuide(guideVehicle) && GiaConstantGuide(guideAo))
            {
                // If every normal is missing, the filter returns these defaults.
                guidesDone = guideVehicle.x == 0.0f && guideAo.x == 1.0f;
                if (!guidesDone)
                {
                    // A valid anchor guarantees a positive weight in the same quad.
                    float2 centre = GiaLoadPlane(g_push.slots.y, min(pixel & ~1, rlast)).xy;
                    guidesDone = GbRt1Valid(centre);
                }
                if (guidesDone)
                {
                    vehicle = guideVehicle.x;
                    ao = guideAo.x;
                }
            }
        }
        for (int j = 0; j < 2 && !guidesDone; ++j)
        {
            for (int i = 0; i < 2; ++i)
            {
                int2 c = clamp(q0 + int2(i, j), int2(0, 0), tsize - 1);
                float bilinear = (i == 0 ? 1.0f - f.x : f.x) * (j == 0 ? 1.0f - f.y : f.y);
                int2 r = min(c * 2, rlast);
                float2 nc1 = GiaLoadPlane(g_push.slots.y, r).xy;
                if (!GbRt1Valid(nc1))
                {
                    continue;
                }
                float4 look = 0.0f;
                if (quarter)
                {
                    // Match the original RGBA16F lookup's guide precision.
                    float depth = GiaLinearDepth(GiaLoadPlane(g_push.slots.x, r).x, fr.lin);
                    look.w = f16tof32(f32tof16(depth));
                }
                else
                {
                    look = g_textures[g_push.info.y].Load(int3(c, 0));
                }
                float w = bilinear * max(GiaDepthWeight(t, look.w) * GiaNormalWeight(n, GbRt1Decode(nc1)), GIA_MIN_UPSAMPLE_WEIGHT);
                if (!quarter)
                {
                    eSum = eSum + look.xyz * w;
                }
                int tap = j * 2 + i;
                float v = gather ? guideVehicle[tap] : GiaVehicleMark(g_textures[g_push.slots.w].Load(int3(c, 0)).x);
                vSum = vSum + v * w;
                if (visibility)
                {
                    float seen = gather ? guideAo[tap] : g_textures[g_push.info.x].Load(int3(c, 0)).x;
                    aoSum = aoSum + seen * w;
                }
                wSum = wSum + w;
            }
        }
        if (wSum > 1.0e-8f)
        {
            float invWeight = 1.0f / wSum;
            if (!quarter)
            {
                eNet = eSum * invWeight;
            }
            vehicle = vSum * invWeight;
            ao = visibility ? aoSum * invWeight : 1.0f;
        }
    }
#else
    {
        GiSample s = GiLookup(GiaReceiverPosition(pixel, t, fr), n, fast);
        eNet = GiaLookupNetMixed(s, n, fr, GiaPixelTerm(pixel, t, n, fr));
        vehicle = GiaVehicleMark(g_textures[g_push.slots.w].Load(int3(pixel, 0)).x);
        if (visibility)
        {
            ao = g_textures[g_push.info.x].Load(int3(pixel, 0)).x;
        }
    }
#endif

    float aoTerm = visibility ? GiaAoTerm(ao, vehicle, t, fr.aoStrength, fr.aoFadeEnd, fr.paintPower) : 1.0f;
    float nDotL = saturate(dot(n, fr.sunDir));
    float3 albedo = GbAlbedoDecode(float3(rt2.x, rt2.y, rt2.z));

    // The player's cabin shields its inside from the street the probes cannot see the car's roof of: the field's share on the cabin's own surfaces (a vehicle's part that is not paint, chrome or glass,
    // inside the car's box) is the cabin's, with the glass's share left. A frame with no rule (the camera is not in a roofed car, the setting is off: share 1) reads nothing here.
    float cabin = 1.0f;
    float4 cabinA = GiaCabinRow(0u);
    if (cabinA.w < 1.0f && GiaMaterialSlot() != GIC_NO_MATERIAL_SLOT)
    {
        float2 rt3 = GiaLoadPlane(GiaMaterialSlot(), pixel).xy;
        cabin = GicShare(cabinA, GiaCabinRow(1u), GiaReceiverPosition(pixel, t, fr), GbRt3Class(rt3), GbRt3Flags(rt3));
    }
    float3 add = NgFinite3(GiaBounceAdd(albedo, rt2.w, eNet, fr.strength, fr.fieldShare * cabin, vehicle, aoTerm, fr.ambient, fr.sunLuma, nDotL, fr.cap));   // added to the scene: a NaN or an infinity is none (nan_guard.hlsli)

    if (view == 9u)
    {
        return float4(add * (4.0f * GIA_VIEW_SCALE), 1.0f);
    }
    if (view == 10u)
    {
        return float4(GiaTone(eNet), 1.0f);
    }
    if (view == 11u)
    {
        return float4(aoTerm * GIA_VIEW_SCALE, aoTerm * GIA_VIEW_SCALE, aoTerm * GIA_VIEW_SCALE, 1.0f);
    }
    if (!any(add > 0.0f))
    {
        discard;
    }
    return float4(add.x, add.y, add.z, 0.0f);
}

#endif
