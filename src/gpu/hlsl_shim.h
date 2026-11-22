#pragma once
// ===========================================================================
// MCLA GPU renderer -- HLSL as C++: the types and intrinsics the noise .hlsli files use (a test helper, no GPU, no renderer)
// ===========================================================================
// A test includes this file, then opens `namespace hlsl_shim { #include "hlsl/xxx.hlsli" }` and calls the shader's own functions (the way ssr_test.cpp compiles ssr_common.hlsli). Only what the
// small noise files need is here; a file that needs more adds it here, not in the test.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace hlsl_shim {

typedef unsigned int uint;

struct float2 {
  float x = 0, y = 0;
  float2() = default;
  float2(float a, float b) : x(a), y(b) {}
};
struct float3 {
  float x = 0, y = 0, z = 0;
  float3() = default;
  float3(float a, float b, float c) : x(a), y(b), z(c) {}
};
struct uint2 {
  uint x = 0, y = 0;
  uint2() = default;
  uint2(uint a, uint b) : x(a), y(b) {}
};

inline float2 operator+(float2 a, float2 b) { return float2(a.x + b.x, a.y + b.y); }
inline float2 operator-(float2 a, float2 b) { return float2(a.x - b.x, a.y - b.y); }
inline float2 operator*(float2 a, float s) { return float2(a.x * s, a.y * s); }
inline float2 operator*(float s, float2 a) { return float2(a.x * s, a.y * s); }
inline float2 operator*(float2 a, float2 b) { return float2(a.x * b.x, a.y * b.y); }
inline float3 operator+(float3 a, float3 b) { return float3(a.x + b.x, a.y + b.y, a.z + b.z); }
inline float3 operator-(float3 a, float3 b) { return float3(a.x - b.x, a.y - b.y, a.z - b.z); }
inline float3 operator*(float3 a, float s) { return float3(a.x * s, a.y * s, a.z * s); }
inline float3 operator*(float s, float3 a) { return float3(a.x * s, a.y * s, a.z * s); }

inline float abs(float v) { return std::fabs(v); }
inline float sqrt(float v) { return std::sqrt(v); }
inline float saturate(float v) { return std::min(std::max(v, 0.0f), 1.0f); }
inline float dot(float2 a, float2 b) { return a.x * b.x + a.y * b.y; }
inline float dot(float3 a, float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }
inline float clamp(float v, float lo, float hi) { return std::min(std::max(v, lo), hi); }
inline float min(float a, float b) { return a < b ? a : b; }
inline float max(float a, float b) { return a > b ? a : b; }
inline uint min(uint a, uint b) { return a < b ? a : b; }
inline uint max(uint a, uint b) { return a > b ? a : b; }
inline int min(int a, int b) { return a < b ? a : b; }
inline int max(int a, int b) { return a > b ? a : b; }
inline float floor(float v) { return std::floor(v); }
inline float2 floor(float2 v) { return float2(std::floor(v.x), std::floor(v.y)); }
inline float frac(float v) { return v - std::floor(v); }
inline float2 frac(float2 v) { return float2(frac(v.x), frac(v.y)); }
inline float cos(float v) { return std::cos(v); }
inline float sin(float v) { return std::sin(v); }
inline float fmod(float a, float b) { return std::fmod(a, b); }
inline uint asuint(float f) {
  uint u;
  std::memcpy(&u, &f, sizeof(u));
  return u;
}
inline float asfloat(uint u) {
  float f;
  std::memcpy(&f, &u, sizeof(f));
  return f;
}

}  // namespace hlsl_shim
