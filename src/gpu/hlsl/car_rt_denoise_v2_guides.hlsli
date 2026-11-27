#ifndef MCLA_CAR_RT_DENOISE_V2_GUIDES_HLSLI
#define MCLA_CAR_RT_DENOISE_V2_GUIDES_HLSLI

static const uint kCarDnTraceGuide = 1u << 30;

// 12 bits of octahedral normal and 12 bits of instance ID are exact in R32F.
// A zero depth marks empty space or chrome, which has a different payload.
float4 CarDnGuide(float depth, float3 normal, uint instance) {
    normal /= abs(normal.x) + abs(normal.y) + abs(normal.z);
    const float2 signOct = float2(normal.x < 0.0 ? -1.0 : 1.0, normal.y < 0.0 ? -1.0 : 1.0);
    const float2 oct = normal.z < 0.0 ? (1.0 - abs(normal.yx)) * signOct : normal.xy;
    const uint2 bits = uint2(round(saturate(oct * 0.5 + 0.5) * 63.0));
    return float4(depth, float(bits.x | (bits.y << 6u) | (instance << 12u)), 0.0, 0.0);
}

uint CarDnInstance(float4 guide) { return uint(guide.y) >> 12u; }

float3 CarDnNormal(float4 guide) {
    const uint bits = uint(guide.y);
    const float2 oct = float2(bits & 63u, (bits >> 6u) & 63u) * (2.0 / 63.0) - 1.0;
    float3 normal = float3(oct, 1.0 - abs(oct.x) - abs(oct.y));
    normal.xy += saturate(-normal.z) * float2(normal.x < 0.0 ? 1.0 : -1.0, normal.y < 0.0 ? 1.0 : -1.0);
    return normalize(normal);
}

#endif
