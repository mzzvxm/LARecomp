// MCLA GPU renderer -- the cars' ray traced reflections (car_rt_refl.h, RR-4b), the raw mode's extra pass: the hit distance guide of DLSS Ray Reconstruction.
//
// Recorded only in a frame Ray Reconstruction takes and the cars' reflections run RAW (car_rt_refl_rules.h RawHitDistance's frame: dlss_rr_rules.h RawModes): the trace's image is then read as
// it is by the paint (the a-trous filter, which takes the cone's grain out, is skipped: Ray Reconstruction is the denoiser), and this pass tells Ray Reconstruction how far each reflection
// went, which is what it moves the reflection's history by (perf/DLSS-RR.md, the specular hit distance guide). One thread is one pixel of the PICTURE (the guide pass reads the output
// pixel for pixel; rr_guides.h Inputs::hit[1]); its texel of the trace's aux image is the pixel's coordinates over the divisor the history image has (1 or 2: car_selfrefl_rules.h).
//
//   0              a texel the trace found no car in (the aux's w is 0: not the player's body or another car's paint, the chrome of another car, nothing at all)
//   the distance   metres from the surface to where the cone's rays hit, the mean of the rays that did (the aux's x is sqrt(distance / reach) in 8 bits: the denoise's own decode)
//   kMissDistance  the car's texel whose cone found no part of the car (the aux's x is 0): the paint shows the environment there, which is far (4096 m, the same number the screen-space
//                  reflections write for a ray that found nothing)
//
// The aux image is the CURRENT frame's trace (the pass runs at the trigger, on this frame's history image): the paint of the NEXT frame reads the trace, reprojected; the distance here is in the
// pixels of the frame the guide pass works on. A pixel next to the car's edge in a 2x2 block (divisor 2) takes its texel's distance: at most one pixel of fringe.
//
// Shader Model 6.0. The layout is the denoise's (car_rt_refl.cpp): set 0 the bindless Texture2D<float4> set (space 0), set 1 the output (u0, space 1), the push block (b0, space 2), 64 bytes.
// car_rt_refl_rules.h RawHitDistance is this arithmetic in C++ (the unit test checks that one): change one, change the other.

#include "car_rt_refl_common.hlsli"

Texture2D<float4> g_textures[] : register(t0, space0);
#ifdef __spirv__
// Vulkan takes the format of a storage image from the shader (DXC would say r32f for a float): the image is R16_FLOAT.
[[vk::image_format("r16f")]]
#endif
RWTexture2D<float> g_output : register(u0, space1);

struct CarRtHitPush
{
    // x the aux image's bindless slot
    uint4 slots;
    // xy the output image's size (the picture's), zw the trace's image's size in texels
    float4 image;
    // x the reflection ray's reach (m), y the divisor (the picture over the trace's image), z the distance a miss writes
    float4 lens;
    float4 reserved;
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<CarRtHitPush> g_push;
#else
ConstantBuffer<CarRtHitPush> g_push : register(b0, space2);
#endif

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const int2 size = int2(g_push.image.xy);
    const int2 pixel = int2(id.xy);
    if (any(pixel >= size))
    {
        return;
    }
    const int2 texels = int2(g_push.image.zw);
    const int divisor = int(max(g_push.lens.y, 1.0));
    const int2 texel = min(pixel / divisor, texels - 1);
    const float4 aux = g_textures[g_push.slots.x].Load(int3(texel, 0));
    float result = 0.0;
    if (aux.w > 0.5)
    {
        // a texel of the car: the cone's mean hit distance (the aux's x squared, over the reach), or the environment's where no ray hit
        result = aux.x > 0.0 ? aux.x * aux.x * g_push.lens.x : g_push.lens.z;
    }
    g_output[pixel] = result;
}
