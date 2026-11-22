// MCLA GPU renderer -- the city hit data's probe (city_hit_probe.h, CITY-RT M2 package H3).
//
// A compute pass that reads the hit data the way the hit shading will, through the descriptor set's t6..t9 (city_hit_common.hlsli), and writes what it read to a buffer the host compares
// with what it copied: for every sector table slot, in one thread each, a sampled checksum of the slot's block (the regions the sector row sizes: the indices, the first three vertices,
// the run ids, the first run record, the corners), the sample count, the row's flags and its triangle count; and in the header the first word of the material table and the first slot of
// the texture table. If the pool, the table or a binding were wrong on this backend (an offset, an alignment, a stale descriptor, a table that was not copied), the host's verdict says
// which ("[city hit] probe: ...").
//
// The checksum is city_hit_pool_rules.h ProbeOfBlock: acc = acc * 31 + word + index in the region, every PROBE_STRIDE_WORDS-th word and the last one of each region, the regions in the order
// below, acc running on through them. The numbers below are in step with that header (the unit test reads this file and compares).
//
// Layout (city_hit_probe.cpp), as the city's shadow pass: set 0 the bindless texture set (not read), set 1 this pass's own (u0 the result buffer, space1), set 2 the ray tracing set (the
// car scene's t0..t5 and the hit data's t6..t9, CITY_HIT_SPACE 2), the push range at (b0, space3; a Vulkan push constant block). Shader Model 6.5, SPIR-V for Vulkan 1.2.

#define CITY_HIT_SPACE 2
#include "city_hit_common.hlsli"

// city_hit_pool_rules.h: the numbers.
#define PROBE_STRIDE_WORDS 61u
#define PROBE_MAGIC 0x42525043u
#define PROBE_HEADER_WORDS 4u
#define PROBE_SLOT_WORDS 4u
#define PROBE_POSITION_BYTES 36u

RWByteAddressBuffer gProbe : register(u0, space1);

struct ProbePush
{
    // x the number of sector table slots (2 x the pack's sectors), yzw zero
    uint4 slots;
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<ProbePush> g_push;
#else
ConstantBuffer<ProbePush> g_push : register(b0, space3);
#endif

// One region of the pool: `size` bytes from byte offset `at` (a multiple of 4), the checksum running on in `acc`, the samples counted in `samples`.
void ProbeRegion(inout uint acc, inout uint samples, uint at, uint size)
{
    const uint words = size / 4u;
    if (words == 0u)
    {
        return;
    }
    for (uint i = 0u; i < words; i += PROBE_STRIDE_WORDS)
    {
        acc = acc * 31u + gCityPool.Load(at + i * 4u) + i;
        samples += 1u;
    }
    if ((words - 1u) % PROBE_STRIDE_WORDS != 0u)
    {
        acc = acc * 31u + gCityPool.Load(at + (words - 1u) * 4u) + (words - 1u);
        samples += 1u;
    }
}

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint slot = id.x;
    if (slot == 0u)
    {
        gProbe.Store(0u, PROBE_MAGIC);
        gProbe.Store(4u, gCityMaterials[0].x);
        gProbe.Store(8u, gCityTextures[0].x);
        gProbe.Store(12u, g_push.slots.x);
    }
    if (slot >= g_push.slots.x)
    {
        return;
    }
    // The slot is the instance id's low bits: (sector << 1) | detail.
    const CityHitSector s = CityHitLoadSector(slot);
    uint acc = 0u;
    uint samples = 0u;
    if ((s.flags & CITY_HIT_SECTOR_VALID) != 0u)
    {
        ProbeRegion(acc, samples, s.indexOffset, s.triangleCount * 12u);
        ProbeRegion(acc, samples, s.positionOffset, PROBE_POSITION_BYTES);
        if ((s.flags & CITY_HIT_SECTOR_RUNS) != 0u)
        {
            ProbeRegion(acc, samples, s.runIdOffset, ((s.triangleCount + 1u) / 2u) * 4u);
            ProbeRegion(acc, samples, s.runTableOffset, CITY_HIT_RUN_BYTES);
        }
        if ((s.flags & CITY_HIT_SECTOR_CORNERS) != 0u)
        {
            ProbeRegion(acc, samples, s.attributeOffset, s.triangleCount * 24u);
        }
    }
    const uint record = (PROBE_HEADER_WORDS + slot * PROBE_SLOT_WORDS) * 4u;
    gProbe.Store(record, acc);
    gProbe.Store(record + 4u, samples);
    gProbe.Store(record + 8u, s.flags);
    gProbe.Store(record + 12u, s.triangleCount);
}
