#include "car_rt_denoise_v2_guides.hlsli"
#include "rt_denoise_v2_math.hlsli"

Texture2D<float4> g_textures[] : register(t0, space0);
#ifdef __spirv__
[[vk::image_format("rgba16f")]]
#endif
RWTexture2D<float4> g_out : register(u0, space1);
#ifdef __spirv__
[[vk::image_format("rgba32f")]]
#endif
RWTexture2D<float4> g_features : register(u1, space1);
ByteAddressBuffer g_motion : register(t0, space2);

struct CarDnPush {
    uint4 slots;
    uint4 extent;
    int4 rect;
    int4 previousRect;
    int4 dispatchRect;
    float4 lens;
    uint4 config;
    float4 trace;
};
#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<CarDnPush> g_push;
#else
ConstantBuffer<CarDnPush> g_push : register(b0, space3);
#endif

bool Glass() { return (g_push.config.z & 1u) != 0u; }
bool Inside(int2 p, int4 rect) { return all(p >= rect.xy) && all(p < rect.zw); }
float4 Read(uint slot, int2 p) { return g_textures[slot].Load(int3(p, 0)); }

float3 Position(float2 p, float depth, float2 lens) {
    const float divisor = float(g_push.config.y);
    const float2 pixel = min(floor((p + 0.5) * divisor), float2(g_push.extent.zw) - 1.0) + 0.5;
    const float2 ndc = pixel * float2(2.0, -2.0) / float2(g_push.extent.zw) + float2(-1.0, 1.0);
    return float3(ndc * lens, 1.0) * depth;
}

float2 Project(float3 p, float2 lens) {
    const float2 ndc = p.xy / (p.z * lens);
    const float2 pixel = (ndc * float2(0.5, -0.5) + 0.5) * float2(g_push.extent.zw);
    const float divisor = float(g_push.config.y);
    // The last texel of an odd extent can represent a partial block.
    const float2 last = float2(g_push.extent.xy) - 1.0;
    const float2 lastPixel = min(floor((last + 0.5) * divisor), float2(g_push.extent.zw) - 1.0) + 0.5;
    const float2 normal = (pixel - (floor(divisor * 0.5) + 0.5)) / divisor;
    const float2 partial = (last - 1.0) + (pixel - (lastPixel - 1.0));
    const bool2 partialAxis = (lastPixel < (last * divisor + floor(divisor * 0.5) + 0.5)) & (pixel > lastPixel - 1.0);
    return float2(partialAxis.x ? partial.x : normal.x, partialAxis.y ? partial.y : normal.y);
}

float HitWeight(float a, float b, bool glass) {
    if (a == 0.0 || b == 0.0) return a == b ? 1.0 : (glass ? 0.0 : 0.4);
    const float tolerance = glass ? 0.03 + 0.025 * min(a, b) : 0.15 + 0.35 * min(a, b);
    return saturate(1.0 - abs(a - b) / tolerance);
}

float Sharp(int2 p) {
    if (Glass()) return 0.0;
    const float aux = Read(g_push.config.w, p).w;
    return aux > 0.5 ? saturate((1.0 - aux) / 0.49) : 1.0;
}

// Preserve the paint's weighted colour and the signed hit class. Glass keeps
// exact pane coverage and carries the additive colour directly.
float4 Signal(float4 packed) {
    if (Glass()) return float4(packed.rgb, 1.0);
    const float weight = saturate(abs(packed.a) - 1.0);
    return float4(packed.rgb * weight, packed.a < 0.0 ? -weight : weight);
}

float4 Finish(float3 colour, float weight, float hidden) {
    if (Glass()) return float4(max(colour, 0.0), 1.0);
    if (weight < 1.0 / 512.0) return float4(0.0, 0.0, 0.0, 1.0);
    return float4(clamp(colour / weight, 0.0, 65504.0),
                  (hidden > 0.5 * weight ? -1.0 : 1.0) * (1.0 + saturate(weight)));
}

float Square(float3 a) {
    const float3 yc = Dn2YCoCg(a);
    return dot(yc * yc, float3(1.0, 0.25, 0.25));
}

