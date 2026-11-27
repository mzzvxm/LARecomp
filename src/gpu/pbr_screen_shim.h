// What the unit tests of the screen passes add to their own shim of the HLSL vector types so that hlsl/pbr_brdf.hlsli (the shared model's contract, PBR-1's) compiles as C++ beside them (PBR level 1C).
// NOT a runtime file: only the tests include it.
//
// The tests that compile the screen passes' .hlsli files (ssr_test, ssr_rt_rules_test, rt_denoise_test, rr_guides_test, pbr_screen_test) each have a small shim of float2 / float3 in `namespace hlsl_shim`, with the
// operators and intrinsics THEIR files use; the contract also needs `float - float3`, `float3 * float`, `lerp` of two float3, `reflect`, `clamp` ... Each test includes this header inside its namespace, after its own
// shim and before the .hlsli: everything here is a TEMPLATE restricted to float3 / float, so where the test's shim has the same function the test's own (a non-template) wins and nothing is redefined, and where
// it does not, this one is used.
//
// Needs, before it: float2, float3 (x, y, z members, a constructor of three floats), and on floats: saturate, min, max, sqrt.
// Also needs <cmath> and <type_traits> included at GLOBAL scope before the test opens its namespace: this header is included inside
// `namespace hlsl_shim`, so it includes no standard header itself (libc++ breaks when <type_traits> first appears inside a namespace;
// the Linux gate failed on rt_denoise_test, rr_guides_test and ssr_test, 2026-11-27).

#ifndef MCLA_PBR_SCREEN_SHIM_H
#define MCLA_PBR_SCREEN_SHIM_H

template <class V, class = std::enable_if_t<std::is_same_v<V, float3>>>
inline V operator+(V a, V b) { return V(a.x + b.x, a.y + b.y, a.z + b.z); }
template <class V, class = std::enable_if_t<std::is_same_v<V, float3>>>
inline V operator-(V a, V b) { return V(a.x - b.x, a.y - b.y, a.z - b.z); }
template <class V, class = std::enable_if_t<std::is_same_v<V, float3>>>
inline V operator-(V a) { return V(-a.x, -a.y, -a.z); }
template <class V, class = std::enable_if_t<std::is_same_v<V, float3>>>
inline V operator*(V a, float s) { return V(a.x * s, a.y * s, a.z * s); }
template <class V, class = std::enable_if_t<std::is_same_v<V, float3>>>
inline V operator*(float s, V a) { return V(a.x * s, a.y * s, a.z * s); }
template <class V, class = std::enable_if_t<std::is_same_v<V, float3>>>
inline V operator*(V a, V b) { return V(a.x * b.x, a.y * b.y, a.z * b.z); }
template <class V, class = std::enable_if_t<std::is_same_v<V, float3>>>
inline V operator-(float s, V a) { return V(s - a.x, s - a.y, s - a.z); }

template <class F, class = std::enable_if_t<std::is_same_v<F, float>>>
inline F clamp(F v, F lo, F hi) { return std::min(std::max(v, lo), hi); }
template <class F, class = std::enable_if_t<std::is_same_v<F, float>>>
inline F lerp(F a, F b, F t) { return a + (b - a) * t; }
template <class V, class = std::enable_if_t<std::is_same_v<V, float3>>>
inline V lerp(V a, V b, float t) { return V(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t); }
template <class V, class = std::enable_if_t<std::is_same_v<V, float3>>>
inline float dot(V a, V b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
template <class V, class = std::enable_if_t<std::is_same_v<V, float3>>>
inline V normalize(V a) { return a * (1.0f / std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z)); }
template <class V, class = std::enable_if_t<std::is_same_v<V, float3>>>
inline V reflect(V i, V n) { return i - n * (2.0f * (n.x * i.x + n.y * i.y + n.z * i.z)); }

#endif  // MCLA_PBR_SCREEN_SHIM_H
