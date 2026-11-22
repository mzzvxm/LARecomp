#pragma once
// ===========================================================================
// MCLA GPU renderer -- TEST SUPPORT: the HLSL types and intrinsics the hit shading's .hlsli files use, in C++ (CITY-RT M2 package H4)
// ===========================================================================
// hlsl/rt_hit_frame.hlsli, hlsl/rt_hit_sample.hlsli, hlsl/city_hit_common.hlsli and hlsl/city_hit_shade.hlsli are written in the subset of HLSL that C++ reads too, so that a test can compile
// THE SHADER'S OWN TEXT as C++ and check it against the twin: what the test runs is what the GPU runs (up to the compiler: the numbers, the order of the operations and the branches are the
// shader's). This header is the shim: vectors with the swizzles the files use (.x .y .z .w .xyz .rgb), the intrinsics, a ByteAddressBuffer and a StructuredBuffer that count a load that is
// misaligned or out of range (the shader's buffer would return 0 or fault), Texture2D<float4> over a FAKE TEXTURE SET (a slot -> the levels the test made), and the bindless array.
//
// A test includes this header at namespace scope, then opens `namespace hlsl_shim { ... }` and includes the .hlsli inside it (so the shader's `abs`, `sqrt`, `pow` ... are found there and the
// standard library's are not in the way), with the macros the includer of the shader would define (RT_HIT_FRAME_ROW, RT_HIT_NO_RAYQUERY, CITY_HIT_NO_RESOURCES ...). Test support only: nothing in
// the game includes it.
// ===========================================================================

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

