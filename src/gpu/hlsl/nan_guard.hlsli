// MCLA GPU renderer -- a NaN decided by its bits, the same on D3D12 and on Vulkan (CITY-RT review: NAN-CLEANUP).
//
// What min / max / clamp / saturate make of a NaN is the API's to say. D3D12's DXIL max, min and clamp return the other operand (max(NaN, 0) is 0), so a shader written against D3D12 may count on a NaN being
// dropped by `min(max(v, 0), x)`; SPIR-V's FMax, FMin and FClamp leave which operand a NaN gives open, so on Vulkan the same text can let the NaN through. A NaN that reaches an image that is kept (a history,
// the probes' atlas, the froxel volume) is then kept for good (NaN times a weight of 0 is NaN), and one in the scene colour is spread by the game's own blurs. These helpers decide the NaN FIRST, by the bits
// (a compiler may fold x == x, and a comparison is not what a test on the bits is), and the clamp only ever sees numbers:
//   NgIsNaN(v)             the bits are a NaN's (either sign, any payload, quiet or signalling)
//   NgIsNumber(v)          neither NaN nor infinite (NgIsNumber3 / NgIsNumber4: all channels)
//   NgNumberOr(v, other)   v where it is a number, else `other` (the neutral value of what is being made: 0 for a light, 1 for a factor)
//   NgFinite(v)            v where it is a number, else 0 ("not a number is none": the rule of RtDnSane, GiuBlend1 and HeldChannel)
//   NgHeld(v, hi)          not negative, at most `hi`, a NaN 0 (the car history's and the screen colour's HeldChannel)
//   NgMax(a, b)            the larger; a NaN operand is dropped, as D3D12's max does (both NaN: NaN)
//   NgSaturate(v)          saturate(v), a NaN 0
// and the 3 and 4 component forms. The test twin is src/gpu/nan_guard_test.cpp (it compiles this file as C++).
//
// No branches: in HLSL 2021 `?:` becomes a branch (and so does `if`), also where both sides are plain values, so the choices here are select() (one instruction). NVIDIA's Vulkan shader compiler
// (nvgpucomp64.dll) hit an internal breakpoint on a branch between a texel load and the conversion of the sample in the resolve shaders (the NANFIX rounds of 2026-11-05, resolve_common.hlsli): the
// same form is avoided here, and a select costs less than a branch on every GPU.
//
// Test support: in C++ the file is compiled inside the test's shim namespace (hlsl_cpp_shim.h), which has no asuint or select of its own that every test agrees on, so NG_BITS reads the bits with
// std::bit_cast and NG_SELECT is the conditional operator there.

#ifndef MCLA_GPU_NAN_GUARD_HLSLI
#define MCLA_GPU_NAN_GUARD_HLSLI

#ifdef __HLSL_VERSION
#define NG_BITS(v) asuint(v)
#define NG_SELECT(condition, yes, no) select((condition), (yes), (no))
#else
#define NG_BITS(v) std::bit_cast<unsigned int>(float(v))
#define NG_SELECT(condition, yes, no) ((condition) ? (yes) : (no))
#endif

bool NgIsNaN(float v) {
  return (NG_BITS(v) & 0x7FFFFFFFu) > 0x7F800000u;
}

bool NgIsNumber(float v) {
  return (NG_BITS(v) & 0x7F800000u) != 0x7F800000u;
}

// All channels are numbers: `&` and not `&&`, which short-circuits into branches in HLSL 2021.
bool NgIsNumber3(float3 v) {
  return (NgIsNumber(v.x) & NgIsNumber(v.y)) & NgIsNumber(v.z);
}

bool NgIsNumber4(float4 v) {
  return (NgIsNumber(v.x) & NgIsNumber(v.y)) & (NgIsNumber(v.z) & NgIsNumber(v.w));
}

float NgNumberOr(float v, float other) {
  return NG_SELECT(NgIsNumber(v), v, other);
}

float3 NgNumberOr3(float3 v, float other) {
  return float3(NgNumberOr(v.x, other), NgNumberOr(v.y, other), NgNumberOr(v.z, other));
}

float NgFinite(float v) {
  return NG_SELECT(NgIsNumber(v), v, 0.0f);
}

float3 NgFinite3(float3 v) {
  return float3(NgFinite(v.x), NgFinite(v.y), NgFinite(v.z));
}

float4 NgFinite4(float4 v) {
  return float4(NgFinite(v.x), NgFinite(v.y), NgFinite(v.z), NgFinite(v.w));
}

float NgHeld(float v, float hi) {
  return NG_SELECT(NgIsNaN(v), 0.0f, min(max(v, 0.0f), hi));
}

float3 NgHeld3(float3 v, float hi) {
  return float3(NgHeld(v.x, hi), NgHeld(v.y, hi), NgHeld(v.z, hi));
}

float4 NgHeld4(float4 v, float hi) {
  return float4(NgHeld(v.x, hi), NgHeld(v.y, hi), NgHeld(v.z, hi), NgHeld(v.w, hi));
}

float NgMax(float a, float b) {
  return NG_SELECT(NgIsNaN(a), b, NG_SELECT(NgIsNaN(b), a, max(a, b)));
}

float NgSaturate(float v) {
  return NG_SELECT(NgIsNaN(v), 0.0f, saturate(v));
}

#endif  // MCLA_GPU_NAN_GUARD_HLSLI
