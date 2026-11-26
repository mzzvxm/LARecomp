// Farthest visible receiver at the AO trigger. In reversed depth this is
// the smallest positive sample. Reduce before linearising so every source
// texel is covered even when the AO's level 0 uses a sparse/half-size grid.
#if SHADOW_FIT_MS
Texture2DMS<float4> g_depth : register(t0);
#else
Texture2D<float4> g_depth : register(t0);
#endif
RWByteAddressBuffer g_result : register(u1);

struct Push {
    uint width;
    uint height;
    uint cols;
    uint tiles;
    uint samples;
    uint token;
    uint finish;
};
#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<Push> g_push;
#else
ConstantBuffer<Push> g_push : register(b0, space1);
#endif

static const uint kEmpty = 0xffffffffu;
groupshared uint g_min[64];

[numthreads(8, 8, 1)]
void main(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID, uint lane : SV_GroupIndex) {
    uint best = kEmpty;
    if (g_push.finish != 0u) {
        for (uint tile = lane; tile < g_push.tiles; tile += 64u) {
            best = min(best, g_result.Load(8u + tile * 4u));
        }
    } else {
        const uint2 origin = group.xy * 32u + thread.xy;
        [unroll] for (uint y = 0u; y < 32u; y += 8u) {
            [unroll] for (uint x = 0u; x < 32u; x += 8u) {
                const uint2 p = origin + uint2(x, y);
                if (p.x < g_push.width && p.y < g_push.height) {
#if SHADOW_FIT_MS
                    for (uint sample_index = 0u; sample_index < g_push.samples; ++sample_index) {
                        const float depth = g_depth.Load(p, sample_index).r;
                        if (depth > 0.0 && depth <= 1.0) best = min(best, asuint(depth));
                    }
#else
                    const float depth = g_depth.Load(int3(p, 0)).r;
                    if (depth > 0.0 && depth <= 1.0) best = min(best, asuint(depth));
#endif
                }
            }
        }
    }

    g_min[lane] = best;
    GroupMemoryBarrierWithGroupSync();
    for (uint stride = 32u; stride != 0u; stride >>= 1u) {
        if (lane < stride) g_min[lane] = min(g_min[lane], g_min[lane + stride]);
        GroupMemoryBarrierWithGroupSync();
    }
    if (lane == 0u) {
        if (g_push.finish != 0u) {
            g_result.Store2(0u, uint2(g_push.token, g_min[0]));
        } else {
            g_result.Store(8u + 4u * (group.y * g_push.cols + group.x), g_min[0]);
        }
    }
}
