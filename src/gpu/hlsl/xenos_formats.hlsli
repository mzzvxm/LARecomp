// MCLA GPU renderer -- Xenos value conversions for the host passes.
//
// Ported from Xenia (BSD-3-Clause, Copyright 2022 Ben Vanik and the Xenia
// authors): src/xenia/gpu/shaders/pixel_formats.xesli (XePackR8G8B8A8UNorm,
// XePackR5G5B5A1UNorm, XeFloat32To20e4, XeFloat20e4To32, XeUNorm24To32) and
// the ReXGlue SDK's src/graphics/xenos.cpp (LinearToPWLGamma). Same
// arithmetic, so a resolve product holds what the emulated path would have
// written to memory and loaded back.

#ifndef MCLA_GPU_XENOS_FORMATS_HLSLI
#define MCLA_GPU_XENOS_FORMATS_HLSLI

// UNORM packing as the Direct3D 11.3 functional specification and Xenia do it:
// saturate, scale, add 0.5, truncate. Returned as the value the host's UNORM
// target stores back exactly (q / max).
float4 XeQuantizeUNorm(float4 value, float4 max_value) {
  return floor(saturate(value) * max_value + 0.5f) / max_value;
}

// Linear -> the Xenos piecewise-linear gamma (xenos.cpp LinearToPWLGamma: the
// truncation is in Direct3D 9's disassembly, vrfiz).
float XeLinearToPWLGamma(float value) {
  value = saturate(value);
  float scale, offset;
  if (value >= 128.0f / 1023.0f) {
    if (value >= 512.0f / 1023.0f) {
      scale = 1023.0f / 8.0f;
      offset = 128.0f / 255.0f;
    } else {
      scale = 1023.0f / 4.0f;
      offset = 64.0f / 255.0f;
    }
  } else {
    if (value >= 64.0f / 1023.0f) {
      scale = 1023.0f / 2.0f;
      offset = 32.0f / 255.0f;
    } else {
      scale = 1023.0f;
      offset = 0.0f;
    }
  }
  return trunc(value * scale) * (1.0f / 255.0f) + offset;
}

float3 XeLinearToPWLGamma3(float3 value) {
  return float3(XeLinearToPWLGamma(value.r), XeLinearToPWLGamma(value.g),
                XeLinearToPWLGamma(value.b));
}

// IEEE float32 bits -> Xenos 20e4 depth (24 bits), Xenia's XeFloat32To20e4.
uint XeFloat32To20e4(uint f32u32, bool round_to_nearest_even) {
  // Positive only, and not -0 or NaN; saturated.
  f32u32 = min((f32u32 <= 0x7FFFFFFFu) ? f32u32 : 0u, 0x3FFFFFF8u);
  uint denormalized =
      ((f32u32 & 0x7FFFFFu) | 0x800000u) >> min(113u - (f32u32 >> 23u), 24u);
  uint f24u32 = (f32u32 < 0x38800000u) ? denormalized : (f32u32 + 0xC8000000u);
  if (round_to_nearest_even) {
    f24u32 += 3u + ((f24u32 >> 3u) & 1u);
  }
  return (f24u32 >> 3u) & 0xFFFFFFu;
}

// Xenos 20e4 depth (24 bits) -> float32, Xenia's XeFloat20e4To32 (no remap).
float XeFloat20e4To32(uint f24u32) {
  uint mantissa = f24u32 & 0xFFFFFu;
  uint exponent = f24u32 >> 20u;
  bool is_denormalized = exponent == 0u;
  // firstbithigh(0) is -1; the zero case is selected away below.
  uint mantissa_lzcnt = 20u - uint(firstbithigh(mantissa));
  exponent = is_denormalized ? (1u - mantissa_lzcnt) : exponent;
  mantissa = is_denormalized ? ((mantissa << mantissa_lzcnt) & 0xFFFFFu) : mantissa;
  return asfloat((f24u32 != 0u) ? (((exponent + 112u) << 23u) | (mantissa << 3u)) : 0u);
}

// 24-bit UNORM depth -> float32, Xenia's XeUNorm24To32: not / 16777215 (which
// turns the common 0xC00000 into something else), an exact shift instead.
float XeUNorm24To32(uint n24) {
  return float(n24 + (n24 >> 23u)) * (1.0f / float(1u << 24u));
}

#endif  // MCLA_GPU_XENOS_FORMATS_HLSLI
