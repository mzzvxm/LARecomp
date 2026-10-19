// MCLA GPU renderer -- the samplers of the host passes that filter: FSR1's EASU gathers, FXAA and SMAA.
//
// The BindlessHeap's sampler set, bound as set 4 of the host pass layout
// (host_passes.h): s0, space4, an array indexed by the heap's fixed slots
// (descriptors.h). On D3D12 the samplers share space 4 with the root constants
// (b0, space4) -- different register types; on Vulkan the push block is no set.
// Included only by the passes that sample: the others leave the set unused.

#ifndef MCLA_GPU_HOST_PASS_SAMPLERS_HLSLI
#define MCLA_GPU_HOST_PASS_SAMPLERS_HLSLI

SamplerState g_samplers[] : register(s0, space4);

// descriptors.h kSamplerLinearClamp, kSamplerPointClamp.
static const uint kSamplerLinearClamp = 0;
static const uint kSamplerPointClamp = 2;

#endif  // MCLA_GPU_HOST_PASS_SAMPLERS_HLSLI