namespace hlsl_shim {

typedef unsigned int uint;

struct float2;
struct int2 {
  int x = 0, y = 0;
  int2() = default;
  int2(int a, int b) : x(a), y(b) {}
  int2(float2 v);
};
struct int3 {
  int x = 0, y = 0, z = 0;
  int3() = default;
  int3(int a, int b, int c) : x(a), y(b), z(c) {}
  int3(int2 xy, int c) : x(xy.x), y(xy.y), z(c) {}
};
struct uint2 {
  uint x = 0, y = 0;
  uint2() = default;
  uint2(uint a, uint b) : x(a), y(b) {}
};
struct uint3 {
  uint x = 0, y = 0, z = 0;
  uint3() = default;
  uint3(uint a, uint b, uint c) : x(a), y(b), z(c) {}
};
struct uint3_pod {
  uint x, y, z;
};
struct float2 {
  float x = 0, y = 0;
  float2() = default;
  float2(float a, float b) : x(a), y(b) {}
  float2(int2 v) : x(float(v.x)), y(float(v.y)) {}
};
inline int2::int2(float2 v) : x(int(v.x)), y(int(v.y)) {}
struct float3;
struct float3_pod {
  float x, y, z;
  float3_pod& operator=(const float3& v);
};
struct float3 {
  float x = 0, y = 0, z = 0;
  float3() = default;
  float3(float a, float b, float c) : x(a), y(b), z(c) {}
  float3(const float3_pod& v) : x(v.x), y(v.y), z(v.z) {}
};
inline float3_pod& float3_pod::operator=(const float3& v) {
  x = v.x;
  y = v.y;
  z = v.z;
  return *this;
}
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wgnu-anonymous-struct"
#pragma clang diagnostic ignored "-Wnested-anon-types"
#endif
struct uint4 {
  union {
    struct {
      uint x, y, z, w;
    };
    struct {
      uint3_pod xyz;
      uint w_;
    };
  };
  uint4() : x(0), y(0), z(0), w(0) {}
  uint4(uint a, uint b, uint c, uint d) : x(a), y(b), z(c), w(d) {}
};
struct float4 {
  union {
    struct {
      float x, y, z, w;
    };
    struct {
      float3_pod xyz;
      float w_;
    };
    struct {
      float3_pod rgb;
      float a_;
    };
    struct {
      float r, g, b, a;
    };
  };
  float4() : x(0), y(0), z(0), w(0) {}
  float4(float a, float b, float c, float d) : x(a), y(b), z(c), w(d) {}
  float4(const float3& v, float d) : x(v.x), y(v.y), z(v.z), w(d) {}
};
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

// ---- operators ---------------------------------------------------------------------------------------------------------------------------------
inline int2 operator+(int2 a, int2 b) { return int2(a.x + b.x, a.y + b.y); }
inline int2 operator-(int2 a, int2 b) { return int2(a.x - b.x, a.y - b.y); }
inline int2 operator-(int2 a, int s) { return int2(a.x - s, a.y - s); }
inline int2 operator%(int2 a, int2 b) { return int2(a.x % b.x, a.y % b.y); }
inline int2 operator>>(int2 a, int s) { return int2(a.x >> s, a.y >> s); }
inline float2 operator+(float2 a, float2 b) { return float2(a.x + b.x, a.y + b.y); }
inline float2 operator-(float2 a, float2 b) { return float2(a.x - b.x, a.y - b.y); }
inline float2 operator-(float2 a, float s) { return float2(a.x - s, a.y - s); }
inline float2 operator*(float2 a, float s) { return float2(a.x * s, a.y * s); }
inline float2 operator*(float s, float2 a) { return float2(a.x * s, a.y * s); }
inline float2 operator*(float2 a, float2 b) { return float2(a.x * b.x, a.y * b.y); }
inline float2 operator/(float2 a, float2 b) { return float2(a.x / b.x, a.y / b.y); }
inline float3 operator+(float3 a, float3 b) { return float3(a.x + b.x, a.y + b.y, a.z + b.z); }
inline float3 operator-(float3 a, float3 b) { return float3(a.x - b.x, a.y - b.y, a.z - b.z); }
inline float3 operator-(float3 a) { return float3(-a.x, -a.y, -a.z); }
inline float3 operator*(float3 a, float s) { return float3(a.x * s, a.y * s, a.z * s); }
inline float3 operator*(float s, float3 a) { return float3(a.x * s, a.y * s, a.z * s); }
inline float3 operator*(float3 a, float3 b) { return float3(a.x * b.x, a.y * b.y, a.z * b.z); }
inline float3 operator-(float s, float3 a) { return float3(s - a.x, s - a.y, s - a.z); }
inline float3& operator+=(float3& a, float3 b) { return a = a + b; }
inline float4 operator+(float4 a, float4 b) { return float4(a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w); }
inline float4 operator*(float4 a, float s) { return float4(a.x * s, a.y * s, a.z * s, a.w * s); }

// ---- scalar intrinsics -------------------------------------------------------------------------------------------------------------------------
// HLSL's literals are floats and C++'s are doubles: every scalar intrinsic takes any arithmetic types and computes in float (or in the common integer type when all are integers)
template <class T> concept Arith = std::is_arithmetic_v<T>;
template <class A, class B> concept AnyFloat = Arith<A> && Arith<B> && (std::is_floating_point_v<A> || std::is_floating_point_v<B>);
template <class A, class B> concept BothInt = std::is_integral_v<A> && std::is_integral_v<B>;
inline float abs(float v) { return std::fabs(v); }
inline float sqrt(float v) { return std::sqrt(v); }
inline float floor(float v) { return std::floor(v); }
inline float trunc(float v) { return std::trunc(v); }
inline float frac(float v) { return v - std::floor(v); }
inline float log2(float v) { return std::log2(v); }
inline float exp2(float v) { return std::exp2(v); }
inline float cos(float v) { return std::cos(v); }
inline float sin(float v) { return std::sin(v); }
inline float saturate(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
template <class A, class B> requires AnyFloat<A, B> inline float pow(A a, B b) { return std::pow(float(a), float(b)); }
template <class A, class B> requires AnyFloat<A, B> inline float max(A a, B b) { return float(a) > float(b) ? float(a) : float(b); }
template <class A, class B> requires BothInt<A, B> inline std::common_type_t<A, B> max(A a, B b) { return a > b ? a : b; }
template <class A, class B> requires AnyFloat<A, B> inline float min(A a, B b) { return float(a) < float(b) ? float(a) : float(b); }
template <class A, class B> requires BothInt<A, B> inline std::common_type_t<A, B> min(A a, B b) { return a < b ? a : b; }
template <class A, class B, class C> requires (Arith<A> && Arith<B> && Arith<C> && (std::is_floating_point_v<A> || std::is_floating_point_v<B> || std::is_floating_point_v<C>))
inline float clamp(A v, B lo, C hi) { return float(v) < float(lo) ? float(lo) : (float(v) > float(hi) ? float(hi) : float(v)); }
template <class A, class B, class C> requires (std::is_integral_v<A> && std::is_integral_v<B> && std::is_integral_v<C>)
inline std::common_type_t<A, B, C> clamp(A v, B lo, C hi) { return v < lo ? lo : (v > hi ? hi : v); }
template <class A, class B, class C> requires (Arith<A> && Arith<B> && Arith<C>)
inline float lerp(A a, B b, C t) { return float(a) + (float(b) - float(a)) * float(t); }
template <class A, class B, class C> requires (Arith<A> && Arith<B> && Arith<C>)
inline float smoothstep(A lo, B hi, C x) {
  const float s = saturate((float(x) - float(lo)) / (float(hi) - float(lo)));
  return s * s * (3.0f - 2.0f * s);
}
inline float asfloat(uint v) { return std::bit_cast<float>(v); }
// independent of any bit manipulation of the twin: the value of a half by its definition
inline float f16tof32(uint v) {
  const int exponent = int((v >> 10) & 31u);
  const int mantissa = int(v & 1023u);
  const float sign = (v & 0x8000u) != 0 ? -1.0f : 1.0f;
  if (exponent == 0) return sign * std::ldexp(float(mantissa), -24);
  if (exponent == 31) return sign * INFINITY;
  return sign * std::ldexp(float(1024 + mantissa), exponent - 25);
}

// ---- vector intrinsics -------------------------------------------------------------------------------------------------------------------------
inline int2 max(int2 a, int2 b) { return int2(a.x > b.x ? a.x : b.x, a.y > b.y ? a.y : b.y); }
inline int2 clamp(int2 v, int2 lo, int2 hi) { return int2(v.x < lo.x ? lo.x : (v.x > hi.x ? hi.x : v.x), v.y < lo.y ? lo.y : (v.y > hi.y ? hi.y : v.y)); }
inline int2 floor_to_int2(float2 v) { return int2(int(std::floor(v.x)), int(std::floor(v.y))); }
inline float2 floor(float2 v) { return float2(std::floor(v.x), std::floor(v.y)); }
inline float3 asfloat(uint3 v) { return float3(asfloat(v.x), asfloat(v.y), asfloat(v.z)); }
inline float3 asfloat(uint3_pod v) { return float3(asfloat(v.x), asfloat(v.y), asfloat(v.z)); }
inline float4 asfloat(uint4 v) { return float4(asfloat(v.x), asfloat(v.y), asfloat(v.z), asfloat(v.w)); }
inline float dot(float3 a, float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float dot(float3_pod a, float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float3 cross(float3 a, float3 b) { return float3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
inline float length(float3 a) { return std::sqrt(dot(a, a)); }
inline float3 normalize(float3 a) { return a * (1.0f / length(a)); }
inline float3 reflect(float3 i, float3 n) { return i - n * (2.0f * dot(n, i)); }
inline float3 max(float3 a, float3 b) { return float3(max(a.x, b.x), max(a.y, b.y), max(a.z, b.z)); }
inline float3 max(float3 a, float s) { return float3(max(a.x, s), max(a.y, s), max(a.z, s)); }
inline float3 lerp(float3 a, float3 b, float t) { return float3(lerp(a.x, b.x, t), lerp(a.y, b.y, t), lerp(a.z, b.z, t)); }
inline float3 lerp(float3 a, float3 b, float3 t) { return float3(lerp(a.x, b.x, t.x), lerp(a.y, b.y, t.y), lerp(a.z, b.z, t.z)); }
inline float4 lerp(float4 a, float4 b, float t) { return float4(lerp(a.x, b.x, t), lerp(a.y, b.y, t), lerp(a.z, b.z, t), lerp(a.w, b.w, t)); }

// ---- buffers ----------------------------------------------------------------------------------------------------------------------------------
struct ByteAddressBuffer {
  const uint8_t* data = nullptr;
  size_t size = 0;
  mutable int errors = 0;
  uint Load(uint offset) const {
    if ((offset & 3u) != 0 || size_t(offset) + 4 > size) {
      ++errors;
      return 0;
    }
    return uint(data[offset]) | (uint(data[offset + 1]) << 8) | (uint(data[offset + 2]) << 16) | (uint(data[offset + 3]) << 24);
  }
  uint2 Load2(uint offset) const { return uint2(Load(offset), Load(offset + 4)); }
  uint3 Load3(uint offset) const { return uint3(Load(offset), Load(offset + 4), Load(offset + 8)); }
  uint4 Load4(uint offset) const { return uint4(Load(offset), Load(offset + 4), Load(offset + 8), Load(offset + 12)); }
};
template <typename T>
struct StructuredBuffer {
  const T* data = nullptr;
  size_t count = 0;
  mutable int errors = 0;
  T operator[](uint index) const {
    if (size_t(index) >= count) {
      ++errors;
      return T();
    }
    return data[index];
  }
};

// ---- textures ---------------------------------------------------------------------------------------------------------------------------------
// A fake texture: levels[l] is a row-major grid of float4 of (max(width >> l, 1)) x (max(height >> l, 1)); `width`, `height` the first level's size.
struct FakeTexture {
  uint width = 0, height = 0;
  std::vector<std::vector<float4>> levels;
};
inline std::vector<FakeTexture>& FakeTextures() {
  static std::vector<FakeTexture> textures;
  return textures;
}
inline int& FakeErrors() {
  static int errors = 0;
  return errors;
}
template <typename T>
struct Texture2D;
template <>
struct Texture2D<float4> {
  const FakeTexture* texture = nullptr;
  float4 Load(int3 p) const {
    if (!texture || p.z < 0 || size_t(p.z) >= texture->levels.size()) {
      ++FakeErrors();
      return float4();
    }
    const int w = std::max(int(texture->width) >> p.z, 1), h = std::max(int(texture->height) >> p.z, 1);
    if (p.x < 0 || p.y < 0 || p.x >= w || p.y >= h) {  // the shader wraps or clamps first: a coordinate outside the level is a bug
      ++FakeErrors();
      return float4();
    }
    return texture->levels[size_t(p.z)][size_t(p.y) * size_t(w) + size_t(p.x)];
  }
  void GetDimensions(uint, uint& w, uint& h, uint& levels) const {
    w = texture ? texture->width : 0;
    h = texture ? texture->height : 0;
    levels = texture ? uint(texture->levels.size()) : 0;
  }
};
inline uint NonUniformResourceIndex(uint i) { return i; }
struct TextureArray {
  Texture2D<float4> operator[](uint slot) const {
    Texture2D<float4> t;
    if (slot < FakeTextures().size()) {
      t.texture = &FakeTextures()[slot];
    } else {
      ++FakeErrors();
    }
    return t;
  }
};

}  // namespace hlsl_shim