float PackMoments(float variance, float count) {
    const uint v = uint(round(clamp(log2(1.0 + variance) * 2048.0, 0.0, 65535.0)));
    return float(v | (uint(round(count * 4.0)) << 16u));
}
float Variance(float packed) { return exp2(float(uint(packed) & 65535u) / 2048.0) - 1.0; }
float Samples(float packed) { return float(uint(packed) >> 16u) * 0.25; }

float Geometry(int2 p, float4 tap, float3 position, float3 normal, uint instance, float2 lens, bool glass) {
    if (tap.x <= 0.0 || CarDnInstance(tap) != instance) return 0.0;
    const float cosine = dot(normal, CarDnNormal(tap));
    const float3 delta = Position(p, tap.x, lens) - position;
    const float plane = abs(dot(delta, normal));
    const float tolerance = glass ? 0.006 + 0.005 * position.z : 0.012 + 0.012 * position.z;
    return smoothstep(glass ? 0.94 : 0.83, 0.99, cosine) * (1.0 - saturate(plane / tolerance));
}

struct Moment {
    float3 colour;
    float weight;
    float hidden;
    float count;
    float variance;
    float support;
};

Moment GatherHistory(float3 position, float3 normal, uint instance, float hit) {
    Moment history = (Moment)0;
    if ((g_push.config.z & 2u) == 0u || instance >= uint(g_push.trace.y)) return history;
    const uint address = instance * 64u;
    const uint previous = g_motion.Load(address + 48u);
    if (previous == 0u) return history;
    const float4 row0 = asfloat(g_motion.Load4(address));
    const float4 row1 = asfloat(g_motion.Load4(address + 16u));
    const float4 row2 = asfloat(g_motion.Load4(address + 32u));
    const float4 homogeneous = float4(position, 1.0);
    const float3 projected = float3(dot(homogeneous, row0), dot(homogeneous, row1), dot(homogeneous, row2));
    if (projected.z <= 0.01) return history;
    const float3 cof0 = cross(row1.xyz, row2.xyz);
    const float3 cof1 = cross(row2.xyz, row0.xyz);
    const float3 cof2 = cross(row0.xyz, row1.xyz);
    const float3 beforeNormal = normalize(float3(dot(normal, cof0), dot(normal, cof1), dot(normal, cof2))) *
                                (dot(row0.xyz, cof0) < 0.0 ? -1.0 : 1.0);
    // A mirror direction changes as the car turns or the viewing angle moves.
    // Limit reuse where the reflected footprint slides across the scene.
    const float3 ray = reflect(normalize(position), normal);
    const float3 beforeRay = reflect(normalize(projected), beforeNormal);
    const float3 transformedRay = normalize(float3(dot(ray, row0.xyz), dot(ray, row1.xyz), dot(ray, row2.xyz)));
    const float rayShift = length(transformedRay - beforeRay) * hit;
    const float allowed = Glass() ? 0.03 + 0.025 * hit : 0.12 + max(g_push.trace.x, 0.035) * hit;
    const float motionWeight = 1.0 - smoothstep(allowed * 0.5, allowed, rayShift);
    if (!(motionWeight > 0.0)) return history;
    const float2 screen = Project(projected, g_push.lens.zw);
    if (!all(screen >= float2(g_push.previousRect.xy) - 1.0) ||
        !all(screen < float2(g_push.previousRect.zw))) return history;
    const float2 uv = frac(screen);
    const int2 base = int2(floor(screen));
    [unroll]
    for (uint i = 0; i < 4u; ++i) {
        const int2 p = base + int2(i & 1u, i >> 1u);
        if (!Inside(p, g_push.previousRect)) continue;
        const int2 q = p - g_push.previousRect.xy;
        const float4 guide = Read(g_push.slots.w, q);
        if (abs(guide.x - projected.z) > 0.02 + 0.025 * projected.z) continue;
        const float geometry = Geometry(p, guide, projected, beforeNormal, previous - 1u, g_push.lens.zw, Glass());
        const float hitWeight = HitWeight(hit, guide.z, Glass());
        const float weight = geometry * hitWeight * ((i & 1u) ? uv.x : 1.0 - uv.x) * ((i & 2u) ? uv.y : 1.0 - uv.y);
        if (weight <= 0.0) continue;
        const float4 signal = Signal(Read(g_push.slots.z, q));
        const float fraction = weight / (history.support + weight);
        history.variance = Dn2Variance(history.variance, Variance(guide.w), Square(signal.rgb - history.colour), 1.0 - fraction);
        history.colour = lerp(history.colour, signal.rgb, fraction);
        history.weight = lerp(history.weight, abs(signal.a), fraction);
        history.hidden = lerp(history.hidden, max(0.0, -signal.a), fraction);
        history.count = lerp(history.count, Samples(guide.w), fraction);
        history.support += weight;
    }
    history.support *= motionWeight;
    return history;
}

