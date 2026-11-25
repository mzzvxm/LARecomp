// MCLA GPU renderer -- the probe field's apply: the player's cabin term (INTERIOR-CAM-FIX; the rules and the reasons are src/gpu/gi_cabin_rules.h's comment, of which this file is THE TWIN).
//
// Included by hlsl/gi_apply_ps.hlsl. Everything here is a function of numbers only, so that src/gpu/gi_cabin_test.cpp can compile THIS FILE as C++ (through a shim of the HLSL vector types)
// and compare each function with gi_cabin_rules.h on random inputs. That is why the text keeps to the subset both languages read: scalar and float3 arithmetic, .x .y .z .w, no swizzles of more
// than one component, no out parameters, no attributes, and only the intrinsics min, max and saturate.
//
// THE ROWS. The caller reads the apply's two cabin rows (after the field's 20 and the apply's 9) and hands them here as float4s: row 0 = the car's box minimum xyz and the cabin's share
// (1: no rule), row 1 = the box's maximum xyz and the edge's width. The share is < 1 only in a frame whose camera is inside a roofed car (gi_cabin_rules.h MakeRows), so a frame with no rule
// costs the caller one compare and no read of the material plane.
//
// The names: every one starts with Gic or GIC_.

#ifndef MCLA_GI_CABIN_COMMON_HLSLI
#define MCLA_GI_CABIN_COMMON_HLSLI

// The classes that are not the car's inside (gbuffer_types.h SurfaceClass: none 0, paint 8, chrome 9, glass 11, emissive 13) and the flag of a vehicle's part (kMaterialFlagVehicle).
#define GIC_CLASS_NONE 0u
#define GIC_CLASS_PAINT 8u
#define GIC_CLASS_CHROME 9u
#define GIC_CLASS_GLASS 11u
#define GIC_CLASS_EMISSIVE 13u
#define GIC_FLAG_VEHICLE 4u
// The push block's last word: the material plane's slot is in bits 16-31, 0xFFFF is none.
#define GIC_MATERIAL_SLOT_SHIFT 16u
#define GIC_NO_MATERIAL_SLOT 0xFFFFu

// A smooth step of t, which is clamped to 0..1 first.
float GicSmooth(float t)
{
    float s = saturate(t);
    return s * s * (3.0f - 2.0f * s);
}

// How far inside the box (lo, hi) the point is, 0 to 1: the smooth step of its distance to the nearest face over `edge`, per axis, multiplied. 0 outside the box.
float GicEdgeWeight(float3 pos, float3 lo, float3 hi, float edge)
{
    float e = max(edge, 1.0e-4f);
    float dx = min(pos.x - lo.x, hi.x - pos.x);
    float dy = min(pos.y - lo.y, hi.y - pos.y);
    float dz = min(pos.z - lo.z, hi.z - pos.z);
    return GicSmooth(dx / e) * GicSmooth(dy / e) * GicSmooth(dz / e);
}

// A surface of the car's inside: a vehicle's part that is not paint, chrome, glass, emissive or no data.
bool GicIsCabinSurface(uint cls, uint flags)
{
    if ((flags & GIC_FLAG_VEHICLE) == 0u)
    {
        return false;
    }
    if (cls == GIC_CLASS_NONE || cls == GIC_CLASS_PAINT || cls == GIC_CLASS_CHROME || cls == GIC_CLASS_GLASS || cls == GIC_CLASS_EMISSIVE)
    {
        return false;
    }
    return true;
}

// The term the field's bounce at `pos` is multiplied by: 1 for a pixel that is not the cabin's, or where the rows carry no rule (share 1); else the share inside the box, blended at its faces.
float GicShare(float4 rowA, float4 rowB, float3 pos, uint cls, uint flags)
{
    float share = rowA.w;
    if (!(share < 1.0f) || !GicIsCabinSurface(cls, flags))
    {
        return 1.0f;
    }
    float inside = GicEdgeWeight(pos, float3(rowA.x, rowA.y, rowA.z), float3(rowB.x, rowB.y, rowB.z), rowB.w);
    return 1.0f + (share - 1.0f) * inside;
}

#endif
