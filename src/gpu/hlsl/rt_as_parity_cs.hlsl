// Standalone BLAS compaction parity fixture (rt_as_parity_test.cpp).
RaytracingAccelerationStructure gScene : register(t0);
RWByteAddressBuffer gHits : register(u1);

static const float2 kOrigins[6] = {
    float2(0.25f, 0.25f), float2(1.25f, 0.25f), float2(4.25f, 0.25f),
    float2(5.25f, 0.25f), float2(0.25f, 0.25f), float2(10.0f, 0.25f)
};
static const uint kMasks[6] = {1u, 1u, 2u, 2u, 4u, 3u};

[numthreads(8, 1, 1)]
void main(uint3 thread : SV_DispatchThreadID) {
    const uint i = thread.x;
    if (i >= 6u) return;

    RayDesc ray;
    ray.Origin = float3(kOrigins[i], -1.0f);
    ray.Direction = float3(0.0f, 0.0f, 1.0f);
    ray.TMin = 0.0f;
    ray.TMax = i == 3u ? 1.5f : 4.0f;

    RayQuery<RAY_FLAG_NONE> closest;
    closest.TraceRayInline(gScene, RAY_FLAG_NONE, kMasks[i], ray);
    while (closest.Proceed()) {
        if (closest.CandidateTriangleBarycentrics().x < 0.5f) closest.CommitNonOpaqueTriangleHit();
    }

    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> visibility;
    visibility.TraceRayInline(gScene, RAY_FLAG_NONE, kMasks[i], ray);
    while (visibility.Proceed()) {
        if (visibility.CandidateTriangleBarycentrics().x < 0.5f) visibility.CommitNonOpaqueTriangleHit();
    }

    uint4 result = uint4(0xffffffffu, 0xffffffffu, 0xffffffffu, 0u);
    if (closest.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        result = uint4(closest.CommittedInstanceID(), closest.CommittedGeometryIndex(),
                       closest.CommittedPrimitiveIndex(), asuint(closest.CommittedRayT()));
    }
    gHits.Store4(i * 20u, result);
    gHits.Store(i * 20u + 16u, visibility.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 1u : 0u);
}