void Temporal(int2 p, float4 raw, float4 guide) {
    const int2 outPixel = p - g_push.rect.xy;
    if (!(guide.x > 0.0) || abs(raw.a) < 0.5) {
        g_features[outPixel] = 0.0;
        g_out[outPixel] = raw;
        return;
    }
    const uint instance = CarDnInstance(guide);
    const float3 normal = CarDnNormal(guide);
    const float3 position = Position(p, guide.x, g_push.lens.xy);
    const float hit = guide.z;
    const float sharp = Sharp(p);
    const float4 signal = Signal(raw);
    const float3 ownClip = Dn2YCoCg(signal.rgb);
    float3 lo = ownClip;
    float3 hi = ownClip;
    float2 weightRange = abs(signal.a);
    float3 sum = 0.0;
    float3 squared = 0.0;
    float bright = 0.0;
    float taps = 0.0;
    [unroll]
    for (int i = 0; i < 9; ++i) {
        if (i == 4) continue;
        const int2 q = p + int2(i % 3 - 1, i / 3 - 1);
        if (!Inside(q, g_push.rect)) continue;
        const float4 neighborRaw = Read(g_push.slots.x, q);
        if (abs(neighborRaw.a) < 0.5) continue;
        const float4 neighbor = Read(g_push.slots.y, q);
        if (Geometry(q, neighbor, position, normal, instance, g_push.lens.xy, Glass()) <= 0.0) continue;
        if (HitWeight(hit, neighbor.z, Glass()) <= 0.0) continue;
        const float4 value = Signal(neighborRaw);
        const float3 yc = Dn2YCoCg(value.rgb);
        lo = min(lo, yc);
        hi = max(hi, yc);
        weightRange = float2(min(weightRange.x, abs(value.a)), max(weightRange.y, abs(value.a)));
        sum += yc;
        squared += yc * yc;
        bright = max(bright, yc.x);
        taps += 1.0;
    }
    const float3 mean = sum / max(taps, 1.0);
    const float3 deviation = sqrt(max(squared / max(taps, 1.0) - mean * mean, 0.0));
    // Glass is a sharp signal. An isolated reflected headlamp may be real.
    const float ceiling = Dn2FireflyCeiling(mean.x, deviation.x, bright);
    const float firefly = !Glass() && taps >= 3.0 && ownClip.x > ceiling ? ceiling / ownClip.x : 1.0;
    const float3 colour = signal.rgb * lerp(firefly, 1.0, sharp);
    const Moment history = GatherHistory(position, normal, instance, hit);
    const float3 margin = max(deviation * 0.5, float3(0.002, 0.001, 0.001));
    const float3 historyClip = Dn2YCoCg(history.colour);
    const float3 clipped = clamp(historyClip, lo - margin, min(hi, max(mean + 3.0 * deviation, lo)) + margin);
    const float3 previous = Dn2Rgb(clipped);
    const float limit = Glass() ? 0.65 : 0.90;
    const float change = abs(historyClip.x - clipped.x) / max(0.02, ownClip.x + deviation.x);
    const float keep = min(limit, history.count / (history.count + 1.0)) * saturate(history.support) *
                       (1.0 - sharp) * (1.0 - smoothstep(0.12, 0.75, change));
    const float weight = clamp(history.weight, weightRange.x, weightRange.y);
    const float mixedWeight = lerp(abs(signal.a), weight, keep);
    const float hidden = lerp(max(0.0, -signal.a), min(history.hidden, weight), keep);
    const float variance = Dn2Variance(history.variance, dot(deviation * deviation, float3(1.0, 0.25, 0.25)),
                                       Square(colour - previous), keep);
    g_out[outPixel] = Finish(lerp(colour, previous, keep), mixedWeight, hidden);
    g_features[outPixel] = float4(guide.xy, hit, PackMoments(variance, min(16.0, 1.0 + history.count * keep)));
}

void Spatial(int2 p, float4 raw, float4 guide) {
    if (guide.x <= 0.0) {
        g_out[p] = raw;
        return;
    }
    const float sharp = Sharp(p);
    const float4 own = Signal(raw);
    const float sigma = sqrt(Variance(guide.w));
    const float signal = max(0.025, Dn2YCoCg(own.rgb).x);
    const float relativeNoise = sigma / signal;
    const float footprint = guide.z * g_push.trace.x * float(g_push.extent.y) / (guide.x * g_push.lens.y);
    const float radius = Dn2Radius(Glass() ? 2.0 : clamp(footprint, 2.0, 8.0), Samples(guide.w), relativeNoise);
    const int stride = int(1u << (g_push.config.x - 1u));
    const float strength = (1.0 - sharp) * (1.0 - smoothstep(radius, radius + 1.0, float(stride)));
    if (strength <= 0.0) {
        g_out[p] = raw;
        return;
    }
    const float3 position = Position(p, guide.x, g_push.lens.xy);
    const float3 normal = CarDnNormal(guide);
    const uint instance = CarDnInstance(guide);
    float3 colour = 4.0 * own.rgb;
    float weight = 4.0 * abs(own.a);
    float hidden = 4.0 * max(0.0, -own.a);
    float total = 4.0;
    [unroll]
    for (int i = 0; i < 9; ++i) {
        if (i == 4) continue;
        const int2 offset = int2(i % 3 - 1, i / 3 - 1);
        const int2 q = p + offset * stride;
        if (!Inside(q, g_push.rect)) continue;
        const float4 neighbor = Read(g_push.slots.y, q - g_push.rect.xy);
        const float geo = Geometry(q, neighbor, position, normal, instance, g_push.lens.xy, Glass());
        const float hit = HitWeight(guide.z, neighbor.z, Glass());
        if (!(geo * hit > 0.0)) continue;
        const float4 tap = Signal(Read(g_push.slots.x, g_push.config.x == 1u ? q - g_push.rect.xy : q));
        const float difference = sqrt(Square(own.rgb - tap.rgb));
        const float scale = Glass() ? max(0.005, sigma * 2.0 + signal * 0.10) : sigma * 4.0 + signal + 0.025;
        const float kernel = (offset.x == 0 || offset.y == 0 ? 2.0 : 1.0) * geo * hit /
                             (1.0 + difference / scale);
        total += kernel;
        colour += kernel * tap.rgb;
        weight += kernel * abs(tap.a);
        hidden += kernel * max(0.0, -tap.a);
    }
    g_out[p] = Finish(lerp(own.rgb, colour / total, strength),
                     lerp(abs(own.a), weight / total, strength),
                     lerp(max(0.0, -own.a), hidden / total, strength));
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    const int2 p = int2(id.xy) + g_push.dispatchRect.xy;
    if (!Inside(p, g_push.dispatchRect)) return;
    if (g_push.config.x == 0u) {
        Temporal(p, Read(g_push.slots.x, p), Read(g_push.slots.y, p));
    } else {
        const int2 q = p - g_push.rect.xy;
        Spatial(p, Read(g_push.slots.x, g_push.config.x == 1u ? q : p), Read(g_push.slots.y, q));
    }
}
